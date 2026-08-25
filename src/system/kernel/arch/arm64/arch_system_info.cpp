/*
 * Copyright 2019-2026 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <OS.h>

#include <arch_cpu.h>
#include <arch/arm64/arch_pmu.h>
#include <arch/system_info.h>
#include <boot/kernel_args.h>
#include <debug.h>


static uint64 sMidr;
	// MIDR_EL1 as read on the boot CPU. Under a hypervisor this is really
	// VPIDR_EL2, i.e. whatever the platform chooses to advertise, but it is an
	// ordinary ID register read that is never trapped to EL2, so unlike the PMU
	// it is safe to read on any machine.

static uint64 sCoreFrequency;
	// Measured core clock in Hz, or 0 if it could not be measured.


struct cpu_part_name {
	uint16		part;
	const char*	name;
};

// Only for the boot log. The name that userland shows comes from the same part
// numbers decoded in headers/private/shared/cpu_type.h, because the topology
// API carries the numeric model and no string; keep the two in step. The boot
// log is the one that matters on a headless machine, where it is the only way
// to see what the kernel thinks it is running on.
static const cpu_part_name kArmPartNames[] = {
	{ CPU_PART_FOUNDATION,		"Foundation" },
	{ CPU_PART_CORTEX_A35,		"Cortex-A35" },
	{ CPU_PART_CORTEX_A53,		"Cortex-A53" },
	{ CPU_PART_CORTEX_A55,		"Cortex-A55" },
	{ CPU_PART_CORTEX_A57,		"Cortex-A57" },
	{ CPU_PART_CORTEX_A72,		"Cortex-A72" },
	{ CPU_PART_CORTEX_A73,		"Cortex-A73" },
	{ CPU_PART_CORTEX_A75,		"Cortex-A75" },
	{ CPU_PART_CORTEX_A76,		"Cortex-A76" },
	{ CPU_PART_CORTEX_A78,		"Cortex-A78" },
	{ CPU_PART_NEOVERSE_N1,		"Neoverse-N1" },
	{ CPU_PART_NEOVERSE_E1,		"Neoverse-E1" },
	{ CPU_PART_NEOVERSE_V1,		"Neoverse-V1" },
	{ CPU_PART_NEOVERSE_N2,		"Neoverse-N2" },
	{ CPU_PART_NEOVERSE_V2,		"Neoverse-V2" },
	{ CPU_PART_NEOVERSE_V3,		"Neoverse-V3" },
	{ CPU_PART_NEOVERSE_N3,		"Neoverse-N3" },
};


static const char*
arm_part_name(uint16 part)
{
	for (size_t i = 0; i < B_COUNT_OF(kArmPartNames); i++) {
		if (kArmPartNames[i].part == part)
			return kArmPartNames[i].name;
	}

	return NULL;
}


static const char*
implementer_name(uint8 implementer)
{
	switch (implementer) {
		case CPU_IMPL_ARM:			return "ARM";
		case CPU_IMPL_BROADCOM:		return "Broadcom";
		case CPU_IMPL_CAVIUM:		return "Cavium";
		case CPU_IMPL_DEC:			return "DEC";
		case CPU_IMPL_FUJITSU:		return "Fujitsu";
		case CPU_IMPL_INFINEON:		return "Infineon";
		case CPU_IMPL_FREESCALE:	return "Freescale";
		case CPU_IMPL_NVIDIA:		return "NVIDIA";
		case CPU_IMPL_APM:			return "APM";
		case CPU_IMPL_QUALCOMM:		return "Qualcomm";
		case CPU_IMPL_MARVELL:		return "Marvell";
		case CPU_IMPL_APPLE:		return "Apple";
		case CPU_IMPL_MICROSOFT:	return "Microsoft";
		case CPU_IMPL_INTEL:		return "Intel";
		case CPU_IMPL_AMPERE:		return "Ampere";
		default:					return NULL;
	}
}


/*!	Maps the MIDR_EL1 implementer field onto the cpu_vendor enum, which predates
	arm64 and names silicon vendors rather than architecture licensees. Most
	arm64 implementers have no entry, and B_CPU_VENDOR_UNKNOWN is the honest
	answer for those -- inventing an enum value userland does not know how to
	name would only move the wrong answer somewhere harder to find.
*/
static enum cpu_vendor
vendor_for_implementer(uint8 implementer)
{
	switch (implementer) {
		case CPU_IMPL_ARM:
			return B_CPU_VENDOR_ARM;
		case CPU_IMPL_FUJITSU:
			return B_CPU_VENDOR_FUJITSU;
		case CPU_IMPL_INTEL:
			return B_CPU_VENDOR_INTEL;
		default:
			return B_CPU_VENDOR_UNKNOWN;
	}
}


/*!	Measures the core clock once and remembers it. Returns whether a frequency
	is known, so that every caller has to deal with the case where it is not.

	Kept out of arch_system_info_init() rather than done unconditionally at
	boot for two reasons: the measurement needs the PMU, which is opt-in
	because EL2 may trap it, and it costs milliseconds. Retrying on each call
	while unknown is what makes the "pmu on" KDL command take effect without a
	reboot.
*/
static bool
ensure_core_frequency(void)
{
	if (sCoreFrequency != 0)
		return true;

	uint64 frequency = 0;
	if (arm64_pmu_measure_core_frequency(&frequency) != B_OK)
		return false;

	sCoreFrequency = frequency;
	return true;
}


void
arch_fill_topology_node(cpu_topology_node_info* node, int32 cpu)
{
	switch (node->type) {
		case B_TOPOLOGY_ROOT:
			node->data.root.platform = B_CPU_ARM_64;
			break;
		case B_TOPOLOGY_PACKAGE:
			node->data.package.vendor
				= vendor_for_implementer((uint8)CPU_IMPL(sMidr));
			node->data.package.cache_line_size = CACHE_LINE_SIZE;
			break;
		case B_TOPOLOGY_CORE:
			// The whole MIDR, not just the part number: it is the
			// architectural identifier of the core, it is what every other
			// arm64 tool reports, and userland needs the variant and revision
			// out of it as well. All CPUs in a package are the same core here
			// -- this port has no big.LITTLE support, and would need per-CPU
			// MIDR sampling before it could describe one.
			node->data.core.model = (uint32)sMidr;

			// No status to return through a struct field, so an unknown clock
			// has to be 0 here. arch_get_frequency() is the interface that can
			// say so, and does.
			node->data.core.default_frequency
				= ensure_core_frequency() ? sCoreFrequency : 0;
			break;
		default:
			break;
	}
}


status_t
arch_system_info_init(struct kernel_args *args)
{
	sMidr = READ_SPECIALREG(MIDR_EL1);

	uint8 implementer = (uint8)CPU_IMPL(sMidr);
	uint16 part = (uint16)CPU_PART(sMidr);
	const char* implementerName = implementer_name(implementer);
	const char* partName = (implementer == CPU_IMPL_ARM)
		? arm_part_name(part) : NULL;

	// Report the decode and the raw value both ways round. A core this kernel
	// has never heard of shows up as an unnamed part number rather than as
	// nothing at all, and the raw MIDR is what any comparison against another
	// operating system on the same machine will be made against.
	dprintf("arm64 CPU: MIDR_EL1 %#010" B_PRIx32 " -- %s %s r%up%u\n",
		(uint32)sMidr,
		implementerName != NULL ? implementerName : "unknown implementer",
		partName != NULL ? partName : "unknown part",
		(unsigned int)CPU_VAR(sMidr), (unsigned int)CPU_REV(sMidr));

	if (implementerName == NULL || partName == NULL) {
		dprintf("arm64 CPU: implementer %#x part %#x is not in this kernel's "
			"tables\n", (unsigned int)implementer, (unsigned int)part);
	}

	if (ensure_core_frequency()) {
		dprintf("arm64 CPU: core clock %" B_PRIu64 " Hz (%" B_PRIu64 " MHz), "
			"measured against the generic timer\n", sCoreFrequency,
			sCoreFrequency / 1000000);
	} else {
		// Say why, because "unknown" here is a consequence of a deliberate
		// choice elsewhere and not a defect to go hunting for.
		dprintf("arch_get_frequency: core clock unknown -- ARM has no register "
			"that states it, and measuring it needs the PMU, which is off by "
			"default (use the \"arm64_pmu\" boot setting or \"pmu on\" in "
			"KDL)\n");
	}

	return B_OK;
}


status_t
arch_get_frequency(uint64 *frequency, int32 cpu)
{
	// Reporting B_OK with 0 Hz here is what this used to do, and it made every
	// caller's check useless: sysinfo printed "running at 0MHz" as though that
	// were a measurement. An unknown frequency is an error.
	if (!ensure_core_frequency())
		return B_NOT_SUPPORTED;

	// One measurement covers every CPU. The cores of an arm64 package this
	// port supports are identical and no dynamic frequency scaling is exposed
	// to the OS, so there is nothing per-CPU to report yet; \a cpu is accepted
	// for the sake of the interface. Sampling per CPU would mean an
	// inter-processor call that spins with interrupts off for milliseconds on
	// the target core, which is not worth it for a value that does not differ.
	(void)cpu;

	*frequency = sCoreFrequency;
	return B_OK;
}
