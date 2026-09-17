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
	// successor having already claimed it. ClearReader() also cancels a Read()
	// the sender thread may be parked in -- and arms that cancel even when the
	// thread has not parked yet -- so the thread is guaranteed to observe the
	// stop request and exit rather than block forever; see StreamingRingBuffer.
	fSource->ClearReader(this);

	// Join the sender thread before touching the endpoint it uses. Its loop
	// calls fEndpoint->Send(), so closing the socket underneath a still-running
	// thread would be a use-after-close. The old suspend/resume dance did not
	// wait for the thread at all, which is also why the endpoint below could not
	// safely be closed here before.
	if (fSenderThread >= 0) {
		status_t result;
		wait_for_thread(fSenderThread, &result);
	}

	// Close this sender's dup of the accepted connection. The copy constructor
	// of BNetEndpoint dup()s the socket, so the NetReceiver closing its own copy
	// at disconnect is not enough: until this second reference is closed too the
	// peer's FIN leaves the connection stuck in CLOSE_WAIT, and one leaked on
	// every reconnect (issue #308). Nothing else owns fEndpoint -- _NewConnection
	// hands it to us and never deletes it on the success path.
	delete fEndpoint;
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
