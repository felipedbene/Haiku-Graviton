/*
 * Copyright 2026 DeBeOS. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYSTEM_ARM64_PMU_SYSCALLS_H
#define _SYSTEM_ARM64_PMU_SYSCALLS_H


#include <SupportDefs.h>


/*!	Userland ABI for the arm64 PMUv3 counter facility.

	The kernel PMU facility (src/system/kernel/arch/arm64/arch_pmu.cpp) is
	deliberately not a userland interface: there is no dedicated syscall and no
	/dev node, and PMUSERENR_EL0 is written to zero so EL0 cannot touch the
	counters directly. This header is the one narrow, read-mostly conduit that
	the `pmustat` diagnostic (src/bin/pmustat) uses to reach it, routed through
	the generic-syscall mechanism (_kern_generic_syscall) rather than a new
	syscall number.

	The conduit reaches the PMU counters of the CPU the calling thread happens
	to be running on -- the counters are strictly per core (never migrated,
	never summed, not saved across a context switch), so a caller that wants a
	coherent reading must pin itself to one CPU (sched_setaffinity) for the
	whole measurement. Every reading carries the CPU number it was taken on so
	that a stitched-together reading is at least detectable.
*/

// Generic-syscall subsystem name, passed as the first argument to
// _kern_generic_syscall(). Registered by arm64_pmu_init_post_modules() only
// when PMUv3 is present on the part.
#define ARM64_PMU_SYSCALLS			"arm64_pmu"

// Bumped if this ABI changes incompatibly; the tool checks it via INFO.
#define ARM64_PMU_SYSCALL_VERSION	1

// Mirrors ARM64_PMU_MAX_EVENT_COUNTERS in the kernel arch_pmu.h. The two must
// agree; a static assert in the handler guards it.
#define ARM64_PMU_USER_MAX_EVENTS	8


// Generic-syscall function selectors.
enum {
	// buffer -> arm64_pmu_user_info. Never touches a PMU register, so it is
	// safe even where the facility is disabled or PMU access is trapped to EL2.
	ARM64_PMU_SYSCALL_INFO			= 0,

	// buffer -> uint32 preset index in [0, presetCount). Programs and zeroes
	// the event counters of the CPU the calling thread is on, starting a fresh
	// measurement window. Returns B_NOT_SUPPORTED when the facility is off (the
	// "arm64_pmu" boot setting was not applied), B_BAD_VALUE for a bad index.
	ARM64_PMU_SYSCALL_SET_PRESET	= 1,

	// buffer -> arm64_pmu_user_events. Programs and zeroes the calling CPU's
	// event counters to an explicit list of ARM PMUv3 event numbers, starting a
	// fresh window. Lets the tool select exactly the <= generalCounters events a
	// runbook pass needs, in an order that fits -- rather than a fixed preset
	// whose tail is dropped once the sampling counter is reserved. Same gating
	// and errors as SET_PRESET; B_BAD_VALUE if count is 0 or exceeds the room.
	ARM64_PMU_SYSCALL_SET_EVENTS	= 3,

	// buffer -> arm64_pmu_user_sample. Reads the calling CPU's counters. The
	// kernel software-extends the 32-bit event counters across wraps, but only
	// across the interval since the previous read on that CPU, so a long window
	// must be polled often enough that no counter wraps twice between reads
	// (well under a second at Graviton clocks) -- exactly the sampling the tool
	// does. Returns B_NOT_SUPPORTED if this CPU has not been programmed.
	ARM64_PMU_SYSCALL_READ			= 2
};


// One reading of one CPU's counters, marshalled to userland. A subset of the
// kernel's arm64_pmu_sample, with fixed-width fields only.
typedef struct arm64_pmu_user_sample {
	int32	cpu;
	uint8	valid;
	uint8	sampling;
	uint8	reserved[2];
	uint32	eventCount;
	uint32	wrapped;
		// Bitmap of event counters that overflowed at least once since the
		// previous read on this CPU. A set bit means the matching value may be
		// low by a multiple of 2^32 -- the "poll more often" warning flag.
	uint64	cycles;
		// PMCCNTR_EL0 since the last SET_PRESET. 64-bit in hardware, does not
		// wrap in any useful window.
	uint16	events[ARM64_PMU_USER_MAX_EVENTS];
		// ARM PMUv3 event numbers actually programmed, so the tool derives its
		// ratios from what the counter holds rather than assuming a layout.
	uint64	values[ARM64_PMU_USER_MAX_EVENTS];
} arm64_pmu_user_sample;


// An explicit set of events to program, for ARM64_PMU_SYSCALL_SET_EVENTS.
typedef struct arm64_pmu_user_events {
	uint32	count;
		// Number of valid entries in events[]. Must be in [1, generalCounters];
		// a larger count is rejected rather than silently truncated.
	uint16	events[ARM64_PMU_USER_MAX_EVENTS];
		// ARM PMUv3 event numbers (e.g. 0x08 INST_RETIRED, 0x11 CPU_CYCLES).
} arm64_pmu_user_events;


// Static capabilities of the facility on this instance. Read once.
typedef struct arm64_pmu_user_info {
	uint32	version;
		// ARM64_PMU_SYSCALL_VERSION the kernel implements.
	uint8	available;
		// PMUv3 is implemented (ID_AA64DFR0_EL1.PMUVer). Says nothing about
		// whether EL2 lets us reach the counters.
	uint8	enabled;
		// The facility was turned on ("arm64_pmu" boot setting). Only then can
		// SET_PRESET/READ touch a PMU register without risking an EL2 trap.
	uint8	longCounters;
		// FEAT_PMUv3p5: the event counters are 64-bit and do not wrap.
	uint8	reserved;
	uint32	generalCounters;
		// Event counters available to a preset (PMCR_EL0.N minus the one
		// reserved for the sampling profiler). On the 6-counter Neoverse parts
		// this is 5, so a 6-event preset drops its tail.
	uint32	presetCount;
		// Number of valid ARM64_PMU_SYSCALL_SET_PRESET indices.
	uint64	pmuVersion;
		// Raw ID_AA64DFR0_EL1.PMUVer field value, for reporting.
} arm64_pmu_user_info;


#endif	/* _SYSTEM_ARM64_PMU_SYSCALLS_H */
