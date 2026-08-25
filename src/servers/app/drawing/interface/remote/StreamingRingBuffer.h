/*
 * Copyright 2009, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */
#ifndef STREAMING_RING_BUFFER_H
#define STREAMING_RING_BUFFER_H

#include <OS.h>
#include <SupportDefs.h>
#include <Locker.h>

class StreamingRingBuffer {
public:
			/*!	With \a discardWithoutReader a full buffer that currently has
				no registered reader makes Write() discard instead of block.
				Without it (the default) Write() always blocks until space
				appears, which deadlocks the writer if nobody ever reads. */
								StreamingRingBuffer(size_t bufferSize,
									bool discardWithoutReader = false);
								~StreamingRingBuffer();

		status_t				InitCheck();

		// blocking read and write
		int32					Read(void *buffer, size_t length,
									bool onlyBlockOnNoData = false);
		status_t				Write(const void *buffer, size_t length);

		void					MakeEmpty();

		// Reader registration; only meaningful with discardWithoutReader.
		// ClearReader() is a no-op unless \a reader is the current one, so a
		// reader being torn down cannot clear its successor.
		void					SetReader(void *reader);
		void					ClearReader(void *reader);

private:
		bool					fReaderWaiting;
		bool					fWriterWaiting;
		bool					fCancelRead;
		bool					fCancelWrite;
		sem_id					fReaderNotifier;
		sem_id					fWriterNotifier;

		BLocker					fReaderLocker;
		BLocker					fWriterLocker;
		BLocker					fDataLocker;

		uint8 *					fBuffer;
		size_t					fBufferSize;
		size_t					fReadable;
		int32					fReadPosition;
		int32					fWritePosition;

		bool					fDiscardWithoutReader;
		void *					fReader;
};

#endif // STREAMING_RING_BUFFER_H
