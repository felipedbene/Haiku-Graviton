/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef REMOTE_PROTOCOL_H
#define REMOTE_PROTOCOL_H


/*!	The URP/1 wire vocabulary: protocol version, capability bits and the
	opcode table.

	Split out of RemoteMessage.h so that it is the single source of truth for
	every consumer, including ones that cannot include RemoteMessage.h at all --
	it pulls in PatternHandler, the wire writer, ViewPrivate and BRegion, none of
	which an off-target unit test of the flow-control policy can build against.
	This header needs nothing but the preprocessor, so the test classifies the
	real opcodes rather than a copy of them that can drift.
*/

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

	// The client understands RP_TIER_END_FRAME as a frame boundary: everything
	// it received since the previous boundary is one coherent update. A client
	// may use it to present once per frame instead of once per draw op; the
	// server uses it to delimit what its outbound flow-control queue may
	// coalesce within, and what it may drop whole (see RemoteFlowQueue and
	// graviton/docs/remote-desktop-m2-flow-control.md).
	//
	// The boundary is not *created* by this bit. The server has always emitted
	// RP_INVALIDATE_RECT / RP_INVALIDATE_REGION at exactly that instant -- it is
	// how the client is told to copy back to front -- and the queue falls back
	// to treating those as the boundary for a client that has never heard of
	// URP/1, so flow control is not capability-gated even though the opcode is.
	// What the bit buys is a boundary that is stated rather than inferred, one
	// that survives a change to how damage is announced, and the frame sequence
	// number Tier P's RP_FRAME_ACK pacing needs.
	//
	// Gated because the in-tree native client routes an unknown opcode to its
	// default case: a client that did not ask for frame boundaries never sees
	// one, and its byte stream is unchanged.
	RP_CAP_FRAME_BOUNDARY		= 1 << 3,
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

#endif	// REMOTE_PROTOCOL_H
