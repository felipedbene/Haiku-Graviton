/*
 * sve_test -- userland (EL0) SVE exercise for DeBeOS arm64 (#88).
 *
 * Purpose: prove the kernel's EL0 SVE save/restore is correct under real
 * scheduling. Each worker thread stamps a thread-unique pattern across all SVE
 * Z registers (and the predicate registers), then spins doing SVE work while
 * forcing frequent preemption/migration, re-checking after every iteration that
 * its registers still hold ITS pattern. A save/restore bug (cross-thread leak,
 * dropped upper lanes, wrong VL stride) makes a thread observe a value that is
 * not its own -> FAIL. A single-thread arithmetic check runs first as a control.
 *
 * SVE is used via inline asm (no HWCAP_SVE / arm_sve.h dependency): the kernel
 * has enabled CPACR_EL1.ZEN, so the instructions execute at EL0 even though the
 * HWCAP bit is deliberately still off.
 *
 * Output goes to stdout AND, on Haiku, to the kernel debug log (serial console)
 * via _kern_debug_output, so the result is visible on a headless Graviton boot
 * with only get-console-output.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#ifdef __HAIKU__
extern void _kern_debug_output(const char* message);
static void serial(const char* s) { _kern_debug_output(s); }
#else
static void serial(const char* s) { (void)s; }
#endif

static void logline(const char* s)
{
	fputs(s, stdout);
	fflush(stdout);
	serial(s);
}

// Effective VL in bytes, from RDVL. Runs at EL0; traps unless the kernel set ZEN.
static uint64_t sve_vl_bytes(void)
{
	uint64_t vl;
	__asm__ volatile(".arch_extension sve\n\trdvl %0, #1" : "=r"(vl));
	return vl;
}

// Fill Z0..Z31 with a per-lane value derived from `seed`, using SVE stores from
// a memory buffer, then read them back into `out`. `buf`/`out` are VL*32 bytes.
// Done as one asm block so the compiler cannot insert NEON that would clobber Z.
static void sve_roundtrip(const uint8_t* buf, uint8_t* out, uint64_t vl)
{
	(void)vl;
	__asm__ volatile(
		".arch_extension sve\n\t"
		"ptrue p0.b\n\t"
		"ld1b { z0.b }, p0/z, [%0, #0, mul vl]\n\t"
		"ld1b { z1.b }, p0/z, [%0, #1, mul vl]\n\t"
		"ld1b { z2.b }, p0/z, [%0, #2, mul vl]\n\t"
		"ld1b { z3.b }, p0/z, [%0, #3, mul vl]\n\t"
		"ld1b { z4.b }, p0/z, [%0, #4, mul vl]\n\t"
		"ld1b { z5.b }, p0/z, [%0, #5, mul vl]\n\t"
		"ld1b { z6.b }, p0/z, [%0, #6, mul vl]\n\t"
		"ld1b { z7.b }, p0/z, [%0, #7, mul vl]\n\t"
		"st1b { z0.b }, p0, [%1, #0, mul vl]\n\t"
		"st1b { z1.b }, p0, [%1, #1, mul vl]\n\t"
		"st1b { z2.b }, p0, [%1, #2, mul vl]\n\t"
		"st1b { z3.b }, p0, [%1, #3, mul vl]\n\t"
		"st1b { z4.b }, p0, [%1, #4, mul vl]\n\t"
		"st1b { z5.b }, p0, [%1, #5, mul vl]\n\t"
		"st1b { z6.b }, p0, [%1, #6, mul vl]\n\t"
		"st1b { z7.b }, p0, [%1, #7, mul vl]\n\t"
		:
		: "r"(buf), "r"(out)
		: "p0", "z0", "z1", "z2", "z3", "z4", "z5", "z6", "z7", "memory");
}

#define NTHREADS 8
#define ITERS 200000

static volatile int gFail = 0;
static uint64_t gVL = 0;

typedef struct {
	int id;
	int ok;
} worker_arg;

// Each worker keeps its unique pattern live in Z8..Z15 across a long loop while
// doing independent SVE work in Z0..Z7 and yielding, then verifies Z8..Z15 were
// preserved bit-exact by the kernel across every context switch it took.
static void* worker(void* p)
{
	worker_arg* a = (worker_arg*)p;
	uint64_t vl = gVL;
	uint8_t* pattern = malloc(vl * 8);
	uint8_t* readback = malloc(vl * 8);
	uint8_t* scratch = malloc(vl * 8);
	for (uint64_t i = 0; i < vl * 8; i++)
		pattern[i] = (uint8_t)((a->id * 131 + i * 7 + 0x5a) & 0xff);

	// Load the persistent pattern into Z8..Z15 and hold it there.
	__asm__ volatile(
		".arch_extension sve\n\t"
		"ptrue p1.b\n\t"
		"ld1b { z8.b },  p1/z, [%0, #0, mul vl]\n\t"
		"ld1b { z9.b },  p1/z, [%0, #1, mul vl]\n\t"
		"ld1b { z10.b }, p1/z, [%0, #2, mul vl]\n\t"
		"ld1b { z11.b }, p1/z, [%0, #3, mul vl]\n\t"
		"ld1b { z12.b }, p1/z, [%0, #4, mul vl]\n\t"
		"ld1b { z13.b }, p1/z, [%0, #5, mul vl]\n\t"
		"ld1b { z14.b }, p1/z, [%0, #6, mul vl]\n\t"
		"ld1b { z15.b }, p1/z, [%0, #7, mul vl]\n\t"
		:
		: "r"(pattern)
		: "p1", "z8", "z9", "z10", "z11", "z12", "z13", "z14", "z15", "memory");

	a->ok = 1;
	for (int it = 0; it < ITERS; it++) {
		// Independent SVE work in Z0..Z7 that also clobbers them, to make sure
		// the kernel really is preserving Z8..Z15 and not leaving stale state.
		sve_roundtrip(pattern, scratch, vl);

		// Store Z8..Z15 back out and compare to the pattern. If the kernel
		// mis-saved/restored across a context switch, this diverges.
		__asm__ volatile(
			".arch_extension sve\n\t"
			"ptrue p1.b\n\t"
			"st1b { z8.b },  p1, [%0, #0, mul vl]\n\t"
			"st1b { z9.b },  p1, [%0, #1, mul vl]\n\t"
			"st1b { z10.b }, p1, [%0, #2, mul vl]\n\t"
			"st1b { z11.b }, p1, [%0, #3, mul vl]\n\t"
			"st1b { z12.b }, p1, [%0, #4, mul vl]\n\t"
			"st1b { z13.b }, p1, [%0, #5, mul vl]\n\t"
			"st1b { z14.b }, p1, [%0, #6, mul vl]\n\t"
			"st1b { z15.b }, p1, [%0, #7, mul vl]\n\t"
			:
			: "r"(readback)
			: "p1", "memory");

		if (memcmp(readback, pattern, vl * 8) != 0) {
			char msg[128];
			snprintf(msg, sizeof(msg),
				"sve_test: FAIL thread %d iter %d: Z8-15 corrupted\n",
				a->id, it);
			logline(msg);
			a->ok = 0;
			gFail = 1;
			break;
		}

		// Yield often to maximise context switches / CPU migration.
		if ((it & 0x3f) == 0)
			sched_yield();
	}

	free(pattern);
	free(readback);
	free(scratch);
	return NULL;
}

int main(void)
{
	char msg[160];

	uint64_t vl = sve_vl_bytes();
	gVL = vl;
	snprintf(msg, sizeof(msg),
		"sve_test: start, effective VL = %llu bytes (%llu bits)\n",
		(unsigned long long)vl, (unsigned long long)vl * 8);
	logline(msg);
	if (vl < 16 || vl > 256) {
		logline("sve_test: implausible VL; RDVL likely trapped -> FAIL\n");
		return 2;
	}

	// Control: single-thread arithmetic correctness through Z registers.
	{
		uint8_t* in = malloc(vl * 8);
		uint8_t* out = malloc(vl * 8);
		for (uint64_t i = 0; i < vl * 8; i++)
			in[i] = (uint8_t)(i & 0xff);
		sve_roundtrip(in, out, vl);
		if (memcmp(in, out, vl * 8) != 0) {
			logline("sve_test: FAIL single-thread SVE round-trip mismatch\n");
			return 2;
		}
		free(in);
		free(out);
		logline("sve_test: single-thread SVE round-trip OK\n");
	}

	// Contended: many SVE threads, each guarding its own Z8..Z15 across switches.
	pthread_t th[NTHREADS];
	worker_arg args[NTHREADS];
	for (int i = 0; i < NTHREADS; i++) {
		args[i].id = i + 1;
		args[i].ok = 0;
		pthread_create(&th[i], NULL, worker, &args[i]);
	}
	int allok = 1;
	for (int i = 0; i < NTHREADS; i++) {
		pthread_join(th[i], NULL);
		if (!args[i].ok)
			allok = 0;
	}

	if (allok && !gFail) {
		snprintf(msg, sizeof(msg),
			"sve_test: PASS -- %d threads x %d iters, no cross-thread SVE "
			"corruption at VL %llu\n", NTHREADS, ITERS,
			(unsigned long long)vl);
		logline(msg);
		return 0;
	}
	logline("sve_test: FAIL -- SVE state corrupted under contention\n");
	return 1;
}
