/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_H
#define ARCH_ARM64_GICV3_H

#include <SupportDefs.h>

#include "soc.h"
#include "gicv3_its.h"


class GICv3InterruptController : public InterruptController {
public:
								GICv3InterruptController(
									phys_addr_t gicdRegs, size_t gicdSize,
									phys_addr_t gicrRegs, size_t gicrSize);

			void				EnableInterrupt(int32 irq);
			void				DisableInterrupt(int32 irq);
			void				HandleInterrupt();
			void				SendMulticastIci(CPUSet& cpuSet);
			void				SendBroadcastIci();

			// Brings up the Interrupt Translation Service, enabling MSIs.
			// Deferred until the kernel can allocate memory for its tables.
			status_t			InitITS(phys_addr_t regs, size_t size);

private:
			void				_PerCpuInit();
			void				_PrefaultRedistributors();
			void				_DistributorInit();

			// Locates and returns this PE's redistributor SGI/PPI frame.
			addr_t				_CurrentRedistributor();

			void				_WaitForRwp();

			uint32				_ReadGicd(uint32 offset);
			void				_WriteGicd(uint32 offset, uint32 value);
			void				_WriteGicd64(uint32 offset, uint64 value);

	static	void				_SendSgi(uint64 mpidr, uint32 sgiId);

			addr_t				fGicdRegs;
			addr_t				fGicrRegs;
			phys_addr_t			fGicrPhysical;
			GICv3ITS*			fITS;
			size_t				fGicrSize;
			size_t				fGicrStride;
			uint32				fIrqCount;
};


#endif /* ARCH_ARM64_GICV3_H */
