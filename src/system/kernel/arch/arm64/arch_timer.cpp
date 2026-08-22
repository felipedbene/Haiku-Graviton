/*
 * Copyright 2019-2022 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <boot/stage2.h>
#include <kernel.h>
#include <debug.h>
#include <interrupts.h>

#include <timer.h>
#include <arch/timer.h>
#include <arch/cpu.h>


static uint64 sTimerFrequency;
static bigtime_t sTimerMaxInterval;

#define TIMER_DISABLED (0)
#define TIMER_ENABLE (1)
#define TIMER_IMASK (2)
#define TIMER_ISTATUS (4)

#define TIMER_IRQ 27

// CNTKCTL_EL1 bits granting EL0 access to the counters. Without one of them
// set, CNTFRQ_EL0 is not readable from EL0 either.
#define CNTKCTL_EL0PCTEN (1 << 0)
#define CNTKCTL_EL0VCTEN (1 << 1)


void
arch_timer_set_hardware_timer(bigtime_t timeout)
{
	if (timeout > sTimerMaxInterval)
		timeout = sTimerMaxInterval;

	// Scale by the frequency itself rather than by a whole number of ticks per
	// microsecond: the counter is not required to run at a multiple of 1 MHz,
	// and does not (1.05 GHz on AWS Graviton, 62.5 MHz on QEMU's virt machine).
	WRITE_SPECIALREG(CNTV_TVAL_EL0, (timeout * sTimerFrequency) / 1000000);
	WRITE_SPECIALREG(CNTV_CTL_EL0, TIMER_ENABLE);
}


void
arch_timer_clear_hardware_timer()
{
	WRITE_SPECIALREG(CNTV_CTL_EL0, TIMER_DISABLED);
}


int32
arch_timer_interrupt(void *data)
{
	WRITE_SPECIALREG(CNTV_CTL_EL0, TIMER_DISABLED);
	return timer_interrupt();
}


int
arch_init_timer(kernel_args *args)
{
	sTimerFrequency = READ_SPECIALREG(CNTFRQ_EL0);

	// CNTV_TVAL_EL0 is a signed 32 bit down counter, so no more than INT32_MAX
	// ticks can be programmed at a time.
	sTimerMaxInterval = ((uint64)INT32_MAX * 1000000) / sTimerFrequency;

	dprintf("arch_timer: generic timer at %" B_PRIu64 " Hz, max interval %"
		B_PRIdBIGTIME " us\n", sTimerFrequency, sTimerMaxInterval);

	// system_time() is implemented by reading CNTVCT_EL0 and CNTFRQ_EL0 from
	// EL0, which is only allowed while this is set. The boot loader sets it as
	// well, but only on the path where it does not have to drop from EL2 to
	// EL1 first.
	WRITE_SPECIALREG(CNTKCTL_EL1, CNTKCTL_EL0VCTEN | CNTKCTL_EL0PCTEN);

	WRITE_SPECIALREG(CNTV_CTL_EL0, TIMER_DISABLED);
	install_io_interrupt_handler(TIMER_IRQ, &arch_timer_interrupt, NULL, 0);

	return B_OK;
}
