/*
 * Copyright 2009, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */
#ifndef NET_RECEIVER_H
#define NET_RECEIVER_H

#include <AutoDeleter.h>
#include <OS.h>
#include <SupportDefs.h>

class BNetEndpoint;
class StreamingRingBuffer;

typedef status_t (*NewConnectionCallback)(void *cookie, BNetEndpoint &endpoint);
typedef void (*ConnectionClosedCallback)(void *cookie);


class NetReceiver {
public:
								NetReceiver(BNetEndpoint *endpoint,
									StreamingRingBuffer *target,
									NewConnectionCallback callback = NULL,
									void *newConnectionCookie = NULL,
									ConnectionClosedCallback closedCallback = NULL);
								~NetReceiver();

		BNetEndpoint *			Endpoint() { return fEndpoint.Get(); }

private:
static	int32					_NetworkReceiverEntry(void *data);
		status_t				_Listen();
		status_t				_Transfer();

		void					_AcceptCandidate();
		bool					_ReceiveCandidateData();
		void					_DropCandidate(const char *reason);

		BNetEndpoint *			fListener;
		StreamingRingBuffer *	fTarget;

		thread_id				fReceiverThread;
		bool					fStopThread;

		NewConnectionCallback	fNewConnectionCallback;
		void *					fNewConnectionCookie;
		ConnectionClosedCallback
								fConnectionClosedCallback;

		ObjectDeleter<BNetEndpoint>
								fEndpoint;

		// A connection accepted while another one is live. It only takes the
		// session over once it has proven it is a real client by sending a
		// valid first protocol frame; until then the live session keeps
		// running. See _Transfer() for the rationale.
		ObjectDeleter<BNetEndpoint>
								fCandidate;
		uint8					fCandidateBuffer[4096];
		size_t					fCandidateBufferUsed;
		bigtime_t				fCandidateDeadline;
};

#endif // NET_RECEIVER_H
