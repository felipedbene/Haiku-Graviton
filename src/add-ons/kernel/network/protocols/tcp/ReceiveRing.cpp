/*
 * Copyright 2026, DeBeOS. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include "ReceiveRing.h"

#include <stdlib.h>
#include <string.h>


ReceiveRing::ReceiveRing()
	:
	fSlots(NULL),
	fCapacity(0),
	fMask(0),
	fTail(0),
	fBytesProduced(0),
	fCachedHead(0),
	fHead(0),
	fBytesConsumed(0)
{
}


ReceiveRing::~ReceiveRing()
{
	Drain();
	free(fSlots);
}


/*!	Allocates (or reallocates) the slot array. The caller MUST guarantee the
	ring is empty and quiescent -- this is called from the constructor and from
	_PrepareReceivePath() before any data flows, both under fLock with no reader
	active.
*/
status_t
ReceiveRing::Init(uint32 slots)
{
	uint32 capacity = 1;
	while (capacity < slots)
		capacity <<= 1;

	net_buffer** array = (net_buffer**)malloc(capacity * sizeof(net_buffer*));
	if (array == NULL)
		return B_NO_MEMORY;
	memset(array, 0, capacity * sizeof(net_buffer*));

	// The ring is required to be empty here; drop any old allocation.
	if (fSlots != NULL) {
		Drain();
		free(fSlots);
	}

	fSlots = array;
	fCapacity = capacity;
	fMask = capacity - 1;
	fTail = fHead = 0;
	fCachedHead = 0;
	fBytesProduced = fBytesConsumed = 0;
	return B_OK;
}


// #pragma mark - producer side (caller holds fLock)


/*!	Refreshes the producer's cached view of the consumer head, but only once
	the cached view shows the ring at least half full. The threshold keeps the
	acquire-load of the consumer-dirtied fHead line off the per-segment fast
	path (a stale head is safe -- it only under-counts free slots) while
	bounding two effects of staleness: Push() keeps working long before the
	ring is genuinely full, and the advertised window derived from FreeSlots()
	never sags below half the ring's slot capacity merely because the cache is
	old. At genuinely high occupancy this degenerates to one load per call --
	exactly the pre-#414 behaviour, so it is never a regression.
*/
void
ReceiveRing::_MaybeRefreshCachedHead() const
{
	if (fTail - fCachedHead >= (int64)(fCapacity / 2))
		fCachedHead = atomic_get64(const_cast<int64*>(&fHead));
}


uint32
ReceiveRing::FreeSlots() const
{
	if (fSlots == NULL)
		return 0;
	_MaybeRefreshCachedHead();
	int64 used = fTail - fCachedHead;
	if (used >= (int64)fCapacity)
		return 0;
	return fCapacity - (uint32)used;
}


bool
ReceiveRing::HasFreeSlot() const
{
	return FreeSlots() > 0;
}


bool
ReceiveRing::Push(net_buffer* buffer)
{
	if (fSlots == NULL)
		return false;

	_MaybeRefreshCachedHead();
	if ((fTail - fCachedHead) >= (int64)fCapacity)
		return false;

	// Write the slot, then publish it: the size counter and the tail index are
	// release-stores, so a consumer that acquire-loads either one is guaranteed
	// to observe the completed slot store.
	fSlots[fTail & fMask] = buffer;
	atomic_set64(&fBytesProduced, fBytesProduced + (int64)buffer->size);
	atomic_set64(&fTail, fTail + 1);
	return true;
}


// #pragma mark - consumer side (caller holds fReadLock)


size_t
ReceiveRing::Available() const
{
	if (fSlots == NULL)
		return 0;
	// Available() is read from the consumer (ReadData, under fReadLock) but also
	// from the PRODUCER thread for receive-window accounting (_ReceiveBuffered /
	// _ReceiveFree in SegmentReceived, under fLock but not fReadLock). Both
	// counters are therefore cross-thread here, so both are acquire-loaded: a
	// plain read of fBytesConsumed would race the consumer's release-store in
	// Read(). The value only ever skews stale-low (less consumed), which shrinks
	// the advertised window -- the safe direction -- but the read must still be
	// atomic to be correct on any architecture, not only arm64's aligned 64-bit
	// loads (#61).
	int64 produced = atomic_get64(const_cast<int64*>(&fBytesProduced));
	int64 consumed = atomic_get64(const_cast<int64*>(&fBytesConsumed));
	int64 available = produced - consumed;
	return available > 0 ? (size_t)available : 0;
}


/*!	Coalesces up to \a bytes bytes from the head of the ring into a freshly
	created net_buffer. When \a peek is false the consumed slots are freed and
	the head advanced; a slot straddling the byte boundary is trimmed in place
	and left at the head. When \a peek is true nothing is mutated or advanced.

	Returns the number of bytes placed in *_buffer, or a negative error. Like
	BufferQueue::Get(), it never drops data on a mid-coalesce allocation
	failure: it stops and returns what it already gathered (the untouched
	sources stay in the ring).
*/
ssize_t
ReceiveRing::Read(size_t bytes, bool peek, net_buffer** _buffer)
{
	*_buffer = NULL;
	if (fSlots == NULL)
		return 0;

	int64 tail = atomic_get64(&fTail);
	int64 produced = atomic_get64(&fBytesProduced);
	size_t available = produced > fBytesConsumed
		? (size_t)(produced - fBytesConsumed) : 0;
	if (bytes > available)
		bytes = available;
	if (bytes == 0)
		return 0;

	net_buffer* out = gBufferModule->create(256);
	if (out == NULL)
		return B_NO_MEMORY;

	int64 head = fHead;
	int64 consumedBytes = 0;
	size_t remaining = bytes;
	size_t peekOffset = 0;
	status_t status = B_OK;

	while (remaining > 0 && head < tail) {
		net_buffer* source = fSlots[head & fMask];
		size_t sourceSize = source->size;
		size_t offset = peek ? peekOffset : 0;
		size_t size = min_c(sourceSize - offset, remaining);

		status = gBufferModule->append_cloned(out, source, offset, size);
		if (status < B_OK)
			break;

		remaining -= size;

		if (peek) {
			if (offset + size >= sourceSize) {
				head++;
				peekOffset = 0;
			} else
				peekOffset = offset + size;
			continue;
		}

		consumedBytes += (int64)size;
		if (size == sourceSize) {
			fSlots[head & fMask] = NULL;
			gBufferModule->free(source);
			head++;
		} else {
			// Partial head slot: trim the wanted prefix out of the source and
			// leave it queued for the next read.
			gBufferModule->remove_header(source, size);
		}
	}

	size_t got = bytes - remaining;
	if (got == 0) {
		gBufferModule->free(out);
		return status < B_OK ? status : 0;
	}

	if (!peek) {
		// Publish the consumption: the byte counter first, then the head index
		// (both release-stores) so the producer's acquire-load of fHead never
		// reuses a slot before the free above is visible.
		atomic_set64(&fBytesConsumed, fBytesConsumed + consumedBytes);
		atomic_set64(&fHead, head);
	}

	*_buffer = out;
	return (ssize_t)got;
}


// #pragma mark - teardown (quiescent)


void
ReceiveRing::Drain()
{
	if (fSlots == NULL)
		return;

	int64 head = fHead;
	int64 tail = fTail;
	while (head < tail) {
		net_buffer* buffer = fSlots[head & fMask];
		if (buffer != NULL) {
			gBufferModule->free(buffer);
			fSlots[head & fMask] = NULL;
		}
		head++;
	}

	fHead = head;
	fBytesConsumed = fBytesProduced;
}
