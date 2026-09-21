/*
 * Copyright 2021-2022, Haiku, Inc. All rights reserved.
 * Released under the terms of the MIT License.
*/


#include "arch_smp.h"

#include <stddef.h>
#include <string.h>

#include <KernelExport.h>

#include <kernel.h>
#include <safemode.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/menu.h>

#include "mmu.h"
#include "aarch64.h"

extern "C" {
#include <libfdt.h>
}


//#define TRACE_SMP
#ifdef TRACE_SMP
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


#define PSCI_CPU_ON 0xc4000003UL


extern "C" void arch_enter_kernel(struct kernel_args* kernelArgs,
	addr_t kernelEntry, addr_t kernelStackTop, uint32 cpu);
void arm64_common_cpu_startup();

static void arm64_psci_call_smc(uint64 func, uint64 arg0, uint64 arg1, uint64 arg2);
static void arm64_psci_call_hvc(uint64 func, uint64 arg0, uint64 arg1, uint64 arg2);


static platform_cpu_info sCpus[SMP_MAX_CPUS];
static uint32 sCpuCount = 0;

static struct kernel_args* sKernelArgs;
static uint64 sKernelEntry;
static uint64 sSecondaryStacks[SMP_MAX_CPUS];
static void (*sPsciCallFn)(uint64, uint64, uint64, uint64);

enum class CpuEnableMethod {
	Unknown,
	Psci,
	SpinTable
};


static CpuEnableMethod sCpuEnableMethod = CpuEnableMethod::Unknown;

// Cleared when a cpu node cannot be parsed: a topology we only half understand
// is not one we are willing to start secondary CPUs from.
static bool sCpuTopologyValid = true;


void
arch_smp_register_cpu(platform_cpu_info** cpu)
{
	uint32 newCount = sCpuCount + 1;
	if (newCount > SMP_MAX_CPUS) {
		*cpu = NULL;
		return;
	}
	*cpu = &sCpus[sCpuCount];
	sCpuCount = newCount;
}


// Called from the ACPI path, where the PSCI conduit comes from the FADT's
// ARM_BOOT_ARCH flags rather than from a device tree node.
void
arch_smp_set_psci_conduit(bool useHvc)
{
	sPsciCallFn = useHvc ? arm64_psci_call_hvc : arm64_psci_call_smc;
	sCpuEnableMethod = CpuEnableMethod::Psci;
	gKernelArgs.arch_args.psci_conduit
		= useHvc ? PSCI_CONDUIT_HVC : PSCI_CONDUIT_SMC;
}


int
arch_smp_get_current_cpu(void)
{
	return 0;
}


void
arch_smp_init_other_cpus(void)
{
	if (sCpuEnableMethod == CpuEnableMethod::Unknown)
		sCpuCount = 1;

	// The cpu nodes can name the PSCI enable method while the /psci node is
	// absent, unrecognised, or carries a conduit we cannot issue. Decide that
	// here, once, rather than discovering it as an indirect call through NULL
	// in arch_smp_boot_other_cpus().
	if (sCpuEnableMethod == CpuEnableMethod::Psci && sPsciCallFn == NULL) {
		dprintf("smp: cpus ask for the PSCI enable method but no usable PSCI "
			"conduit was found, booting single-CPU\n");
		sCpuCount = 1;
	}

	if (!sCpuTopologyValid) {
		dprintf("smp: incomplete cpu topology in the device tree, booting "
			"single-CPU\n");
		sCpuCount = 1;
	}

	gKernelArgs.num_cpus = sCpuCount;

	if (get_safemode_boolean(B_SAFEMODE_DISABLE_SMP, false)) {
		// SMP has been disabled!
		TRACE("smp disabled per safemode setting\n");
		gKernelArgs.num_cpus = 1;
	}

	if (gKernelArgs.num_cpus < 2)
		return;

	for (uint32 i = 1; i < gKernelArgs.num_cpus; i++) {
		// create a final stack the trampoline code will put the ap processor on
		void * stack = NULL;
		const size_t size = KERNEL_STACK_SIZE + KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE;
		if (platform_allocate_region(&stack, size, 0) != B_OK) {
			panic("Unable to allocate AP stack");
		}
		memset(stack, 0, size);
		gKernelArgs.cpu_kstack[i].start = fix_address((uint64_t)stack);
		gKernelArgs.cpu_kstack[i].size = size;
		sSecondaryStacks[i] = (uint64)stack + size;
	}

	return;
}


void
arm64_secondary_startup()
{
	// We may or may not have a stack at this point, and
	// the bootstrap processor couldn't pass us any information
	// so we need to configure ourselves using static variables
	// without using a stack
	asm(
	//       Put our MPIDR in x0 and clear bits 31 and 24
	//       (which aren't used for identifying the CPU)
		"    mrs x0, MPIDR_EL1\n"
		"    mov x1, #((1 << 31) | (1 << 24))\n"
		"    bic x0, x0, x1\n"
		"    mov x1, %0\n"
		"    mov x2, #%2\n"
		"    mov x4, #%3\n"
	//       Search through sCpus to find the entry corresponding to
	//       our MPIDR. The scan is bounded by the size of the array: an MPIDR
	//       the boot loader never registered used to walk off the end of it
	//       and fault somewhere unrelated, with no stack to report from.
		"0:  cbz x4, 2f\n"
		"    ldr x3, [x1, #%4]\n"
		"    cmp x0, x3\n"
		"    beq 1f\n"
		"    add x1, x1, x2\n"
		"    sub x4, x4, #1\n"
		"    b 0b\n"
		"1:  ldr w0, [x1]\n"
	//       Use the id in our sCpus entry to get our stack pointer
	//       from sSecondaryStacks
		"	 lsl x1, x0, #3\n"
		"    mov x2, %1\n"
		"    add x1, x2, x1\n"
		"    ldr x1, [x1]\n"
		"    mov sp, x1\n"
	//       Enable the FPU, jump to arm64_secondary_startup2
		"    mov x1, #0x300000\n"
		"    msr CPACR_EL1, x1\n"
		"    b arm64_secondary_startup2\n"
	//       We are a CPU the boot loader does not know about: there is no
	//       stack to run on and no way to report it, so park rather than
	//       reading past the end of sCpus.
		"2:  wfi\n"
		"    b 2b"
		:
		: "r" (sCpus), "r" (sSecondaryStacks),
			"i" (sizeof(platform_cpu_info)), "i" (SMP_MAX_CPUS),
			"i" (offsetof(platform_cpu_info, mpidr))
		: "x0", "x1", "x2", "x3", "x4"
	);
}


extern "C" void
arm64_secondary_startup2(uint32 cpu)
{
	arm64_common_cpu_startup();

	arch_enter_kernel(sKernelArgs, sKernelEntry,
		gKernelArgs.cpu_kstack[cpu].start + gKernelArgs.cpu_kstack[cpu].size,
		cpu);
}


void
arch_smp_boot_other_cpus(addr_t ttbr1, uint64 kernelEntry, addr_t virtKernelArgs)
{
	sKernelEntry = kernelEntry;
	sKernelArgs = (struct kernel_args*)virtKernelArgs;

	// Only the CPUs we counted into the kernel args have a stack: safemode's
	// "Disable SMP" leaves num_cpus at one while sCpuCount still reflects the
	// firmware description, and a CPU started beyond that would load sp from a
	// never-filled sSecondaryStacks entry.
	for (uint32 i = 0; i < gKernelArgs.num_cpus; i++) {
		platform_cpu_info* cpu = &sCpus[i];

		if (cpu->id == 0)
			continue;

		switch (sCpuEnableMethod) {
		case CpuEnableMethod::Psci:
			// arch_smp_init_other_cpus() already refuses to claim more than one
			// CPU without a conduit; this keeps the indirect call itself honest
			// for any future caller that reaches here by another route.
			if (sPsciCallFn == NULL) {
				dprintf("smp: no PSCI conduit, cannot start cpu %" B_PRIu32
					"\n", cpu->id);
				continue;
			}
			sPsciCallFn(PSCI_CPU_ON, cpu->mpidr, (uint64)arm64_secondary_startup, 0);
			break;
		case CpuEnableMethod::SpinTable:
			// The enable method is a single global while the release address is
			// per-CPU, so a tree that names spin-table on one cpu node and
			// something else on another leaves us here with no address for the
			// latter. Writing the entry point to address 0 would take the boot
			// loader down instead of reporting that.
			if (cpu->releaseAddr == 0) {
				dprintf("smp: no cpu-release-addr for cpu %" B_PRIu32
					", cannot start it\n", cpu->id);
				continue;
			}
			*((uint64*)cpu->releaseAddr) = (uint64)arm64_secondary_startup;
			asm("sev");
			break;
		default:
			// Unreachable: without a known enable method
			// arch_smp_init_other_cpus() clamps num_cpus to one, so this loop
			// only ever sees the boot CPU, which is skipped above.
			break;
		}
	}
}


void
arch_smp_add_safemode_menus(Menu *menu)
{
	MenuItem *item;

	if (gKernelArgs.num_cpus < 2)
		return;

	item = new(nothrow) MenuItem("Disable SMP");
	menu->AddItem(item);
	item->SetData(B_SAFEMODE_DISABLE_SMP);
	item->SetType(MENU_ITEM_MARKABLE);
	item->SetHelpText("Disables all but one CPU core.");
}


void
arch_smp_init(void)
{
}


// Every fdt_getprop() below can return NULL — a device tree is under no
// obligation to carry a property we happen to read — and a property that is
// present can still be shorter than the value we want out of it. A malformed
// or merely unexpected tree therefore has to degrade to a single-CPU boot with
// a diagnostic; dereferencing the result faults before anything can report it.
void
arm64_handle_fdt_cpu_node(const void *fdt, int node)
{
	int parent = fdt_parent_offset(fdt, node);
	if (parent < 0) {
		dprintf("fdt: cpu node %d has no parent, ignoring it\n", node);
		sCpuTopologyValid = false;
		return;
	}

	// Per the device tree specification #address-cells defaults to 2 when the
	// parent does not state it.
	uint32 addressCells = 2;
	int length;
	const void* prop = fdt_getprop(fdt, parent, "#address-cells", &length);
	if (prop != NULL && length >= (int)sizeof(uint32))
		addressCells = fdt32_to_cpu(*(const uint32*)prop);

	prop = fdt_getprop(fdt, node, "reg", &length);
	if (prop == NULL) {
		dprintf("fdt: cpu node %d has no reg property, ignoring it\n", node);
		sCpuTopologyValid = false;
		return;
	}

	// Follow what the parent said where the property is long enough for it,
	// and fall back on what fits otherwise: a tree that omits #address-cells
	// but carries a single-cell MPIDR still describes its CPUs unambiguously,
	// and reading eight bytes out of a four-byte property never would.
	uint64 mpidr;
	if (addressCells != 1 && length >= (int)sizeof(uint64))
		mpidr = fdt64_to_cpu(*(const uint64*)prop);
	else if (length >= (int)sizeof(uint32))
		mpidr = fdt32_to_cpu(*(const uint32*)prop);
	else {
		dprintf("fdt: cpu node %d has a truncated reg property (%d bytes), "
			"ignoring it\n", node, length);
		sCpuTopologyValid = false;
		return;
	}

	// enable-method is optional in the cpu node: PSCI is commonly described by
	// the /psci node alone. Only spin-table has to be named here, because only
	// it needs the per-CPU release address that comes with it.
	uint64 releaseAddr = 0;
	CpuEnableMethod enableMethod = CpuEnableMethod::Unknown;
	const char* methodName = (const char*)fdt_getprop(fdt, node,
		"enable-method", &length);
	if (methodName != NULL && length > 0 && methodName[length - 1] == '\0') {
		if (strcmp(methodName, "spin-table") == 0) {
			prop = fdt_getprop(fdt, node, "cpu-release-addr", &length);
			if (prop == NULL || length < (int)sizeof(uint64)) {
				dprintf("fdt: cpu node %d uses spin-table without a usable "
					"cpu-release-addr, ignoring it\n", node);
				sCpuTopologyValid = false;
				return;
			}
			releaseAddr = fdt64_to_cpu(*(const uint64*)prop);
			enableMethod = CpuEnableMethod::SpinTable;
		} else if (strcmp(methodName, "psci") == 0) {
			enableMethod = CpuEnableMethod::Psci;
		} else {
			dprintf("fdt: cpu node %d has unsupported enable-method \"%s\"\n",
				node, methodName);
		}
	}

	// Registering last keeps a CPU we could not fully describe out of the
	// table: a half-filled entry would be handed to PSCI CPU_ON, and it would
	// also be searched by MPIDR from the secondary startup path.
	platform_cpu_info* info = NULL;
	arch_smp_register_cpu(&info);
	if (info == NULL)
		return;
	info->id = sCpuCount - 1;
	info->mpidr = mpidr;
	info->releaseAddr = releaseAddr;

	if (enableMethod != CpuEnableMethod::Unknown)
		sCpuEnableMethod = enableMethod;
}


void
arm64_handle_fdt_psci_node(const void *fdt, int node)
{
	int length;
	const char* method = (const char*)fdt_getprop(fdt, node, "method",
		&length);
	if (method == NULL || length < 2 || method[length - 1] != '\0') {
		dprintf("fdt: psci node has no usable method property, no PSCI "
			"conduit available\n");
		return;
	}

	if (strcmp(method, "smc") == 0) {
		sPsciCallFn = arm64_psci_call_smc;
		gKernelArgs.arch_args.psci_conduit = PSCI_CONDUIT_SMC;
	} else if (strcmp(method, "hvc") == 0) {
		sPsciCallFn = arm64_psci_call_hvc;
		gKernelArgs.arch_args.psci_conduit = PSCI_CONDUIT_HVC;
	} else {
		dprintf("fdt: unsupported psci method \"%s\", no PSCI conduit "
			"available\n", method);
	}
}


static void
arm64_psci_call_smc(uint64 func, uint64 arg0, uint64 arg1, uint64 arg2)
{
	register uint64 x0 asm("x0") = func;
	register uint64 x1 asm("x1") = arg0;
	register uint64 x2 asm("x2") = arg1;
	register uint64 x3 asm("x3") = arg2;
	asm("smc #0" :: "r" (x0), "r" (x1), "r" (x2), "r" (x3));
}


static void
arm64_psci_call_hvc(uint64 func, uint64 arg0, uint64 arg1, uint64 arg2)
{
	register uint64 x0 asm("x0") = func;
	register uint64 x1 asm("x1") = arg0;
	register uint64 x2 asm("x2") = arg1;
	register uint64 x3 asm("x3") = arg2;
	asm("hvc #0" :: "r" (x0), "r" (x1), "r" (x2), "r" (x3));
}
