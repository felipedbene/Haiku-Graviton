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

// Mirrors RP_SESSION_COOKIE_MAX_LENGTH from RemoteMessage.h, which this header
// deliberately does not include -- it drags in the drawing state and pattern
// types, and this one is included by the client too. NetReceiver.cpp asserts
// that the two agree.
static const size_t kMaxSessionCookieLength = 256;


class NetReceiver {
public:
								NetReceiver(BNetEndpoint *endpoint,
									StreamingRingBuffer *target,
									NewConnectionCallback callback = NULL,
									void *newConnectionCookie = NULL,
									ConnectionClosedCallback closedCallback = NULL,
									RemoteWireReader *wireReader = NULL,
									const char *sessionCookie = NULL,
									size_t sessionCookieLength = 0);
								~NetReceiver();

		BNetEndpoint *			Endpoint() { return fEndpoint.Get(); }

private:
static	int32					_NetworkReceiverEntry(void *data);
		status_t				_Listen();
		status_t				_Transfer();
		status_t				_TransferLoop();

		void					_AcceptCandidate();
		bool					_ReceiveCandidateData();
		void					_DropCandidate(const char *reason);
		bool					_ValidateCandidate(bigtime_t deadline);
		bool					_CookieMatches(const uint8 *cookie,
									size_t length) const;

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

		// The secret a connection must present, in server (listening) mode, to
		// become the session: app_server mints it per listener into an
		// owner-only file, so only a process that can read that file can take
		// the session. Empty in client mode; an empty one in server mode is
		// fatal (see _Listen()), never a reason to accept anything.
		char					fSessionCookie[kMaxSessionCookieLength];
		size_t					fSessionCookieLength;

		// A connection accepted while another one is live. It only takes the
		// session over once it has presented the session cookie; until then the
		// live session keeps running. See _Transfer() for the rationale.
		ObjectDeleter<BNetEndpoint>
								fCandidate;
		// Exactly the session-cookie frame, which is all the gate ever reads:
		// buffering more would forward unexamined bytes to the parser on
		// promotion. Header (6) + method and length (8) + the cookie.
		uint8					fCandidateBuffer[6 + 8
									+ kMaxSessionCookieLength];
		size_t					fCandidateBufferUsed;
		// Total length the candidate's first frame declared, or 0 while its
		// six header bytes are still incomplete. It bounds every further read
		// from the candidate.
		uint32					fCandidateFrameLength;
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
