/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>

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


status_t
GICv3ITS::Init(phys_addr_t regs, size_t size, addr_t gicdRegs,
	const gicr_region* gicrRegions, uint32 gicrRegionCount, size_t gicrStride)
{
	// Nothing in Init() takes fLock: it runs single-threaded during boot, and
	// the interface is not reachable from anywhere else until the
	// msi_set_interface() at the end publishes it.
	mutex_init(&fLock, "gicv3 its");

	memset(fAllocated, 0, sizeof(fAllocated));
	memset(fDevices, 0, sizeof(fDevices));
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
	fPhysicalTargetAddress = (typer & GITS_TYPER_PTA) != 0;

	dprintf("gicv3-its: typer %#" B_PRIx64 ": itt entry size %" B_PRIu32 ", %"
		B_PRIu32 " event id bits, %" B_PRIu32 " device id bits, pta %d\n",
		typer, fIttEntrySize, fEventIDBits, fDeviceIDBits,
		fPhysicalTargetAddress ? 1 : 0);

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

	status = allocate_io_interrupt_vectors(GIC_ITS_MAX_VECTORS, &fVectorBase,
		INTERRUPT_TYPE_IRQ);
	if (status != B_OK) {
		ERROR("unable to allocate interrupt vectors for MSIs\n");
		return status;
	}

	// A single collection, targeting the redistributor of the boot CPU, is
	// enough: every LPI is delivered there.
	status = _MapCollection(0, fCollectionTarget, true);
	if (status != B_OK)
		return status;
	_Sync();

	dprintf("gicv3-its: ready, %d vectors from %" B_PRId32 ", translater %#"
		B_PRIxPHYSADDR "\n", GIC_ITS_MAX_VECTORS, fVectorBase,
		fTranslaterPhysical);


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
			// machines we care about stay small.
			uint32 bits = min_c(fDeviceIDBits, (uint32)16);
			entries = 1ull << bits;
		} else {
			entries = 16;
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
	bool haveTarget = false;
	uint32 redistributors = 0;
	for (uint32 region = 0; region < gicrRegionCount; region++) {
		addr_t frame = gicrRegions[region].base;
		phys_addr_t framePhysical = gicrRegions[region].physicalBase;
		const addr_t end = frame + gicrRegions[region].size;

		while (frame + gicrStride <= end) {
			const uint64 typer = gic_read64(frame + GICR_TYPER);

			addr_t pending;
			phys_addr_t pendingPhysical;
			status = allocate_its_table("gicv3-lpi-pending",
				ROUNDUP((1ul << fLpiIDBits) / 8, GICR_PENDBASER_ALIGNMENT),
				GICR_PENDBASER_ALIGNMENT, area, pending, pendingPhysical);
			if (status != B_OK) {
				ERROR("unable to allocate an LPI pending table\n");
				return status;
			}

			gic_write64(frame + GICR_PROPBASER, propbaser);
			gic_write64(frame + GICR_PENDBASER,
				(pendingPhysical & 0x000ffffffff0000ull)
					| GICR_BASER_INNER_CACHE | GICR_BASER_SHAREABILITY);
			gic_write32(frame + GICR_CTLR,
				gic_read32(frame + GICR_CTLR) | GICR_CTLR_ENABLE_LPIS);

			redistributors++;

			if (!haveTarget) {
				// Collections name their target either by physical
				// redistributor address or by the processor number the
				// redistributor reports.
				fCollectionTarget = fPhysicalTargetAddress
					? (framePhysical >> 16) : GICR_TYPER_PROC_NUM(typer);
				haveTarget = true;
			}

			if ((typer & GICR_TYPER_LAST) != 0)
				break;

			frame += gicrStride;
			framePhysical += gicrStride;
		}
	}

	if (!haveTarget) {
		ERROR("no redistributor found for the LPI collection\n");
		return B_ERROR;
	}

	dprintf("gicv3-its: lpis enabled on %" B_PRIu32 " redistributor(s), %"
		B_PRIu32 " lpi intid bits, collection target %#" B_PRIx64 "\n",
		redistributors, fLpiIDBits, fCollectionTarget);

	return B_OK;
}


// Commands are 32 bytes and consumed from a ring; GITS_CWRITER is the producer
// index in bytes and GITS_CREADR the consumer's.
status_t
GICv3ITS::_SubmitCommand(const uint64* command)
{
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
	uint64 command[4] = { GITS_CMD_SYNC, 0, fCollectionTarget << 16, 0 };
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

		fAllocated[index / 32] |= 1u << (index % 32);
		fVectorDevice[index] = requesterID;
		fVectorEvent[index] = i;

		// Enable the LPI in the shared configuration table before it is
		// mapped, then let the ITS pick the change up.
		((volatile uint8*)fPropertyTable)[lpi - GIC_LPI_BASE]
			= GIC_PRIORITY_DEFAULT | GIC_LPI_CONFIG_ENABLE;

		status_t status = _MapInterrupt(requesterID, i, lpi, 0);
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
