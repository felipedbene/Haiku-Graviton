/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef TLS_STREAM_H
#define TLS_STREAM_H

#include <OS.h>
#include <SupportDefs.h>

struct ssl_ctx_st;
struct ssl_st;


// Length of a lowercase-hex SHA-256 fingerprint, without terminator.
#define TLS_FINGERPRINT_HEX_LENGTH	64


/*!	A process-lifetime OpenSSL context. Configure exactly once, share between
	streams; the underlying SSL_CTX is thread-safe for concurrent SSL_new().
*/
class TLSContext {
public:
								TLSContext();
								~TLSContext();

			// Server side: load the certificate (chain) and private key.
			status_t			InitServer(const char* certificatePath,
									const char* privateKeyPath);

			// Client side. Peer verification is deliberately not configured
			// here: the trust model is key pinning, enforced by the caller
			// via TLSStream::PeerCertificateFingerprint() after the
			// handshake.
			status_t			InitClient();

			ssl_ctx_st*			Context() const { return fContext; }

private:
			ssl_ctx_st*			fContext;
};


/*!	One TLS connection over an already connected TCP socket, usable from one
	thread at a time. All operations take an absolute system_time() deadline;
	the socket is switched to non-blocking mode and waits are bounded.
*/
class TLSStream {
public:
								TLSStream();
								~TLSStream();

			// Both take ownership of \a socket (also on failure).
			status_t			Accept(TLSContext& context, int socket,
									bigtime_t deadline);
			status_t			Connect(TLSContext& context, int socket,
									const char* serverName,
									bigtime_t deadline);

			// Returns the (positive) number of bytes read, 0 on clean TLS
			// shutdown or connection end, or a negative error code
			// (B_TIMED_OUT when the deadline passed first).
			ssize_t				Read(void* buffer, size_t size,
									bigtime_t deadline);
			status_t			ReadFully(void* buffer, size_t size,
									bigtime_t deadline);
			status_t			WriteFully(const void* buffer, size_t size,
									bigtime_t deadline);

			// Whether decrypted data is already buffered inside the TLS
			// layer, i.e. Read() can make progress without the socket
			// becoming readable. Must be checked before blocking in select()
			// on Socket().
			bool				HasBufferedData() const;

			int					Socket() const { return fSocket; }

			// Lowercase-hex SHA-256 of the peer's certificate (DER form),
			// for key pinning. \a buffer must hold
			// TLS_FINGERPRINT_HEX_LENGTH + 1 bytes.
			status_t			PeerCertificateFingerprint(char* buffer);

			void				Close();

private:
			status_t			_Handshake(bigtime_t deadline);
			status_t			_WaitForSocket(bool write, bigtime_t deadline);

			ssl_st*				fSSL;
			int					fSocket;
};


#endif // TLS_STREAM_H
