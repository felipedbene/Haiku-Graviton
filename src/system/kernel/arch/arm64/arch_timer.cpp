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

#include <string.h>


static uint64 sTimerFrequency;
static bigtime_t sTimerMaxInterval;

#define TIMER_DISABLED (0)
#define TIMER_ENABLE (1)
#define TIMER_IMASK (2)
#define TIMER_ISTATUS (4)

// The generic timer the scheduler runs on is chosen by the exception level the
// kernel actually executes at, because that determines which timer is "ours"
// and which PPI the GIC delivers it on:
//
//   * On a KVM guest (e.g. c7g.large) the kernel runs at EL1 and owns the
//     non-secure EL1 physical timer, CNTP_*_EL0, delivered on PPI INTID 30.
//   * On bare metal (e.g. c7g.metal) the boot loader finds FEAT_VHE and keeps
//     the kernel at EL2 as a VHE host (HCR_EL2.E2H); it never drops to EL1.
//     There the EL1 timers are not ours: the EL1 virtual timer PPI (INTID 27)
//     and the EL1 physical timer PPI (INTID 30) are never delivered, so a
//     kernel that programmed either was tickless and wedged the instant it
//     first blocked on a scheduler tick / timed wait. The host timer at EL2 is
//     the EL2 physical timer CNTHP_*_EL2, delivered on PPI INTID 26.
//
// (Diagnosed on dev.haiku #224 by hardware register readback: at the freeze
// every one of the 64 PEs had a timer-fire count of 0 while device IRQs kept
// firing, and CurrentEL read EL2 on metal versus EL1 under KVM. With the EL2
// physical timer selected, metal ticks and boots to first-login; the EL1 path
// is unchanged and still verified booting under KVM.)
//
// These PPI numbers are the Arm Base System Architecture assignments the ACPI
// GTDT reflects (26 EL2 physical, 27 EL1 virtual, 30 EL1 physical non-secure).
// The two timers' control registers share the ENABLE/IMASK/ISTATUS layout, so
// only the register encoding and the INTID differ between the two paths.
//
// They are a *recommendation*, not part of the architecture, and they are only
// the fallback here: where firmware described the timer the boot loader carries
// what it said in arch_args.timer and that is used instead. Every platform
// measured so far happens to agree with these numbers, which is why nothing
// noticed that they were never read from anywhere.
#define TIMER_IRQ_EL1_PHYS 30
#define TIMER_IRQ_EL2_PHYS 26

// CNTKCTL_EL1 bits granting EL0 access to the counters. Either one of them
// also makes CNTFRQ_EL0 readable from EL0.
#define CNTKCTL_EL0PCTEN (1 << 0)
#define CNTKCTL_EL0VCTEN (1 << 1)

// True when the kernel runs as a VHE host at EL2 (bare metal) and must drive
// the EL2 physical timer; false when it runs at EL1 (KVM guest) and drives the
// EL1 physical timer. Set once in arch_init_timer.
static bool sUseEL2Timer;


void
arch_timer_set_hardware_timer(bigtime_t timeout)
{
	if (timeout > sTimerMaxInterval)
		timeout = sTimerMaxInterval;

	// Scale by the frequency itself rather than by a whole number of ticks per
	// microsecond: the counter is not required to run at a multiple of 1 MHz,
	// and does not (1.05 GHz on AWS Graviton, 62.5 MHz on QEMU's virt machine).
	// TVAL is a relative down-counter, so it programs the same interval whether
	// the physical or virtual counter drives it - the CNTVOFF rebase that keeps
	// system_time()'s CNTVCT reads meaningful under KVM does not affect it.
	uint64 ticks = (timeout * sTimerFrequency) / 1000000;
	if (sUseEL2Timer) {
		WRITE_SPECIALREG(CNTHP_TVAL_EL2, ticks);
		WRITE_SPECIALREG(CNTHP_CTL_EL2, TIMER_ENABLE);
	} else {
		WRITE_SPECIALREG(CNTP_TVAL_EL0, ticks);
		WRITE_SPECIALREG(CNTP_CTL_EL0, TIMER_ENABLE);
	}
}


void
arch_timer_clear_hardware_timer()
{
	if (sUseEL2Timer)
		WRITE_SPECIALREG(CNTHP_CTL_EL2, TIMER_DISABLED);
	else
		WRITE_SPECIALREG(CNTP_CTL_EL0, TIMER_DISABLED);
}


int32
arch_timer_interrupt(void *data)
{
	if (sUseEL2Timer)
		WRITE_SPECIALREG(CNTHP_CTL_EL2, TIMER_DISABLED);
	else
		WRITE_SPECIALREG(CNTP_CTL_EL0, TIMER_DISABLED);
	return timer_interrupt();
}


int
arch_init_timer(kernel_args *args)
{
	const arm_generic_timer_info& timerInfo = args->arch_args.timer;

	// CNTFRQ_EL0 is authoritative wherever firmware programmed it, and every
	// platform this has run on does. It is not guaranteed to, though, and the
	// value is a *divisor* below -- so take what firmware described as the
	// fallback, and refuse to continue with neither rather than dividing by
	// zero inside timer init, before the kernel can say why it stopped.
	const char* frequencySource = "CNTFRQ_EL0";
	sTimerFrequency = READ_SPECIALREG(CNTFRQ_EL0);
	if (sTimerFrequency == 0) {
		sTimerFrequency = timerInfo.frequency;
		frequencySource = "firmware";
	}
	if (sTimerFrequency == 0) {
		panic("arch_timer: no counter frequency: CNTFRQ_EL0 reads 0 and "
			"firmware described none");
	}

	// Derive system_time()'s tick->microsecond multiply/shift factors now, while
	// boot is still single-threaded, so its hot path never has to. (It also has
	// a lazy fallback for any system_time() call that beats us here.)
	__arch_init_system_time();

	// The TVAL registers are signed 32 bit down counters, so no more than
	// INT32_MAX ticks can be programmed at a time.
	sTimerMaxInterval = ((uint64)INT32_MAX * 1000000) / sTimerFrequency;

	// A kernel left at EL2 by the boot loader (VHE host on bare metal) must use
	// the EL2 physical timer; at EL1 (KVM guest) it uses the EL1 physical timer.
	// Which of the timer's views is ours therefore also decides which of the
	// firmware-described interrupts is ours.
	sUseEL2Timer = (READ_SPECIALREG(CurrentEL) >> 2) >= 2;
	uint32 timerIrqIndex = sUseEL2Timer
		? ARM_TIMER_IRQ_HYP_PHYS : ARM_TIMER_IRQ_PHYS;

	bool irqFromFirmware
		= (timerInfo.interrupt_valid & (1 << timerIrqIndex)) != 0;
	int timerIrq = irqFromFirmware
		? (int)timerInfo.interrupt[timerIrqIndex]
		: (sUseEL2Timer ? TIMER_IRQ_EL2_PHYS : TIMER_IRQ_EL1_PHYS);

	dprintf("arch_timer: generic timer at %" B_PRIu64 " Hz (%s), max interval %"
		B_PRIdBIGTIME " us, %s timer on INTID %d (%s)\n", sTimerFrequency,
		frequencySource, sTimerMaxInterval,
		sUseEL2Timer ? "EL2 physical" : "EL1 physical", timerIrq,
		irqFromFirmware ? "firmware" : "architected default");

	// system_time() is implemented by reading CNTVCT_EL0 and CNTFRQ_EL0 from
	// EL0, which is only allowed while this is set. The boot loader sets it as
	// well, but only on the path where it does not have to drop from EL2 to
	// EL1 first. Note this is the counter granted to EL0 for time-of-day; it is
	// independent of which timer (physical, above) drives scheduler interrupts.
	//
	// Grant the virtual counter only, as Linux does. The physical counter is
	// not rebased by a hypervisor, so under KVM it reads the host's counter,
	// which has been running since the host powered on and bears no relation to
	// this machine's uptime. Userland that reads it therefore gets a plausible
	// but wrong time silently; a build chroot carrying a stale libroot.so that
	// read CNTPCT_EL0 ran 18 hours behind for a day before anyone noticed. With
	// EL0PCTEN clear it faults instead.
	WRITE_SPECIALREG(CNTKCTL_EL1, CNTKCTL_EL0VCTEN);

	if (sUseEL2Timer)
		WRITE_SPECIALREG(CNTHP_CTL_EL2, TIMER_DISABLED);
	else
		WRITE_SPECIALREG(CNTP_CTL_EL0, TIMER_DISABLED);

	// The return value only became worth checking once the INTID stopped being
	// a compile-time constant: an out-of-range one is refused here, and a
	// kernel that silently has no timer handler is tickless and wedges at the
	// first thing that blocks, a long way from the cause.
	status_t status = install_io_interrupt_handler(timerIrq,
		&arch_timer_interrupt, NULL, 0);
	if (status != B_OK && irqFromFirmware) {
		int fallback = sUseEL2Timer ? TIMER_IRQ_EL2_PHYS : TIMER_IRQ_EL1_PHYS;
		dprintf("arch_timer: firmware INTID %d unusable (%s); falling back to "
			"the architected INTID %d\n", timerIrq, strerror(status), fallback);
		timerIrq = fallback;
		status = install_io_interrupt_handler(timerIrq, &arch_timer_interrupt,
			NULL, 0);
	}
	if (status != B_OK) {
		panic("arch_timer: could not install the timer handler on INTID %d: %s",
			timerIrq, strerror(status));
	}

	return B_OK;
}
