/*
 * sve_fork_test -- fork() FP/SIMD preservation check for DeBeOS arm64.
 *
 * Regression test for the SVE fork bug: on SVE hardware (gArm64SVEVectorBytes
 * != 0, i.e. Graviton 3/4) the EL0 return path restores V0-31 from the thread's
 * off-stack SVE Z buffer, not from the iframe. A fork()ed child starts with a
 * zeroed SVE buffer, so before the fix the child returned to userspace with its
 * FP/SIMD registers cleared -- including the AAPCS callee-saved d8-d15, which
 * the ABI requires to survive the fork() call. Baseline kernel: child sees 0.
 * Fixed kernel: child sees the parent's values.
 *
 * The test is deliberately narrow: it asserts only the low 64 bits d8-d15,
 * which are the registers the procedure call standard guarantees across the
 * fork() call. The upper 64 bits of V8-V15 and all of V0-V7/V16-V31 are
 * caller-saved and may legitimately be clobbered by libc's fork() itself, and
 * the SVE upper lanes are likewise not required to survive -- so testing them
 * would produce false failures independent of the kernel bug.
 *
 * Output goes to stdout AND, on Haiku, to the kernel debug log so the result is
 * visible on a headless Graviton boot via get-console-output.
 */

// Bare aarch64 inline asm below, so this is an arm64-only tool. The build
// gates it on TARGET_ARCH = arm64 (src/bin/sve_test/Jamfile); this guard only
// turns a hand-compile for another architecture into one legible error rather
// than a wall of assembler diagnostics.
#ifndef __aarch64__
#	error "sve_fork_test is arm64-only (bare aarch64 inline asm)"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

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

// Known, non-zero, per-register bit patterns for d8..d15.
static const uint64_t kPattern[8] = {
	0x0808080808080808ULL, 0x1909190919091909ULL,
	0x2a0a2a0a2a0a2a0aULL, 0x3b0b3b0b3b0b3b0bULL,
	0x4c0c4c0c4c0c4c0cULL, 0x5d0d5d0d5d0d5d0dULL,
	0x6e0e6e0e6e0e6e0eULL, 0x7f0f7f0f7f0f7f0fULL,
};

// Load d8..d15 from an 8-entry uint64 buffer. Clobbering the registers tells
// the compiler not to keep any C value there, so the raw values we write are
// what a subsequent kernel entry (fork) sees and must preserve.
static inline void load_d8_d15(const uint64_t* v)
{
	__asm__ volatile(
		"ldr d8,  [%0, #0]\n\t"
		"ldr d9,  [%0, #8]\n\t"
		"ldr d10, [%0, #16]\n\t"
		"ldr d11, [%0, #24]\n\t"
		"ldr d12, [%0, #32]\n\t"
		"ldr d13, [%0, #40]\n\t"
		"ldr d14, [%0, #48]\n\t"
		"ldr d15, [%0, #56]\n\t"
		:
		: "r"(v)
		: "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15", "memory");
}

// Store d8..d15 back out to an 8-entry uint64 buffer.
static inline void store_d8_d15(uint64_t* v)
{
	__asm__ volatile(
		"str d8,  [%0, #0]\n\t"
		"str d9,  [%0, #8]\n\t"
		"str d10, [%0, #16]\n\t"
		"str d11, [%0, #24]\n\t"
		"str d12, [%0, #32]\n\t"
		"str d13, [%0, #40]\n\t"
		"str d14, [%0, #48]\n\t"
		"str d15, [%0, #56]\n\t"
		:
		: "r"(v)
		: "memory");
}

int main(void)
{
	uint64_t child[8];

	logline("sve_fork_test: start\n");

	// Stamp the pattern into d8..d15, then fork immediately. Nothing between the
	// stamp and the fork() syscall uses FP, so these are the live values at the
	// child's kernel entry.
	load_d8_d15(kPattern);

	pid_t pid = fork();
	if (pid < 0) {
		logline("sve_fork_test: fork() failed -> FAIL\n");
		return 2;
	}

	if (pid == 0) {
		// Child: read d8..d15 back before anything can touch FP, then compare.
		store_d8_d15(child);
		int ok = (memcmp(child, kPattern, sizeof(child)) == 0);
		int zeroed = 1;
		for (int i = 0; i < 8; i++)
			if (child[i] != 0)
				zeroed = 0;
		char msg[256];
		snprintf(msg, sizeof(msg),
			"sve_fork_test: child d8=%016llx (want %016llx) d15=%016llx "
			"(want %016llx) -> %s\n",
			(unsigned long long)child[0], (unsigned long long)kPattern[0],
			(unsigned long long)child[7], (unsigned long long)kPattern[7],
			ok ? "PASS" : (zeroed ? "FAIL (child FP zeroed)" : "FAIL"));
		logline(msg);
		_exit(ok ? 0 : 1);
	}

	// Parent: wait for the child and relay its verdict.
	int status = 0;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		logline("sve_fork_test: PASS -- child preserved d8-d15 across fork()\n");
		return 0;
	}
	logline("sve_fork_test: FAIL -- child did not preserve d8-d15 across fork()\n");
	return 1;
}
