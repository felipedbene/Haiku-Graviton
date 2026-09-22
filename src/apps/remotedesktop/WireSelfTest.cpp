/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */

/*!	Conformance test for the URP/1 wire layer, run as `RemoteDesktop
	--wire-selftest`.

	Why this exists, and why here. The wire layer has one branch that no
	workload can reach: RemoteWireWriter::_IsPreCompressed() exempts
	RP_CODEC_TILE and RP_AUDIO_PACKET, and nothing in this tree emits either --
	they are Tier P opcodes, reserved ahead of their implementation precisely so
	that the exemption rule would not have to be invented later. So every
	session reports `exempt 0`, not because the raw-segment path is untested but
	because *no session can test it*: bitmaps travel as RP_DRAW_BITMAP raw
	pixels, which are highly compressible and deliberately not exempt. A branch
	whose first ever execution is the day Tier P lands is exactly the "rule that
	silently fails to apply" RemoteMessage.h warns about.

	This drives the real RemoteWireWriter and RemoteWireReader -- the shipped
	implementation, on whatever architecture the binary was built for -- through
	a stream that genuinely mixes compressed and raw segments, and checks the
	bytes both ways. It is headless and takes no arguments, so it runs over SSM
	on a Graviton instance as readily as under an emulator.

	It also covers the two encoder error paths added for issue #437, which are
	reachable only through a race a live session cannot be asked to perform on
	cue: a segment torn by the ring buffer's reader disappearing mid-write, and
	a segment dropped wholesale because no reader is left.
*/

#include "RemoteMessage.h"
#include "RemoteWireFormat.h"
#include "RemoteWireReader.h"
#include "RemoteWireWriter.h"
#include "StreamingRingBuffer.h"

#include <DataIO.h>
#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Comfortably larger than anything this test produces, so a single
// onlyBlockOnNoData read empties a buffer without ever parking on it.
static const size_t kDrainCapacity = 4 * 1024 * 1024;

static int sChecks = 0;
static int sFailures = 0;


static void
check(const char* label, bool condition, const char* detail = NULL)
{
	sChecks++;
	if (condition) {
		printf("  ok    %s\n", label);
		return;
	}

	sFailures++;
	printf("  FAIL  %s%s%s\n", label, detail != NULL ? "  " : "",
		detail != NULL ? detail : "");
}


/*!	Frames one RP message into \a out the way RemoteMessage does -- uint16 code,
	uint32 total length with the header counted, then the payload -- and returns
	the framed size.
*/
static size_t
frame(uint8* out, uint16 code, const void* payload, size_t length)
{
	static const size_t kHeaderSize = sizeof(uint16) + sizeof(uint32);
	uint32 total = (uint32)(kHeaderSize + length);
	memcpy(out, &code, sizeof(code));
	memcpy(out + sizeof(code), &total, sizeof(total));
	if (length > 0)
		memcpy(out + kHeaderSize, payload, length);

	return total;
}


//! Deterministic bytes zstd cannot do anything with.
static void
fill_incompressible(uint8* out, size_t length, uint32 seed)
{
	uint32 state = seed;
	for (size_t i = 0; i < length; i++) {
		state = state * 1664525u + 1013904223u;
		out[i] = (uint8)(state >> 24);
	}
}


/*!	Walks the segment framing of \a wire from \a plainPrefix on, counting raw and
	compressed segments and collecting the raw payloads, so the writer's framing
	decisions are asserted directly rather than inferred from what the reader
	managed to decode.
*/
static bool
survey_segments(const uint8* wire, size_t length, size_t plainPrefix,
	int32& _compressed, int32& _raw, BMallocIO& rawPayloads)
{
	_compressed = 0;
	_raw = 0;

	size_t offset = plainPrefix;
	while (offset < length) {
		size_t payloadLength = 0;
		bool raw = false;
		int consumed = remote_segment_header_read(wire + offset,
			length - offset, payloadLength, raw);
		if (consumed <= 0)
			return false;

		offset += consumed;
		if (offset + payloadLength > length)
			return false;

		if (raw) {
			_raw++;
			rawPayloads.Write(wire + offset, payloadLength);
		} else
			_compressed++;

		offset += payloadLength;
	}

	return offset == length;
}


//! Moves everything currently buffered into \a out. Never parks: see above.
static bool
drain(StreamingRingBuffer& buffer, uint8* scratch, BMallocIO& out)
{
	int32 read = buffer.Read(scratch, kDrainCapacity, true);
	if (read <= 0 || (size_t)read >= kDrainCapacity)
		return false;

	out.Write(scratch, read);
	return true;
}


// ---------------------------------------------------------------------------
// The mixed compressed/raw round trip
// ---------------------------------------------------------------------------

static const size_t kFillBodySize = 512;
static const int32 kFillsBefore = 400;
static const int32 kFillsAfter = 20;
static const size_t kTileSize = 4096;
static const size_t kLargeSize = 192 * 1024;


static void
test_mixed_round_trip(uint8* scratch, uint8* message)
{
	printf("  -- mixed compressed/raw round trip --\n");

	uint32 capability = RemoteWireWriter::SupportedCapabilities()
		& RemoteWireReader::SupportedCapabilities();
	check("this build offers a compression capability", capability != 0,
		"(no zstd build feature -- nothing below this line is covered)");
	if (capability == 0)
		return;

	// Big enough that the writer never has to park: this phase has no
	// concurrent drain, and a blocking ring buffer with nobody reading would
	// deadlock rather than discard.
	StreamingRingBuffer wire(2 * 1024 * 1024);
	StreamingRingBuffer plain(2 * 1024 * 1024);
	if (wire.InitCheck() != B_OK || plain.InitCheck() != B_OK) {
		check("ring buffers allocated", false);
		return;
	}

	RemoteWireWriter writer(&wire);
	RemoteWireReader reader(&plain);

	// What the peer has to end up with, byte for byte.
	BMallocIO expected;
	size_t length;

	// 1. Plain phase. Every session opens like this, before any handshake, and
	//    these bytes have to cross unchanged.
	length = frame(message, RP_INIT_CONNECTION, NULL, 0);
	check("plain message written", writer.Write(message, length) == B_OK);
	expected.Write(message, length);
	size_t plainPrefix = length;

	// 2. The acknowledgement that switches the stream, written and armed in one
	//    operation: its own bytes are still plain, the byte after it is already
	//    a segment, and the reader has to find that boundary for itself.
	check("codec prepared", writer.PrepareCompression(capability));

	uint32 ack[2] = { RP_PROTOCOL_VERSION, capability };
	length = frame(message, RP_HELLO_ACK, ack, sizeof(ack));
	check("acknowledgement written and compression armed",
		writer.WriteAndEnable(message, length, capability) == B_OK);
	check("the writer reports compressing", writer.IsCompressing());
	expected.Write(message, length);
	plainPrefix += length;

	// 3. Ordinary, repetitive drawing traffic -- identical fills, which is what
	//    the cross-message window is for and where the ratio comes from.
	uint8 body[kFillBodySize];
	memset(body, 0x5a, sizeof(body));

	bool allWritten = true;
	for (int32 i = 0; i < kFillsBefore; i++) {
		length = frame(message, RP_FILL_RECT, body, sizeof(body));
		if (writer.Write(message, length) != B_OK)
			allWritten = false;

		expected.Write(message, length);
	}
	check("compressed messages written", allWritten);

	// 4. The exempt message: the raw/passthrough segment. Incompressible by
	//    construction, which is the whole reason the exemption exists.
	uint8 tile[kTileSize];
	fill_incompressible(tile, sizeof(tile), 0x54494c45);
	length = frame(message, RP_CODEC_TILE, tile, sizeof(tile));

	uint8 exempt[kTileSize + 64];
	size_t exemptLength = length;
	memcpy(exempt, message, length);

	check("exempt message written", writer.Write(message, length) == B_OK);
	expected.Write(message, length);

	// 5. More ordinary traffic *after* the raw segment. This is the claim in
	//    RemoteWireFormat.h that a raw segment can be interleaved at a message
	//    boundary without disturbing the zstd stream around it: had the
	//    passthrough been fed to the compressor, or had the flush not landed on
	//    the boundary, these would not decode.
	allWritten = true;
	for (int32 i = 0; i < kFillsAfter; i++) {
		length = frame(message, RP_FILL_RECT, body, sizeof(body));
		if (writer.Write(message, length) != B_OK)
			allWritten = false;

		expected.Write(message, length);
	}
	check("the compressed stream survives an interleaved raw segment",
		allWritten);

	// 6. One incompressible message far larger than the encoder's staging
	//    buffer, so a single message spans several compressed segments -- the
	//    case #416 widened from one write per message to many.
	fill_incompressible(scratch, kLargeSize, 0x42495453);
	length = frame(message, RP_DRAW_BITMAP, scratch, kLargeSize);
	check("multi-segment message written", writer.Write(message, length)
		== B_OK);
	expected.Write(message, length);

	// ---- what actually went on the wire ----
	BMallocIO wireBytes;
	if (!drain(wire, scratch, wireBytes)) {
		check("the wire buffer drained", false);
		return;
	}

	int32 compressedSegments = 0;
	int32 rawSegments = 0;
	BMallocIO rawPayloads;
	check("the segment stream is exactly framed",
		survey_segments((const uint8*)wireBytes.Buffer(),
			wireBytes.BufferLength(), plainPrefix, compressedSegments,
			rawSegments, rawPayloads));

	char detail[160];
	snprintf(detail, sizeof(detail), "(raw=%" B_PRId32 " compressed=%" B_PRId32
		")", rawSegments, compressedSegments);
	check("exactly one raw segment, among many compressed ones",
		rawSegments == 1 && compressedSegments > 1, detail);

	// One flush per compressed message, so anything beyond the message count is
	// a message that had to be split.
	const int32 compressedMessages = kFillsBefore + kFillsAfter + 1;
	check("a single message spans several compressed segments",
		compressedSegments > compressedMessages, detail);

	// The exemption is a *passthrough*: the raw payload has to be the message
	// itself, untouched, not a re-encoding of it.
	check("the raw segment carries the exempt message verbatim",
		rawPayloads.BufferLength() == exemptLength
			&& memcmp(rawPayloads.Buffer(), exempt, exemptLength) == 0);

	uint64 plainBytes, wireCount, messages, exemptMessages;
	bigtime_t encodeTime;
	writer.GetStatistics(plainBytes, wireCount, messages, exemptMessages,
		encodeTime);
	check("the writer counted exactly one exempt message", exemptMessages == 1);
	snprintf(detail, sizeof(detail), "(counted %" B_PRIu64 ", produced %zu)",
		wireCount, wireBytes.BufferLength());
	check("the writer's wire count matches the bytes it produced",
		wireCount == wireBytes.BufferLength(), detail);
	snprintf(detail, sizeof(detail), "(plain=%" B_PRIu64 " wire=%" B_PRIu64
		" msgs=%" B_PRIu64 " encode=%" B_PRIdBIGTIME "us)", plainBytes,
		wireCount, messages, encodeTime);
	check("the wire is smaller than the plain stream", wireCount < plainBytes,
		detail);

	// ---- and back again, through the real reader ----
	// Fed in small, uneven chunks so segment headers, varint continuations and
	// segment bodies are each split across calls.
	static const size_t kChunks[] = { 1, 3, 5, 2, 7, 4, 1, 4093, 11 };
	static const size_t kChunkCount = sizeof(kChunks) / sizeof(kChunks[0]);

	const uint8* cursor = (const uint8*)wireBytes.Buffer();
	size_t left = wireBytes.BufferLength();
	size_t chunkIndex = 0;
	status_t decoded = B_OK;
	while (left > 0 && decoded == B_OK) {
		size_t take = kChunks[chunkIndex++ % kChunkCount];
		if (take > left)
			take = left;

		decoded = reader.Process(cursor, take);
		cursor += take;
		left -= take;
	}

	check("the reader accepted the whole stream", decoded == B_OK,
		strerror(decoded));

	BMallocIO recovered;
	if (!drain(plain, scratch, recovered)) {
		check("the plain buffer drained", false);
		return;
	}

	snprintf(detail, sizeof(detail), "(%zu vs %zu bytes)",
		recovered.BufferLength(), expected.BufferLength());
	check("the plain message stream is reproduced byte for byte",
		recovered.BufferLength() == expected.BufferLength()
			&& memcmp(recovered.Buffer(), expected.Buffer(),
				expected.BufferLength()) == 0, detail);
}


// ---------------------------------------------------------------------------
// The two #437 encoder error paths
// ---------------------------------------------------------------------------

struct TearContext {
	RemoteWireWriter*	writer;
	const uint8*		message;
	size_t				length;
	status_t			result;
};


static int32
tear_writer(void* data)
{
	TearContext* context = (TearContext*)data;
	context->result = context->writer->Write(context->message,
		context->length);
	return 0;
}


/*!	Arms a writer on \a buffer and gets the stream into its compressed state,
	leaving the buffer empty. Returns false if that could not be done.
*/
static bool
arm_compression(RemoteWireWriter& writer, StreamingRingBuffer& buffer,
	uint32 capability)
{
	if (!writer.PrepareCompression(capability))
		return false;

	uint8 message[64];
	uint32 ack[2] = { RP_PROTOCOL_VERSION, capability };
	size_t length = frame(message, RP_HELLO_ACK, ack, sizeof(ack));
	if (writer.WriteAndEnable(message, length, capability) != B_OK)
		return false;

	uint8 sink[64];
	return buffer.Read(sink, length) == (int32)length;
}


/*!	A segment torn by the reader disappearing mid-write.

	The ring buffer is smaller than the segment, so the write cannot finish in
	one pass and has to park for space. This thread then frees some of that
	space -- committing a *prefix* of the segment, which in a live session is
	already on the socket -- and only then unregisters the reader. Write() takes
	its discardWithoutReader branch, drops the rest, and (before #437) returned
	B_OK: a header on the wire with no payload behind it, reported as success.
*/
static void
test_torn_segment()
{
	printf("  -- a segment torn by the reader going away --\n");

	uint32 capability = RemoteWireWriter::SupportedCapabilities();
	if (capability == 0) {
		printf("  skip  (no compression capability in this build)\n");
		return;
	}

	// Deliberately tiny, and discarding: that is the send buffer's own
	// configuration (RemoteHWInterface passes discardWithoutReader), scaled
	// down so the tear is deterministic instead of a race.
	StreamingRingBuffer buffer(1024, true);
	if (buffer.InitCheck() != B_OK) {
		check("ring buffer allocated", false);
		return;
	}

	int reader = 0;
	buffer.SetReader(&reader);

	RemoteWireWriter writer(&buffer);
	check("compression armed", arm_compression(writer, buffer, capability));

	// An exempt message, so this exercises the two-write raw path -- the one
	// that still has a window between a header and its payload. 4 KiB against a
	// 1 KiB ring guarantees the writer parks.
	uint8 tile[kTileSize];
	uint8 message[kTileSize + 64];
	fill_incompressible(tile, sizeof(tile), 0x544f524e);
	size_t length = frame(message, RP_CODEC_TILE, tile, sizeof(tile));

	TearContext context = { &writer, message, length, B_OK };
	thread_id thread = spawn_thread(tear_writer, "wire tear writer",
		B_NORMAL_PRIORITY, &context);
	if (thread < 0) {
		check("writer thread spawned", false);
		return;
	}

	resume_thread(thread);

	// Commit a prefix: each Read() blocks until the writer has produced that
	// much and then frees the space for it to produce more. 1536 bytes handed
	// back against a 1 KiB ring caps what can have been written at 2560 of
	// 4102, so the segment cannot already be complete when the reader goes away.
	uint8 sink[1024];
	size_t committed = 0;
	int32 read = buffer.Read(sink, sizeof(sink));
	if (read > 0)
		committed += read;
	read = buffer.Read(sink, 512);
	if (read > 0)
		committed += read;

	buffer.ClearReader(&reader);

	status_t threadResult;
	wait_for_thread(thread, &threadResult);

	char detail[160];
	snprintf(detail, sizeof(detail), "(committed %zu of %zu bytes, got %s)",
		committed, length, strerror(context.result));
	check("a torn segment is reported, not passed off as success",
		context.result != B_OK, detail);

	// And the stream stays refused: a later segment cannot repair a window the
	// peer never received, so continuing to emit them is worse than stopping.
	uint8 body[32];
	memset(body, 0x5a, sizeof(body));
	length = frame(message, RP_FILL_RECT, body, sizeof(body));
	check("the broken stream stays refused",
		writer.Write(message, length) != B_OK);

	// Until the connection boundary, which is the only place a fresh compressor
	// can be put in front of a fresh decoder.
	writer.Reset();
	buffer.SetReader(&reader);
	check("Reset() clears the break", writer.PrepareCompression(capability));
	check("and the stream is plain again", !writer.IsCompressing());
	check("writes work again after the reset",
		writer.Write(message, length) == B_OK);
}


/*!	A segment dropped whole because no reader is left.

	Nothing is torn -- the framing already on the wire stays consistent -- but
	the compressor's window has advanced past bytes that never left, so every
	later segment refers to history the peer does not have. Reported as an error
	so the round-trip callers (RP_STRING_WIDTH, RP_READ_BITMAP) skip a reply
	wait that cannot be answered.
*/
static void
test_dropped_segment()
{
	printf("  -- a segment dropped with no reader --\n");

	uint32 capability = RemoteWireWriter::SupportedCapabilities();
	if (capability == 0) {
		printf("  skip  (no compression capability in this build)\n");
		return;
	}

	StreamingRingBuffer buffer(64 * 1024, true);
	if (buffer.InitCheck() != B_OK) {
		check("ring buffer allocated", false);
		return;
	}

	int reader = 0;
	buffer.SetReader(&reader);

	RemoteWireWriter writer(&buffer);
	check("compression armed", arm_compression(writer, buffer, capability));

	// The drain goes away without the connection being reset: a sender thread
	// that died on a send error, which is how this happens in the field.
	buffer.ClearReader(&reader);

	uint8 message[128];
	uint8 body[32];
	memset(body, 0x5a, sizeof(body));
	size_t length = frame(message, RP_FILL_RECT, body, sizeof(body));

	check("a dropped segment is reported, not passed off as success",
		writer.Write(message, length) != B_OK);
	check("and the stream stays refused",
		writer.Write(message, length) != B_OK);
}


/*!	The primitive the client-side decode-error fix relies on: emptying the
	buffer releases a parser waiting for the rest of a message that will never
	arrive, rather than leaving it blocked on bytes nobody will write.
*/
static void
test_parser_release()
{
	printf("  -- a stranded parser is released, not left waiting --\n");

	StreamingRingBuffer buffer(4096);
	if (buffer.InitCheck() != B_OK) {
		check("ring buffer allocated", false);
		return;
	}

	// The front of a message, and nothing behind it: exactly what a decode
	// failure mid-message leaves queued.
	uint8 message[128];
	uint8 body[64];
	memset(body, 0x11, sizeof(body));
	frame(message, RP_FILL_RECT, body, sizeof(body));
	check("the truncated prefix is queued",
		buffer.Write(message, 8) == B_OK);

	RemoteMessage parser(&buffer, (StreamingRingBuffer*)NULL);
	uint16 code = 0;
	check("the parser accepts the header and waits for the body",
		parser.NextMessage(code) == B_OK && code == RP_FILL_RECT
			&& parser.DataLeft() == sizeof(body));

	// MakeEmpty() must both discard the tail and cancel the read the parser is
	// about to make -- including when the parser has not parked yet, which is
	// the common case (it has just finished draining the burst that preceded
	// the failure). Without the cancel being armed for a reader that is not
	// there yet, this read never returns.
	buffer.MakeEmpty();

	uint8 sink[64];
	int32 read = buffer.Read(sink, sizeof(sink));
	check("the stranded read is cancelled rather than blocking",
		read == B_CANCELED, strerror(read));
}


// ---------------------------------------------------------------------------

int
remote_wire_selftest()
{
	// Line-buffered even when stdout is a file or a pipe. Half of what this
	// checks is that a thread does *not* block forever, and the failure mode of
	// such a check is producing no output at all -- so a run that has to be
	// killed must still show how far it got. Block buffering loses exactly that.
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("URP/1 wire self-test\n");
	printf("  writer capabilities %#" B_PRIx32 ", reader capabilities %#"
		B_PRIx32 "\n", RemoteWireWriter::SupportedCapabilities(),
		RemoteWireReader::SupportedCapabilities());

	uint8* scratch = (uint8*)malloc(kDrainCapacity);
	uint8* message = (uint8*)malloc(kLargeSize + 64);
	if (scratch == NULL || message == NULL) {
		printf("WIRE_SELFTEST=FAIL  (no memory)\n");
		free(scratch);
		free(message);
		return 1;
	}

	test_mixed_round_trip(scratch, message);
	test_torn_segment();
	test_dropped_segment();
	test_parser_release();

	free(scratch);
	free(message);

	printf("\n");
	if (sFailures > 0) {
		printf("WIRE_SELFTEST=FAIL  CHECKS=%d FAILURES=%d\n", sChecks,
			sFailures);
		return 1;
	}

	// The check count is part of the result on purpose: a build without the
	// zstd feature runs far fewer checks, and must not be mistaken for a full
	// pass just because nothing failed.
	printf("WIRE_SELFTEST=PASS  CHECKS=%d\n", sChecks);
	return 0;
}
