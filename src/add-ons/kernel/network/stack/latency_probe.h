/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef LATENCY_PROBE_H
#define LATENCY_PROBE_H


#include <KernelExport.h>
#include <OS.h>

#include <net/net_stack.h>


/*!	A latency instrument for the receive path.

	Existing instruments in this tree measure *CPU*: per-thread `active_time`
	(`netprof`), CPU per byte (`nettput`), the sampling profiler. None of them can
	see a thread that is neither running nor short of work, because a blocked
	thread accrues no CPU time at all. That blind spot is exactly where a
	serialized hand-off hides, so measuring it needs a different instrument.

	Three design constraints, each of which cost something to learn elsewhere in
	this tree:

	- **Not `system_time()`.** On arm64 it returns *microseconds*
	  (`libroot/os/arch/arm64/system_time.c`), and several stages here are
	  expected to be a microsecond or less. Quantising a 2 us stage to 1 us is not
	  a measurement. The architected counter underneath it runs at ~1.05 GHz on
	  this hardware, so `CNTVCT_EL0` is read directly and kept in raw ticks;
	  conversion happens once, at report time, in userland.

	- **Not a trace.** `dprintf` on this platform writes the UART one character
	  at a time, synchronously -- a barrier, not a probe. Everything accumulates
	  into fixed-size counters plus a base-2 histogram, read out by a private
	  `getsockopt`.

	- **Histograms, not means.** The hypothesis under test is about *tails*: a
	  hand-off that is usually cheap and occasionally very expensive is invisible
	  in an average and is the entire story.

	No locks and no atomics on the accumulate path, by construction rather than by
	luck: every statistic below is written by exactly one thread. RXLAT_READER_*
	statistics are touched only by the device reader thread, RXLAT_CONSUMER_* and
	everything downstream of the FIFO only by the device consumer thread. A reader
	of the counters can therefore see a torn 64-bit pair on a 32-bit host, which
	arm64 is not.
*/

#define RXLAT_BUCKETS	40
	// Bucket b covers [2^b, 2^(b+1)) ticks. At 1.05 GHz that is 0.95 ns for
	// bucket 0 up to ~17 minutes for bucket 39, so nothing can fall off either
	// end and be silently lost.

enum {
	// Reader thread (writes these and only these).
	RXLAT_READER_RECEIVE_DATA	= 0,
		// The whole `receive_data()` call: the driver's blocking wait for the
		// device plus the frame copy. Long here means "waiting for the wire".
	RXLAT_READER_DEFRAME,
	RXLAT_READER_ENQUEUE,
		// Time inside fifo_enqueue_buffer_tracked, i.e. how long the reader
		// waited for the FIFO mutex the consumer also holds.
	RXLAT_READER_LOOP,
		// Wall clock between successive frames leaving the reader. Its inverse
		// is the reader's achieved frame rate, including all waiting.
	RXLAT_ENQUEUE_DEPTH_BYTES,
		// Not a latency: FIFO occupancy in bytes seen at enqueue. Kept in the
		// same shape so one histogram printer serves both.

	// Written from the tcp module through net_stack_module_info::rxlat_add, on
	// the consumer thread -- so still single-writer, with one exception noted
	// below.
	RXLAT_TCP_ENDPOINT_LOCK		= 5,
		// Acquiring TCPEndpoint::fLock in SegmentReceived(). The lock is also
		// taken by the reading application in ReadData() and by the net timer.
	RXLAT_TCP_SEGMENT,
		// The whole of SegmentReceived(), lock included.
	RXLAT_TCP_NOTIFY_READER,
		// _NotifyReader(): the condition-variable wake of the reading thread plus
		// the select notification. NOTE: this is the one statistic that can have a
		// second writer, because TCP's timers also reach _NotifyReader from the
		// net timer thread. 64-bit stores are single-copy-atomic here, so the
		// failure mode is a small undercount, not a corrupt value.

	// Consumer thread (writes these and only these).
	RXLAT_CONSUMER_DEQUEUE		= 8,
		// Time inside fifo_dequeue_buffer_tracked. Splits into "the queue was
		// empty and I blocked" and "I waited for the mutex"; the depth histogram
		// below says which.
	RXLAT_FIFO_RESIDENCY,
		// THE number this instrument exists for: enqueue to dequeue, per frame.
	RXLAT_CONSUMER_RECEIVE_LOCK,
		// Acquiring interface->receive_lock.
	RXLAT_CONSUMER_DISPATCH,
		// The protocol walk: ethernet demux -> IPv4 -> TCP -> socket queue ->
		// wake the reading thread. This is where the ACK is generated, so it is
		// the last stage that can affect the sender's measured round trip.
	RXLAT_CONSUMER_LOOP,
	RXLAT_DEQUEUE_DEPTH_BYTES,
		// FIFO occupancy in bytes seen at dequeue. Near zero means the consumer
		// is starved; near the limit means it is the constraint.

	RXLAT_STAT_COUNT			= 16
};


struct rxlat_stat {
	uint64	count;
	uint64	sum;
	uint64	min;
	uint64	max;
	uint64	bucket[RXLAT_BUCKETS];
};


// Private getsockopt/setsockopt channel. There is no other way out of the stack
// module to userland that does not involve either a new device or KDL, and KDL
// needs a serial console this platform can only just about drive.
#define RXLAT_SOL				0x52584c54		/* 'RXLT' */
#define RXLAT_OPT_CHUNK_BASE	0x1000
	// getsockopt(fd, RXLAT_SOL, RXLAT_OPT_CHUNK_BASE + n, buf, &len) returns
	// bytes [n*128, n*128+128) of the flat statistics blob. Chunked because the
	// syscall caps an option at MAX_SOCKET_OPTION_LENGTH = 128 bytes.
#define RXLAT_OPT_CHUNK_SIZE	128
#define RXLAT_OPT_ENABLE		1	/* setsockopt, int32: 0 off, 1 on */
#define RXLAT_OPT_RESET			2	/* setsockopt, ignored value */
#define RXLAT_OPT_FIFO_MAX		3	/* setsockopt, int32 bytes: receive FIFO cap */
#define RXLAT_OPT_INFO			4	/* getsockopt, struct rxlat_info */


struct rxlat_info {
	uint64	tick_frequency;
	uint64	enabled;
	uint64	fifo_max_bytes;
	uint64	fifo_current_bytes;
	uint64	fifo_peak_bytes;
	uint64	fifo_current_packets;
	uint64	fifo_peak_packets;
	uint64	fifo_enqueued;
	uint64	fifo_fail_nobufs;
	uint64	stat_count;
	uint64	bucket_count;
	uint64	build_stamp;
};


extern int32 gRxlatEnabled;
extern struct rxlat_stat gRxlatStats[RXLAT_STAT_COUNT];

// Set by device_interfaces.cpp when the interface comes up, so the readout can
// reach the FIFO whose numbers are being reported without a lookup by name.
extern struct net_device_interface* gRxlatInterface;

status_t rxlat_getsockopt(int option, void* value, int* _length);
status_t rxlat_setsockopt(int option, const void* value, int length);


/*!	Reads the architected counter.

	No ISB. An `mrs` from CNTVCT_EL0 can be reordered against surrounding loads
	and stores within the core's reorder window, so each sample carries an error
	of at most that window -- tens of nanoseconds. Every stage this instrument
	reports is microseconds or larger, and an ISB would add a real, measurable
	cost to the datapath in exchange for removing an error two to five orders of
	magnitude below the signal. The calibration in `rxlat` measures both variants
	so that this trade is a reported number rather than an assertion.
*/
static inline uint64
rxlat_ticks(void)
{
#if defined(__aarch64__)
	uint64 value;
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r" (value));
	return value;
#else
	return (uint64)system_time();
#endif
}


static inline void
rxlat_add(int which, uint64 delta)
{
	if (gRxlatEnabled == 0)
		return;

	struct rxlat_stat* stat = &gRxlatStats[which];

	if (stat->count == 0 || delta < stat->min)
		stat->min = delta;
	if (delta > stat->max)
		stat->max = delta;
	stat->count++;
	stat->sum += delta;

	// __builtin_clzll is undefined at zero, and a zero delta is normal: two
	// counter reads a few instructions apart can land on the same tick.
	int bucket = (delta == 0) ? 0 : (63 - __builtin_clzll(delta));
	if (bucket >= RXLAT_BUCKETS)
		bucket = RXLAT_BUCKETS - 1;
	stat->bucket[bucket]++;
}


#endif	// LATENCY_PROBE_H
