/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */

#include "RemoteWireReader.h"

#include "RemoteMessage.h"
#include "StreamingRingBuffer.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZSTD_ENABLED
#	include <zstd.h>
#endif


#ifdef CLIENT_COMPILE
#	define TRACE_ALWAYS(x...)	printf("RemoteWireReader: " x)
#else
#	define TRACE_ALWAYS(x...)	debug_printf("RemoteWireReader: " x)
#endif

#define TRACE(x...)				/*TRACE_ALWAYS(x)*/
#define TRACE_ERROR(x...)		TRACE_ALWAYS(x)


static const size_t kOutputBufferSize = 64 * 1024;

// Matches RemoteWireWriter's window; a client must be willing to allocate at
// least as large a window as the server compressed with.
static const int kWindowLogMax = 20;


RemoteWireReader::RemoteWireReader(StreamingRingBuffer* target)
	:
	fTarget(target),
	fCompressed(false),
	fHeaderUsed(0),
	fBodyLeft(0),
	fCapturingAck(false),
	fAckUsed(0),
	fSegmentHeaderUsed(0),
	fSegmentLeft(0),
	fSegmentRaw(false),
	fSegmentPending(false),
	fDecompressionContext(NULL),
	fOutputBuffer(NULL),
	fOutputBufferSize(0)
{
}


RemoteWireReader::~RemoteWireReader()
{
	_ResetCodec();
}


/*static*/ uint32
RemoteWireReader::SupportedCapabilities()
{
#ifdef ZSTD_ENABLED
	return RP_CAP_COMPRESS_ZSTD;
#else
	return 0;
#endif
}


status_t
RemoteWireReader::Process(const uint8* buffer, size_t length)
{
	while (length > 0) {
		status_t result = fCompressed
			? _ProcessSegmented(buffer, length)
			: _ProcessPlain(buffer, length);
		if (result != B_OK)
			return result;
	}

	return B_OK;
}


void
RemoteWireReader::Reset()
{
	_ResetCodec();

	fCompressed = false;
	fHeaderUsed = 0;
	fBodyLeft = 0;
	fCapturingAck = false;
	fAckUsed = 0;
	fSegmentHeaderUsed = 0;
	fSegmentLeft = 0;
	fSegmentRaw = false;
	fSegmentPending = false;
}


/*!	Forwards the still-plain stream unchanged while tracking message boundaries,
	so that RP_HELLO_ACK can be recognised and the switch made at the byte after
	it. Consumes from \a buffer / \a length and returns with the stream switched
	as soon as the acknowledgement says so.
*/
status_t
RemoteWireReader::_ProcessPlain(const uint8*& buffer, size_t& length)
{
	if (fHeaderUsed < sizeof(fHeader)) {
		size_t take = min_c(length, sizeof(fHeader) - fHeaderUsed);
		memcpy(fHeader + fHeaderUsed, buffer, take);
		fHeaderUsed += take;

		status_t result = fTarget->Write(buffer, take);
		if (result != B_OK)
			return result;

		buffer += take;
		length -= take;

		if (fHeaderUsed < sizeof(fHeader))
			return B_OK;

		uint16 code = (uint16)fHeader[0] | ((uint16)fHeader[1] << 8);
		uint32 messageLength = (uint32)fHeader[2] | ((uint32)fHeader[3] << 8)
			| ((uint32)fHeader[4] << 16) | ((uint32)fHeader[5] << 24);
		if (messageLength < sizeof(fHeader)) {
			TRACE_ERROR("message claims %" B_PRIu32 " bytes, less than the "
				"header\n", messageLength);
			return B_ERROR;
		}

		fBodyLeft = messageLength - sizeof(fHeader);
		fCapturingAck = code == RP_HELLO_ACK;
		fAckUsed = 0;

		if (fBodyLeft == 0) {
			fHeaderUsed = 0;
			fCapturingAck = false;
		}

		return B_OK;
	}

	size_t take = min_c(length, fBodyLeft);
	status_t result = fTarget->Write(buffer, take);
	if (result != B_OK)
		return result;

	if (fCapturingAck && fAckUsed < sizeof(fAck)) {
		size_t capture = min_c(take, sizeof(fAck) - fAckUsed);
		memcpy(fAck + fAckUsed, buffer, capture);
		fAckUsed += capture;
	}

	buffer += take;
	length -= take;
	fBodyLeft -= take;

	if (fBodyLeft > 0)
		return B_OK;

	fHeaderUsed = 0;

	if (!fCapturingAck)
		return B_OK;

	fCapturingAck = false;
	if (fAckUsed < sizeof(fAck)) {
		// A server that answers with a shorter acknowledgement than URP/1
		// defines cannot have negotiated anything; stay plain.
		return B_OK;
	}

	// RP_HELLO_ACK payload: uint32 negotiated version, uint32 negotiated
	// capabilities.
	uint32 capabilities = (uint32)fAck[4] | ((uint32)fAck[5] << 8)
		| ((uint32)fAck[6] << 16) | ((uint32)fAck[7] << 24);
	uint32 compression = capabilities & SupportedCapabilities();
	if (compression == 0)
		return B_OK;

	if (!_StartCodec(compression)) {
		// We advertised it, so the server is entitled to use it from here on
		// and the stream is no longer parsable. Fail rather than paint garbage.
		TRACE_ERROR("failed to start the decompressor\n");
		return B_NO_MEMORY;
	}

	fCompressed = true;
	TRACE_ALWAYS("inbound stream is compressed from here (capability %#"
		B_PRIx32 ")\n", compression);
	return B_OK;
}


status_t
RemoteWireReader::_ProcessSegmented(const uint8*& buffer, size_t& length)
{
	if (!fSegmentPending) {
		while (length > 0
			&& fSegmentHeaderUsed < REMOTE_SEGMENT_MAX_VARINT_SIZE) {
			fSegmentHeader[fSegmentHeaderUsed++] = *buffer++;
			length--;

			int consumed = remote_segment_header_read(fSegmentHeader,
				fSegmentHeaderUsed, fSegmentLeft, fSegmentRaw);
			if (consumed < 0) {
				TRACE_ERROR("malformed segment header\n");
				return B_ERROR;
			}

			if (consumed > 0) {
				fSegmentHeaderUsed = 0;
				fSegmentPending = true;
				break;
			}
		}

		if (!fSegmentPending)
			return B_OK;

		// A zero-length segment carries nothing; the writer does not emit them,
		// but accepting them costs nothing and keeps the framing total.
		if (fSegmentLeft == 0) {
			fSegmentPending = false;
			return B_OK;
		}
	}

	if (length == 0)
		return B_OK;

	size_t take = min_c(length, fSegmentLeft);
	status_t result = fSegmentRaw
		? fTarget->Write(buffer, take)
		: _Decompress(buffer, take);
	if (result != B_OK)
		return result;

	buffer += take;
	length -= take;
	fSegmentLeft -= take;
	if (fSegmentLeft == 0)
		fSegmentPending = false;

	return B_OK;
}


status_t
RemoteWireReader::_Decompress(const uint8* buffer, size_t length)
{
#ifdef ZSTD_ENABLED
	ZSTD_DCtx* context = (ZSTD_DCtx*)fDecompressionContext;
	ZSTD_inBuffer input = { buffer, length, 0 };

	while (input.pos < input.size) {
		ZSTD_outBuffer output = { fOutputBuffer, fOutputBufferSize, 0 };
		size_t consumedBefore = input.pos;

		size_t result = ZSTD_decompressStream(context, &output, &input);
		if (ZSTD_isError(result)) {
			TRACE_ERROR("decompression failed: %s\n",
				ZSTD_getErrorName(result));
			return B_ERROR;
		}

		if (output.pos > 0) {
			status_t writeResult = fTarget->Write(fOutputBuffer, output.pos);
			if (writeResult != B_OK)
				return writeResult;
		}

		if (input.pos == consumedBefore && output.pos == 0) {
			// The server never ends the frame, so this means the decoder can
			// make no progress at all: the stream is corrupt.
			TRACE_ERROR("decompressor made no progress\n");
			return B_ERROR;
		}
	}

	return B_OK;
#else
	(void)buffer;
	(void)length;
	return B_NOT_SUPPORTED;
#endif
}


bool
RemoteWireReader::_StartCodec(uint32 capability)
{
#ifdef ZSTD_ENABLED
	if ((capability & RP_CAP_COMPRESS_ZSTD) == 0)
		return false;

	ZSTD_DCtx* context = ZSTD_createDCtx();
	if (context == NULL)
		return false;

	size_t error = ZSTD_DCtx_setParameter(context, ZSTD_d_windowLogMax,
		kWindowLogMax);
	if (ZSTD_isError(error)) {
		ZSTD_freeDCtx(context);
		return false;
	}

	fOutputBuffer = (uint8*)malloc(kOutputBufferSize);
	if (fOutputBuffer == NULL) {
		ZSTD_freeDCtx(context);
		return false;
	}

	fDecompressionContext = context;
	fOutputBufferSize = kOutputBufferSize;
	return true;
#else
	(void)capability;
	return false;
#endif
}


void
RemoteWireReader::_ResetCodec()
{
#ifdef ZSTD_ENABLED
	if (fDecompressionContext != NULL) {
		ZSTD_freeDCtx((ZSTD_DCtx*)fDecompressionContext);
		fDecompressionContext = NULL;
	}
#endif

	free(fOutputBuffer);
	fOutputBuffer = NULL;
	fOutputBufferSize = 0;
}
