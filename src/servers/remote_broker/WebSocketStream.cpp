/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */

#include "WebSocketStream.h"
#include "TLSStream.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>


#define TRACE_ERROR(x...)	fprintf(stderr, "WebSocketStream: " x)


static const char* kWebSocketGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// A remote desktop frame is at most one RP message plus change; anything
// larger in a single WebSocket frame is bogus.
static const uint64 kMaxDataFrameSize = 64 * 1024 * 1024;

static const uint8 kOpcodeContinuation = 0x0;
static const uint8 kOpcodeText = 0x1;
static const uint8 kOpcodeBinary = 0x2;
static const uint8 kOpcodeClose = 0x8;
static const uint8 kOpcodePing = 0x9;
static const uint8 kOpcodePong = 0xa;


/*!	base64(SHA1(<key> + <GUID>)) as required for Sec-WebSocket-Accept.
	\a result must hold at least 29 bytes.
*/
static void
compute_accept_key(const char* clientKey, char* result)
{
	char input[128];
	snprintf(input, sizeof(input), "%s%s", clientKey, kWebSocketGUID);

	unsigned char digest[SHA_DIGEST_LENGTH];
	SHA1((const unsigned char*)input, strlen(input), digest);

	EVP_EncodeBlock((unsigned char*)result, digest, SHA_DIGEST_LENGTH);
}


/*!	Finds an HTTP header in \a headers (case-insensitive name match) and
	copies its trimmed value into \a value. Returns false when absent.
*/
static bool
find_header(const char* headers, const char* name, char* value,
	size_t valueSize)
{
	size_t nameLength = strlen(name);
	const char* line = headers;
	while (line != NULL && *line != '\0') {
		const char* lineEnd = strstr(line, "\r\n");
		if (lineEnd == NULL)
			lineEnd = line + strlen(line);

		if (strncasecmp(line, name, nameLength) == 0
			&& line[nameLength] == ':') {
			const char* start = line + nameLength + 1;
			while (start < lineEnd && isspace(*start))
				start++;
			const char* end = lineEnd;
			while (end > start && isspace(end[-1]))
				end--;

			size_t length = end - start;
			if (length >= valueSize)
				length = valueSize - 1;
			memcpy(value, start, length);
			value[length] = '\0';
			return true;
		}

		line = *lineEnd == '\0' ? NULL : lineEnd + 2;
	}

	return false;
}


/*!	Whether the comma separated header \a value contains \a token,
	case-insensitively.
*/
static bool
header_contains_token(const char* value, const char* token)
{
	size_t tokenLength = strlen(token);
	const char* position = value;
	while (*position != '\0') {
		while (*position == ' ' || *position == ',' || *position == '\t')
			position++;
		const char* end = position;
		while (*end != '\0' && *end != ',')
			end++;
		const char* trimmedEnd = end;
		while (trimmedEnd > position && isspace(trimmedEnd[-1]))
			trimmedEnd--;

		if ((size_t)(trimmedEnd - position) == tokenLength
			&& strncasecmp(position, token, tokenLength) == 0) {
			return true;
		}

		position = *end == '\0' ? end : end + 1;
	}

	return false;
}


// #pragma mark - WebSocketStream


WebSocketStream::WebSocketStream(TLSStream& stream)
	:
	fStream(stream),
	fIsClient(false),
	fCloseReceived(false),
	fCloseSent(false),
	fReceiveBufferUsed(0),
	fFrameRemaining(0),
	fFrameMasked(false),
	fFrameMaskPosition(0),
	fFrameIsControl(false),
	fFrameOpcode(0),
	fControlPayloadUsed(0),
	fWriteDeadline(0)
{
}


WebSocketStream::~WebSocketStream()
{
}


status_t
WebSocketStream::AcceptHandshake(bigtime_t deadline)
{
	fIsClient = false;

	// Read the HTTP request up to the header terminator. Anything the client
	// pipelined after it stays in the receive buffer for frame decoding.
	char request[8192];
	size_t used = 0;
	char* headersEnd = NULL;
	while (headersEnd == NULL) {
		if (used == sizeof(request) - 1) {
			TRACE_ERROR("oversized upgrade request\n");
			return B_BAD_DATA;
		}

		ssize_t read = fStream.Read(request + used,
			sizeof(request) - 1 - used, deadline);
		if (read < 0)
			return (status_t)read;
		if (read == 0)
			return B_IO_ERROR;

		used += read;
		request[used] = '\0';
		headersEnd = strstr(request, "\r\n\r\n");
	}

	size_t headersLength = headersEnd + 4 - request;
	size_t pipelined = used - headersLength;
	if (pipelined > 0) {
		if (pipelined > sizeof(fReceiveBuffer))
			return B_BAD_DATA;
		memcpy(fReceiveBuffer, request + headersLength, pipelined);
		fReceiveBufferUsed = pipelined;
	}
	request[headersLength] = '\0';

	char key[128];
	char upgrade[128];
	if (strncmp(request, "GET ", 4) != 0
		|| !find_header(request, "Upgrade", upgrade, sizeof(upgrade))
		|| strcasecmp(upgrade, "websocket") != 0
		|| !find_header(request, "Sec-WebSocket-Key", key, sizeof(key))
		|| strlen(key) > 64) {
		static const char* kBadRequest
			= "HTTP/1.1 400 Bad Request\r\n"
			"Connection: close\r\n\r\n";
		fStream.WriteFully(kBadRequest, strlen(kBadRequest), deadline);
		return B_BAD_DATA;
	}

	char acceptKey[32];
	compute_accept_key(key, acceptKey);

	// Echo the "binary" subprotocol if the client offered it (websockify
	// heritage; the in-tree HTML5 client requests it).
	char protocolHeader[64] = "";
	char protocol[128];
	if (find_header(request, "Sec-WebSocket-Protocol", protocol,
			sizeof(protocol))
		&& header_contains_token(protocol, "binary")) {
		strlcpy(protocolHeader, "Sec-WebSocket-Protocol: binary\r\n",
			sizeof(protocolHeader));
	}

	char response[512];
	int responseLength = snprintf(response, sizeof(response),
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n"
		"%s"
		"\r\n", acceptKey, protocolHeader);

	return fStream.WriteFully(response, responseLength, deadline);
}


status_t
WebSocketStream::ClientHandshake(const char* host, bigtime_t deadline)
{
	fIsClient = true;

	unsigned char keyBytes[16];
	if (RAND_bytes(keyBytes, sizeof(keyBytes)) != 1)
		return B_ERROR;

	char key[32];
	EVP_EncodeBlock((unsigned char*)key, keyBytes, sizeof(keyBytes));

	char request[512];
	int requestLength = snprintf(request, sizeof(request),
		"GET / HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade\r\n"
		"Sec-WebSocket-Key: %s\r\n"
		"Sec-WebSocket-Version: 13\r\n"
		"Sec-WebSocket-Protocol: binary\r\n"
		"\r\n", host, key);

	status_t result = fStream.WriteFully(request, requestLength, deadline);
	if (result != B_OK)
		return result;

	char response[8192];
	size_t used = 0;
	char* headersEnd = NULL;
	while (headersEnd == NULL) {
		if (used == sizeof(response) - 1)
			return B_BAD_DATA;

		ssize_t read = fStream.Read(response + used,
			sizeof(response) - 1 - used, deadline);
		if (read < 0)
			return (status_t)read;
		if (read == 0)
			return B_IO_ERROR;

		used += read;
		response[used] = '\0';
		headersEnd = strstr(response, "\r\n\r\n");
	}

	size_t headersLength = headersEnd + 4 - response;
	size_t pipelined = used - headersLength;
	if (pipelined > 0) {
		if (pipelined > sizeof(fReceiveBuffer))
			return B_BAD_DATA;
		memcpy(fReceiveBuffer, response + headersLength, pipelined);
		fReceiveBufferUsed = pipelined;
	}
	response[headersLength] = '\0';

	if (strncmp(response, "HTTP/1.1 101", 12) != 0) {
		TRACE_ERROR("upgrade refused: %.64s\n", response);
		return B_NOT_ALLOWED;
	}

	char expectedAccept[32];
	compute_accept_key(key, expectedAccept);

	char acceptValue[128];
	if (!find_header(response, "Sec-WebSocket-Accept", acceptValue,
			sizeof(acceptValue))
		|| strcmp(acceptValue, expectedAccept) != 0) {
		TRACE_ERROR("Sec-WebSocket-Accept mismatch\n");
		return B_BAD_DATA;
	}

	return B_OK;
}


ssize_t
WebSocketStream::ReadSome(void* buffer, size_t size, bigtime_t deadline)
{
	if (size == 0)
		return 0;

	// Frame writes triggered from inside the parser (pong, close echo) use
	// the caller's deadline.
	fWriteDeadline = deadline;

	while (true) {
		ssize_t produced = _ParseBuffer((uint8*)buffer, size);
		if (produced != 0)
			return produced;

		if (fCloseReceived)
			return 0;

		status_t result = _FillReceiveBuffer(deadline);
		if (result != B_OK)
			return result;
	}
}


status_t
WebSocketStream::WriteAll(const void* buffer, size_t size, bigtime_t deadline)
{
	return _SendFrame(kOpcodeBinary, buffer, size, deadline);
}


bool
WebSocketStream::HasBufferedData() const
{
	return fReceiveBufferUsed > 0 || fStream.HasBufferedData();
}


/*static*/ void
WebSocketStream::Relay(WebSocketStream& webSocket, TLSStream& stream,
	int plainSocket)
{
	// Bound on a blocked relay write; a peer that stalls the stream this
	// long is gone or hostile either way.
	static const bigtime_t kRelayWriteTimeout = 30 * 1000 * 1000;

	uint8 buffer[16 * 1024];

	while (true) {
		if (!webSocket.HasBufferedData()) {
			fd_set readSet;
			FD_ZERO(&readSet);
			FD_SET(stream.Socket(), &readSet);
			FD_SET(plainSocket, &readSet);
			int maxSocket = stream.Socket() > plainSocket
				? stream.Socket() : plainSocket;

			int ready = select(maxSocket + 1, &readSet, NULL, NULL, NULL);
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				return;
			}

			if (FD_ISSET(plainSocket, &readSet)) {
				ssize_t read = recv(plainSocket, buffer, sizeof(buffer), 0);
				if (read <= 0)
					return;
				if (webSocket.WriteAll(buffer, read,
						system_time() + kRelayWriteTimeout) != B_OK) {
					return;
				}
			}

			if (!FD_ISSET(stream.Socket(), &readSet))
				continue;
		}

		ssize_t read = webSocket.ReadSome(buffer, sizeof(buffer),
			system_time() + kRelayWriteTimeout);
		if (read <= 0)
			return;

		const uint8* position = buffer;
		size_t remaining = read;
		while (remaining > 0) {
			ssize_t written = write(plainSocket, position, remaining);
			if (written < 0) {
				if (errno == EINTR)
					continue;
				return;
			}
			position += written;
			remaining -= written;
		}
	}
}


void
WebSocketStream::SendClose(uint16 code, bigtime_t deadline)
{
	if (fCloseSent)
		return;
	fCloseSent = true;

	uint8 payload[2] = { (uint8)(code >> 8), (uint8)(code & 0xff) };
	_SendFrame(kOpcodeClose, payload, sizeof(payload), deadline);
}


/*!	Decodes as much of the receive buffer as possible. Returns the number of
	payload bytes copied to \a buffer (may be 0 when more network data is
	needed), or a negative error code on protocol violations.
*/
ssize_t
WebSocketStream::_ParseBuffer(uint8* buffer, size_t size)
{
	size_t consumed = 0;
	size_t produced = 0;

	while (produced < size) {
		size_t available = fReceiveBufferUsed - consumed;

		if (fFrameRemaining > 0) {
			if (available == 0)
				break;

			size_t chunk = available;
			if (chunk > fFrameRemaining)
				chunk = (size_t)fFrameRemaining;

			if (fFrameIsControl) {
				for (size_t i = 0; i < chunk; i++) {
					uint8 byte = fReceiveBuffer[consumed + i];
					if (fFrameMasked)
						byte ^= fFrameMask[fFrameMaskPosition++ % 4];
					fControlPayload[fControlPayloadUsed++] = byte;
				}
			} else {
				if (chunk > size - produced)
					chunk = size - produced;
				for (size_t i = 0; i < chunk; i++) {
					uint8 byte = fReceiveBuffer[consumed + i];
					if (fFrameMasked)
						byte ^= fFrameMask[fFrameMaskPosition++ % 4];
					buffer[produced++] = byte;
				}
			}

			consumed += chunk;
			fFrameRemaining -= chunk;

			if (fFrameRemaining == 0 && fFrameIsControl) {
				status_t result = _HandleControlFrame(fFrameOpcode,
					fControlPayload, fControlPayloadUsed);
				fControlPayloadUsed = 0;
				if (result != B_OK)
					return result;
				if (fCloseReceived)
					break;
			}

			continue;
		}

		// Parse the next frame header.
		if (available < 2)
			break;

		const uint8* header = fReceiveBuffer + consumed;
		uint8 flags = header[0];
		uint8 opcode = flags & 0x0f;
		bool fin = (flags & 0x80) != 0;
		bool masked = (header[1] & 0x80) != 0;
		uint64 payloadLength = header[1] & 0x7f;

		if ((flags & 0x70) != 0) {
			TRACE_ERROR("reserved bits set\n");
			return B_BAD_DATA;
		}

		size_t headerSize = 2;
		if (payloadLength == 126)
			headerSize += 2;
		else if (payloadLength == 127)
			headerSize += 8;
		if (masked)
			headerSize += 4;

		if (available < headerSize)
			break;

		if (payloadLength == 126) {
			payloadLength = ((uint64)header[2] << 8) | header[3];
		} else if (payloadLength == 127) {
			payloadLength = 0;
			for (int i = 0; i < 8; i++)
				payloadLength = (payloadLength << 8) | header[2 + i];
		}

		// The peer that must mask is the client; the server must not
		// (RFC 6455 5.1). Enforced in both roles.
		if (fIsClient == masked) {
			TRACE_ERROR("frame masking violates role\n");
			return B_BAD_DATA;
		}

		bool isControl = (opcode & 0x08) != 0;
		if (isControl && (!fin || payloadLength > 125)) {
			TRACE_ERROR("malformed control frame\n");
			return B_BAD_DATA;
		}

		if (!isControl && opcode != kOpcodeBinary
			&& opcode != kOpcodeContinuation) {
			TRACE_ERROR("unsupported data frame opcode %u\n", opcode);
			return B_BAD_DATA;
		}

		if (payloadLength > kMaxDataFrameSize) {
			TRACE_ERROR("oversized frame\n");
			return B_BAD_DATA;
		}

		if (masked)
			memcpy(fFrameMask, header + headerSize - 4, 4);
		fFrameMasked = masked;
		fFrameMaskPosition = 0;
		fFrameRemaining = payloadLength;
		fFrameIsControl = isControl;
		fFrameOpcode = opcode;
		fControlPayloadUsed = 0;
		consumed += headerSize;

		if (fFrameRemaining == 0 && isControl) {
			status_t result = _HandleControlFrame(opcode, NULL, 0);
			if (result != B_OK)
				return result;
			if (fCloseReceived)
				break;
		}
	}

	if (consumed > 0) {
		memmove(fReceiveBuffer, fReceiveBuffer + consumed,
			fReceiveBufferUsed - consumed);
		fReceiveBufferUsed -= consumed;
	}

	return produced;
}


status_t
WebSocketStream::_FillReceiveBuffer(bigtime_t deadline)
{
	if (fReceiveBufferUsed == sizeof(fReceiveBuffer)) {
		// Cannot happen with the frame sizes the parser accepts, but never
		// spin on a full buffer.
		return B_BAD_DATA;
	}

	ssize_t read = fStream.Read(fReceiveBuffer + fReceiveBufferUsed,
		sizeof(fReceiveBuffer) - fReceiveBufferUsed, deadline);
	if (read < 0)
		return (status_t)read;
	if (read == 0) {
		// Peer went away without a close frame; treat as closed.
		fCloseReceived = true;
		return B_OK;
	}

	fReceiveBufferUsed += read;
	return B_OK;
}


status_t
WebSocketStream::_SendFrame(uint8 opcode, const void* payload, size_t size,
	bigtime_t deadline)
{
	uint8 header[14];
	size_t headerSize = 2;

	header[0] = 0x80 | opcode;

	if (size <= 125) {
		header[1] = (uint8)size;
	} else if (size <= 65535) {
		header[1] = 126;
		header[2] = (uint8)(size >> 8);
		header[3] = (uint8)(size & 0xff);
		headerSize += 2;
	} else {
		header[1] = 127;
		for (int i = 0; i < 8; i++)
			header[2 + i] = (uint8)((uint64)size >> (8 * (7 - i)));
		headerSize += 8;
	}

	uint8 mask[4] = { 0, 0, 0, 0 };
	if (fIsClient) {
		header[1] |= 0x80;
		if (RAND_bytes(mask, sizeof(mask)) != 1)
			return B_ERROR;
		memcpy(header + headerSize, mask, 4);
		headerSize += 4;
	}

	status_t result = fStream.WriteFully(header, headerSize, deadline);
	if (result != B_OK)
		return result;

	if (size == 0)
		return B_OK;

	if (!fIsClient)
		return fStream.WriteFully(payload, size, deadline);

	// Masked (client) payload; mask in bounded chunks to keep the stack flat.
	const uint8* source = (const uint8*)payload;
	uint8 chunk[4096];
	size_t offset = 0;
	while (offset < size) {
		size_t chunkSize = size - offset;
		if (chunkSize > sizeof(chunk))
			chunkSize = sizeof(chunk);
		for (size_t i = 0; i < chunkSize; i++)
			chunk[i] = source[offset + i] ^ mask[(offset + i) % 4];

		result = fStream.WriteFully(chunk, chunkSize, deadline);
		if (result != B_OK)
			return result;

		offset += chunkSize;
	}

	return B_OK;
}


status_t
WebSocketStream::_HandleControlFrame(uint8 opcode, const uint8* payload,
	size_t size)
{
	switch (opcode) {
		case kOpcodePing:
			return _SendFrame(kOpcodePong, payload, size, fWriteDeadline);

		case kOpcodePong:
			return B_OK;

		case kOpcodeClose:
		{
			fCloseReceived = true;
			if (!fCloseSent) {
				fCloseSent = true;
				_SendFrame(kOpcodeClose, payload, size > 2 ? 2 : size,
					fWriteDeadline);
			}
			return B_OK;
		}

		default:
			TRACE_ERROR("unknown control opcode %u\n", opcode);
			return B_BAD_DATA;
	}
}
