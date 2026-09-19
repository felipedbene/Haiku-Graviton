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


// How long a newly accepted connection gets to send its first valid protocol
// frame before it is dropped and the live session continues undisturbed.
static const bigtime_t kCandidateTimeout = 10 * 1000 * 1000;


/*!	Whether \a buffer begins with a frame a genuine client would open with.
	Every client starts its stream with RP_INIT_CONNECTION (an empty message,
	total length exactly the 6 byte header) optionally followed by RP_HELLO;
	a reconnecting URP/1 client may lead with RP_HELLO directly, whose body is
	small but allowed to grow additively by the compatibility rule. Framing is
	little-endian by specification, so the fields are decoded explicitly
	rather than read through a host-order struct.

	Returns 1 for a valid first frame, 0 for definitely invalid, and -1 when
	fewer than the 6 header bytes have arrived so far.
*/
static int
validate_first_frame(const uint8 *buffer, size_t size)
{
	if (size < 6)
		return -1;

	uint16 code = (uint16)buffer[0] | ((uint16)buffer[1] << 8);
	uint32 length = (uint32)buffer[2] | ((uint32)buffer[3] << 8)
		| ((uint32)buffer[4] << 16) | ((uint32)buffer[5] << 24);

	if (code == RP_INIT_CONNECTION)
		return length == 6 ? 1 : 0;
	if (code == RP_HELLO)
		return length >= 6 && length <= 4096 ? 1 : 0;

	return 0;
}


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
	fEndpoint(newConnectionCallback == NULL ? listener : NULL),
	fCandidateBufferUsed(0),
	fCandidateDeadline(0),
	fCandidateValidated(false)
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
		// A candidate may be pending because it validated (it asked for the
		// session) or because the live session ended while it was still
		// being validated. Only a *validated* candidate is ever promoted:
		// otherwise a peer that connects and says nothing would inherit the
		// session for free whenever the real client disconnects, which is
		// the very takeover this gate exists to prevent. An unvalidated one
		// gets the rest of its deadline to prove itself first.
		if (fCandidate.IsSet() && !fCandidateValidated)
			_ValidateCandidate(fCandidateDeadline);

		if (fCandidate.IsSet() && fCandidateValidated) {
			fEndpoint.SetTo(fCandidate.Detach());
			fCandidateValidated = false;
		} else {
			if (fCandidate.IsSet())
				_DropCandidate("not validated in time");

			fEndpoint.SetTo(fListener->Accept(5000));
			if (!fEndpoint.IsSet()) {
				TRACE("got NULL endpoint from accept\n");
				continue;
			}
		}

		TRACE("new endpoint connection: %p\n", fEndpoint);

		if (fNewConnectionCallback != NULL
			&& fNewConnectionCallback(
				fNewConnectionCookie, *fEndpoint.Get()) != B_OK)
		{
			TRACE("connection callback rejected connection\n");
			fCandidateBufferUsed = 0;
			continue;
		}

		// Hand over whatever the connection already sent while it was being
		// validated as a takeover candidate; these bytes are the head of its
		// stream and must reach the parser before anything read below.
		if (fCandidateBufferUsed > 0) {
			status_t result = fTarget->Write(fCandidateBuffer,
				fCandidateBufferUsed);
			fCandidateBufferUsed = 0;
			if (result != B_OK) {
				// The head of this client's stream is lost, so its framing
				// can never resynchronise; drop the connection rather than
				// feed the parser a stream missing its first message.
				TRACE_ERROR("writing candidate data to ring buffer failed, "
					"dropping connection: %s\n", strerror(result));
				fEndpoint.Unset();
				if (fConnectionClosedCallback != NULL)
					fConnectionClosedCallback(fNewConnectionCookie);
				continue;
			}
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
	// a new client can replace a stale one. In client mode (no callback)
	// fEndpoint is the sole socket and there is nothing to preempt with.
	//
	// A new connection does NOT preempt the live one merely by connecting,
	// though. This protocol carries every keystroke of the session, so letting
	// a bare TCP connect tear down the desktop hands a trivial
	// session-kill/session-steal to anything that can reach the port (a port
	// scan, a health probe, a stray curl). A newly accepted connection is
	// therefore parked as a *candidate* and must prove it is a real client by
	// sending a valid first protocol frame (RP_INIT_CONNECTION or RP_HELLO,
	// which every client emits immediately on connect) within
	// kCandidateTimeout. Only then does it take the session over -- which
	// keeps the deliberate-reconnect and the dead-peer-recovery behaviour --
	// while junk connections are closed without the session ever noticing.
	const bool watchListener = fNewConnectionCallback != NULL
		&& fListener != NULL && fListener != fEndpoint.Get();

	while (!fStopThread) {
		if (watchListener) {
			int connSocket = fEndpoint->Socket();
			int listenSocket = fListener->Socket();
			int candidateSocket
				= fCandidate.IsSet() ? fCandidate->Socket() : -1;
			if (connSocket < 0)
				return B_ERROR;

			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(connSocket, &readSet);
			if (listenSocket >= 0)
				FD_SET(listenSocket, &readSet);
			if (candidateSocket >= 0)
				FD_SET(candidateSocket, &readSet);

			int maxSocket = connSocket;
			if (listenSocket > maxSocket)
				maxSocket = listenSocket;
			if (candidateSocket > maxSocket)
				maxSocket = candidateSocket;

			// While a candidate is pending, wake up periodically to enforce
			// its deadline even if no socket becomes readable.
			struct timeval candidateWait = { 1, 0 };
			int ready = select(maxSocket + 1, &readSet, NULL, NULL,
				fCandidate.IsSet() ? &candidateWait : NULL);
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				TRACE_ERROR("select failed, closing connection: %s\n",
					strerror(errno));
				return B_ERROR;
			}

			if (fCandidate.IsSet() && system_time() > fCandidateDeadline)
				_DropCandidate("timeout waiting for first frame");

			// The pending candidate is served BEFORE a new connection is
			// accepted. Reversing this would let a connection arriving in
			// the same select round evict a candidate whose valid first
			// frame is already readable -- so a prober connecting once a
			// second would starve every real client out of the session.
			if (fCandidate.IsSet() && candidateSocket >= 0
				&& FD_ISSET(candidateSocket, &readSet)) {
				if (_ReceiveCandidateData()) {
					// The candidate proved itself; let _Listen() tear this
					// connection down and promote it.
					return B_OK;
				}
			}

			if (listenSocket >= 0 && FD_ISSET(listenSocket, &readSet))
				_AcceptCandidate();

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


void
NetReceiver::_AcceptCandidate()
{
	// A candidate that has already started speaking keeps the slot: it is
	// the one most likely to be a real client, and letting every new
	// connection displace it is how a repeatedly connecting prober would
	// keep real clients from ever being validated. A silent candidate is
	// replaced, so a stalled or half-open one cannot block the slot either
	// (it also still has its own deadline).
	if (fCandidate.IsSet()) {
		if (fCandidateBufferUsed > 0) {
			// Leave the pending connection in the listen backlog; it is
			// accepted once this candidate resolves.
			return;
		}

		_DropCandidate("replaced by newer connection");
	}

	fCandidate.SetTo(fListener->Accept(0));
	if (!fCandidate.IsSet())
		return;

	fCandidateBufferUsed = 0;
	fCandidateValidated = false;
	fCandidateDeadline = system_time() + kCandidateTimeout;
	TRACE("accepted takeover candidate\n");
}


/*!	Waits, within \a deadline, for the pending candidate to produce a valid
	first frame. Used when the live session ended while a candidate was still
	unvalidated: it must still prove itself before it inherits the session.
	Sets fCandidateValidated and returns true on success; drops the candidate
	and returns false otherwise.
*/
bool
NetReceiver::_ValidateCandidate(bigtime_t deadline)
{
	while (fCandidate.IsSet() && !fStopThread) {
		bigtime_t remaining = deadline - system_time();
		if (remaining <= 0) {
			_DropCandidate("timeout waiting for first frame");
			return false;
		}

		int candidateSocket = fCandidate->Socket();
		if (candidateSocket < 0) {
			_DropCandidate("candidate socket gone");
			return false;
		}

		struct timeval timeout;
		timeout.tv_sec = remaining / 1000000;
		timeout.tv_usec = remaining % 1000000;

		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(candidateSocket, &readSet);

		int ready = select(candidateSocket + 1, &readSet, NULL, NULL,
			&timeout);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			_DropCandidate("select failed while validating");
			return false;
		}

		if (ready == 0) {
			_DropCandidate("timeout waiting for first frame");
			return false;
		}

		if (_ReceiveCandidateData())
			return true;
	}

	return false;
}


/*!	Reads whatever the candidate connection has sent so far and validates the
	first frame once its header is complete. Returns true when the candidate
	has proven itself and should take over the session; on garbage or EOF the
	candidate is dropped and false is returned.
*/
bool
NetReceiver::_ReceiveCandidateData()
{
	int32 readSize = fCandidate->Receive(
		fCandidateBuffer + fCandidateBufferUsed,
		sizeof(fCandidateBuffer) - fCandidateBufferUsed);
	if (readSize <= 0) {
		_DropCandidate("closed or failed before first frame");
		return false;
	}

	fCandidateBufferUsed += readSize;

	int valid = validate_first_frame(fCandidateBuffer, fCandidateBufferUsed);
	if (valid < 0) {
		// Header not complete yet; keep waiting within the deadline.
		return false;
	}

	if (valid == 0) {
		_DropCandidate("invalid first frame");
		return false;
	}

	fCandidateValidated = true;
	return true;
}


void
NetReceiver::_DropCandidate(const char *reason)
{
	TRACE_ERROR("dropping takeover candidate: %s\n", reason);
	fCandidate.Unset();
	fCandidateBufferUsed = 0;
	fCandidateValidated = false;
}
