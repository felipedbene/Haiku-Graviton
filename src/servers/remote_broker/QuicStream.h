/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef QUIC_STREAM_H
#define QUIC_STREAM_H

#include <OS.h>
#include <SupportDefs.h>

#include <netinet/in.h>

struct ssl_ctx_st;
struct ssl_st;
struct bio_st;

class RelayFilter;


// The one application protocol the QUIC front door speaks, in ALPN wire form
// (length-prefixed). The "/1" names the QUIC stream layout -- one
// client-initiated bidirectional stream carrying exactly what a wss connection
// carries -- not the remote protocol, whose features RP_HELLO negotiates. A
// multi-stream layout will be a different ALPN, chosen per connection.
#define QUIC_ALPN_URP1			"\x0c" "debeos-urp/1"
#define QUIC_ALPN_URP1_LENGTH	13

// Application error codes carried in CONNECTION_CLOSE. 0 is the orderly end
// (after a FIN on the stream); everything else is the broker giving up.
enum {
	kQuicErrorNone				= 0,
	// The relay ended abnormally: the session port went away, a write stalled
	// past its bound, or the session was taken over.
	kQuicErrorRelay				= 1,
	// The peer broke the stream rules: a second stream, a unidirectional one,
	// or a first stream that is not client-initiated bidirectional stream 0.
	kQuicErrorStreamRules		= 2
};

/*!	The UDP listener of the QUIC front door: one explicit OpenSSL QUIC domain
	in the thread-assisted model with reliable blocking support, so that every
	connection can be served from its own thread, as the TCP front door does,
	while QUIC timers keep being handled in the background.

	Client address validation (a Retry round trip before any state is
	committed to a peer) is OpenSSL's default and is deliberately left on: a
	UDP source address means nothing until it has been validated, and the
	per-address accounting in front of the token check depends on it.
*/
class QuicListener {
public:
								QuicListener();
								~QuicListener();

			status_t			Init(const char* certificatePath,
									const char* privateKeyPath,
									in_addr_t address, uint16 port);

			// Blocks until the next connection is available. Returns NULL
			// only when the listener itself has failed; the caller owns the
			// returned connection object.
			ssl_st*				Accept();

private:
	static	int					_NewPendingConnection(ssl_ctx_st* context,
									ssl_st* connection, void* cookie);

			ssl_ctx_st*			fContext;
			ssl_st*				fDomain;
			ssl_st*				fListener;
			bio_st*				fPeerRecorder;
			int					fSocket;
};


/*!	One accepted QUIC connection and the single stream the front door allows
	on it, presented as a byte stream with the same deadline semantics as
	WebSocketStream, so that the broker's authentication and relay code can
	serve either.

	Use from one thread at a time, except Interrupt(), which may be called from
	any thread to end blocking waits (session takeover).
*/
class QuicStream {
public:
								QuicStream(ssl_st* connection);
								~QuicStream();

			// The validated peer address the connection was created for (see
			// QuicListener). INADDR_ANY if it could not be determined.
			in_addr_t			PeerAddress() const { return fPeerAddress; }

			// Completes the handshake and accepts the one stream: it must be
			// client-initiated bidirectional stream 0 and nothing else may be
			// opened before it.
			status_t			AcceptStream(bigtime_t deadline);

			// Returns the (positive) number of bytes read, 0 on the peer's
			// FIN or the connection ending, or a negative error code
			// (B_TIMED_OUT when the deadline passed first, B_INTERRUPTED
			// after Interrupt()).
			ssize_t				ReadSome(void* buffer, size_t size,
									bigtime_t deadline);
			status_t			WriteAll(const void* buffer, size_t size,
									bigtime_t deadline);

			// Whether the peer opened a stream beyond the one allowed. Checked
			// by the relay; a stream-rule violation ends the connection.
			bool				HasForbiddenStream();

			// Ends blocking waits from any thread: Interrupt() for a session
			// takeover, Stop() for the relay ending itself.
			void				Interrupt();
			void				Stop();
			bool				IsInterrupted() const;

			// Ends the connection: an orderly FIN on the stream first when
			// \a errorCode is kQuicErrorNone, then CONNECTION_CLOSE carrying
			// \a errorCode. Waits for the close to be sent until \a deadline.
			void				Close(uint64 errorCode, bigtime_t deadline);

			// Bidirectional relay between this stream and a plain socket,
			// until either side ends or the session is interrupted. Blocks.
			// \a inboundFilter, when given, decides what of the client
			// direction reaches \a plainSocket. Returns the application error
			// code the connection should be closed with.
	static	uint64				Relay(QuicStream& stream, int plainSocket,
									RelayFilter* inboundFilter = NULL);

private:
			status_t			_Wait(ssl_st* object, uint64 events,
									bigtime_t deadline, uint64* revents);

			ssl_st*				fConnection;
			ssl_st*				fStream;
			in_addr_t			fPeerAddress;
			int32				fInterrupted;
			int32				fStopped;
};


#endif // QUIC_STREAM_H
