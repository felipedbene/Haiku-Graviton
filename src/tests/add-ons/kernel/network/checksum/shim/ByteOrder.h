/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CHECKSUM_TEST_SHIM_BYTE_ORDER_H
#define CHECKSUM_TEST_SHIM_BYTE_ORDER_H


/*!	Just enough of Haiku's ByteOrder.h to compile checksum.h on a host. See the
	note in this directory's SupportDefs.h.

	B_HOST_IS_LENDIAN is what selects the odd-byte placement in checksum.h and the
	expected values in the test, so getting it wrong here would produce a test
	that passes against the wrong convention. It is derived from the compiler's own
	byte-order macro rather than assumed.
*/

#include <SupportDefs.h>


#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#	if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#		define B_HOST_IS_LENDIAN 0
#		define B_HOST_IS_BENDIAN 1
#	else
#		define B_HOST_IS_LENDIAN 1
#		define B_HOST_IS_BENDIAN 0
#	endif
#else
#	error "cannot determine host byte order"
#endif


static inline uint16
__swap_int16(uint16 value)
{
	return (uint16)((value >> 8) | (value << 8));
}


#endif	// CHECKSUM_TEST_SHIM_BYTE_ORDER_H
