/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef REMOTE_WIRE_WRITER_H
#define REMOTE_WIRE_WRITER_H

#include <Locker.h>
#include <OS.h>
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

	Messages are flushed out of the compressor once per *drain window*, not once
	per message (issue #543). Flushing per message costs 3.1x the bytes, because
	every flush ends a zstd block and pays its own entropy tables for as little
	as 25 bytes of drawing op. The window is closed by the flusher thread below;
	what it never does is flush in the middle of a message, so the property
	RemoteWireFormat.h states -- a decoder is never left holding a *partial*
	message -- is exactly preserved: whole messages simply share a flush.
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

			/*!	Ends the open drain window now: everything written so far
					leaves as segments the peer can decode.

				Called by the flusher thread when the window is up or the
				drawing threads have gone quiet, and directly by anything that
				must not wait for either -- a query the server then blocks on,
				and the wire self-test, which has to see the bytes it just
				asked for. Idempotent: with no window open it does nothing. */
			status_t			Flush();

			bool				IsCompressing() const { return fCapability != 0; }

			/*!	Cumulative counters for the connection, for the A/B
				measurement: bytes offered by the drawing engines, bytes
				actually written to the ring buffer, messages, exempt (raw)
				messages, flushes of the compressed stream, and total time spent
				inside the compressor. */
			void				GetStatistics(uint64& _plainBytes,
									uint64& _wireBytes, uint64& _messages,
									uint64& _exemptMessages, uint64& _flushes,
									bigtime_t& _encodeTime) const;

			//! The drain window in use, for tests and for the statistics line.
			bigtime_t			FlushWindow() const { return fFlushWindow; }

private:
			status_t			_WriteLocked(const void* buffer, size_t length);
			status_t			_WriteCompressed(const void* buffer,
									size_t length, bool flushNow);
			status_t			_FlushLocked();
			status_t			_WriteStagedSegment(size_t length);
			status_t			_WriteRawSegment(const void* payload,
									size_t length);
			status_t			_BreakStream(const char* what,
									status_t reason);
			void				_ResetCodec();
	static	bool				_IsPreCompressed(uint16 code);
	static	bool				_MustFlushNow(uint16 code);
			void				_OpenWindow();
			void				_StartFlusher();
			void				_StopFlusher();
	static	int32				_FlusherEntry(void* data);
			void				_Flusher();
			void				_MaybeReportStatistics();

			StreamingRingBuffer* fTarget;
			mutable BLocker		fLock;

			// Zero while the stream is plain, otherwise the single negotiated
			// capability bit in use.
			uint32				fCapability;

			// Set by PrepareCompression() once the codec below is built and
			// ready; WriteAndEnable() will only arm a capability that matches.
			uint32				fPreparedCapability;

			// Latched the moment a segment reaches the ring buffer only in
			// part, or not at all once the stream is compressed. Either way the
			// peer's decoder can no longer be in step with this compressor's
			// window, and no later segment can put it back -- so stop emitting
			// them instead of compressing into a stream nobody can decode.
			// Cleared only by Reset(), i.e. at the next connection boundary.
			bool				fStreamBroken;

			void*				fCompressionContext;

			// Staging buffer for the compressor's output. The first
			// REMOTE_SEGMENT_MAX_VARINT_SIZE bytes are reserved for the segment
			// header so that header and payload leave as a single ring-buffer
			// write; the compressor writes at fOutputBuffer + that reserve.
			uint8*				fOutputBuffer;
			size_t				fOutputBufferSize;

			// How long messages may accumulate in the compressor before the
			// window is closed. Zero restores the pre-#543 policy -- one flush
			// per message -- which is what the A/B's control arm runs.
			bigtime_t			fFlushWindow;

			// system_time() when the open window's first message was
			// compressed, or 0 when no window is open, i.e. when everything
			// written so far has been flushed. Guarded by fLock, like
			// everything else here; the flusher thread takes the same lock.
			bigtime_t			fWindowOpened;

			// Messages compressed into the open window. The flusher watches it
			// for a change: an unchanged count means the drawing threads have
			// stopped feeding the window, so it can be closed early.
			uint64				fWindowMessages;

			thread_id			fFlusher;
			sem_id				fFlushSignal;
			bool				fFlusherQuitting;

			uint64				fPlainBytes;
			uint64				fWireBytes;
			uint64				fMessages;
			uint64				fExemptMessages;
			uint64				fFlushes;
			bigtime_t			fEncodeTime;
			bigtime_t			fLastReport;
};

#endif	// REMOTE_WIRE_WRITER_H
