/*
 * rng980.c -- issue 980: AT_HWCAP2 must advertise HWCAP2_RNG on a CPU with
 * FEAT_RNG (RNDR/RNDRRS), as Linux does on the same instance type.
 *
 * Usage: rng980
 *
 * Prints AT_HWCAP/AT_HWCAP2 and the HWCAP2_RNG bit. It then executes RNDR and
 * RNDRRS 10000 times each regardless of the bit -- they are EL0 instructions
 * on any CPU that implements FEAT_RNG, so this shows whether the bit tells
 * the truth -- counting successes (NZCV.Z clear) and repeated values, and
 * catching SIGILL for a CPU that lacks them. Rows:
 *   - HWCAP2_RNG agrees with whether RNDR executes
 *   - RNDR and RNDRRS succeed on most reads and never repeat a value
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

#ifndef HWCAP2_RNG
#	define HWCAP2_RNG	(1UL << 16)
#endif

#define READS	10000

static sigjmp_buf sJump;
static int sFailures;


static void
sigill_handler(int signal)
{
	siglongjmp(sJump, 1);
}


// RNDR is S3_3_C2_C4_0, RNDRRS S3_3_C2_C4_1; Z is set when no random number
// could be returned in reasonable time.
#define RANDOM_READER(name, encoding) \
	static int name(uint64_t* _value) \
	{ \
		uint64_t value, nzcv; \
		__asm__ volatile("mrs %0, " encoding "\n\tmrs %1, nzcv" \
			: "=r"(value), "=r"(nzcv) : : "cc"); \
		*_value = value; \
		return (nzcv & (1UL << 30)) == 0; \
	}
RANDOM_READER(read_rndr, "S3_3_C2_C4_0")
RANDOM_READER(read_rndrrs, "S3_3_C2_C4_1")


static void
check(const char* what, int ok)
{
	printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		sFailures++;
}


static int
compare_values(const void* a, const void* b)
{
	uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
	return x < y ? -1 : x > y;
}


/*!	Returns -1 on SIGILL, else the number of successful reads; *_repeats gets
	the number of duplicates among them. */
static int
exercise(int (*reader)(uint64_t*), int* _repeats)
{
	static uint64_t values[READS];
	volatile int good = 0;
	*_repeats = 0;
	if (sigsetjmp(sJump, 1) != 0)
		return -1;
	for (int i = 0; i < READS; i++) {
		uint64_t value;
		if (reader(&value))
			values[good++] = value;
	}
	qsort(values, good, sizeof(values[0]), compare_values);
	for (int i = 1; i < good; i++) {
		if (values[i] == values[i - 1])
			(*_repeats)++;
	}
	return good;
}


int
main(void)
{
	unsigned long hwcap = getauxval(AT_HWCAP);
	unsigned long hwcap2 = getauxval(AT_HWCAP2);
	int advertised = (hwcap2 & HWCAP2_RNG) != 0;
	printf("AT_HWCAP  %#018lx\nAT_HWCAP2 %#018lx\nHWCAP2_RNG %s\n", hwcap,
		hwcap2, advertised ? "set" : "clear");

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = sigill_handler;
	sigaction(SIGILL, &action, NULL);

	int repeats, repeatsRS;
	int good = exercise(read_rndr, &repeats);
	int goodRS = exercise(read_rndrrs, &repeatsRS);
	printf("RNDR   %s: %d/%d reads succeeded, %d repeated\n",
		good < 0 ? "SIGILL" : "executes", good < 0 ? 0 : good, READS,
		repeats);
	printf("RNDRRS %s: %d/%d reads succeeded, %d repeated\n",
		goodRS < 0 ? "SIGILL" : "executes", goodRS < 0 ? 0 : goodRS, READS,
		repeatsRS);

	check("HWCAP2_RNG agrees with whether RNDR executes",
		advertised == (good >= 0));
	if (good >= 0) {
		check("RNDR: most reads succeed, no value repeats",
			good > READS / 2 && repeats == 0);
		check("RNDRRS: most reads succeed, no value repeats",
			goodRS > READS / 2 && repeatsRS == 0);
	}

	printf("failures: %d\n", sFailures);
	return sFailures != 0;
}
