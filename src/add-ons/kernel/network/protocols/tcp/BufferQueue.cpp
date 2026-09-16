/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "BufferQueue.h"

#include <KernelExport.h>
#include <arpa/inet.h>

#include <util/AutoLock.h>


//#define TRACE_BUFFER_QUEUE
#ifdef TRACE_BUFFER_QUEUE
#	define TRACE(x) dprintf x
#else
#	define TRACE(x)
#endif

#if DEBUG_TCP_BUFFER_QUEUE
#	define VERIFY() Verify();
#else
#	define VERIFY() ;
#endif


BufferQueue::BufferQueue(size_t maxBytes)
	:
	fMaxBytes(maxBytes),
	fNumBytes(0),
	fContiguousBytes(0),
	fFirstSequence(0),
	fLastSequence(0),
	fPushPointer(0)
{
	recursive_lock_init(&fLock, "tcp buffer queue");
}


BufferQueue::~BufferQueue()
{
	// free up any buffers left in the queue

	net_buffer *buffer;
	while ((buffer = fList.RemoveHead()) != NULL) {
		gBufferModule->free(buffer);
	}

	recursive_lock_destroy(&fLock);
}


void
BufferQueue::SetMaxBytes(size_t maxBytes)
{
	RecursiveLocker _(fLock);
	fMaxBytes = maxBytes;
}


void
BufferQueue::SetInitialSequence(tcp_sequence sequence)
{
	RecursiveLocker _(fLock);
	TRACE(("BufferQueue@%p::SetInitialSequence(%" B_PRIu32 ")\n", this,
		sequence.Number()));

	fFirstSequence = fLastSequence = sequence;
}



void
BufferQueue::Add(net_buffer *buffer)
{
	RecursiveLocker _(fLock);
	Add(buffer, fLastSequence);
}


void
BufferQueue::Add(net_buffer *buffer, tcp_sequence sequence)
{
	RecursiveLocker _(fLock);
	TRACE(("BufferQueue@%p::Add(buffer %p, size %" B_PRIu32 ", sequence %"
		B_PRIu32 ")\n", this, buffer, buffer->size, sequence.Number()));
	TRACE(("  in: first: %" B_PRIu32 ", last: %" B_PRIu32 ", num: %lu, cont: "
		"%lu\n", fFirstSequence.Number(), fLastSequence.Number(), fNumBytes,
		fContiguousBytes));
	VERIFY();

	if (tcp_sequence(sequence + buffer->size) <= fFirstSequence
		|| buffer->size == 0) {
		// This buffer does not contain any data of interest
		gBufferModule->free(buffer);
		return;
	}
	if (sequence < fFirstSequence) {
		// this buffer contains data that is already long gone - trim it
		gBufferModule->remove_header(buffer,
			(fFirstSequence - sequence).Number());
		sequence = fFirstSequence;
	}

	if (fList.IsEmpty() || sequence >= fLastSequence) {
		// we usually just add the buffer to the end of the queue
		fList.Add(buffer);
		buffer->sequence = sequence.Number();

		if (sequence == fLastSequence
			&& fLastSequence - fFirstSequence == fNumBytes) {
			// there is no hole in the buffer, we can make the whole buffer
			// available
			fContiguousBytes += buffer->size;
		}

		fLastSequence = sequence + buffer->size;
		fNumBytes += buffer->size;

		TRACE(("  out0: first: %" B_PRIu32 ", last: %" B_PRIu32 ", num: %"
			B_PRIuSIZE ", cont: %" B_PRIuSIZE "\n",	fFirstSequence.Number(),
			fLastSequence.Number(), fNumBytes, fContiguousBytes));
		VERIFY();
		return;
	}

	if (fLastSequence < sequence + buffer->size)
		fLastSequence = sequence + buffer->size;

	// find the place where to insert the buffer into the queue

	SegmentList::ReverseIterator iterator = fList.GetReverseIterator();
	net_buffer *previous = NULL;
	net_buffer *next = NULL;
	while ((previous = iterator.Next()) != NULL) {
		if (sequence >= previous->sequence) {
			// The new fragment can be inserted after this one
			break;
		}

		next = previous;
	}

	// check if we have duplicate data, and remove it if that is the case
	if (previous != NULL) {
		if (sequence == previous->sequence) {
			// we already have at least part of this data - ignore new data
			// whenever it makes sense (because some TCP implementations send
			// bogus data when probing the window)
			if (previous->size >= buffer->size) {
				gBufferModule->free(buffer);
				buffer = NULL;
			} else {
				fList.Remove(previous);
				fNumBytes -= previous->size;
				gBufferModule->free(previous);
			}
		} else if (tcp_sequence(previous->sequence + previous->size)
				>= sequence + buffer->size) {
			// We already know this data
			gBufferModule->free(buffer);
			buffer = NULL;
		} else if (tcp_sequence(previous->sequence + previous->size)
				> sequence) {
			// We already have the first part of this buffer
			gBufferModule->remove_header(buffer,
				(previous->sequence + previous->size - sequence).Number());
			sequence = previous->sequence + previous->size;
		}
	}

	// "next" always starts at or after the buffer sequence
	ASSERT(next == NULL || buffer == NULL || next->sequence >= sequence);

	while (buffer != NULL && next != NULL
		&& tcp_sequence(sequence + buffer->size) > next->sequence) {
		// we already have at least part of this data
		if (tcp_sequence(next->sequence + next->size)
				<= sequence + buffer->size) {
			net_buffer *remove = next;
			next = (net_buffer *)next->link.next;

			fList.Remove(remove);
			fNumBytes -= remove->size;
			gBufferModule->free(remove);
		} else if (tcp_sequence(next->sequence) > sequence) {
			// We have the end of this buffer already
			gBufferModule->remove_trailer(buffer,
				(sequence + buffer->size - next->sequence).Number());
		} else {
			// We already have this data
			gBufferModule->free(buffer);
			buffer = NULL;
		}
	}

	if (buffer == NULL) {
		TRACE(("  out1: first: %" B_PRIu32 ", last: %" B_PRIu32 ", num: %"
			B_PRIuSIZE ", cont: %" B_PRIuSIZE "\n", fFirstSequence.Number(),
			fLastSequence.Number(), fNumBytes, fContiguousBytes));
		VERIFY();
		return;
	}

	fList.InsertBefore(next, buffer);
	buffer->sequence = sequence.Number();
	fNumBytes += buffer->size;

	// we might need to update the number of bytes available

	if (fLastSequence - fFirstSequence == fNumBytes)
		fContiguousBytes = fNumBytes;
	else if (fFirstSequence + fContiguousBytes == sequence) {
		// the complicated case: the new segment may have connected almost all
		// buffers in the queue (but not all, or the above would be true)

		do {
			fContiguousBytes += buffer->size;

			buffer = (struct net_buffer *)buffer->link.next;
		} while (buffer != NULL
			&& fFirstSequence + fContiguousBytes == buffer->sequence);
	}

	TRACE(("  out2: first: %" B_PRIu32 ", last: %" B_PRIu32 ", num: %lu, cont: "
		"%lu\n", fFirstSequence.Number(), fLastSequence.Number(), fNumBytes,
		fContiguousBytes));
	VERIFY();
}


/*!	Removes all data in the queue up to the \a sequence number as specified.

	NOTE: If there are missing segments in the buffers to be removed,
	fContiguousBytes is not maintained correctly!
*/
status_t
BufferQueue::RemoveUntil(tcp_sequence sequence)
{
	RecursiveLocker _(fLock);
	TRACE(("BufferQueue@%p::RemoveUntil(sequence %" B_PRIu32 ")\n", this,
		sequence.Number()));
	VERIFY();

	if (sequence < fFirstSequence)
		return B_OK;

	SegmentList::Iterator iterator = fList.GetIterator();
	tcp_sequence lastRemoved = fFirstSequence;
	net_buffer *buffer = NULL;
	while ((buffer = iterator.Next()) != NULL && buffer->sequence < sequence) {
		ASSERT(lastRemoved == buffer->sequence);
			// This assures that the queue has no holes, and fContiguousBytes
			// is maintained correctly.

		if (sequence >= buffer->sequence + buffer->size) {
			// remove this buffer completely
			iterator.Remove();
			fNumBytes -= buffer->size;

			fContiguousBytes -= buffer->size;
			lastRemoved = buffer->sequence + buffer->size;
			gBufferModule->free(buffer);
		} else {
			// remove the header as far as needed
			size_t size = (sequence - buffer->sequence).Number();
			gBufferModule->remove_header(buffer, size);

			buffer->sequence += size;
			fNumBytes -= size;
			fContiguousBytes -= size;
			break;
		}
	}

	if (fList.IsEmpty())
		fFirstSequence = fLastSequence;
	else
		fFirstSequence = fList.Head()->sequence;

	VERIFY();
	return B_OK;
}


/*!	Clones the requested data in the buffer queue into the provided \a buffer.
*/
status_t
BufferQueue::Get(net_buffer *buffer, tcp_sequence sequence, size_t bytes)
{
	RecursiveLocker _(fLock);
	TRACE(("BufferQueue@%p::Get(sequence %" B_PRIu32 ", bytes %lu)\n", this,
		sequence.Number(), bytes));
	VERIFY();

	if (bytes == 0)
		return B_OK;

	if (sequence >= fLastSequence || sequence < fFirstSequence) {
		// we don't have the requested data
		return B_BAD_VALUE;
	}
	if (tcp_sequence(sequence + bytes) > fLastSequence)
		bytes = (fLastSequence - sequence).Number();

	size_t bytesLeft = bytes;

	// find first buffer matching the sequence

	SegmentList::Iterator iterator = fList.GetIterator();
	net_buffer *source = NULL;
	while ((source = iterator.Next()) != NULL) {
		if (sequence < source->sequence + source->size)
			break;
	}

	if (source == NULL)
		panic("we should have had that data...");
	if (tcp_sequence(source->sequence) > sequence) {
		panic("source %p, sequence = %" B_PRIu32 " (%" B_PRIu32 ")\n", source,
			source->sequence, sequence.Number());
	}

	// clone the data

	uint32 offset = (sequence - source->sequence).Number();

	while (source != NULL && bytesLeft > 0) {
		size_t size = min_c(source->size - offset, bytesLeft);
		status_t status = gBufferModule->append_cloned(buffer, source, offset,
			size);
		if (status < B_OK)
			return status;

		bytesLeft -= size;
		offset = 0;
		source = iterator.Next();
	}

	VERIFY();
	return B_OK;
}


/*!	Clones \a bytes bytes from the start of the queue into a new buffer without
	removing them. Runs entirely under the lock; used for MSG_PEEK, which is
	rare and must not disturb the queue.
*/
status_t
BufferQueue::_GetCloned(size_t bytes, net_buffer** _buffer)
{
	RecursiveLocker _(fLock);

	if (bytes > Available())
		bytes = Available();
	if (bytes == 0) {
		*_buffer = NULL;
		return B_OK;
	}

	net_buffer* buffer = gBufferModule->create(256);
	if (buffer == NULL)
		return B_NO_MEMORY;

	size_t bytesLeft = bytes;
	SegmentList::Iterator iterator = fList.GetIterator();
	net_buffer* source = NULL;
	status_t status = B_OK;
	while (bytesLeft > 0 && (source = iterator.Next()) != NULL) {
		size_t size = min_c(source->size, bytesLeft);
		status = gBufferModule->append_cloned(buffer, source, 0, size);
		if (status < B_OK)
			break;
		bytesLeft -= size;
	}

	if (status < B_OK && buffer->size == 0) {
		gBufferModule->free(buffer);
		return status;
	}

	*_buffer = buffer;
	return B_OK;
}


/*!	Re-inserts \a pieces (a contiguous run that was detached from the head of
	the queue) back at the front, restoring the byte counters. Used only to
	avoid losing already-acknowledged data when the lock-free coalesce below
	hits an allocation failure -- a rare path, but it must not drop data.
*/
void
BufferQueue::_PrependContiguous(SegmentList& pieces, size_t bytes)
{
	RecursiveLocker _(fLock);

	// Insert in reverse so the run ends up in its original order at the head.
	net_buffer* piece;
	while ((piece = pieces.Tail()) != NULL) {
		pieces.Remove(piece);
		if (fList.IsEmpty())
			fList.Add(piece);
		else
			fList.InsertBefore(fList.First(), piece);
		fFirstSequence = piece->sequence;
	}

	fNumBytes += bytes;
	fContiguousBytes += bytes;
}


/*!	Creates a new buffer containing \a bytes bytes from the start of the
	buffer queue. If \a remove is \c true, the data is removed from the
	queue, if not, the data is cloned from the queue.

	For the common removing case the head buffers are detached from the queue
	while holding the lock, but the O(segments) coalesce (clone the data-node
	references into a single buffer, free the sources) is done with the lock
	RELEASED. This is what keeps the RX consumer's Add() from waiting on the
	whole dequeue: it only ever contends for the brief detach, not the clone
	(#61 -- Option B left the clone under the lock, which relocated the
	contention into HOLD).
*/
status_t
BufferQueue::Get(size_t bytes, bool remove, net_buffer **_buffer)
{
	if (!remove)
		return _GetCloned(bytes, _buffer);

	// Phase 1 -- detach the head run under the lock. splitClone carries the
	// single partial buffer (head fragment) when the request does not fall on
	// a buffer boundary; at most one clone happens under the lock.
	SegmentList detached;
	net_buffer* splitClone = NULL;
	{
		RecursiveLocker locker(fLock);

		if (bytes > Available())
			bytes = Available();
		if (bytes == 0) {
			*_buffer = NULL;
			return B_OK;
		}

		size_t remaining = bytes;
		net_buffer* buf;
		while (remaining > 0 && (buf = fList.First()) != NULL) {
			if (buf->size <= remaining) {
				fList.Remove(buf);
				fFirstSequence += buf->size;
				fNumBytes -= buf->size;
				fContiguousBytes -= buf->size;
				remaining -= buf->size;
				detached.Add(buf);
			} else {
				// Partial head fragment: clone the wanted prefix (one bounded
				// clone under the lock), trim the source, leave it queued.
				net_buffer* clone = gBufferModule->create(256);
				if (clone != NULL && gBufferModule->append_cloned(clone, buf, 0,
						remaining) == B_OK) {
					gBufferModule->remove_header(buf, remaining);
					buf->sequence += remaining;
					fFirstSequence += remaining;
					fNumBytes -= remaining;
					fContiguousBytes -= remaining;
					splitClone = clone;
				} else if (clone != NULL)
					gBufferModule->free(clone);
				break;
			}
		}
	}

	// Phase 2 -- coalesce with the lock released. Reuse the first detached
	// buffer as the base and clone the rest of the run into it.
	if (detached.IsEmpty()) {
		// Only a partial first buffer (or nothing on allocation failure).
		*_buffer = splitClone;
		return splitClone != NULL ? B_OK : B_NO_MEMORY;
	}

	net_buffer* buffer = detached.RemoveHead();
	net_buffer* source;
	while ((source = detached.RemoveHead()) != NULL) {
		if (gBufferModule->append_cloned(buffer, source, 0, source->size)
				!= B_OK) {
			// Out of memory mid-coalesce: put the still-owned run (this source
			// + the remainder + the split fragment) back so no data is lost,
			// and return what we have already coalesced.
			size_t requeued = source->size;
			SegmentList back;
			back.Add(source);
			net_buffer* rest;
			while ((rest = detached.RemoveHead()) != NULL) {
				requeued += rest->size;
				back.Add(rest);
			}
			if (splitClone != NULL) {
				requeued += splitClone->size;
				back.Add(splitClone);
			}
			_PrependContiguous(back, requeued);
			*_buffer = buffer;
			return B_OK;
		}
		gBufferModule->free(source);
	}

	if (splitClone != NULL) {
		if (gBufferModule->append_cloned(buffer, splitClone, 0, splitClone->size)
				== B_OK) {
			gBufferModule->free(splitClone);
		} else {
			SegmentList back;
			back.Add(splitClone);
			_PrependContiguous(back, splitClone->size);
		}
	}

	*_buffer = buffer;
	return B_OK;
}


size_t
BufferQueue::Available(tcp_sequence sequence) const
{
	RecursiveLocker _(fLock);
	if (sequence > (fFirstSequence + fContiguousBytes).Number())
		return 0;

	return (fContiguousBytes + fFirstSequence - sequence).Number();
}


tcp_sequence
BufferQueue::NextSequence() const
{
	// fFirstSequence and fContiguousBytes are moved in lockstep by a concurrent
	// reader's Get() (front removal advances one and shrinks the other by the
	// same amount), so their sum is invariant -- but only if read together, so
	// take the lock to avoid a torn read that would corrupt fReceiveNext.
	RecursiveLocker _(fLock);
	return fFirstSequence + fContiguousBytes;
}


void
BufferQueue::SetPushPointer()
{
	RecursiveLocker _(fLock);
	if (fList.IsEmpty())
		fPushPointer = 0;
	else
		fPushPointer = fList.Tail()->sequence + fList.Tail()->size;
}


int
BufferQueue::PopulateSackInfo(tcp_sequence sequence, int maxSackCount,
	tcp_sack* sacks)
{
	RecursiveLocker _(fLock);
	SegmentList::ReverseIterator iterator = fList.GetReverseIterator();
	net_buffer* buffer = iterator.Next();

	int sackCount = 0;
	TRACE(("BufferQueue::PopulateSackInfo() %" B_PRIu32 "\n",
		sequence.Number()));
	while (buffer != NULL && buffer->sequence > sequence) {
		if (buffer->sequence + buffer->size < sacks[sackCount].left_edge) {
			if (sackCount + 1 == maxSackCount)
				break;
			++sackCount;
			sacks[sackCount].left_edge = buffer->sequence;
			sacks[sackCount].right_edge = buffer->sequence + buffer->size;
		} else {
			sacks[sackCount].left_edge = buffer->sequence;
			if (sacks[sackCount].right_edge == 0)
				sacks[sackCount].right_edge = buffer->sequence + buffer->size;
		}

		buffer = iterator.Next();
	}

	if (sacks[0].left_edge != 0) {
		for (int i = 0; i <= sackCount; ++i) {
			sacks[i].left_edge = htonl(sacks[i].left_edge);
			sacks[i].right_edge = htonl(sacks[i].right_edge);
		}
		++sackCount;
	}

	return sackCount;
}

#if DEBUG_TCP_BUFFER_QUEUE

/*!	Perform a sanity check of the whole queue.
*/
void
BufferQueue::Verify() const
{
	ASSERT(Available() == 0 || fList.First() != NULL);

	if (fList.First() == NULL) {
		ASSERT(fNumBytes == 0);
		return;
	}

	SegmentList::ConstIterator iterator = fList.GetIterator();
	size_t numBytes = 0;
	size_t contiguousBytes = 0;
	bool contiguous = true;
	tcp_sequence last = fFirstSequence;

	while (net_buffer* buffer = iterator.Next()) {
		if (contiguous && buffer->sequence == last)
			contiguousBytes += buffer->size;
		else
			contiguous = false;

		ASSERT(last <= buffer->sequence);
		ASSERT(buffer->size > 0);

		numBytes += buffer->size;
		last = buffer->sequence + buffer->size;
	}

	ASSERT(last == fLastSequence);
	ASSERT(contiguousBytes == fContiguousBytes);
	ASSERT(numBytes == fNumBytes);
}


void
BufferQueue::Dump() const
{
	SegmentList::ConstIterator iterator = fList.GetIterator();
	int32 number = 0;
	while (net_buffer* buffer = iterator.Next()) {
		kprintf("      %" B_PRId32 ". buffer %p, sequence %" B_PRIu32
			", size %" B_PRIu32 "\n", ++number, buffer, buffer->sequence,
			buffer->size);
	}
}

#endif	// DEBUG_TCP_BUFFER_QUEUE
