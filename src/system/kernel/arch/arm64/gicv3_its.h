/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_ITS_H
#define ARCH_ARM64_GICV3_ITS_H

#include <SupportDefs.h>
#include <arch/generic/msi.h>
#include <lock.h>

#include "gicv3_regs.h"


// Number of MSI vectors the ITS hands out. Each one consumes an LPI and an
// entry in the kernel's I/O interrupt vector space.
#define GIC_ITS_MAX_VECTORS		256

// Devices the ITS will track. Each gets its own Interrupt Translation Table.
#define GIC_ITS_MAX_DEVICES		32

// Events per device, and therefore the size of each device's ITT.
#define GIC_ITS_EVENTS_PER_DEVICE	32

// Upper bound on the per-CPU ITS collections. One collection is mapped per CPU
// so MSIs can be spread across cores; this caps the fixed collection-target
// array and matches the arm64 SMP_MAX_CPUS (asserted in the implementation).
#define GIC_ITS_MAX_COLLECTIONS		64

// Layout of the DeviceID the ITS derives from a PCI requester ID (see
// pci_requester_id() in the PCI bus manager): the 16-bit BDF with the PCI
// segment folded in above it. The device table has to span the segment bits as
// well, or two devices with the same bus/device/function in different segments
// would alias onto one DeviceID and steal each other's interrupts. 3 bits
// covers the 8 segments the PCI bus manager allows (its MAX_PCI_DOMAINS); keep
// the two in step. The device table is still capped at what the ITS reports it
// can address (GITS_TYPER.Devbits), so a segment beyond that range is rejected
// at map time rather than silently aliased.
#define GIC_ITS_SEGMENT_ID_SHIFT	16
#define GIC_ITS_SEGMENT_ID_BITS		3
#define GIC_ITS_DEVICE_ID_BITS		(GIC_ITS_SEGMENT_ID_SHIFT + GIC_ITS_SEGMENT_ID_BITS)


struct its_device {
	uint32		requester_id;
	addr_t		itt;
	phys_addr_t	itt_physical;
	bool		valid;
};


class GICv3ITS : public MSIInterface {
public:
	virtual						~GICv3ITS() {}

			status_t			Init(phys_addr_t regs, size_t size,
									addr_t gicdRegs,
									const gicr_region* gicrRegions,
									uint32 gicrRegionCount,
									size_t gicrStride);

			// MSIInterface
			status_t			AllocateVectors(uint32 count,
									uint32& startVector, uint64& address,
									uint32& data);
			status_t			AllocateVectors(uint32 requesterID,
									uint32 count, uint32& startVector,
									uint64& address, uint32& data);
			void				FreeVectors(uint32 count, uint32 startVector);

			// Translates an incoming LPI INTID into the kernel interrupt
			// vector it was allocated against, or a negative value if the
			// INTID does not belong to us.
			int32				VectorForLpi(uint32 intid) const;

			// Reports the CPU an allocated MSI vector currently targets, via
			// the ITS collection it was mapped to (collection id == cpu id), or
			// a negative value if the vector is not one of ours. Reads only
			// state fixed at allocation, so it is safe from the interrupt path.
			int32				CurrentCpuForVector(int32 vector) const;

			// Re-routes an allocated MSI vector to the collection that targets
			// the requested CPU (collection id == cpu id) with a MOVI, and
			// returns the CPU it now targets. If the ITS holds fewer collections
			// than there are CPUs the request may fold onto one it can address;
			// the return is always the CPU actually targeted, never negative.
			// Issues ITS commands under a spinlock only (never the fLock mutex),
			// so it is callable with interrupts disabled from the affinity
			// dispatch -- unlike AllocateVectors(), which may sleep.
			int32				SetVectorAffinity(int32 vector, int32 cpu);

private:
			status_t			_InitTables();
			status_t			_InitCommandQueue();
			status_t			_InitLpis(addr_t gicdRegs,
									const gicr_region* gicrRegions,
									uint32 gicrRegionCount,
									size_t gicrStride);

			its_device*			_DeviceFor(uint32 requesterID);

			status_t			_SubmitCommand(const uint64* command);
			status_t			_Sync();
			status_t			_Sync(uint64 target);
			status_t			_MapDevice(uint32 deviceID, phys_addr_t itt,
									uint32 eventIDBits, bool valid);
			status_t			_MapCollection(uint32 collection,
									uint64 target, bool valid);
			status_t			_MapInterrupt(uint32 deviceID, uint32 eventID,
									uint32 lpi, uint32 collection);
			status_t			_MoveInterrupt(uint32 deviceID, uint32 eventID,
									uint32 collection);
			status_t			_Discard(uint32 deviceID, uint32 eventID);

			void				_ReleaseVector(uint32 index);

			// Guards the vector allocator (fAllocated bitmap, fDevices, and the
			// sleeping ITT/table allocations) against concurrent
			// AllocateVectors()/FreeVectors() callers. Not taken by
			// VectorForLpi(), which runs from the interrupt path and only reads
			// state that Init() fixes once.
			mutex				fLock;

			// Guards a single command-ring submission (fCommandIndex, CWRITER,
			// the CREADR drain). Held only across one _SubmitCommand(), never
			// while sleeping, so SetVectorAffinity() can re-route a vector with
			// interrupts disabled without taking fLock. AllocateVectors() holds
			// fLock for its bookkeeping and this briefly per command underneath.
			spinlock			fCommandLock;

			addr_t				fRegs;
			phys_addr_t			fTranslaterPhysical;

			uint32				fIttEntrySize;
			uint32				fEventIDBits;
			uint32				fDeviceIDBits;
			// DeviceID bits the device table actually covers: the requester-ID
			// layout (GIC_ITS_DEVICE_ID_BITS) clamped to what the ITS reports it
			// can address (fDeviceIDBits). A requester ID wider than this cannot
			// be mapped without indexing past the table.
			uint32				fDeviceTableBits;
			bool				fPhysicalTargetAddress;

			// One collection per CPU, each targeting that CPU's redistributor,
			// so MSIs can be spread across cores. Collection id == cpu id.
			// fCollectionTarget keeps the boot CPU's target for the plain
			// _Sync(). fMaxCollections is what the ITS can hold.
			uint64				fCollectionTarget;
			uint64				fCollectionTargets[GIC_ITS_MAX_COLLECTIONS];
			uint32				fCollectionCount;
			uint32				fMaxCollections;

			addr_t				fCommandQueue;
			phys_addr_t			fCommandQueuePhysical;
			uint32				fCommandIndex;

			addr_t				fPropertyTable;
			phys_addr_t			fPropertyTablePhysical;
			uint32				fLpiIDBits;

			int32				fVectorBase;
			uint32				fAllocated[GIC_ITS_MAX_VECTORS / 32];
			uint32				fVectorDevice[GIC_ITS_MAX_VECTORS];
			uint32				fVectorEvent[GIC_ITS_MAX_VECTORS];
			uint32				fVectorCollection[GIC_ITS_MAX_VECTORS];

			its_device			fDevices[GIC_ITS_MAX_DEVICES];
};


#endif /* ARCH_ARM64_GICV3_ITS_H */
