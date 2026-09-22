/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */

#include "RemoteWireWriter.h"

#include "RemoteMessage.h"
#include "RemoteWireFormat.h"
#include "StreamingRingBuffer.h"

#include <Autolock.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZSTD_ENABLED
#	include <zstd.h>
#endif


// Matches the other files in this directory: the same sources are compiled into
// the client, which has no debug_printf().
#ifdef CLIENT_COMPILE
#	define TRACE_ALWAYS(x...)	printf("RemoteWireWriter: " x)
#else
#	define TRACE_ALWAYS(x...)	debug_printf("RemoteWireWriter: " x)
#endif

#define TRACE(x...)				/*TRACE_ALWAYS(x)*/
#define TRACE_ERROR(x...)		TRACE_ALWAYS(x)


// Compression level 1. The point of compressing this stream is latency on a
// constrained link, so the encoder must never become the bottleneck: at level 1
// zstd runs at hundreds of MB/s per core while still finding the repetition
// that dominates a drawing-op stream. Higher levels buy a few percent of ratio
// for multiples of the CPU time, which is the wrong trade for a display server.
static const int kCompressionLevel = 1;

// 1 MiB match window. Bounds the compressor's and every client decompressor's
// memory, and is far more history than a drawing-op stream needs -- the
// repetition it exploits is between neighbouring messages, not megabytes apart.
static const int kWindowLog = 20;

// Output staging buffer. The encoder loop writes one segment per filled buffer,
// so this only has to be large enough to make the per-segment header
// negligible; it does not have to bound a message's compressed size.
//
// The first kSegmentHeaderReserve bytes are not offered to the compressor: they
// are where the segment header is laid down, immediately in front of the
// payload it describes, so that the pair can leave as a single
// StreamingRingBuffer::Write(). A varint header is 1..5 bytes, so the header
// starts somewhere inside the reserve and ends exactly where the payload
// begins.
static const size_t kSegmentHeaderReserve = REMOTE_SEGMENT_MAX_VARINT_SIZE;
static const size_t kOutputBufferSize = 64 * 1024 + kSegmentHeaderReserve;

// At most one statistics line per this interval. The serial console on the
// target is write-bound, so the instrumentation has to cost bytes it can afford.
static const bigtime_t kStatisticsInterval = 5 * 1000 * 1000;


RemoteWireWriter::RemoteWireWriter(StreamingRingBuffer* target)
	:
	fTarget(target),
	fLock("remote wire writer"),
	fCapability(0),
	fPreparedCapability(0),
	fStreamBroken(false),
	fCompressionContext(NULL),
	fOutputBuffer(NULL),
	fOutputBufferSize(0),
	fPlainBytes(0),
	fWireBytes(0),
	fMessages(0),
	fExemptMessages(0),
	fEncodeTime(0),
	fLastReport(0)
{
}


RemoteWireWriter::~RemoteWireWriter()
{
	_ResetCodec();
}


/*static*/ uint32
RemoteWireWriter::SupportedCapabilities()
{
#ifdef ZSTD_ENABLED
	return RP_CAP_COMPRESS_ZSTD;
#else
	return 0;
#endif
}


status_t
RemoteWireWriter::Write(const void* buffer, size_t length)
{
	BAutolock lock(fLock);
	if (!lock.IsLocked())
		return B_ERROR;

	return _WriteLocked(buffer, length);
}


bool
RemoteWireWriter::PrepareCompression(uint32 capability)
{
	BAutolock lock(fLock);
	if (!lock.IsLocked())
		return false;

	if (capability == 0 || (capability & SupportedCapabilities()) == 0)
		return false;

	// Already prepared for exactly this capability, or already compressing with
	// it: either way the answer to "can you honour it" is yes, and re-arming it
	// must then be allowed to succeed -- a client that sends RP_HELLO twice
	// still has to get an acknowledgement, and the second one has to travel as
	// segments because its decoder already switched. Note what this must NOT do:
	// build a second compressor. That would start a fresh zstd frame in the
	// middle of the stream the client is still decoding against the old window,
	// and desynchronise it permanently.
	if (fPreparedCapability == capability || fCapability == capability) {
		fPreparedCapability = capability;
		return true;
	}

	// Anything left over from a previous connection would be the wrong stream
	// state to compress into, and Reset() between connections should already
	// have cleared it. Being strict here rather than reusing is what keeps a
	// second client from inheriting the first one's compressor history.
	if (fCompressionContext != NULL || fOutputBuffer != NULL)
		return false;

#ifdef ZSTD_ENABLED
	ZSTD_CCtx* context = ZSTD_createCCtx();
	if (context == NULL) {
		TRACE_ERROR("failed to create compression context\n");
		return false;
	}

	size_t error = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel,
		kCompressionLevel);
	if (!ZSTD_isError(error)) {
		error = ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, kWindowLog);
	}
	if (!ZSTD_isError(error)) {
		// The transport is TCP; a second checksum over every flush would be
		// bytes and cycles spent re-proving what the stream already guarantees.
		error = ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 0);
	}

	if (ZSTD_isError(error)) {
		TRACE_ERROR("failed to configure compression context: %s\n",
			ZSTD_getErrorName(error));
		ZSTD_freeCCtx(context);
		return false;
	}

	uint8* outputBuffer = (uint8*)malloc(kOutputBufferSize);
	if (outputBuffer == NULL) {
		ZSTD_freeCCtx(context);
		return false;
	}

	fCompressionContext = context;
	fOutputBuffer = outputBuffer;
	fOutputBufferSize = kOutputBufferSize;
	fPreparedCapability = capability;
	return true;
#else
	return false;
#endif
}


status_t
RemoteWireWriter::WriteAndEnable(const void* buffer, size_t length,
	uint32 capability)
{
	BAutolock lock(fLock);
	if (!lock.IsLocked())
		return B_ERROR;

	// Refuse before the acknowledgement goes out, not after. A caller asking to
	// arm a capability it never prepared has already composed an RP_HELLO_ACK we
	// cannot honour, so the connection is not salvageable and the only honest
	// answer is an error -- writing the acknowledgement first and then declining
	// to compress is precisely the silent desynchronisation this split exists to
	// make impossible.
	if (capability != 0 && fPreparedCapability != capability) {
		TRACE_ERROR("asked to enable an unprepared capability 0x%" B_PRIx32
			"\n", capability);
		return B_NOT_ALLOWED;
	}

	status_t result = _WriteLocked(buffer, length);
	if (result != B_OK)
		return result;

	if (capability == 0)
		return B_OK;

	fCapability = capability;
	fPreparedCapability = 0;
	fLastReport = system_time();
	TRACE_ALWAYS("compressing the outbound stream (zstd level %d)\n",
		kCompressionLevel);

	return B_OK;
}


void
RemoteWireWriter::Reset()
{
	BAutolock lock(fLock);
	if (!lock.IsLocked())
		return;

	if (fMessages > 0 && fWireBytes > 0) {
		TRACE_ALWAYS("connection wire summary: msgs %" B_PRIu64 " exempt %"
			B_PRIu64 " plain %" B_PRIu64 " wire %" B_PRIu64 " encode %" B_PRId64
			"us\n", fMessages, fExemptMessages, fPlainBytes, fWireBytes,
			(int64)fEncodeTime);
	}

	// Emptied under fLock so a drawing thread cannot be left half way through
	// a message (or a segment) when the stream restarts.
	fTarget->MakeEmpty();

	_ResetCodec();

	fPlainBytes = fWireBytes = fMessages = fExemptMessages = 0;
	fEncodeTime = 0;
	fLastReport = 0;
}


void
RemoteWireWriter::GetStatistics(uint64& _plainBytes, uint64& _wireBytes,
	uint64& _messages, uint64& _exemptMessages, bigtime_t& _encodeTime) const
{
	BAutolock lock(fLock);
	_plainBytes = fPlainBytes;
	_wireBytes = fWireBytes;
	_messages = fMessages;
	_exemptMessages = fExemptMessages;
	_encodeTime = fEncodeTime;
}


status_t
RemoteWireWriter::_WriteLocked(const void* buffer, size_t length)
{
	fPlainBytes += length;
	fMessages++;

	if (fCapability == 0) {
		fWireBytes += length;
		return fTarget->Write(buffer, length);
	}

	// The code is the first field of every framed message.
	uint16 code = 0;
	if (length >= sizeof(uint16))
		memcpy(&code, buffer, sizeof(uint16));

	// A broken segment stream cannot be repaired by sending more segments, so
	// stop here rather than spending the compressor's CPU on output the peer
	// can no longer decode. Reported as an error so the round-trip callers
	// (RP_STRING_WIDTH, RP_READ_BITMAP) skip the reply wait instead of stalling
	// on an answer that is not coming.
	if (fStreamBroken)
		return B_IO_ERROR;

	status_t result;
	if (_IsPreCompressed(code)) {
		fExemptMessages++;
		result = _WriteRawSegment(buffer, length);
	} else
		result = _WriteCompressed(buffer, length);

	_MaybeReportStatistics();
	return result;
}


status_t
RemoteWireWriter::_WriteCompressed(const void* buffer, size_t length)
{
#ifdef ZSTD_ENABLED
	ZSTD_CCtx* context = (ZSTD_CCtx*)fCompressionContext;
	ZSTD_inBuffer input = { buffer, length, 0 };

	// Only the time actually spent inside the compressor is accumulated; the
	// ring-buffer writes in between can block on a slow client and would
	// otherwise be charged to the encoder.
	while (true) {
		ZSTD_outBuffer output = { fOutputBuffer + kSegmentHeaderReserve,
			fOutputBufferSize - kSegmentHeaderReserve, 0 };

		// ZSTD_e_flush at every message boundary: the client must be able to
		// decode a whole message as soon as its segments have arrived, never
		// after "some later message also went out". The window is kept, so the
		// next message still compresses against this one.
		bigtime_t start = system_time();
		size_t remaining = ZSTD_compressStream2(context, &output, &input,
			ZSTD_e_flush);
		fEncodeTime += system_time() - start;

		if (ZSTD_isError(remaining)) {
			TRACE_ERROR("compression failed: %s\n",
				ZSTD_getErrorName(remaining));
			return B_ERROR;
		}

		if (output.pos > 0) {
			status_t result = _WriteStagedSegment(output.pos);
			if (result != B_OK)
				return result;
		}

		if (input.pos == input.size && remaining == 0)
			break;

		// Neither input consumed nor output produced would spin forever.
		if (output.pos == 0 && input.pos == input.size && remaining != 0) {
			TRACE_ERROR("compressor made no progress\n");
			return B_ERROR;
		}
	}

	return B_OK;
#else
	// Without a codec there is nothing to compress into, so a capability can
	// never have been armed (SupportedCapabilities() is empty) and this is
	// unreachable. Pass the message through as a raw segment rather than
	// inventing a framing, so that even a mis-built server stays parsable.
	return _WriteRawSegment(buffer, length);
#endif
}


/*!	Frames and writes the compressor's staged output as one compressed segment.
	\a length bytes of payload are expected at fOutputBuffer +
	kSegmentHeaderReserve; the header is laid down in front of them so that the
	whole segment leaves in a single ring-buffer write.

	One write, not two, is what makes a segment all-or-nothing. Two writes leave
	a window in which the header is on the wire and the payload is not, and the
	peer then reads the next segment's header as this segment's payload --
	silent desynchronisation. The window is narrow but it is not theoretical:
	the send ring is constructed with discardWithoutReader, so
	StreamingRingBuffer::Write() returns B_OK having written *nothing* the
	moment its reader goes away, and a second write between the two halves of a
	segment can take that branch while the first one did not. Coalescing also
	halves the ring-buffer lock traffic on the hot path, since the compressed
	case is 1..N segments per message.
*/
status_t
RemoteWireWriter::_WriteStagedSegment(size_t length)
{
	uint8 header[REMOTE_SEGMENT_MAX_VARINT_SIZE];
	size_t headerSize = remote_segment_header_write(header, length, false);

	// Ends exactly where the payload begins.
	uint8* segment = fOutputBuffer + kSegmentHeaderReserve - headerSize;
	memcpy(segment, header, headerSize);

	size_t total = headerSize + length;
	size_t written = 0;
	status_t result = fTarget->Write(segment, total, B_INFINITE_TIMEOUT,
		written);
	if (result == B_OK && written == total) {
		fWireBytes += total;
		return B_OK;
	}

	return _BreakStream(written == 0 ? "dropped" : "torn",
		result != B_OK ? result : B_IO_ERROR);
}


/*!	Writes an already-compressed message through as a raw segment.

	Unlike the compressed case the payload is the caller's buffer, so there is
	nowhere in front of it to put the header and the segment costs two writes.
	That is the same exposure the legacy one-write-per-message path always had
	(a raw segment is one per message, not one per 64 KiB), but it is checked
	rather than assumed: a header that reached the ring without its payload
	behind it breaks the stream, and saying so is the difference between a
	dropped frame and a session that silently paints garbage.
*/
status_t
RemoteWireWriter::_WriteRawSegment(const void* payload, size_t length)
{
	uint8 header[REMOTE_SEGMENT_MAX_VARINT_SIZE];
	size_t headerSize = remote_segment_header_write(header, length, true);

	size_t headerWritten = 0;
	status_t result = fTarget->Write(header, headerSize, B_INFINITE_TIMEOUT,
		headerWritten);
	if (result != B_OK || headerWritten != headerSize) {
		// Nothing of the header landed: the segment simply never happened, so
		// the framing is still whole -- but the stream is no longer being
		// delivered either, which is what "dropped" latches.
		return _BreakStream(headerWritten == 0 ? "dropped" : "torn header",
			result != B_OK ? result : B_IO_ERROR);
	}

	size_t payloadWritten = 0;
	result = fTarget->Write(payload, length, B_INFINITE_TIMEOUT,
		payloadWritten);
	if (result == B_OK && payloadWritten == length) {
		fWireBytes += headerSize + length;
		return B_OK;
	}

	// The header is already queued, promising bytes that will not follow it.
	return _BreakStream("torn", result != B_OK ? result : B_IO_ERROR);
}


/*!	Records that the compressed stream can no longer be delivered in step with
	the peer's decoder and stops emitting segments until the next connection.
	Always returns an error, so that callers which wait for a reply skip the
	wait.
*/
status_t
RemoteWireWriter::_BreakStream(const char* what, status_t reason)
{
	if (!fStreamBroken) {
		fStreamBroken = true;
		TRACE_ALWAYS("segment %s (%s); no further segments this connection\n",
			what, strerror(reason));
	}

	return reason;
}


void
RemoteWireWriter::_ResetCodec()
{
#ifdef ZSTD_ENABLED
	if (fCompressionContext != NULL) {
		ZSTD_freeCCtx((ZSTD_CCtx*)fCompressionContext);
		fCompressionContext = NULL;
	}
#endif

	free(fOutputBuffer);
	fOutputBuffer = NULL;
	fOutputBufferSize = 0;
	fCapability = 0;
	fPreparedCapability = 0;

	// A broken stream is a property of the connection, not of the process: the
	// next one renegotiates from a fresh compressor and a fresh ring buffer.
	fStreamBroken = false;
}


/*!	Whether a message's payload is already compressed, in which case it is
	passed through as a raw segment instead of being fed to the entropy coder.

	JPEG, H.264/AV1 and Opus bytes are the output of an entropy coder already:
	zstd will find almost nothing in them, so compressing them spends real CPU
	in the display server's drawing path to save a fraction of a percent. Tier V
	payloads -- opcodes, draw state, coordinates, glyph runs, and raw BGRA
	bitmap bits -- are all highly compressible and none of them belong here.

	Note what this does *not* do: it never guesses from the payload. A decision
	made after compressing would be useless, because the compressor's window has
	already advanced by then and there is no way to take the bytes back without
	resetting the history that pays for the whole feature. So the exemption is
	keyed on the opcode, decided before a byte is compressed, and every future
	opcode that carries an opaque encoded byte string has to be listed here.
*/
/*static*/ bool
RemoteWireWriter::_IsPreCompressed(uint16 code)
{
	switch (code) {
		case RP_CODEC_TILE:
		case RP_AUDIO_PACKET:
			return true;

		default:
			return false;
	}
}


void
RemoteWireWriter::_MaybeReportStatistics()
{
	bigtime_t now = system_time();
	if (now - fLastReport < kStatisticsInterval)
		return;

	fLastReport = now;

	// Integer arithmetic only: this runs in the drawing path.
	uint64 ratio = fWireBytes > 0 ? fPlainBytes * 100 / fWireBytes : 0;
	uint64 encodePerMessage = fMessages > 0
		? (uint64)fEncodeTime * 1000 / fMessages : 0;
	TRACE_ALWAYS("zstd msgs %" B_PRIu64 " exempt %" B_PRIu64 " plain %" B_PRIu64
		" wire %" B_PRIu64 " ratio %" B_PRIu64 ".%02" B_PRIu64 "x encode %"
		B_PRIu64 "ns/msg\n", fMessages, fExemptMessages, fPlainBytes,
		fWireBytes, ratio / 100, ratio % 100, encodePerMessage);
}
