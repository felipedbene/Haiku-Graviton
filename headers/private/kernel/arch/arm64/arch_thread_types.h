/*
 * Copyright 2022, Haiku Inc. All rights reserved.
 * Copyright 2018, Jaroslaw Pelczar <jarek@jpelczar.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_ARM64_ARCH_THREAD_TYPES_H_
#define _KERNEL_ARCH_ARM64_ARCH_THREAD_TYPES_H_


#include <kernel.h>

#define	IFRAME_TRACE_DEPTH 4


// First-cut SVE support caps the effective vector length at 256 bits. ZCR_EL1
// is programmed to clamp a wider hardware VL down to this bound (arch_cpu.cpp),
// so the fixed-size per-thread save area below is always large enough. Raising
// the cap only needs this constant and the ZCR_EL1 clamp changed.
#define	SVE_MAX_VL_BYTES	32			// 256-bit vector length
#define	SVE_MAX_PRED_BYTES	(SVE_MAX_VL_BYTES / 8)	// predicate/FFR = VL/8


// The iframe FP state is deliberately left at its NEON size: the exception path
// spills it on the 16 KB kernel stack on every entry, and an SVE-sized area here
// (~1 KB extra per frame) risks a stack overflow under nested exceptions -- the
// failure mode that held the first SVE attempt (silent hang, no panic). The
// wide SVE state lives off-stack in arch_thread::sve instead (see below), which
// is also how x86_64 keeps its FPU state per thread rather than in the iframe.
struct aarch64_fpu_state
{
	uint64 regs[32 * 2];	// V0-V31; the fp_q view read by signals/backtrace,
							// and the low 128 bits of Z0-Z31 when SVE is active.
	uint64 fpsr;
	uint64 fpcr;
};


// Per-thread SVE register file, meaningful only when gArm64SVEVectorBytes != 0.
// Saved/restored by _fp_save_el0/_fp_restore_el0 (arch_asm.S) across EL0<->EL1
// transitions -- SVE state is only ever live at EL0, and EL0 cannot nest, so one
// buffer per thread is sufficient and it rides context switches with the thread.
// The slot stride is the *effective* VL, matching the "MUL VL" addressing the
// asm uses, so only the first 32 * gArm64SVEVectorBytes bytes of z[] are touched.
// z[]'s low 128 bits of each Zn alias the Vn in aarch64_fpu_state::regs; on the
// restore path z[] is the single source of truth for V0-V31.
struct arm64_sve_state
{
	uint8 z[32 * SVE_MAX_VL_BYTES];		// Z0-Z31
	uint8 p[16 * SVE_MAX_PRED_BYTES];	// P0-P15
	uint8 ffr[SVE_MAX_PRED_BYTES];		// First-Fault Register
};


/* raw exception frames */
struct iframe {
	// return info
	uint64 elr;
	uint64 spsr;
	uint64 x[29];
	uint64 fp;
	uint64 lr;
	uint64 sp;
	uint64 tpidr;

	// exception info
	uint64 esr;
	uint64 far;

	// fpu
	struct aarch64_fpu_state fpu;
};


struct iframe_stack {
	struct iframe *frames[IFRAME_TRACE_DEPTH];
	int32	index;
};


struct arch_thread {
	uint64 regs[14]; // x19-x30, sp, tpidr_el0
	uint64 fp_regs[8]; // d8-d15
	uint64 old_x0;

	// used to track interrupts on this thread
	struct iframe_stack	iframes;

	// Per-thread CPU-cycle accounting, driven from PMCCNTR_EL0 on each context
	// switch. Both stay zero and untouched while the arm64 PMU facility is off
	// (arm64_pmu_enabled()), so the timer-based CPU-time estimate is unaffected.
	uint64 cpu_cycles;
		// Core cycles attributed to this thread, summed at each switch-out.
	uint64 cycle_ref;
		// PMCCNTR_EL0 sampled when this thread was last switched in. The
		// switch-out delta (now - cycle_ref) is what gets added to cpu_cycles.
		// The snapshot and the delta are always taken on the same core within
		// one running interval, so the per-core counter's absolute value never
		// has to be compared across a migration.

	// Off-stack SVE register file for this thread. Zeroed at thread creation
	// (arch_thread_init_thread_struct) and, on SVE hardware, saved on every
	// EL0->EL1 entry and restored on every EL1->EL0 return. Kept out of the
	// iframe on purpose so the kernel stack does not grow (see the note on
	// aarch64_fpu_state above). Untouched on non-SVE hardware.
	struct arm64_sve_state sve;
};


struct arch_team {
	int			dummy;
};


struct arch_fork_arg {
	struct iframe frame;
};


#endif /* _KERNEL_ARCH_ARM64_ARCH_THREAD_TYPES_H_ */
