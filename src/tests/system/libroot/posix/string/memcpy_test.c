/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	A correctness test for memcpy(), memmove() and bcopy().

	Written because there was not one. Before this file,
	src/tests/system/libroot/posix/string/ contained a single 496-byte program
	that exercised the *comparison* functions on one byte of input and printed
	its results instead of asserting them, and nothing anywhere in src/tests/
	made any claim about a copy at all. An architecture-specific memcpy is
	therefore, at the moment it is written, entirely untested by the tree it
	ships in.

	Three properties of this file matter more than its size.

	The oracle is local. It is a byte-at-a-time loop through volatile pointers,
	so it cannot be recognised as a copy and turned into a call to the routine
	under test, and it does not become the routine under test when the routine
	under test is replaced. A test whose oracle is memcpy() proves nothing about
	memcpy() -- it passes unconditionally and prints that it compared many
	cases, which is worse than failing.

	It fails, rather than printing. Every check contributes to an exit status.

	And it tests the two things a block-at-a-time copy gets wrong that a
	same-length comparison cannot see: writing outside the destination, and
	reading or writing *ahead of* the length. The first is caught by poisoned
	guard bands. The second is caught by placing the copy against an unmapped
	page, which is the only arrangement in which a routine that reads eight or
	sixteen bytes to satisfy the last three of them is distinguishable from a
	correct one -- and, being faster, it is the failure that timing rewards.
*/


#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>


/*!	The routine under test. Named through a macro so that the same source can be
	built against libroot's memcpy (the default) or against a differently
	compiled copy of the same algorithm linked in beside it -- in particular the
	kernel's, whose flags differ enough to produce different instructions from
	the same C.
*/
#ifndef COPY_UNDER_TEST
#	define COPY_UNDER_TEST memcpy
#endif

#define STRINGIFY_(x)	#x
#define STRINGIFY(x)	STRINGIFY_(x)
#define COPY_UNDER_TEST_NAME	STRINGIFY(COPY_UNDER_TEST)

extern void* COPY_UNDER_TEST(void* dest, const void* source, size_t count);


typedef void* (*copy_func)(void*, const void*, size_t);

/*!	Volatile, so the call is indirect and the compiler cannot inline the routine
	under test, constant-fold it, or replace it with its own idea of a copy.
	Testing the compiler's memcpy instead of libroot's is an easy mistake to make
	and leaves no trace in the output.
*/
static volatile copy_func sCopy = COPY_UNDER_TEST;

/* Defined with the page-boundary tests below, which is where the fault
   machinery belongs; declared here because the read-only self-copy test needs
   it too. */
static void fault_handler(int signal);
static int copy_may_fault(void* dest, const void* source, size_t size);

static int sFailures = 0;
static int sChecks = 0;
static int sReported = 0;
static const int kMaxReported = 25;


static void
fail(const char* format, ...)
{
	va_list args;

	sFailures++;
	if (sReported >= kMaxReported) {
		if (sReported == kMaxReported)
			printf("    ... further failures suppressed\n");
		sReported++;
		return;
	}
	sReported++;

	printf("    FAIL: ");
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	printf("\n");
}


// #pragma mark - oracles


/*!	The reference copy. Byte at a time through volatile pointers, which is what
	stops GCC's loop-distribution pass recognising the loop and emitting a call
	to memcpy -- i.e. to the very routine being checked.
*/
static void*
oracle_copy(void* dest, const void* source, size_t count)
{
	volatile unsigned char* d = (volatile unsigned char*)dest;
	volatile const unsigned char* s = (volatile const unsigned char*)source;

	while (count > 0) {
		*d++ = *s++;
		count--;
	}

	return dest;
}


/*!	The reference move: correct for every overlap, by choosing the direction. */
static void*
oracle_move(void* dest, const void* source, size_t count)
{
	volatile unsigned char* d = (volatile unsigned char*)dest;
	volatile const unsigned char* s = (volatile const unsigned char*)source;

	if (d == s || count == 0)
		return dest;

	if (d < s) {
		while (count > 0) {
			*d++ = *s++;
			count--;
		}
	} else {
		d += count;
		s += count;
		while (count > 0) {
			*--d = *--s;
			count--;
		}
	}

	return dest;
}


/*!	A faithful transcription of arch/generic/generic_memcpy.c, which is what
	arm64 used before. Kept only so that the change in behaviour on overlapping
	ranges can be reported as measured fact rather than argued about; memcpy()
	on overlapping ranges is undefined, so a difference here is not a failure.
*/
static void*
legacy_copy(void* dest, const void* source, size_t count)
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


// #pragma mark - the size and alignment sweep


#define kGuard		64u
#define kPoison		0xcdu
#define kMaxSize	(1u << 16)

static unsigned char* sSourceArena;
static unsigned char* sDestArena;
static unsigned char* sExpected;


/*!	Fills with a pattern in which every byte position has a distinct-ish value,
	so that a copy displaced by a few bytes does not accidentally compare equal.
*/
static void
fill_pattern(unsigned char* buffer, size_t size, unsigned seed)
{
	size_t i;
	uint32_t x = seed * 2654435761u + 1u;

	for (i = 0; i < size; i++) {
		x = x * 1103515245u + 12345u;
		buffer[i] = (unsigned char)((x >> 16) ^ i);
	}
}


/*!	One case: copy \a size bytes from \a sourceOffset to \a destOffset within
	arenas aligned well past any alignment the routine could care about, with
	poisoned guard bands either side of the destination.

	The guard check is the point. A same-length memcmp cannot see a routine that
	rounds its length up, and rounding the length up is exactly what a
	block-at-a-time copy does when it gets its tail wrong.
*/
static void
check_case(size_t size, size_t sourceOffset, size_t destOffset)
{
	unsigned char* source = sSourceArena + sourceOffset;
	unsigned char* dest = sDestArena + kGuard + destOffset;
	void* returned;
	size_t i;

	sChecks++;

	memset(sDestArena, kPoison, kGuard + destOffset + size + kGuard);
	oracle_copy(sExpected, source, size);

	returned = sCopy(dest, source, size);

	if (returned != dest) {
		fail("size %zu s+%zu d+%zu: returned %p, expected %p (memcpy must "
			"return its destination)", size, sourceOffset, destOffset,
			returned, (void*)dest);
	}

	if (size != 0 && memcmp(dest, sExpected, size) != 0) {
		size_t first = 0;
		while (first < size && dest[first] == sExpected[first])
			first++;
		fail("size %zu s+%zu d+%zu: content differs, first at byte %zu "
			"(got 0x%02x want 0x%02x)", size, sourceOffset, destOffset, first,
			dest[first], sExpected[first]);
	}

	/* Not one byte outside [dest, dest + size). */
	for (i = 0; i < kGuard + destOffset; i++) {
		if (sDestArena[i] != kPoison) {
			fail("size %zu s+%zu d+%zu: wrote %zu bytes BEFORE the destination "
				"(byte at -%zu is 0x%02x)", size, sourceOffset, destOffset,
				kGuard + destOffset - i, kGuard + destOffset - i,
				sDestArena[i]);
			break;
		}
	}
	for (i = 0; i < kGuard; i++) {
		if (dest[size + i] != kPoison) {
			fail("size %zu s+%zu d+%zu: wrote %zu bytes PAST the destination "
				"(byte at +%zu is 0x%02x)", size, sourceOffset, destOffset,
				i + 1, i, dest[size + i]);
			break;
		}
	}
}


static void
test_sizes_and_alignments(void)
{
	/* Sizes either side of every threshold in the arm64 routine (16 for the
	   short-copy cutoff, 32 for the block body, 8 for the word tail), either
	   side of a page, and a few large ones. */
	static const size_t kExtra[] = {
		257, 258, 263, 264, 265, 271, 272, 288, 289,
		511, 512, 513, 1023, 1024, 1025,
		1447, 1448, 1449, 1919, 1920, 1921,
		4095, 4096, 4097, 8191, 8192, 8193,
		8960, 8961, 8962, 16383, 16384, 16385,
		65534, 65535
	};
	const size_t kExtraCount = sizeof(kExtra) / sizeof(kExtra[0]);
	size_t index;

	printf("  size 0..256 exhaustively + %zu boundary sizes, x 64 alignment "
		"pairs (mod 8)\n", kExtraCount);

	for (index = 0; index < 257 + kExtraCount; index++) {
		const size_t size = index < 257 ? index : kExtra[index - 257];
		size_t sourceOffset, destOffset;

		for (sourceOffset = 0; sourceOffset < 8; sourceOffset++) {
			for (destOffset = 0; destOffset < 8; destOffset++)
				check_case(size, sourceOffset, destOffset);
		}
	}

	/* All 4096 alignment pairs mod 64, over a smaller size set. A routine that
	   assumes 16-, 32- or cache-line alignment somewhere -- and the compiler
	   does emit 16-byte ldp/stp and ldr/str q for this source -- cannot hide
	   from this, and a sweep of only the 64 pairs mod 8 would not find it. */
	printf("  4096 alignment pairs (mod 64) x 40 sizes\n");
	for (index = 0; index < 40; index++) {
		static const size_t kSizes[40] = {
			0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 23, 24, 25, 31, 32, 33,
			39, 40, 41, 47, 48, 49, 55, 56, 57, 63, 64, 65, 71, 72,
			95, 96, 97, 127, 128, 129, 191, 192, 255, 256
		};
		const size_t size = kSizes[index];
		size_t sourceOffset, destOffset;

		for (sourceOffset = 0; sourceOffset < 64; sourceOffset++) {
			for (destOffset = 0; destOffset < 64; destOffset++)
				check_case(size, sourceOffset, destOffset);
		}
	}
}


static void
test_degenerate(void)
{
	unsigned char byte = 0x5a;
	unsigned char other = 0xa5;
	void* returned;

	printf("  degenerate arguments\n");

	/* A zero count must touch nothing and must still return the destination.
	   Passing pointers that are valid but must not be read or written. */
	sChecks++;
	returned = sCopy(&byte, &other, 0);
	if (returned != &byte)
		fail("count 0: returned %p, expected %p", returned, (void*)&byte);
	if (byte != 0x5a)
		fail("count 0: wrote to the destination anyway (0x%02x)", byte);

	/* dest == source. Undefined by the letter of the standard for memcpy, but
	   the previous routine special-cased it and callers exist, so it must at
	   least not corrupt anything. */
	sChecks++;
	memset(sDestArena, 0x11, 512);
	returned = sCopy(sDestArena, sDestArena, 512);
	if (returned != sDestArena)
		fail("dest == source: returned %p", returned);
	{
		size_t i;
		for (i = 0; i < 512; i++) {
			if (sDestArena[i] != 0x11) {
				fail("dest == source: corrupted byte %zu (0x%02x)", i,
					sDestArena[i]);
				break;
			}
		}
	}

	sChecks++;
	returned = sCopy(&byte, &byte, 1);
	if (returned != &byte || byte != 0x5a)
		fail("single byte to itself: returned %p, byte 0x%02x", returned, byte);
}


/*!	memcpy(p, p, n) where p is read-only.

	Undefined by the letter of the standard, since the ranges overlap
	completely. But the generic routine short-circuited dest == source and
	therefore never wrote, so a routine that drops the short-circuit and writes
	unconditionally turns a working call into a fault -- and this routine is
	also the kernel's memcpy and user_memcpy(), where the fault is a KDL panic
	rather than a signal. Cheap to keep working, expensive to discover.
*/
static void
test_self_copy_read_only(void)
{
	const size_t pageSize = (size_t)sysconf(_SC_PAGESIZE);
	struct sigaction action, oldSegv, oldBus;
	unsigned char* page;

	printf("  memcpy(p, p, n) on a read-only mapping\n");

	page = (unsigned char*)mmap(NULL, pageSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		fail("mmap failed: %s", strerror(errno));
		return;
	}
	fill_pattern(page, pageSize, 5);

	if (mprotect(page, pageSize, PROT_READ) != 0) {
		fail("mprotect(PROT_READ) failed: %s", strerror(errno));
		munmap(page, pageSize);
		return;
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = fault_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, &oldSegv);
	sigaction(SIGBUS, &action, &oldBus);

	/* A size in each of the routine's internal cases. */
	{
		static const size_t kSizes[] = { 1, 4, 8, 16, 32, 33, 64, 128, 129,
			256, 1024 };
		size_t i;

		for (i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
			sChecks++;
			if (copy_may_fault(page, page, kSizes[i])) {
				fail("memcpy(p, p, %zu) on a read-only page FAULTED -- the "
					"dest == source short-circuit is missing", kSizes[i]);
			}
		}
	}

	/* The negative control for this subtest: writing to a different offset in
	   the same read-only page must fault, or the page was never made read-only
	   and the passes above mean nothing. */
	sChecks++;
	if (!copy_may_fault(page + 64, page, 32)) {
		fail("a write to the read-only page did NOT fault -- this subtest "
			"proved nothing");
	}

	sigaction(SIGSEGV, &oldSegv, NULL);
	sigaction(SIGBUS, &oldBus, NULL);
	munmap(page, pageSize);
}


static void
test_large(void)
{
	const size_t kSize = 4u * 1024 * 1024;
	unsigned char* source = (unsigned char*)malloc(kSize + 64);
	unsigned char* dest = (unsigned char*)malloc(kSize + 64);
	size_t offset;

	printf("  4 MiB copies at 8 source offsets\n");

	if (source == NULL || dest == NULL) {
		fail("could not allocate the 4 MiB buffers");
		free(source);
		free(dest);
		return;
	}

	fill_pattern(source, kSize + 64, 99);

	for (offset = 0; offset < 8; offset++) {
		size_t i;
		sChecks++;
		memset(dest, kPoison, kSize + 64);
		sCopy(dest, source + offset, kSize);
		for (i = 0; i < kSize; i++) {
			if (dest[i] != source[offset + i]) {
				fail("4 MiB copy at s+%zu: byte %zu differs", offset, i);
				break;
			}
		}
		if (dest[kSize] != kPoison)
			fail("4 MiB copy at s+%zu: wrote past the destination", offset);
	}

	free(source);
	free(dest);
}


// #pragma mark - reading or writing ahead of the length


/*!	Three pages, of which only the middle one is accessible. A copy arranged to
	end exactly at the last byte of the middle page faults if and only if the
	routine touches memory beyond its length; a copy arranged to start exactly
	at the first byte faults if and only if it touches memory before its start.

	This is the only test here that can distinguish a correct routine from one
	that reads sixteen bytes to satisfy the last three -- and since reading
	sixteen bytes once is faster than reading three bytes three times, it is a
	mistake that every measurement rewards.
*/
typedef struct {
	unsigned char*	base;		/* the whole three-page mapping */
	unsigned char*	page;		/* the accessible middle page */
	size_t			pageSize;
} guarded_pages;


static sigjmp_buf sFaultJump;
static volatile sig_atomic_t sFaultExpected = 0;
static volatile sig_atomic_t sFaulted = 0;


static void
fault_handler(int signal)
{
	(void)signal;
	if (sFaultExpected) {
		sFaulted = 1;
		siglongjmp(sFaultJump, 1);
	}
	/* Not ours; let the default action happen. */
	_exit(99);
}


static int
make_guarded_pages(guarded_pages* pages)
{
	const size_t pageSize = (size_t)sysconf(_SC_PAGESIZE);
	unsigned char* base = (unsigned char*)mmap(NULL, pageSize * 3,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (base == MAP_FAILED) {
		fail("mmap of three pages failed: %s", strerror(errno));
		return -1;
	}

	if (mprotect(base, pageSize, PROT_NONE) != 0
		|| mprotect(base + pageSize * 2, pageSize, PROT_NONE) != 0) {
		fail("mprotect(PROT_NONE) on the guard pages failed: %s",
			strerror(errno));
		munmap(base, pageSize * 3);
		return -1;
	}

	pages->base = base;
	pages->page = base + pageSize;
	pages->pageSize = pageSize;
	return 0;
}


/*!	Runs one copy with the fault handler armed. Returns 1 if it faulted. */
static int
copy_may_fault(void* dest, const void* source, size_t size)
{
	sFaulted = 0;
	sFaultExpected = 1;

	if (sigsetjmp(sFaultJump, 1) == 0)
		sCopy(dest, source, size);

	sFaultExpected = 0;
	return sFaulted ? 1 : 0;
}


static void
test_page_boundaries(void)
{
	guarded_pages source, dest;
	struct sigaction action, oldSegv, oldBus;
	size_t size;
	int faults = 0;

	printf("  copies ending at, and starting at, a page whose neighbour is "
		"unmapped\n");

	if (make_guarded_pages(&source) != 0)
		return;
	if (make_guarded_pages(&dest) != 0) {
		munmap(source.base, source.pageSize * 3);
		return;
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = fault_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, &oldSegv);
	sigaction(SIGBUS, &action, &oldBus);

	fill_pattern(source.page, source.pageSize, 7);

	/* Sizes 1..384 walk every alignment mod 64 six times over, and put the
	   boundary inside the head, the 32-byte body, the 8-byte tail and the byte
	   tail in turn. The interior buffer is offset eight ways so that the
	   mismatched-alignment path -- the one this routine changed -- is the one
	   under test as often as the matched one. */
	for (size = 1; size <= 384; size++) {
		size_t offset;

		for (offset = 0; offset < 8; offset++) {
			unsigned char* srcEnd = source.page + source.pageSize - size;
			unsigned char* dstEnd = dest.page + dest.pageSize - size;
			unsigned char* srcMid = source.page + 512 + offset;
			unsigned char* dstMid = dest.page + 512 + offset;

			/* (a) the SOURCE ends at the boundary: catches reading ahead. */
			sChecks++;
			if (copy_may_fault(dstMid, srcEnd, size)) {
				fail("READ PAST THE SOURCE: size %zu, source ends at the last "
					"byte of a mapped page, dest at +%zu", size, offset);
				faults++;
			}

			/* (b) the DESTINATION ends at the boundary: catches writing
			   ahead. */
			sChecks++;
			if (copy_may_fault(dstEnd, srcMid, size)) {
				fail("WROTE PAST THE DESTINATION: size %zu, dest ends at the "
					"last byte of a mapped page, source at +%zu", size, offset);
				faults++;
			}

			/* (c) the SOURCE starts at the boundary: catches reading behind. */
			sChecks++;
			if (copy_may_fault(dstMid, source.page, size)) {
				fail("READ BEFORE THE SOURCE: size %zu, source starts at the "
					"first byte of a mapped page, dest at +%zu", size, offset);
				faults++;
			}

			/* (d) the DESTINATION starts at the boundary: writing behind. */
			sChecks++;
			if (copy_may_fault(dest.page, srcMid, size)) {
				fail("WROTE BEFORE THE DESTINATION: size %zu, dest starts at "
					"the first byte of a mapped page, source at +%zu", size,
					offset);
				faults++;
			}

			if (faults > 8)
				goto done;
		}
	}

done:
	/* Confirm the guard pages really do fault, so that a run in which
	   mprotect() silently did nothing cannot be mistaken for a pass. This is
	   the negative control for this test. */
	sChecks++;
	if (!copy_may_fault(dest.page, source.base, 8)) {
		fail("the PROT_NONE guard page did NOT fault -- this test proved "
			"nothing and its passes must be discarded");
	}

	sigaction(SIGSEGV, &oldSegv, NULL);
	sigaction(SIGBUS, &oldBus, NULL);
	munmap(source.base, source.pageSize * 3);
	munmap(dest.base, dest.pageSize * 3);
}


// #pragma mark - memmove and bcopy


static void
test_memmove_and_bcopy(void)
{
	const size_t kArena = 1024;
	unsigned char* arena = (unsigned char*)malloc(kArena + 2 * kGuard);
	unsigned char* expected = (unsigned char*)malloc(kArena);
	unsigned char* pattern = (unsigned char*)malloc(kArena);
	size_t size;

	printf("  memmove/bcopy over every overlap in -256..256, sizes 0..256\n");

	if (arena == NULL || expected == NULL || pattern == NULL) {
		fail("could not allocate the memmove arena");
		free(arena);
		free(expected);
		free(pattern);
		return;
	}

	fill_pattern(pattern, kArena, 31);

	for (size = 0; size <= 256; size++) {
		long displacement;

		for (displacement = -256; displacement <= 256; displacement++) {
			unsigned char* base = arena + kGuard + 300;
			unsigned char* source = base;
			unsigned char* dest = base + displacement;
			int pass;

			for (pass = 0; pass < 2; pass++) {
				sChecks++;
				memcpy(arena + kGuard, pattern, kArena);
				memcpy(expected, pattern, kArena);
				oracle_move(expected + (dest - (arena + kGuard)),
					expected + (source - (arena + kGuard)), size);

				if (pass == 0)
					memmove(dest, source, size);
				else
					bcopy(source, dest, size);

				if (memcmp(arena + kGuard, expected, kArena) != 0) {
					size_t i = 0;
					while (i < kArena && arena[kGuard + i] == expected[i])
						i++;
					fail("%s size %zu displacement %+ld: wrong at byte %zu "
						"(got 0x%02x want 0x%02x)",
						pass == 0 ? "memmove" : "bcopy", size, displacement,
						i, arena[kGuard + i], expected[i]);
					goto next_size;
				}
			}
		}
next_size:
		;
	}

	free(arena);
	free(expected);
	free(pattern);
}


/*!	memcpy() on overlapping ranges is undefined, so this reports rather than
	fails. It is here because the tree may contain callers that are wrong about
	it, and their symptom changes when the routine changes: the old byte loop
	replicated a pattern of period (dest - source), a block copy replicates in
	blocks. Recording where the two disagree turns "some latent bug might
	change behaviour" into a bounded, checkable statement.
*/
static void
report_overlap_divergence(void)
{
	const size_t kArena = 2048;
	unsigned char* a = (unsigned char*)malloc(kArena);
	unsigned char* b = (unsigned char*)malloc(kArena);
	unsigned char* pattern = (unsigned char*)malloc(kArena);
	size_t size;
	long lowestForward = 0;
	int forwardDiffers = 0;
	int backwardDiffers = 0;
	int agreeCount = 0;
	int differCount = 0;

	if (a == NULL || b == NULL || pattern == NULL) {
		printf("    (could not allocate; skipped)\n");
		free(a); free(b); free(pattern);
		return;
	}

	fill_pattern(pattern, kArena, 77);

	for (size = 1; size <= 256; size++) {
		long displacement;

		for (displacement = -256; displacement <= 256; displacement++) {
			unsigned char* base;
			if (displacement == 0)
				continue;
			/* Only overlapping cases are interesting. */
			if ((size_t)(displacement < 0 ? -displacement : displacement)
					>= size) {
				continue;
			}

			memcpy(a, pattern, kArena);
			memcpy(b, pattern, kArena);
			base = a + 512;
			sCopy(base + displacement, base, size);
			legacy_copy(b + 512 + displacement, b + 512, size);

			if (memcmp(a, b, kArena) == 0) {
				agreeCount++;
			} else {
				differCount++;
				if (displacement > 0) {
					if (!forwardDiffers || displacement < lowestForward) {
						lowestForward = displacement;
						forwardDiffers = 1;
					}
				} else
					backwardDiffers = 1;
			}
		}
	}

	printf("    overlapping cases: %d agree with the old generic routine, "
		"%d differ\n", agreeCount, differCount);
	printf("    dest below source (dest < source): %s\n",
		backwardDiffers ? "DIFFERS somewhere" : "identical everywhere "
			"(both are correct here)");
	if (forwardDiffers) {
		printf("    dest above source: differs from displacement %+ld upward "
			"-- both are wrong, differently\n", lowestForward);
	} else
		printf("    dest above source: identical everywhere\n");

	free(a);
	free(b);
	free(pattern);
}


// #pragma mark -


int
main(void)
{
	printf("memcpy_test: routine under test is %s\n", COPY_UNDER_TEST_NAME);
	printf("oracle is a local volatile byte loop, not memcpy()\n\n");

	/* Arenas aligned to 4096 so that adding an offset gives exactly the
	   alignment asked for, whatever malloc happened to return. */
	if (posix_memalign((void**)&sSourceArena, 4096, kMaxSize + 512) != 0
		|| posix_memalign((void**)&sDestArena, 4096,
			kMaxSize + 512 + 2 * kGuard) != 0
		|| posix_memalign((void**)&sExpected, 4096, kMaxSize + 512) != 0) {
		printf("could not allocate the arenas\n");
		return 1;
	}
	fill_pattern(sSourceArena, kMaxSize + 512, 13);

	printf("sizes and alignments:\n");
	test_sizes_and_alignments();
	printf("degenerate cases:\n");
	test_degenerate();
	printf("large copies:\n");
	test_large();
	printf("reading or writing outside the length:\n");
	test_page_boundaries();
	test_self_copy_read_only();
	printf("memmove and bcopy:\n");
	test_memmove_and_bcopy();
	printf("overlap behaviour against the old generic routine (informational, "
		"memcpy overlap is undefined):\n");
	report_overlap_divergence();

	printf("\n%d checks, %d failures: %s\n", sChecks, sFailures,
		sFailures == 0 ? "PASS" : "FAIL");
	return sFailures == 0 ? 0 : 1;
}
