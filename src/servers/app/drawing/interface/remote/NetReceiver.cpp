/*
 * Copyright 2009, 2017, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include "NetReceiver.h"
#include "RemoteMessage.h"

#include "StreamingRingBuffer.h"

#include <NetEndpoint.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#define TRACE(x...)			/*debug_printf("NetReceiver: " x)*/
#define TRACE_ERROR(x...)	debug_printf("NetReceiver: " x)


NetReceiver::NetReceiver(BNetEndpoint *listener, StreamingRingBuffer *target,
	NewConnectionCallback newConnectionCallback, void *newConnectionCookie,
	ConnectionClosedCallback connectionClosedCallback)
	:
	fListener(listener),
	fTarget(target),
	fReceiverThread(-1),
	fStopThread(false),
	fNewConnectionCallback(newConnectionCallback),
	fNewConnectionCookie(newConnectionCookie),
	fConnectionClosedCallback(connectionClosedCallback),
	fEndpoint(newConnectionCallback == NULL ? listener : NULL)
{
	fReceiverThread = spawn_thread(_NetworkReceiverEntry, "network receiver",
		B_NORMAL_PRIORITY, this);
	resume_thread(fReceiverThread);
}


NetReceiver::~NetReceiver()
{
	fStopThread = true;
	fEndpoint.Unset();

	suspend_thread(fReceiverThread);
	resume_thread(fReceiverThread);
}


int32
NetReceiver::_NetworkReceiverEntry(void *data)
{
	NetReceiver *receiver = (NetReceiver *)data;
	if (receiver->fNewConnectionCallback)
		return receiver->_Listen();
	else
		return receiver->_Transfer();
}


status_t
NetReceiver::_Listen()
{
	status_t result = fListener->Listen();
	if (result != B_OK) {
		TRACE_ERROR("failed to listen on port: %s\n", strerror(result));
		return result;
	}

	while (!fStopThread) {
		fEndpoint.SetTo(fListener->Accept(5000));
		if (!fEndpoint.IsSet()) {
			TRACE("got NULL endpoint from accept\n");
			continue;
		}

		TRACE("new endpoint connection: %p\n", fEndpoint);

		if (fNewConnectionCallback != NULL
			&& fNewConnectionCallback(
				fNewConnectionCookie, *fEndpoint.Get()) != B_OK)
		{
			TRACE("connection callback rejected connection\n");
			continue;
		}

		_Transfer();

		// _Transfer() only returns when the peer went away (EOF or error).
		// Release the accepted connection right here instead of leaving it for
		// the next Accept()/_NewConnection() to replace: close our accepted
		// socket immediately, and let the owner drop the sender that holds a
		// dup of the same connection. Until both sides are closed the socket
		// lingers in CLOSE_WAIT; deferring that to the next client is what let
		// them accumulate.
		fEndpoint.Unset();
		if (fConnectionClosedCallback != NULL)
			fConnectionClosedCallback(fNewConnectionCookie);
	}

	return B_OK;
}


status_t
NetReceiver::_Transfer()
{
	int32 errorCount = 0;

	// In server (listening) mode a single thread both accepts connections and
	// pumps the accepted one, serving one client at a time. If that client's
	// peer vanishes without a clean close -- a half-open connection, which
	// BNetEndpoint::Receive() waits on forever because the accepted socket has
	// no receive timeout and this TCP stack sends no keepalive probes -- the
	// accept loop can never come back around. A fresh client then completes its
	// TCP handshake into the listen backlog but is never accepted and never gets
	// its RP_INIT_CONNECTION reply (black screen), while the abandoned sockets
	// pile up in CLOSE_WAIT. Watch the listener alongside the live connection so
	// a new client preempts a stale one: returning here lets _Listen() tear this
	// connection down and accept the waiting one. In client mode (no callback)
	// fEndpoint is the sole socket and there is nothing to preempt with.
	const bool watchListener = fNewConnectionCallback != NULL
		&& fListener != NULL && fListener != fEndpoint.Get();

	while (!fStopThread) {
		if (watchListener) {
			int connSocket = fEndpoint->Socket();
			int listenSocket = fListener->Socket();
			if (connSocket < 0)
				return B_ERROR;

			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(connSocket, &readSet);
			if (listenSocket >= 0)
				FD_SET(listenSocket, &readSet);

			int maxSocket = connSocket > listenSocket ? connSocket : listenSocket;
			int ready = select(maxSocket + 1, &readSet, NULL, NULL, NULL);
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				TRACE_ERROR("select failed, closing connection: %s\n",
					strerror(errno));
				return B_ERROR;
			}

			// A connection is waiting on the listener: give this one up so the
			// accept loop can replace it. Done even when the current connection
			// also has data -- the protocol drives a single client and the newest
			// one wins, matching _NewConnection()'s replace-on-connect model.
			if (listenSocket >= 0 && FD_ISSET(listenSocket, &readSet))
				return B_OK;

			if (!FD_ISSET(connSocket, &readSet))
				continue;
		}

		uint8 buffer[4096];
		int32 readSize = fEndpoint->Receive(buffer, sizeof(buffer));
		if (readSize < 0) {
			TRACE_ERROR("read failed, closing connection: %s\n",
				strerror(readSize));
			return readSize;
		}

		if (readSize == 0) {
			TRACE("read 0 bytes, retrying\n");
			snooze(100 * 1000);
			errorCount++;
			if (errorCount == 5) {
				TRACE_ERROR("failed to read, assuming disconnect\n");
				return B_ERROR;
			}

			continue;
		}

		errorCount = 0;
		status_t result = fTarget->Write(buffer, readSize);
		if (result != B_OK) {
			TRACE_ERROR("writing to ring buffer failed: %s\n",
				strerror(result));
			return result;
		}
	}

	return B_OK;
}
