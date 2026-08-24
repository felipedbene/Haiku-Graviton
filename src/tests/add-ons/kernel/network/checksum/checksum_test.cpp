/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Correctness and speed of the network stack's internet checksum.

	This file includes the shipping header directly:

		#include "checksum.h"

	so the routine verified here is the routine that runs in the kernel, not a
	transcription of it. That is deliberate and is the whole point of checksum.h
	existing -- a sibling effort on this tree lost its entire evidentiary basis to
	a harness that benchmarked a copy which differed from the shipping code in
	exactly the interesting place. Do not "simplify" this by pasting the loop in.

	Builds two ways from the same source:

	  Haiku, in-tree:  jam -q net_checksum_test
	  host, standalone: c++ -O2 -I shim -I ../../../../../add-ons/kernel/network/stack \
	                        -o checksum_test checksum_test.cpp

	Run with no arguments to verify; `--bench` to measure.
*/


#include "checksum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


static int sFailures = 0;
static long sChecks = 0;


// #pragma mark - oracles


/*!	Independent reference, written from the contract in checksum.h rather than
	from either implementation: assemble each 16-bit host-order word explicitly
	from its two bytes, accumulate wide, fold. Structurally shares nothing with
	the routine under test -- no casts, no wide loads, no unrolling -- so an error
	common to both is unlikely.
*/
static uint16
oracle_naive(const uint8* p, size_t length)
{
	uint64 sum = 0;
	size_t i = 0;

	while (i + 2 <= length) {
#if B_HOST_IS_LENDIAN
		sum += (uint16)p[i] | ((uint16)p[i + 1] << 8);
#else
		sum += ((uint16)p[i] << 8) | (uint16)p[i + 1];
#endif
		i += 2;
	}

	if (i < length) {
#if B_HOST_IS_LENDIAN
		sum += p[i];
#else
		sum += (uint16)p[i] << 8;
#endif
	}

	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);

	return (uint16)sum;
}


/*!	The implementation this replaced, verbatim, as it stood in utility.cpp before
	checksum.h existed. Kept only so the change can be shown to be behaviour
	preserving. Note the uint32 accumulator: it wraps after 131076 bytes, which is
	the one input class where this oracle is wrong and the new routine is right.
*/
static uint16
oracle_original(uint8* _buffer, size_t length)
{
	uint16* buffer = (uint16*)_buffer;
	uint32 sum = 0;

	while (length >= 2) {
		sum += *buffer++;
		length -= 2;
	}

	if (length) {
#if B_HOST_IS_LENDIAN
		sum += *(uint8*)buffer;
#else
		uint8 ordered[2];
		ordered[0] = *(uint8*)buffer;
		ordered[1] = 0;
		sum += *(uint16*)ordered;
#endif
	}

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return sum;
}


// #pragma mark - helpers


static void
fail(const char* what, size_t length, size_t alignment, const char* pattern,
	unsigned got, unsigned expected)
{
	if (sFailures < 40) {
		printf("FAIL %-22s len=%-7zu align=%-3zu pattern=%-10s "
			"got=0x%04x want=0x%04x\n", what, length, alignment, pattern, got,
			expected);
	}
	sFailures++;
}


enum pattern {
	kZero, kOnes, kAlternating, kHighBit, kCounting, kRandom, kPatternCount
};

static const char* kPatternNames[]
	= { "zero", "ones", "alt", "highbit", "counting", "random" };


static void
fill(uint8* p, size_t length, int pattern, unsigned seed)
{
	for (size_t i = 0; i < length; i++) {
		switch (pattern) {
			case kZero:			p[i] = 0x00; break;
			case kOnes:			p[i] = 0xff; break;
			case kAlternating:	p[i] = (i & 1) ? 0x00 : 0xff; break;
			case kHighBit:		p[i] = 0x80; break;
			case kCounting:		p[i] = (uint8)(i * 31 + 7); break;
			default:
				seed = seed * 1103515245 + 12345;
				p[i] = (uint8)(seed >> 16);
				break;
		}
	}
}


// #pragma mark - tests


/*!	Every length against every alignment against every content pattern, compared
	against both oracles. Carry propagation is what the "ones" and "highbit"
	patterns are for: 0xffff words make every addition carry, which is where a
	hand-folded one's complement sum goes wrong.
*/
static void
test_exhaustive(size_t maxLength)
{
	const size_t kMaxAlignment = 32;
	uint8* block = (uint8*)malloc(maxLength + kMaxAlignment + 8);

	for (int pattern = 0; pattern < kPatternCount; pattern++) {
		for (size_t alignment = 0; alignment < kMaxAlignment; alignment++) {
			uint8* p = block + alignment;
			fill(p, maxLength, pattern, (unsigned)(alignment * 7919 + 1));

			for (size_t length = 0; length <= maxLength; length++) {
				uint16 got = net_checksum_compute(p, length);
				uint16 want = oracle_naive(p, length);
				sChecks++;

				if (got != want) {
					fail("vs naive oracle", length, alignment,
						kPatternNames[pattern], got, want);
					continue;
				}

				uint16 old = oracle_original(p, length);
				if (got != old) {
					fail("vs original", length, alignment,
						kPatternNames[pattern], got, old);
				}
			}
		}
	}

	free(block);
}


/*!	Lengths straddling every unroll boundary, out to well past any real frame.
	test_exhaustive() already covers 0..1024 densely; this is about the 32-byte
	and 8-byte remainder paths repeating correctly over a long buffer.
*/
static void
test_unroll_boundaries(void)
{
	const size_t kMax = 70000;
	uint8* block = (uint8*)malloc(kMax + 40);

	for (int pattern = 0; pattern < kPatternCount; pattern++) {
		fill(block, kMax + 8, pattern, 4242);

		for (size_t base = 0; base <= kMax; base += 8) {
			for (int delta = -3; delta <= 3; delta++) {
				if ((ssize_t)base + delta < 0)
					continue;
				size_t length = base + delta;
				if (length > kMax)
					continue;

				for (size_t alignment = 0; alignment < 9; alignment++) {
					uint8* p = block + alignment;
					uint16 got = net_checksum_compute(p, length);
					uint16 want = oracle_naive(p, length);
					sChecks++;
					if (got != want) {
						fail("boundary", length, alignment,
							kPatternNames[pattern], got, want);
					}
				}
			}
		}
	}

	free(block);
}


/*!	The composition property checksum_data() in net_buffer.cpp depends on.

	It sums per-node results and byte-swaps the folded result of any node
	beginning at an odd offset. Reproduced here exactly, and checked against the
	whole-buffer result for every possible pair and triple of split points --
	including odd ones, which are the case that breaks word parity and the reason
	the swap is there at all. A change to the folding or complementing convention
	in checksum.h passes the tests above and fails this one.
*/
static void
test_node_composition(void)
{
	const size_t kSize = 601;			// odd on purpose
	uint8* block = (uint8*)malloc(kSize);

	for (int pattern = 0; pattern < kPatternCount; pattern++) {
		fill(block, kSize, pattern, 31337);
		const uint16 whole = net_checksum_compute(block, kSize);

		// two nodes
		for (size_t split = 0; split <= kSize; split++) {
			uint32 sum = net_checksum_compute(block, split);
			size_t rest = kSize - split;
			if (rest > 0) {
				uint16 second = net_checksum_compute(block + split, rest);
				sum += (split & 1) != 0 ? __swap_int16(second) : second;
			}
			while (sum >> 16)
				sum = (sum & 0xffff) + (sum >> 16);

			sChecks++;
			if ((uint16)sum != whole) {
				fail("2-node compose", split, 0, kPatternNames[pattern],
					(uint16)sum, whole);
			}
		}

		// three nodes, so that a node both starts and ends at an odd offset
		for (size_t a = 0; a <= kSize; a += 7) {
			for (size_t b = a; b <= kSize; b += 5) {
				uint32 sum = net_checksum_compute(block, a);

				if (b > a) {
					uint16 mid = net_checksum_compute(block + a, b - a);
					sum += (a & 1) != 0 ? __swap_int16(mid) : mid;
				}
				if (kSize > b) {
					uint16 last = net_checksum_compute(block + b, kSize - b);
					sum += (b & 1) != 0 ? __swap_int16(last) : last;
				}

				while (sum >> 16)
					sum = (sum & 0xffff) + (sum >> 16);

				sChecks++;
				if ((uint16)sum != whole) {
					fail("3-node compose", a * 1000 + b, 0,
						kPatternNames[pattern], (uint16)sum, whole);
				}
			}
		}
	}

	free(block);
}


/*!	Fixed known answer, so that a change of convention cannot pass by being
	self-consistent. The vector is RFC 1071 section 3's worked example: the bytes
	00 01 f2 03 f4 f5 f6 f7 have one's complement sum 0xddf2 and checksum 0x220d
	in network order. This routine sums host-order words, so on a little-endian
	host it must return that sum byte-swapped.
*/
static void
test_rfc1071_vector(void)
{
	static const uint8 kVector[]
		= { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };

#if B_HOST_IS_LENDIAN
	const uint16 kExpectedSum = 0xf2dd;			// 0xddf2 byte-swapped
#else
	const uint16 kExpectedSum = 0xddf2;
#endif

	uint16 got = net_checksum_compute(kVector, sizeof(kVector));
	sChecks++;
	if (got != kExpectedSum)
		fail("RFC 1071 vector", sizeof(kVector), 0, "-", got, kExpectedSum);

	// And the complemented form the wire carries, which is what checksum()
	// returns: byte-swapped 0x220d on a little-endian host.
	uint16 wire = (uint16)~got;
#if B_HOST_IS_LENDIAN
	const uint16 kExpectedWire = 0x0d22;
#else
	const uint16 kExpectedWire = 0x220d;
#endif
	sChecks++;
	if (wire != kExpectedWire)
		fail("RFC 1071 wire", sizeof(kVector), 0, "-", wire, kExpectedWire);
}


/*!	Above 131076 bytes the old uint32 accumulator wrapped. Nothing in the stack
	can pass a buffer that long -- a net_buffer is capped far below it -- so this
	documents rather than guards: the new routine must agree with the reference
	where the old one no longer does.
*/
static void
test_beyond_old_overflow(void)
{
	const size_t kSize = 200000;
	uint8* block = (uint8*)malloc(kSize);
	fill(block, kSize, kOnes, 1);

	for (size_t length = 131000; length <= 131200; length++) {
		uint16 got = net_checksum_compute(block, length);
		uint16 want = oracle_naive(block, length);
		sChecks++;
		if (got != want)
			fail("past 128K", length, 0, "ones", got, want);
	}

	uint16 got = net_checksum_compute(block, kSize);
	uint16 want = oracle_naive(block, kSize);
	sChecks++;
	if (got != want)
		fail("200000 bytes", kSize, 0, "ones", got, want);

	uint16 old = oracle_original(block, kSize);
	printf("  note: at %zu bytes of 0xff the new routine and the reference agree "
		"on 0x%04x;\n        the replaced implementation returned 0x%04x "
		"(uint32 accumulator wrap)%s\n", kSize, got, old,
		old == got ? " -- no divergence at this length" : "");

	free(block);
}


// #pragma mark - benchmark


static double
now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}


static void
benchmark(void)
{
	// 1988 is the interesting one in practice: a net_buffer's payload is carved
	// from 2048-byte buffers, so checksum_data() calls this once per data_node
	// with roughly that much, not once per frame. 1448 and 8949 are the MTU 1500
	// and 9001 maximum segment sizes; 64 is a bare ACK.
	static const size_t kLengths[] = { 64, 512, 1448, 1988, 8949, 65495 };
	const int kRounds = 5;

	printf("%-8s %-10s %14s %14s %9s\n", "length", "alignment", "replaced ns/B",
		"current ns/B", "speedup");

	for (size_t i = 0; i < sizeof(kLengths) / sizeof(kLengths[0]); i++) {
		const size_t length = kLengths[i];
		const int iterations = (int)(400000000 / (length + 64));

		for (size_t alignment = 0; alignment <= 1; alignment++) {
			uint8* block = (uint8*)malloc(length + 64);
			uint8* p = block + alignment;
			fill(p, length, kCounting, 7);

			volatile uint16 sink = 0;
			double bestOld = 1e30, bestNew = 1e30;

			// Both routines are pure and the arguments are loop invariant, so a
			// compiler is entitled to compute either one once and reuse it. The
			// buffer is therefore mutated every iteration, which it cannot see
			// through. Without this the measurement is of an empty loop.
			// (It is not the reason the numbers look good -- 0.05 ns/byte is
			// 8 bytes/cycle, which is one 64-bit load and two adds per cycle and
			// well inside what the core can retire -- but "the compiler deleted
			// the work" is not a hypothesis a benchmark should leave open.)
			for (int round = 0; round < kRounds; round++) {
				double t0 = now();
				for (int n = 0; n < iterations; n++) {
					p[n & 63] = (uint8)n;
					sink = (uint16)(sink ^ oracle_original(p, length));
				}
				double t1 = now();
				for (int n = 0; n < iterations; n++) {
					p[n & 63] = (uint8)n;
					sink = (uint16)(sink ^ net_checksum_compute(p, length));
				}
				double t2 = now();

				double old = (t1 - t0) / iterations / length * 1e9;
				double cur = (t2 - t1) / iterations / length * 1e9;
				if (old < bestOld)
					bestOld = old;
				if (cur < bestNew)
					bestNew = cur;
			}

			printf("%-8zu %-10zu %14.4f %14.4f %8.2fx\n", length, alignment,
				bestOld, bestNew, bestOld / bestNew);
			free(block);
		}
	}
}


// #pragma mark -


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
		benchmark();
		return 0;
	}

	printf("verifying net_checksum_compute() from checksum.h\n");
	printf("  host is %s-endian\n", B_HOST_IS_LENDIAN ? "little" : "big");

	printf("- exhaustive: lengths 0..1024 x alignments 0..31 x %d patterns\n",
		(int)kPatternCount);
	test_exhaustive(1024);

	printf("- unroll boundaries out to 70000 bytes\n");
	test_unroll_boundaries();

	printf("- node composition (the checksum_data() contract)\n");
	test_node_composition();

	printf("- RFC 1071 known answer\n");
	test_rfc1071_vector();

	printf("- lengths past the replaced implementation's overflow\n");
	test_beyond_old_overflow();

	printf("\n%ld checks, %d failures\n", sChecks, sFailures);
	if (sFailures != 0) {
		printf("FAILED\n");
		return 1;
	}

	printf("PASSED\n");
	return 0;
}
