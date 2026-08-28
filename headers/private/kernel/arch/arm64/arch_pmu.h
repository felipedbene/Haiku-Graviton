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
		// the corresponding value may have lost a multiple of 2^32. The cycle
		// counter and the dedicated sampling counter are excluded.
	bool	sampling;
		// A counter is dedicated to overflow-driven sampling on this CPU
		// (E-PMU-1a/1b). The three fields below are only meaningful when set.
	uint32	sampleCounter;
		// Event-counter index reserved for the CPU_CYCLES sampling source.
	uint64	samplePeriod;
		// Cycles between sampling-counter overflows.
	uint64	sampleOverflows;
		// Sampling overflow interrupts serviced on this CPU since the last
		// reset. Under a CPU-bound load this climbs at ~(core clock / period)
		// per second; a runaway would be an interrupt storm.
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

// True only where PMCCNTR_EL0 is SAFE to read on the CALLING CPU: the facility
// is available AND this CPU's counters have been programmed. Stricter than
// arm64_pmu_enabled() -- the "arm64_pmu" boot flag can be set while PMU access
// is still trapped to EL2 (AWS sizes without full PMU) or before this CPU's
// pmu_program_cpu() has run, and reading PMCCNTR_EL0 in either window faults.
// Cheap; intended for the context-switch hot path. Call with interrupts off
// (it reads the current CPU's state).
bool		arm64_pmu_pmccntr_usable(void);

// All of these act on the calling CPU only, and pin to it for their duration.
status_t	arm64_pmu_enable(void);
void		arm64_pmu_disable(void);
void		arm64_pmu_reset(void);
void		arm64_pmu_read(arm64_pmu_sample* sample);

// Dumps the calling CPU's counters and the derived ratios via dprintf(), for
// bracketing a code path under test. Cheap enough to call, not cheap enough to
// call in a loop.
void		arm64_pmu_dump(void);

// Measures the core clock of the CPU this is called on, by counting
// PMCCNTR_EL0 cycles over a known CNTVCT_EL0 interval. There is no register
// that simply states the core clock on ARM, and CNTFRQ_EL0 is the timer's rate
// rather than the core's, so counting is the only way to get it.
//
// Costs a few milliseconds and pins to one CPU with interrupts off for 1 ms at
// a time, so this is for one-shot calibration, not for polling. Requires the
// facility to be enabled, because it has to read a PMU register; returns
// B_NOT_SUPPORTED if it is not, and B_ERROR if what it measured is not a
// credible clock rate. Never reports success without a real measurement.
status_t	arm64_pmu_measure_core_frequency(uint64* frequency);


#ifdef __cplusplus
}
#endif


#endif /* _KERNEL_ARCH_ARM64_ARCH_PMU_H_ */
