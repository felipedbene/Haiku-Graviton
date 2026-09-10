/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include <KernelExport.h>

#include <sys/auxv.h>

#include <arch/arm64/arch_pmu.h>
#include <arch/cpu.h>
#include <boot/kernel_args.h>
#include <commpage.h>
#include <elf.h>


extern "C" void _exception_vectors(void);


// PSCI function IDs (ARM DEN 0022, SMC32 calling convention). Both are
// mandatory from PSCI 0.2 on, which is the version the FADT and the device
// tree bindings the boot loader looks at describe.
#define PSCI_SYSTEM_OFF		0x84000008
#define PSCI_SYSTEM_RESET	0x84000009

// The SMC calling convention only guarantees x18 and above are preserved, so
// everything the compiler could be keeping in a caller-saved register has to
// be declared clobbered. This matters on the error return: PSCI_SYSTEM_OFF and
// PSCI_SYSTEM_RESET do not come back when they succeed.
#define PSCI_CLOBBERS \
	"x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", \
	"x15", "x16", "x17", "memory"


static uint32 sPsciConduit = PSCI_CONDUIT_NONE;


static uint64
psci_call_smc(uint32 function)
{
	register uint64 x0 asm("x0") = function;
	register uint64 x1 asm("x1") = 0;
	register uint64 x2 asm("x2") = 0;
	register uint64 x3 asm("x3") = 0;
	asm volatile("smc #0"
		: "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
		:
		: PSCI_CLOBBERS);
	return x0;
}


static uint64
psci_call_hvc(uint32 function)
{
	register uint64 x0 asm("x0") = function;
	register uint64 x1 asm("x1") = 0;
	register uint64 x2 asm("x2") = 0;
	register uint64 x3 asm("x3") = 0;
	asm volatile("hvc #0"
		: "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
		:
		: PSCI_CLOBBERS);
	return x0;
}


status_t
arch_cpu_preboot_init_percpu(kernel_args *args, int curr_cpu)
{
	WRITE_SPECIALREG(VBAR_EL1, _exception_vectors);
	return B_OK;
}


status_t
arch_cpu_init_percpu(kernel_args *args, int curr_cpu)
{
	uint64_t tcr = READ_SPECIALREG(TCR_EL1);
	uint64_t mmfr1 = READ_SPECIALREG(ID_AA64MMFR1_EL1);

	uint64_t hafdbs = ID_AA64MMFR1_HAFDBS(mmfr1);
	if (hafdbs == ID_AA64MMFR1_HAFDBS_AF) {
		tcr |= (1UL << 39);
	}
	if (hafdbs == ID_AA64MMFR1_HAFDBS_AF_DBS) {
		tcr |= (1UL << 40) | (1UL << 39);
	}

	WRITE_SPECIALREG(TCR_EL1, tcr);

	gCPU[curr_cpu].arch.mpidr = READ_SPECIALREG(MPIDR_EL1);

	// Every CPU has to program its own performance monitors; this is the only
	// hook that runs on all of them. It is a no-op unless the facility was
	// asked for, because reaching the PMU registers is not safe everywhere
	// this image boots (see arch_pmu.cpp).
	arm64_pmu_init_percpu(args, curr_cpu);

	return 0;
}


status_t
arch_cpu_init(kernel_args *args)
{
	sPsciConduit = args->arch_args.psci_conduit;
	dprintf("PSCI conduit: %s\n",
		sPsciConduit == PSCI_CONDUIT_SMC ? "smc"
			: sPsciConduit == PSCI_CONDUIT_HVC ? "hvc" : "none");

	for (uint32 i = 0; i < args->num_cpus; i++) {
		cpu_ent* cpu = &gCPU[i];

		cpu->topology_id[CPU_TOPOLOGY_PACKAGE] = 0;
		cpu->topology_id[CPU_TOPOLOGY_CORE] = i;
		cpu->topology_id[CPU_TOPOLOGY_SMT] = 0;
	}

	// Detection only, and deliberately before arch_cpu_init_percpu() runs: it
	// decides whether any CPU programs its counters at all.
	arm64_pmu_init(args);

	return B_OK;
}


status_t
arch_cpu_init_post_vm(kernel_args *args)
{
	return B_OK;
}


status_t
arch_cpu_init_post_modules(kernel_args *args)
{
	// Late enough that add_debugger_command_etc() can allocate.
	arm64_pmu_init_post_modules(args);

	return B_OK;
}


void
arm64_get_hwcap(uint64* hwcap, uint64* hwcap2)
{
	// The ID_AA64* feature registers are only readable at EL1, and DeBeOS does
	// not trap-and-emulate their MRS for EL0, so this runs once in the kernel
	// and the result is published through the commpage. The bit layout mirrors
	// Linux's AT_HWCAP so ported crypto/atomics dispatch keeps working.
	//
	// A homogeneous ISA across CPUs is assumed (as Linux does with its
	// boot-CPU-sanitised values); every core on the supported SoCs advertises
	// the same feature set.
	uint64 caps = 0;
	uint64 caps2 = 0;

	const uint64 isar0 = READ_SPECIALREG(ID_AA64ISAR0_EL1);
	const uint64 isar1 = READ_SPECIALREG(ID_AA64ISAR1_EL1);
	const uint64 pfr0 = READ_SPECIALREG(ID_AA64PFR0_EL1);

	// ID_AA64PFR0_EL1: floating point and Advanced SIMD (0xf encodes "absent").
	if (ID_AA64PFR0_FP(pfr0) != ID_AA64PFR0_FP_NONE) {
		caps |= HWCAP_FP;
		if (ID_AA64PFR0_FP(pfr0) == ID_AA64PFR0_FP_HP)
			caps |= HWCAP_FPHP;
	}
	if (ID_AA64PFR0_ADV_SIMD(pfr0) != ID_AA64PFR0_ADV_SIMD_NONE) {
		caps |= HWCAP_ASIMD;
		if (ID_AA64PFR0_ADV_SIMD(pfr0) == ID_AA64PFR0_ADV_SIMD_HP)
			caps |= HWCAP_ASIMDHP;
	}
	// Deliberately NOT advertising HWCAP_SVE: SVE instructions trap at EL0 until
	// the kernel enables the SVE path (CPACR_EL1.ZEN), which it does not yet do.
	// Setting the bit here would tell userland to run instructions that fault.

	// ID_AA64ISAR0_EL1: crypto and integer extensions.
	if (ID_AA64ISAR0_AES(isar0) >= ID_AA64ISAR0_AES_BASE) {
		caps |= HWCAP_AES;
		if (ID_AA64ISAR0_AES(isar0) >= ID_AA64ISAR0_AES_PMULL)
			caps |= HWCAP_PMULL;
	}
	if (ID_AA64ISAR0_SHA1(isar0) >= ID_AA64ISAR0_SHA1_BASE)
		caps |= HWCAP_SHA1;
	if (ID_AA64ISAR0_SHA2(isar0) >= ID_AA64ISAR0_SHA2_BASE) {
		caps |= HWCAP_SHA2;
		if (ID_AA64ISAR0_SHA2(isar0) >= ID_AA64ISAR0_SHA2_512)
			caps |= HWCAP_SHA512;
	}
	if (ID_AA64ISAR0_CRC32(isar0) >= ID_AA64ISAR0_CRC32_BASE)
		caps |= HWCAP_CRC32;
	if (ID_AA64ISAR0_ATOMIC(isar0) >= ID_AA64ISAR0_ATOMIC_IMPL)
		caps |= HWCAP_ATOMICS;
	if (ID_AA64ISAR0_RDM(isar0) >= ID_AA64ISAR0_RDM_IMPL)
		caps |= HWCAP_ASIMDRDM;
	if (ID_AA64ISAR0_SHA3(isar0) >= ID_AA64ISAR0_SHA3_IMPL)
		caps |= HWCAP_SHA3;
	if (ID_AA64ISAR0_SM3(isar0) >= ID_AA64ISAR0_SM3_IMPL)
		caps |= HWCAP_SM3;
	if (ID_AA64ISAR0_SM4(isar0) >= ID_AA64ISAR0_SM4_IMPL)
		caps |= HWCAP_SM4;
	if (ID_AA64ISAR0_DP(isar0) >= ID_AA64ISAR0_DP_IMPL)
		caps |= HWCAP_ASIMDDP;

	// ID_AA64ISAR1_EL1.
	if (ID_AA64ISAR1_DPB(isar1) >= ID_AA64ISAR1_DPB_IMPL) {
		caps |= HWCAP_DCPOP;
		if (ID_AA64ISAR1_DPB(isar1) >= (0x2 << ID_AA64ISAR1_DPB_SHIFT))
			caps2 |= HWCAP2_DCPODP;
	}
	if (ID_AA64ISAR1_JSCVT(isar1) >= ID_AA64ISAR1_JSCVT_IMPL)
		caps |= HWCAP_JSCVT;
	if (ID_AA64ISAR1_FCMA(isar1) >= ID_AA64ISAR1_FCMA_IMPL)
		caps |= HWCAP_FCMA;
	if (ID_AA64ISAR1_LRCPC(isar1) >= ID_AA64ISAR1_LRCPC_IMPL) {
		caps |= HWCAP_LRCPC;
		if (ID_AA64ISAR1_LRCPC(isar1) >= (0x2 << ID_AA64ISAR1_LRCPC_SHIFT))
			caps |= HWCAP_ILRCPC;
	}

	*hwcap = caps;
	*hwcap2 = caps2;
}


status_t
arch_cpu_shutdown(bool reboot)
{
	if (sPsciConduit == PSCI_CONDUIT_NONE) {
		dprintf("arch_cpu_shutdown: no PSCI conduit, cannot %s\n",
			reboot ? "reset the system" : "power the system off");
		return B_ERROR;
	}

	uint32 function = reboot ? PSCI_SYSTEM_RESET : PSCI_SYSTEM_OFF;

	disable_interrupts();

	uint64 result = sPsciConduit == PSCI_CONDUIT_HVC
		? psci_call_hvc(function) : psci_call_smc(function);

	// A successful call does not return, so getting here is always a failure.
	dprintf("arch_cpu_shutdown: PSCI call %#" B_PRIx32 " returned %" B_PRId64
		"\n", function, (int64)result);
	return B_ERROR;
}


void
arch_cpu_sync_icache(void *address, size_t len)
{
	uint64_t ctr_el0 = 0;
	asm volatile ("mrs\t%0, ctr_el0":"=r" (ctr_el0));

	uint64_t icache_line_size = 4 << (ctr_el0 & 0xF);
	uint64_t dcache_line_size = 4 << ((ctr_el0 >> 16) & 0xF);
	uint64_t addr = (uint64_t)address;
	uint64_t end = addr + len;

	for (uint64_t address_dcache = ROUNDDOWN(addr, dcache_line_size);
	     address_dcache < end; address_dcache += dcache_line_size) {
		asm volatile ("dc cvau, %0" : : "r"(address_dcache) : "memory");
	}

	asm("dsb ish");

	for (uint64_t address_icache = ROUNDDOWN(addr, icache_line_size);
         address_icache < end; address_icache += icache_line_size) {
		asm volatile ("ic ivau, %0" : : "r"(address_icache) : "memory");
	}
	asm("dsb ish");
	asm("isb");
}


void
arch_cpu_invalidate_tlb_range(intptr_t, addr_t start, addr_t end)
{
	arch_cpu_global_tlb_invalidate();
}


void
arch_cpu_invalidate_tlb_list(intptr_t, addr_t pages[], int num_pages)
{
	arch_cpu_global_tlb_invalidate();
}


void
arch_cpu_global_tlb_invalidate()
{
	asm(
		"dsb ishst\n"
		"tlbi vmalle1\n"
		"dsb ish\n"
		"isb\n"
	);
}


void
arch_cpu_user_tlb_invalidate(intptr_t)
{
	arch_cpu_global_tlb_invalidate();
}
