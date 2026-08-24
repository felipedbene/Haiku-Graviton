/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <interrupts.h>
#include <interrupt_controller.h>
#include <kernel.h>
#include <vm/vm.h>
#include <smp.h>
#include <KernelExport.h>

#include <arch/cpu.h>
#include <cpu.h>

#include "arch_int_gicv3.h"
#include "gicv3_regs.h"


#define ICI_IRQ 0


GICv3InterruptController::GICv3InterruptController(const intc_info& info)
	:
	InterruptController(),
	fGicdRegs(0),
	fITS(NULL),
	fGicrRegionCount(0),
	fGicrFallbackStride(GICR_STRIDE_V3),
	fIrqCount(GIC_SPECIAL_BASE)
{
	reserve_io_interrupt_vectors(GIC_SPECIAL_BASE, 0, INTERRUPT_TYPE_IRQ);

	memset(fGicrRegions, 0, sizeof(fGicrRegions));

	size_t gicdSize = info.regs1.size;
	if (gicdSize == 0)
		gicdSize = 0x10000;

	area_id gicdArea = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv3-gicd",
		(void**)&fGicdRegs, B_ANY_KERNEL_ADDRESS, gicdSize,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, info.regs1.start, false);
	if (gicdArea < 0)
		panic("gicv3: unable to map the distributor registers\n");

	// Only used to size a window when firmware describes the redistributors
	// with neither a length nor a set of regions. The walks themselves step by
	// what each redistributor reports -- see gicr_frame_stride().
	const uint32 pidr2 = _ReadGicd(GICD_PIDR2);
	if (GICD_PIDR2_ARCH(pidr2) >= 4)
		fGicrFallbackStride = GICR_STRIDE_V4;

	dprintf("gicv3: gicd %#" B_PRIx64 " (size %#" B_PRIxSIZE "), arch rev %"
		B_PRIu32 ", typer %#" B_PRIx32 ", fallback stride %#" B_PRIxSIZE "\n",
		info.regs1.start, gicdSize, GICD_PIDR2_ARCH(pidr2),
		_ReadGicd(GICD_TYPER), fGicrFallbackStride);

	_MapRedistributors(info);

	_DistributorInit();
	_PrefaultRedistributors();

	call_all_cpus_sync([](void* arg, int cpu) {
		((GICv3InterruptController*)arg)->_PerCpuInit();
	}, this);

	EnableInterrupt(ICI_IRQ);
}


// The redistributors reach us either as a single range or, when firmware
// describes them per-CPU because they are not all adjacent, as several. Map
// each one separately: the gaps between them on real hardware are measured in
// gigabytes and hold other devices.
void
GICv3InterruptController::_MapRedistributors(const intc_info& info)
{
	addr_range ranges[INTC_MAX_GICR_REGIONS];
	uint32 count = info.gicr_region_count;

	if (count > INTC_MAX_GICR_REGIONS)
		count = INTC_MAX_GICR_REGIONS;

	if (count == 0) {
		// One contiguous range, described the other way round. A size of zero
		// means firmware did not say, so guess at one frame set per CPU.
		ranges[0].start = info.regs2.start;
		ranges[0].size = info.regs2.size != 0
			? info.regs2.size : fGicrFallbackStride * smp_get_num_cpus();
		count = 1;
	} else {
		for (uint32 i = 0; i < count; i++)
			ranges[i] = info.gicr_regions[i];
	}

	for (uint32 i = 0; i < count; i++) {
		if (ranges[i].size == 0)
			continue;

		addr_t mapped = 0;
		area_id area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv3-gicr",
			(void**)&mapped, B_ANY_KERNEL_ADDRESS, ranges[i].size,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, ranges[i].start, false);
		if (area < 0) {
			panic("gicv3: unable to map redistributor region %" B_PRIu32
				" at %#" B_PRIx64 "\n", i, ranges[i].start);
			return;
		}

		fGicrRegions[fGicrRegionCount].base = mapped;
		fGicrRegions[fGicrRegionCount].physicalBase = ranges[i].start;
		fGicrRegions[fGicrRegionCount].size = ranges[i].size;
		fGicrRegionCount++;

		dprintf("gicv3: redistributor region %" B_PRIu32 ": %#" B_PRIx64
			" size %#" B_PRIx64 " (at most %" B_PRIu64 " PEs)\n", i,
			ranges[i].start, ranges[i].size,
			ranges[i].size / GICR_STRIDE_V3);
	}
}


uint32
GICv3InterruptController::_ReadGicd(uint32 offset)
{
	return gic_read32(fGicdRegs + offset);
}


void
GICv3InterruptController::_WriteGicd(uint32 offset, uint32 value)
{
	gic_write32(fGicdRegs + offset, value);
}


void
GICv3InterruptController::_WriteGicd64(uint32 offset, uint64 value)
{
	gic_write64(fGicdRegs + offset, value);
}


// Register writes that affect distribution are posted; software must wait for
// GICD_CTLR.RWP to read back as zero before assuming they have taken effect.
void
GICv3InterruptController::_WaitForRwp()
{
	uint32 attempts = 100000;
	while ((_ReadGicd(GICD_CTLR) & GICD_CTLR_RWP) != 0) {
		if (--attempts == 0) {
			dprintf("gicv3: timed out waiting for GICD_CTLR.RWP\n");
			return;
		}
	}
}


void
GICv3InterruptController::_DistributorInit()
{
	// Disable the distributor while it is reconfigured.
	_WriteGicd(GICD_CTLR, 0);
	_WaitForRwp();

	fIrqCount = GICD_TYPER_ITLINES(_ReadGicd(GICD_TYPER));
	if (fIrqCount > GIC_SPECIAL_BASE)
		fIrqCount = GIC_SPECIAL_BASE;

	// Put every SPI in group 1 non-secure, disabled, level triggered, at a
	// uniform priority. SGIs and PPIs are owned by the redistributors.
	for (uint32 irq = GIC_SPI_BASE; irq < fIrqCount; irq += 32) {
		_WriteGicd(GICD_ICENABLER + (irq / 32) * 4, 0xffffffff);
		_WriteGicd(GICD_IGROUPR + (irq / 32) * 4, 0xffffffff);
	}

	for (uint32 irq = GIC_SPI_BASE; irq < fIrqCount; irq += 4) {
		_WriteGicd(GICD_IPRIORITYR + irq, 0x01010101u * GIC_PRIORITY_DEFAULT);
	}

	for (uint32 irq = GIC_SPI_BASE; irq < fIrqCount; irq += 16)
		_WriteGicd(GICD_ICFGR + (irq / 16) * 4, 0);

	_WaitForRwp();

	// Enable affinity routing (mandatory for GICv3) together with group 1
	// non-secure interrupts. See GICD_CTLR_ENABLE_G1 for why both enable bits
	// are written.
	_WriteGicd(GICD_CTLR,
		GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1 | GICD_CTLR_ENABLE_G1NS);
	_WaitForRwp();

	dprintf("gicv3: %" B_PRIu32 " interrupt lines, gicd_ctlr %#" B_PRIx32
		"\n", fIrqCount, _ReadGicd(GICD_CTLR));

	// With ARE enabled, SPI targeting is by affinity rather than a CPU bitmask.
	// Route everything to the boot CPU until something asks otherwise.
	uint64 route = gic_routing_affinity(READ_SPECIALREG(MPIDR_EL1));
	for (uint32 irq = GIC_SPI_BASE; irq < fIrqCount; irq++)
		_WriteGicd64(GICD_IROUTER + irq * 8, route);
}


// _PerCpuInit() runs on the secondary CPUs from call_all_cpus_sync(), i.e. in
// inter-processor interrupt context with interrupts disabled, where the VM
// refuses to service a page fault and turns it into a panic. The redistributor
// mapping is populated lazily, so touch every frame we will later use while
// still on the boot CPU in normal context. Only frames belonging to real PEs
// are read: on a virtualised GIC the frames past the last redistributor are
// not backed and would fault for a different reason entirely.
void
GICv3InterruptController::_PrefaultRedistributors()
{
	uint32 found = 0;

	for (uint32 region = 0; region < fGicrRegionCount; region++) {
		addr_t frame = fGicrRegions[region].base;
		const addr_t end = frame + fGicrRegions[region].size;

		// A redistributor is only walked if its RD_base and SGI_base frames --
		// the two we touch -- are inside the region firmware described.
		while (frame + GICR_STRIDE_V3 <= end) {
			const uint64 typer = gic_read64(frame + GICR_TYPER);

			// The SGI/PPI frame is a separate page from the RD frame.
			(void)gic_read32(frame + GICR_SGI_FRAME + GICR_IGROUPR0);

			dprintf("gicv3: redistributor %" B_PRIu32 " (region %" B_PRIu32
				") affinity %#" B_PRIx32 ", processor %" B_PRIu32 ", vlpis %d,"
				" last %d\n", found, region, (uint32)(typer >> 32),
				(uint32)GICR_TYPER_PROC_NUM(typer),
				(typer & GICR_TYPER_VLPIS) != 0 ? 1 : 0,
				(typer & GICR_TYPER_LAST) != 0 ? 1 : 0);
			found++;

			if ((typer & GICR_TYPER_LAST) != 0)
				break;

			frame += gicr_frame_stride(typer);
		}
	}

	dprintf("gicv3: %" B_PRIu32 " redistributor(s) across %" B_PRIu32
		" region(s) for %" B_PRId32 " cpu(s)\n", found, fGicrRegionCount,
		smp_get_num_cpus());
}


// Walk the redistributor frames looking for the one whose GICR_TYPER affinity
// matches this PE, and return the address of its RD_base frame. GICR_TYPER.Last
// only marks the end of the region it appears in, so a machine whose
// redistributors are split across several regions has several of them; running
// out of one region is a reason to try the next, not to give up.
addr_t
GICv3InterruptController::_CurrentRedistributor()
{
	uint32 affinity = gic_packed_affinity(READ_SPECIALREG(MPIDR_EL1));

	for (uint32 region = 0; region < fGicrRegionCount; region++) {
		addr_t frame = fGicrRegions[region].base;
		const addr_t end = frame + fGicrRegions[region].size;

		while (frame + GICR_STRIDE_V3 <= end) {
			const uint64 typer = gic_read64(frame + GICR_TYPER);
			if ((uint32)(typer >> 32) == affinity)
				return frame;

			if ((typer & GICR_TYPER_LAST) != 0)
				break;

			frame += gicr_frame_stride(typer);
		}
	}

	panic("gicv3: no redistributor for affinity %#" B_PRIx32 "\n", affinity);
	return 0;
}


void
GICv3InterruptController::_PerCpuInit()
{
	addr_t rdBase = _CurrentRedistributor();
	addr_t sgiBase = rdBase + GICR_SGI_FRAME;

	// Bring the redistributor out of sleep and wait for it to acknowledge.
	gic_write32(rdBase + GICR_WAKER,
		gic_read32(rdBase + GICR_WAKER) & ~GICR_WAKER_PROCESSOR_SLEEP);

	uint32 attempts = 100000;
	while ((gic_read32(rdBase + GICR_WAKER) & GICR_WAKER_CHILDREN_ASLEEP) != 0) {
		if (--attempts == 0) {
			dprintf("gicv3: redistributor did not wake up\n");
			break;
		}
	}

	// SGIs and PPIs: group 1 non-secure, all disabled, uniform priority.
	gic_write32(sgiBase + GICR_IGROUPR0, 0xffffffff);
	gic_write32(sgiBase + GICR_ICENABLER0, 0xffffffff);
	for (uint32 irq = 0; irq < GIC_SPI_BASE; irq += 4) {
		gic_write32(sgiBase + GICR_IPRIORITYR + irq,
			0x01010101u * GIC_PRIORITY_DEFAULT);
	}

	// Switch the CPU interface from the memory-mapped GICv2 view to the
	// system register interface. Nothing below works until SRE is set.
	uint64 sre = READ_SPECIALREG(ICC_SRE_EL1);
	if ((sre & ICC_SRE_EL1_SRE) == 0) {
		WRITE_SPECIALREG(ICC_SRE_EL1, sre | ICC_SRE_EL1_SRE);
		__asm__ __volatile__("isb");
	}

	WRITE_SPECIALREG(ICC_PMR_EL1, GIC_PRIORITY_MASK);
	WRITE_SPECIALREG(ICC_BPR1_EL1, 0);

	// EOImode 0: a single write to ICC_EOIR1_EL1 both drops priority and
	// deactivates the interrupt.
	uint64 ctlr = READ_SPECIALREG(ICC_CTLR_EL1);
	WRITE_SPECIALREG(ICC_CTLR_EL1, ctlr & ~(uint64)ICC_CTLR_EL1_EOIMODE);

	WRITE_SPECIALREG(ICC_IGRPEN1_EL1, 1);
	__asm__ __volatile__("isb");
}


void
GICv3InterruptController::EnableInterrupt(int32 irq)
{
	if (irq < GIC_SPI_BASE) {
		// SGIs and PPIs are private to each PE, so they must be enabled in
		// every redistributor.
		call_all_cpus_sync([](void* arg, int cpu) {
			int32 irq = (int32)(addr_t)arg;
			GICv3InterruptController* self
				= (GICv3InterruptController*)InterruptController::Get();
			addr_t sgiBase = self->_CurrentRedistributor() + GICR_SGI_FRAME;
			gic_write32(sgiBase + GICR_ISENABLER0, 1 << irq);
		}, (void*)(addr_t)irq);
		return;
	}

	_WriteGicd(GICD_ISENABLER + (irq / 32) * 4, 1 << (irq % 32));
	_WaitForRwp();
}


void
GICv3InterruptController::DisableInterrupt(int32 irq)
{
	if (irq < GIC_SPI_BASE) {
		call_all_cpus_sync([](void* arg, int cpu) {
			int32 irq = (int32)(addr_t)arg;
			GICv3InterruptController* self
				= (GICv3InterruptController*)InterruptController::Get();
			addr_t sgiBase = self->_CurrentRedistributor() + GICR_SGI_FRAME;
			gic_write32(sgiBase + GICR_ICENABLER0, 1 << irq);
		}, (void*)(addr_t)irq);
		return;
	}

	_WriteGicd(GICD_ICENABLER + (irq / 32) * 4, 1 << (irq % 32));
	_WaitForRwp();
}


void
GICv3InterruptController::HandleInterrupt()
{
	uint64 iar = READ_SPECIALREG(ICC_IAR1_EL1);
	uint32 irqnr = iar & 0xffffff;

	if (irqnr >= GIC_SPECIAL_BASE && irqnr < GIC_LPI_BASE) {
		// 1020-1023 are reserved; no EOI is required for them.
		return;
	}

	if (irqnr >= GIC_LPI_BASE) {
		// A message-signalled interrupt translated by the ITS.
		const int32 vector = fITS != NULL ? fITS->VectorForLpi(irqnr) : -1;
		if (vector >= 0)
			io_interrupt_handler(vector, true);
	} else if (irqnr == ICI_IRQ) {
		smp_intercpu_interrupt_handler(smp_get_current_cpu());
	} else {
		io_interrupt_handler(irqnr, true);
	}

	WRITE_SPECIALREG(ICC_EOIR1_EL1, iar);
}


// GICv3 addresses SGI targets by affinity: bits [15:0] are a target list of
// PEs sharing Aff3.Aff2.Aff1, selected by Aff0.
void
GICv3InterruptController::_SendSgi(uint64 mpidr, uint32 sgiId)
{
	uint64 value = ((uint64)CPU_AFF3(mpidr) << ICC_SGI1R_EL1_AFF3_SHIFT)
		| ((uint64)CPU_AFF2(mpidr) << ICC_SGI1R_EL1_AFF2_SHIFT)
		| ((uint64)CPU_AFF1(mpidr) << ICC_SGI1R_EL1_AFF1_SHIFT)
		| ((uint64)(sgiId & ICC_SGI1R_EL1_SGIID_MASK)
			<< ICC_SGI1R_EL1_SGIID_SHIFT)
		| (1ull << (CPU_AFF0(mpidr) % 16));

	WRITE_SPECIALREG(ICC_SGI1R_EL1, value);
	__asm__ __volatile__("isb");
}


void
GICv3InterruptController::SendMulticastIci(CPUSet& cpuSet)
{
	int32 cpuCount = smp_get_num_cpus();
	for (int32 cpu = 0; cpu < cpuCount; cpu++) {
		if (cpuSet.GetBit(cpu))
			_SendSgi(gCPU[cpu].arch.mpidr, ICI_IRQ);
	}
}


void
GICv3InterruptController::SendBroadcastIci()
{
	// IRM routes the SGI to every PE except the one issuing it, matching the
	// GICv2 driver's use of the "all but self" target list filter.
	WRITE_SPECIALREG(ICC_SGI1R_EL1, ICC_SGI1R_EL1_IRM
		| ((uint64)ICI_IRQ << ICC_SGI1R_EL1_SGIID_SHIFT));
	__asm__ __volatile__("isb");
}


status_t
GICv3InterruptController::InitITS(phys_addr_t regs, size_t size)
{
	if (regs == 0)
		return B_NAME_NOT_FOUND;

	GICv3ITS* its = new(std::nothrow) GICv3ITS();
	if (its == NULL)
		return B_NO_MEMORY;

	status_t status = its->Init(regs, size, fGicdRegs, fGicrRegions,
		fGicrRegionCount);
	if (status != B_OK) {
		delete its;
		return status;
	}

	fITS = its;
	return B_OK;
}
