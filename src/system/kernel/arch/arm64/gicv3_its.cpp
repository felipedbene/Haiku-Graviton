/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>

#include <cpu.h>
#include <interrupts.h>
#include <kernel.h>
#include <smp.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <KernelExport.h>

#include "gicv3_its.h"
#include "gicv3_regs.h"


//#define TRACE_ITS
#ifdef TRACE_ITS
#	define TRACE(x...) dprintf("gicv3-its: " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...) dprintf("gicv3-its: " x)


// The ITS walks its tables with its own DMA reads, so every table has to be
// physically contiguous and aligned more strictly than a page in some cases.
// Over-allocate and take an aligned window inside the region; because the area
// is contiguous, a virtual offset is also a physical one.
static status_t
allocate_its_table(const char* name, size_t size, size_t alignment,
	area_id& areaID, addr_t& virtualAddress, phys_addr_t& physicalAddress)
{
	const size_t total = ROUNDUP(size + alignment, B_PAGE_SIZE);

	void* address = NULL;
	areaID = create_area(name, &address, B_ANY_KERNEL_ADDRESS, total,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (areaID < 0)
		return areaID;

	physical_entry entry;
	status_t status = get_memory_map(address, total, &entry, 1);
	if (status != B_OK) {
		delete_area(areaID);
		return status;
	}

	const phys_addr_t aligned = ROUNDUP(entry.address, alignment);
	virtualAddress = (addr_t)address + (aligned - entry.address);
	physicalAddress = aligned;

	memset((void*)virtualAddress, 0, size);
	return B_OK;
}


// Whether any CPU the kernel actually runs on lives behind this redistributor.
// An emulated GIC hands out a redistributor per PE the platform could ever have,
// so on a machine with more PEs than SMP_MAX_CPUS -- or with CPUs firmware
// disabled -- a good many of them belong to nobody.
static bool
redistributor_has_cpu(uint32 affinity)
{
	const int32 cpuCount = smp_get_num_cpus();
	for (int32 i = 0; i < cpuCount; i++) {
		if (gic_packed_affinity(gCPU[i].arch.mpidr) == affinity)
			return true;
	}

	return false;
}


// The index of the CPU that sits behind this redistributor, or -1 if none. The
// index doubles as the ITS collection id, so an MSI mapped to collection i is
// delivered to CPU i's redistributor.
static int32
cpu_for_affinity(uint32 affinity)
{
	const int32 cpuCount = smp_get_num_cpus();
	for (int32 i = 0; i < cpuCount; i++) {
		if (gic_packed_affinity(gCPU[i].arch.mpidr) == affinity)
			return i;
	}

	return -1;
}


status_t
GICv3ITS::Init(phys_addr_t regs, size_t size, addr_t gicdRegs,
	const gicr_region* gicrRegions, uint32 gicrRegionCount, size_t gicrStride)
{
	// Nothing in Init() takes fLock: it runs single-threaded during boot, and
	// the interface is not reachable from anywhere else until the
	// msi_set_interface() at the end publishes it.
	mutex_init(&fLock, "gicv3 its");
	B_INITIALIZE_SPINLOCK(&fCommandLock);

	STATIC_ASSERT(GIC_ITS_MAX_COLLECTIONS >= SMP_MAX_CPUS);

	memset(fAllocated, 0, sizeof(fAllocated));
	memset(fDevices, 0, sizeof(fDevices));
	memset(fCollectionTargets, 0, sizeof(fCollectionTargets));
	fCollectionCount = 0;
	fCommandIndex = 0;

	if (size == 0)
		size = 0x20000;

	area_id area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv3-its",
		(void**)&fRegs, B_ANY_KERNEL_ADDRESS, size,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, regs, false);
	if (area < 0) {
		ERROR("unable to map the ITS registers\n");
		return area;
	}

	fTranslaterPhysical = regs + GITS_TRANSLATER;

	const uint64 typer = gic_read64(fRegs + GITS_TYPER);
	if ((typer & GITS_TYPER_PHYSICAL) == 0) {
		ERROR("ITS does not support physical LPIs\n");
		return B_NOT_SUPPORTED;
	}

	fIttEntrySize = GITS_TYPER_ITT_SIZE(typer);
	fEventIDBits = GITS_TYPER_ID_BITS(typer);
	fDeviceIDBits = GITS_TYPER_DEV_BITS(typer);
	// Set to the table's real width once _InitTables() allocates the device
	// table; a device baser is mandatory, but default it here so a requester-ID
	// bounds check can never read a stale value.
	fDeviceTableBits = 0;
	fPhysicalTargetAddress = (typer & GITS_TYPER_PTA) != 0;

	// How many collections the ITS can hold. CIL narrows the CollectionID space
	// to CIDbits; otherwise it is the full 16 bits. HCC counts collections the
	// implementation keeps in hardware registers, which need no memory table.
	// We only ever need one per CPU, so this is a ceiling we stay well under.
	fMaxCollections = (typer & GITS_TYPER_CIL) != 0
		? (1u << GITS_TYPER_CID_BITS(typer)) : (1u << 16);
	const uint32 hcc = GITS_TYPER_HCC(typer);
	if (hcc > fMaxCollections)
		fMaxCollections = hcc;

	dprintf("gicv3-its: typer %#" B_PRIx64 ": itt entry size %" B_PRIu32 ", %"
		B_PRIu32 " event id bits, %" B_PRIu32 " device id bits, pta %d, up to %"
		B_PRIu32 " collections\n", typer, fIttEntrySize, fEventIDBits,
		fDeviceIDBits, fPhysicalTargetAddress ? 1 : 0, fMaxCollections);

	// The ITS must be quiescent before its tables can be reprogrammed.
	gic_write32(fRegs + GITS_CTLR, 0);
	for (int i = 0; i < 100000; i++) {
		if ((gic_read32(fRegs + GITS_CTLR) & GITS_CTLR_QUIESCENT) != 0)
			break;
	}

	status_t status = _InitTables();
	if (status != B_OK)
		return status;

	status = _InitCommandQueue();
	if (status != B_OK)
		return status;

	gic_write32(fRegs + GITS_CTLR, GITS_CTLR_ENABLED);

	status = _InitLpis(gicdRegs, gicrRegions, gicrRegionCount, gicrStride);
	if (status != B_OK)
		return status;

	// Independent per-vector assignment records: each LPI travels through its
	// own ITS collection, so its target CPU is tracked and steered per vector.
	// A shared record would collapse the whole ITS vector space onto one CPU
	// and defeat the per-CPU collections mapped just below.
	status = allocate_io_interrupt_vectors(GIC_ITS_MAX_VECTORS, &fVectorBase,
		INTERRUPT_TYPE_IRQ, true);
	if (status != B_OK) {
		ERROR("unable to allocate interrupt vectors for MSIs\n");
		return status;
	}

	// One collection per CPU, each aimed at that CPU's redistributor, so an MSI
	// can be delivered to any core. _InitLpis captured a target per CPU;
	// collection id equals cpu id, which is what the round-robin in
	// AllocateVectors and CurrentCpuForVector rely on.
	for (uint32 collection = 0; collection < fCollectionCount; collection++) {
		status = _MapCollection(collection, fCollectionTargets[collection],
			true);
		if (status != B_OK)
			return status;
		_Sync(fCollectionTargets[collection]);
	}

	dprintf("gicv3-its: ready, %d vectors from %" B_PRId32 ", %" B_PRIu32
		" collection(s), translater %#" B_PRIxPHYSADDR "\n",
		GIC_ITS_MAX_VECTORS, fVectorBase, fCollectionCount, fTranslaterPhysical);


	msi_set_interface(static_cast<MSIInterface*>(this));
	return B_OK;
}


// GITS_BASER's Size field counts granules chosen by its page-size field, and
// the address is laid out differently for the 64 KB granule: bits [47:16] hold
// the address and bits [15:12] hold address bits [51:48], where the smaller
// granules simply use bits [47:12].
static size_t
its_baser_granule(uint64 baser)
{
	switch ((baser & GITS_BASER_PAGE_SIZE_MASK) >> 8) {
		case 0:		return 4 * 1024;
		case 1:		return 16 * 1024;
		default:	return 64 * 1024;
	}
}


static uint64
its_baser_encode_address(phys_addr_t physical, size_t granule)
{
	if (granule == 64 * 1024) {
		return (physical & 0x0000ffffffff0000ull)
			| (((physical >> 48) & 0xf) << 12);
	}

	return physical & 0x0000fffffffff000ull;
}


status_t
GICv3ITS::_InitTables()
{
	for (int i = 0; i < GITS_BASER_COUNT; i++) {
		const addr_t reg = fRegs + GITS_BASER + i * 8;
		uint64 baser = gic_read64(reg);

		const uint32 type = GITS_BASER_TYPE(baser);
		if (type != GITS_BASER_TYPE_DEVICE
			&& type != GITS_BASER_TYPE_COLLECTION) {
			continue;
		}

		const uint32 entrySize = GITS_BASER_ENTRY_SIZE(baser);
		uint64 entries;
		if (type == GITS_BASER_TYPE_DEVICE) {
			// A flat table for the full DeviceID space would be enormous on
			// some implementations; cap it, since PCI requester IDs on the
			// machines we care about stay small. The cap has to leave room for
			// the PCI segment folded above the 16-bit BDF (see the requester-ID
			// layout in gicv3_its.h), so that same-BDF devices in different
			// segments land on distinct DeviceIDs. Never ask for more than the
			// ITS says it can address.
			uint32 bits = min_c(fDeviceIDBits, (uint32)GIC_ITS_DEVICE_ID_BITS);
			fDeviceTableBits = bits;
			entries = 1ull << bits;
		} else {
			// One collection per CPU so MSIs can be spread across cores, but
			// never fewer than the 16 this has always requested, and never more
			// than the implementation can hold.
			uint32 wanted = max_c((uint32)smp_get_num_cpus(), (uint32)16);
			if (wanted > fMaxCollections)
				wanted = fMaxCollections;
			entries = wanted;
		}

		const size_t bytes = entries * entrySize;

		// Ask for the granule the ITS already advertises. If it insists on a
		// different one the allocation is wrong -- both its alignment and the
		// layout of the address we program -- so adopt what it reports and try
		// again. Getting this wrong is silent: the ITS simply reads its tables
		// from the wrong physical address, finds no device for any incoming
		// message and drops every interrupt.
		size_t granule = its_baser_granule(baser);
		area_id area = -1;
		uint64 readback = 0;
		// The geometry the ITS finally accepted, kept outside the retry loop so
		// it can still be reported after it.
		phys_addr_t tablePhysical = 0;
		size_t tableSize = 0;

		for (int attempt = 0; attempt < 3; attempt++) {
			size_t size = ROUNDUP(bytes, granule);
			uint32 pages = size / granule;
			if (pages > 256) {
				pages = 256;
				size = (size_t)pages * granule;
			}

			addr_t table;
			phys_addr_t physical;
			status_t status = allocate_its_table("gicv3-its-table", size,
				granule, area, table, physical);
			if (status != B_OK) {
				ERROR("unable to allocate table type %" B_PRIu32 "\n", type);
				return status;
			}

			tablePhysical = physical;
			tableSize = size;

			uint64 pageSize = GITS_BASER_PAGE_SIZE_64K;
			if (granule == 4 * 1024)
				pageSize = GITS_BASER_PAGE_SIZE_4K;
			else if (granule == 16 * 1024)
				pageSize = GITS_BASER_PAGE_SIZE_16K;

			baser = GITS_BASER_VALID | pageSize | GITS_BASER_INNER_CACHE
				| GITS_BASER_SHAREABILITY
				| its_baser_encode_address(physical, granule)
				| (uint64)(pages - 1);
			gic_write64(reg, baser);

			readback = gic_read64(reg);
			if ((readback & GITS_BASER_VALID) == 0) {
				ERROR("ITS rejected table type %" B_PRIu32 "\n", type);
				delete_area(area);
				return B_ERROR;
			}

			const size_t accepted = its_baser_granule(readback);
			if (accepted == granule)
				break;

			// It wants a different granule. Start over with that one.
			TRACE("table type %" B_PRIu32 ": ITS wants a %" B_PRIuSIZE
				" byte granule, not %" B_PRIuSIZE "\n", type, accepted,
				granule);
			gic_write64(reg, 0);
			delete_area(area);
			area = -1;
			granule = accepted;
		}

		if (area < 0 || its_baser_granule(gic_read64(reg)) != granule) {
			ERROR("could not agree a page size for table type %" B_PRIu32
				"\n", type);
			return B_ERROR;
		}

		// An implementation is allowed to ignore what we asked for. If it
		// downgraded the table to non-cacheable then it reads around our
		// caches, and every update would need cleaning to the point of
		// coherency; we do not do that, so say so instead of failing
		// mysteriously later.
		if ((readback & GITS_BASER_CACHE_MASK) != GITS_BASER_INNER_CACHE
			|| (readback & GITS_BASER_SHARE_MASK)
				!= GITS_BASER_SHAREABILITY) {
			ERROR("ITS downgraded table type %" B_PRIu32 " to cache %"
				B_PRIu64 " share %" B_PRIu64 "; LPI delivery may not work\n",
				type, (uint64)((readback & GITS_BASER_CACHE_MASK) >> 59),
				(uint64)((readback & GITS_BASER_SHARE_MASK) >> 10));
		}

		TRACE("table type %" B_PRIu32 ": %" B_PRIuSIZE " bytes at %#"
			B_PRIxPHYSADDR "\n", type, tableSize, tablePhysical);
	}

	return B_OK;
}


status_t
GICv3ITS::_InitCommandQueue()
{
	area_id area;
	status_t status = allocate_its_table("gicv3-its-cmdq",
		GITS_CMD_QUEUE_SIZE, GITS_CMD_QUEUE_SIZE, area, fCommandQueue,
		fCommandQueuePhysical);
	if (status != B_OK) {
		ERROR("unable to allocate the command queue\n");
		return status;
	}

	const uint64 cbaser = GITS_CBASER_VALID | GITS_CBASER_INNER_CACHE
		| GITS_CBASER_SHAREABILITY
		| (fCommandQueuePhysical & 0x000ffffffffff000ull)
		| (uint64)((GITS_CMD_QUEUE_SIZE / B_PAGE_SIZE) - 1);
	gic_write64(fRegs + GITS_CBASER, cbaser);
	gic_write64(fRegs + GITS_CWRITER, 0);
	fCommandIndex = 0;

	// The ITS is allowed to ignore the requested cacheability/shareability for
	// the command queue exactly as it may for the tables (see _InitTables). A
	// silent downgrade means it fetches commands around our caches while we
	// never clean them, so it can see stale command-queue contents; say so
	// instead of failing mysteriously later.
	const uint64 cbaserReadback = gic_read64(fRegs + GITS_CBASER);
	if ((cbaserReadback & GITS_CBASER_CACHE_MASK) != GITS_CBASER_INNER_CACHE
		|| (cbaserReadback & GITS_CBASER_SHARE_MASK)
			!= GITS_CBASER_SHAREABILITY) {
		ERROR("ITS downgraded the command queue to cache %" B_PRIu64 " share %"
			B_PRIu64 "; command delivery may not work\n",
			(uint64)((cbaserReadback & GITS_CBASER_CACHE_MASK) >> 59),
			(uint64)((cbaserReadback & GITS_CBASER_SHARE_MASK) >> 10));
	}

	return B_OK;
}


status_t
GICv3ITS::_InitLpis(addr_t gicdRegs, const gicr_region* gicrRegions,
	uint32 gicrRegionCount, size_t gicrStride)
{
	// The LPI configuration table is shared by every redistributor, so it is
	// allocated once. One byte per LPI. Never claim more INTID bits than the
	// distributor implements.
	fLpiIDBits = min_c((uint32)14,
		(uint32)GICD_TYPER_IDBITS(gic_read32(gicdRegs + GICD_TYPER)));
	if ((1ul << fLpiIDBits) <= GIC_LPI_BASE) {
		ERROR("distributor implements no LPI INTID space\n");
		return B_NOT_SUPPORTED;
	}
	const size_t propSize = (1ul << fLpiIDBits) - GIC_LPI_BASE;

	area_id area;
	status_t status = allocate_its_table("gicv3-lpi-config", propSize,
		B_PAGE_SIZE, area, fPropertyTable, fPropertyTablePhysical);
	if (status != B_OK) {
		ERROR("unable to allocate the LPI configuration table\n");
		return status;
	}

	// Every LPI starts disabled at the driver's usual priority; AllocateVectors
	// enables the ones it hands out.
	memset((void*)fPropertyTable, GIC_PRIORITY_DEFAULT, propSize);

	const uint64 propbaser = (fPropertyTablePhysical & 0x000ffffffffff000ull)
		| GICR_BASER_INNER_CACHE | GICR_BASER_SHAREABILITY
		| (uint64)(fLpiIDBits - 1);

	// Each redistributor needs its own pending table, and LPIs have to be
	// enabled on all of them even though only the boot CPU is targeted today.
	// GICR_TYPER.Last ends the region it is in, not the walk: firmware that
	// describes the redistributors per-CPU may hand us several regions.
	// The collection every LPI is delivered to must name the redistributor of
	// the CPU that will handle it. Init() runs on the boot CPU, so that is this
	// PE's own affinity -- picking the first redistributor in the first region
	// instead would name whichever one happens to sit at the lowest address.
	const uint32 bootAffinity
		= gic_packed_affinity(READ_SPECIALREG(MPIDR_EL1));

	const size_t pendingSize
		= ROUNDUP((1ul << fLpiIDBits) / 8, GICR_PENDBASER_ALIGNMENT);

	bool haveTarget = false;
	uint32 redistributors = 0;
	uint32 skipped = 0;
	for (uint32 region = 0; region < gicrRegionCount; region++) {
		addr_t frame = gicrRegions[region].base;
		phys_addr_t framePhysical = gicrRegions[region].physicalBase;

		for (uint32 i = 0; i < gicrRegions[region].count; i++) {
			const uint64 typer = gic_read64(frame + GICR_TYPER);
			const uint32 affinity = (uint32)(typer >> 32);

			// Skip the ones no CPU sits behind. Each costs a physically
			// contiguous pending table, and GICR_CTLR.EnableLPIs is a one-way
			// latch -- there is no reason to spend either on a PE that will
			// never be started.
			if (!redistributor_has_cpu(affinity)) {
				skipped++;
				if ((typer & GICR_TYPER_LAST) != 0)
					break;
				frame += gicrRegions[region].stride;
				framePhysical += gicrRegions[region].stride;
				continue;
			}

			addr_t pending;
			phys_addr_t pendingPhysical;
			status = allocate_its_table("gicv3-lpi-pending", pendingSize,
				GICR_PENDBASER_ALIGNMENT, area, pending, pendingPhysical);
			if (status != B_OK) {
				// One physically contiguous pending table per redistributor, so
				// this is where a large machine runs out: say what it costs and
				// what is lost, because the caller only logs and carries on with
				// no MSI at all.
				ERROR("unable to allocate an LPI pending table for "
					"redistributor %" B_PRIu32 " of %" B_PRIu32 " (%"
					B_PRIuSIZE " KiB contiguous each); MSI will be "
					"unavailable\n", redistributors, gicrRegionCount,
					pendingSize / 1024);
				return status;
			}

			gic_write64(frame + GICR_PROPBASER, propbaser);
			gic_write64(frame + GICR_PENDBASER,
				(pendingPhysical & 0x000ffffffff0000ull)
					| GICR_BASER_INNER_CACHE | GICR_BASER_SHAREABILITY);

			// A redistributor may quietly downgrade the cacheability or
			// shareability of these registers, just like the ITS tables. A
			// downgraded PROPBASER makes it read the LPI configuration table
			// around our caches, so an LPI we enable there can silently never
			// enable on this PE -- invisible without this readback. Both
			// registers are only writable while LPIs are disabled, so check
			// them before latching GICR_CTLR.EnableLPIs.
			const uint64 propReadback = gic_read64(frame + GICR_PROPBASER);
			if ((propReadback & GICR_BASER_CACHE_MASK) != GICR_BASER_INNER_CACHE
				|| (propReadback & GICR_BASER_SHARE_MASK)
					!= GICR_BASER_SHAREABILITY) {
				ERROR("redistributor %" B_PRIu32 " downgraded PROPBASER to "
					"cache %" B_PRIu64 " share %" B_PRIu64 "; LPIs may never "
					"enable on it\n", redistributors,
					(uint64)((propReadback & GICR_BASER_CACHE_MASK) >> 7),
					(uint64)((propReadback & GICR_BASER_SHARE_MASK) >> 10));
			}

			const uint64 pendReadback = gic_read64(frame + GICR_PENDBASER);
			if ((pendReadback & GICR_BASER_CACHE_MASK) != GICR_BASER_INNER_CACHE
				|| (pendReadback & GICR_BASER_SHARE_MASK)
					!= GICR_BASER_SHAREABILITY) {
				ERROR("redistributor %" B_PRIu32 " downgraded PENDBASER to "
					"cache %" B_PRIu64 " share %" B_PRIu64 "; LPI delivery may "
					"not work\n", redistributors,
					(uint64)((pendReadback & GICR_BASER_CACHE_MASK) >> 7),
					(uint64)((pendReadback & GICR_BASER_SHARE_MASK) >> 10));
			}

			gic_write32(frame + GICR_CTLR,
				gic_read32(frame + GICR_CTLR) | GICR_CTLR_ENABLE_LPIS);

			redistributors++;

			// Collections name their target either by physical redistributor
			// address or by the processor number the redistributor reports.
			// Record one per CPU, indexed by the CPU id (== the collection id),
			// so MSIs can be routed to any core -- not just the boot CPU.
			const uint64 target = fPhysicalTargetAddress
				? (framePhysical >> 16) : GICR_TYPER_PROC_NUM(typer);
			const int32 cpu = cpu_for_affinity(affinity);
			if (cpu >= 0 && cpu < (int32)GIC_ITS_MAX_COLLECTIONS)
				fCollectionTargets[cpu] = target;

			if (affinity == bootAffinity) {
				fCollectionTarget = target;
				haveTarget = true;
			}

			if ((typer & GICR_TYPER_LAST) != 0)
				break;

			frame += gicrRegions[region].stride;
			framePhysical += gicrRegions[region].stride;
		}
	}

	if (!haveTarget) {
		ERROR("no redistributor for the boot cpu's affinity %#" B_PRIx32
			"; MSI will be unavailable\n", bootAffinity);
		return B_ERROR;
	}

	// One collection per CPU, so long as the ITS can hold that many. If it
	// holds fewer, the round-robin folds the extra CPUs back onto the ones it
	// can address -- still spread, just not one-to-one.
	fCollectionCount = min_c((uint32)smp_get_num_cpus(),
		min_c(fMaxCollections, (uint32)GIC_ITS_MAX_COLLECTIONS));
	if (fCollectionCount == 0)
		fCollectionCount = 1;

	dprintf("gicv3-its: lpis enabled on %" B_PRIu32 " redistributor(s), %"
		B_PRIu32 " skipped as cpu-less, %" B_PRIu32 " lpi intid bits, %"
		B_PRIuSIZE " KiB of pending tables, collection target %#" B_PRIx64
		"\n", redistributors, skipped, fLpiIDBits,
		(redistributors * pendingSize) / 1024, fCollectionTarget);

	// The count should be exactly the CPUs we run on. Anything else means the
	// affinity match above is not seeing what it thinks it is, and the
	// consequence -- LPIs enabled on the wrong set of redistributors -- is
	// quiet, so say it out loud here.
	if (redistributors != (uint32)smp_get_num_cpus()) {
		ERROR("lpis enabled on %" B_PRIu32 " redistributor(s) for %" B_PRId32
			" cpu(s)\n", redistributors, smp_get_num_cpus());
	}

	return B_OK;
}


// Commands are 32 bytes and consumed from a ring; GITS_CWRITER is the producer
// index in bytes and GITS_CREADR the consumer's.
status_t
GICv3ITS::_SubmitCommand(const uint64* command)
{
	// One command's worth of ring manipulation is atomic against any other
	// submitter. The lock also lets SetVectorAffinity() re-route a vector from
	// an interrupts-disabled context without taking fLock (which may sleep):
	// the ring write, the CWRITER advance and the CREADR drain all happen while
	// interrupts are off, so a concurrent submit cannot interleave the ring.
	InterruptsSpinLocker locker(fCommandLock);

	const addr_t slot = fCommandQueue + fCommandIndex * GITS_CMD_SIZE;
	for (int i = 0; i < 4; i++)
		gic_write64(slot + i * 8, command[i]);

	// Make sure the command is visible to the ITS before the index advances.
	__asm__ __volatile__("dsb sy" ::: "memory");

	fCommandIndex = (fCommandIndex + 1) % (GITS_CMD_QUEUE_SIZE / GITS_CMD_SIZE);
	gic_write64(fRegs + GITS_CWRITER, fCommandIndex * GITS_CMD_SIZE);

	const uint64 target = fCommandIndex * GITS_CMD_SIZE;
	for (int i = 0; i < 1000000; i++) {
		const uint64 readr = gic_read64(fRegs + GITS_CREADR);
		if ((readr & ~0x1full) == target)
			return B_OK;
		if ((readr & 1) != 0) {
			ERROR("command queue stalled at %#" B_PRIx64 " on command %#"
				B_PRIx64 "\n", readr, command[0]);
			return B_ERROR;
		}
	}

	ERROR("timed out waiting for the command queue to drain\n");
	return B_TIMED_OUT;
}


status_t
GICv3ITS::_Sync()
{
	return _Sync(fCollectionTarget);
}


// SYNC ensures earlier commands affecting a given redistributor have completed.
// A MAPTI to a new collection has to be synced against that collection's
// target, not always the boot CPU's.
status_t
GICv3ITS::_Sync(uint64 target)
{
	uint64 command[4] = { GITS_CMD_SYNC, 0, target << 16, 0 };
	return _SubmitCommand(command);
}


status_t
GICv3ITS::_MapDevice(uint32 deviceID, phys_addr_t itt, uint32 eventIDBits,
	bool valid)
{
	uint64 command[4];
	command[0] = GITS_CMD_MAPD | ((uint64)deviceID << 32);
	command[1] = eventIDBits - 1;
	command[2] = (itt & 0x000ffffffffff00ull)
		| (valid ? (1ull << 63) : 0);
	command[3] = 0;
	return _SubmitCommand(command);
}


status_t
GICv3ITS::_MapCollection(uint32 collection, uint64 target, bool valid)
{
	uint64 command[4];
	command[0] = GITS_CMD_MAPC;
	command[1] = 0;
	command[2] = (uint64)collection | (target << 16)
		| (valid ? (1ull << 63) : 0);
	command[3] = 0;
	return _SubmitCommand(command);
}


status_t
GICv3ITS::_MapInterrupt(uint32 deviceID, uint32 eventID, uint32 lpi,
	uint32 collection)
{
	uint64 command[4];
	command[0] = GITS_CMD_MAPTI | ((uint64)deviceID << 32);
	command[1] = (uint64)eventID | ((uint64)lpi << 32);
	command[2] = collection;
	command[3] = 0;
	return _SubmitCommand(command);
}


// Re-routes an already-mapped (DeviceID, EventID) to a different collection.
// Unlike MAPTI this keeps the LPI's existing ITT entry and only changes which
// collection -- and therefore which redistributor/CPU -- it is delivered to.
status_t
GICv3ITS::_MoveInterrupt(uint32 deviceID, uint32 eventID, uint32 collection)
{
	uint64 command[4];
	command[0] = GITS_CMD_MOVI | ((uint64)deviceID << 32);
	command[1] = eventID;
	command[2] = collection;
	command[3] = 0;
	return _SubmitCommand(command);
}


// The counterpart of MAPTI: it removes the (DeviceID, EventID) entry from the
// device's ITT and clears the LPI's pending state. The architecture requires it
// before that EventID may be mapped again, which INVALL does not cover -- INVALL
// only makes the ITS re-read the property table, and never touches the ITT.
status_t
GICv3ITS::_Discard(uint32 deviceID, uint32 eventID)
{
	uint64 command[4];
	command[0] = GITS_CMD_DISCARD | ((uint64)deviceID << 32);
	command[1] = eventID;
	command[2] = 0;
	command[3] = 0;
	return _SubmitCommand(command);
}


// Undoes one vector's worth of AllocateVectors(). The caller owns the INVALL and
// the SYNC that have to follow, since releasing a run of vectors needs only one
// of each. Nothing happens for a vector that is not allocated: fVectorDevice and
// fVectorEvent are only meaningful while the bit is set, so a stray DISCARD
// built from them could unmap an event belonging to some other device.
void
GICv3ITS::_ReleaseVector(uint32 index)
{
	if ((fAllocated[index / 32] & (1u << (index % 32))) == 0)
		return;

	const uint32 lpi = GIC_LPI_BASE + index;
	((volatile uint8*)fPropertyTable)[lpi - GIC_LPI_BASE]
		= GIC_PRIORITY_DEFAULT;

	_Discard(fVectorDevice[index], fVectorEvent[index]);

	fAllocated[index / 32] &= ~(1u << (index % 32));
}


its_device*
GICv3ITS::_DeviceFor(uint32 requesterID)
{
	// A requester ID wider than the device table can address (e.g. a PCI
	// segment beyond what GITS_TYPER.Devbits allows) would index past the
	// table's end. Refuse it here rather than let a MAPD alias onto, or run
	// off, another device's entry -- the failure surfaces as a device that
	// gets no MSIs, which is far easier to diagnose than silent interrupt
	// cross-talk.
	if (fDeviceTableBits < 32
		&& requesterID >= (1u << fDeviceTableBits)) {
		ERROR("requester id %#" B_PRIx32 " exceeds the %" B_PRIu32 "-bit ITS "
			"device table; PCI segment cannot be represented\n", requesterID,
			fDeviceTableBits);
		return NULL;
	}

	for (int i = 0; i < GIC_ITS_MAX_DEVICES; i++) {
		if (fDevices[i].valid && fDevices[i].requester_id == requesterID)
			return &fDevices[i];
	}

	for (int i = 0; i < GIC_ITS_MAX_DEVICES; i++) {
		if (fDevices[i].valid)
			continue;

		// The ITT must be 256-byte aligned and large enough for every event
		// this device may raise.
		size_t size = GIC_ITS_EVENTS_PER_DEVICE * fIttEntrySize;
		if (size < 256)
			size = 256;

		area_id area;
		addr_t itt;
		phys_addr_t physical;
		if (allocate_its_table("gicv3-its-itt", size, 256, area, itt,
				physical) != B_OK) {
			return NULL;
		}

		fDevices[i].requester_id = requesterID;
		fDevices[i].itt = itt;
		fDevices[i].itt_physical = physical;
		fDevices[i].valid = true;

		uint32 bits = 1;
		while ((1u << bits) < GIC_ITS_EVENTS_PER_DEVICE)
			bits++;

		if (_MapDevice(requesterID, physical, bits, true) != B_OK) {
			fDevices[i].valid = false;
			delete_area(area);
			return NULL;
		}

		TRACE("mapped device %#" B_PRIx32 " itt %#" B_PRIxPHYSADDR "\n",
			requesterID, physical);
		return &fDevices[i];
	}

	ERROR("out of device slots\n");
	return NULL;
}


status_t
GICv3ITS::AllocateVectors(uint32 count, uint32& startVector, uint64& address,
	uint32& data)
{
	// Without knowing which device will emit the message there is nothing to
	// program into the ITS; callers must use the requester-aware variant.
	return B_NOT_SUPPORTED;
}


status_t
GICv3ITS::AllocateVectors(uint32 requesterID, uint32 count,
	uint32& startVector, uint64& address, uint32& data)
{
	if (count == 0 || count > GIC_ITS_EVENTS_PER_DEVICE)
		return B_BAD_VALUE;

	// A mutex rather than a spinlock: every path from here reaches
	// _SubmitCommand(), which spins on GITS_CREADR for as long as it takes the
	// ITS to drain the queue, and this is only ever called from a driver's
	// attach path -- msi_allocate_vectors_for_device() is reached through the
	// PCI bus manager, which maps areas immediately either side of the call, so
	// the caller can already block and interrupts are enabled.
	MutexLocker locker(fLock);

	its_device* device = _DeviceFor(requesterID);
	if (device == NULL)
		return B_NO_MEMORY;

	// Find a run of free vectors.
	int32 found = -1;
	for (uint32 base = 0; base + count <= GIC_ITS_MAX_VECTORS; base++) {
		bool free = true;
		for (uint32 i = 0; i < count && free; i++) {
			if ((fAllocated[(base + i) / 32] & (1u << ((base + i) % 32))) != 0)
				free = false;
		}
		if (free) {
			found = base;
			break;
		}
	}
	if (found < 0)
		return B_BUSY;

	for (uint32 i = 0; i < count; i++) {
		const uint32 index = found + i;
		const uint32 lpi = GIC_LPI_BASE + index;

		// Spread this device's events across the per-CPU collections so its
		// interrupts do not all pile onto the boot CPU. A device's events are
		// numbered from zero, so event 0 lands on CPU 0, event 1 on CPU 1, and
		// so on -- for ENA that puts the io queue's interrupt on a non-boot CPU
		// while the management interrupt stays on CPU 0.
		const uint32 collection = i % fCollectionCount;

		fAllocated[index / 32] |= 1u << (index % 32);
		fVectorDevice[index] = requesterID;
		fVectorEvent[index] = i;
		fVectorCollection[index] = collection;

		// Enable the LPI in the shared configuration table before it is
		// mapped, then let the ITS pick the change up.
		((volatile uint8*)fPropertyTable)[lpi - GIC_LPI_BASE]
			= GIC_PRIORITY_DEFAULT | GIC_LPI_CONFIG_ENABLE;

		status_t status = _MapInterrupt(requesterID, i, lpi, collection);
		if (status == B_OK) {
			// A MAPTI to a new collection takes effect once it is synced
			// against that collection's redistributor target.
			_Sync(fCollectionTargets[collection]);
		}
		if (status != B_OK) {
			// Returning with the earlier vectors still committed would leak
			// them for the rest of the boot -- FreeVectors() is never called
			// for a request that failed -- and leave their LPIs enabled in the
			// shared property table with nothing mapped behind them. This one
			// never reached the ITT, so it gets its bookkeeping cleared but no
			// DISCARD.
			((volatile uint8*)fPropertyTable)[lpi - GIC_LPI_BASE]
				= GIC_PRIORITY_DEFAULT;
			fAllocated[index / 32] &= ~(1u << (index % 32));

			for (uint32 j = 0; j < i; j++)
				_ReleaseVector((uint32)found + j);

			__asm__ __volatile__("dsb sy" ::: "memory");

			uint64 invall[4] = { GITS_CMD_INVALL, 0, 0, 0 };
			_SubmitCommand(invall);
			_Sync();

			return status;
		}
	}

	__asm__ __volatile__("dsb sy" ::: "memory");

	uint64 invall[4] = { GITS_CMD_INVALL, 0, 0, 0 };
	_SubmitCommand(invall);
	_Sync();

	startVector = fVectorBase + found;
	address = fTranslaterPhysical;
	data = 0;	// EventIDs are per device and start at zero

	TRACE("allocated %" B_PRIu32 " vector(s) from %" B_PRIu32 " for device %#"
		B_PRIx32 "\n", count, startVector, requesterID);
	return B_OK;
}


void
GICv3ITS::FreeVectors(uint32 count, uint32 startVector)
{
	MutexLocker locker(fLock);

	int32 index = (int32)startVector - fVectorBase;
	while (count > 0 && index >= 0 && index < GIC_ITS_MAX_VECTORS) {
		_ReleaseVector((uint32)index);

		index++;
		count--;
	}

	__asm__ __volatile__("dsb sy" ::: "memory");

	uint64 invall[4] = { GITS_CMD_INVALL, 0, 0, 0 };
	_SubmitCommand(invall);
	_Sync();
}


int32
GICv3ITS::VectorForLpi(uint32 intid) const
{
	const uint32 index = intid - GIC_LPI_BASE;
	if (intid < GIC_LPI_BASE || index >= GIC_ITS_MAX_VECTORS)
		return -1;

	return fVectorBase + index;
}


int32
GICv3ITS::CurrentCpuForVector(int32 vector) const
{
	const int32 index = vector - fVectorBase;
	if (vector < fVectorBase || index >= GIC_ITS_MAX_VECTORS)
		return -1;

	if ((fAllocated[index / 32] & (1u << (index % 32))) == 0)
		return -1;

	// A collection was mapped one-to-one onto each CPU, so the collection id an
	// event was routed to is the CPU id it is delivered on.
	return (int32)fVectorCollection[index];
}


int32
GICv3ITS::SetVectorAffinity(int32 vector, int32 cpu)
{
	const int32 index = vector - fVectorBase;
	if (vector < fVectorBase || index >= GIC_ITS_MAX_VECTORS)
		return -1;

	if ((fAllocated[index / 32] & (1u << (index % 32))) == 0)
		return -1;

	// Collection id == cpu id, but the ITS may hold fewer collections than
	// there are CPUs. If the requested CPU has no collection of its own, fold
	// it onto one that exists so the interrupt still lands somewhere sane --
	// and report the CPU actually targeted, never the one we could not reach.
	if (fCollectionCount == 0)
		return (int32)fVectorCollection[index];
	uint32 collection = (uint32)cpu;
	if (collection >= fCollectionCount)
		collection %= fCollectionCount;

	// Already there: nothing to move, and re-issuing MOVI would be needless
	// command-queue traffic from the (possibly hot) affinity dispatch.
	if (fVectorCollection[index] == collection)
		return (int32)collection;

	// Re-route the mapped event to the new collection, then make the change
	// visible by syncing against that collection's redistributor. MOVI keeps
	// the ITT entry, so no MAPTI/DISCARD is needed. Issued under fCommandLock
	// (inside _SubmitCommand) only, so this is safe with interrupts disabled.
	status_t status = _MoveInterrupt(fVectorDevice[index], fVectorEvent[index],
		collection);
	if (status != B_OK) {
		// The move did not take; the vector still targets its old collection.
		return (int32)fVectorCollection[index];
	}
	_Sync(fCollectionTargets[collection]);

	fVectorCollection[index] = collection;
	return (int32)collection;
}
