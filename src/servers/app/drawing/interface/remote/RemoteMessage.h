/*
 * Copyright 2009, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */
#ifndef REMOTE_MESSAGE_H
#define REMOTE_MESSAGE_H

#ifndef CLIENT_COMPILE
#	include "PatternHandler.h"
#	include "RemoteWireWriter.h"
#	include <ViewPrivate.h>
#endif

#include "StreamingRingBuffer.h"

#include <AffineTransform.h>
#include <GraphicsDefs.h>
#include <Region.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

class BBitmap;
class BFont;
class BGradient;
class BView;
class DrawState;
class Pattern;
class RemotePainter;
class ServerBitmap;
class ServerCursor;
class ServerFont;
struct ViewLineArrayInfo;

// URP/1 protocol version carried on the wire in RP_HELLO / RP_HELLO_ACK. The
// negotiated version is min(client, server); it is deliberately unrelated to the
// legacy accelerant fProtocolVersion field.
#define RP_PROTOCOL_VERSION 1

// URP/1 capability bits. A client announces what it can do in the RP_HELLO
// feature bitmap; the server may only use a feature the client advertised, and
// echoes the negotiated intersection back in RP_HELLO_ACK. A pre-handshake
// client sends no RP_HELLO, so its capability set is empty and the server falls
// back to legacy behaviour -- which is exactly today's wire. New bits are added
// here as later milestones land (bitmap cache, resync, frame boundary, Tier P
// codecs); M0 defines only the one it uses.
enum {
	// Bit 0 is RETIRED and reserved: it used to mean "the client answers
	// RP_STRING_WIDTH with RP_STRING_WIDTH_RESULT". The server no longer asks
	// any client for text metrics (see the note on RP_STRING_WIDTH below), so it
	// no longer advertises the bit and never negotiates it. A client that still
	// offers bit 0 is not harmed -- the server masks an unsupported bit out of
	// the intersection it acknowledges -- but the bit MUST NOT be reused for a
	// different feature, because such a client would then be taken to have
	// promised something else entirely.
	//
	// RP_CAP_RESERVED_BIT0		= 1 << 0,

	// The client can decode a zstd-compressed server -> client stream. When
	// this is negotiated, everything the server sends after the RP_HELLO_ACK
	// message is framed in compressed segments instead of plain messages; see
	// RemoteWireFormat.h for the framing and RemoteWireWriter for what is
	// exempt from it. A client that does not advertise this gets the plain
	// stream, byte for byte as before.
	RP_CAP_COMPRESS_ZSTD		= 1 << 1,

	// The client understands RP_RESYNC and the session identity that
	// RP_HELLO_ACK carries when this bit is negotiated (session id +
	// connection generation). Two things become possible with it:
	//
	//   - the client can tell "same session, new connection" (same session id,
	//     higher generation) from "new session" (different session id), which
	//     is the difference between content it may still trust and content it
	//     must throw away;
	//   - the client can *ask* for a state replay instead of only ever being
	//     told, which is the only recovery available to a client that detects
	//     it has desynchronised for a reason the server cannot see.
	//
	// The server replays drawing state on every accept whether or not this is
	// negotiated -- that replay uses existing opcodes only and is what fixes
	// the reconnect black screen for every client, including one that has
	// never heard of URP/1. This bit gates the *conversation*, not the repair.
	RP_CAP_RESYNC				= 1 << 2,
};

// Session-cookie methods, carried in the RP_SESSION_COOKIE message. Only one
// exists; the field is there so a future challenge/response scheme does not
// need a second opcode (the same shape RP_AUTHENTICATE uses).
enum {
	RP_COOKIE_METHOD_PER_BOOT	= 1,
};

// Longest cookie accepted on the wire. The minted cookie is 64 hex characters
// (256 bits); the limit is generous so an operator-supplied one has room, and
// bounded so the candidate gate's buffer is a fixed size.
#define RP_SESSION_COOKIE_MAX_LENGTH 256

enum {
	RP_INIT_CONNECTION = 1,
	RP_UPDATE_DISPLAY_MODE,
	RP_CLOSE_CONNECTION,
	RP_GET_SYSTEM_PALETTE,
	RP_GET_SYSTEM_PALETTE_RESULT,
	RP_HELLO,
	RP_HELLO_ACK,

	// Resynchronisation, in both directions, gated on RP_CAP_RESYNC. Payload:
	// uint32 generation.
	//
	// server -> client: "everything after this message belongs to connection
	// generation N; discard anything you cached or inferred from an earlier
	// one, a full state replay follows." It is a barrier, not a request: the
	// replay is already on its way behind it.
	//
	// client -> server: "I believe I am at generation N (0 = I do not know) and
	// I cannot draw correctly; replay." The server answers with the barrier
	// above, replays every live drawing engine's state, and re-announces the
	// screen so a full repaint follows. This exists because the server cannot
	// see every way a client can lose its place -- a decoder that gave up on a
	// message, a canvas the browser threw away -- so the client has to be able
	// to say so rather than wait to be told.
	RP_RESYNC = 8,

	// Transport-security preamble, owned by the remote_broker daemon that
	// fronts the loopback RP listener with TLS + WebSocket for connections
	// from the open internet. RP_AUTHENTICATE must be the first message a
	// broker client sends (payload: uint32 method -- 1 = shared token --
	// followed by a length-prefixed string); the broker answers with
	// RP_AUTH_RESULT (uint32 status, 0 = success) and forwards nothing to the
	// session port until authentication has succeeded. app_server itself
	// never sends or processes either message; the values are reserved here
	// so no future session opcode collides with them.
	RP_AUTHENTICATE = 10,
	RP_AUTH_RESULT,

	// The per-boot session cookie, and the first frame of every connection to
	// the session port. Payload: uint32 method (1 = per-boot shared cookie)
	// followed by a length-prefixed cookie string.
	//
	// app_server mints the cookie into an owner-only file before its listener
	// exists and requires a matching one here before a connection may become
	// the session; the broker presents it after its own authentication
	// succeeded. Unlike RP_AUTHENTICATE this message IS app_server's, but it
	// is consumed entirely by the candidate gate in NetReceiver and never
	// reaches the message parser -- so above the gate a client's stream still
	// begins with RP_INIT_CONNECTION, exactly as before.
	RP_SESSION_COOKIE = 12,

	RP_CREATE_STATE = 20,
	RP_DELETE_STATE,
	RP_ENABLE_SYNC_DRAWING,
	RP_DISABLE_SYNC_DRAWING,
	RP_INVALIDATE_RECT,
	RP_INVALIDATE_REGION,

	RP_SET_OFFSETS = 40,
	RP_SET_HIGH_COLOR,
	RP_SET_LOW_COLOR,
	RP_SET_PEN_SIZE,
	RP_SET_STROKE_MODE,
	RP_SET_BLENDING_MODE,
	RP_SET_PATTERN,
	RP_SET_DRAWING_MODE,
	RP_SET_FONT,
	RP_SET_TRANSFORM,

	RP_CONSTRAIN_CLIPPING_REGION = 60,
	RP_COPY_RECT_NO_CLIPPING,
	RP_INVERT_RECT,
	RP_DRAW_BITMAP,
	RP_DRAW_BITMAP_RECTS,

	RP_STROKE_ARC = 80,
	RP_STROKE_BEZIER,
	RP_STROKE_ELLIPSE,
	RP_STROKE_POLYGON,
	RP_STROKE_RECT,
	RP_STROKE_ROUND_RECT,
	RP_STROKE_SHAPE,
	RP_STROKE_TRIANGLE,
	RP_STROKE_LINE,
	RP_STROKE_LINE_ARRAY,

	RP_FILL_ARC = 100,
	RP_FILL_BEZIER,
	RP_FILL_ELLIPSE,
	RP_FILL_POLYGON,
	RP_FILL_RECT,
	RP_FILL_ROUND_RECT,
	RP_FILL_SHAPE,
	RP_FILL_TRIANGLE,
	RP_FILL_REGION,

	RP_FILL_ARC_GRADIENT = 120,
	RP_FILL_BEZIER_GRADIENT,
	RP_FILL_ELLIPSE_GRADIENT,
	RP_FILL_POLYGON_GRADIENT,
	RP_FILL_RECT_GRADIENT,
	RP_FILL_ROUND_RECT_GRADIENT,
	RP_FILL_SHAPE_GRADIENT,
	RP_FILL_TRIANGLE_GRADIENT,
	RP_FILL_REGION_GRADIENT,

	RP_STROKE_POINT_COLOR = 140,
	RP_STROKE_LINE_1PX_COLOR,
	RP_STROKE_RECT_1PX_COLOR,

	RP_FILL_RECT_COLOR = 160,
	RP_FILL_REGION_COLOR_NO_CLIPPING,

	RP_DRAW_STRING = 180,
	RP_DRAW_STRING_WITH_OFFSETS,
	RP_DRAW_STRING_RESULT,

	// RETIRED and reserved (183, 184). The server never sends RP_STRING_WIDTH
	// and no longer handles RP_STRING_WIDTH_RESULT; it measures text with its
	// own ServerFont, which is the same metric source that answers an
	// application's BFont::StringWidth(), so the two cannot disagree. The names
	// stay so the numbering of everything after them does not shift, and so a
	// future implementor reuses neither number for a different message. See the
	// retired capability bit 0 above, and the comment on
	// DrawingEngine::StringWidth() for why the query could never be issued.
	RP_STRING_WIDTH,
	RP_STRING_WIDTH_RESULT,

	RP_READ_BITMAP,
	RP_READ_BITMAP_RESULT,

	RP_SET_CURSOR = 200,
	RP_SET_CURSOR_VISIBLE,
	RP_MOVE_CURSOR_TO,

	RP_MOUSE_MOVED = 220,
	RP_MOUSE_DOWN,
	RP_MOUSE_UP,
	RP_MOUSE_WHEEL_CHANGED,

	RP_KEY_DOWN = 240,
	RP_KEY_UP,
	RP_UNMAPPED_KEY_DOWN,
	RP_UNMAPPED_KEY_UP,
	RP_MODIFIERS_CHANGED,

	RP_STROKE_ARC_GRADIENT = 260,
	RP_STROKE_BEZIER_GRADIENT,
	RP_STROKE_ELLIPSE_GRADIENT,
	RP_STROKE_POLYGON_GRADIENT,
	RP_STROKE_RECT_GRADIENT,
	RP_STROKE_ROUND_RECT_GRADIENT,
	RP_STROKE_SHAPE_GRADIENT,
	RP_STROKE_TRIANGLE_GRADIENT,
	RP_STROKE_LINE_GRADIENT,

	// Tier P -- per-region encoded pixels and audio. Not implemented yet (a
	// later URP/1 milestone owns them); the values are reserved here the same
	// way M0 reserved the broker's RP_AUTHENTICATE pair before app_server used
	// it, so nothing else claims the block.
	//
	// They are reserved *now*, ahead of their implementation, because
	// RemoteWireWriter::_IsPreCompressed() has to name them: an
	// RP_CODEC_TILE carries JPEG/H.264/AV1 bytes and an RP_AUDIO_PACKET
	// carries Opus, all of which are entropy-coded already. The exemption has
	// to be keyed on the opcode and decided before a byte is compressed (the
	// compressor's window has moved on by the time its output could be
	// judged), so the alternative to reserving them is a rule that silently
	// fails to apply on the day Tier P lands.
	RP_TIER_BEGIN_FRAME = 280,
	RP_CODEC_TILE,
	RP_TIER_END_FRAME,
	RP_AUDIO_PACKET,
	RP_FRAME_ACK,
};


class RemoteMessage {
public:
								RemoteMessage(StreamingRingBuffer* source,
									StreamingRingBuffer *target);
#ifndef CLIENT_COMPILE
								/*!	Server-side outbound messages go through
									the wire writer, which owns the compression
									state and the ordering guarantee. */
								RemoteMessage(StreamingRingBuffer* source,
									RemoteWireWriter *target);
#endif
								~RemoteMessage();

		void					Start(uint16 code);
		status_t				Flush();
#ifndef CLIENT_COMPILE
		status_t				FlushAndEnableCompression(uint32 capability);
#endif
		void					Cancel();

		status_t				NextMessage(uint16& code);
		void					Reset();
		bool					ResetIfGenerationChanged(uint32 generation);
		uint16					Code() { return fCode; }
		uint32					DataLeft() { return fDataLeft; }

		template<typename T>
		void					Add(const T& value);

		void					AddString(const char* string, size_t length);
		void					AddRegion(const BRegion& region);
		void					AddGradient(const BGradient& gradient);
		void					AddTransform(const BAffineTransform& transform);

#ifndef CLIENT_COMPILE
		void					AddBitmap(const ServerBitmap& bitmap,
									bool minimal = false);
		void					AddFont(const ServerFont& font);
		void					AddPattern(const Pattern& pattern);
		void					AddDrawState(const DrawState& drawState);
		void					AddArrayLine(const ViewLineArrayInfo& line);
		void					AddCursor(const ServerCursor& cursor);
#else
		void					AddBitmap(const BBitmap& bitmap);
#endif

		template<typename T>
		void					AddList(const T* array, int32 count);

		template<typename T>
		status_t				Read(T& value);

		status_t				ReadRegion(BRegion& region);
		status_t				ReadFontState(BFont& font);
									// sets font state
		status_t				ReadViewState(BView& view, ::pattern& pattern);
									// sets viewstate and returns pattern

		status_t				ReadString(char** _string, size_t& length);
		status_t				ReadBitmap(BBitmap** _bitmap,
									bool minimal = false,
									color_space colorSpace = B_RGB32,
									uint32 flags = 0);
		status_t				ReadGradient(BGradient** _gradient);
		status_t				ReadTransform(BAffineTransform& transform);
		status_t				ReadArrayLine(BPoint& startPoint,
									BPoint& endPoint, rgb_color& color);

		template<typename T>
		status_t				ReadList(T* array, int32 count);

private:
		bool					_MakeSpace(size_t size);

		StreamingRingBuffer*	fSource;
		StreamingRingBuffer*	fTarget;
#ifndef CLIENT_COMPILE
		RemoteWireWriter*		fWireTarget;
#endif

		uint8*					fBuffer;
		size_t					fAvailable;
		size_t					fWriteIndex;
		uint32					fDataLeft;
		uint16					fCode;
		uint32					fGeneration;
};


inline
RemoteMessage::RemoteMessage(StreamingRingBuffer* source,
	StreamingRingBuffer* target)
	:
	fSource(source),
	fTarget(target),
#ifndef CLIENT_COMPILE
	fWireTarget(NULL),
#endif
	fBuffer(NULL),
	fAvailable(0),
	fWriteIndex(0),
	fDataLeft(0),
	fCode(0),
	fGeneration(0)
{
}


#ifndef CLIENT_COMPILE
inline
RemoteMessage::RemoteMessage(StreamingRingBuffer* source,
	RemoteWireWriter* target)
	:
	fSource(source),
	fTarget(NULL),
	fWireTarget(target),
	fBuffer(NULL),
	fAvailable(0),
	fWriteIndex(0),
	fDataLeft(0),
	fCode(0),
	fGeneration(0)
{
}
#endif


inline
RemoteMessage::~RemoteMessage()
{
	if (fWriteIndex > 0)
		Flush();
	free(fBuffer);
}


inline void
RemoteMessage::Start(uint16 code)
{
	if (fWriteIndex > 0)
		Flush();

	Add(code);

	uint32 sizeDummy = 0;
	Add(sizeDummy);
}


inline status_t
RemoteMessage::Flush()
{
#ifdef CLIENT_COMPILE
	if (fWriteIndex == 0 || fTarget == NULL)
		return B_NO_INIT;
#else
	if (fWriteIndex == 0 || (fTarget == NULL && fWireTarget == NULL))
		return B_NO_INIT;
#endif

	uint32 length = fWriteIndex;
	fAvailable += fWriteIndex;
	fWriteIndex = 0;

	memcpy(fBuffer + sizeof(uint16), &length, sizeof(uint32));

#ifndef CLIENT_COMPILE
	if (fWireTarget != NULL)
		return fWireTarget->Write(fBuffer, length);
#endif

	return fTarget->Write(fBuffer, length);
}


#ifndef CLIENT_COMPILE
/*!	Flushes this message and, atomically with it, switches the outbound stream
	to compressed segments. Only meaningful for RP_HELLO_ACK: see
	RemoteWireWriter::WriteAndEnable() for why the two steps cannot be separate.
*/
inline status_t
RemoteMessage::FlushAndEnableCompression(uint32 capability)
{
	if (fWriteIndex == 0 || fWireTarget == NULL)
		return B_NO_INIT;

	uint32 length = fWriteIndex;
	fAvailable += fWriteIndex;
	fWriteIndex = 0;

	memcpy(fBuffer + sizeof(uint16), &length, sizeof(uint32));
	return fWireTarget->WriteAndEnable(fBuffer, length, capability);
}
#endif


template<typename T>
inline void
RemoteMessage::Add(const T& value)
{
	if (!_MakeSpace(sizeof(T)))
		return;

	memcpy(fBuffer + fWriteIndex, &value, sizeof(T));
	fWriteIndex += sizeof(T);
	fAvailable -= sizeof(T);
}


inline void
RemoteMessage::AddString(const char* string, size_t length)
{
	Add((uint32)length);
	if (length > fAvailable && !_MakeSpace(length))
		return;

	memcpy(fBuffer + fWriteIndex, string, length);
	fWriteIndex += length;
	fAvailable -= length;
}


inline void
RemoteMessage::AddRegion(const BRegion& region)
{
	int32 rectCount = region.CountRects();
	Add(rectCount);

	for (int32 i = 0; i < rectCount; i++)
		Add(region.RectAt(i));
}


template<typename T>
inline void
RemoteMessage::AddList(const T* array, int32 count)
{
	for (int32 i = 0; i < count; i++)
		Add(array[i]);
}


template<typename T>
inline status_t
RemoteMessage::Read(T& value)
{
	if (fDataLeft < sizeof(T))
		return B_ERROR;

	if (fSource == NULL)
		return B_NO_INIT;

	int32 readSize = fSource->Read(&value, sizeof(T));
	if (readSize < 0)
		return readSize;

	if (readSize != sizeof(T))
		return B_ERROR;

	fDataLeft -= sizeof(T);
	return B_OK;
}


inline status_t
RemoteMessage::ReadRegion(BRegion& region)
{
	region.MakeEmpty();

	int32 rectCount;
	status_t result = Read(rectCount);
	if (result != B_OK)
		return B_ERROR;

	for (int32 i = 0; i < rectCount; i++) {
		BRect rect;
		status_t result = Read(rect);
		if (result != B_OK)
			return result;

		region.Include(rect);
	}

	return B_OK;
}


template<typename T>
inline status_t
RemoteMessage::ReadList(T* array, int32 count)
{
	for (int32 i = 0; i < count; i++) {
		status_t result = Read(array[i]);
		if (result != B_OK)
			return result;
	}

	return B_OK;
}


inline bool
RemoteMessage::_MakeSpace(size_t size)
{
	if (fAvailable >= size)
		return true;

	size_t extraSize = size + 20;
	uint8 *newBuffer = (uint8*)realloc(fBuffer, fWriteIndex + extraSize);
	if (newBuffer == NULL)
		return false;

	fAvailable = extraSize;
	fBuffer = newBuffer;
	return true;
}

#endif // REMOTE_MESSAGE_H
