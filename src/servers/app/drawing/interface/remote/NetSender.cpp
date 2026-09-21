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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#define TRACE(x...)			/*debug_printf("NetSender: " x)*/
#define TRACE_ERROR(x...)	debug_printf("NetSender: " x)


// How long a single Send() may block before the loop gets control back. Only
// the polling interval: a slow but progressing client is not disturbed.
static const bigtime_t kSendChunkTimeout = 1 * 1000 * 1000;

// How long the client may accept no data at all before the connection is
// considered dead and torn down. A peer that has stopped reading (or whose
// window has closed for good) would otherwise hold this thread in send()
// forever, and with it the whole remote interface -- see _NetworkSender().
static const bigtime_t kSendStallTimeout = 15 * 1000 * 1000;


NetSender::NetSender(BNetEndpoint *endpoint, StreamingRingBuffer *source)
	:
	fEndpoint(endpoint),
	fSource(source),
	fSenderThread(-1),
	fStopThread(false)
{
	// Bound how long a single Send() can block. Without this the drain thread
	// parks in send() for as long as the peer withholds window space, which is
	// unbounded, cannot be joined by the destructor, and (once the send ring
	// behind it fills) stalls the drawing producers too.
	int socket = fEndpoint->Socket();
	if (socket >= 0) {
		struct timeval timeout;
		timeout.tv_sec = kSendChunkTimeout / 1000000;
		timeout.tv_usec = kSendChunkTimeout % 1000000;
		setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	}

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

		uint8* position = buffer;
		bigtime_t lastProgress = system_time();
		while (readSize > 0) {
			int32 sendSize = fEndpoint->Send(position, readSize);
			if (sendSize < 0) {
				// BNetEndpoint::Send() fails without calling send() at all when
				// the endpoint has no socket, leaving errno untouched, so the
				// socket is checked before errno is trusted. Retrying a
				// classified-as-transient stale errno here would spin.
				int socket = fEndpoint->Socket();
				if (socket < 0) {
					TRACE_ERROR("send endpoint has no socket, stopping\n");
					return B_ERROR;
				}

				status_t error = errno;
				if (error == EINTR)
					continue;

				// The socket send timeout expired, so the client has taken
				// nothing for a while. That is normal for a moment (a slow
				// link, a busy client), but a peer that never reads again must
				// not hold this thread forever: the send ring behind it fills,
				// the drawing producers block on it, and with them the thread
				// that also accepts new connections -- which is how one dead
				// client used to make the interface refuse every later one.
				// Give up on the connection instead and shut the socket down,
				// so the receive side sees the peer as gone and tears the
				// session down in its normal path.
				if (error == EWOULDBLOCK || error == ETIMEDOUT) {
					if (fStopThread)
						return B_INTERRUPTED;

					if (system_time() - lastProgress < kSendStallTimeout)
						continue;

					TRACE_ERROR("client accepted no data for %" B_PRIdBIGTIME
						" us, dropping connection\n",
						system_time() - lastProgress);
					shutdown(socket, SHUT_RDWR);
					return B_TIMED_OUT;
				}

				TRACE_ERROR("sending data failed: %s\n", strerror(error));
				return error;
			}

			// A short send must resume from where it stopped. Without advancing
			// position, a partial Send() re-sent the leading bytes and dropped
			// the tail, corrupting the framed stream for the rest of the session
			// (defect D2).
			position += sendSize;
			readSize -= sendSize;
			if (sendSize > 0)
				lastProgress = system_time();
		}
	}

	return B_OK;
}
