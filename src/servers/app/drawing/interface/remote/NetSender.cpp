/*
 * Copyright 2009, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include "NetSender.h"

#include "StreamingRingBuffer.h"

#include <NetEndpoint.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACE(x...)			/*debug_printf("NetSender: " x)*/
#define TRACE_ERROR(x...)	debug_printf("NetSender: " x)


NetSender::NetSender(BNetEndpoint *endpoint, StreamingRingBuffer *source)
	:
	fEndpoint(endpoint),
	fSource(source),
	fSenderThread(-1),
	fStopThread(false)
{
	// Claim the source before the thread exists, so that no drawing output is
	// discarded between this connection being accepted and the drain starting.
	fSource->SetReader(this);

	fSenderThread = spawn_thread(_NetworkSenderEntry, "network sender",
		B_NORMAL_PRIORITY, this);
	resume_thread(fSenderThread);
}


NetSender::~NetSender()
{
	fStopThread = true;

	// Give up the claim: from here on the source has no drain and must discard
	// rather than block its writers. Guarded inside ClearReader() against a
	// successor having already claimed it.
	fSource->ClearReader(this);

	suspend_thread(fSenderThread);
	resume_thread(fSenderThread);
}


int32
NetSender::_NetworkSenderEntry(void *data)
{
	NetSender *sender = (NetSender *)data;
	status_t result = sender->_NetworkSender();

	// The drain has stopped -- usually because the client went away and Send()
	// failed. The NetSender object outlives this thread, so without this the
	// source would keep a dead reader registered and go on blocking its
	// writers forever. ClearReader() ignores us if a newer sender has already
	// taken the source over.
	sender->fSource->ClearReader(sender);
	return result;
}


status_t
NetSender::_NetworkSender()
{
	while (!fStopThread) {
		uint8 buffer[4096];
		int32 readSize = fSource->Read(buffer, sizeof(buffer), true);
		if (readSize < 0) {
			TRACE_ERROR("read failed, stopping sender thread: %s\n",
				strerror(readSize));
			return readSize;
		}

		while (readSize > 0) {
			int32 sendSize = fEndpoint->Send(buffer, readSize);
			if (sendSize < 0) {
				TRACE_ERROR("sending data failed: %s\n", strerror(sendSize));
				return sendSize;
			}

			readSize -= sendSize;
		}
	}

	return B_OK;
}
