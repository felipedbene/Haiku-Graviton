/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */

#include "TLSStream.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>


#define TRACE_ERROR(x...)	fprintf(stderr, "TLSStream: " x)


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


// #pragma mark - TLSContext


TLSContext::TLSContext()
	:
	fContext(NULL)
{
}


TLSContext::~TLSContext()
{
	if (fContext != NULL)
		SSL_CTX_free(fContext);
}


status_t
TLSContext::InitServer(const char* certificatePath, const char* privateKeyPath)
{
	fContext = SSL_CTX_new(TLS_server_method());
	if (fContext == NULL) {
		log_openssl_error("SSL_CTX_new");
		return B_NO_MEMORY;
	}

	SSL_CTX_set_min_proto_version(fContext, TLS1_2_VERSION);

	// A chain file so that a real CA-issued certificate with intermediates
	// can simply be dropped in place of the generated self-signed one.
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

	return B_OK;
}


status_t
TLSContext::InitClient()
{
	fContext = SSL_CTX_new(TLS_client_method());
	if (fContext == NULL) {
		log_openssl_error("SSL_CTX_new");
		return B_NO_MEMORY;
	}

	SSL_CTX_set_min_proto_version(fContext, TLS1_2_VERSION);

	// SSL_VERIFY_NONE is not "no verification": the trust decision is key
	// pinning against the peer certificate fingerprint, made by the caller
	// after the handshake, where a self-signed certificate is as strong as a
	// CA-issued one.
	SSL_CTX_set_verify(fContext, SSL_VERIFY_NONE, NULL);

	return B_OK;
}


// #pragma mark - TLSStream


TLSStream::TLSStream()
	:
	fSSL(NULL),
	fSocket(-1)
{
}


TLSStream::~TLSStream()
{
	Close();
}


status_t
TLSStream::Accept(TLSContext& context, int socket, bigtime_t deadline)
{
	fSocket = socket;
	fcntl(fSocket, F_SETFL, fcntl(fSocket, F_GETFL, 0) | O_NONBLOCK);

	fSSL = SSL_new(context.Context());
	if (fSSL == NULL) {
		log_openssl_error("SSL_new");
		return B_NO_MEMORY;
	}

	if (SSL_set_fd(fSSL, fSocket) != 1) {
		log_openssl_error("SSL_set_fd");
		return B_ERROR;
	}

	SSL_set_accept_state(fSSL);
	return _Handshake(deadline);
}


status_t
TLSStream::Connect(TLSContext& context, int socket, const char* serverName,
	bigtime_t deadline)
{
	fSocket = socket;
	fcntl(fSocket, F_SETFL, fcntl(fSocket, F_GETFL, 0) | O_NONBLOCK);

	fSSL = SSL_new(context.Context());
	if (fSSL == NULL) {
		log_openssl_error("SSL_new");
		return B_NO_MEMORY;
	}

	if (SSL_set_fd(fSSL, fSocket) != 1) {
		log_openssl_error("SSL_set_fd");
		return B_ERROR;
	}

	if (serverName != NULL && serverName[0] != '\0')
		SSL_set_tlsext_host_name(fSSL, serverName);

	SSL_set_connect_state(fSSL);
	return _Handshake(deadline);
}


ssize_t
TLSStream::Read(void* buffer, size_t size, bigtime_t deadline)
{
	if (fSSL == NULL)
		return B_NO_INIT;

	while (true) {
		ERR_clear_error();
		int result = SSL_read(fSSL, buffer, (int)size);
		if (result > 0)
			return result;

		int error = SSL_get_error(fSSL, result);
		switch (error) {
			case SSL_ERROR_ZERO_RETURN:
				return 0;

			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
			{
				status_t waitResult = _WaitForSocket(
					error == SSL_ERROR_WANT_WRITE, deadline);
				if (waitResult != B_OK)
					return waitResult;
				break;
			}

			case SSL_ERROR_SYSCALL:
				// A peer that just disappears (reset, or an abrupt close
				// without close_notify) ends the stream; report it like a
				// close so callers tear the session down.
				return 0;

			default:
				log_openssl_error("SSL_read");
				return B_IO_ERROR;
		}
	}
}


status_t
TLSStream::ReadFully(void* buffer, size_t size, bigtime_t deadline)
{
	uint8* position = (uint8*)buffer;
	while (size > 0) {
		ssize_t read = Read(position, size, deadline);
		if (read < 0)
			return (status_t)read;
		if (read == 0)
			return B_IO_ERROR;

		position += read;
		size -= read;
	}

	return B_OK;
}


status_t
TLSStream::WriteFully(const void* buffer, size_t size, bigtime_t deadline)
{
	if (fSSL == NULL)
		return B_NO_INIT;

	const uint8* position = (const uint8*)buffer;
	while (size > 0) {
		ERR_clear_error();
		int result = SSL_write(fSSL, position, (int)size);
		if (result > 0) {
			position += result;
			size -= result;
			continue;
		}

		int error = SSL_get_error(fSSL, result);
		if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
			status_t waitResult = _WaitForSocket(
				error == SSL_ERROR_WANT_WRITE, deadline);
			if (waitResult != B_OK)
				return waitResult;
			continue;
		}

		if (error != SSL_ERROR_SYSCALL)
			log_openssl_error("SSL_write");
		return B_IO_ERROR;
	}

	return B_OK;
}


bool
TLSStream::HasBufferedData() const
{
	return fSSL != NULL && SSL_pending(fSSL) > 0;
}


status_t
TLSStream::PeerCertificateFingerprint(char* buffer)
{
	if (fSSL == NULL)
		return B_NO_INIT;

	X509* certificate = SSL_get1_peer_certificate(fSSL);
	if (certificate == NULL)
		return B_ENTRY_NOT_FOUND;

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digestLength = 0;
	int result = X509_digest(certificate, EVP_sha256(), digest,
		&digestLength);
	X509_free(certificate);

	if (result != 1 || digestLength * 2 != TLS_FINGERPRINT_HEX_LENGTH)
		return B_ERROR;

	for (unsigned int i = 0; i < digestLength; i++)
		sprintf(buffer + 2 * i, "%02x", digest[i]);

	return B_OK;
}


void
TLSStream::Close()
{
	if (fSSL != NULL) {
		// Best effort close_notify; the socket is non-blocking, so this
		// either goes out immediately or not at all.
		SSL_shutdown(fSSL);
		SSL_free(fSSL);
		fSSL = NULL;
	}

	if (fSocket >= 0) {
		close(fSocket);
		fSocket = -1;
	}
}


status_t
TLSStream::_Handshake(bigtime_t deadline)
{
	while (true) {
		ERR_clear_error();
		int result = SSL_do_handshake(fSSL);
		if (result == 1)
			return B_OK;

		int error = SSL_get_error(fSSL, result);
		if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
			status_t waitResult = _WaitForSocket(
				error == SSL_ERROR_WANT_WRITE, deadline);
			if (waitResult != B_OK)
				return waitResult;
			continue;
		}

		if (error != SSL_ERROR_SYSCALL)
			log_openssl_error("TLS handshake");
		return B_IO_ERROR;
	}
}


status_t
TLSStream::_WaitForSocket(bool write, bigtime_t deadline)
{
	while (true) {
		bigtime_t remaining = deadline - system_time();
		if (remaining <= 0)
			return B_TIMED_OUT;

		struct timeval timeout;
		timeout.tv_sec = remaining / 1000000;
		timeout.tv_usec = remaining % 1000000;

		fd_set set;
		FD_ZERO(&set);
		FD_SET(fSocket, &set);

		int result = select(fSocket + 1, write ? NULL : &set,
			write ? &set : NULL, NULL, &timeout);
		if (result > 0)
			return B_OK;
		if (result == 0)
			return B_TIMED_OUT;
		if (errno != EINTR)
			return B_ERROR;
	}
}
