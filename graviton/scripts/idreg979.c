/*
 * idreg979.c -- issue 979: EL0 reads of the arm64 ID registers (HWCAP_CPUID).
 *
 * Usage: idreg979 [-f]
 *
 * Prints AT_HWCAP/AT_HWCAP2 and whether HWCAP_CPUID is advertised. When it
 * is (or with -f, to show what happens when it is not), it reads every
 * register in the space Linux emulates -- MIDR/MPIDR/REVIDR and op0 3, op1 0,
 * CRn 0, CRm 2-7, op2 0-7 -- with an EL0 MRS and prints each value, so the
 * output can be diffed against Linux on the same instance type. It then
 * checks that:
 *   - an MRS into x29, x30 and xzr is emulated too, and leaves the other
 *     general registers alone;
 *   - ID registers outside the emulated space (CRm 1, the AArch32 ID_PFR0_EL1)
 *     and a non-ID EL1 register (SCTLR_EL1) still raise SIGILL;
 *   - MIDR_EL1 is stable across 100000 reads while the thread migrates.
 * The last line is the failure count; the exit status is non-zero if any
 * row failed.
 */

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

#ifndef HWCAP_CPUID
#	define HWCAP_CPUID	(1UL << 11)
#endif

static sigjmp_buf sJump;
static volatile sig_atomic_t sSignal;
static int sFailures;


static void
signal_handler(int signal)
{
	sSignal = signal;
	siglongjmp(sJump, 1);
}


// One reader per encoding: MRS takes its register as an immediate.
#define READER(crm, op2) \
	static uint64_t read_c##crm##_##op2(void) \
	{ \
		uint64_t value; \
		__asm__ volatile("mrs %0, S3_0_C0_C" #crm "_" #op2 : "=r"(value)); \
		return value; \
	}
#define READERS(crm) READER(crm, 0) READER(crm, 1) READER(crm, 2) \
	READER(crm, 3) READER(crm, 4) READER(crm, 5) READER(crm, 6) READER(crm, 7)
READERS(0) READERS(1) READERS(2) READERS(3) READERS(4) READERS(5) READERS(6)
READERS(7)

#define ROW(crm) { read_c##crm##_0, read_c##crm##_1, read_c##crm##_2, \
	read_c##crm##_3, read_c##crm##_4, read_c##crm##_5, read_c##crm##_6, \
	read_c##crm##_7 }
static uint64_t (*sReaders[8][8])(void) = {
	ROW(0), ROW(1), ROW(2), ROW(3), ROW(4), ROW(5), ROW(6), ROW(7)
};

static const char* kNames[8][8] = {
	{ "MIDR_EL1", NULL, NULL, NULL, NULL, "MPIDR_EL1", "REVIDR_EL1", NULL },
	{ "ID_PFR0_EL1", NULL, NULL, NULL, NULL, NULL, NULL, NULL },
	{ "ID_ISAR0_EL1", NULL, NULL, NULL, NULL, NULL, NULL, NULL },
	{ "MVFR0_EL1", NULL, NULL, NULL, NULL, NULL, NULL, NULL },
	{ "ID_AA64PFR0_EL1", "ID_AA64PFR1_EL1", "ID_AA64PFR2_EL1", NULL,
		"ID_AA64ZFR0_EL1", "ID_AA64SMFR0_EL1", NULL, "ID_AA64FPFR0_EL1" },
	{ "ID_AA64DFR0_EL1", "ID_AA64DFR1_EL1", NULL, NULL,
		"ID_AA64AFR0_EL1", "ID_AA64AFR1_EL1", NULL, NULL },
	{ "ID_AA64ISAR0_EL1", "ID_AA64ISAR1_EL1", "ID_AA64ISAR2_EL1",
		"ID_AA64ISAR3_EL1", NULL, NULL, NULL, NULL },
	{ "ID_AA64MMFR0_EL1", "ID_AA64MMFR1_EL1", "ID_AA64MMFR2_EL1",
		"ID_AA64MMFR3_EL1", "ID_AA64MMFR4_EL1", NULL, NULL, NULL },
};


/*!	Runs one reader; returns 0 and the value, or the signal it raised. */
static int
try_read(uint64_t (*reader)(void), uint64_t* _value)
{
	sSignal = 0;
	if (sigsetjmp(sJump, 1) == 0)
		*_value = reader();
	return sSignal;
}


static void
check(const char* what, int ok)
{
	printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		sFailures++;
}


static uint64_t
read_sctlr(void)
{
	uint64_t value;
	__asm__ volatile("mrs %0, S3_0_C1_C0_0" : "=r"(value));
	return value;
}


int
main(int argc, char** argv)
{
	int force = argc > 1 && strcmp(argv[1], "-f") == 0;
	unsigned long hwcap = getauxval(AT_HWCAP);
	unsigned long hwcap2 = getauxval(AT_HWCAP2);
	int cpuid = (hwcap & HWCAP_CPUID) != 0;

	printf("AT_HWCAP  %#018lx\nAT_HWCAP2 %#018lx\nHWCAP_CPUID %s\n", hwcap,
		hwcap2, cpuid ? "set" : "clear");

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_handler;
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);

	if (!cpuid && !force) {
		printf("HWCAP_CPUID is clear, so ID registers are not readable; "
			"-f reads them anyway\nfailures: 1\n");
		return 1;
	}

	// The dump: every encoding in the emulated space, plus CRm 1 to show
	// it is outside it.
	for (int crm = 0; crm < 8; crm++) {
		for (int op2 = 0; op2 < 8; op2++) {
			uint64_t value = 0;
			int signal = try_read(sReaders[crm][op2], &value);
			char name[32];
			if (kNames[crm][op2] != NULL)
				snprintf(name, sizeof(name), "%s", kNames[crm][op2]);
			else
				snprintf(name, sizeof(name), "S3_0_C0_C%d_%d", crm, op2);
			if (signal != 0)
				printf("reg %-18s signal %d\n", name, signal);
			else
				printf("reg %-18s %#018llx\n", name,
					(unsigned long long)value);
		}
	}

	if (!cpuid) {
		printf("failures: 1\n");
		return 1;
	}

	// The registers x29/x30/xzr are kept apart from x0-x28 in DeBeOS's
	// iframe, so they take a different store path. x19-x28 are callee-saved
	// and set around the MRS so a stray write would show.
	uint64_t midr = read_c0_0();
	uint64_t results[5];
	// The results go out through x9, pinned so that it can be neither x29
	// nor x30, which the sequence overwrites.
	register uint64_t* out __asm__("x9") = results;
	__asm__ volatile(
		"mov x19, #0x1919\n\t"
		"mov x20, #0x2020\n\t"
		"mov x28, #0x2828\n\t"
		"stp x29, x30, [sp, #-16]!\n\t"
		"mrs x29, MIDR_EL1\n\t"
		"mrs x30, MIDR_EL1\n\t"
		"stp x29, x30, [%0]\n\t"
		"ldp x29, x30, [sp], #16\n\t"
		"mrs xzr, MIDR_EL1\n\t"
		"stp x19, x20, [%0, #16]\n\t"
		"str x28, [%0, #32]\n\t"
		:
		: "r"(out)
		: "x19", "x20", "x28", "x30", "memory");
	uint64_t viaFp = results[0], viaLr = results[1];
	uint64_t* others = &results[2];
	check("MRS into x29 reads MIDR_EL1", viaFp == midr);
	check("MRS into x30 reads MIDR_EL1", viaLr == midr);
	check("MRS into xzr is skipped, x19/x20/x28 untouched",
		others[0] == 0x1919 && others[1] == 0x2020 && others[2] == 0x2828);

	uint64_t dummy;
	check("ID_PFR0_EL1 (CRm 1, not emulated) raises SIGILL",
		try_read(read_c1_0, &dummy) == SIGILL);
	check("SCTLR_EL1 (not an ID register) raises SIGILL",
		try_read(read_sctlr, &dummy) == SIGILL);

	int stable = 1;
	for (int i = 0; i < 100000; i++) {
		if (read_c0_0() != midr) {
			stable = 0;
			break;
		}
		if (i % 1000 == 0)
			usleep(0);
	}
	check("MIDR_EL1 stable over 100000 reads", stable);

	printf("failures: %d\n", sFailures);
	return sFailures != 0;
}
