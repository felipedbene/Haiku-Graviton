/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Measures the memcpy() this binary is linked against, at the alignments and
	sizes the network receive path actually uses, against the generic routine
	arm64 used before.

	Profiling receive on arm64 (see graviton/docs/net-receive-profile.md) put
	~78% of the CPU cost of a transfer in two byte-copies, at a combined ~4
	cycles per byte -- one to two orders of magnitude more than a copy should
	cost. This exists to say whether that is the memory system or the copy
	routine, which the profile alone cannot distinguish.

	The alignments are not arbitrary. A received TCP payload begins 54 bytes
	into the frame (14 ethernet + 20 IPv4 + 20 TCP), so a copy out of it has a
	source misaligned by 54 % 8 == 6 against a destination the application
	almost always got from malloc() or mmap() and is therefore 8- or
	page-aligned. arch/generic/generic_memcpy.c only takes its word-at-a-time
	path when source and destination share the same misalignment, and otherwise
	copies the whole region one byte at a time. That is the common case on
	receive, not a corner.

	Two things about the shape of this file are deliberate, because the version
	it replaces got both wrong in ways that made its output misleading rather
	than merely incomplete.

	It measures the *real* memcpy(), through a volatile function pointer so the
	call cannot be inlined or constant-folded, against a verbatim transcription
	of the incumbent generic routine compiled into this same binary. The
	previous version measured a separate reference_memcpy() living in this file,
	which had drifted from the routine that actually shipped -- it lacked the
	count < 16 early-out and guarded its alignment loop differently, i.e. it
	differed in precisely the two places the shipped routine was non-obvious.
	Carrying the incumbent here instead means old and new are compared in one
	binary, on one boot, on one machine, which is what removes the memory
	system, the clock and the compiler from the comparison -- and it means the
	comparison does not silently become new-against-itself the moment the new
	routine ships.

	And verify() checks the real routine against a local byte-at-a-time oracle,
	not against memcpy(). A test whose oracle is the thing under test passes
	unconditionally. It also sweeps offsets 0..31 rather than 0..7, because the
	compiler widens the body to 128-bit ldr q/str q and the behaviour is
	therefore sensitive to the 16-byte phase, which offsets 0..7 never vary.

	The exhaustive correctness work lives in
	tests/system/libroot/posix/string/memcpy_test.c -- guard bands, unmapped
	pages either side, memmove and bcopy. verify() here is a fast gate so that a
	wrong routine cannot be reported as a fast one, since a copy that moves too
	few bytes is a *fast* copy and timing alone rewards it.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>


/* Big enough that a buffer pair cannot sit in any cache on this part, so the
   "cold" numbers describe reads that actually go to memory, as a received frame
   just written by the device does. */
#define BUFFER_SIZE			(64 * 1024 * 1024)
#define NOMINAL_GHZ			2.6		/* Neoverse V1 on c7g; for cycles/byte */


static uint8_t* sSource;
static uint8_t* sDest;


typedef void* (*copy_func)(void*, const void*, size_t);


/*!	The routine under test: whatever this binary's libroot supplies. Volatile so
	that the call is indirect and the compiler can neither inline it nor replace
	it with its own idea of a copy -- measuring GCC's inlined memcpy instead of
	libroot's is an easy mistake that leaves no trace in the output.
*/
static volatile copy_func sMemcpy = memcpy;


/*!	A verbatim transcription of arch/generic/generic_memcpy.c: the routine arm64
	used before, carried here so that old and new can be measured in one binary
	rather than across two images.
*/
static void*
legacy_memcpy(void* dest, const void* source, size_t count)
{
	uint8_t* d = (uint8_t*)dest;
	const uint8_t* s = (const uint8_t*)source;

	if (count == 0 || dest == source)
		return dest;

	if (((uintptr_t)d & (sizeof(size_t) - 1))
			== ((uintptr_t)s & (sizeof(size_t) - 1))) {
		while (count > 0 && ((uintptr_t)d & (sizeof(size_t) - 1)) != 0) {
			*d++ = *s++;
			count--;
		}

		while (count >= sizeof(size_t)) {
			*(size_t*)d = *(const size_t*)s;
			d += sizeof(size_t);
			s += sizeof(size_t);
			count -= sizeof(size_t);
		}
	}

	while (count > 0) {
		*d++ = *s++;
		count--;
	}

	return dest;
}


/*!	The oracle. A byte loop through volatile pointers, which is what stops the
	compiler recognising it and emitting a call to the routine being checked.
*/
static void*
oracle_memcpy(void* dest, const void* source, size_t count)
{
	volatile uint8_t* d = (volatile uint8_t*)dest;
	volatile const uint8_t* s = (volatile const uint8_t*)source;

	while (count > 0) {
		*d++ = *s++;
		count--;
	}

	return dest;
}


/*!	Checks the real memcpy() against the oracle at every alignment pair mod 32
	and at every size where its internal cases can meet.

	A faster copy that is wrong is worthless, and the failure mode of a
	block-at-a-time copy is an off-by-one where its head, body and tail cases
	join -- which timing alone would never catch, because a routine that copies
	too few bytes is a fast one. Sizes up to 336 are covered exhaustively, along
	with the sizes either side of each threshold, at all 1024 alignment pairs
	mod 32, and the bytes just outside the destination are checked too so that
	an overrun cannot pass.
*/
static int
verify(void)
{
	static const size_t kExtra[] = { 337, 383, 384, 385, 511, 512, 513,
		1023, 1024, 1025, 1447, 1448, 1449, 1919, 1920, 1921,
		4095, 4096, 4097, 8960, 8961, 8962, 65534, 65535 };
	const size_t kExtraCount = sizeof(kExtra) / sizeof(kExtra[0]);
	const size_t kGuard = 64;
	const size_t kMax = 65535;
	uint8_t* expected = (uint8_t*)malloc(kMax + 64 + 2 * kGuard);
	uint8_t* actual = (uint8_t*)malloc(kMax + 64 + 2 * kGuard);
	int failures = 0;

	if (expected == NULL || actual == NULL) {
		free(expected);
		free(actual);
		return -1;
	}

	for (size_t i = 0; i < kMax + 64; i++)
		sSource[i] = (uint8_t)(i * 31 + 7);

	for (size_t index = 0; index < 337 + kExtraCount; index++) {
		const size_t size = index < 337 ? index : kExtra[index - 337];
		const size_t span = size + 64 + 2 * kGuard;

		for (int sourceOffset = 0; sourceOffset < 32; sourceOffset++) {
			for (int destOffset = 0; destOffset < 32; destOffset++) {
				memset(expected, 0xcc, span);
				memset(actual, 0xcc, span);

				oracle_memcpy(expected + kGuard + destOffset,
					sSource + sourceOffset, size);
				if (sMemcpy(actual + kGuard + destOffset,
						sSource + sourceOffset, size)
					!= actual + kGuard + destOffset) {
					if (failures < 10) {
						printf("  BAD RETURN size %zu source+%d dest+%d\n",
							size, sourceOffset, destOffset);
					}
					failures++;
				}

				if (memcmp(expected, actual, span) != 0) {
					if (failures < 10) {
						printf("  MISMATCH size %zu source+%d dest+%d\n",
							size, sourceOffset, destOffset);
					}
					failures++;
				}
			}
		}
	}

	free(expected);
	free(actual);
	return failures;
}


/*!	Copies \a size bytes repeatedly, walking the whole buffer so that every
	read misses if \a cold is set, and returns nanoseconds per byte.
*/
static double
measure(copy_func copy, size_t size, int sourceOffset, int destOffset,
	int cold, const char** _note)
{
	/* Cold: stride through the whole 64 MiB so nothing is resident. Warm: sit
	   on one small region, which is the best case any copy routine can have and
	   so isolates the instruction cost from the memory cost. */
	const size_t span = cold ? BUFFER_SIZE - size - 64 : (256 * 1024);
	const size_t stride = size < 4096 ? 4096 : ((size + 4095) & ~(size_t)4095);
	size_t iterations = cold ? (span / stride) : 4096;
	if (iterations < 1)
		iterations = 1;

	/* Enough repeats that the timer's resolution cannot matter. */
	int rounds = 1;
	bigtime_t elapsed = 0;
	uint64_t bytes = 0;

	while (elapsed < 200000) {
		bigtime_t start = system_time();
		for (int round = 0; round < rounds; round++) {
			for (size_t i = 0; i < iterations; i++) {
				size_t offset = i * stride;
				copy(sDest + destOffset + offset,
					sSource + sourceOffset + offset, size);
			}
		}
		elapsed = system_time() - start;
		bytes = (uint64_t)size * iterations * rounds;
		if (elapsed >= 200000)
			break;
		rounds *= 4;
		if (rounds > (1 << 20)) {
			*_note = "timer never advanced enough";
			break;
		}
	}

	return elapsed * 1000.0 / (double)bytes;
}


/*!	Interleaves the two routines rather than running all of one and then all of
	the other, so that a drift in clock or thermal state over the run cannot be
	read as a difference between them. Reports the better of two alternations
	each way, which is the standard way to keep a single scheduling accident
	from becoming a result.
*/
static void
row(const char* label, size_t size, int sourceOffset, int destOffset, int cold)
{
	const char* note = "";
	double old1 = measure(legacy_memcpy, size, sourceOffset, destOffset, cold,
		&note);
	double new1 = measure(sMemcpy, size, sourceOffset, destOffset, cold, &note);
	double new2 = measure(sMemcpy, size, sourceOffset, destOffset, cold, &note);
	double old2 = measure(legacy_memcpy, size, sourceOffset, destOffset, cold,
		&note);
	double oldBest = old1 < old2 ? old1 : old2;
	double newBest = new1 < new2 ? new1 : new2;

	printf("  %-26s %6zu %5d %5d %-5s  %8.3f %7.2f   %8.3f %7.2f  %6.2fx\n",
		label, size, sourceOffset, destOffset, cold ? "cold" : "warm",
		oldBest, oldBest * NOMINAL_GHZ, newBest, newBest * NOMINAL_GHZ,
		oldBest / newBest);
}


int
main(void)
{
	sSource = (uint8_t*)malloc(BUFFER_SIZE + 4096);
	sDest = (uint8_t*)malloc(BUFFER_SIZE + 4096);
	if (sSource == NULL || sDest == NULL) {
		fprintf(stderr, "memcpybench: cannot allocate %d bytes twice\n",
			BUFFER_SIZE);
		return 1;
	}

	/* Touch both so the timings measure copying and not page faulting. */
	memset(sSource, 0xa5, BUFFER_SIZE + 4096);
	memset(sDest, 0x5a, BUFFER_SIZE + 4096);

	printf("memcpybench: %d MiB buffers, cycles at a nominal %.1f GHz\n",
		BUFFER_SIZE / (1024 * 1024), NOMINAL_GHZ);
	printf("\n");
	printf("correctness of this libroot's memcpy() against a byte-loop "
		"oracle:\n");
	int failures = verify();
	if (failures < 0) {
		printf("  could not allocate the comparison buffers\n");
		return 1;
	}
	printf("  %s (%d failures over 1024 alignment pairs mod 32 x 361 "
		"sizes)\n", failures == 0 ? "correct" : "WRONG", failures);
	if (failures > 0)
		return 1;

	/* The verify() pass filled the front of the source buffer with a pattern;
	   restore the whole thing so the timings below are not measuring a copy out
	   of a partly cold, partly hot region. */
	memset(sSource, 0xa5, BUFFER_SIZE + 4096);
	printf("\n");
	printf("  %-26s %6s %5s %5s %-5s  %8s %7s   %8s %7s  %6s\n",
		"case", "size", "s.off", "d.off", "temp",
		"ns/byte", "cyc/B", "ns/byte", "cyc/B", "ratio");
	printf("  %-26s %6s %5s %5s %-5s  %-16s   %-16s  %6s\n",
		"", "", "", "", "", "  old generic", "  this libroot", "");

	/* The two copies the receive path performs, at the offsets it performs
	   them at: driver DMA buffer -> net_buffer node (both 8-aligned), and
	   net_buffer payload -> application buffer (source off by 54 % 8). */
	puts("");
	puts("  the receive path's two copies");
	row("driver -> net_buffer", 1920, 0, 0, 1);
	row("net_buffer -> user", 1920, 6, 0, 1);
	row("net_buffer -> user, warm", 1920, 6, 0, 0);

	puts("");
	puts("  alignment sweep at a jumbo payload size");
	row("aligned", 8961, 0, 0, 1);
	row("source off 1", 8961, 1, 0, 1);
	row("source off 2", 8961, 2, 0, 1);
	row("source off 4", 8961, 4, 0, 1);
	row("source off 6 (TCP payload)", 8961, 6, 0, 1);
	row("both off 6 (same mod 8)", 8961, 6, 6, 1);
	row("dest off 6", 8961, 0, 6, 1);
	row("aligned, warm", 8961, 0, 0, 0);
	row("source off 6, warm", 8961, 6, 0, 0);

	/* Small sizes, warm, at matching alignment -- the case the previous version
	   of this file could not see, because its smallest size was 64 bytes and
	   its only misalignment was source-off-6. Matched-alignment short copies
	   are the case the generic routine handled *well*, so they are where a
	   replacement is most likely to regress, and the first version of the arm64
	   routine lost up to 50% here. */
	puts("");
	puts("  short copies at matching alignment (where a replacement regresses)");
	row("1 B", 1, 0, 0, 0);
	row("4 B", 4, 0, 0, 0);
	row("8 B", 8, 0, 0, 0);
	row("12 B", 12, 0, 0, 0);
	row("15 B", 15, 0, 0, 0);
	row("16 B", 16, 0, 0, 0);
	row("24 B", 24, 0, 0, 0);
	row("31 B", 31, 0, 0, 0);
	row("32 B", 32, 0, 0, 0);
	row("33 B", 33, 0, 0, 0);
	row("64 B", 64, 0, 0, 0);
	row("96 B", 96, 0, 0, 0);
	row("128 B", 128, 0, 0, 0);
	row("129 B", 129, 0, 0, 0);
	row("256 B", 256, 0, 0, 0);

	puts("");
	puts("  short copies at mismatched alignment");
	row("8 B", 8, 6, 0, 0);
	row("16 B", 16, 6, 0, 0);
	row("32 B", 32, 6, 0, 0);
	row("64 B", 64, 6, 0, 0);
	row("128 B", 128, 6, 0, 0);

	puts("");
	puts("  size sweep at the receive path's misalignment");
	row("64 B", 64, 6, 0, 1);
	row("256 B", 256, 6, 0, 1);
	row("1448 B (MTU 1500 payload)", 1448, 6, 0, 1);
	row("8961 B (MTU 9001 payload)", 8961, 6, 0, 1);
	row("65535 B (a socket read)", 65535, 6, 0, 1);

	free(sSource);
	free(sDest);
	return 0;
}
