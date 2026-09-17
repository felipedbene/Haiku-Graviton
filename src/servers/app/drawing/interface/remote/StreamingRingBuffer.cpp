/*
 * Copyright 2009, 2017, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include "StreamingRingBuffer.h"

#include <Autolock.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifdef CLIENT_COMPILE
#define TRACE_ALWAYS(x...)		printf("StreamingRingBuffer: " x)
#else
#define TRACE_ALWAYS(x...)		debug_printf("StreamingRingBuffer: " x)
#endif

#define TRACE(x...)				/*TRACE_ALWAYS(x)*/
#define TRACE_ERROR(x...)		TRACE_ALWAYS(x)


StreamingRingBuffer::StreamingRingBuffer(size_t bufferSize,
	bool discardWithoutReader)
	:
	fReaderWaiting(false),
	fWriterWaiting(false),
	fCancelRead(false),
	fCancelWrite(false),
	fReaderNotifier(-1),
	fWriterNotifier(-1),
	fReaderLocker("StreamingRingBuffer reader"),
	fWriterLocker("StreamingRingBuffer writer"),
	fDataLocker("StreamingRingBuffer data"),
	fBuffer(NULL),
	fBufferSize(bufferSize),
	fReadable(0),
	fReadPosition(0),
	fWritePosition(0),
	fDiscardWithoutReader(discardWithoutReader),
	fReader(NULL)
{
	fReaderNotifier = create_sem(0, "StreamingRingBuffer read notify");
	fWriterNotifier = create_sem(0, "StreamingRingBuffer write notify");

	fBuffer = (uint8 *)malloc(fBufferSize);
	if (fBuffer == NULL)
		fBufferSize = 0;
}


StreamingRingBuffer::~StreamingRingBuffer()
{
	delete_sem(fReaderNotifier);
	delete_sem(fWriterNotifier);
	free(fBuffer);
}


status_t
StreamingRingBuffer::InitCheck()
{
	if (fReaderNotifier < 0)
		return fReaderNotifier;
	if (fWriterNotifier < 0)
		return fWriterNotifier;
	if (fBuffer == NULL)
		return B_NO_MEMORY;

	return B_OK;
}


int32
StreamingRingBuffer::Read(void *buffer, size_t length, bool onlyBlockOnNoData)
{
	BAutolock readerLock(fReaderLocker);
	if (!readerLock.IsLocked())
		return B_ERROR;

	BAutolock dataLock(fDataLocker);
	if (!dataLock.IsLocked())
		return B_ERROR;

	int32 readSize = 0;
	while (length > 0) {
		size_t copyLength = min_c(length, fBufferSize - fReadPosition);
		copyLength = min_c(copyLength, fReadable);

		if (copyLength == 0) {
			if (onlyBlockOnNoData && readSize > 0)
				return readSize;

			// Honour a cancel requested (under fDataLocker) since the
			// caller's last stop check, before committing to park. Without
			// this a ClearReader()/MakeEmpty() that ran while this thread
			// was between that check and here would not find fReaderWaiting
			// set, so its wake would be lost and this Read() would block
			// forever.
			if (fCancelRead) {
				fCancelRead = false;
				return B_CANCELED;
			}

			fReaderWaiting = true;
			dataLock.Unlock();

			status_t result;
			do {
				TRACE("waiting in reader\n");
				result = acquire_sem(fReaderNotifier);
				TRACE("done waiting in reader with status: %#" B_PRIx32 "\n",
					result);
			} while (result == B_INTERRUPTED);

			if (result != B_OK)
				return result;

			if (!dataLock.Lock()) {
				TRACE_ERROR("failed to acquire data lock\n");
				return B_ERROR;
			}

			if (fCancelRead) {
				TRACE("read canceled\n");
				fCancelRead = false;
				return B_CANCELED;
			}

			continue;
		}

		// support discarding input
		if (buffer != NULL) {
			memcpy(buffer, fBuffer + fReadPosition, copyLength);
			buffer = (uint8 *)buffer + copyLength;
		}

		fReadPosition = (fReadPosition + copyLength) % fBufferSize;
		fReadable -= copyLength;
		readSize += copyLength;
		length -= copyLength;

		if (fWriterWaiting) {
			release_sem_etc(fWriterNotifier, 1, B_DO_NOT_RESCHEDULE);
			fWriterWaiting = false;
		}
	}

	return readSize;
}


status_t
StreamingRingBuffer::Write(const void *buffer, size_t length)
{
	BAutolock writerLock(fWriterLocker);
	if (!writerLock.IsLocked())
		return B_ERROR;

	BAutolock dataLock(fDataLocker);
	if (!dataLock.IsLocked())
		return B_ERROR;

	while (length > 0) {
		// Nothing is draining this buffer, so waiting for space would wait
		// forever. Discard instead: the writer is a drawing producer whose
		// output has nowhere to go, and a reader that attaches later starts
		// from an empty buffer and a full repaint anyway.
		if (fDiscardWithoutReader && fReader == NULL)
			return B_OK;

		size_t copyLength = min_c(length, fBufferSize - fWritePosition);
		copyLength = min_c(copyLength, fBufferSize - fReadable);

		if (copyLength == 0) {
			fWriterWaiting = true;
			dataLock.Unlock();

			status_t result;
			do {
				TRACE("waiting in writer\n");
				result = acquire_sem(fWriterNotifier);
				TRACE("done waiting in writer with status: %#" B_PRIx32 "\n",
					result);
			} while (result == B_INTERRUPTED);

			if (result != B_OK)
				return result;

			if (!dataLock.Lock()) {
				TRACE_ERROR("failed to acquire data lock\n");
				return B_ERROR;
			}

			if (fCancelWrite) {
				TRACE("write canceled\n");
				fCancelWrite = false;
				return B_CANCELED;
			}

			continue;
		}

		memcpy(fBuffer + fWritePosition, buffer, copyLength);
		fWritePosition = (fWritePosition + copyLength) % fBufferSize;
		fReadable += copyLength;

		buffer = (uint8 *)buffer + copyLength;
		length -= copyLength;

		if (fReaderWaiting) {
			release_sem_etc(fReaderNotifier, 1, B_DO_NOT_RESCHEDULE);
			fReaderWaiting = false;
		}
	}

	return B_OK;
}


void
StreamingRingBuffer::MakeEmpty()
{
	BAutolock dataLock(fDataLocker);
	if (!dataLock.IsLocked())
		return;

	fReadPosition = fWritePosition = 0;
	fReadable = 0;

	if (fWriterWaiting) {
		release_sem_etc(fWriterNotifier, 1, 0);
		fWriterWaiting = false;
		fCancelWrite = true;
	}

	if (fReaderWaiting) {
		release_sem_etc(fReaderNotifier, 1, 0);
		fReaderWaiting = false;
		fCancelRead = true;
	}
}


void
StreamingRingBuffer::SetReader(void *reader)
{
	BAutolock dataLock(fDataLocker);
	if (!dataLock.IsLocked())
		return;

	// A prior ClearReader() may have left the read armed for cancel on behalf of
	// the departed reader (see below). Clear it so this fresh reader's first
	// Read() is serviced normally instead of returning a spurious B_CANCELED,
	// which would kill the new sender's drain thread and blank the display.
	fCancelRead = false;
	fReader = reader;
}


void
StreamingRingBuffer::ClearReader(void *reader)
{
	BAutolock dataLock(fDataLocker);
	if (!dataLock.IsLocked())
		return;

	// Guarded so that a reader shutting down after its successor has already
	// registered cannot clear the live one.
	if (fReader != reader)
		return;

	fReader = NULL;

	// Release a writer that is waiting for space it is never going to get. It
	// re-checks the discard condition and drops its data instead. Deliberately
	// not a cancel: dropping output while unread is normal, not an error.
	if (fWriterWaiting) {
		release_sem_etc(fWriterNotifier, 1, 0);
		fWriterWaiting = false;
	}

	// Cancel the reader too. The only reader is the sender thread being torn
	// down; without this its owner cannot join it (it would block forever on an
	// empty buffer) and so cannot close the socket -- which is what leaked a
	// CLOSE_WAIT per reconnect (issue #308). fCancelRead is armed even when no
	// reader is currently parked to close the lost-wakeup window where the thread
	// is between its stop check and parking in Read(): Read() re-checks
	// fCancelRead under fDataLocker before parking and exits instead of blocking.
	// SetReader() disarms it for the successor.
	fCancelRead = true;
	if (fReaderWaiting) {
		release_sem_etc(fReaderNotifier, 1, 0);
		fReaderWaiting = false;
	}
}
