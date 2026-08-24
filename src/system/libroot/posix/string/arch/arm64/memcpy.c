/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <stdint.h>
#include <string.h>
#undef memcpy


/*!	An arm64 memcpy() that does not fall to a byte loop on mismatched alignment.

	arm64 previously used string/arch/generic/generic_memcpy.c, which reaches
	its word-at-a-time loop only when source and destination are misaligned by
	the *same* amount and copies the whole region one byte at a time otherwise.
	That condition exists for architectures on which an unaligned access traps
	or is microcoded. arm64 performs unaligned accesses to Normal memory in
	hardware, so the requirement bought nothing here and cost a byte loop in the
	case that matters most.

	The case that matters most is measured, not assumed. A received TCP payload
	begins 54 bytes into the frame (14 ethernet + 20 IPv4 + 20 TCP), so copying
	it out to an application buffer obtained from malloc() puts a source at
	6 mod 8 against an aligned destination -- mismatched, hence the byte loop.
	The mismatch is also self-sustaining across a fragmented net_buffer: once
	the first misaligned segment has advanced the destination out of phase,
	every following segment copy is mismatched too, even the ones whose source
	is perfectly aligned.

	Structure, and why each part is the way it is:

	- Sizes up to 64, and the tail of a longer copy, are done as a single group
	  of at most four fixed-width accesses, with no loop at all. Short copies
	  are much the commonest kind, and the first version of this routine
	  byte-copied everything below 16 bytes -- which lost up to 50% against the
	  generic routine at n = 8, because a short copy whose operands *do* agree
	  in alignment is exactly the case the generic routine handled well. The
	  accesses overlap for any size that is not an exact multiple of their
	  width, so a few destination bytes are written twice; memcpy is free to do
	  that, and it is much cheaper than a branch per byte. Every access still
	  lies inside [dest, dest + count).

	- 65..128 bytes are 64 bytes, then the remainder as one more such group,
	  still with no loop and no alignment work: below roughly this size a
	  prologue costs more than aligned stores save.

	- Only above 128 bytes is the destination aligned, and it is aligned to 16,
	  not 8. The first version of this routine aligned to 8 on the stated
	  grounds that "stores are the side that benefits"; that reasoning did not
	  survive contact with the compiler, which widens the body to 16-byte
	  accesses regardless. Aligning to 8 and then storing 16 at a time leaves
	  half the stores crossing an alignment boundary anyway, and measured
	  *slower* at 8961 bytes than the case where the prologue happened to align
	  the source instead. Align to the width actually emitted, or do not
	  bother.

	- The body moves 64 bytes an iteration. Reading memory a device has just
	  written by DMA misses in every cache, and a loop with one access in
	  flight is bounded by memory latency rather than by bandwidth.

	Three properties are relied upon elsewhere and should not be lost.

	The routine never reads or writes outside [dest, dest + count). That is what
	makes it safe when either end abuts an unmapped page, and it is checked by
	tests/system/libroot/posix/string/memcpy_test.c against PROT_NONE guard
	pages.

	dest == source returns immediately without writing, because the generic
	routine short-circuited it, the tree has callers that rely on it, and this
	routine is also user_memcpy() and the kernel's memcpy: a write to a
	read-only mapping is a caught SIGSEGV in userland but a KDL panic in the
	kernel. (glibc does not do this, and fails that check; the point is
	compatibility with what this tree's callers were built against, not a claim
	about the standard.)

	And the accesses are ordered so that the routine stays *correct* when the
	ranges overlap with the destination below the source -- the accidental
	`memcpy(p, p + k, n)` that in-place header removal produces. memcpy is
	undefined on any overlap and this is not a promise, but both the routine
	this replaces and glibc happen to tolerate that direction, so a caller that
	works today should not start failing.

	The rule that gives it is narrower than it first appears, and the obvious
	form of it is wrong. Writing destination byte p destroys the source byte at
	that address, which is source index p - delta for delta = source - dest > 0.
	So a group of accesses that reads source [x, y) is safe exactly when no
	earlier write has touched [x + delta, y + delta). Two consequences:

	- A group that reads and writes the same ascending, non-overlapping range is
	  always safe, for any delta: earlier writes covered [0, x), and x + delta
	  is at least x.
	- An *overlapping* group -- one whose write range starts before the previous
	  write ended, which is how a tail is handled without a loop -- is safe only
	  if the previous writes stopped at or before its start plus delta. With
	  delta as small as 1 that means: not at all, unless the previous writes
	  stopped exactly at its start.

	The first attempt at this routine got that wrong. It wrote [0, 32) and then
	an overlapping tail at [count - 32, count), which for count = 33 reads
	source bytes that the first access had already written over; the test found
	it at size 33, displacement 1. So: every group writes a range that begins
	exactly where the previous one ended, and every group issues all of its
	loads before any of its stores, which absorbs the overlap *within* a group.
	That is why the 33..128 case is one load-everything-then-store-everything
	group rather than a ladder of four, and why the destination-alignment
	prologue copies exactly the bytes it consumes rather than a full 16 with a
	short advance.

	dest above source remains wrong, as it must be for any forward copy, and as
	it was before.

	Known limitation, arm64-general rather than specific to this routine: an
	unaligned or wider-than-8-byte access to Device-nGnRnE memory raises an
	Alignment fault, and vm_map_physical_memory() silently defaults to
	B_UNCACHED_MEMORY -- which VMSAv8TranslationMap maps to Device-nGnRnE -- for
	any caller that does not name a memory type. Copying to or from MMIO with
	memcpy() was already wrong on arm64; with this routine it fails rather than
	working by accident. See graviton/docs/arm64-memcpy.md for the audit of
	which callers that reaches.
*/


/*!	Unaligned, may-alias access types.

	aligned(1) tells the compiler the pointer may be unaligned, so it emits a
	plain ldr/str instead of assuming otherwise. may_alias is what makes the
	uint8_t-to-wider punning below defined rather than merely working: the tree
	is built -fno-strict-aliasing (build/jam/ArchitectureRules), so it would
	work without it, but the one routine whose miscompilation is undetectable
	should not rest on a global build flag.

	__uint128_t rather than a two-element struct on purpose. A struct assignment
	is exactly the shape the compiler is permitted to lower into a call to
	memcpy, and a call to memcpy from inside memcpy does not return; a scalar
	assignment cannot become a call. This is verified on the generated object
	code for both the kernel and libroot builds, not assumed -- see the doc.
*/
typedef uint32_t __attribute__((may_alias, aligned(1))) unaligned_uint32;
typedef uint64_t __attribute__((may_alias, aligned(1))) unaligned_uint64;
typedef __uint128_t __attribute__((may_alias, aligned(1))) unaligned_uint128;


static inline void
copy_64(uint8_t* d, const uint8_t* s)
{
	const unaligned_uint128 a = ((const unaligned_uint128*)s)[0];
	const unaligned_uint128 b = ((const unaligned_uint128*)s)[1];
	const unaligned_uint128 c = ((const unaligned_uint128*)s)[2];
	const unaligned_uint128 e = ((const unaligned_uint128*)s)[3];

	((unaligned_uint128*)d)[0] = a;
	((unaligned_uint128*)d)[1] = b;
	((unaligned_uint128*)d)[2] = c;
	((unaligned_uint128*)d)[3] = e;
}


/*!	Copies 0..32 bytes in a fixed number of accesses, without a loop.

	The pairs overlap for any size that is not an exact multiple of the access
	width, which writes a few destination bytes twice and reads a few source
	bytes twice. Both stay strictly inside the caller's range, so this cannot
	fault where a byte loop would not.

	Every load is issued before every store, which costs nothing here and is
	what keeps the routine correct when the ranges overlap with the destination
	below the source -- see the note on ordering at memcpy() below.

	The comparisons are strict on purpose. At exactly the access width the two
	halves coincide, so `>=` would move every byte twice; measured, that cost
	15% at 32 bytes and 5% at 16 against the routine this replaces, which is a
	regression rather than an improvement. A count equal to the width falls to
	the next case down, where two accesses of half the width cover it exactly.
*/
static inline void
copy_0_to_32(uint8_t* d, const uint8_t* s, size_t count)
{
	if (count > 16) {
		const unaligned_uint128 a = *(const unaligned_uint128*)s;
		const unaligned_uint128 b = *(const unaligned_uint128*)(s + count - 16);

		*(unaligned_uint128*)d = a;
		*(unaligned_uint128*)(d + count - 16) = b;
	} else if (count > 8) {
		const uint64_t a = *(const unaligned_uint64*)s;
		const uint64_t b = *(const unaligned_uint64*)(s + count - 8);

		*(unaligned_uint64*)d = a;
		*(unaligned_uint64*)(d + count - 8) = b;
	} else if (count >= 4) {
		const uint32_t a = *(const unaligned_uint32*)s;
		const uint32_t b = *(const unaligned_uint32*)(s + count - 4);

		*(unaligned_uint32*)d = a;
		*(unaligned_uint32*)(d + count - 4) = b;
	} else if (count > 0) {
		/* 1..3 bytes in three accesses, which coincide for 1 and overlap
		   for 2. */
		const uint8_t a = s[0];
		const uint8_t b = s[count >> 1];
		const uint8_t c = s[count - 1];

		d[0] = a;
		d[count >> 1] = b;
		d[count - 1] = c;
	}
}


/*!	Copies 0..64 bytes as a single group: every load issued before every store,
	so the overlap inside the group is harmless, and 64 bytes needs only eight
	registers. Used both for short copies and for the tail of a long one, where
	it is the last group and so may begin exactly where the previous write
	ended.
*/
static inline void
copy_0_to_64(uint8_t* d, const uint8_t* s, size_t count)
{
	if (count > 48) {
		const unaligned_uint128 a = ((const unaligned_uint128*)s)[0];
		const unaligned_uint128 b = ((const unaligned_uint128*)s)[1];
		const unaligned_uint128 c = ((const unaligned_uint128*)s)[2];
		const unaligned_uint128 e = *(const unaligned_uint128*)(s + count - 16);

		((unaligned_uint128*)d)[0] = a;
		((unaligned_uint128*)d)[1] = b;
		((unaligned_uint128*)d)[2] = c;
		*(unaligned_uint128*)(d + count - 16) = e;
	} else if (count > 32) {
		/* Three accesses, not four: 33..48 bytes needs 48, and taking the
		   whole second half would move 64 -- which cost 15% at 33 bytes. */
		const unaligned_uint128 a = ((const unaligned_uint128*)s)[0];
		const unaligned_uint128 b = ((const unaligned_uint128*)s)[1];
		const unaligned_uint128 c = *(const unaligned_uint128*)(s + count - 16);

		((unaligned_uint128*)d)[0] = a;
		((unaligned_uint128*)d)[1] = b;
		*(unaligned_uint128*)(d + count - 16) = c;
	} else
		copy_0_to_32(d, s, count);
}


void*
memcpy(void* dest, const void* source, size_t count)
{
	uint8_t* d = (uint8_t*)dest;
	const uint8_t* s = (const uint8_t*)source;

	if (count == 0 || dest == source)
		return dest;

	if (count <= 128) {
		if (count <= 64)
			copy_0_to_64(d, s, count);
		else {
			/* [0, 64) first, then 1..64 more as the trailing group -- which
			   begins exactly where that write ended, so it is safe to let its
			   two halves overlap each other. */
			copy_64(d, s);
			copy_0_to_64(d + 64, s + 64, count - 64);
		}
		return dest;
	}

	/* Align the destination to 16. count > 128 above, so this cannot exhaust
	   it and the loop below needs no further guarding. Exactly the bytes needed
	   are copied, rather than a full 16 with a short advance, so that the loop
	   does not re-read source bytes the prologue has already written over when
	   the ranges overlap. */
	{
		const size_t advance = (-(uintptr_t)d) & 15;

		if (advance != 0) {
			copy_0_to_32(d, s, advance);
			d += advance;
			s += advance;
			count -= advance;
		}
	}

	while (count >= 64) {
		copy_64(d, s);
		d += 64;
		s += 64;
		count -= 64;
	}

	/* 0..63 left, as the trailing group. */
	copy_0_to_64(d, s, count);

	return dest;
}
