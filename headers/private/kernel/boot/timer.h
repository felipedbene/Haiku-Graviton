/*
 * Copyright 2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_BOOT_TIMER_H
#define KERNEL_BOOT_TIMER_H


#include <boot/addr_range.h>
#include <SupportDefs.h>


#define		TIMER_KIND_ARMV7	"armv7"
#define		TIMER_KIND_OMAP3	"omap3"
#define		TIMER_KIND_PXA		"pxa"


typedef struct {
	char		kind[32];
	addr_range	regs;
	uint32_t	interrupt;
} __attribute__((packed)) boot_timer_info;


// The Arm generic timer (ARM DDI 0487, "Generic Timer"). It has no registers to
// map -- it is reached through system registers -- so all the loader can tell
// the kernel about it is which interrupt each of its views is delivered on and
// how fast its counter runs. Both are properties of the *platform*, not of the
// architecture: the INTIDs below are only recommendations of the Arm Base System
// Architecture, which a SoC is free to ignore (a Raspberry Pi 3 routes them
// through a SoC-local controller, numbered from 0), and CNTFRQ_EL0 is programmed
// by firmware and is not guaranteed to be programmed at all.
//
// The five views, in the order the device-tree binding lists them:
#define		ARM_TIMER_IRQ_SEC_PHYS	0	// secure EL1 physical, CNTPS_*_EL1
#define		ARM_TIMER_IRQ_PHYS		1	// non-secure EL1 physical, CNTP_*_EL0
#define		ARM_TIMER_IRQ_VIRT		2	// EL1 virtual, CNTV_*_EL0
#define		ARM_TIMER_IRQ_HYP_PHYS	3	// EL2 physical, CNTHP_*_EL2
#define		ARM_TIMER_IRQ_HYP_VIRT	4	// EL2 virtual, CNTHV_*_EL2
#define		ARM_TIMER_IRQ_COUNT		5

typedef struct {
	// Counter frequency in Hz, or 0 when firmware did not state one. Only a
	// fallback for CNTFRQ_EL0: the register is authoritative where it is
	// programmed, and a device tree's "clock-frequency" exists precisely for
	// the platforms where it is not.
	uint64		frequency;

	// Interrupt numbers, indexed by ARM_TIMER_IRQ_*, valid only where the
	// matching bit of interrupt_valid is set. A bitmask rather than a zero
	// sentinel because 0 is a legal answer: on a GIC these are PPIs and 0 would
	// be nonsense, but on a SoC-local interrupt controller with its own
	// numbering space the first timer really can be interrupt 0.
	uint32		interrupt[ARM_TIMER_IRQ_COUNT];
	uint32		interrupt_valid;
} __attribute__((packed)) arm_generic_timer_info;


#endif /* KERNEL_BOOT_TIMER_H */
