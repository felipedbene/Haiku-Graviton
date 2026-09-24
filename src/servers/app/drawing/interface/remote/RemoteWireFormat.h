/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef REMOTE_WIRE_FORMAT_H
#define REMOTE_WIRE_FORMAT_H

#include <SupportDefs.h>


/*!	URP/1 compressed segment framing -- the transport record layer that sits
	*below* the RP message framing, on the server -> client direction only.

	A URP/1 stream starts out as a plain sequence of RP messages
	(uint16 code, uint32 totalLength, payload), byte for byte what every
	version of this protocol has always sent. If, and only if, the client
	advertised a compression capability in RP_HELLO and the server echoed it
	back in RP_HELLO_ACK, then from the byte immediately after that
	RP_HELLO_ACK message the server -> client direction is a sequence of
	segments instead:

		header   varint  value = (payloadLength << 1) | rawFlag
		payload  payloadLength bytes

	rawFlag 0 -- the payload is a fragment of a single session-long zstd
		stream. Feeding every compressed segment's payload, in order, to one
		ZSTD_DStream reproduces the plain RP message stream. The server calls
		ZSTD_e_flush only on a message boundary -- never inside a message -- so
		a decoder is never left holding a partial message once the segments for
		that message have arrived; the retained window across messages is where
		the ratio comes from, since consecutive drawing ops repeat opcodes,
		state and coordinates.

		A flush lands on *some* message boundaries, not all of them: the server
		batches the messages written within one drain window and flushes once
		for the batch, because a flush ends a zstd block and a block header is
		worth more than the mean 25 to 37 byte drawing op it would be spent on
		(issue #543 -- per-message flushing measured 1.95x where per-window
		measured 5.16x on the same bytes). A decoder cannot tell, and must not
		try to: it decodes whatever complete messages the bytes it has produce,
		and waits for more. What it may not assume is that the segments for the
		message it wants have already been sent.

	rawFlag 1 -- the payload is that many bytes of plain RP message stream,
		passed through untouched and NOT entered into the compressor's
		history. This is the exemption path for payloads that are already
		compressed (see RemoteWireWriter::_IsPreCompressed): running an
		entropy coder over JPEG/H.264/Opus bytes spends CPU for approximately
		nothing. A raw segment can be interleaved at any message boundary
		without disturbing the zstd stream that surrounds it -- the server
		closes any open drain window first, so that the messages before a raw
		one are on the wire before it and the peer decodes them in the order
		they were written.

	Segment lengths are varints rather than fixed 32-bit fields so that the
	framing cannot make the small, latency-critical messages bigger: a cursor
	move compresses to a handful of bytes and pays a single header byte.

	The direction is deliberately one-way. Client -> server carries input
	events and query replies: a few tens of bytes each, latency-critical, and
	nothing on that side is a bandwidth problem, so compressing it would mean
	a compressor in every client for no measurable gain.
*/

// varint: little-endian base 128, low seven bits are data, high bit means
// "another byte follows". Five bytes cover the full uint32 range.
#define REMOTE_SEGMENT_MAX_VARINT_SIZE	5

// Largest payload a single segment may declare. Bounds what a decoder will
// allocate on a peer's word alone. Plain segments carry at most one message's
// worth of bytes and compressed ones are capped by the encoder's output
// buffer, so this is orders of magnitude above anything legitimate.
#define REMOTE_SEGMENT_MAX_PAYLOAD		(64u * 1024 * 1024)


/*!	Writes the segment header for \a payloadLength bytes of payload (\a raw
	selecting the passthrough form) into \a buffer, which must have room for
	REMOTE_SEGMENT_MAX_VARINT_SIZE bytes. Returns the number of bytes written.
*/
static inline size_t
remote_segment_header_write(uint8* buffer, size_t payloadLength, bool raw)
{
	uint64 value = ((uint64)payloadLength << 1) | (raw ? 1 : 0);
	size_t size = 0;
	while (value >= 0x80) {
		buffer[size++] = (uint8)(value | 0x80);
		value >>= 7;
	}

	buffer[size++] = (uint8)value;
	return size;
}


/*!	Decodes a segment header from \a buffer / \a size.

	Returns the number of header bytes consumed and fills in \a _payloadLength
	and \a _raw on success; 0 when the header is not complete yet (the caller
	must read more bytes and retry); -1 when the header is malformed or
	declares a payload above REMOTE_SEGMENT_MAX_PAYLOAD, which means the
	stream has desynchronised and the connection must be dropped.
*/
static inline int
remote_segment_header_read(const uint8* buffer, size_t size,
	size_t& _payloadLength, bool& _raw)
{
	uint64 value = 0;
	for (size_t i = 0; i < size && i < REMOTE_SEGMENT_MAX_VARINT_SIZE; i++) {
		value |= (uint64)(buffer[i] & 0x7f) << (7 * i);
		if ((buffer[i] & 0x80) != 0)
			continue;

		uint64 payloadLength = value >> 1;
		if (payloadLength > REMOTE_SEGMENT_MAX_PAYLOAD)
			return -1;

		_payloadLength = (size_t)payloadLength;
		_raw = (value & 1) != 0;
		return (int)(i + 1);
	}

	// A continuation bit on the fifth byte cannot be a valid uint32 varint.
	if (size >= REMOTE_SEGMENT_MAX_VARINT_SIZE)
		return -1;

	return 0;
}


#endif	// REMOTE_WIRE_FORMAT_H
