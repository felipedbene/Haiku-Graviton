/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NET_STACK_CHECKSUM_H
#define NET_STACK_CHECKSUM_H


#include <ByteOrder.h>
#include <SupportDefs.h>

#include <string.h>


/*!	The internet checksum (RFC 1071) inner loop.

	Deliberately the only copy of this algorithm and deliberately free of kernel
	dependencies, so that the routine which ships is also the routine a userspace
	test can compile and verify. See src/tests/add-ons/kernel/network/checksum/.

	\section contract The contract callers depend on

	This is not "the checksum of a byte range". It is one term of a sum that
	callers may accumulate further, and three details of its representation are
	load bearing:

	-# Words are summed in **host** byte order, not network order. The returned
	   value is therefore byte-swapped with respect to the on-the-wire checksum
	   on a little-endian host. That is harmless because every consumer is
	   consistently swapped -- the folded sum of swapped words is the swap of the
	   folded sum -- and it saves a byte swap per word. It does mean the value is
	   meaningless unless it reaches the wire through the same convention.
	-# A trailing odd byte is the **first** byte of a 16-bit word whose second
	   byte the caller has not supplied, so it is placed where the first byte of
	   a word goes: the low half on a little-endian host, the high half on a
	   big-endian one.
	-# The result is **folded but not complemented**. checksum_data() in
	   net_buffer.cpp relies on both: it sums per-node results, and it byte-swaps
	   the folded result of a node that begins at an odd offset in order to
	   restore the word parity that the node boundary broke. Complementing here,
	   or returning an unfolded sum, would silently break that.

	Changing any of the three requires changing checksum_data() and
	Checksum::operator uint16() together with it.

	\section why Why it is written this way

	The obvious loop -- add one 16-bit word per iteration into a 32-bit
	accumulator -- costs one load, one add and one branch per two bytes, and was
	measured at 0.229 ns/byte on Neoverse V1. Summing 32-bit halves of 64-bit
	loads into a 64-bit accumulator costs one load and two adds per eight bytes
	and measures 0.086. Folding an over-wide accumulator at the end is exactly
	equivalent, because one's complement addition is associative and the fold
	only ever adds 16-bit lanes together; lane order does not matter, which is
	also why the same expression is correct on either endianness.

	The 64-bit accumulator additionally removes an overflow the 32-bit one had:
	adding 16-bit words into a uint32 wraps after 131076 bytes. No caller can
	reach that today (a net_buffer is capped well below it), so this fixes a bug
	nobody could hit rather than one anybody did -- but it is why lengths above
	131076 are the one place this routine and its predecessor disagree, and the
	predecessor is the one that is wrong.

	Loads go through memcpy() rather than a cast because a cast from uint8* to
	uint64* is both an aliasing violation and an unaligned access. Every compiler
	of interest turns a fixed-size memcpy into the single load it describes, and
	arm64 permits that load to be unaligned; measured, alignment costs nothing
	here.
*/
static inline uint16
net_checksum_compute(const uint8* buffer, size_t length)
{
	uint64 sum = 0;

	// Four 64-bit loads per iteration. Past this the loads outrun the adds and
	// the measured rate stops improving.
	while (length >= 32) {
		uint64 a, b, c, d;
		memcpy(&a, buffer, sizeof(a));
		memcpy(&b, buffer + 8, sizeof(b));
		memcpy(&c, buffer + 16, sizeof(c));
		memcpy(&d, buffer + 24, sizeof(d));

		// One add per half, never `sum += half + half`: the halves are uint32, so
		// C would evaluate that sum in 32-bit arithmetic and discard the carry
		// out of it before widening. Two adds into the uint64 cannot.
		sum += (uint32)a;
		sum += (uint32)(a >> 32);
		sum += (uint32)b;
		sum += (uint32)(b >> 32);
		sum += (uint32)c;
		sum += (uint32)(c >> 32);
		sum += (uint32)d;
		sum += (uint32)(d >> 32);

		buffer += 32;
		length -= 32;
	}

	while (length >= 8) {
		uint64 word;
		memcpy(&word, buffer, sizeof(word));
		sum += (uint32)word;
		sum += (uint32)(word >> 32);

		buffer += 8;
		length -= 8;
	}

	while (length >= 2) {
		uint16 word;
		memcpy(&word, buffer, sizeof(word));
		sum += word;

		buffer += 2;
		length -= 2;
	}

	if (length != 0) {
		// The odd byte opens a word rather than closing one; see the contract.
#if B_HOST_IS_LENDIAN
		sum += *buffer;
#else
		sum += (uint16)*buffer << 8;
#endif
	}

	sum = (sum & 0xffffffffULL) + (sum >> 32);
	while ((sum >> 16) != 0)
		sum = (sum & 0xffff) + (sum >> 16);

	return (uint16)sum;
}


#endif	// NET_STACK_CHECKSUM_H
