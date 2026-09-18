/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <OS.h>

#include <arch_cpu.h>
#include <libroot_private.h>
#include <real_time_data.h>


// CNTVCT_EL0 counts at the fixed rate CNTFRQ_EL0 reports, and that frequency is
// constant for the life of the machine. system_time() is the most-frequently
// called function in the system, so rather than divide ticks by the frequency
// on every call - an integer udiv is many cycles, and the old two-step code did
// two of them - the scale factor is precomputed once as a multiply and a shift:
//
//     microseconds = (ticks * sMultiplier) >> sShift
//
// The multiplier is kept within 64 bits, so the ticks * sMultiplier product
// always fits the 128 bits it is computed in: nothing overflows or is truncated
// over the full 2**64 tick range. The scale factor itself is a rounded
// approximation of 10^6 / CNTFRQ (see __arch_compute_time_conversion), so a
// result can differ from the exact ticks*10^6/freq by at most one microsecond
// (one LSB), and that bound does not grow with uptime. That preserves the
// correctness the earlier fix established - converting in one 64-bit step
// overflowed after ~5 hours on a 1.05 GHz counter and jumped the clock ~4.9
// hours backwards - while dropping the per-call divides.
//
// This one source file is compiled into both libroot and the kernel, so each
// gets its own copy of these factors. They are derived once from the owning
// init path - the kernel's arch_init_timer() at boot and libroot's
// __arch_init_time() at process startup, both single-threaded before any
// concurrent system_time() call - with a lazy first-call fallback in case
// system_time() runs before that site. Because both init sites complete while
// still single-threaded, any such early call is single-threaded too, so the
// fallback's plain stores need no barrier; after init no writer remains. sShift
// is never legitimately zero (see __arch_compute_time_conversion), so it
// doubles as the "not yet derived" sentinel.
static uint64 sMultiplier;
static uint32 sShift;


// Represent the scale factor 10^6 / freq (microseconds per counter tick) as
// mult / 2^shift, following the clocksource mult/shift derivation in Linux's
// clocks_calc_mult_shift(): pick the largest shift for which mult still fits in
// 64 bits. A larger shift is a smaller per-tick rounding error; the 64-bit cap
// keeps ticks * mult inside 128 bits so the result stays exact across the whole
// counter range. freq >= 1 guarantees a fit at shift 1, so the loop always
// leaves shift >= 1 and mult != 0.
static void
__arch_compute_time_conversion(uint64 freq, uint64* multiplier, uint32* shift)
{
	uint32 s;
	unsigned __int128 mult = 0;

	for (s = 64; s > 0; s--) {
		mult = ((unsigned __int128)1000000 << s) + freq / 2;
		mult /= freq;
		if ((mult >> 64) == 0)
			break;
	}

	*multiplier = (uint64)mult;
	*shift = s;
}


void
__arch_init_system_time(void)
{
	uint64 freq;
	uint64 multiplier;
	uint32 shift;

	asm volatile("mrs %0, CNTFRQ_EL0": "=r" (freq));
	__arch_compute_time_conversion(freq, &multiplier, &shift);

	// Publish the multiplier before the shift: sShift is the sentinel the hot
	// path tests, so it must not become non-zero until the multiplier it pairs
	// with is in place. Both init sites run single-threaded before any other
	// thread or CPU can observe these, so no stronger barrier is required.
	sMultiplier = multiplier;
	sShift = shift;
}


bigtime_t
system_time(void)
{
	uint64 ticks;

	// The virtual counter is the one to read, for two reasons: it is what the
	// virtual timer the kernel arms (CNTV_*) counts against, and a hypervisor
	// rebases it so that it starts at zero when the machine does. The physical
	// counter is not rebased, so a guest reading CNTPCT_EL0 sees the host's
	// counter, which has been running since the host powered on.
	asm volatile("mrs %0, CNTVCT_EL0": "=r" (ticks));

	if (sShift == 0)
		__arch_init_system_time();

	return (bigtime_t)(((unsigned __int128)ticks * sMultiplier) >> sShift);
}
