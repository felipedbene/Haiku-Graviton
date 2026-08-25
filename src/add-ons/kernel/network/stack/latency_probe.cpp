/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "latency_probe.h"
#include "device_interfaces.h"
#include "utility.h"

#include <util/AutoLock.h>

#include <string.h>


int32 gRxlatEnabled = 0;
struct rxlat_stat gRxlatStats[RXLAT_STAT_COUNT];
struct net_device_interface* gRxlatInterface = NULL;

// Bumped by hand on every rebuild of this instrument. An artefact has to announce
// itself: a driver placed in the wrong directory once measured the packaged copy
// for a whole session and the run looked like "the change did nothing".
#define RXLAT_BUILD_STAMP	4


status_t
rxlat_getsockopt(int option, void* value, int* _length)
{
	if (option == RXLAT_OPT_INFO) {
		struct rxlat_info info;
		memset(&info, 0, sizeof(info));

		info.tick_frequency = 0;
#if defined(__aarch64__)
		uint64 frequency;
		__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r" (frequency));
		info.tick_frequency = frequency;
#else
		info.tick_frequency = 1000000;
#endif
		info.enabled = (uint64)gRxlatEnabled;
		info.stat_count = RXLAT_STAT_COUNT;
		info.bucket_count = RXLAT_BUCKETS;
		info.build_stamp = RXLAT_BUILD_STAMP;

		if (gRxlatInterface != NULL) {
			net_fifo_watermark snapshot;
			snapshot_fifo_watermark(&gRxlatInterface->receive_queue,
				&gRxlatInterface->receive_queue_diagnostics, &snapshot, false);

			info.fifo_max_bytes = gRxlatInterface->receive_queue.max_bytes;
			info.fifo_current_bytes = snapshot.current_bytes;
			info.fifo_peak_bytes = snapshot.peak_bytes;
			info.fifo_current_packets = snapshot.current_packets;
			info.fifo_peak_packets = snapshot.peak_packets;
			info.fifo_enqueued = snapshot.enqueued;
			info.fifo_fail_nobufs = snapshot.fail_nobufs;
		}

		size_t length = sizeof(info);
		if ((size_t)*_length < length)
			length = *_length;
		memcpy(value, &info, length);
		*_length = length;
		return B_OK;
	}

	if (option >= RXLAT_OPT_CHUNK_BASE) {
		const size_t total = sizeof(gRxlatStats);
		const size_t offset
			= (size_t)(option - RXLAT_OPT_CHUNK_BASE) * RXLAT_OPT_CHUNK_SIZE;
		if (offset >= total)
			return B_BAD_VALUE;

		size_t length = total - offset;
		if (length > RXLAT_OPT_CHUNK_SIZE)
			length = RXLAT_OPT_CHUNK_SIZE;
		if ((size_t)*_length < length)
			length = *_length;

		memcpy(value, (const uint8*)gRxlatStats + offset, length);
		*_length = length;
		return B_OK;
	}

	return B_BAD_VALUE;
}


status_t
rxlat_setsockopt(int option, const void* value, int length)
{
	int32 argument = 0;
	if (value != NULL && length >= (int)sizeof(int32))
		memcpy(&argument, value, sizeof(int32));

	switch (option) {
		case RXLAT_OPT_ENABLE:
			// Clearing on the way *in* rather than on the way out, so that
			// "enable, run, read" cannot accidentally include samples from a
			// previous arm. Ordering matters: zero the counters before the flag,
			// or the running threads add to a table that is about to be wiped.
			if (argument != 0)
				memset(gRxlatStats, 0, sizeof(gRxlatStats));
			gRxlatEnabled = argument != 0 ? 1 : 0;
			return B_OK;

		case RXLAT_OPT_RESET:
			gRxlatEnabled = 0;
			memset(gRxlatStats, 0, sizeof(gRxlatStats));
			return B_OK;

		case RXLAT_OPT_FIFO_MAX:
		{
			// Changing the cap on a live FIFO is safe in the direction that
			// matters: base_fifo_enqueue_buffer() compares current_bytes against
			// max_bytes on every enqueue, so lowering the cap below the current
			// occupancy simply refuses new frames until the consumer has drained
			// past it. Nothing is freed here and no queued frame is lost.
			if (gRxlatInterface == NULL)
				return B_NOT_INITIALIZED;
			if (argument < 65536)
				return B_BAD_VALUE;

			MutexLocker locker(gRxlatInterface->receive_queue.lock);
			gRxlatInterface->receive_queue.max_bytes = (size_t)argument;
			gRxlatInterface->receive_queue_diagnostics.limit_bytes
				= (size_t)argument;
			return B_OK;
		}
	}

	return B_BAD_VALUE;
}
