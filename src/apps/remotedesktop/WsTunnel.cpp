/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */

#include "WsTunnel.h"

#include <openssl/crypto.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>


// Wire constants shared with the remote drawing protocol; the values are
// reserved in src/servers/app/drawing/interface/remote/RemoteMessage.h.
static const uint16 kRPAuthenticate = 10;
static const uint16 kRPAuthResult = 11;
static const uint32 kAuthMethodSharedToken = 1;

static const bigtime_t kHandshakeTimeout = 15 * 1000 * 1000;


WsTunnel::WsTunnel()
	:
	fWebSocket(fStream),
	fListenSocket(-1),
	fLocalPort(0),
	fPumpThread(-1)
{
}


WsTunnel::~WsTunnel()
{
	if (fListenSocket >= 0)
		close(fListenSocket);

	// Shutting the TLS socket down wakes the pump out of select()/reads.
	if (fStream.Socket() >= 0)
		shutdown(fStream.Socket(), SHUT_RDWR);

	if (fPumpThread >= 0) {
		status_t result;
		wait_for_thread(fPumpThread, &result);
	}
}


status_t
WsTunnel::Connect(const char* host, uint16 port, const char* token,
	const char* pinnedFingerprint, bool insecure)
{
	bigtime_t deadline = system_time() + kHandshakeTimeout;

	status_t result = fContext.InitClient();
	if (result != B_OK)
		return result;

	// Resolve and connect (IPv4, like the rest of the client).
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	char portString[16];
	snprintf(portString, sizeof(portString), "%u", port);

	struct addrinfo* addresses = NULL;
	if (getaddrinfo(host, portString, &hints, &addresses) != 0
		|| addresses == NULL) {
		printf("failed to resolve %s\n", host);
		return B_NAME_NOT_FOUND;
	}

	int tlsSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (tlsSocket < 0) {
		freeaddrinfo(addresses);
		return errno;
	}

	if (connect(tlsSocket, addresses->ai_addr, addresses->ai_addrlen) != 0) {
		printf("failed to connect to %s:%u: %s\n", host, port,
			strerror(errno));
		freeaddrinfo(addresses);
		close(tlsSocket);
		return B_ERROR;
	}

	freeaddrinfo(addresses);

	int noDelay = 1;
	setsockopt(tlsSocket, IPPROTO_TCP, TCP_NODELAY, &noDelay,
		sizeof(noDelay));

	result = fStream.Connect(fContext, tlsSocket, host, deadline);
	if (result != B_OK) {
		printf("TLS handshake with %s:%u failed\n", host, port);
		return result;
	}

	// Trust decision: certificate pinning.
	char fingerprint[TLS_FINGERPRINT_HEX_LENGTH + 1];
	result = fStream.PeerCertificateFingerprint(fingerprint);
	if (result != B_OK) {
		printf("failed to get the server certificate\n");
		return result;
	}

	if (pinnedFingerprint != NULL) {
		if (strncasecmp(pinnedFingerprint, "sha256:", 7) == 0)
			pinnedFingerprint += 7;
		if (strlen(pinnedFingerprint) != TLS_FINGERPRINT_HEX_LENGTH
			|| strcasecmp(pinnedFingerprint, fingerprint) != 0) {
			printf("server certificate sha256=%s does not match the pin\n",
				fingerprint);
			return B_NOT_ALLOWED;
		}
	} else if (!insecure) {
		printf("no certificate pin given; the server presented\n"
			"    sha256=%s\n"
			"Verify it out-of-band (broker.fingerprint in the server's "
			"remote_desktop\nsettings directory) and re-run with "
			"--pin <fingerprint>, or use --insecure.\n", fingerprint);
		return B_NOT_ALLOWED;
	}

	result = fWebSocket.ClientHandshake(host, deadline);
	if (result != B_OK) {
		printf("WebSocket handshake failed\n");
		return result;
	}

	// Authenticate before anything else crosses the tunnel.
	size_t tokenLength = strlen(token);
	size_t messageLength = 6 + 8 + tokenLength;
	uint8 message[6 + 8 + 1024];
	if (tokenLength == 0 || messageLength > sizeof(message)) {
		printf("invalid token length\n");
		return B_BAD_VALUE;
	}

	message[0] = (uint8)(kRPAuthenticate & 0xff);
	message[1] = (uint8)(kRPAuthenticate >> 8);
	for (int i = 0; i < 4; i++) {
		message[2 + i] = (uint8)((uint32)messageLength >> (8 * i));
		message[6 + i] = (uint8)(kAuthMethodSharedToken >> (8 * i));
		message[10 + i] = (uint8)((uint32)tokenLength >> (8 * i));
	}
	memcpy(message + 14, token, tokenLength);

	result = fWebSocket.WriteAll(message, messageLength, deadline);
	OPENSSL_cleanse(message, sizeof(message));
	if (result != B_OK)
		return result;

	// The reply is one fixed size RP_AUTH_RESULT message.
	uint8 reply[10];
	size_t replyUsed = 0;
	while (replyUsed < sizeof(reply)) {
		ssize_t read = fWebSocket.ReadSome(reply + replyUsed,
			sizeof(reply) - replyUsed, deadline);
		if (read < 0)
			return (status_t)read;
		if (read == 0) {
			printf("broker closed the connection during authentication\n");
			return B_IO_ERROR;
		}
		replyUsed += read;
	}

	uint16 replyCode = (uint16)reply[0] | ((uint16)reply[1] << 8);
	uint32 replyLength = (uint32)reply[2] | ((uint32)reply[3] << 8)
		| ((uint32)reply[4] << 16) | ((uint32)reply[5] << 24);
	uint32 status = (uint32)reply[6] | ((uint32)reply[7] << 8)
		| ((uint32)reply[8] << 16) | ((uint32)reply[9] << 24);

	if (replyCode != kRPAuthResult || replyLength != sizeof(reply)) {
		printf("unexpected reply to authentication\n");
		return B_BAD_DATA;
	}

	if (status != 0) {
		printf("the broker rejected the authentication token (status %"
			B_PRIu32 ")\n", status);
		return B_PERMISSION_DENIED;
	}

	// Expose the byte stream on a local loopback port for RemoteView.
	fListenSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (fListenSocket < 0)
		return errno;

	struct sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	local.sin_port = 0;

	socklen_t length = sizeof(local);
	if (bind(fListenSocket, (struct sockaddr*)&local, sizeof(local)) != 0
		|| listen(fListenSocket, 1) != 0
		|| getsockname(fListenSocket, (struct sockaddr*)&local,
			&length) != 0) {
		return errno;
	}

	fLocalPort = ntohs(local.sin_port);

	fPumpThread = spawn_thread(_PumpEntry, "wss tunnel pump",
		B_NORMAL_PRIORITY, this);
	if (fPumpThread < 0)
		return fPumpThread;

	resume_thread(fPumpThread);
	return B_OK;
}


int32
WsTunnel::_PumpEntry(void* data)
{
	return ((WsTunnel*)data)->_Pump();
}


status_t
WsTunnel::_Pump()
{
	struct sockaddr_in peer;
	socklen_t peerLength = sizeof(peer);
	int localSocket = accept(fListenSocket, (struct sockaddr*)&peer,
		&peerLength);
	if (localSocket < 0)
		return errno;

	int noDelay = 1;
	setsockopt(localSocket, IPPROTO_TCP, TCP_NODELAY, &noDelay,
		sizeof(noDelay));

	WebSocketStream::Relay(fWebSocket, fStream, localSocket);

	close(localSocket);
	printf("connection to the broker ended\n");
	return B_OK;
}
