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
#include <stdlib.h>
#include <string.h>

#ifdef ZSTD_ENABLED
#	include <zstd.h>
#endif


#define TRACE(x...)				/*debug_printf("RemoteWireWriter: " x)*/
#define TRACE_ALWAYS(x...)		debug_printf("RemoteWireWriter: " x)
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
static const size_t kOutputBufferSize = 64 * 1024;

// At most one statistics line per this interval. The serial console on the
// target is write-bound, so the instrumentation has to cost bytes it can afford.
static const bigtime_t kStatisticsInterval = 5 * 1000 * 1000;


RemoteWireWriter::RemoteWireWriter(StreamingRingBuffer* target)
	:
	fTarget(target),
	fLock("remote wire writer"),
	fCapability(0),
	fPreparedCapability(0),
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

	status_t result;
	if (_IsPreCompressed(code)) {
		fExemptMessages++;
		result = _WriteSegment(buffer, length, true);
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
		ZSTD_outBuffer output = { fOutputBuffer, fOutputBufferSize, 0 };

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
			status_t result = _WriteSegment(fOutputBuffer, output.pos, false);
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
	return _WriteSegment(buffer, length, true);
#endif
}


status_t
RemoteWireWriter::_WriteSegment(const void* payload, size_t length, bool raw)
{
	uint8 header[REMOTE_SEGMENT_MAX_VARINT_SIZE];
	size_t headerSize = remote_segment_header_write(header, length, raw);

	status_t result = fTarget->Write(header, headerSize);
	if (result != B_OK)
		return result;

	result = fTarget->Write(payload, length);
	if (result != B_OK)
		return result;

	fWireBytes += headerSize + length;
	return B_OK;
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
