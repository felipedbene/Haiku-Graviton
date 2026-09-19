/*
 * Copyright 2006-2008, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */
#ifndef NET_UTILITY_H
#define NET_UTILITY_H


#include <net_stack.h>


class UserBuffer {
public:
								UserBuffer(void* buffer, size_t size);

			void*				Push(void* source, size_t size);
			status_t			Pad(size_t length);
			status_t			PadToNext(size_t length);

			status_t			Status() const
									{ return fStatus; }
			size_t				BytesConsumed() const
									{ return fBufferSize - fAvailable; }

private:
			uint8*				fBuffer;
			size_t				fBufferSize;
			size_t				fAvailable;
			status_t			fStatus;
};


inline
UserBuffer::UserBuffer(void* buffer, size_t size)
	:
	fBuffer((uint8*)buffer),
	fBufferSize(size),
	fAvailable(size),
	fStatus(B_OK)
{
}


// checksums
uint16		compute_checksum(uint8* _buffer, size_t length);
uint16		checksum(uint8* buffer, size_t length);

// notifications
status_t	notify_socket(net_socket* socket, uint8 event, int32 value);

/*!	Version stamp for the receive-path diagnostic instrumentation.

	Bump this whenever the instrumentation changes. It is printed once when the
	stack module initializes and prefixes every diagnostic line, so that a
	measurement can always be attributed to a specific build. A hot-swapped
	module that fails to load is otherwise indistinguishable from a change that
	had no effect.
*/
#define NET_RX_DIAG_VERSION		"rxdiag-1"

/*!	Occupancy instrumentation for a net_fifo.

	This deliberately lives *beside* the fifo rather than inside it: net_fifo is
	declared in the shared <net_stack.h> and is embedded in structures owned by
	other network modules, so growing it would silently break any module built
	against the smaller definition. Everything here is private to the stack
	module.

	All fields are updated while fifo->lock is held, which makes the whole
	structure a consistent snapshot when copied under that lock.
*/
struct net_fifo_watermark {
	// capacity and occupancy
	size_t		limit_bytes;
	size_t		current_bytes;
	size_t		peak_bytes;
	uint32		current_packets;
	uint32		peak_packets;
	uint64		enqueued;
	uint64		dequeued;

	// enqueue failures, split by cause. base_fifo_enqueue_buffer() can only fail
	// with ENOBUFS today, so counting the other case is how we would find out if
	// that ever stopped being true instead of assuming it.
	uint64		fail_total;
	uint64		fail_nobufs;
	uint64		fail_other;
	size_t		fail_bytes_min;
	size_t		fail_bytes_max;
		// occupancy observed at a failed enqueue. If a failure really is the
		// queue being full then this sits just under limit_bytes; anything else
		// means the limit is not what we think it is.
};


/*!	Sojourn-time (CoDel-style) queue discipline for a net_fifo.

	The device-interface receive FIFO is sized in bytes -- 16 MiB -- and on a
	fast link fed past the consumer's drain rate it fills and stays full, so a
	frame's transit is dominated by the time it spends *sitting in the queue*
	(its sojourn), not by any per-frame work. A standing queue that deep inflates
	the round trip an order of magnitude, and TCP, seeing the inflated RTT,
	throttles the senders through their congestion windows -- so the byte cap
	silently became the throughput governor, and a worse one than a shorter queue
	would be.

	The cure is to bound the queue by *time* rather than by bytes. This is the
	controlled-delay (CoDel) discipline of Nichols & Jacobson: timestamp each
	buffer at enqueue, measure its sojourn at dequeue, and once the sojourn has
	stayed above a small target continuously for a full interval -- i.e. the queue
	is persistently, not transiently, overloaded -- drop buffers at a controlled,
	increasing rate until the sojourn comes back under target. A transient burst
	passes through untouched; only a standing queue is shed. That is what keeps
	the loss rate near a byte cap's while removing the latency a byte cap leaves
	in place.

	The byte cap (net_fifo::max_bytes) is retained untouched as an absolute
	backstop, so this can never admit more than the old code did.

	All state is read and written on exactly one thread (the device consumer),
	so no lock is taken here. The parameters are set once at init.
*/
struct net_fifo_codel {
	// Parameters (set at init, then read-only on the hot path).
	bigtime_t	target;			// sojourn we tolerate before acting (us)
	bigtime_t	interval;		// window sojourn must stay high before dropping (us)
	size_t		min_bytes;		// never drop while the queue holds less than this

	// State, consumer-thread private.
	bigtime_t	first_above_time;	// when sojourn first went above target (0 = below)
	bigtime_t	drop_next;			// scheduled time of the next drop
	uint32		count;				// drops in this dropping episode (drives the rate)
	bool		dropping;			// currently in a dropping episode

	// Counters, for the receive-queue debugger dump.
	uint64		dropped;			// buffers shed by this discipline
	uint64		evaluated;			// buffers examined
	bigtime_t	last_sojourn;		// most recent sojourn seen (us)
	bigtime_t	max_sojourn;		// high-water sojourn (us)
};

// Defaults, chosen from the measured behaviour of the arm64 ENA receive path
// rather than from CoDel's textbook 5 ms / 100 ms -- the knee for this queue is
// hundreds of microseconds, not milliseconds. Measured on c7g.16xlarge, MTU
// 9001, 8 receive flows, against the shipped 16 MiB byte cap (7.1 Gbit/s,
// ~16.8 ms loaded RTT):
//
//   target   goodput   loaded RTT   induced loss
//   -------   -------   ----------   ------------
//    300 us   8.3 Gb/s    ~1.1 ms       ~0.75%
//    500 us   8.2 Gb/s    ~1.3 ms       ~0.44%    <- default
//    700 us   8.3 Gb/s    ~1.8 ms       ~0.31%
//   1000 us   8.0 Gb/s    ~2.3 ms       ~0.26%
//
// Every point improves goodput and cuts loaded latency by an order of magnitude
// over the byte cap, and every point sheds far less than a fixed small byte cap
// would at the same latency (a 256 KiB cap gave the same latency band at 3.3%
// loss). Loss and latency trade against each other along TCP's own control law
// (lower RTT at a given rate needs a higher drop rate to hold the window there),
// so a single default cannot be simultaneously lowest on both; 500 us keeps the
// order-of-magnitude latency win while holding induced loss to a fraction of any
// comparable byte cap.
#define NET_FIFO_CODEL_TARGET		500			// us
#define NET_FIFO_CODEL_INTERVAL		10000		// us
#define NET_FIFO_CODEL_MIN_BYTES	65536		// bytes

// fifos
status_t	init_fifo(net_fifo* fifo, const char *name, size_t maxBytes);
void		uninit_fifo(net_fifo* fifo);
void		set_fifo_max_bytes(net_fifo* fifo, size_t maxBytes);
status_t	fifo_enqueue_buffer(net_fifo* fifo, struct net_buffer* buffer);
ssize_t		fifo_dequeue_buffer(net_fifo* fifo, uint32 flags, bigtime_t timeout,
				struct net_buffer** _buffer);

// fifo instrumentation (stack-module private)
void		init_fifo_watermark(net_fifo_watermark* diagnostics,
				size_t limitBytes);
void		snapshot_fifo_watermark(net_fifo* fifo,
				net_fifo_watermark* diagnostics, net_fifo_watermark* snapshot,
				bool resetPeaks);
status_t	fifo_enqueue_buffer_tracked(net_fifo* fifo,
				struct net_buffer* buffer, net_fifo_watermark* diagnostics);
ssize_t		fifo_dequeue_buffer_tracked(net_fifo* fifo, uint32 flags,
				bigtime_t timeout, struct net_buffer** _buffer,
				net_fifo_watermark* diagnostics);
status_t	clear_fifo(net_fifo* fifo);
status_t	fifo_socket_enqueue_buffer(net_fifo* fifo, net_socket* socket,
				uint8 event, net_buffer* buffer);

// sojourn-time queue discipline (stack-module private)
void		init_fifo_codel(net_fifo_codel* codel, bigtime_t target,
				bigtime_t interval, size_t minBytes);
ssize_t		fifo_dequeue_buffer_codel(net_fifo* fifo, net_fifo_codel* codel,
				net_fifo_watermark* diagnostics, struct net_buffer** _buffer);

// timer
void		init_timer(net_timer* timer, net_timer_func hook, void* data);
void		set_timer(net_timer* timer, bigtime_t delay);
bool		cancel_timer(struct net_timer* timer);
status_t	wait_for_timer(struct net_timer* timer);
bool		is_timer_active(net_timer* timer);
bool		is_timer_running(net_timer* timer);
status_t	init_timers(void);
void		uninit_timers(void);

// syscall restart
bool		is_syscall(void);
bool		is_restarted_syscall(void);
void		store_syscall_restart_timeout(bigtime_t timeout);
bigtime_t	restore_syscall_restart_timeout(void);

#endif	// NET_UTILITY_H
