/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef REMOTE_WIRE_READER_H
#define REMOTE_WIRE_READER_H

#include <SupportDefs.h>

#include "RemoteWireFormat.h"

class StreamingRingBuffer;


/*!	The client-side counterpart of RemoteWireWriter: turns the bytes arriving
	from the socket back into the plain RP message stream the rest of a client
	already knows how to parse, so nothing above this class changes.

	It finds the switch-over point itself rather than being told about it, and
	that is the whole point of the design. The negotiated capability set arrives
	in RP_HELLO_ACK, but in every client the message parser runs on a different
	thread from the socket reader, behind a ring buffer. By the time the parser
	sees the acknowledgement the reader may already have pushed the first
	compressed bytes through as plain -- which would desynchronise the stream
	permanently. So the reader watches the framing itself while the stream is
	still plain, reads the negotiated bitmap out of RP_HELLO_ACK as it passes,
	and switches at exactly the right byte.

	Not thread safe: it is owned and driven by the single socket-reading thread.
*/
class RemoteWireReader {
public:
								RemoteWireReader(StreamingRingBuffer* target);
								~RemoteWireReader();

	//! The compression capability bits this build can actually decode.
	static	uint32				SupportedCapabilities();

			/*!	Feeds \a length bytes as they came off the socket. Writes the
				plain message stream into the target ring buffer. Returns an
				error only when the stream is unusable (malformed framing or a
				decoder failure), in which case the caller must drop the
				connection: a desynchronised stream cannot be resynchronised. */
			status_t			Process(const uint8* buffer, size_t length);

			//! Forgets all stream state, for a fresh connection.
			void				Reset();

			bool				IsCompressed() const { return fCompressed; }

private:
			status_t			_ProcessPlain(const uint8*& buffer,
									size_t& length);
			status_t			_ProcessSegmented(const uint8*& buffer,
									size_t& length);
			status_t			_Decompress(const uint8* buffer, size_t length);
			bool				_StartCodec(uint32 capability);
			void				_ResetCodec();

			StreamingRingBuffer* fTarget;

			bool				fCompressed;

			// Plain-stream framing scanner, live only until the switch.
			uint8				fHeader[6];
			size_t				fHeaderUsed;
			size_t				fBodyLeft;
			bool				fCapturingAck;
			uint8				fAck[8];
			size_t				fAckUsed;

			// Segment framing state.
			uint8				fSegmentHeader[REMOTE_SEGMENT_MAX_VARINT_SIZE];
			size_t				fSegmentHeaderUsed;
			size_t				fSegmentLeft;
			bool				fSegmentRaw;
			bool				fSegmentPending;

			void*				fDecompressionContext;
			uint8*				fOutputBuffer;
			size_t				fOutputBufferSize;
};

#endif	// REMOTE_WIRE_READER_H
