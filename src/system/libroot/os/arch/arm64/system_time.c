/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <OS.h>

#include <arch_cpu.h>
#include <libroot_private.h>
#include <real_time_data.h>


bigtime_t
system_time(void)
{
	uint64 ticks;
	uint64 freq;
	uint64 seconds;

	// The virtual counter is the one to read, for two reasons: it is what the
	// virtual timer the kernel arms (CNTV_*) counts against, and a hypervisor
	// rebases it so that it starts at zero when the machine does. The physical
	// counter is not rebased, so a guest reading CNTPCT_EL0 sees the host's
	// counter, which has been running since the host powered on.
	asm volatile("mrs %0, CNTVCT_EL0": "=r" (ticks));
	asm volatile("mrs %0, CNTFRQ_EL0": "=r" (freq));

	// Convert in two steps, because ticks * 1000000 overflows 64 bits after
	// 2**64 / 10**6 ticks. That is under five hours of counting on the 1.05GHz
	// counter of an AWS Graviton, and every time it happened the clock jumped
	// 2**64 / CNTFRQ_EL0 microseconds (4h52m48s there) into the past.
	seconds = ticks / freq;
	return (bigtime_t)(seconds * 1000000
		+ ((ticks - seconds * freq) * 1000000) / freq);
}
