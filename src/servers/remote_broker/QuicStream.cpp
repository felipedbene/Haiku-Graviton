/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 *
 * The QUIC front door's transport: OpenSSL's own QUIC stack (3.5 is the first
 * release with the server side), so the broker gains no new library. See
 * graviton/docs/remote-desktop-broker.md, "QUIC front door (M5a)".
 */

#include "QuicStream.h"

#include "WebSocketStream.h"
	// RelayFilter

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <errno.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


#define TRACE_ERROR(x...)	fprintf(stderr, "QuicStream: " x)


// How long one wait inside OpenSSL may last before the caller's deadline and
// the interrupt flags are looked at again. SSL_poll() cannot wait on anything
// but QUIC objects, so a takeover or the other relay direction ending is
// noticed at this granularity; it bounds the latency of *giving up*, never of
// data, which wakes the wait as soon as it arrives.
static const bigtime_t kWaitSlice = 100 * 1000;

// The bound on a stalled relay write toward the client, for the same reason
// and with the same value as the WebSocket relay (WebSocketStream.cpp): it
// must stay below app_server's 15 s zero-progress drop.
static const bigtime_t kRelayWriteTimeout = 10 * 1000 * 1000;

#ifdef SSL_VALUE_QUIC_MAX_PENDING_CONNS
// Connections the stack may hold handshaking before the accept loop takes
// them. The accept loop takes them as fast as they come, so this only bounds
// pre-accept work under a flood; OpenSSL's default is 256.
static const uint64 kMaxPendingConnections = 16;
#endif


static void
log_openssl_error(const char* what)
{
	unsigned long error = ERR_get_error();
	char buffer[256] = "unknown error";
	if (error != 0)
		ERR_error_string_n(error, buffer, sizeof(buffer));
	TRACE_ERROR("%s: %s\n", what, buffer);
	ERR_clear_error();
}


static int
select_alpn(SSL* ssl, const unsigned char** out, unsigned char* outLength,
	const unsigned char* in, unsigned int inLength, void* arg)
{
	// Exactly one protocol. A client offering anything else -- or nothing --
	// fails the handshake with no_application_protocol, before any byte of
	// the remote protocol is exchanged.
	if (SSL_select_next_proto((unsigned char**)out, outLength,
			(const unsigned char*)QUIC_ALPN_URP1, QUIC_ALPN_URP1_LENGTH, in,
			inLength) == OPENSSL_NPN_NEGOTIATED) {
		return SSL_TLSEXT_ERR_OK;
	}

	return SSL_TLSEXT_ERR_ALERT_FATAL;
}


// #pragma mark - peer addresses


/*	The listener's datagram BIO is wrapped in a filter that records peer
	addresses (and, until #790 lands, carries the errno workaround below).

	OpenSSL 3.5 has no call that returns a QUIC connection's peer address, and
	the broker's per-address accounting (failure backoff, the per-address share
	of handshake slots) needs one. So the listener learns it itself, from the
	two places it can see:

	- Its datagram BIO is wrapped in a filter that hands the stack at most one
	  datagram per read and remembers whom it came from. The stack's
	  demultiplexer reads, then processes what it read before it reads again
	  (quic_demux.c, ossl_quic_demux_pump()), so with a batch of one, "the
	  datagram being processed" and "the last datagram read" are the same.
	- The new-pending-connection callback runs while the port processes the
	  datagram that creates the connection -- which, with address validation
	  on, is the client's Initial that carries a valid Retry token. So the
	  address recorded there is the validated one.

	The callback stores that address on the connection object, where QuicStream
	picks it up once the connection is accepted. Both run inside the stack's
	event processing, under the domain lock, so the "last peer" needs no lock
	of its own. The cost is a read call per datagram, which at remote-desktop
	rates is noise.
*/


static int
peer_address_index()
{
	static int sIndex = -1;
	static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
	struct Init {
		// Owns the recorded address, so that a connection the stack discards
		// before it is ever accepted does not leak it.
		static void Free(void* parent, void* pointer, CRYPTO_EX_DATA* data,
			int index, long argument, void* cookie)
		{
			delete (in_addr_t*)pointer;
		}

		static void Do()
		{
			sIndex = SSL_get_ex_new_index(0, (void*)"peer address", NULL,
				NULL, Free);
		}
	};
	pthread_once(&sOnce, Init::Do);
	return sIndex;
}


struct PeerRecordingState {
	in_addr_t	lastPeer;
	bool		hasLastPeer;
};


// ---- Haiku-Graviton #790 workaround: begin ---------------------------------
//
// TEMPORARY. When the openssl3 recipe carries the #790 patch, delete this
// block and keep only its #else branch; nothing else in the file changes.
// Build with QUIC_ERRNO_WORKAROUND=0 (jam -sQUIC_ERRNO_WORKAROUND=0, or
// -DQUIC_ERRNO_WORKAROUND=0) to run #790's acceptance check 2: rd766, built
// the same way, against a broker without it.
//
// OpenSSL packs a system error as ERR_SYSTEM_FLAG | (errno & INT_MAX)
// (include/openssl/err.h, ERR_GET_REASON), and BIO_err_is_non_fatal()
// (crypto/bio/bio_lib.c) unpacks it and compares against EAGAIN. Haiku's errno
// values are negative -- EAGAIN is B_WOULD_BLOCK, 0x8000000b -- so the packing
// strips the sign, the comparison fails, and "no datagram yet" reads as a
// dead network: the QUIC stack shuts its port down on the very first empty
// non-blocking read and no handshake ever completes. Seen on DeBeOS with
// openssl3 3.5.7 ("system library::No Error (11)", then "quic network
// error"). So the datagram calls restate a transient failure in the one form
// that survives the packing, BIO_R_NON_FATAL. On other systems this changes
// nothing: there the packed form was already recognised.

#ifndef QUIC_ERRNO_WORKAROUND
#define QUIC_ERRNO_WORKAROUND 1
#endif

#if QUIC_ERRNO_WORKAROUND
static bool
is_transient_failure()
{
	int error = errno;
	if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
		return true;

	unsigned long packed = ERR_peek_last_error();
	if (!ERR_SYSTEM_ERROR(packed))
		return false;
	unsigned long reason = ERR_GET_REASON(packed);
	return reason == ((unsigned int)EAGAIN & ERR_SYSTEM_MASK)
		|| reason == ((unsigned int)EWOULDBLOCK & ERR_SYSTEM_MASK)
		|| reason == ((unsigned int)EINTR & ERR_SYSTEM_MASK);
}


static int
finish_datagram_call(int result)
{
	if (result) {
		ERR_clear_last_mark();
		return result;
	}

	if (is_transient_failure()) {
		ERR_pop_to_mark();
		ERR_raise(ERR_LIB_BIO, BIO_R_NON_FATAL);
	} else
		ERR_clear_last_mark();
	return 0;
}


// Said once at startup, so a log shows which build is running.
static void __attribute__((constructor))
announce_errno_workaround()
{
	TRACE_ERROR("OpenSSL errno workaround (#790) compiled in\n");
}
#else
static int
finish_datagram_call(int result)
{
	ERR_clear_last_mark();
	return result;
}
#endif

// ---- Haiku-Graviton #790 workaround: end -----------------------------------


// ---- Haiku-Graviton #792 workaround: begin ---------------------------------
//
// TEMPORARY. OpenSSL 3.5.7's SSL_poll() reports *failure* when an object
// becomes ready while it is entering its blocking section: poll_block()
// (ssl/rio/poll_immediate.c) leaves ok = 0 on its abort_blocking path, with
// nothing on the error queue and errno untouched. The broker's relay hits
// that race constantly, because its two threads wait on one stream (one for
// R, one for W) and each makes the other's object ready; unhandled, it ended
// a healthy session every few hundred kilobytes. Fixed upstream in 3.5.8
// (openssl/openssl#31743, commit d6ca7796bf); when the openssl3 recipe is at
// 3.5.8 or carries that commit, delete this block and make _Wait() treat
// every SSL_poll() failure as fatal.
//
// So a failure with nothing on the error queue is taken for that report and
// polled again at once, when the readout reports the event: no pause. Every
// real failure on the readout and translation paths raises an error and stays
// fatal. The one real failure that raises nothing, the OS poll() itself
// failing, would repeat on every call, so a run of kMaxSilentPollFailures
// with no successful poll between them is fatal too; the race never comes
// close (it needs another thread to make this object ready each time).

static const int32 kMaxSilentPollFailures = 1000;

static bool
poll_failure_is_readiness(int32& silentFailures)
{
	if (ERR_peek_error() != 0)
		return false;
	return ++silentFailures < kMaxSilentPollFailures;
}

// ---- Haiku-Graviton #792 workaround: end -----------------------------------


static int
peer_bio_recvmmsg(BIO* bio, BIO_MSG* messages, size_t stride, size_t count,
	uint64_t flags, size_t* processed)
{
	PeerRecordingState* state = (PeerRecordingState*)BIO_get_data(bio);
	*processed = 0;
	if (count == 0)
		return 1;

	// One datagram per call: see above.
	ERR_set_mark();
	errno = 0;
	if (!finish_datagram_call(BIO_recvmmsg(BIO_next(bio), messages, stride, 1,
			flags, processed))) {
		return 0;
	}

	state->hasLastPeer = false;
	if (*processed == 1 && messages[0].peer != NULL
		&& BIO_ADDR_family(messages[0].peer) == AF_INET) {
		struct in_addr raw;
		size_t length = sizeof(raw);
		if (BIO_ADDR_rawaddress(messages[0].peer, &raw, &length) == 1
			&& length == sizeof(raw)) {
			state->lastPeer = raw.s_addr;
			state->hasLastPeer = true;
		}
	}

	return 1;
}


static int
peer_bio_sendmmsg(BIO* bio, BIO_MSG* messages, size_t stride, size_t count,
	uint64_t flags, size_t* processed)
{
	ERR_set_mark();
	errno = 0;
	return finish_datagram_call(BIO_sendmmsg(BIO_next(bio), messages, stride,
		count, flags, processed));
}


static long
peer_bio_ctrl(BIO* bio, int command, long number, void* pointer)
{
	// Everything else -- capabilities, MTU, poll descriptors for blocking
	// waits -- is the datagram BIO's business.
	return BIO_ctrl(BIO_next(bio), command, number, pointer);
}


static int
peer_bio_create(BIO* bio)
{
	PeerRecordingState* state = new(std::nothrow) PeerRecordingState;
	if (state == NULL)
		return 0;
	state->lastPeer = INADDR_ANY;
	state->hasLastPeer = false;
	BIO_set_data(bio, state);
	BIO_set_init(bio, 1);
	return 1;
}


static int
peer_bio_destroy(BIO* bio)
{
	delete (PeerRecordingState*)BIO_get_data(bio);
	BIO_set_data(bio, NULL);
	return 1;
}


static BIO_METHOD*
peer_bio_method()
{
	static BIO_METHOD* sMethod = NULL;
	static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
	struct Init {
		static void Do()
		{
			BIO_METHOD* method = BIO_meth_new(
				BIO_get_new_index() | BIO_TYPE_FILTER,
				"remote_broker peer recorder");
			if (method == NULL)
				return;
			BIO_meth_set_recvmmsg(method, peer_bio_recvmmsg);
			BIO_meth_set_sendmmsg(method, peer_bio_sendmmsg);
			BIO_meth_set_ctrl(method, peer_bio_ctrl);
			BIO_meth_set_create(method, peer_bio_create);
			BIO_meth_set_destroy(method, peer_bio_destroy);
			sMethod = method;
		}
	};
	pthread_once(&sOnce, Init::Do);
	return sMethod;
}


// #pragma mark - QuicListener


QuicListener::QuicListener()
	:
	fContext(NULL),
	fDomain(NULL),
	fListener(NULL),
	fPeerRecorder(NULL),
	fSocket(-1)
{
}


QuicListener::~QuicListener()
{
	SSL_free(fListener);
	SSL_free(fDomain);
	SSL_CTX_free(fContext);
	if (fSocket >= 0)
		close(fSocket);
}


status_t
QuicListener::Init(const char* certificatePath, const char* privateKeyPath,
	in_addr_t address, uint16 port)
{
	fContext = SSL_CTX_new(OSSL_QUIC_server_method());
	if (fContext == NULL) {
		log_openssl_error("SSL_CTX_new(QUIC)");
		return B_NO_MEMORY;
	}

	// The same pair the TCP front door serves, so broker.fingerprint is the
	// pin for both listeners.
	if (SSL_CTX_use_certificate_chain_file(fContext, certificatePath) != 1) {
		log_openssl_error("loading certificate");
		return B_BAD_DATA;
	}

	if (SSL_CTX_use_PrivateKey_file(fContext, privateKeyPath,
			SSL_FILETYPE_PEM) != 1) {
		log_openssl_error("loading private key");
		return B_BAD_DATA;
	}

	if (SSL_CTX_check_private_key(fContext) != 1) {
		log_openssl_error("certificate/private key mismatch");
		return B_BAD_DATA;
	}

	SSL_CTX_set_verify(fContext, SSL_VERIFY_NONE, NULL);

	// No 0-RTT: early data could replay RP_AUTHENTICATE or input. 3.5 cannot
	// accept it, but it does advertise it in its session tickets when asked
	// to (rd766 check 3d), so the default of none is pinned here rather than
	// left to a default.
	if (SSL_CTX_set_max_early_data(fContext, 0) != 1
		|| SSL_CTX_set_recv_max_early_data(fContext, 0) != 1) {
		log_openssl_error("disabling early data");
		return B_ERROR;
	}
	SSL_CTX_set_alpn_select_cb(fContext, select_alpn, NULL);
	SSL_CTX_set_new_pending_conn_cb(fContext, _NewPendingConnection, this);

	// Thread-assisted, with reliable blocking: every connection is served by
	// its own thread, as on the TCP front door, and QUIC's timers (loss
	// recovery, idle timeout) keep running while those threads are busy
	// elsewhere -- in a socket write to the session port, say.
	fDomain = SSL_new_domain(fContext,
		SSL_DOMAIN_FLAG_THREAD_ASSISTED | SSL_DOMAIN_FLAG_BLOCKING);
	if (fDomain == NULL) {
		log_openssl_error("SSL_new_domain");
		return B_NO_MEMORY;
	}

	// No SSL_LISTENER_FLAG_NO_VALIDATE: address validation stays on.
	fListener = SSL_new_listener_from(fDomain, 0);
	if (fListener == NULL) {
		log_openssl_error("SSL_new_listener_from");
		return B_NO_MEMORY;
	}

#ifdef SSL_VALUE_QUIC_MAX_PENDING_CONNS
	// Not in every 3.5 release (DeBeOS's 3.5.7 lacks it); where it is missing
	// the stack's own bound of 256 applies.
	if (!SSL_set_generic_value_uint(fListener, SSL_VALUE_QUIC_MAX_PENDING_CONNS,
			kMaxPendingConnections)) {
		log_openssl_error("limiting pending connections");
		return B_ERROR;
	}
#endif

	fSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fSocket < 0) {
		TRACE_ERROR("cannot create the UDP socket: %s\n", strerror(errno));
		return errno;
	}

	struct sockaddr_in socketAddress;
	memset(&socketAddress, 0, sizeof(socketAddress));
	socketAddress.sin_family = AF_INET;
	socketAddress.sin_addr.s_addr = address;
	socketAddress.sin_port = htons(port);
	if (bind(fSocket, (struct sockaddr*)&socketAddress, sizeof(socketAddress))
			!= 0) {
		TRACE_ERROR("cannot bind UDP port %u: %s\n", port, strerror(errno));
		return errno;
	}

	BIO_METHOD* method = peer_bio_method();
	if (method == NULL || peer_address_index() < 0) {
		log_openssl_error("creating the peer-recording BIO");
		return B_NO_MEMORY;
	}

	BIO* datagram = BIO_new_dgram(fSocket, BIO_NOCLOSE);
	fPeerRecorder = BIO_new(method);
	if (datagram == NULL || fPeerRecorder == NULL) {
		BIO_free(datagram);
		BIO_free(fPeerRecorder);
		fPeerRecorder = NULL;
		log_openssl_error("creating the listener BIOs");
		return B_NO_MEMORY;
	}

	// The listener takes the chain; fPeerRecorder stays a borrowed pointer.
	BIO_push(fPeerRecorder, datagram);
	SSL_set_bio(fListener, fPeerRecorder, fPeerRecorder);

	if (SSL_listen(fListener) != 1) {
		log_openssl_error("starting the QUIC listener");
		return B_ERROR;
	}

	return B_OK;
}


/*static*/ int
QuicListener::_NewPendingConnection(SSL_CTX* context, SSL* connection,
	void* cookie)
{
	QuicListener* listener = (QuicListener*)cookie;
	PeerRecordingState* state
		= (PeerRecordingState*)BIO_get_data(listener->fPeerRecorder);
	if (!state->hasLastPeer) {
		// Without a source address the per-address limits cannot apply to
		// this peer, and they are what stands between a flood and the token
		// check. Refuse it rather than let it in unaccounted.
		TRACE_ERROR("refusing a connection whose source address is"
			" unknown\n");
		return 0;
	}

	in_addr_t* peer = new(std::nothrow) in_addr_t(state->lastPeer);
	if (peer == NULL
		|| !SSL_set_ex_data(connection, peer_address_index(), peer)) {
		delete peer;
		return 0;
	}

	return 1;
}


ssl_st*
QuicListener::Accept()
{
	while (true) {
		SSL* connection = SSL_accept_connection(fListener, 0);
		if (connection != NULL)
			return connection;

		// A failed accept is about one peer, not about the listener -- unless
		// the listener has failed, in which case every further call fails the
		// same way at once. Back off so that case cannot spin, and let the
		// caller see it if it persists.
		log_openssl_error("SSL_accept_connection");
		snooze(100 * 1000);
	}
}


// #pragma mark - QuicStream


QuicStream::QuicStream(ssl_st* connection)
	:
	fConnection(connection),
	fStream(NULL),
	fPeerAddress(INADDR_ANY),
	fInterrupted(0),
	fStopped(0)
{
	in_addr_t* peer = (in_addr_t*)SSL_get_ex_data(fConnection,
		peer_address_index());
	if (peer != NULL)
		fPeerAddress = *peer;
}


QuicStream::~QuicStream()
{
	SSL_free(fStream);
	SSL_free(fConnection);
}


status_t
QuicStream::AcceptStream(bigtime_t deadline)
{
	// Non-blocking at the OpenSSL level; every wait below goes through
	// _Wait(), which owns deadlines and interruption. Partial writes so that
	// WriteAll() knows how much a write took.
	if (!SSL_set_blocking_mode(fConnection, 0)
		|| !SSL_set_default_stream_mode(fConnection,
			SSL_DEFAULT_STREAM_MODE_NONE)
		|| !SSL_set_incoming_stream_policy(fConnection,
			SSL_INCOMING_STREAM_POLICY_ACCEPT, 0)) {
		log_openssl_error("configuring the connection");
		return B_ERROR;
	}
	SSL_set_mode(fConnection, SSL_MODE_ENABLE_PARTIAL_WRITE);

	while (true) {
		fStream = SSL_accept_stream(fConnection, SSL_ACCEPT_STREAM_NO_BLOCK);
		if (fStream != NULL)
			break;

		// Waiting on the connection also drives its handshake; a handshake
		// that fails (no ALPN in common, say) ends the connection, which is
		// an exception event here.
		uint64 revents = 0;
		status_t result = _Wait(fConnection,
			SSL_POLL_EVENT_IS | SSL_POLL_EVENT_EC, deadline, &revents);
		if (result != B_OK)
			return result;
		if ((revents & SSL_POLL_EVENT_EC) != 0)
			return B_IO_ERROR;
	}

	// Defence in depth behind the ALPN callback: the handshake cannot have
	// completed with anything else.
	const unsigned char* alpn = NULL;
	unsigned int alpnLength = 0;
	SSL_get0_alpn_selected(fConnection, &alpn, &alpnLength);
	if (alpnLength != QUIC_ALPN_URP1_LENGTH - 1
		|| memcmp(alpn, QUIC_ALPN_URP1 + 1, alpnLength) != 0) {
		return B_NOT_ALLOWED;
	}

	// The one stream: client-initiated bidirectional stream 0 -- the ID alone
	// says both, RFC 9000 2.1 -- and nothing opened alongside it.
	if (SSL_get_stream_id(fStream) != 0 || HasForbiddenStream()) {
		return B_NOT_ALLOWED;
	}

	return B_OK;
}


ssize_t
QuicStream::ReadSome(void* buffer, size_t size, bigtime_t deadline)
{
	if (size == 0)
		return 0;

	while (true) {
		// A success that read nothing is not an end of stream -- that is the
		// state check below -- and on DeBeOS's 3.5.7 it does happen, in bursts:
		// returned as-is it would read as the peer's FIN.
		size_t read = 0;
		if (SSL_read_ex(fStream, buffer, size, &read) == 1 && read > 0)
			return (ssize_t)read;

		// The stream's state, not SSL_get_error(): the relay reads and writes
		// this stream from two threads, and the last-error slot is per
		// object, so it may describe the other thread's call.
		switch (SSL_get_stream_read_state(fStream)) {
			case SSL_STREAM_STATE_OK:
				break;

			case SSL_STREAM_STATE_FINISHED:
			case SSL_STREAM_STATE_CONN_CLOSED:
				ERR_clear_error();
				return 0;

			default:
				ERR_clear_error();
				return B_IO_ERROR;
		}

		ERR_clear_error();
		status_t result = _Wait(fStream, SSL_POLL_EVENT_R | SSL_POLL_EVENT_ER
			| SSL_POLL_EVENT_EC, deadline, NULL);
		if (result != B_OK)
			return result;
	}
}


status_t
QuicStream::WriteAll(const void* buffer, size_t size, bigtime_t deadline)
{
	const uint8* position = (const uint8*)buffer;
	while (size > 0) {
		size_t written = 0;
		if (SSL_write_ex(fStream, position, size, &written) == 1) {
			position += written;
			size -= written;
			continue;
		}

		// As in ReadSome(): state, not the per-object last error.
		if (SSL_get_stream_write_state(fStream) != SSL_STREAM_STATE_OK) {
			ERR_clear_error();
			return B_IO_ERROR;
		}

		ERR_clear_error();
		status_t result = _Wait(fStream, SSL_POLL_EVENT_W | SSL_POLL_EVENT_EW
			| SSL_POLL_EVENT_EC, deadline, NULL);
		if (result != B_OK)
			return result;
	}

	return B_OK;
}


bool
QuicStream::HasForbiddenStream()
{
	// Every stream the peer opens lands in the accept queue (the policy is
	// "accept", precisely so that none is silently swallowed), and the front
	// door never accepts a second one, so anything queued is a violation.
	return SSL_get_accept_stream_queue_len(fConnection) > 0;
}


void
QuicStream::Interrupt()
{
	__atomic_store_n(&fInterrupted, 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&fStopped, 1, __ATOMIC_SEQ_CST);
}


void
QuicStream::Close(uint64 errorCode, bigtime_t deadline)
{
	uint64 flags = SSL_SHUTDOWN_FLAG_NO_BLOCK;
	if (errorCode == kQuicErrorNone) {
		// The orderly end: a FIN on the stream, flushed ahead of the close.
		if (fStream != NULL)
			SSL_stream_conclude(fStream, 0);
	} else
		flags |= SSL_SHUTDOWN_FLAG_RAPID;

	SSL_SHUTDOWN_EX_ARGS args;
	memset(&args, 0, sizeof(args));
	args.quic_error_code = errorCode;
	args.quic_reason = errorCode == kQuicErrorNone ? "" : "remote_broker";

	while (SSL_shutdown_ex(fConnection, flags, &args, sizeof(args)) != 1) {
		if (system_time() >= deadline)
			break;
		SSL_handle_events(fConnection);
		snooze(10 * 1000);
	}
	ERR_clear_error();
}


namespace {

struct OutboundRelay {
	QuicStream*	stream;
	int			plainSocket;
	// Set by the outbound thread when it ends, with why.
	int32		ended;
	bool		orderly;
};

} // namespace


static void*
relay_outbound(void* data)
{
	OutboundRelay* relay = (OutboundRelay*)data;
	uint8 buffer[16 * 1024];

	relay->orderly = false;
	while (true) {
		ssize_t read = recv(relay->plainSocket, buffer, sizeof(buffer), 0);
		if (read < 0 && errno == EINTR)
			continue;
		if (read <= 0) {
			// app_server ending the session cleanly is an orderly end of
			// the stream too.
			relay->orderly = read == 0;
			break;
		}

		status_t result = relay->stream->WriteAll(buffer, read,
			system_time() + kRelayWriteTimeout);
		if (result != B_OK) {
			if (result == B_TIMED_OUT) {
				TRACE_ERROR("client accepted no data for %" B_PRId64
					" us, dropping session\n", kRelayWriteTimeout);
			}
			break;
		}
	}

	__atomic_store_n(&relay->ended, 1, __ATOMIC_SEQ_CST);
	relay->stream->Stop();
	return NULL;
}


static status_t
write_socket_fully(int fd, const uint8* buffer, size_t size)
{
	while (size > 0) {
		ssize_t written = write(fd, buffer, size);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}
		buffer += written;
		size -= written;
	}

	return B_OK;
}


/*static*/ uint64
QuicStream::Relay(QuicStream& stream, int plainSocket,
	RelayFilter* inboundFilter)
{
	// Two threads, one per direction. The WebSocket relay multiplexes both on
	// one select(), but SSL_poll() can wait on QUIC objects only, never on the
	// session port's socket -- so the server -> client direction blocks in
	// recv() on a thread of its own and this one serves the client.
	OutboundRelay outbound;
	outbound.stream = &stream;
	outbound.plainSocket = plainSocket;
	outbound.ended = 0;
	outbound.orderly = false;

	pthread_t thread;
	if (pthread_create(&thread, NULL, relay_outbound, &outbound) != 0)
		return kQuicErrorRelay;

	uint64 errorCode = kQuicErrorRelay;
	uint8 buffer[16 * 1024];

	while (true) {
		if (stream.HasForbiddenStream()) {
			TRACE_ERROR("peer opened a second stream, closing the"
				" connection\n");
			errorCode = kQuicErrorStreamRules;
			break;
		}

		// A short deadline so that the stream-rule check above runs even on
		// a client that sends nothing; data still wakes the read at once.
		ssize_t read = stream.ReadSome(buffer, sizeof(buffer),
			system_time() + 4 * kWaitSlice);
		if (read == B_TIMED_OUT)
			continue;

		if (read == B_INTERRUPTED) {
			// Either a takeover, or the other direction ended.
			if (!stream.IsInterrupted()
				&& __atomic_load_n(&outbound.ended, __ATOMIC_SEQ_CST) != 0
				&& outbound.orderly) {
				errorCode = kQuicErrorNone;
			}
			break;
		}

		if (read <= 0) {
			// The client's FIN is the orderly end; anything else is not.
			if (read == 0)
				errorCode = kQuicErrorNone;
			break;
		}

		status_t result = inboundFilter != NULL
			? inboundFilter->Forward(plainSocket, buffer, read)
			: write_socket_fully(plainSocket, buffer, read);
		if (result != B_OK)
			break;
	}

	// Wake and collect the other direction before the caller closes the
	// socket it reads from.
	stream.Stop();
	shutdown(plainSocket, SHUT_RDWR);
	pthread_join(thread, NULL);

	return errorCode;
}


void
QuicStream::Stop()
{
	__atomic_store_n(&fStopped, 1, __ATOMIC_SEQ_CST);
}


bool
QuicStream::IsInterrupted() const
{
	return __atomic_load_n(&fInterrupted, __ATOMIC_SEQ_CST) != 0;
}


status_t
QuicStream::_Wait(ssl_st* object, uint64 events, bigtime_t deadline,
	uint64* revents)
{
	int32 silentFailures = 0;
	while (true) {
		if (__atomic_load_n(&fStopped, __ATOMIC_SEQ_CST) != 0)
			return B_INTERRUPTED;

		bigtime_t now = system_time();
		if (now >= deadline)
			return B_TIMED_OUT;

		bigtime_t slice = deadline - now;
		if (slice > kWaitSlice)
			slice = kWaitSlice;

		struct timeval timeout;
		timeout.tv_sec = slice / 1000000;
		timeout.tv_usec = slice % 1000000;

		SSL_POLL_ITEM item;
		memset(&item, 0, sizeof(item));
		item.desc = SSL_as_poll_descriptor(object);
		item.events = events;

		size_t count = 0;
		errno = 0;
		if (SSL_poll(&item, 1, sizeof(item), &timeout, 0, &count) != 1) {
			if (poll_failure_is_readiness(silentFailures))
				continue;
			TRACE_ERROR("SSL_poll failed (errno %d)\n", errno);
			log_openssl_error("SSL_poll");
			return B_IO_ERROR;
		}
		silentFailures = 0;

		if (count > 0 && (item.revents & events) != 0) {
			if (revents != NULL)
				*revents = item.revents;
			return B_OK;
		}
	}
}
