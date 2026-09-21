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
class RemoteWireReader;
class StreamingRingBuffer;

typedef status_t (*NewConnectionCallback)(void *cookie, BNetEndpoint &endpoint);
typedef void (*ConnectionClosedCallback)(void *cookie);


class NetReceiver {
public:
								NetReceiver(BNetEndpoint *endpoint,
									StreamingRingBuffer *target,
									NewConnectionCallback callback = NULL,
									void *newConnectionCookie = NULL,
									ConnectionClosedCallback closedCallback = NULL,
									RemoteWireReader *wireReader = NULL);
								~NetReceiver();

		BNetEndpoint *			Endpoint() { return fEndpoint.Get(); }

private:
static	int32					_NetworkReceiverEntry(void *data);
		status_t				_Listen();
		status_t				_Transfer();

		void					_AcceptCandidate();
		bool					_ReceiveCandidateData();
		void					_DropCandidate(const char *reason);
		bool					_ValidateCandidate(bigtime_t deadline);

		bool					_HasPendingInput() const
									{ return fPendingOffset < fPendingUsed; }
		status_t				_WritePendingInput(bigtime_t timeout);
		void					_DiscardPendingInput();

		BNetEndpoint *			fListener;
		StreamingRingBuffer *	fTarget;

		// Client side only: de-frames and decompresses the inbound stream on
		// its way into the target buffer once the server has switched to
		// compressed segments. NULL on the server side, where the inbound
		// direction is always plain.
		RemoteWireReader *		fWireReader;

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
		bool					fCandidateValidated;

		// Input read from the live connection that the target buffer has not
		// taken yet. Holding it here is what lets the receive loop keep
		// accepting while the consumer is behind; see _Transfer().
		uint8					fPendingBuffer[4096];
		size_t					fPendingUsed;
		size_t					fPendingOffset;
};

#endif // NET_RECEIVER_H
