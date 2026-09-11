/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_H
#define ARCH_ARM64_GICV3_H

#include <SupportDefs.h>

#include <boot/interrupt_controller.h>

#include "soc.h"
#include "gicv3_its.h"
#include "gicv3_regs.h"


class GICv3InterruptController : public InterruptController {
public:
								GICv3InterruptController(
									const intc_info& info);

			void				EnableInterrupt(int32 irq);
			void				DisableInterrupt(int32 irq);
			void				HandleInterrupt();

			// #224 boot-bringup freeze diagnostic (remove with #224): read the
			// CPU-interface state (running priority etc.) from the idle path,
			// where a drained PE must show ICC_RPR_EL1 == 0.
			void				Debug224IdleProbe();
			void				Debug224DumpCpuIface(const char* where);
			int32				AssignToCpu(int32 irq, int32 cpu);
			void				SendMulticastIci(CPUSet& cpuSet);
			void				SendBroadcastIci();

			// Brings up the Interrupt Translation Service, enabling MSIs.
			// Deferred until the kernel can allocate memory for its tables.
			status_t			InitITS(phys_addr_t regs, size_t size);

private:
			void				_PerCpuInit();
			void				_MapRedistributors(const intc_info& info);
			void				_PrefaultRedistributors();
			void				_DistributorInit();

			// Locates and returns this PE's redistributor RD_base frame.
			addr_t				_CurrentRedistributor();

			void				_WaitForRwp();

			uint32				_ReadGicd(uint32 offset);
			void				_WriteGicd(uint32 offset, uint32 value);
			void				_WriteGicd64(uint32 offset, uint64 value);

	static	void				_SendSgi(uint64 mpidr, uint32 sgiId);

			addr_t				fGicdRegs;
			GICv3ITS*			fITS;

			gicr_region			fGicrRegions[INTC_MAX_GICR_REGIONS];
			uint32				fGicrRegionCount;
			size_t				fGicrStride;

			uint32				fIrqCount;
};


#endif /* ARCH_ARM64_GICV3_H */
