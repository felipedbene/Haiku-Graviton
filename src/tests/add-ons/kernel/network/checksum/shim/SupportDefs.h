/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CHECKSUM_TEST_SHIM_SUPPORT_DEFS_H
#define CHECKSUM_TEST_SHIM_SUPPORT_DEFS_H


/*!	Just enough of Haiku's SupportDefs.h to compile checksum.h on a host, so the
	same source can be verified without a cross toolchain. Types only -- no
	algorithm lives here, and nothing here may shadow anything checksum.h relies
	on for behaviour. Under jam the real headers are used instead.
*/

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>


typedef uint8_t		uint8;
typedef uint16_t	uint16;
typedef uint32_t	uint32;
typedef uint64_t	uint64;

typedef int8_t		int8;
typedef int16_t		int16;
typedef int32_t		int32;
typedef int64_t		int64;


#endif	// CHECKSUM_TEST_SHIM_SUPPORT_DEFS_H
