/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_ITS_H
#define ARCH_ARM64_GICV3_ITS_H

#include <SupportDefs.h>
#include <arch/generic/msi.h>
#include <lock.h>


// Number of MSI vectors the ITS hands out. Each one consumes an LPI and an
// entry in the kernel's I/O interrupt vector space.
#define GIC_ITS_MAX_VECTORS		256

// Devices the ITS will track. Each gets its own Interrupt Translation Table.
#define GIC_ITS_MAX_DEVICES		32

// Events per device, and therefore the size of each device's ITT.
#define GIC_ITS_EVENTS_PER_DEVICE	32


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
									addr_t gicdRegs, addr_t gicrRegs,
									phys_addr_t gicrPhysical, size_t gicrSize,
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

private:
			status_t			_InitTables();
			status_t			_InitCommandQueue();
			status_t			_InitLpis(addr_t gicdRegs, addr_t gicrRegs,
									phys_addr_t gicrPhysical, size_t gicrSize,
									size_t gicrStride);

			its_device*			_DeviceFor(uint32 requesterID);

			status_t			_SubmitCommand(const uint64* command);
			status_t			_Sync();
			status_t			_MapDevice(uint32 deviceID, phys_addr_t itt,
									uint32 eventIDBits, bool valid);
			status_t			_MapCollection(uint32 collection,
									uint64 target, bool valid);
			status_t			_MapInterrupt(uint32 deviceID, uint32 eventID,
									uint32 lpi, uint32 collection);
			status_t			_Discard(uint32 deviceID, uint32 eventID);

			void				_ReleaseVector(uint32 index);

			// Guards the command queue and the vector allocator against
			// concurrent AllocateVectors()/FreeVectors() callers. Not taken by
			// VectorForLpi(), which runs from the interrupt path and only reads
			// state that Init() fixes once.
			mutex				fLock;

			addr_t				fRegs;
			phys_addr_t			fTranslaterPhysical;

			uint32				fIttEntrySize;
			uint32				fEventIDBits;
			uint32				fDeviceIDBits;
			bool				fPhysicalTargetAddress;
			uint64				fCollectionTarget;

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

			its_device			fDevices[GIC_ITS_MAX_DEVICES];
};


#endif /* ARCH_ARM64_GICV3_ITS_H */
