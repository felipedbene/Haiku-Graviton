/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 *
 * Unit test for the M2 outbound flow-control policy (RemoteFlowQueue).
 *
 * The policy is pure logic, so it is tested directly rather than inferred from a
 * running desktop. Every rule the policy claims has at least one check that
 * fails if the rule is removed; run mutation_test.sh to see each of them go red.
 *
 * Build and run:  ./run.sh
 */

#include "RemoteFlowQueue.h"
#include "RemoteProtocol.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>


static int sChecks = 0;
static int sFailures = 0;


static void
check(bool ok, const char* what)
{
	sChecks++;
	if (ok)
		return;

	sFailures++;
	printf("FAIL  %s\n", what);
}


// #pragma mark - message construction


class Message {
public:
	Message(uint16 code)
	{
		Put16(code);
		Put32(0);			// back-patched by Bytes()
		fCode = code;
	}

	void Put16(uint16 value)
	{
		fBytes.push_back((uint8)(value & 0xff));
		fBytes.push_back((uint8)(value >> 8));
	}

	void Put32(uint32 value)
	{
		for (int i = 0; i < 4; i++)
			fBytes.push_back((uint8)((value >> (8 * i)) & 0xff));
	}

	void PutFloat(float value)
	{
		uint32 bits;
		memcpy(&bits, &value, 4);
		Put32(bits);
	}

	void PutRect(float l, float t, float r, float b)
	{
		PutFloat(l);
		PutFloat(t);
		PutFloat(r);
		PutFloat(b);
	}

	std::vector<uint8> Bytes() const
	{
		std::vector<uint8> out = fBytes;
		uint32 length = (uint32)out.size();
		for (int i = 0; i < 4; i++)
			out[2 + i] = (uint8)((length >> (8 * i)) & 0xff);
		return out;
	}

	uint16 Code() const { return fCode; }

private:
	std::vector<uint8>	fBytes;
	uint16				fCode;
};


//! A pixel op: token, then a tag so the test can tell instances apart.
static std::vector<uint8>
pixelOp(uint16 code, uint32 token, uint32 tag)
{
	Message m(code);
	m.Put32(token);
	m.Put32(tag);
	return m.Bytes();
}


//! An op with no token at all (RP_COPY_RECT_NO_CLIPPING and friends).
static std::vector<uint8>
tokenlessOp(uint16 code, uint32 tag)
{
	Message m(code);
	m.Put32(tag);
	return m.Bytes();
}


static std::vector<uint8>
setDrawingMode(uint32 token, uint32 mode, uint32 tag)
{
	Message m(RP_SET_DRAWING_MODE);
	m.Put32(token);
	m.Put32(mode);
	m.Put32(tag);
	return m.Bytes();
}


static std::vector<uint8>
invalidateRegion(const std::vector<std::vector<float> >& rects, uint32 tag)
{
	Message m(RP_INVALIDATE_REGION);
	m.Put32((uint32)rects.size());
	for (size_t i = 0; i < rects.size(); i++)
		m.PutRect(rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
	m.Put32(tag);
	return m.Bytes();
}


static std::vector<uint8>
invalidateOne(float l, float t, float r, float b, uint32 tag)
{
	std::vector<float> rect;
	rect.push_back(l);
	rect.push_back(t);
	rect.push_back(r);
	rect.push_back(b);
	std::vector<std::vector<float> > rects;
	rects.push_back(rect);
	return invalidateRegion(rects, tag);
}


static std::vector<uint8>
endFrame(uint32 sequence, uint32 tag)
{
	Message m(RP_TIER_END_FRAME);
	m.Put32(sequence);
	m.Put32(tag);
	return m.Bytes();
}


static status_t
feed(RemoteFlowQueue& queue, const std::vector<uint8>& message, int32 owner)
{
	return queue.Enqueue(&message[0], message.size(), owner);
}


static std::vector<uint32>
drainTags(RemoteFlowQueue& queue, std::vector<uint8>& _stream)
{
	std::vector<uint32> tags;
	while (true) {
		size_t length = 0;
		const uint8* data = queue.PeekFront(length);
		if (data == NULL)
			break;

		_stream.insert(_stream.end(), data, data + length);

		uint32 tag = 0;
		if (length >= 4)
			memcpy(&tag, data + length - 4, 4);
		tags.push_back(tag);

		queue.PopFront();
	}

	return tags;
}


/*!	Walks \a stream as a sequence of whole framed RP messages.

	This is the instrument for "no partial message is ever emitted", so it has to
	be able to fail: it returns false for a truncated tail, a length field that
	disagrees with what is there, and a length below the 6-byte header. Check 30
	is its positive control.
*/
static bool
framingIsWhole(const std::vector<uint8>& stream, size_t& _messages)
{
	_messages = 0;
	size_t offset = 0;
	while (offset < stream.size()) {
		if (stream.size() - offset < 6)
			return false;

		uint32 length = 0;
		for (int i = 0; i < 4; i++)
			length |= (uint32)stream[offset + 2 + i] << (8 * i);

		if (length < 6)
			return false;
		if (length > stream.size() - offset)
			return false;

		offset += length;
		_messages++;
	}

	return offset == stream.size();
}


struct Stats {
	uint64 enqueued;
	uint64 drained;
	uint64 coalesced;
	uint64 superseded;
	uint64 collapses;
};


static Stats
statsOf(const RemoteFlowQueue& queue)
{
	Stats s;
	queue.GetStatistics(s.enqueued, s.drained, s.coalesced, s.superseded,
		s.collapses);
	return s;
}


static bool
contains(const std::vector<uint32>& tags, uint32 tag)
{
	for (size_t i = 0; i < tags.size(); i++) {
		if (tags[i] == tag)
			return true;
	}

	return false;
}


// #pragma mark - the tests


static void
testClassification()
{
	typedef RemoteFlowQueue Q;

	check(Q::Classify(RP_FILL_RECT) == Q::OP_DROPPABLE,
		"1: RP_FILL_RECT is droppable");
	check(Q::Classify(RP_DRAW_BITMAP) == Q::OP_DROPPABLE,
		"2: RP_DRAW_BITMAP is droppable");
	check(Q::Classify(RP_INVALIDATE_REGION) == Q::OP_DROPPABLE,
		"3: RP_INVALIDATE_REGION is droppable");
	check(Q::Classify(RP_SET_HIGH_COLOR) == Q::OP_COALESCABLE,
		"4: RP_SET_HIGH_COLOR is coalescable, not droppable");
	check(Q::Classify(RP_CONSTRAIN_CLIPPING_REGION) == Q::OP_COALESCABLE,
		"5: RP_CONSTRAIN_CLIPPING_REGION is coalescable");
	check(Q::Classify(RP_COPY_RECT_NO_CLIPPING) == Q::OP_BARRIER,
		"6: RP_COPY_RECT_NO_CLIPPING is a barrier (it reads the surface)");
	check(Q::Classify(RP_INVERT_RECT) == Q::OP_BARRIER,
		"7: RP_INVERT_RECT is a barrier");
	check(Q::Classify(RP_CREATE_STATE) == Q::OP_PINNED,
		"8: RP_CREATE_STATE is pinned");
	check(Q::Classify(RP_DRAW_STRING) == Q::OP_PINNED,
		"9: RP_DRAW_STRING is pinned (a thread waits for its result)");
	check(Q::Classify(RP_STRING_WIDTH) == Q::OP_PINNED,
		"10: RP_STRING_WIDTH is pinned");
	check(Q::Classify(RP_HELLO_ACK) == Q::OP_PINNED,
		"11: RP_HELLO_ACK is pinned");
	check(Q::Classify(60000) == Q::OP_PINNED,
		"12: an unknown opcode is pinned, not droppable");
	check(Q::Classify(RP_KEY_DOWN) == Q::OP_PINNED,
		"13: an input opcode is pinned");
}


//! Two frames, the second repainting exactly what the first did.
static void
testSupersedeCovered()
{
	RemoteFlowQueue queue(6, 1024 * 1024);

	check(feed(queue, setDrawingMode(1, 0, 100), 1) == B_OK,
		"14: the opaque drawing mode is accepted");
	feed(queue, invalidateOne(0, 0, 9, 9, 101), 1);			// closes frame 0
	feed(queue, pixelOp(RP_FILL_RECT, 1, 110), 1);			// frame 1
	feed(queue, invalidateOne(0, 0, 99, 99, 111), 1);		// closes frame 1
	feed(queue, pixelOp(RP_FILL_RECT, 1, 120), 1);			// frame 2
	feed(queue, invalidateOne(0, 0, 99, 99, 121), 1);		// closes frame 2

	check(queue.CountMessages() == 6, "15: six messages queued, at the bound");

	// The seventh trips the policy.
	feed(queue, pixelOp(RP_FILL_RECT, 1, 130), 1);

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	check(!contains(tags, 110),
		"16: the superseded frame's draw op was dropped");
	check(!contains(tags, 111),
		"17: the superseded frame's damage was dropped with it");
	check(contains(tags, 120) && contains(tags, 121),
		"18: the covering frame was kept");
	check(contains(tags, 100),
		"19: the drawing-mode setter was kept (coalescable, never dropped)");
	check(contains(tags, 130), "20: the incoming message was queued");
	Stats stats = statsOf(queue);
	check(stats.superseded == 2 && stats.collapses == 0,
		"20a: exactly one frame (two messages) was superseded, no collapse");

	size_t messages = 0;
	check(framingIsWhole(stream, messages),
		"21: what came out is a sequence of whole messages");
	check(!queue.ResyncOwed(),
		"22: a policy that succeeded does not owe a resync");
}


//! The same shape, but the later frame repaints only a quarter of it.
static void
testSupersedeNotCovered()
{
	RemoteFlowQueue queue(6, 1024 * 1024);

	feed(queue, setDrawingMode(1, 0, 200), 1);
	feed(queue, invalidateOne(0, 0, 9, 9, 201), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 210), 1);
	feed(queue, invalidateOne(0, 0, 99, 99, 211), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 220), 1);
	feed(queue, invalidateOne(0, 0, 49, 49, 221), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 230), 1);

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	Stats stats = statsOf(queue);
	check(stats.superseded == 0,
		"23: a frame the later frames do not fully repaint is NOT dropped");
	check(stats.collapses == 1,
		"24: with nothing droppable the bound collapsed instead");
	check(queue.ResyncOwed(),
		"25: and the collapse owes an RP_RESYNC");
	size_t messages = 0;
	check(framingIsWhole(stream, messages),
		"25a: the collapse left only whole messages");
}


//! A frame that contains a state op is not whole-frame content.
static void
testStateOpPinsItsFrame()
{
	RemoteFlowQueue queue(6, 1024 * 1024);

	feed(queue, setDrawingMode(1, 0, 300), 1);
	feed(queue, invalidateOne(0, 0, 9, 9, 301), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 310), 1);
	feed(queue, pixelOp(RP_CREATE_STATE, 2, 311), 1);	// pinned, in frame 1
	feed(queue, invalidateOne(0, 0, 99, 99, 312), 1);
	feed(queue, invalidateOne(0, 0, 99, 99, 321), 1);	// frame 2, covers it
	feed(queue, pixelOp(RP_FILL_RECT, 1, 330), 1);		// trips the policy

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	Stats stats = statsOf(queue);
	check(stats.superseded == 0,
		"26: a frame containing a pinned state op is not whole-frame content");
	check(stats.collapses == 1,
		"27: so the bound collapsed rather than drop it silently");
	(void)tags;
}


//! A barrier op protects everything written before it.
static void
testBarrierProtectsOlderFrames()
{
	RemoteFlowQueue queue(7, 1024 * 1024);

	feed(queue, setDrawingMode(1, 0, 400), 1);
	feed(queue, invalidateOne(0, 0, 9, 9, 401), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 410), 1);			// frame 1
	feed(queue, invalidateOne(0, 0, 99, 99, 411), 1);
	feed(queue, tokenlessOp(RP_COPY_RECT_NO_CLIPPING, 420), 1);	// frame 2
	feed(queue, pixelOp(RP_FILL_RECT, 1, 421), 1);
	feed(queue, invalidateOne(0, 0, 99, 99, 422), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 430), 1);			// trips the policy

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	// Frame 1's damage IS covered by frame 2's, so a coverage-only rule would
	// drop it -- and the scroll in frame 2 would then move stale pixels. The
	// barrier rule is the only thing standing between those two facts.
	Stats stats = statsOf(queue);
	check(stats.superseded == 0,
		"28: a frame older than a surface-reading op was not dropped");
	check(queue.ResyncOwed() && stats.collapses == 1,
		"29: it degraded to RP_RESYNC instead");
	(void)tags;
}


//! Coverage by the union of two later frames.
static void
testCoverageByUnion()
{
	RemoteFlowQueue queue(8, 1024 * 1024);

	feed(queue, setDrawingMode(1, 0, 500), 1);
	feed(queue, invalidateOne(0, 0, 9, 9, 501), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 510), 1);			// frame 1
	feed(queue, invalidateOne(0, 0, 99, 99, 511), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 520), 1);			// frame 2: top half
	feed(queue, invalidateOne(0, 0, 99, 49, 521), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 530), 1);			// frame 3: bottom
	feed(queue, invalidateOne(0, 50, 99, 99, 531), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 540), 1);			// trips the policy

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	check(!contains(tags, 510),
		"30: a frame covered by the union of two later frames was dropped");
	check(contains(tags, 520) && contains(tags, 530),
		"31: both covering frames were kept");
	Stats stats = statsOf(queue);
	check(stats.superseded == 2 && stats.collapses == 0,
		"31a: exactly the covered frame went, by supersession not collapse");
}


//! Frames belong to their emitter, not to the stream.
static void
testFramesArePerEmitter()
{
	// Emitter 1 leaves a draw op in flight -- its transaction has not closed, so
	// its damage has not been declared yet. Emitter 2 then produces two complete
	// frames, the second repainting the first. A queue that treated frames as a
	// property of the stream rather than of the emitter would sweep emitter 1's
	// op into emitter 2's first frame and drop it, and nothing would ever
	// repaint it.
	RemoteFlowQueue queue(9, 1024 * 1024);

	feed(queue, setDrawingMode(1, 0, 1600), 1);
	feed(queue, invalidateOne(0, 0, 1, 1, 1601), 1);		// closes 1's frame 0
	feed(queue, setDrawingMode(2, 0, 1602), 2);
	feed(queue, invalidateOne(0, 0, 1, 1, 1603), 2);		// closes 2's frame 0
	feed(queue, pixelOp(RP_FILL_RECT, 1, 1610), 1);			// 1: still in flight
	feed(queue, pixelOp(RP_FILL_RECT, 2, 1620), 2);			// 2's frame 1
	feed(queue, invalidateOne(0, 0, 99, 99, 1621), 2);
	feed(queue, pixelOp(RP_FILL_RECT, 2, 1630), 2);			// 2's frame 2
	feed(queue, invalidateOne(0, 0, 99, 99, 1631), 2);
	feed(queue, pixelOp(RP_FILL_RECT, 2, 1640), 2);			// trips the policy

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);
	Stats stats = statsOf(queue);

	check(contains(tags, 1610),
		"32: an in-flight op from another emitter was not swept into a "
		"superseded frame");
	check(!contains(tags, 1620) && !contains(tags, 1621),
		"32a: the emitter's own superseded frame did go");
	check(stats.superseded == 2,
		"32b: exactly that frame's two messages were superseded");
}


//! Without a known opaque drawing mode, a pixel op may be reading the surface.
static void
testUnknownDrawingModeIsConservative()
{
	RemoteFlowQueue queue(6, 1024 * 1024);

	// No RP_SET_DRAWING_MODE at all this time.
	feed(queue, pixelOp(RP_FILL_RECT, 1, 700), 1);
	feed(queue, invalidateOne(0, 0, 99, 99, 701), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 710), 1);
	feed(queue, invalidateOne(0, 0, 99, 99, 711), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 720), 1);
	feed(queue, invalidateOne(0, 0, 99, 99, 721), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 730), 1);

	check(queue.ResyncOwed(),
		"33: with no known drawing mode nothing is droppable, so it resynced");

	std::vector<uint8> stream;
	drainTags(queue, stream);
	size_t messages = 0;
	check(framingIsWhole(stream, messages),
		"34: a collapse still leaves only whole messages");
}


//! A blend mode makes a pixel op a barrier, so it cannot supersede.
static void
testBlendModeIsABarrier()
{
	RemoteFlowQueue queue(7, 1024 * 1024);

	feed(queue, setDrawingMode(1, 0, 800), 1);			// B_OP_COPY
	feed(queue, invalidateOne(0, 0, 9, 9, 801), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 810), 1);		// frame 1, opaque
	feed(queue, invalidateOne(0, 0, 99, 99, 811), 1);
	feed(queue, setDrawingMode(1, 11, 820), 1);			// B_OP_ALPHA, frame 2
	feed(queue, pixelOp(RP_FILL_RECT, 1, 821), 1);		// now a barrier
	feed(queue, invalidateOne(0, 0, 99, 99, 822), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 830), 1);

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	Stats stats = statsOf(queue);
	check(stats.superseded == 0,
		"35: a frame a blending frame draws over was not dropped");
	(void)tags;
}


//! Non-integral damage is not trusted by the coverage test.
static void
testNonIntegralDamageIsNotTrusted()
{
	RemoteFlowQueue queue(6, 1024 * 1024);

	feed(queue, setDrawingMode(1, 0, 900), 1);
	feed(queue, invalidateOne(0, 0, 9, 9, 901), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 910), 1);
	feed(queue, invalidateOne(0, 0, 99.5f, 99, 911), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 920), 1);
	feed(queue, invalidateOne(0, 0, 99.5f, 99, 921), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 930), 1);

	check(queue.ResyncOwed(),
		"36: damage with a fractional edge was not used as coverage");
}


//! A frame that declares no damage cannot be shown to be repainted.
static void
testFrameWithoutDamageIsNotDropped()
{
	RemoteFlowQueue queue(7, 1024 * 1024);
	queue.SetExplicitBoundaries(true);

	feed(queue, setDrawingMode(1, 0, 1700), 1);
	feed(queue, endFrame(0, 1701), 1);						// closes frame 0
	feed(queue, pixelOp(RP_FILL_RECT, 1, 1710), 1);			// frame 1: painted,
	feed(queue, endFrame(1, 1711), 1);						//   damage unstated
	feed(queue, pixelOp(RP_FILL_RECT, 1, 1720), 1);			// frame 2
	feed(queue, invalidateOne(0, 0, 99, 99, 1721), 1);
	feed(queue, endFrame(2, 1722), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 1730), 1);			// trips the policy

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);
	Stats stats = statsOf(queue);

	check(stats.superseded == 0,
		"33b: a frame that declared no damage was not treated as covered");
	check(queue.ResyncOwed() || contains(tags, 1710),
		"33c: its draw op was not silently lost");
}


// #pragma mark - coalescing


static void
testCoalesceWithinFrame()
{
	RemoteFlowQueue queue(4, 1024 * 1024);

	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1000), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1001), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1002), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1003), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1004), 1);

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	check(!queue.ResyncOwed(),
		"37: redundant setters were coalesced, not resynced away");
	check(tags.size() == 2 && tags[0] == 1003 && tags[1] == 1004,
		"38: a run of redundant setters collapses to its last one");
	Stats stats = statsOf(queue);
	check(stats.coalesced == 3 && stats.superseded == 0,
		"38a: three were coalesced away and nothing was superseded");
}


static void
testCoalesceStopsAtADrawOp()
{
	RemoteFlowQueue queue(4, 1024 * 1024);

	feed(queue, setDrawingMode(1, 0, 1100), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1101), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 1102), 1);		// observes the colour
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1103), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1104), 1);

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	check(contains(tags, 1101) || queue.ResyncOwed(),
		"39: a setter a draw op observed was not coalesced away");
	check(!contains(tags, 1103) || queue.ResyncOwed(),
		"40: the setter after it was coalesced by its successor");
}


static void
testCoalesceDoesNotCrossAFrame()
{
	RemoteFlowQueue queue(4, 1024 * 1024);

	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1200), 1);
	feed(queue, invalidateOne(0, 0, 9, 9, 1201), 1);		// frame boundary
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1202), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1203), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1204), 1);

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	check(contains(tags, 1200) || queue.ResyncOwed(),
		"41: coalescing did not reach back across a frame boundary");
}


static void
testCoalesceRespectsTokens()
{
	RemoteFlowQueue queue(4, 1024 * 1024);

	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 1, 1300), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 2, 1301), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 3, 1302), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 4, 1303), 1);
	feed(queue, pixelOp(RP_SET_HIGH_COLOR, 5, 1304), 1);

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	check(queue.ResyncOwed(),
		"42: setters for different tokens are not each other's successors");
}


// #pragma mark - the bound


static void
testBoundIsHonoured()
{
	const size_t maxMessages = 16;
	const size_t maxBytes = 2048;
	RemoteFlowQueue queue(maxMessages, maxBytes);

	size_t largest = 0;
	bool withinMessages = true;
	bool withinBytes = true;

	for (uint32 i = 0; i < 500; i++) {
		std::vector<uint8> message = pixelOp(RP_FILL_RECT, 1, 2000 + i);
		if (message.size() > largest)
			largest = message.size();

		feed(queue, message, 1);

		if (queue.CountMessages() > maxMessages)
			withinMessages = false;
		if (queue.CountBytes() > maxBytes + largest)
			withinBytes = false;
	}

	check(withinMessages, "43: the message bound was never exceeded");
	check(withinBytes,
		"44: the byte bound was never exceeded by more than one message");
	check(queue.ResyncOwed(),
		"45: reaching the bound with nothing droppable owes a resync");
}


static void
testOversizedMessageIsNotSplit()
{
	RemoteFlowQueue queue(16, 64);

	Message big(RP_FILL_RECT);
	big.Put32(1);
	for (int i = 0; i < 200; i++)
		big.Put32(0xdeadbeef);
	big.Put32(3000);
	std::vector<uint8> message = big.Bytes();

	check(feed(queue, message, 1) == B_OK,
		"46: a message larger than the whole bound is still accepted");
	check(queue.CountMessages() == 1, "47: it is stored as one message");

	size_t length = 0;
	const uint8* data = queue.PeekFront(length);
	check(data != NULL && length == message.size(),
		"48: it comes back at its full length, not truncated to the bound");
	check(data != NULL && memcmp(data, &message[0], message.size()) == 0,
		"49: byte for byte what went in");
}


static void
testQuietSessionIsInvisible()
{
	RemoteFlowQueue queue;

	feed(queue, setDrawingMode(1, 0, 4000), 1);
	for (uint32 i = 0; i < 1000; i++) {
		feed(queue, pixelOp(RP_FILL_RECT, 1, 4100 + i), 1);
		if ((i % 10) == 9)
			feed(queue, invalidateOne(0, 0, 99, 99, 4200 + i), 1);
	}

	check(!queue.ResyncOwed(),
		"50: a session inside the bound never triggers the policy");

	uint64 enqueued, drained, coalesced, superseded, collapses;
	queue.GetStatistics(enqueued, drained, coalesced, superseded, collapses);
	check(coalesced == 0 && superseded == 0 && collapses == 0,
		"51: and nothing was coalesced, superseded or collapsed");

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);
	check(tags.size() == 1101, "52: every message came back out");

	size_t messages = 0;
	check(framingIsWhole(stream, messages) && messages == 1101,
		"53: as 1101 whole messages");
}


// #pragma mark - framing, and the instrument's own control


static void
testNoPartialMessageEver()
{
	// Overflow hard, from a mixture the policy cannot fully resolve, and check
	// that whatever survives is still whole messages -- with a frame left open
	// (no closing invalidate) at the moment the bound is hit.
	RemoteFlowQueue queue(12, 4096);

	feed(queue, setDrawingMode(1, 0, 5000), 1);
	for (uint32 i = 0; i < 300; i++) {
		feed(queue, pixelOp(RP_FILL_RECT, 1, 5100 + i), 1);
		if ((i % 7) == 6)
			feed(queue, invalidateOne(0, 0, 99, 99, 5500 + i), 1);
		if ((i % 23) == 22)
			feed(queue, pixelOp(RP_DRAW_STRING, 1, 5900 + i), 1);
		if ((i % 31) == 30)
			feed(queue, tokenlessOp(RP_COPY_RECT_NO_CLIPPING, 6300 + i), 1);

		// Mid-frame: the last message enqueued is a draw op, not a boundary.
		std::vector<uint8> partialStream;
		size_t length = 0;
		const uint8* data = queue.PeekFront(length);
		if (data != NULL) {
			uint32 declared = 0;
			memcpy(&declared, data + 2, 4);
			if (declared != length) {
				check(false,
					"53a: a queued message's length field matches it");
				return;
			}
		}
	}

	std::vector<uint8> stream;
	drainTags(queue, stream);

	size_t messages = 0;
	check(framingIsWhole(stream, messages),
		"54: after 300 overflow rounds the output is still whole messages");
	check(messages > 0, "55: and there was something left to walk");
}


static void
testFramingInstrumentCanFail()
{
	// The positive control for check 54. If the walker cannot detect a partial
	// message, "no partial messages observed" means nothing.
	std::vector<uint8> stream;
	std::vector<uint8> one = pixelOp(RP_FILL_RECT, 1, 7000);
	std::vector<uint8> two = pixelOp(RP_FILL_RECT, 1, 7001);
	stream.insert(stream.end(), one.begin(), one.end());
	stream.insert(stream.end(), two.begin(), two.end());

	size_t messages = 0;
	check(framingIsWhole(stream, messages) && messages == 2,
		"56: the framing walker accepts a whole stream");

	std::vector<uint8> truncated = stream;
	truncated.pop_back();
	check(!framingIsWhole(truncated, messages),
		"57: POSITIVE CONTROL -- it rejects a stream truncated by one byte");

	std::vector<uint8> headerOnly(stream.begin(), stream.begin() + 6);
	headerOnly[2] = 0xff;
	check(!framingIsWhole(headerOnly, messages),
		"58: POSITIVE CONTROL -- it rejects a header promising absent bytes");

	std::vector<uint8> shortLength = stream;
	shortLength[2] = 3;
	check(!framingIsWhole(shortLength, messages),
		"59: POSITIVE CONTROL -- it rejects a length below the header size");
}


static void
testFrameBoundaryMode()
{
	RemoteFlowQueue implicitQueue;
	check(!implicitQueue.ExplicitBoundaries(),
		"60: boundaries are inferred until the client asks for them");
	check(implicitQueue.FrameIsClose(RP_INVALIDATE_REGION),
		"61: without the capability, an invalidate closes the frame");
	check(!implicitQueue.FrameIsClose(RP_TIER_END_FRAME),
		"62: and RP_TIER_END_FRAME does not");

	RemoteFlowQueue explicitQueue;
	explicitQueue.SetExplicitBoundaries(true);
	check(explicitQueue.FrameIsClose(RP_TIER_END_FRAME),
		"63: with the capability, RP_TIER_END_FRAME closes the frame");
	check(!explicitQueue.FrameIsClose(RP_INVALIDATE_REGION),
		"64: and the invalidate no longer does");
}


static void
testSupersedeWithExplicitBoundaries()
{
	RemoteFlowQueue queue(8, 1024 * 1024);
	queue.SetExplicitBoundaries(true);

	feed(queue, setDrawingMode(1, 0, 8000), 1);
	feed(queue, endFrame(0, 8001), 1);						// closes frame 0
	feed(queue, pixelOp(RP_FILL_RECT, 1, 8010), 1);			// frame 1
	feed(queue, invalidateOne(0, 0, 99, 99, 8011), 1);
	feed(queue, endFrame(1, 8012), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 8020), 1);			// frame 2
	feed(queue, invalidateOne(0, 0, 99, 99, 8021), 1);
	feed(queue, endFrame(2, 8022), 1);
	feed(queue, pixelOp(RP_FILL_RECT, 1, 8030), 1);			// trips the policy

	std::vector<uint8> stream;
	std::vector<uint32> tags = drainTags(queue, stream);

	check(!contains(tags, 8010) && !contains(tags, 8011)
			&& !contains(tags, 8012),
		"65: RP_TIER_END_FRAME delimits a frame that supersession can drop");
	check(contains(tags, 8020) && contains(tags, 8022),
		"66: and the covering frame is kept");
}


static void
testResetClearsEverything()
{
	RemoteFlowQueue queue(4, 512);
	for (uint32 i = 0; i < 50; i++)
		feed(queue, pixelOp(RP_FILL_RECT, 1, 9000 + i), 1);

	check(queue.ResyncOwed(), "67: the run overflowed as designed");

	queue.Reset();
	check(queue.IsEmpty() && queue.CountBytes() == 0,
		"68: Reset() empties the queue");
	check(!queue.ResyncOwed(),
		"69: and clears the owed resync, because the next connection replays");
	check(!queue.ExplicitBoundaries(),
		"70: and forgets the negotiated frame boundary");
}


int
main()
{
	testClassification();
	testSupersedeCovered();
	testSupersedeNotCovered();
	testStateOpPinsItsFrame();
	testBarrierProtectsOlderFrames();
	testCoverageByUnion();
	testFramesArePerEmitter();
	testFrameWithoutDamageIsNotDropped();
	testUnknownDrawingModeIsConservative();
	testBlendModeIsABarrier();
	testNonIntegralDamageIsNotTrusted();
	testCoalesceWithinFrame();
	testCoalesceStopsAtADrawOp();
	testCoalesceDoesNotCrossAFrame();
	testCoalesceRespectsTokens();
	testBoundIsHonoured();
	testOversizedMessageIsNotSplit();
	testQuietSessionIsInvisible();
	testNoPartialMessageEver();
	testFramingInstrumentCanFail();
	testFrameBoundaryMode();
	testSupersedeWithExplicitBoundaries();
	testResetClearsEverything();

	printf("%s  %d checks, %d failures\n", sFailures == 0 ? "PASS" : "FAIL",
		sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
