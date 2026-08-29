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


// One line per redistributor is O(PEs), and at 96 PEs it is around 9 KB -- more
// than enough to push the loader's own discovery lines out of the firmware's
// console ring and destroy the very log it was added to serve. The summary that
// follows it is O(1) and carries what actually decides whether discovery
// worked; this is what you turn on when the summary and the CPU count disagree.
//#define TRACE_GICV3
#ifdef TRACE_GICV3
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


GICv3InterruptController::GICv3InterruptController(const intc_info& info)
	:
	InterruptController(),
	fGicdRegs(0),
	fITS(NULL),
	fGicrRegionCount(0),
	fGicrStride(GICR_STRIDE_V3),
	fIrqCount(GIC_SPECIAL_BASE)
{
	reserve_io_interrupt_vectors(GIC_SPECIAL_BASE, 0, INTERRUPT_TYPE_IRQ);

	memset(fGicrRegions, 0, sizeof(fGicrRegions));

	// GICD_PIDR2 lives at 0xffe8, so the whole 64 KB frame has to be mapped
	// whatever a device tree claims the distributor's size to be -- reading it
	// through a short mapping would fault here, inside the constructor of the
	// object that would have to report the fault.
	size_t gicdSize = info.regs1.size;
	if (gicdSize < 0x10000)
		gicdSize = 0x10000;

	area_id gicdArea = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv3-gicd",
		(void**)&fGicdRegs, B_ANY_KERNEL_ADDRESS, gicdSize,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, info.regs1.start, false);
	if (gicdArea < 0)
		panic("gicv3: unable to map the distributor registers\n");

	// How far apart the redistributors are depends on whether this is a GICv4
	// implementation, which gives each PE four 64 KB frames rather than two --
	// unconditionally, not only where virtual LPIs are implemented (Arm IHI
	// 0069G 12.10). A hypervisor's emulated distributor reports v3 and a real
	// GIC-700 reports v4. This is only consulted when firmware handed us no
	// regions to walk; a region always arrives with the stride that was used to
	// measure it.
	const uint32 pidr2 = _ReadGicd(GICD_PIDR2);
	if (GICD_PIDR2_ARCH(pidr2) >= 4)
		fGicrStride = GICR_STRIDE_V4;

	dprintf("gicv3: gicd %#" B_PRIx64 " (size %#" B_PRIxSIZE "), arch rev %"
		B_PRIu32 ", typer %#" B_PRIx32 ", fallback stride %#" B_PRIxSIZE
		"\n", info.regs1.start, gicdSize, GICD_PIDR2_ARCH(pidr2),
		_ReadGicd(GICD_TYPER), fGicrStride);

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
	gicr_region_info ranges[INTC_MAX_GICR_REGIONS];
	uint32 count = info.gicr_region_count;

	// Whether the geometry was measured for us, or is a window we are guessing
	// our way through. Only the former is expected to be an exact number of
	// redistributors.
	const bool measured = count != 0;

	if (count > INTC_MAX_GICR_REGIONS)
		count = INTC_MAX_GICR_REGIONS;

	if (count == 0) {
		// One contiguous range, described the other way round, and with no
		// stride attached -- so here, and only here, the architecture revision
		// decides it. A size of zero means firmware did not say, so guess at
		// one frame set per CPU.
		ranges[0].start = info.regs2.start;
		ranges[0].size = info.regs2.size != 0
			? info.regs2.size : fGicrStride * smp_get_num_cpus();
		ranges[0].stride = fGicrStride;
		count = 1;
	} else {
		for (uint32 i = 0; i < count; i++)
			ranges[i] = info.gicr_regions[i];
	}

	for (uint32 i = 0; i < count; i++) {
		if (ranges[i].size == 0 || ranges[i].stride == 0) {
			dprintf("gicv3: ignoring redistributor region %" B_PRIu32 " at %#"
				B_PRIx64 ": size %#" B_PRIx64 ", stride %#" B_PRIx64 "\n", i,
				ranges[i].start, ranges[i].size, ranges[i].stride);
			continue;
		}

		addr_t mapped = 0;
		area_id area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv3-gicr",
			(void**)&mapped, B_ANY_KERNEL_ADDRESS, ranges[i].size,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, ranges[i].start, false);
		if (area < 0) {
			panic("gicv3: unable to map redistributor region %" B_PRIu32
				" at %#" B_PRIx64 "\n", i, ranges[i].start);
			return;
		}

		gicr_region& region = fGicrRegions[fGicrRegionCount];
		region.base = mapped;
		region.physicalBase = ranges[i].start;
		region.size = ranges[i].size;
		region.stride = ranges[i].stride;
		region.count = ranges[i].size / ranges[i].stride;

		// A firmware-declared window is generous, not an array: a guest declares
		// 0xfdf0000 bytes, which is 2031 strides on a machine with at most 96
		// PEs. Left at that, the only thing keeping the walk short is
		// GICR_TYPER.Last -- and Last is not dependably set, since Graviton3
		// metal omits it entirely on some hosts. So bound it by the number of
		// PEs the firmware described, which is an exact upper bound on how many
		// redistributors can exist.
		//
		// Deliberately not smp_get_num_cpus(): that is how many PEs we *use*,
		// capped at SMP_MAX_CPUS, and it is smaller than the number that exist
		// on exactly the machines this matters for -- a 96-PE guest runs 64,
		// and clamping to 64 would cut off 32 real, backed redistributors and
		// silently make the diagnostic under-report.
		if (!measured && info.pe_count != 0 && region.count > info.pe_count) {
			dprintf("gicv3: region %" B_PRIu32 " spans %" B_PRIu32 " strides for"
				" %" B_PRIu32 " pe(s); walking %" B_PRIu32 "\n", i,
				region.count, info.pe_count, info.pe_count);
			region.count = info.pe_count;
		}

		// A coalesced region is an exact number of redistributors by
		// construction, so a remainder there means the loader and the kernel
		// disagree about the geometry -- the count truncates, so the effect is
		// redistributors that go unfound rather than reads off the end, but it
		// is a bug either way. A single firmware-declared range is just a
		// generous window and its size means nothing in particular: a guest
		// reports 0xfdf0000, which is 2031 and a half strides, every boot.
		if (measured && (ranges[i].size % ranges[i].stride) != 0) {
			dprintf("gicv3: redistributor region %" B_PRIu32 " at %#" B_PRIx64
				" is %#" B_PRIx64 " bytes, not a multiple of its %#" B_PRIx64
				" stride\n", i, ranges[i].start, ranges[i].size,
				ranges[i].stride);
		}

		// Linux checks this and we did not: if there is no redistributor at the
		// base we were given, everything read from here is meaningless.
		const uint32 rpidr2 = gic_read32(mapped + GICR_PIDR2);
		const uint32 rarch = GICD_PIDR2_ARCH(rpidr2);
		if (rarch != 3 && rarch != 4) {
			dprintf("gicv3: no redistributor at region %" B_PRIu32 " base %#"
				B_PRIx64 ": GICR_PIDR2 %#" B_PRIx32 " (arch rev %" B_PRIu32
				")\n", i, ranges[i].start, rpidr2, rarch);
		}

		fGicrRegionCount++;

		dprintf("gicv3: redistributor region %" B_PRIu32 ": %#" B_PRIx64
			" size %#" B_PRIx64 ", stride %#" B_PRIx64 ", %" B_PRIu32
			" PEs, arch rev %" B_PRIu32 "\n", i, ranges[i].start,
			ranges[i].size, ranges[i].stride, region.count, rarch);
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

	// Extended SPIs, INTID 4096 and up, exist on real hardware -- Graviton3
	// implements 128 of them -- and reset into group 0, which non-secure
	// software cannot take. Nothing allocates one today, so nothing can raise
	// one; but if anything ever did, the CPU interface would hand
	// HandleInterrupt() a reserved INTID, which it drops without an EOI, and
	// the interrupt would be re-presented for ever. That is a livelock with no
	// panic to point at it, so put the whole range in group 1 and leave it
	// disabled. Only group and enable are programmed: priority, configuration
	// and routing would also have to be set before any of these could actually
	// be used.
	const uint32 typer = _ReadGicd(GICD_TYPER);
	uint32 espiCount = 0;
	if ((typer & GICD_TYPER_ESPI) != 0)
		espiCount = GICD_TYPER_ESPI_COUNT(typer);

	for (uint32 i = 0; i < espiCount / 32; i++) {
		_WriteGicd(GICD_ICENABLERE + i * 4, 0xffffffff);
		_WriteGicd(GICD_IGROUPRE + i * 4, 0xffffffff);
	}

	_WaitForRwp();

	// Enable affinity routing (mandatory for GICv3) together with group 1
	// non-secure interrupts. See GICD_CTLR_ENABLE_G1 for which bit does the
	// work and why the other is written too.
	_WriteGicd(GICD_CTLR,
		GICD_CTLR_ARE_NS | GICD_CTLR_ENABLE_G1 | GICD_CTLR_ENABLE_G1NS);
	_WaitForRwp();

	dprintf("gicv3: %" B_PRIu32 " interrupt lines, %" B_PRIu32 " extended "
		"spis, gicd_ctlr %#" B_PRIx32 "\n", fIrqCount, espiCount,
		_ReadGicd(GICD_CTLR));

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
	uint32 vlpis = 0;
	uint32 last = 0;

	for (uint32 region = 0; region < fGicrRegionCount; region++) {
		addr_t frame = fGicrRegions[region].base;

		for (uint32 i = 0; i < fGicrRegions[region].count; i++) {
			const uint64 typer = gic_read64(frame + GICR_TYPER);

			// The SGI/PPI frame is a separate page from the RD frame.
			(void)gic_read32(frame + GICR_SGI_FRAME + GICR_IGROUPR0);

			TRACE("gicv3: redistributor %" B_PRIu32 " (region %" B_PRIu32
				") affinity %#" B_PRIx32 ", processor %" B_PRIu32 ", vlpis %d,"
				" last %d\n", found, region, (uint32)(typer >> 32),
				(uint32)GICR_TYPER_PROC_NUM(typer),
				(uint32)((typer & GICR_TYPER_VLPIS) != 0 ? 1 : 0),
				(uint32)((typer & GICR_TYPER_LAST) != 0 ? 1 : 0));
			found++;

			// Aggregates of what the per-PE line carries, so the interesting
			// part survives at O(1): a mixed vlpis report would mean the frame
			// geometry is not uniform, and where Last falls says how much of
			// the walk's bound the hardware is actually supplying.
			if ((typer & GICR_TYPER_VLPIS) != 0)
				vlpis++;
			if ((typer & GICR_TYPER_LAST) != 0)
				last++;

			if ((typer & GICR_TYPER_LAST) != 0)
				break;

			frame += fGicrRegions[region].stride;
		}
	}

	dprintf("gicv3: %" B_PRIu32 " redistributor(s) across %" B_PRIu32
		" region(s) for %" B_PRId32 " cpu(s); %" B_PRIu32 " report vlpis, %"
		B_PRIu32 " report last\n", found, fGicrRegionCount,
		smp_get_num_cpus(), vlpis, last);
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

		for (uint32 i = 0; i < fGicrRegions[region].count; i++) {
			const uint64 typer = gic_read64(frame + GICR_TYPER);
			if ((uint32)(typer >> 32) == affinity)
				return frame;

			if ((typer & GICR_TYPER_LAST) != 0)
				break;

			frame += fGicrRegions[region].stride;
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

	// And the extended PPI range where it exists -- Graviton3 reaches INTID
	// 1087 -- for the same reason as the extended SPIs above.
	const uint32 ppiNum = GICR_TYPER_PPI_NUM(gic_read64(rdBase + GICR_TYPER));
	if (ppiNum >= 1) {
		gic_write32(sgiBase + GICR_IGROUPR1E, 0xffffffff);
		gic_write32(sgiBase + GICR_ICENABLER1E, 0xffffffff);
	}
	if (ppiNum >= 2) {
		gic_write32(sgiBase + GICR_IGROUPR2E, 0xffffffff);
		gic_write32(sgiBase + GICR_ICENABLER2E, 0xffffffff);
	}

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


// The single dispatch point for interrupt affinity. SPIs and MSIs travel by
// different routing mechanisms, so which one an incoming vector needs is
// decided here by its range.
int32
GICv3InterruptController::AssignToCpu(int32 irq, int32 cpu)
{
	const int32 cpuCount = smp_get_num_cpus();
	if (cpu < 0 || cpu >= cpuCount)
		return 0;

	// SPIs are routed by the distributor's per-INTID affinity register. Point
	// GICD_IROUTER at the requested CPU's affinity and let the write settle;
	// the kernel vector equals the INTID across the SPI range.
	if (irq >= GIC_SPI_BASE && irq < (int32)fIrqCount) {
		_WriteGicd64(GICD_IROUTER + irq * 8,
			gic_routing_affinity(gCPU[cpu].arch.mpidr));
		_WaitForRwp();
		return cpu;
	}

	// MSIs/LPIs are routed by the ITS collection -> redistributor map, not by
	// GICD_IROUTER. The ITS fixes each vector's collection when it is allocated
	// (round-robin across CPUs). It cannot be re-issued from here: this runs
	// from the install path with interrupts disabled under a vector spinlock,
	// where the ITS command queue -- which blocks on a mutex and spins for the
	// queue to drain -- must not be touched. Report the CPU the vector already
	// targets so the bookkeeping is honest and the rebalancer does not churn.
	if (fITS != NULL) {
		const int32 target = fITS->CurrentCpuForVector(irq);
		if (target >= 0)
			return target;
	}

	// Anything we do not route -- PPIs, SGIs, unmanaged vectors -- stays on the
	// boot CPU, which is where the distributor left it.
	return 0;
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
		fGicrRegionCount, fGicrStride);
	if (status != B_OK) {
		delete its;
		return status;
	}

	fITS = its;
	return B_OK;
}
