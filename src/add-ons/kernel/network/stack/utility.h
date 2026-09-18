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
