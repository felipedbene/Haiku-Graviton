/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Measures memcpy() at the alignments and sizes the network receive path
	actually uses.

	Profiling receive on arm64 (see graviton/docs/net-receive-profile.md) put
	~78% of the CPU cost of a transfer in two byte-copies, at a combined ~4
	cycles per byte -- one to two orders of magnitude more than a copy should
	cost. This exists to say whether that is the memory system or the copy
	routine, which the profile alone cannot distinguish.

	The alignments are not arbitrary. A received TCP payload begins 54 bytes
	into the frame (14 ethernet + 20 IPv4 + 20 TCP), so a copy out of it has a
	source misaligned by 54 % 8 == 6 against a destination the application
	almost always got from malloc() or mmap() and is therefore 8- or
	page-aligned. arch/generic/generic_memcpy.c -- which is what arm64 uses,
	unlike x86_64 which has its own -- only takes its word-at-a-time path when
	source and destination share the same misalignment, and otherwise copies the
	whole region one byte at a time. That is the common case on receive, not a
	corner.

	The reference implementation below is deliberately the simplest thing that
	could replace it rather than anything clever: align the *destination*, then
	use unaligned 16-byte loads, unrolled. arm64 does unaligned loads and stores
	in hardware at little or no cost, so the alignment-matching requirement the
	generic routine inherits from architectures that do not is what turns the
	common case into a byte loop. Reading both numbers from one binary on one
	machine is the point -- it removes the memory system, the clock, and the
	compiler from the comparison.
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


/*!	Destination-aligning, unaligned-load reference copy.

	The only structural difference from generic_memcpy.c is that misalignment is
	absorbed on the load side instead of disqualifying the fast path, and that
	the body moves 32 bytes per iteration rather than 8 so there are enough
	loads in flight to cover memory latency.
*/
static void*
reference_memcpy(void* dest, const void* source, size_t count)
{
	uint8_t* d = (uint8_t*)dest;
	const uint8_t* s = (const uint8_t*)source;

	/* Align the destination: stores are the side that benefits, and the loads
	   no longer care. */
	while (count > 0 && ((uintptr_t)d & 7) != 0) {
		*d++ = *s++;
		count--;
	}

	while (count >= 32) {
		/* memcpy() rather than a cast dereference: the source may be
		   unaligned, and this is the portable way to spell "unaligned load"
		   that the compiler turns into plain ldr/ldp on arm64. */
		uint64_t a, b, c, e;
		memcpy(&a, s + 0, 8);
		memcpy(&b, s + 8, 8);
		memcpy(&c, s + 16, 8);
		memcpy(&e, s + 24, 8);
		*(uint64_t*)(d + 0) = a;
		*(uint64_t*)(d + 8) = b;
		*(uint64_t*)(d + 16) = c;
		*(uint64_t*)(d + 24) = e;
		d += 32;
		s += 32;
		count -= 32;
	}

	while (count >= 8) {
		uint64_t a;
		memcpy(&a, s, 8);
		*(uint64_t*)d = a;
		d += 8;
		s += 8;
		count -= 8;
	}

	while (count > 0) {
		*d++ = *s++;
		count--;
	}

	return dest;
}


typedef void* (*copy_func)(void*, const void*, size_t);


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


static void
row(const char* label, size_t size, int sourceOffset, int destOffset, int cold)
{
	const char* note = "";
	double generic = measure(memcpy, size, sourceOffset, destOffset, cold,
		&note);
	double reference = measure(reference_memcpy, size, sourceOffset, destOffset,
		cold, &note);

	printf("  %-26s %6zu %5d %5d %-5s  %8.3f %7.2f   %8.3f %7.2f  %6.2fx\n",
		label, size, sourceOffset, destOffset, cold ? "cold" : "warm",
		generic, generic * NOMINAL_GHZ, reference, reference * NOMINAL_GHZ,
		generic / reference);
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
	printf("  %-26s %6s %5s %5s %-5s  %8s %7s   %8s %7s  %6s\n",
		"case", "size", "s.off", "d.off", "temp",
		"ns/byte", "cyc/B", "ns/byte", "cyc/B", "ratio");
	printf("  %-26s %6s %5s %5s %-5s  %-16s   %-16s  %6s\n",
		"", "", "", "", "", "  libroot memcpy", "  reference", "");

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
