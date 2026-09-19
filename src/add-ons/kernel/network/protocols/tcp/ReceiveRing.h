/*
 * Copyright 2026, DeBeOS. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RECEIVE_RING_H
#define RECEIVE_RING_H


#include "tcp.h"


/*!	Lockless single-producer / single-consumer ring of in-order net_buffers.

	This is the ordered delivery FIFO for the TCP receive fast path (#61). The
	single RX consumer thread pushes contiguous, in-order segments here while
	holding the endpoint's fLock; the application reader drains them in
	ReadData() with fLock RELEASED. Producer and consumer never share a lock, so
	the reader's O(segments) coalesce no longer serialises the RX consumer on
	fLock -- which is the whole point of #61.

	Concurrency contract (enforced by the caller, TCPEndpoint):
	  - The PRODUCER side (Push / HasFreeSlot / Produced / FreeSlots) is only
	    ever entered while holding the endpoint fLock. Every path that feeds the
	    ring (the RX consumer, and the reader's own _DrainToRing flush of staged
	    data) holds fLock, so there is a single serialised producer stream.
	  - The CONSUMER side that MUTATES the ring (Read / advancing the head) is
	    only ever entered while holding the endpoint fReadLock (a separate
	    mutex), so there is a single serialised consumer stream. fReadLock is NOT
	    fLock, so the reader does not contend with the producer.
	  - The read-only byte-counter query Available() is called from BOTH sides:
	    the consumer (ReadData) and the producer (SegmentReceived's window
	    accounting, under fLock). It therefore acquire-loads BOTH counters so it
	    never races on a plain cross-thread read (see Available()). Consumed() and
	    Produced() are simple getters read only from their owning side.
	  - Teardown (Drain / destructor) runs single-threaded once both sides are
	    quiescent (the read syscall holds a socket reference, so the endpoint is
	    not freed under an in-flight reader).

	arm64 / weak-memory ordering: fTail and fBytesProduced are written only by
	the producer with release semantics (atomic_set64); the consumer reads them
	with acquire semantics (atomic_get64). fHead and fBytesConsumed are written
	only by the consumer with release semantics and read by the producer with
	acquire semantics. Haiku's atomic_set64/atomic_get64 are __ATOMIC_RELEASE /
	__ATOMIC_ACQUIRE, which is exactly the SPSC pairing: the slot store is
	published by the release-store of fTail and observed after the acquire-load
	of fTail, so the consumer never reads a slot the producer has not finished
	writing, and the producer never reuses a slot the consumer has not finished
	freeing. No torn indices (64-bit monotonic counters, slot = counter & mask).

	fCachedHead (#414): a producer-owned copy of fHead, refreshed from the
	atomic only when the cached view shows the ring at least half full. fHead is
	on a cache line the consumer dirties on every read syscall, so the acquire
	loads in Push()/FreeSlots() were a per-segment cross-core line transfer
	inside the RX consumer's fLock hold. Staleness is safe by monotonicity: the
	head only advances, so a stale cache only UNDER-counts free slots -- Push()
	never overruns, and the advertised window (derived from FreeSlots()) only
	skews smaller, never larger, bounded by the half-capacity refresh threshold.
*/
class ReceiveRing {
public:
							ReceiveRing();
							~ReceiveRing();

			status_t		Init(uint32 slots);
			bool			IsInitialized() const { return fSlots != NULL; }
			uint32			Capacity() const { return fCapacity; }

	// Producer side -- caller must hold fLock.
			bool			HasFreeSlot() const;
			uint32			FreeSlots() const;
			bool			Push(net_buffer* buffer);
			size_t			Produced() const { return (size_t)fBytesProduced; }

	// Consumer side -- caller must hold fReadLock.
			size_t			Available() const;
			size_t			Consumed() const { return (size_t)fBytesConsumed; }
			ssize_t			Read(size_t bytes, bool peek, net_buffer** _buffer);

	// Teardown -- caller must ensure quiescence.
			void			Drain();

private:
			void			_MaybeRefreshCachedHead() const;

private:
			net_buffer**	fSlots;
			uint32			fCapacity;		// power of two, or 0 if uninitialised
			uint32			fMask;

			// Producer-owned (written under fLock, read by consumer via acquire).
			int64			fTail;
			int64			fBytesProduced;
			mutable int64	fCachedHead;
				// producer's lazily refreshed view of fHead (see class comment);
				// mutable because the const producer queries maintain it

			// Consumer-owned (written under fReadLock, read by producer via acquire).
			int64			fHead;
			int64			fBytesConsumed;
};


#endif	// RECEIVE_RING_H
