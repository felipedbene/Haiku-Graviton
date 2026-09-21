/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef REMOTE_WIRE_WRITER_H
#define REMOTE_WIRE_WRITER_H

#include <Locker.h>
#include <SupportDefs.h>

class StreamingRingBuffer;


/*!	The server -> client end of the URP/1 wire: every outbound RP message goes
	through here on its way into the send ring buffer.

	Until a client negotiates a compression capability this is a pure
	passthrough and the bytes reaching the ring buffer are byte for byte what
	the protocol has always put there. Once Enable() has run, messages are
	compressed into segments as described in RemoteWireFormat.h.

	One instance per RemoteHWInterface, shared by every drawing engine thread.
	The lock is what makes "compress, then write" atomic: the compressed stream
	carries history across messages, so the order in which messages enter the
	compressor must be the order their bytes reach the socket. Without the lock
	two drawing threads could compress in one order and write in the other and
	desynchronise the client for the rest of the session. It adds no
	serialisation that was not already there -- StreamingRingBuffer::Write()
	already holds its writer lock across a whole message, including the wait
	for space.
*/
class RemoteWireWriter {
public:
								RemoteWireWriter(StreamingRingBuffer* target);
								~RemoteWireWriter();

	//! The compression capability bits this build can actually offer.
	static	uint32				SupportedCapabilities();

			/*!	Writes one complete RP message (\a buffer starts with the
				uint16 code, \a length is the whole framed message). */
			status_t			Write(const void* buffer, size_t length);

			/*!	Builds the codec for \a capability without switching the stream,
				and reports whether it can be honoured.

				This exists so that the decision the server announces and the
				capability it can actually deliver are the same decision. The
				announcement is RP_HELLO_ACK, and once those bytes are on the
				wire the client switches its decoder; a codec that then failed to
				build would leave the server emitting plain bytes into a decoder
				expecting segments, which is an unrecoverable desynchronisation
				that no error path can reach. So every way of failing to compress
				has to happen *before* the acknowledgement is composed. Call this
				first, and clear the bit from the acknowledgement if it returns
				false. */
			bool				PrepareCompression(uint32 capability);

			/*!	Writes \a buffer uncompressed and, atomically with it, switches
				the stream to compressed segments for \a capability. Used for
				RP_HELLO_ACK: the acknowledgement itself must reach a client
				that is still reading plain bytes, and the very next byte the
				client reads must already be a segment. Doing this in two steps
				would let another thread's drawing op slip in between and be
				misparsed as a segment header.

				\a capability must be zero or a capability for which
				PrepareCompression() has already returned true; anything else is
				refused with an error rather than silently downgraded, because a
				silent downgrade here is the desynchronisation described above. */
			status_t			WriteAndEnable(const void* buffer,
									size_t length, uint32 capability);

			/*!	Drops the send buffer and returns to the plain stream, for the
				next connection to renegotiate from scratch. Empties the ring
				buffer under the same lock that serialises writers, so a
				concurrent drawing op is either wholly discarded or wholly part
				of the fresh stream and cannot be left torn in the middle. */
			void				Reset();

			bool				IsCompressing() const { return fCapability != 0; }

			/*!	Cumulative counters for the connection, for the A/B
				measurement: bytes offered by the drawing engines, bytes
				actually written to the ring buffer, messages, exempt (raw)
				messages, and total time spent inside the compressor. */
			void				GetStatistics(uint64& _plainBytes,
									uint64& _wireBytes, uint64& _messages,
									uint64& _exemptMessages,
									bigtime_t& _encodeTime) const;

private:
			status_t			_WriteLocked(const void* buffer, size_t length);
			status_t			_WriteCompressed(const void* buffer,
									size_t length);
			status_t			_WriteSegment(const void* payload,
									size_t length, bool raw);
			void				_ResetCodec();
	static	bool				_IsPreCompressed(uint16 code);
			void				_MaybeReportStatistics();

			StreamingRingBuffer* fTarget;
			mutable BLocker		fLock;

			// Zero while the stream is plain, otherwise the single negotiated
			// capability bit in use.
			uint32				fCapability;

			// Set by PrepareCompression() once the codec below is built and
			// ready; WriteAndEnable() will only arm a capability that matches.
			uint32				fPreparedCapability;

			void*				fCompressionContext;
			uint8*				fOutputBuffer;
			size_t				fOutputBufferSize;

			uint64				fPlainBytes;
			uint64				fWireBytes;
			uint64				fMessages;
			uint64				fExemptMessages;
			bigtime_t			fEncodeTime;
			bigtime_t			fLastReport;
};

#endif	// REMOTE_WIRE_WRITER_H
