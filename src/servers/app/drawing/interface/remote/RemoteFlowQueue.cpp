/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */

#include "RemoteFlowQueue.h"

#include "RemoteProtocol.h"

#include <stdlib.h>
#include <string.h>


// Every framed RP message is at least a header: uint16 code, uint32 length.
static const size_t kHeaderSize = sizeof(uint16) + sizeof(uint32);

// B_OP_COPY. Spelled as a literal so this file does not have to pull in
// GraphicsDefs.h for the off-target build of the unit test; asserted against the
// real enumerator in the test.
static const int kModeCopy = 0;

#ifndef REMOTE_FLOW_QUEUE_TEST
#	include <GraphicsDefs.h>
	// Spelling it as a literal above is only safe if it really is the literal.
	static_assert((int)B_OP_COPY == kModeCopy,
		"the flow queue's opaque-mode literal no longer matches B_OP_COPY");
#endif

// No RP_SET_DRAWING_MODE has been seen for this token on this connection, so
// nothing may be assumed about what a pixel op does to the pixels underneath it.
static const int kModeUnknown = -1;

// Damage rectangles considered per frame, and fragments carried by the coverage
// test. Both are caps on work done on the drawing path, not on correctness: over
// the cap the frame is treated as not covered, i.e. kept.
static const size_t kMaxDamageRects = 64;
static const size_t kMaxFragments = 256;


RemoteFlowQueue::RemoteFlowQueue(size_t maxMessages, size_t maxBytes)
	:
	fEntries(NULL),
	fCapacity(0),
	fCount(0),
	fBytes(0),
	fMaxMessages(maxMessages),
	fMaxBytes(maxBytes),
	fExplicitBoundaries(false),
	fResyncOwed(false),
	fOwners(NULL),
	fOwnerCapacity(0),
	fOwnerCount(0),
	fTokens(NULL),
	fTokenCapacity(0),
	fTokenCount(0),
	fEnqueued(0),
	fDrained(0),
	fCoalesced(0),
	fSuperseded(0),
	fCollapses(0)
{
}


RemoteFlowQueue::~RemoteFlowQueue()
{
	for (size_t i = 0; i < fCount; i++)
		free(fEntries[i].data);

	free(fEntries);
	free(fOwners);
	free(fTokens);
}


/*!	The entire safety argument of this class, one opcode at a time.

	Read the default case first: an opcode nobody has classified is OP_PINNED.
	Every future opcode therefore starts out undroppable and only becomes
	droppable when someone writes down why -- which is the opposite of the usual
	accident, where a new opcode inherits whatever the range it happens to land
	in already meant.
*/
/*static*/ RemoteFlowQueue::op_class
RemoteFlowQueue::Classify(uint16 code)
{
	switch (code) {
		// ---- Barriers: these read the client's surface, so every pixel
		// written before them is an input to them and nothing older than one of
		// them may be dropped.
		case RP_COPY_RECT_NO_CLIPPING:
			// A scroll. Its source is the pixels an earlier frame wrote; drop
			// that frame and the copy moves stale content, with damage that
			// still looks "covered". This is the case that makes a naive
			// coverage test unsound.
		case RP_INVERT_RECT:
			// Reads the destination by definition.
		case RP_READ_BITMAP:
			// Reads the destination, and a thread is waiting for the answer.
			return OP_BARRIER;

		// ---- Droppable: pure pixel producers whose effect is confined to the
		// damage their frame declares.
		case RP_INVALIDATE_RECT:
		case RP_INVALIDATE_REGION:
			// The server's own statement of which pixels changed, and (without
			// RP_CAP_FRAME_BOUNDARY) the frame boundary itself. Droppable only
			// with the frame it belongs to: a later frame declares the same
			// region again.
		case RP_TIER_END_FRAME:
		case RP_TIER_BEGIN_FRAME:
		case RP_CODEC_TILE:
			// Tier P is the media channel: lossy-tolerant by design.
		case RP_DRAW_BITMAP:
		case RP_DRAW_BITMAP_RECTS:
		case RP_STROKE_ARC:
		case RP_STROKE_BEZIER:
		case RP_STROKE_ELLIPSE:
		case RP_STROKE_POLYGON:
		case RP_STROKE_RECT:
		case RP_STROKE_ROUND_RECT:
		case RP_STROKE_SHAPE:
		case RP_STROKE_TRIANGLE:
		case RP_STROKE_LINE:
		case RP_STROKE_LINE_ARRAY:
		case RP_FILL_ARC:
		case RP_FILL_BEZIER:
		case RP_FILL_ELLIPSE:
		case RP_FILL_POLYGON:
		case RP_FILL_RECT:
		case RP_FILL_ROUND_RECT:
		case RP_FILL_SHAPE:
		case RP_FILL_TRIANGLE:
		case RP_FILL_REGION:
		case RP_FILL_ARC_GRADIENT:
		case RP_FILL_BEZIER_GRADIENT:
		case RP_FILL_ELLIPSE_GRADIENT:
		case RP_FILL_POLYGON_GRADIENT:
		case RP_FILL_RECT_GRADIENT:
		case RP_FILL_ROUND_RECT_GRADIENT:
		case RP_FILL_SHAPE_GRADIENT:
		case RP_FILL_TRIANGLE_GRADIENT:
		case RP_FILL_REGION_GRADIENT:
		case RP_STROKE_ARC_GRADIENT:
		case RP_STROKE_BEZIER_GRADIENT:
		case RP_STROKE_ELLIPSE_GRADIENT:
		case RP_STROKE_POLYGON_GRADIENT:
		case RP_STROKE_RECT_GRADIENT:
		case RP_STROKE_ROUND_RECT_GRADIENT:
		case RP_STROKE_SHAPE_GRADIENT:
		case RP_STROKE_TRIANGLE_GRADIENT:
		case RP_STROKE_LINE_GRADIENT:
		case RP_STROKE_POINT_COLOR:
		case RP_STROKE_LINE_1PX_COLOR:
		case RP_STROKE_RECT_1PX_COLOR:
		case RP_FILL_RECT_COLOR:
		case RP_FILL_REGION_COLOR_NO_CLIPPING:
			return OP_DROPPABLE;

		// ---- Coalescable: durable client-side state that a later message of
		// the same opcode and token fully overwrites. Never dropped outright --
		// RemoteDrawingEngine dedups against its shadow, so a dropped setter is
		// never re-sent and the client draws with the wrong state for the rest
		// of the session (this is D4, from the other direction).
		case RP_SET_OFFSETS:
		case RP_SET_HIGH_COLOR:
		case RP_SET_LOW_COLOR:
		case RP_SET_PEN_SIZE:
		case RP_SET_STROKE_MODE:
		case RP_SET_BLENDING_MODE:
		case RP_SET_PATTERN:
		case RP_SET_DRAWING_MODE:
		case RP_SET_FONT:
		case RP_SET_TRANSFORM:
		case RP_CONSTRAIN_CLIPPING_REGION:
		case RP_MOVE_CURSOR_TO:
			// Latest position wins; an intermediate one has no other effect.
			return OP_COALESCABLE;

		// ---- Pinned. Listed rather than left to the default so that the
		// reason is on the record.
		case RP_CREATE_STATE:
		case RP_DELETE_STATE:
		case RP_ENABLE_SYNC_DRAWING:
		case RP_DISABLE_SYNC_DRAWING:
			// State lifecycle: dropping one leaves the client with a token it
			// does not have, or keeps one it should have freed.
		case RP_DRAW_STRING:
		case RP_DRAW_STRING_WITH_OFFSETS:
		case RP_DRAW_STRING_RESULT:
		case RP_STRING_WIDTH:
		case RP_STRING_WIDTH_RESULT:
		case RP_READ_BITMAP_RESULT:
		case RP_GET_SYSTEM_PALETTE:
		case RP_GET_SYSTEM_PALETTE_RESULT:
			// Round trips. A drawing thread is parked on the reply; dropping
			// the query converts a dropped frame into a stalled desktop.
			//
			// RP_DRAW_STRING is a pixel producer *and* a round trip
			// (RP_DRAW_STRING_RESULT carries the advance back), so it is pinned
			// even though its pixels would be redrawn. That is deliberately
			// conservative: it means a frame containing text is never dropped.
			// Text frames are the cheap ones (measured: the whole text path is
			// 19 % of a typing capture at 15.8 kB/s), so the conservatism costs
			// little and buys a rule with no exception to get wrong.
		case RP_SET_CURSOR:
		case RP_SET_CURSOR_VISIBLE:
			// Cursor identity and visibility are not re-sent.
		case RP_INIT_CONNECTION:
		case RP_UPDATE_DISPLAY_MODE:
		case RP_CLOSE_CONNECTION:
		case RP_HELLO:
		case RP_HELLO_ACK:
		case RP_RESYNC:
		case RP_AUTHENTICATE:
		case RP_AUTH_RESULT:
		case RP_SESSION_COOKIE:
		case RP_AUDIO_PACKET:
			return OP_PINNED;

		default:
			// Unknown, including every client -> server input opcode that has no
			// business in this queue and every opcode a later milestone adds.
			return OP_PINNED;
	}
}


/*!	Whether \a code carries a uint32 state token immediately after the framed
	header. Most engine ops do; the three exceptions below are real and were
	found by reading the emitters, not assumed from the opcode ranges.
*/
static bool
op_has_token(uint16 code)
{
	switch (code) {
		case RP_COPY_RECT_NO_CLIPPING:
			// (xOffset, yOffset, rect) -- no token.
		case RP_FILL_REGION_COLOR_NO_CLIPPING:
			// (region, color) -- no token.
		case RP_INVALIDATE_RECT:
		case RP_INVALIDATE_REGION:
			// Emitted by the interface, not by an engine.
		case RP_TIER_BEGIN_FRAME:
		case RP_TIER_END_FRAME:
		case RP_AUDIO_PACKET:
		case RP_FRAME_ACK:
		case RP_INIT_CONNECTION:
		case RP_UPDATE_DISPLAY_MODE:
		case RP_CLOSE_CONNECTION:
		case RP_GET_SYSTEM_PALETTE:
		case RP_GET_SYSTEM_PALETTE_RESULT:
		case RP_HELLO:
		case RP_HELLO_ACK:
		case RP_RESYNC:
		case RP_AUTHENTICATE:
		case RP_AUTH_RESULT:
		case RP_SESSION_COOKIE:
		case RP_SET_CURSOR:
		case RP_SET_CURSOR_VISIBLE:
		case RP_MOVE_CURSOR_TO:
			return false;

		default:
			return true;
	}
}


/*!	Whether \a code's pixels depend on the pixels already there.

	Only the fast colour primitives and the frame/damage bookkeeping are
	unconditionally independent of the token's drawing mode; everything else is
	interpreted by the client against whatever RP_SET_DRAWING_MODE last said, so
	a queue that does not know the mode does not know whether the op reads the
	destination.
*/
static bool
op_is_mode_independent(uint16 code)
{
	switch (code) {
		case RP_STROKE_POINT_COLOR:
		case RP_STROKE_LINE_1PX_COLOR:
		case RP_STROKE_RECT_1PX_COLOR:
		case RP_FILL_RECT_COLOR:
		case RP_FILL_REGION_COLOR_NO_CLIPPING:
			// The fast colour primitives exist precisely for the opaque case.
		case RP_INVALIDATE_RECT:
		case RP_INVALIDATE_REGION:
		case RP_TIER_END_FRAME:
		case RP_TIER_BEGIN_FRAME:
		case RP_CODEC_TILE:
			return true;

		default:
			return false;
	}
}


void
RemoteFlowQueue::SetExplicitBoundaries(bool explicitBoundaries)
{
	fExplicitBoundaries = explicitBoundaries;
}


bool
RemoteFlowQueue::FrameIsClose(uint16 code) const
{
	if (fExplicitBoundaries)
		return code == RP_TIER_END_FRAME;

	return code == RP_INVALIDATE_RECT || code == RP_INVALIDATE_REGION;
}


void
RemoteFlowQueue::Observe(const void* message, size_t length)
{
	if (message == NULL || length < kHeaderSize)
		return;

	uint16 code = 0;
	memcpy(&code, message, sizeof(uint16));
	if (code != RP_SET_DRAWING_MODE)
		return;

	// token:uint32, mode:uint32 (an enum, encoded as four bytes).
	if (length < kHeaderSize + sizeof(uint32) + sizeof(uint32))
		return;

	const uint8* data = (const uint8*)message;
	uint32 token = 0;
	uint32 mode = 0;
	memcpy(&token, data + kHeaderSize, sizeof(uint32));
	memcpy(&mode, data + kHeaderSize + sizeof(uint32), sizeof(uint32));

	_NoteMode(token, (int)mode);
}


status_t
RemoteFlowQueue::Enqueue(const void* message, size_t length, int32 owner)
{
	if (message == NULL || length < kHeaderSize)
		return B_BAD_VALUE;

	Observe(message, length);

	uint16 code = 0;
	memcpy(&code, message, sizeof(uint16));

	op_class klass = Classify(code);

	uint32 token = 0;
	bool hasToken = op_has_token(code)
		&& _ReadToken((const uint8*)message, length, token);

	// A pixel op whose token's drawing mode is unknown, or is anything other
	// than B_OP_COPY, may be reading the destination -- so it is demoted to a
	// barrier rather than trusted as a pure producer. This is what keeps
	// alpha-blended content from being "covered" by a frame that only blends
	// over it, and it is also why the queue has to see every message
	// (Observe()), not only the ones it stores.
	if (klass == OP_DROPPABLE && !op_is_mode_independent(code)) {
		if (!hasToken || _ModeOf(token) != kModeCopy)
			klass = OP_BARRIER;
	}

	if (!_EnforceBound(length))
		return B_NO_MEMORY;

	if (!_Grow(fCount + 1)) {
		// Nothing was stored and the caller is about to believe otherwise.
		CollapseToResync();
		return B_NO_MEMORY;
	}

	uint8* copy = (uint8*)malloc(length);
	if (copy == NULL) {
		CollapseToResync();
		return B_NO_MEMORY;
	}

	memcpy(copy, message, length);

	entry& e = fEntries[fCount];
	e.data = copy;
	e.length = length;
	e.code = code;
	e.klass = (uint8)klass;
	e.owner = owner;
	e.frame = _FrameOf(owner);
	e.token = token;
	e.hasToken = hasToken;

	fCount++;
	fBytes += length;
	fEnqueued++;

	if (FrameIsClose(code))
		_CloseFrame(owner);

	return B_OK;
}


const uint8*
RemoteFlowQueue::PeekFront(size_t& _length) const
{
	if (fCount == 0) {
		_length = 0;
		return NULL;
	}

	_length = fEntries[0].length;
	return fEntries[0].data;
}


void
RemoteFlowQueue::PopFront()
{
	if (fCount == 0)
		return;

	fDrained++;
	_Remove(0);
}


void
RemoteFlowQueue::CollapseToResync()
{
	if (fCount > 0 || !fResyncOwed)
		fCollapses++;

	for (size_t i = 0; i < fCount; i++)
		free(fEntries[i].data);

	fCount = 0;
	fBytes = 0;
	fResyncOwed = true;

	// The frames those entries belonged to are gone; nothing may be attributed
	// to them afterwards.
	fOwnerCount = 0;
}


void
RemoteFlowQueue::Reset()
{
	for (size_t i = 0; i < fCount; i++)
		free(fEntries[i].data);

	fCount = 0;
	fBytes = 0;
	fResyncOwed = false;
	fOwnerCount = 0;
	fTokenCount = 0;
	fExplicitBoundaries = false;
	fEnqueued = fDrained = fCoalesced = fSuperseded = fCollapses = 0;
}


void
RemoteFlowQueue::GetStatistics(uint64& _enqueued, uint64& _drained,
	uint64& _coalesced, uint64& _superseded, uint64& _collapses) const
{
	_enqueued = fEnqueued;
	_drained = fDrained;
	_coalesced = fCoalesced;
	_superseded = fSuperseded;
	_collapses = fCollapses;
}


size_t
RemoteFlowQueue::CountFrames() const
{
	size_t frames = 0;
	for (size_t i = 0; i < fCount; i++) {
		if (FrameIsClose(fEntries[i].code))
			frames++;
	}

	return frames;
}


// #pragma mark - bound enforcement


bool
RemoteFlowQueue::_EnforceBound(size_t incoming)
{
	if (fCount + 1 <= fMaxMessages && fBytes + incoming <= fMaxBytes)
		return true;

	fCoalesced += _Coalesce();
	if (fCount + 1 <= fMaxMessages && fBytes + incoming <= fMaxBytes)
		return true;

	fSuperseded += _Supersede();
	if (fCount + 1 <= fMaxMessages && fBytes + incoming <= fMaxBytes)
		return true;

	// Out of policy. Everything queued goes, and the connection owes an
	// RP_RESYNC plus a state replay before what the client shows can be
	// trusted again. The incoming message is then queued into an empty queue,
	// so the effective bound is kMaxBytes plus one message -- a single message
	// is never split to fit, because a split message is unrecoverable and a
	// slightly larger bound is not.
	CollapseToResync();
	return true;
}


size_t
RemoteFlowQueue::_Coalesce()
{
	// A coalescable setter is removed when a later message in the same frame,
	// from the same emitter and for the same token, sets the same thing again
	// with nothing in between that could have observed the earlier value.
	//
	// The scan stops at the first same-owner entry that is not itself
	// coalescable. That single condition is what keeps this inside one frame:
	// the frame's closing message (an invalidate, or RP_TIER_END_FRAME) is
	// droppable, not coalescable, so the scan can never cross it.
	size_t removed = 0;
	size_t i = 0;
	while (i < fCount) {
		const entry& candidate = fEntries[i];
		if (candidate.klass != OP_COALESCABLE || !candidate.hasToken) {
			i++;
			continue;
		}

		bool superseded = false;
		for (size_t j = i + 1; j < fCount; j++) {
			const entry& later = fEntries[j];
			if (later.owner != candidate.owner)
				continue;

			if (later.klass != OP_COALESCABLE)
				break;

			if (later.code == candidate.code && later.hasToken
				&& later.token == candidate.token) {
				superseded = true;
				break;
			}
		}

		if (!superseded) {
			i++;
			continue;
		}

		_Remove(i);
		removed++;
		// Do not advance: _Remove() shifted the successor into i.
	}

	return removed;
}


size_t
RemoteFlowQueue::_Supersede()
{
	if (fCount == 0)
		return 0;

	// Nothing at or before the newest barrier may be dropped: a barrier reads
	// the surface, so every pixel written before it is one of its inputs.
	long lastBarrier = -1;
	for (size_t i = 0; i < fCount; i++) {
		if (fEntries[i].klass == OP_BARRIER)
			lastBarrier = (long)i;
	}

	size_t removed = 0;

	// Frames are per-emitter. One emitter's messages between two of its own
	// boundaries are exactly one drawing transaction (or, across an update
	// session with copy-to-front disabled, several transactions whose union the
	// closing invalidate declares), so the closing damage is a statement about
	// all of them. Across emitters it is not: engine B's ops can land inside
	// engine A's span while B's damage is only declared later, and dropping
	// "A's frame" would then drop ops that nothing repaints. Keying frames on
	// the emitting thread is what makes the coverage test a proof instead of a
	// guess.
	size_t ownerIndex = 0;
	while (ownerIndex < fCount) {
		int32 owner = fEntries[ownerIndex].owner;

		bool seen = false;
		for (size_t k = 0; k < ownerIndex; k++) {
			if (fEntries[k].owner == owner) {
				seen = true;
				break;
			}
		}

		if (seen) {
			ownerIndex++;
			continue;
		}

		removed += _SupersedeOwner(owner, lastBarrier);
		ownerIndex++;
	}

	return removed;
}


/*!	Runs the coverage test over one emitter's frames, newest first, dropping a
	frame whose damage the frames kept after it already repaint.
*/
size_t
RemoteFlowQueue::_SupersedeOwner(int32 owner, long lastBarrier)
{
	// Collect this emitter's frame numbers, newest first.
	uint32 frames[64];
	size_t frameCount = 0;
	for (size_t i = fCount; i > 0; i--) {
		const entry& e = fEntries[i - 1];
		if (e.owner != owner)
			continue;

		bool have = false;
		for (size_t k = 0; k < frameCount; k++) {
			if (frames[k] == e.frame) {
				have = true;
				break;
			}
		}

		if (!have) {
			if (frameCount == 64)
				break;
			frames[frameCount++] = e.frame;
		}
	}

	rect coverage[kMaxFragments];
	size_t coverageCount = 0;
	size_t removed = 0;

	for (size_t f = 0; f < frameCount; f++) {
		uint32 frame = frames[f];

		// Gather the frame: its index span, whether it is closed, whether every
		// message in it is droppable, and the damage it declares.
		bool closed = false;
		bool allDroppable = true;
		size_t firstIndex = fCount;
		rect damage[kMaxDamageRects];
		size_t damageCount = 0;
		bool damageUsable = true;

		for (size_t i = 0; i < fCount; i++) {
			const entry& e = fEntries[i];
			if (e.owner != owner || e.frame != frame)
				continue;

			if (i < firstIndex)
				firstIndex = i;

			if (e.klass != OP_DROPPABLE)
				allDroppable = false;

			if (FrameIsClose(e.code))
				closed = true;

			if (e.code == RP_INVALIDATE_RECT || e.code == RP_INVALIDATE_REGION) {
				size_t count = 0;
				if (!_ReadDamage(e.data, e.length, e.code,
						damage + damageCount, kMaxDamageRects - damageCount,
						count)) {
					damageUsable = false;
				} else
					damageCount += count;
			}
		}

		bool droppable = closed && allDroppable && damageUsable
			&& damageCount > 0 && (lastBarrier < 0
				|| (long)firstIndex > lastBarrier)
			&& _Covers(coverage, coverageCount, damage, damageCount);

		if (droppable) {
			// Remove the frame's entries back to front so the indices ahead of
			// each removal stay valid.
			for (size_t i = fCount; i > 0; i--) {
				const entry& e = fEntries[i - 1];
				if (e.owner == owner && e.frame == frame) {
					_Remove(i - 1);
					removed++;
				}
			}

			continue;
		}

		// Kept: its damage becomes coverage for the frames older than it. Only
		// kept frames contribute, which is exactly "the union of the damage of
		// the later frames that will actually be delivered".
		for (size_t d = 0; d < damageCount && coverageCount < kMaxFragments;
				d++) {
			coverage[coverageCount++] = damage[d];
		}
	}

	return removed;
}


// #pragma mark - storage


bool
RemoteFlowQueue::_Grow(size_t needed)
{
	if (needed <= fCapacity)
		return true;

	size_t capacity = fCapacity == 0 ? 64 : fCapacity * 2;
	while (capacity < needed)
		capacity *= 2;

	entry* entries = (entry*)realloc(fEntries, capacity * sizeof(entry));
	if (entries == NULL)
		return false;

	fEntries = entries;
	fCapacity = capacity;
	return true;
}


void
RemoteFlowQueue::_Remove(size_t index)
{
	if (index >= fCount)
		return;

	fBytes -= fEntries[index].length;
	free(fEntries[index].data);

	if (index + 1 < fCount) {
		memmove(fEntries + index, fEntries + index + 1,
			(fCount - index - 1) * sizeof(entry));
	}

	fCount--;
}


uint32
RemoteFlowQueue::_FrameOf(int32 owner) const
{
	for (size_t i = 0; i < fOwnerCount; i++) {
		if (fOwners[i].owner == owner)
			return fOwners[i].frame;
	}

	return 0;
}


void
RemoteFlowQueue::_CloseFrame(int32 owner)
{
	for (size_t i = 0; i < fOwnerCount; i++) {
		if (fOwners[i].owner == owner) {
			fOwners[i].frame++;
			return;
		}
	}

	if (fOwnerCount == fOwnerCapacity) {
		size_t capacity = fOwnerCapacity == 0 ? 8 : fOwnerCapacity * 2;
		owner_state* owners = (owner_state*)realloc(fOwners,
			capacity * sizeof(owner_state));
		if (owners == NULL)
			return;

		fOwners = owners;
		fOwnerCapacity = capacity;
	}

	fOwners[fOwnerCount].owner = owner;
	fOwners[fOwnerCount].frame = 1;
	fOwnerCount++;
}


int
RemoteFlowQueue::_ModeOf(uint32 token) const
{
	for (size_t i = 0; i < fTokenCount; i++) {
		if (fTokens[i].token == token)
			return fTokens[i].mode;
	}

	return kModeUnknown;
}


void
RemoteFlowQueue::_NoteMode(uint32 token, int mode)
{
	for (size_t i = 0; i < fTokenCount; i++) {
		if (fTokens[i].token == token) {
			fTokens[i].mode = mode;
			return;
		}
	}

	if (fTokenCount == fTokenCapacity) {
		size_t capacity = fTokenCapacity == 0 ? 16 : fTokenCapacity * 2;
		token_state* tokens = (token_state*)realloc(fTokens,
			capacity * sizeof(token_state));
		if (tokens == NULL)
			return;

		fTokens = tokens;
		fTokenCapacity = capacity;
	}

	fTokens[fTokenCount].token = token;
	fTokens[fTokenCount].mode = mode;
	fTokenCount++;
}


// #pragma mark - wire parsing


/*static*/ bool
RemoteFlowQueue::_ReadToken(const uint8* data, size_t length, uint32& _token)
{
	if (length < kHeaderSize + sizeof(uint32))
		return false;

	memcpy(&_token, data + kHeaderSize, sizeof(uint32));
	return true;
}


/*!	Reads the damage an RP_INVALIDATE_RECT / RP_INVALIDATE_REGION declares.

	Returns false -- meaning "this frame's damage is not usable, keep the frame"
	-- for a truncated message, more rectangles than \a capacity, or a
	rectangle with a non-integral edge. The last one matters: the coverage test
	below works in Haiku's inclusive-edge pixel units and subtracts with +/- 1,
	which is only exact on integral coordinates. Every damage region app_server
	produces comes from a BRegion of pixel-aligned rectangles, so refusing the
	rest costs nothing and removes a class of float-fuzz mistake entirely.
*/
/*static*/ bool
RemoteFlowQueue::_ReadDamage(const uint8* data, size_t length, uint16 code,
	rect* rects, size_t capacity, size_t& _count)
{
	_count = 0;

	size_t offset = kHeaderSize;
	size_t count = 0;

	if (code == RP_INVALIDATE_RECT)
		count = 1;
	else {
		if (length < offset + sizeof(int32))
			return false;

		int32 rectCount = 0;
		memcpy(&rectCount, data + offset, sizeof(int32));
		offset += sizeof(int32);
		if (rectCount < 0)
			return false;

		count = (size_t)rectCount;
	}

	if (count > capacity)
		return false;

	if (length < offset + count * 4 * sizeof(float))
		return false;

	for (size_t i = 0; i < count; i++) {
		rect r;
		memcpy(&r, data + offset, 4 * sizeof(float));
		offset += 4 * sizeof(float);

		if (!_IsIntegral(r))
			return false;

		// An empty or inverted rectangle declares nothing.
		if (r.right < r.left || r.bottom < r.top)
			return false;

		rects[i] = r;
	}

	_count = count;
	return true;
}


/*static*/ bool
RemoteFlowQueue::_IsIntegral(const rect& r)
{
	const float values[4] = { r.left, r.top, r.right, r.bottom };
	for (int i = 0; i < 4; i++) {
		float value = values[i];
		if (value < -1e7f || value > 1e7f)
			return false;
		if (value != (float)(long)value)
			return false;
	}

	return true;
}


/*!	Whether every pixel of \a inner is inside \a outer.

	Straight rectangle subtraction on inclusive edges: each inner rectangle is
	cut down by every outer rectangle in turn, and the answer is yes only when
	nothing is left over. Running out of fragments answers no, which keeps the
	frame.
*/
/*static*/ bool
RemoteFlowQueue::_Covers(const rect* outer, size_t outerCount,
	const rect* inner, size_t innerCount)
{
	if (innerCount == 0)
		return false;

	if (outerCount == 0)
		return false;

	for (size_t i = 0; i < innerCount; i++) {
		rect pending[kMaxFragments];
		size_t pendingCount = 1;
		pending[0] = inner[i];

		for (size_t o = 0; o < outerCount && pendingCount > 0; o++) {
			const rect& cut = outer[o];

			rect next[kMaxFragments];
			size_t nextCount = 0;

			for (size_t p = 0; p < pendingCount; p++) {
				const rect& r = pending[p];

				// No overlap: the fragment survives whole.
				if (cut.right < r.left || cut.left > r.right
					|| cut.bottom < r.top || cut.top > r.bottom) {
					if (nextCount == kMaxFragments)
						return false;
					next[nextCount++] = r;
					continue;
				}

				// Up to four survivors: above, below, left, right.
				if (cut.top > r.top) {
					if (nextCount == kMaxFragments)
						return false;
					rect piece = r;
					piece.bottom = cut.top - 1;
					next[nextCount++] = piece;
				}

				if (cut.bottom < r.bottom) {
					if (nextCount == kMaxFragments)
						return false;
					rect piece = r;
					piece.top = cut.bottom + 1;
					next[nextCount++] = piece;
				}

				float top = cut.top > r.top ? cut.top : r.top;
				float bottom = cut.bottom < r.bottom ? cut.bottom : r.bottom;

				if (cut.left > r.left) {
					if (nextCount == kMaxFragments)
						return false;
					rect piece;
					piece.left = r.left;
					piece.right = cut.left - 1;
					piece.top = top;
					piece.bottom = bottom;
					next[nextCount++] = piece;
				}

				if (cut.right < r.right) {
					if (nextCount == kMaxFragments)
						return false;
					rect piece;
					piece.left = cut.right + 1;
					piece.right = r.right;
					piece.top = top;
					piece.bottom = bottom;
					next[nextCount++] = piece;
				}
			}

			memcpy(pending, next, nextCount * sizeof(rect));
			pendingCount = nextCount;
		}

		if (pendingCount > 0)
			return false;
	}

	return true;
}
