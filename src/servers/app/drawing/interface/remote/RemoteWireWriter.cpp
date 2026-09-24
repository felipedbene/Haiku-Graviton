/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */

#include "RemoteWireWriter.h"

#include "RemoteMessage.h"
#include "RemoteWireFormat.h"
#include "StreamingRingBuffer.h"

#include <Autolock.h>
#include <OS.h>

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

/*!	The drain window: the longest a message may sit in the compressor before the
	stream is flushed and it can be decoded.

	Why there is a window at all. A flush ends a zstd block, so it pays that
	block's entropy tables -- and the census says the mean message on this wire
	is 25.6 to 36.9 bytes, which is far less than a block header is worth.
	Priced on a captured 30 s idle stream with the shipped parameters: one flush
	per message 1.95x, one per 4 ms 5.16x, one per 16 ms 5.28x, and a single
	flush over the whole capture 6.02x. The ratio is nearly all recovered by
	4 ms.

	Why 4 ms and not 16. A window is a deliberate delay, and this server's
	interactive budget is measured in fractions of a millisecond: keystroke to
	first draw op is 0.58 ms p50, a menu highlight 0.42 ms. 16 ms buys 2 % more
	ratio (5.28x against 5.16x) for 4x the worst-case delay -- and it is also
	most of a 60 Hz frame, which is the scale at which an added delay stops
	being invisible. 4 ms is the smaller risk for all but the last 2 %.

	The window is a *bound*, not a wait: kQuiescenceInterval below closes it as
	soon as the drawing threads stop producing, which on an interactive stream is
	almost immediately, and _MustFlushNow() skips it entirely for the messages
	that cannot afford even that.
*/
static const bigtime_t kFlushWindow = 4000;

/*!	How often the flusher looks at an open window to see whether the drawing
	threads have gone quiet.

	This is what keeps the window from costing its full length on an interactive
	stream. Drawing ops arrive in tight bursts -- a burst of a dozen and then
	silence -- so almost every window is closed by the burst ending rather than
	by kFlushWindow expiring, and the delay a keystroke actually pays is this
	interval, not the window. It runs only while a window is open (at most
	kFlushWindow / this many wakeups per window, and none at all on an idle
	stream), so it is bounded polling inside an event, not a polling loop.
*/
static const bigtime_t kQuiescenceInterval = 500;

/*!	Environment override for the window, in microseconds, read once per writer.

	It exists for the A/B that justified the window: with
	REMOTE_WIRE_FLUSH_WINDOW=0 this build flushes once per message -- the exact
	pre-#543 policy -- so both arms of the measurement are the same binary on the
	same boot and differ in nothing but this. A measurement whose two arms are
	two builds cannot tell the policy apart from the build.
*/
static const char* const kFlushWindowEnvironmentVariable
	= "REMOTE_WIRE_FLUSH_WINDOW";


static bigtime_t
configured_flush_window()
{
	const char* value = getenv(kFlushWindowEnvironmentVariable);
	if (value == NULL || value[0] == '\0')
		return kFlushWindow;

	char* end = NULL;
	long long window = strtoll(value, &end, 10);
	if (end == value || window < 0)
		return kFlushWindow;

	return (bigtime_t)window;
}


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
	fFlushWindow(configured_flush_window()),
	fWindowOpened(0),
	fWindowMessages(0),
	fFlusher(-1),
	fFlushSignal(-1),
	fFlusherQuitting(false),
	fQueue(),
	fPlainBytes(0),
	fWireBytes(0),
	fMessages(0),
	fExemptMessages(0),
	fFlushes(0),
	fEncodeTime(0),
	fLastReport(0)
{
}


RemoteWireWriter::~RemoteWireWriter()
{
	// Before the lock, and before the codec the thread compresses into: see
	// _StopFlusher().
	_StopFlusher();
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
	TRACE_ALWAYS("compressing the outbound stream (zstd level %d, flush window %"
		B_PRId64 "us)\n", kCompressionLevel, (int64)fFlushWindow);

	// Only now: with no capability armed there is no window to close, so the
	// thread would have nothing to do, and a plain stream (every client
	// connection starts as one, and stays one if it negotiates nothing) must not
	// pay for a thread it never uses.
	_StartFlusher();

	return B_OK;
}


status_t
RemoteWireWriter::Flush()
{
	BAutolock lock(fLock);
	if (!lock.IsLocked())
		return B_ERROR;

	return _FlushLocked();
}


void
RemoteWireWriter::Reset()
{
	// Outside the lock, because the flusher thread takes it: joining the thread
	// while holding the lock it is waiting for is a deadlock. Stopping it first
	// also means the codec below is freed with nobody left to compress into it.
	_StopFlusher();

	BAutolock lock(fLock);
	if (!lock.IsLocked())
		return;

	if (fMessages > 0 && fWireBytes > 0) {
		uint64 enqueued, drained, coalesced, superseded, collapses;
		fQueue.GetStatistics(enqueued, drained, coalesced, superseded,
			collapses);
		TRACE_ALWAYS("connection wire summary: msgs %" B_PRIu64 " exempt %"
			B_PRIu64 " flushes %" B_PRIu64 " plain %" B_PRIu64 " wire %" B_PRIu64
			" encode %" B_PRId64 "us queued %" B_PRIu64 " drained %" B_PRIu64
			" coalesced %" B_PRIu64 " superseded %" B_PRIu64 " collapses %"
			B_PRIu64 "\n", fMessages, fExemptMessages, fFlushes, fPlainBytes,
			fWireBytes, (int64)fEncodeTime, enqueued, drained, coalesced,
			superseded, collapses);
	}

	// Emptied under fLock so a drawing thread cannot be left half way through
	// a message (or a segment) when the stream restarts. The flow-control queue
	// goes with it: its contents were framed for a client that is gone, and the
	// next connection replays state and repaints unconditionally -- which is
	// also why the owed resync it may be holding is dropped rather than carried
	// across.
	fTarget->MakeEmpty();
	fQueue.Reset();

	_ResetCodec();

	fPlainBytes = fWireBytes = fMessages = fExemptMessages = fFlushes = 0;
	fEncodeTime = 0;
	fLastReport = 0;
}


void
RemoteWireWriter::GetStatistics(uint64& _plainBytes, uint64& _wireBytes,
	uint64& _messages, uint64& _exemptMessages, uint64& _flushes,
	bigtime_t& _encodeTime) const
{
	BAutolock lock(fLock);
	_plainBytes = fPlainBytes;
	_wireBytes = fWireBytes;
	_messages = fMessages;
	_exemptMessages = fExemptMessages;
	_flushes = fFlushes;
	_encodeTime = fEncodeTime;
}


void
RemoteWireWriter::SetFrameBoundariesExplicit(bool explicitly)
{
	BAutolock lock(fLock);
	if (!lock.IsLocked())
		return;

	fQueue.SetExplicitBoundaries(explicitly);
}


bool
RemoteWireWriter::TakeResyncOwed()
{
	BAutolock lock(fLock);
	if (!lock.IsLocked())
		return false;

	if (!fQueue.ResyncOwed())
		return false;

	fQueue.ClearResyncOwed();
	return true;
}


void
RemoteWireWriter::GetFlowStatistics(uint64& _enqueued, uint64& _drained,
	uint64& _coalesced, uint64& _superseded, uint64& _collapses) const
{
	BAutolock lock(fLock);
	fQueue.GetStatistics(_enqueued, _drained, _coalesced, _superseded,
		_collapses);
}


void
RemoteWireWriter::GetFlowDepth(size_t& _messages, size_t& _bytes) const
{
	BAutolock lock(fLock);
	_messages = fQueue.CountMessages();
	_bytes = fQueue.CountBytes();
}


/*!	One message in, on its way to the socket, under fLock.

	The order here is the flow-control contract:

	  1. the queue sees every message, whether or not it stores it -- it has to
	     track the per-token drawing mode to know which pixel ops read the
	     surface, and a mode setter that went straight through would otherwise be
	     invisible to it;
	  2. anything already queued is drained first, because the stream's order is
	     the order the drawing engines produced it and nothing may overtake;
	  3. a message is only handed to the compressor when the ring can take the
	     whole of it. Otherwise it goes into the queue.

	Step 3 is what replaces "discard when nobody listens". The send ring is
	constructed with discardWithoutReader, so before this a Write() with no
	client attached returned B_OK having written nothing: the message was gone,
	the caller believed it sent, and the client that connected later was never
	told. Now there is nowhere for a message to vanish that is not a counted,
	bounded, resync-forcing policy decision.
*/
status_t
RemoteWireWriter::_WriteLocked(const void* buffer, size_t length)
{
	fPlainBytes += length;
	fMessages++;

	fQueue.Observe(buffer, length);

	if (!fQueue.IsEmpty()) {
		_DrainQueue();
		if (!fQueue.IsEmpty())
			return fQueue.Enqueue(buffer, length, find_thread(NULL));
	}

	if (!_CanDeliver(length) && !_LargerThanTheRing(length))
		return fQueue.Enqueue(buffer, length, find_thread(NULL));

	return _Deliver(buffer, length);
}


/*!	Whether the ring can take \a length bytes of message right now, whole.

	A reader has to exist (without one the ring discards and the message would
	be lost), and there has to be room for the message plus one staging buffer:
	the compressed form of a message is at worst marginally larger than the
	plain form and is emitted in segments of at most kOutputBufferSize, so that
	headroom bounds the expansion for any message the ring could hold at all.
*/
bool
RemoteWireWriter::_CanDeliver(size_t length) const
{
	if (!fTarget->HasReader())
		return false;

	return fTarget->FreeSpace() >= length + kOutputBufferSize;
}


/*!	A message the ring could never hold whole, even empty.

	Those cannot be gated on free space or they would sit in the queue forever,
	so they take the old path: a blocking write that the sender drains
	underneath. That is exactly the behaviour every message had before this
	queue existed, and it is only reachable for a message approaching a megabyte
	-- which, since RP_DRAW_BITMAP is cropped to the rect actually drawn, no
	longer happens on the paths that used to produce it.
*/
bool
RemoteWireWriter::_LargerThanTheRing(size_t length) const
{
	if (!fTarget->HasReader())
		return false;

	return length + kOutputBufferSize > fTarget->BufferSize();
}


void
RemoteWireWriter::_DrainQueue()
{
	while (true) {
		size_t length = 0;
		const uint8* data = fQueue.PeekFront(length);
		if (data == NULL)
			return;

		if (!_CanDeliver(length) && !_LargerThanTheRing(length))
			return;

		// Only pop once the whole message has reached the ring. A failure
		// leaves it queued rather than half-gone; the stream is broken by then
		// and Reset() at the next connection is what clears it.
		if (_Deliver(data, length) != B_OK)
			return;

		fQueue.PopFront();
	}
}


status_t
RemoteWireWriter::_Deliver(const void* buffer, size_t length)
{
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

		// Order, not framing, is what forces this flush. A raw segment carries
		// its message past the compressor, so if an open window still held
		// earlier messages the peer would decode this one *before* them. The
		// interleaving RemoteWireFormat.h describes is only safe at a boundary
		// where nothing is outstanding, so close the window to make one.
		result = _FlushLocked();
		if (result == B_OK)
			result = _WriteRawSegment(buffer, length);
	} else
		result = _WriteCompressed(buffer, length, _MustFlushNow(code));

	_MaybeReportStatistics();
	return result;
}


/*!	Compresses one whole message, and closes the drain window if \a flushNow or
	if this message is the one that runs the window out.

	The window only ever opens and closes on a message boundary: this is called
	with one complete framed message, and the flush is asked for in the same call
	that feeds it (so it lands after that message's last byte) or not at all. A
	flush therefore never splits a message -- what changes against the pre-#543
	encoder is that several whole messages can share one flush, not that a
	decoder can be handed half of one.
*/
status_t
RemoteWireWriter::_WriteCompressed(const void* buffer, size_t length,
	bool flushNow)
{
#ifdef ZSTD_ENABLED
	ZSTD_CCtx* context = (ZSTD_CCtx*)fCompressionContext;
	ZSTD_inBuffer input = { buffer, length, 0 };

	// The window is a bound on how long a message may wait, so it is measured
	// from the *first* message in the window, not from the last flush.
	if (fFlushWindow == 0 || (fWindowOpened != 0
			&& system_time() - fWindowOpened >= fFlushWindow)) {
		flushNow = true;
	}

	ZSTD_EndDirective mode = flushNow ? ZSTD_e_flush : ZSTD_e_continue;

	// Only the time actually spent inside the compressor is accumulated; the
	// ring-buffer writes in between can block on a slow client and would
	// otherwise be charged to the encoder.
	while (true) {
		ZSTD_outBuffer output = { fOutputBuffer + kSegmentHeaderReserve,
			fOutputBufferSize - kSegmentHeaderReserve, 0 };

		bigtime_t start = system_time();
		size_t remaining = ZSTD_compressStream2(context, &output, &input, mode);
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

		// Under ZSTD_e_continue the return value is only a hint about the
		// compressor's internal buffers, and is routinely non-zero with the
		// input fully consumed -- that is precisely the state this policy wants
		// the encoder left in. Waiting for it to reach zero would be waiting for
		// a flush nobody asked for.
		bool drained = mode == ZSTD_e_continue || remaining == 0;
		if (input.pos == input.size && drained)
			break;

		// Neither input consumed nor output produced would spin forever.
		if (output.pos == 0 && input.pos == input.size && remaining != 0) {
			TRACE_ERROR("compressor made no progress\n");
			return B_ERROR;
		}
	}

	if (flushNow) {
		fWindowOpened = 0;
		fWindowMessages = 0;
		fFlushes++;
	} else
		_OpenWindow();

	return B_OK;
#else
	// Without a codec there is nothing to compress into, so a capability can
	// never have been armed (SupportedCapabilities() is empty) and this is
	// unreachable. Pass the message through as a raw segment rather than
	// inventing a framing, so that even a mis-built server stays parsable.
	return _WriteRawSegment(buffer, length);
#endif
}


/*!	Closes the open drain window, with fLock held.

	Every byte the compressor is holding belongs to a message it was handed
	whole, and the last of them ended on a message boundary -- nothing else can
	open a window -- so this is a flush at a message boundary regardless of which
	thread asks for it. Does nothing at all when no window is open, which makes
	it safe to call on any path that merely *might* need one closed.
*/
status_t
RemoteWireWriter::_FlushLocked()
{
	if (fWindowOpened == 0)
		return B_OK;

	// Nothing more may be emitted on a broken stream, but the window must still
	// be let go of: leaving it open would have the flusher thread coming back
	// for it every quiescence interval for the rest of the connection.
	if (fStreamBroken) {
		fWindowOpened = 0;
		fWindowMessages = 0;
		return B_IO_ERROR;
	}

#ifdef ZSTD_ENABLED
	ZSTD_CCtx* context = (ZSTD_CCtx*)fCompressionContext;
	if (context == NULL) {
		fWindowOpened = 0;
		fWindowMessages = 0;
		return B_NO_INIT;
	}

	// Empty input: this adds nothing to the stream, it only ends the block. A
	// valid non-NULL pointer rather than NULL, because a zero-length buffer is
	// the one case where zstd's own assertions disagree about NULL.
	ZSTD_inBuffer input = { fOutputBuffer, 0, 0 };
	status_t result = B_OK;

	while (true) {
		ZSTD_outBuffer output = { fOutputBuffer + kSegmentHeaderReserve,
			fOutputBufferSize - kSegmentHeaderReserve, 0 };

		bigtime_t start = system_time();
		size_t remaining = ZSTD_compressStream2(context, &output, &input,
			ZSTD_e_flush);
		fEncodeTime += system_time() - start;

		if (ZSTD_isError(remaining)) {
			TRACE_ERROR("flush failed: %s\n", ZSTD_getErrorName(remaining));
			result = B_ERROR;
			break;
		}

		if (output.pos > 0) {
			result = _WriteStagedSegment(output.pos);
			if (result != B_OK)
				break;
		}

		if (remaining == 0)
			break;

		if (output.pos == 0) {
			TRACE_ERROR("compressor made no progress flushing\n");
			result = B_ERROR;
			break;
		}
	}

	fWindowOpened = 0;
	fWindowMessages = 0;
	fFlushes++;
	return result;
#else
	fWindowOpened = 0;
	fWindowMessages = 0;
	return B_OK;
#endif
}


/*!	Records that a message is sitting unflushed in the compressor, waking the
	flusher for the first one. With fLock held.
*/
void
RemoteWireWriter::_OpenWindow()
{
	fWindowMessages++;
	if (fWindowOpened != 0)
		return;

	fWindowOpened = system_time();

	// One wakeup per window, not per message: the flusher is released only as
	// the window opens, and watches fWindowMessages for the rest of it.
	if (fFlushSignal >= 0)
		release_sem(fFlushSignal);
}


void
RemoteWireWriter::_StartFlusher()
{
	if (fFlusher >= 0)
		return;

	fFlusherQuitting = false;
	fFlushSignal = create_sem(0, "remote wire flush");
	if (fFlushSignal < 0) {
		TRACE_ERROR("no flush semaphore (%s); flushing every message\n",
			strerror(fFlushSignal));
		fFlushWindow = 0;
		return;
	}

	// Above B_NORMAL_PRIORITY because this thread is on the latency path: the
	// bytes a client is waiting for do not leave until it runs. Below the
	// drawing threads' own urgency, so it cannot preempt the work it exists to
	// batch.
	fFlusher = spawn_thread(_FlusherEntry, "remote wire flusher",
		B_DISPLAY_PRIORITY, this);
	if (fFlusher < 0) {
		TRACE_ERROR("no flusher thread (%s); flushing every message\n",
			strerror(fFlusher));
		delete_sem(fFlushSignal);
		fFlushSignal = -1;

		// Without the thread nothing would ever close a window, and a message
		// could sit in the compressor until the next one happened along. Fall
		// back to the policy that needs no help.
		fFlushWindow = 0;
		return;
	}

	resume_thread(fFlusher);
}


/*!	Stops the flusher thread. Must be called with fLock *not* held: the thread
	takes that lock, so joining it while holding the lock would deadlock.
*/
void
RemoteWireWriter::_StopFlusher()
{
	if (fFlusher < 0) {
		if (fFlushSignal >= 0) {
			delete_sem(fFlushSignal);
			fFlushSignal = -1;
		}
		return;
	}

	fFlusherQuitting = true;
	release_sem(fFlushSignal);

	status_t unused;
	wait_for_thread(fFlusher, &unused);
	fFlusher = -1;

	delete_sem(fFlushSignal);
	fFlushSignal = -1;
}


/*static*/ int32
RemoteWireWriter::_FlusherEntry(void* data)
{
	((RemoteWireWriter*)data)->_Flusher();
	return 0;
}


/*!	Closes each drain window: as soon as the drawing threads stop feeding it, or
	when it has been open for fFlushWindow, whichever comes first.

	Closing early is the whole reason the window is affordable. A burst of
	drawing ops is over in well under a millisecond, and once it is over there is
	nothing left to batch with, so holding the bytes for the rest of the window
	would be latency bought for no bytes at all.
*/
void
RemoteWireWriter::_Flusher()
{
	while (!fFlusherQuitting) {
		// An idle stream has no open window, so this thread costs nothing until
		// a message is actually held back.
		if (acquire_sem(fFlushSignal) != B_OK)
			break;

		uint64 seen = 0;
		while (!fFlusherQuitting) {
			snooze(kQuiescenceInterval);

			BAutolock lock(fLock);
			if (!lock.IsLocked())
				break;

			// Already closed -- by a message that could not wait, by a raw
			// segment, or by a Reset(). Nothing to do for this wakeup.
			if (fWindowOpened == 0)
				break;

			bool quiet = fWindowMessages == seen;
			bool expired = system_time() - fWindowOpened >= fFlushWindow;
			if (quiet || expired) {
				_FlushLocked();
				break;
			}

			seen = fWindowMessages;
		}
	}
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

	// Whatever the departed compressor was holding went with it. The next
	// connection starts from an empty window, not from this one's clock.
	fWindowOpened = 0;
	fWindowMessages = 0;

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


/*!	Whether this message's flush cannot wait for the drain window.

	Two kinds cannot. A message the server then *blocks on a reply to* --
	RP_DRAW_STRING and RP_DRAW_STRING_WITH_OFFSETS return the client's measured
	end point, RP_STRING_WIDTH its metrics, RP_READ_BITMAP the framebuffer -- and
	a message the client is blocked waiting for, which on this side is the
	RP_INIT_CONNECTION echo and RP_GET_SYSTEM_PALETTE_RESULT. Batching those does
	not trade latency for bytes; it trades latency for *nothing*, because the
	thread that would have produced the next message is parked until the answer
	comes back, so there is no next message to batch with. The window would just
	be added round-trip time.

	Then the two session-control messages, RP_RESYNC and RP_CLOSE_CONNECTION. The
	first says "everything you have is stale, repaint", the second is the last
	thing this connection ever sends -- and both are rare enough that flushing
	them costs no measurable bytes.

	Everything else is a drawing op nobody is waiting on individually, which is
	what the window is for.
*/
/*static*/ bool
RemoteWireWriter::_MustFlushNow(uint16 code)
{
	switch (code) {
		case RP_DRAW_STRING:
		case RP_DRAW_STRING_WITH_OFFSETS:
		case RP_STRING_WIDTH:
		case RP_READ_BITMAP:
		case RP_INIT_CONNECTION:
		case RP_GET_SYSTEM_PALETTE_RESULT:
		case RP_RESYNC:
		case RP_CLOSE_CONNECTION:
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
	uint64 messagesPerFlush = fFlushes > 0 ? fMessages * 100 / fFlushes : 0;
	TRACE_ALWAYS("zstd msgs %" B_PRIu64 " exempt %" B_PRIu64 " plain %" B_PRIu64
		" wire %" B_PRIu64 " ratio %" B_PRIu64 ".%02" B_PRIu64 "x flushes %"
		B_PRIu64 " (%" B_PRIu64 ".%02" B_PRIu64 " msgs each) encode %" B_PRIu64
		"ns/msg\n", fMessages, fExemptMessages, fPlainBytes, fWireBytes,
		ratio / 100, ratio % 100, fFlushes, messagesPerFlush / 100,
		messagesPerFlush % 100, encodePerMessage);
}
