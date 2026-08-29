/*
 * Copyright 2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_BOOT_INTERRUPT_CONTROLLER_H
#define KERNEL_BOOT_INTERRUPT_CONTROLLER_H


#include <boot/addr_range.h>
#include <SupportDefs.h>


#define		INTC_KIND_GICV1		"gicv1"
#define		INTC_KIND_GICV2		"gicv2"
#define		INTC_KIND_GICV3		"gicv3"
#define		INTC_KIND_OMAP3		"omap3"
#define		INTC_KIND_PXA		"pxa"
#define		INTC_KIND_SUN4I		"sun4i"


// GICv3/v4 redistributors are not necessarily one contiguous run of frames.
// ACPI describes them either with a single MADT GICR structure, or with a
// per-CPU base address in each GICC entry -- and the per-CPU form exists
// precisely for the case where the frames are *not* contiguous, which real
// hardware does use. Adjacent frames are coalesced into these regions.
#define		INTC_MAX_GICR_REGIONS	16


// A region carries the distance between its redistributors as well as its
// extent. Whoever worked out where the region ends is the only party that can
// say how to step through it, and having the two derived independently at
// either end of the boot handoff is how a region ends up yielding no
// redistributors at all.
typedef struct {
	uint64 start;
	uint64 size;
	uint64 stride;
} __attribute__((packed)) gicr_region_info;


typedef struct {
	char kind[32];
	addr_range regs1;
	addr_range regs2;
	// GICv3 only: the ITS, which translates MSIs into LPIs. Zero when absent.
	addr_range regs3;

	// GICv3 only. When zero, regs2 describes the one and only redistributor
	// region; otherwise these do, and regs2 is just the lowest of them.
	uint32 gicr_region_count;
	gicr_region_info gicr_regions[INTC_MAX_GICR_REGIONS];

	// How many PEs the firmware described, including any the kernel has no room
	// to run. An exact upper bound on the number of redistributors that can
	// exist, which is the only safe way to bound a walk across a region whose
	// declared size is a generous window rather than an array. Zero if unknown.
	uint32 pe_count;

	// The PMU counter-overflow interrupt number, taken from the MADT GICC
	// "Performance Interrupt GSIV" field (ACPI 6.x, GIC CPU Interface
	// structure). This is the authoritative INTID for the PMUv3 overflow
	// interrupt, and it is not derivable from any PMU register -- the
	// architected device-tree PPI 7 (INTID 23) is only a recommendation, and
	// firmware is free to place it elsewhere. Zero when the firmware did not
	// state it (or the GICCs disagreed), in which case PMU sampling stays off.
	uint32 pmu_gsiv;
} __attribute__((packed)) intc_info;


#endif /* KERNEL_BOOT_INTERRUPT_CONTROLLER_H */
