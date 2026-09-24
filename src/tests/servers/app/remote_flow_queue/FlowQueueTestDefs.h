/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef FLOW_QUEUE_TEST_DEFS_H
#define FLOW_QUEUE_TEST_DEFS_H

/*!	The few things <SupportDefs.h> supplies that RemoteFlowQueue actually uses,
	so the policy can be compiled and exercised off-target with a plain host
	compiler.

	Deliberately tiny. The opcode table is NOT redefined here -- the queue and
	this test both include the real RemoteProtocol.h, so a classification test
	cannot pass against a stale copy of the vocabulary.
*/

#include <stddef.h>
#include <stdint.h>

typedef uint8_t		uint8;
typedef uint16_t	uint16;
typedef uint32_t	uint32;
typedef uint64_t	uint64;
typedef int8_t		int8;
typedef int16_t		int16;
typedef int32_t		int32;
typedef int64_t		int64;

typedef int32		status_t;

#define B_OK			((status_t)0)
#define B_ERROR			((status_t)-1)
#define B_BAD_VALUE		((status_t)-2147483642)
#define B_NO_MEMORY		((status_t)-2147483648LL)

#endif	// FLOW_QUEUE_TEST_DEFS_H
