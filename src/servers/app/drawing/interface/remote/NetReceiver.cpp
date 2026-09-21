/*
 * Copyright 2009, 2017, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include "NetReceiver.h"
#include "RemoteMessage.h"
#include "RemoteWireReader.h"

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

// How long a single attempt to hand input to the target buffer may park before
// the loop goes back to watching its sockets. Short enough that the accept path
// stays responsive while the consumer is behind, long enough that the normal
// case (space available at once) never pays for the timeout.
static const bigtime_t kTargetWriteTimeout = 100 * 1000;

// How long to wait in select() while input is still queued for the target
// buffer. Only the retry cadence: the connection is not read from again until
// the queued input has been handed over.
static const bigtime_t kPendingInputRetry = 50 * 1000;


// Bytes of a message header -- a uint16 code and a uint32 length, little-endian
// by specification. This is all the candidate gate ever reads, so it is also
// the size of the candidate buffer.
static const size_t kFrameHeaderSize = 6;


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
	if (size < kFrameHeaderSize)
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
	ConnectionClosedCallback connectionClosedCallback,
	RemoteWireReader *wireReader)
	:
	fListener(listener),
	fTarget(target),
	fWireReader(wireReader),
	fReceiverThread(-1),
	fStopThread(false),
	fNewConnectionCallback(newConnectionCallback),
	fNewConnectionCookie(newConnectionCookie),
	fConnectionClosedCallback(connectionClosedCallback),
	fEndpoint(newConnectionCallback == NULL ? listener : NULL),
	fCandidateBufferUsed(0),
	fCandidateDeadline(0),
	fCandidateValidated(false),
	fPendingUsed(0),
	fPendingOffset(0)
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
		// EVERY connection goes through the candidate gate, not just one
		// arriving while a session is live. A connection accepted when no
		// session exists used to become the session unvalidated, which
		// handed the drawing state replay (and from then on the desktop) to
		// anything that could open the port and stay silent. So: accept into
		// the candidate slot, require a valid first frame within the
		// deadline, and only then promote.
		if (!fCandidate.IsSet()) {
			fCandidate.SetTo(fListener->Accept(5000));
			if (!fCandidate.IsSet()) {
				// BNetEndpoint::Accept() closes the listening socket when
				// accept() itself fails, so the usual "just try again" is a
				// silent busy loop that can never accept anything once that has
				// happened. Report it and give up instead of spinning.
				if (fListener->Socket() < 0) {
					TRACE_ERROR("listening socket was closed (%s), no further "
						"connections can be accepted\n",
						strerror(fListener->Error()));
					return B_ERROR;
				}

				TRACE("got NULL endpoint from accept\n");
				continue;
			}

			fCandidateBufferUsed = 0;
			fCandidateValidated = false;
			fCandidateDeadline = system_time() + kCandidateTimeout;
		}

		// A candidate validated inside _Transfer() (it asked for the session
		// while another one was live) is promoted straight away; one still
		// unvalidated -- freshly accepted here, or left over because the live
		// session ended mid-validation -- gets the rest of its deadline to
		// prove itself and is dropped if it does not.
		if (!fCandidateValidated && !_ValidateCandidate(fCandidateDeadline))
			continue;

		fEndpoint.SetTo(fCandidate.Detach());
		fCandidateValidated = false;
		_DiscardPendingInput();

		TRACE("new endpoint connection: %p\n", fEndpoint);

		if (fNewConnectionCallback != NULL
			&& fNewConnectionCallback(
				fNewConnectionCookie, *fEndpoint.Get()) != B_OK)
		{
			TRACE("connection callback rejected connection\n");
			fCandidateBufferUsed = 0;
			continue;
		}

		// Hand over the first frame's header, the only thing read while the
		// connection was being validated; it is the head of its stream and must
		// reach the parser before anything read below. It goes through the same
		// pending queue as everything else so this handover cannot block the
		// accept loop either -- _Transfer() drains the queue before it reads
		// from the connection again, which keeps the stream in order.
		if (fCandidateBufferUsed > 0) {
			memcpy(fPendingBuffer, fCandidateBuffer, fCandidateBufferUsed);
			fPendingUsed = fCandidateBufferUsed;
			fPendingOffset = 0;
			fCandidateBufferUsed = 0;
		}

		_Transfer();
		_DiscardPendingInput();

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

			// Input already read from the connection but not yet taken by the
			// target buffer has to be handed over before more is read, or the
			// stream would be reordered. Stop watching the connection until the
			// queue drains -- the kernel receive buffer holds the rest, and
			// leaving the connection unread is precisely the back pressure the
			// stalled consumer calls for.
			const bool pendingInput = _HasPendingInput();

			fd_set readSet;
			FD_ZERO(&readSet);
			if (!pendingInput)
				FD_SET(connSocket, &readSet);
			if (listenSocket >= 0)
				FD_SET(listenSocket, &readSet);
			if (candidateSocket >= 0)
				FD_SET(candidateSocket, &readSet);

			int maxSocket = pendingInput ? -1 : connSocket;
			if (listenSocket > maxSocket)
				maxSocket = listenSocket;
			if (candidateSocket > maxSocket)
				maxSocket = candidateSocket;

			// While a candidate is pending, wake up periodically to enforce
			// its deadline even if no socket becomes readable; while input is
			// queued, wake up sooner to retry handing it over.
			struct timeval selectTimeout = { 1, 0 };
			if (pendingInput) {
				selectTimeout.tv_sec = 0;
				selectTimeout.tv_usec = (suseconds_t)kPendingInputRetry;
			}

			int ready = select(maxSocket + 1, &readSet, NULL, NULL,
				(fCandidate.IsSet() || pendingInput) ? &selectTimeout : NULL);
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

			if (pendingInput) {
				// Retry the handover with a bounded wait. Parking here without
				// a limit is what wedged the whole interface: this one thread
				// is both the accept loop and the connection's reader, so a client
				// that stops draining its socket (filling the send ring, which
				// stalls the consumer of this buffer) used to stop the server
				// accepting anything at all -- every later client connected and
				// then read nothing for the rest of the app_server's life.
				status_t result = _WritePendingInput(kTargetWriteTimeout);
				if (result != B_OK && result != B_TIMED_OUT) {
					TRACE_ERROR("writing to ring buffer failed: %s\n",
						strerror(result));
					return result;
				}

				continue;
			}

			if (!FD_ISSET(connSocket, &readSet))
				continue;
		} else if (_HasPendingInput()) {
			// Client mode: nothing to stay responsive for, so just finish the
			// handover before reading more.
			status_t result = _WritePendingInput(B_INFINITE_TIMEOUT);
			if (result != B_OK) {
				TRACE_ERROR("writing to ring buffer failed: %s\n",
					strerror(result));
				return result;
			}
		}

		int32 readSize = fEndpoint->Receive(fPendingBuffer,
			sizeof(fPendingBuffer));
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

		// There are two ways to hand the bytes just read to the parser, and
		// which one applies is a property of the side this receiver runs on
		// rather than a runtime choice. The two states are mutually exclusive by
		// construction: a wire reader is only ever passed in by the client
		// (which passes no connection callbacks, so fNewConnectionCallback is
		// NULL, watchListener is false, and _Listen() -- the only producer of
		// queued input -- never runs).
		//
		// Client side: the stream may have switched to compressed segments, and
		// the reader turns it back into the plain message stream the parser
		// above expects, being a passthrough until the switch. It writes
		// through to the target as it decodes and so cannot report partial
		// progress -- which is harmless precisely here, where this thread reads
		// one socket and accepts nothing.
		if (fWireReader != NULL) {
			status_t result = fWireReader->Process(fPendingBuffer, readSize);
			if (result != B_OK) {
				TRACE_ERROR("decoding the inbound stream failed: %s\n",
					strerror(result));
				return result;
			}

			continue;
		}

		// Server side: this thread is also the accept loop, so the handover has
		// to be interruptible. Queue the bytes and hand them over with a bounded
		// wait; whatever the target would not take is retried from the select()
		// above, which keeps the listener watched throughout. Inbound is always
		// plain here, so there is nothing to decode.
		fPendingUsed = readSize;
		fPendingOffset = 0;

		status_t result = _WritePendingInput(watchListener
			? kTargetWriteTimeout : B_INFINITE_TIMEOUT);
		if (result != B_OK && result != B_TIMED_OUT) {
			TRACE_ERROR("writing to ring buffer failed: %s\n",
				strerror(result));
			return result;
		}
	}

	return B_OK;
}


/*!	Hands as much of the queued input as fits to the target buffer, waiting
	at most \a timeout for space. Returns B_TIMED_OUT with input still queued when
	the target stayed full, which is not an error: the caller goes back to
	watching its sockets and retries.
*/
status_t
NetReceiver::_WritePendingInput(bigtime_t timeout)
{
	if (!_HasPendingInput())
		return B_OK;

	size_t written = 0;
	status_t result = fTarget->Write(fPendingBuffer + fPendingOffset,
		fPendingUsed - fPendingOffset, timeout, written);
	fPendingOffset += written;

	if (!_HasPendingInput()) {
		fPendingUsed = 0;
		fPendingOffset = 0;
	}

	return result;
}


void
NetReceiver::_DiscardPendingInput()
{
	fPendingUsed = 0;
	fPendingOffset = 0;
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
	first frame -- the gate every connection passes before it becomes the
	session. Sets fCandidateValidated and returns true on success; drops the
	candidate and returns false otherwise.
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

		// The listener is watched alongside the candidate so that a silent
		// candidate cannot make a real client wait out the whole validation
		// deadline before it is even looked at. _AcceptCandidate() replaces
		// a candidate that has said nothing and keeps one that has started
		// speaking, and the backlog is FIFO, so the earliest waiting client
		// is the one that gets the slot.
		int listenSocket = fListener != NULL ? fListener->Socket() : -1;

		struct timeval timeout;
		timeout.tv_sec = remaining / 1000000;
		timeout.tv_usec = remaining % 1000000;

		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(candidateSocket, &readSet);
		if (listenSocket >= 0)
			FD_SET(listenSocket, &readSet);

		int maxSocket = candidateSocket > listenSocket
			? candidateSocket : listenSocket;
		int ready = select(maxSocket + 1, &readSet, NULL, NULL, &timeout);
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

		// The candidate is served first: its readable first frame decides
		// the slot before any newly arriving connection can displace it.
		if (FD_ISSET(candidateSocket, &readSet)) {
			if (_ReceiveCandidateData())
				return true;
			continue;
		}

		if (listenSocket >= 0 && FD_ISSET(listenSocket, &readSet))
			_AcceptCandidate();
	}

	return false;
}


/*!	Reads the candidate connection's first frame header and validates it once
	complete. Returns true when the candidate has proven itself and should take
	over the session; on garbage or EOF the candidate is dropped and false is
	returned.

	Never reads past the header. The gate only ever validates those six bytes,
	so reading further would buffer -- and, on promotion, forward to the parser
	-- bytes nothing has looked at: a connection opening with a well-formed
	RP_INIT_CONNECTION followed by arbitrary junk in the same segment would be
	promoted and have all of it handed over. The rest of a real client's stream
	costs nothing to leave in the kernel receive buffer, where _Transfer() reads
	it after promotion, in order.
*/
bool
NetReceiver::_ReceiveCandidateData()
{
	if (fCandidateBufferUsed >= sizeof(fCandidateBuffer)) {
		// A full header is always decided by validate_first_frame(), so this
		// cannot be reached with a candidate still pending.
		_DropCandidate("first frame header already complete");
		return false;
	}

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
