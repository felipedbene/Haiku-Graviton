/*
 * Copyright 2026 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_ARM64_ARCH_PMU_H_
#define _KERNEL_ARCH_ARM64_ARCH_PMU_H_


#include <SupportDefs.h>


struct kernel_args;


// The architecture allows up to 31 event counters plus the dedicated cycle
// counter, and how many actually exist is read from PMCR_EL0.N. Every Neoverse
// core so far has 6, so this facility keeps room for 8 and clamps PMCR_EL0.N to
// it rather than carrying 31 counters' worth of state per CPU for nothing.
#define ARM64_PMU_MAX_EVENT_COUNTERS	8


// One reading of one CPU's counters. A PMU reading without a CPU number is
// meaningless: the counters are per-core, and nothing migrates or sums them.
typedef struct arm64_pmu_sample {
	int32	cpu;
	bool	valid;
	uint32	eventCount;
	uint64	cycles;
		// PMCCNTR_EL0 since the last reset. 64 bit in hardware (PMCR_EL0.LC),
		// so this does not wrap in any useful timeframe.
	uint16	events[ARM64_PMU_MAX_EVENT_COUNTERS];
	uint64	values[ARM64_PMU_MAX_EVENT_COUNTERS];
		// Software-extended to 64 bit from the 32 bit hardware counters, which
		// is only correct if sampled more often than they wrap; see wrapped.
	uint32	wrapped;
		// Bitmap of event counters that overflowed at least once since the
		// previous sample, as reported by PMOVSCLR_EL0. Any bit set here means
		// the corresponding value may have lost a multiple of 2^32.
} arm64_pmu_sample;


#ifdef __cplusplus
extern "C" {
#endif


// Boot CPU, before any CPU is programmed: reads ID_AA64DFR0_EL1 only, and
// touches no PMU register, so it is safe where the PMU is trapped to EL2.
status_t	arm64_pmu_init(struct kernel_args* args);

// Registers the "pmu" KDL command. Late enough for the heap to exist.
status_t	arm64_pmu_init_post_modules(struct kernel_args* args);

// Programs and enables the calling CPU's counters, if the facility is on.
// Called on every CPU from arch_cpu_init_percpu().
void		arm64_pmu_init_percpu(struct kernel_args* args, int cpu);

bool		arm64_pmu_available(void);
bool		arm64_pmu_enabled(void);

// All of these act on the calling CPU only, and pin to it for their duration.
status_t	arm64_pmu_enable(void);
void		arm64_pmu_disable(void);
void		arm64_pmu_reset(void);
void		arm64_pmu_read(arm64_pmu_sample* sample);

// Dumps the calling CPU's counters and the derived ratios via dprintf(), for
// bracketing a code path under test. Cheap enough to call, not cheap enough to
// call in a loop.
void		arm64_pmu_dump(void);


#ifdef __cplusplus
}
#endif


#endif /* _KERNEL_ARCH_ARM64_ARCH_PMU_H_ */
