/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef ACK_ESTIMATOR_TEST_DEFS_H
#define ACK_ESTIMATOR_TEST_DEFS_H

/*!	The few things <SupportDefs.h> supplies that RemoteAckEstimator actually
	uses, so the estimator can be compiled and exercised off-target with a plain
	host compiler.

	Deliberately tiny, and the same shape as FlowQueueTestDefs.h next door. The
	opcode table is NOT redefined here -- the test includes the real
	RemoteProtocol.h, so a check on the RP_FRAME_ACK wire contract cannot pass
	against a stale copy of the vocabulary.
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

typedef int64		bigtime_t;

#endif	// ACK_ESTIMATOR_TEST_DEFS_H
