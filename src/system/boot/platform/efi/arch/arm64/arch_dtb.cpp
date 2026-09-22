/*
 * Copyright 2019-2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *   Alexander von Gluck IV <kallisti5@unixzen.com>
 */

#include <arch_cpu_defs.h>
#include <arch_dtb.h>
#include <arch_smp.h>
#include <boot/platform.h>
#include <boot/stage2.h>

extern "C" {
#include <libfdt.h>
}

#include "dtb.h"


void arm64_handle_fdt_psci_node(const void *fdt, int node);
void arm64_handle_fdt_cpu_node(const void *fdt, int node);


/* TODO: Code taken from ARM port just for building purposes */

/* The potential interrupt controoller would be present in the dts as:
 * compatible = "arm,gic-v3";
 */
const struct supported_interrupt_controllers {
	const char*	dtb_compat;
	const char*	kind;
} kSupportedInterruptControllers[] = {
	{ "arm,cortex-a9-gic", INTC_KIND_GICV1 },
	{ "arm,cortex-a15-gic", INTC_KIND_GICV2 },
	{ "arm,gic-400", INTC_KIND_GICV2 },
	{ "arm,gic-v3", INTC_KIND_GICV3 },
	{ "ti,omap3-intc", INTC_KIND_OMAP3 },
	{ "marvell,pxa-intc", INTC_KIND_PXA },
};


// The Arm generic timer. Both bindings describe the same timer; an armv8 tree
// commonly claims both, and the armv7 binding is what a 64-bit SoC with a
// 32-bit-era device tree will say.
static const char* const kGenericTimerCompatible[] = {
	"arm,armv8-timer",
	"arm,armv7-timer",
};

// Optional "interrupt-names" values, in the positional order the binding
// defines for a node that does not name them. Indexed by ARM_TIMER_IRQ_*.
static const char* const kGenericTimerIrqNames[ARM_TIMER_IRQ_COUNT] = {
	"sec-phys",
	"phys",
	"virt",
	"hyp-phys",
	"hyp-virt",
};


// Position of \a pattern in a NUL-separated string-list property, or -1.
//
// Bounded by the property's length for the same reason dtb_has_fdt_string() is:
// the list is input from firmware and nothing guarantees its last entry carries
// a terminator, so a run with no NUL inside the property is malformed and ends
// the walk rather than continuing into the rest of the blob.
static int
arm64_fdt_string_index(const char* prop, int size, const char* pattern)
{
	if (prop == NULL || size <= 0)
		return -1;

	size_t patternLen = strlen(pattern);
	const char* propEnd = prop + size;
	int index = 0;
	while (prop < propEnd) {
		const char* end = (const char*)memchr(prop, '\0', propEnd - prop);
		if (end == NULL)
			return -1;
		if ((size_t)(end - prop) == patternLen
			&& memcmp(prop, pattern, patternLen) == 0) {
			return index;
		}
		prop = end + 1;
		index++;
	}

	return -1;
}


// Record what the device tree says about the generic timer.
//
// The timer itself is reached through system registers, so there is nothing to
// map: what firmware alone can tell us is which interrupt each view is
// delivered on, and how fast the counter runs. Both are platform facts that the
// kernel has been assuming architected values for.
static void
arm64_handle_fdt_timer_node(const void* fdt, int node)
{
	arm_generic_timer_info &timer = gKernelArgs.arch_args.timer;

	// A node that names its lines is the node most likely to have reordered
	// them, so "interrupt-names" wins where it is present; otherwise the
	// binding's positional order applies, which is also what Linux's own
	// arch_timer driver reads. A tree that lists fewer entries than that order
	// has names for is read as the first N of it -- there is no other
	// interpretation available without names, and inventing one would put a
	// plausible but wrong INTID in front of the kernel.
	int namesLen = 0;
	const char* names = (const char*)fdt_getprop(fdt, node, "interrupt-names",
		&namesLen);

	for (uint32 i = 0; i < ARM_TIMER_IRQ_COUNT; i++) {
		int index = (int)i;
		if (names != NULL) {
			index = arm64_fdt_string_index(names, namesLen,
				kGenericTimerIrqNames[i]);
			if (index < 0)
				continue;
		}

		uint32 interrupt = 0;
		if (!dtb_get_interrupt_at(fdt, node, (uint32)index, interrupt))
			continue;

		timer.interrupt[i] = interrupt;
		timer.interrupt_valid |= 1 << i;
	}

	// "clock-frequency" is the counter frequency, and exists in the binding for
	// exactly the platforms whose firmware does not program CNTFRQ_EL0. Carry
	// it; the kernel still prefers the register where that is non-zero.
	int frequencyLen = 0;
	const uint32* frequency = (const uint32*)fdt_getprop(fdt, node,
		"clock-frequency", &frequencyLen);
	if (frequency != NULL && frequencyLen == (int)sizeof(uint32))
		timer.frequency = fdt32_to_cpu(*frequency);
}


void
arch_handle_fdt(const void* fdt, int node)
{
	// The device tree is input from firmware, so a string property is only a
	// string once we have seen its terminator inside the property's own length:
	// strcmp() reads until a NUL, and would otherwise run off the end of the
	// property into the rest of the blob.
	int deviceTypeLen;
	const char* deviceType = (const char*)fdt_getprop(fdt, node,
		"device_type", &deviceTypeLen);

	if (deviceType != NULL && deviceTypeLen > 0
		&& deviceType[deviceTypeLen - 1] == '\0') {
		if (strcmp(deviceType, "cpu") == 0) {
			arm64_handle_fdt_cpu_node(fdt, node);
		}
	}

	int compatibleLen;
	const char* compatible = (const char*)fdt_getprop(fdt, node,
		"compatible", &compatibleLen);

	if (compatible == NULL)
		return;

	// A "compatible" whose last byte is not a NUL is malformed, and used to be
	// refused outright here because the string-list walk it feeds was unbounded
	// and would have read past the value. That walk is now bounded by the
	// property length and rejects only the unterminated run (#432), so say so
	// and keep going: the entries before it are well-formed, and discarding a
	// correctly declared "arm,gic-v3" because something after it was truncated
	// costs the machine its interrupt controller for no safety gained.
	if (compatibleLen <= 0 || compatible[compatibleLen - 1] != '\0') {
		dprintf("fdt: node %d has a malformed compatible property; only its "
			"terminated entries will be matched\n", node);
	}

	intc_info &interrupt_controller = gKernelArgs.arch_args.interrupt_controller;
	if (interrupt_controller.kind[0] == 0) {
		for (uint32 i = 0; i < B_COUNT_OF(kSupportedInterruptControllers); i++) {
			if (dtb_has_fdt_string(compatible, compatibleLen,
				kSupportedInterruptControllers[i].dtb_compat)) {

				memcpy(interrupt_controller.kind, kSupportedInterruptControllers[i].kind,
					sizeof(interrupt_controller.kind));

				dtb_get_reg(fdt, node, 0, interrupt_controller.regs1);
				dtb_get_reg(fdt, node, 1, interrupt_controller.regs2);
			}
		}
	}

	// "compatible" is an ordered list of NUL-terminated strings, and a device
	// tree is free to put the most specific binding first. Comparing only the
	// first entry made PSCI discovery depend on that order, so a tree listing
	// e.g. "arm,psci-0.2" ahead of "arm,psci-1.0" left us with no conduit at
	// all. Walk the whole list instead, and accept every version that defines
	// the standard 0.2+ function IDs we issue (CPU_ON, SYSTEM_OFF/RESET);
	// "arm,psci" alone is 0.1, whose function IDs come from the node's own
	// properties, so it is deliberately not claimed here.
	if (dtb_has_fdt_string(compatible, compatibleLen, "arm,psci-1.0")
		|| dtb_has_fdt_string(compatible, compatibleLen, "arm,psci-0.2")) {
		arm64_handle_fdt_psci_node(fdt, node);
	}

	// Only the first timer node is taken: there is one generic timer per
	// machine, and a second node claiming to be it is a tree we do not
	// understand rather than a second timer.
	if (gKernelArgs.arch_args.timer.interrupt_valid == 0
		&& gKernelArgs.arch_args.timer.frequency == 0) {
		for (uint32 i = 0; i < B_COUNT_OF(kGenericTimerCompatible); i++) {
			if (dtb_has_fdt_string(compatible, compatibleLen,
					kGenericTimerCompatible[i])) {
				arm64_handle_fdt_timer_node(fdt, node);
				break;
			}
		}
	}
}


void
arch_dtb_set_kernel_args(void)
{
	intc_info &interrupt_controller = gKernelArgs.arch_args.interrupt_controller;
	dprintf("Chosen interrupt controller:\n");
	if (interrupt_controller.kind[0] == 0) {
		dprintf("kind: None!\n");
	} else {
		dprintf("  kind: %s\n", interrupt_controller.kind);
		dprintf("  regs: %#" B_PRIx64 ", %#" B_PRIx64 "\n",
			interrupt_controller.regs1.start,
			interrupt_controller.regs1.size);
		dprintf("        %#" B_PRIx64 ", %#" B_PRIx64 "\n",
			interrupt_controller.regs2.start,
			interrupt_controller.regs2.size);
	}

	// Only report the timer when the device tree actually described one: on the
	// ACPI path this is still all zeroes, and a line of zeroes would read as a
	// parse failure rather than as "not this boot path".
	arm_generic_timer_info &timer = gKernelArgs.arch_args.timer;
	if (timer.interrupt_valid != 0 || timer.frequency != 0) {
		dprintf("Generic timer from fdt:\n");
		dprintf("  frequency: %" B_PRIu64 " Hz%s\n", timer.frequency,
			timer.frequency == 0 ? " (not stated; kernel uses CNTFRQ_EL0)" : "");
		for (uint32 i = 0; i < ARM_TIMER_IRQ_COUNT; i++) {
			if ((timer.interrupt_valid & (1 << i)) != 0) {
				dprintf("  %s: INTID %" B_PRIu32 "\n",
					kGenericTimerIrqNames[i], timer.interrupt[i]);
			}
		}
	}
}
