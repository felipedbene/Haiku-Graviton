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

	fCachedHead (#414): a producer-owned copy of fHead, so the per-segment
	producer queries need not load the consumer-written fHead. The saving is not
	the instruction, it is the memory: this structure is 56 bytes and
	deliberately NOT padded, so the producer indices (fTail/fBytesProduced) and
	the consumer indices (fHead/fBytesConsumed) share one cache line which the
	reader dirties on every read syscall -- each producer load of fHead is
	therefore a remote line fetch, taken inside the RX consumer's fLock hold.
	Measured: reinstating three such loads per segment cost about 6.5% of
	16-flow receive goodput on a c8gn.4xlarge. Splitting the two index groups
	onto separate lines is a separate and plausibly larger win; it is noted
	here, not done here.

	Staleness is safe by monotonicity: the head only advances, so a stale cache
	can only UNDER-count free slots. Every query is nonetheless EXACT, because
	each refreshes before it would answer in the direction the staleness could
	get wrong: Push()/HasFreeSlot() before declaring the ring full,
	HasFreeSlots() before answering "not enough", FreeSlots() always (it is the
	ADVERTISED WINDOW input via _ReceiveFree(), where an under-estimate costs
	throughput rather than being merely conservative -- hence also
	HasFreeSlots(), which lets _ReceiveFree() stay exact without the load
	whenever its byte budget is the binding term).
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
			bool			HasFreeSlots(uint32 count) const;
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
			void			_RefreshCachedHead() const;
			void			_MaybeRefreshCachedHead() const;

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
