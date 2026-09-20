/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef WEB_SOCKET_STREAM_H
#define WEB_SOCKET_STREAM_H

#include <OS.h>
#include <SupportDefs.h>

class TLSStream;


/*!	RFC 6455 WebSocket framing over a TLSStream, presented as a plain byte
	stream: consecutive binary frames are one continuous stream in both
	directions (the remote protocol has its own 6-byte framing and does not
	rely on WebSocket message boundaries). Text frames are a protocol error.
	Ping, pong and close are handled internally. One thread at a time.
*/
class WebSocketStream {
public:
								WebSocketStream(TLSStream& stream);
								~WebSocketStream();

			// Server side: read and answer the HTTP upgrade request.
			status_t			AcceptHandshake(bigtime_t deadline);

			// Client side: send the upgrade request and verify the response.
			status_t			ClientHandshake(const char* host,
									bigtime_t deadline);

			// Returns the (positive) number of payload bytes read, 0 when
			// the connection was closed, or a negative error code
			// (B_TIMED_OUT when the deadline passed first).
			ssize_t				ReadSome(void* buffer, size_t size,
									bigtime_t deadline);

			// Sends one binary frame (masked when this is the client side).
			status_t			WriteAll(const void* buffer, size_t size,
									bigtime_t deadline);

			// Whether a Read can make progress without the underlying socket
			// becoming readable (data buffered here or in the TLS layer).
			bool				HasBufferedData() const;

			void				SendClose(uint16 code, bigtime_t deadline);

			// Bidirectional relay between this WebSocket and a plain
			// socket, until either side ends. Blocks; run it on the thread
			// that owns both.
	static	void				Relay(WebSocketStream& webSocket,
									TLSStream& stream, int plainSocket);

private:
			ssize_t				_ParseBuffer(uint8* buffer, size_t size);
			status_t			_FillReceiveBuffer(bigtime_t deadline);
			status_t			_SendFrame(uint8 opcode, const void* payload,
									size_t size, bigtime_t deadline);
			status_t			_HandleControlFrame(uint8 opcode,
									const uint8* payload, size_t size);

			TLSStream&			fStream;
			bool				fIsClient;
			bool				fCloseReceived;
			bool				fCloseSent;

			// Raw (still framed) received bytes.
			uint8				fReceiveBuffer[16 * 1024];
			size_t				fReceiveBufferUsed;

			// Decoder state for the frame currently being consumed.
			uint64				fFrameRemaining;
			bool				fFrameMasked;
			uint8				fFrameMask[4];
			uint32				fFrameMaskPosition;
			bool				fFrameIsControl;
			uint8				fFrameOpcode;

			// Payload of a control frame being accumulated (<= 125 bytes).
			uint8				fControlPayload[125];
			size_t				fControlPayloadUsed;

			bigtime_t			fWriteDeadline;
};


#endif // WEB_SOCKET_STREAM_H
