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


// Effective SVE vector length in bytes, or 0 when SVE is absent or disabled.
// Set once per CPU by arch_sve_init_percpu(); read by the EL0 exception FP
// save/restore path (_fp_save_el0/_fp_restore_el0 in arch_asm.S) to decide
// whether to also save/restore the SVE Z/P/FFR state, and by
// arch_restore_signal_frame(). C linkage and a plain scalar so the assembly can
// load it directly. All CPUs on the supported homogeneous SoCs converge on the
// same value, so the repeated writes are benign. Hidden visibility keeps the
// symbol non-preemptible for the adrp/add reference in the -shared kernel image.
extern "C" { uint32 gArm64SVEVectorBytes __attribute__((visibility("hidden"))) = 0; }


// Enable SVE for EL0/EL1 on this CPU and program the effective vector length.
// Runs from arch_cpu_init_percpu() on every core, with interrupts masked and
// before the CPU takes any EL0 exception, so by the time an SVE instruction can
// execute (only ever on the EL0 save/restore path) this CPU's CPACR_EL1.ZEN is
// already set -- that ordering is what the first attempt got wrong. SVE stays
// fully disabled (and gArm64SVEVectorBytes stays 0, keeping the NEON-only save
// path) on hardware that does not implement it.
//
// On a VHE host (c7g.metal runs the kernel at EL2 with HCR_EL2.E2H set), the
// CPACR_EL1 and ZCR_EL1 names below resolve to CPTR_EL2/ZCR_EL2, which govern
// the current EL there; the ZEN field layout is identical, so the same code is
// correct at EL1 (c7g) and EL2 (c7g.metal). The kernel only ever runs at EL1 or
// at EL2-with-E2H (arch_start.cpp drops plain EL2 to EL1), so no CurrentEL gate
// is needed for these accesses to hit the register that controls this EL.
static void
arch_sve_init_percpu(int curr_cpu)
{
	const uint64 pfr0 = READ_SPECIALREG(ID_AA64PFR0_EL1);
	if (ID_AA64PFR0_SVE(pfr0) == ID_AA64PFR0_SVE_NONE) {
		if (curr_cpu == 0)
			dprintf("arm64: SVE not implemented; NEON-only FP save path\n");
		return;
	}

	// Let EL0 and EL1 execute SVE instructions (ZEN = 0b11, no trap). EL1 access
	// is required because the save/restore spill runs at EL1.
	uint64 cpacr = READ_SPECIALREG(CPACR_EL1);
	cpacr = (cpacr & ~(uint64)CPACR_ZEN_MASK) | CPACR_ZEN_TRAP_NONE;
	WRITE_SPECIALREG(CPACR_EL1, cpacr);
	arm64_isb();

	// Clamp the effective VL to the first-cut cap so the fixed per-thread SVE
	// save area can never overflow. A hardware VL below the cap is left as-is; a
	// larger one is clamped down by ZCR_EL1.LEN. RDVL then reports the resulting
	// effective VL in bytes. This whole block is one asm statement so
	// ".arch_extension sve" covers ZCR_EL1 and RDVL: the TU is built with
	// -mcpu=neoverse-n1, whose assembler otherwise rejects both.
	uint64 vectorBytes;
	__asm__ volatile(
		".arch_extension sve\n\t"
		"msr ZCR_EL1, %1\n\t"
		"isb\n\t"
		"rdvl %0, #1"
		: "=r"(vectorBytes)
		: "r"((uint64)((SVE_MAX_VL_BYTES / 16) - 1)));
	gArm64SVEVectorBytes = (uint32)vectorBytes;

	// One concise line per CPU (mirrors the GIC per-CPU logging): on a 2-vCPU
	// c7g.large this is two lines, and it is the evidence that the hardware
	// exposes SVE, at what VL, and that ZEN actually took on this core.
	dprintf("arm64: cpu %d SVE enabled, VL %" B_PRIu32 " bytes (%" B_PRIu32
		" bits), CPACR %#" B_PRIx64 "\n", curr_cpu, gArm64SVEVectorBytes,
		gArm64SVEVectorBytes * 8, READ_SPECIALREG(CPACR_EL1));
}


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

	// Turn on SVE access and fix the vector length before this CPU can take any
	// EL0 exception, so the EL0 FP save/restore path can rely on ZEN being set.
	arch_sve_init_percpu(curr_cpu);

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
	// Deliberately still NOT advertising HWCAP_SVE. The kernel now enables SVE
	// access (CPACR_EL1.ZEN) and saves/restores Z/P/FFR across EL0<->EL1 and
	// therefore across context switches (arch_sve_init_percpu(),
	// _fp_save_el0/_fp_restore_el0). Flipping this bit on -- so getauxval()
	// tells userland SVE is usable -- is the explicit follow-up (#99) once an
	// SVE workload is validated on hardware to compute correctly across
	// preemption with no NEON/FP regression.

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
	if (ID_AA64ISAR0_FHM(isar0) >= ID_AA64ISAR0_FHM_IMPL)
		caps |= HWCAP_ASIMDFHM;
	if (ID_AA64ISAR0_TS(isar0) >= ID_AA64ISAR0_TS_FLAGM) {
		caps |= HWCAP_FLAGM;
		if (ID_AA64ISAR0_TS(isar0) >= ID_AA64ISAR0_TS_FLAGM2)
			caps2 |= HWCAP2_FLAGM2;
	}

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
	if (ID_AA64ISAR1_FRINTTS(isar1) >= ID_AA64ISAR1_FRINTTS_IMPL)
		caps2 |= HWCAP2_FRINT;
	if (ID_AA64ISAR1_SB(isar1) >= ID_AA64ISAR1_SB_IMPL)
		caps |= HWCAP_SB;
	// The int8 matrix-multiply (I8MM) and BFloat16 (BF16) extensions are the
	// NEON data paths that ported ML/GEMM code (ggml/llama.cpp, libhwy) selects
	// at runtime via AT_HWCAP2; unlike SVE they add no EL0-visible register
	// state, so advertising them needs no context-switch support.
	if (ID_AA64ISAR1_BF16(isar1) >= ID_AA64ISAR1_BF16_IMPL)
		caps2 |= HWCAP2_BF16;
	if (ID_AA64ISAR1_I8MM(isar1) >= ID_AA64ISAR1_I8MM_IMPL)
		caps2 |= HWCAP2_I8MM;

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
