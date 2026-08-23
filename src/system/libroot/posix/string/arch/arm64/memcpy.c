/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <stdint.h>
#include <string.h>
#undef memcpy


/*!	An arm64 memcpy() that does not care whether its arguments agree in
	alignment.

	arm64 previously used string/arch/generic/generic_memcpy.c, which reaches its
	word-at-a-time loop only when source and destination are misaligned by the
	*same* amount, and copies the whole region one byte at a time otherwise. That
	condition exists for architectures on which an unaligned load traps or is
	microcoded. arm64 is not one of them: it performs unaligned loads and stores
	in hardware, so the requirement bought nothing here and cost a byte loop in
	the case that matters most.

	The case that matters most is measured, not assumed. A received TCP payload
	begins 54 bytes into the frame (14 ethernet + 20 IPv4 + 20 TCP), so copying
	it out to an application buffer obtained from malloc() puts a source at
	6 mod 8 against an aligned destination -- mismatched, hence the byte loop.
	Worse, the mismatch is self-sustaining across a fragmented buffer: once the
	first misaligned segment has advanced the destination out of phase, every
	following segment copy is mismatched too, even the ones whose source is
	perfectly aligned. On a c7g.large moving 4.9 Gbit/s that single effect was
	3.5 of the 19 microseconds of CPU spent per 9 KB frame; measured on hardware
	with tests/system/benchmarks/memcpybench.c, the mismatched case cost
	0.388 ns/byte against 0.074 ns/byte aligned -- a factor of 5.2 -- and this
	routine does 0.058 ns/byte at every alignment.

	So misalignment is absorbed on the load side, and only the destination is
	aligned, because stores are the side that benefits from it. The body moves 32
	bytes per iteration rather than 8 for a second reason the same measurements
	showed: reading memory a device has just written by DMA is a cache miss every
	time, and a loop with one load in flight is bounded by memory latency rather
	than bandwidth. Four independent loads per iteration give the core enough to
	overlap.
*/


/*!	Spells "load that may be unaligned" in a way the compiler turns into a plain
	ldr, rather than into a call back into memcpy() -- which is what
	__builtin_memcpy() would become here, since this file is compiled
	-fno-builtin, and would recurse forever.
*/
typedef uint64_t __attribute__((aligned(1))) unaligned_uint64;


void*
memcpy(void* dest, const void* source, size_t count)
{
	uint8_t* d = (uint8_t*)dest;
	const uint8_t* s = (const uint8_t*)source;

	/* Below a couple of words the alignment work costs more than it saves, and
	   short copies are much the most frequent kind. */
	if (count < 16) {
		while (count > 0) {
			*d++ = *s++;
			count--;
		}
		return dest;
	}

	/* Align the destination. count >= 16 above guarantees this cannot exhaust
	   it, so the loops below need no further guarding. */
	while (((uintptr_t)d & 7) != 0) {
		*d++ = *s++;
		count--;
	}

	while (count >= 32) {
		const uint64_t a = ((const unaligned_uint64*)s)[0];
		const uint64_t b = ((const unaligned_uint64*)s)[1];
		const uint64_t c = ((const unaligned_uint64*)s)[2];
		const uint64_t e = ((const unaligned_uint64*)s)[3];

		((uint64_t*)d)[0] = a;
		((uint64_t*)d)[1] = b;
		((uint64_t*)d)[2] = c;
		((uint64_t*)d)[3] = e;

		d += 32;
		s += 32;
		count -= 32;
	}

	while (count >= 8) {
		*(uint64_t*)d = *(const unaligned_uint64*)s;
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
