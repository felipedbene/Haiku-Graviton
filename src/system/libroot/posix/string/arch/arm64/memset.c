/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <stdint.h>
#include <string.h>
#undef memset


/*!	An arm64 memset() written in the same idiom as this directory's memcpy.c.

	It replaces string/arch/generic/generic_memset.c for arm64. The generic
	routine aligns the destination and then stores one word per iteration; on
	arm64, where unaligned stores to Normal memory are free in hardware and a
	`dc zva` clears a whole cache line in one instruction, that leaves most of
	the achievable bandwidth on the table.

	Structure, mirroring memcpy.c:

	- Sizes up to 128 are handled with a fixed ladder of overlapping wide
	  stores and no loop at all. memset has it easier than memcpy here: every
	  byte written is identical, so an overlap between two stores is always
	  harmless and there is no load-before-store ordering to preserve.

	- Above 128 bytes there are two paths. A zero fill uses `dc zva`, which
	  zeroes an aligned, implementation-defined block (64 bytes on Graviton) per
	  instruction without moving any data across the store pipeline. A non-zero
	  fill, or a target on which `dc zva` is unavailable, falls back to aligning
	  the destination to 16 and storing 64 bytes an iteration.

	`dc zva` is used only when DCZID_EL0 says it is both permitted and the size
	this routine's loop assumes:

	- bit[4] (DZP) set means `dc zva` is prohibited at the current exception
	  level and would fault; the fallback path is taken.
	- bits[3:0] (BS) give log2 of the block size in words. The loop below
	  advances 64 bytes at a time, so it only takes the `dc zva` path when the
	  block is exactly 64 bytes (BS == 4).

	Testing `(DCZID_EL0 & 0x1f) == 4` covers both conditions at once: DZP clear
	and BS == 4. Any other value -- DZP set, or a block that is not 64 bytes --
	takes the store-loop fallback, so the routine is correct on hardware whose
	`dc zva` geometry differs from Graviton's rather than silently mis-filling.

	Like memcpy.c the routine never touches memory outside [dest, dest + count),
	and it must be built with -fno-builtin -fno-tree-loop-distribute-patterns:
	the zero-store fallback loop is exactly the shape the loop-idiom pass would
	rewrite into a call to memset, i.e. into a call to this function from inside
	itself. The Jamfile sets both flags; see graviton/docs/arm64-memcpy.md.
*/


/*!	Unaligned, may-alias access types -- identical rationale to memcpy.c: the
	tree is built -fno-strict-aliasing, but the punning should not depend on a
	global flag, and a scalar assignment (unlike a struct copy) cannot be
	lowered to a memset call. */
typedef uint32_t __attribute__((may_alias, aligned(1))) unaligned_uint32;
typedef uint64_t __attribute__((may_alias, aligned(1))) unaligned_uint64;
typedef __uint128_t __attribute__((may_alias, aligned(1))) unaligned_uint128;


static inline void
set_64(uint8_t* d, unaligned_uint128 v)
{
	((unaligned_uint128*)d)[0] = v;
	((unaligned_uint128*)d)[1] = v;
	((unaligned_uint128*)d)[2] = v;
	((unaligned_uint128*)d)[3] = v;
}


/*!	Fills 0..32 bytes in a fixed number of accesses, without a loop. The two
	accesses of each width coincide at exactly that width and overlap below it;
	because every byte is the same value that is always safe. */
static inline void
set_0_to_32(uint8_t* d, size_t count, unaligned_uint128 v128, uint64_t v64,
	uint32_t v32, uint8_t v8)
{
	if (count >= 16) {
		*(unaligned_uint128*)d = v128;
		*(unaligned_uint128*)(d + count - 16) = v128;
	} else if (count >= 8) {
		*(unaligned_uint64*)d = v64;
		*(unaligned_uint64*)(d + count - 8) = v64;
	} else if (count >= 4) {
		*(unaligned_uint32*)d = v32;
		*(unaligned_uint32*)(d + count - 4) = v32;
	} else if (count > 0) {
		/* 1..3 bytes: bytes 0, count>>1 and count-1, which coincide for 1 and
		   overlap for 2. */
		d[0] = v8;
		d[count >> 1] = v8;
		d[count - 1] = v8;
	}
}


/*!	Fills 0..64 bytes as a fixed ladder. */
static inline void
set_0_to_64(uint8_t* d, size_t count, unaligned_uint128 v128, uint64_t v64,
	uint32_t v32, uint8_t v8)
{
	if (count > 32) {
		*(unaligned_uint128*)d = v128;
		*(unaligned_uint128*)(d + 16) = v128;
		*(unaligned_uint128*)(d + count - 32) = v128;
		*(unaligned_uint128*)(d + count - 16) = v128;
	} else
		set_0_to_32(d, count, v128, v64, v32, v8);
}


void*
memset(void* dest, int c, size_t count)
{
	uint8_t* d = (uint8_t*)dest;
	const uint8_t v8 = (uint8_t)c;

	if (count == 0)
		return dest;

	const uint64_t v64 = (uint64_t)0x0101010101010101ULL * v8;
	const uint32_t v32 = (uint32_t)v64;
	const unaligned_uint128 v128 = ((unaligned_uint128)v64 << 64) | v64;

	if (count <= 128) {
		if (count <= 64)
			set_0_to_64(d, count, v128, v64, v32, v8);
		else {
			set_64(d, v128);
			set_0_to_64(d + 64, count - 64, v128, v64, v32, v8);
		}
		return dest;
	}

	/* count > 128. */
	uint8_t* const dstend = d + count;

	/* Zero fill: use dc zva when DCZID_EL0 permits it and the block is the
	   64 bytes this loop advances by. */
	if (v8 == 0) {
		uint64_t dczid;
		__asm__("mrs %0, dczid_el0" : "=r"(dczid));
		if ((dczid & 0x1fu) == 4u) {
			/* Head: fill [d, first 64-byte boundary) with plain stores. That
			   boundary is at most 63 bytes ahead, so a 64-byte store from d
			   covers it, and count > 128 keeps d + 64 inside the buffer. */
			uint8_t* const zdst
				= (uint8_t*)(((uintptr_t)d + 63) & ~(uintptr_t)63);
			set_64(d, v128);

			/* Interior: whole 64-byte blocks. count > 128 guarantees at least
			   one, and zend > zdst, so the loop always makes progress and never
			   writes past dstend. */
			uint8_t* const zend = (uint8_t*)((uintptr_t)dstend & ~(uintptr_t)63);
			for (uint8_t* p = zdst; p != zend; p += 64)
				__asm__ volatile("dc zva, %0" : : "r"(p) : "memory");

			/* Tail: last 64 bytes, overlapping the final block harmlessly. */
			set_64(dstend - 64, v128);
			return dest;
		}
	}

	/* Non-zero fill, or dc zva unavailable: align the destination to 16 (the
	   store width the body uses) and fill 64 bytes an iteration. The first
	   store covers the <16-byte head that the advance skips. */
	{
		const size_t advance = (-(uintptr_t)d) & 15;

		*(unaligned_uint128*)d = v128;
		d += advance;
		count -= advance;
	}

	while (count >= 64) {
		set_64(d, v128);
		d += 64;
		count -= 64;
	}

	set_0_to_64(d, count, v128, v64, v32, v8);
	return dest;
}
