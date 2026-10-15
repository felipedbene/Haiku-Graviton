/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 *
 * Unit test for the RP_FRAME_ACK round-trip estimator (RemoteAckEstimator).
 *
 * The estimator is pure arithmetic over a synthetic ack stream, so it is tested
 * directly rather than inferred from a running desktop -- which matters here
 * more than usual: the two properties it exists for (an RTT that does not
 * include the client's held time, and a backpressure signal that an idle screen
 * cannot raise) are both invisible on a LAN with a well-behaved client, the
 * only configuration hardware testing offers for free.
 *
 * Build and run:  ./run.sh
 */

#include "RemoteAckEstimator.h"
#include "RemoteProtocol.h"

#include <stdio.h>


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


// All times µs, as system_time() reports them.
static const bigtime_t kMillisecond = 1000;


struct Stats {
	uint64	sent;
	uint64	accepted;
	uint64	unknown;
	uint64	implausible;
	uint64	forgotten;
};


static Stats
statsOf(const RemoteAckEstimator& estimator)
{
	Stats stats;
	estimator.GetStatistics(stats.sent, stats.accepted, stats.unknown,
		stats.implausible, stats.forgotten);
	return stats;
}


/*!	One synthetic round trip: the server emits frame \a frameId at \a sendTime,
	the client holds it for \a heldMillis, and the ack comes back after
	\a linkDelay of total network time. Returns what NoteAck() made of it.
*/
static RemoteAckEstimator::ack_result
roundTrip(RemoteAckEstimator& estimator, uint32 frameId, bigtime_t sendTime,
	bigtime_t linkDelay, uint16 heldMillis, uint16 decodeMillis = 0,
	uint8 queueDepth = 0)
{
	estimator.NoteFrameSent(frameId, sendTime);
	bigtime_t ackTime = sendTime + linkDelay + (bigtime_t)heldMillis
		* kMillisecond;
	return estimator.NoteAck(frameId, decodeMillis, queueDepth, heldMillis,
		ackTime);
}


// #pragma mark - the wire contract


static void
testOpcodeIsWhereThePayloadSaysItIs()
{
	// The payload documented in RemoteProtocol.h is keyed to this value, and a
	// client that was built against it is on the wire; renumbering the Tier P
	// block would silently reinterpret its acks as some other message.
	check(RP_TIER_BEGIN_FRAME == 280, "01: Tier P block still starts at 280");
	check(RP_FRAME_ACK == 284, "02: RP_FRAME_ACK is still opcode 284");

	// The field widths the payload is specified in. A wider field would move
	// held_ms, and a narrower held_ms would silently wrap a clamped client's
	// one-second hold.
	check(sizeof(uint32) + sizeof(uint16) + sizeof(uint8) + sizeof(uint16) == 9,
		"03: the RP_FRAME_ACK payload is 9 packed bytes");
}


// #pragma mark - the held-time correction


static void
testRoundTripIgnoresHeldTime()
{
	// The headline property: the same 8 ms link, answered by clients that sat on
	// the frame for wildly different times -- a prompt one, one on a 16 ms
	// timer, one whose tab is backgrounded and clamped to a second, one worse
	// still. The estimate must be the link, every time.
	static const bigtime_t kLink = 8 * kMillisecond;
	static const uint16 kHeld[] = { 0, 1, 16, 250, 1000, 3000 };

	RemoteAckEstimator estimator;
	for (uint32 i = 0; i < sizeof(kHeld) / sizeof(kHeld[0]); i++) {
		bigtime_t sendTime = 1000000 + (bigtime_t)i * 5000000;
		RemoteAckEstimator::ack_result result = roundTrip(estimator, 100 + i,
			sendTime, kLink, kHeld[i]);
		check(result == RemoteAckEstimator::ACK_ACCEPTED,
			"04: every well-formed ack is accepted");
	}

	check(estimator.HasEstimate(), "05: a usable estimate exists");
	check(estimator.RoundTripTime() == kLink,
		"06: the smoothed RTT is the injected link delay, not the held time");
	check(estimator.LastRoundTripTime() == kLink,
		"07: the last sample is the injected link delay");
	check(estimator.MinimumRoundTripTime() == kLink,
		"08: the minimum is the injected link delay");

	// And the number the naive estimator would have produced, for contrast: the
	// last of those acks arrived 3008 ms after its frame was sent.
	check(kLink + 3000 * kMillisecond != estimator.LastRoundTripTime(),
		"09: the uncorrected interval is not what was reported");
}


static void
testBackgroundedTabReportsItsLink()
{
	// The scenario from the rationale, end to end: a 2 ms LAN and a browser tab
	// whose timers the browser clamped to one second.
	RemoteAckEstimator estimator;
	roundTrip(estimator, 1, 0, 2 * kMillisecond, 1000);

	check(estimator.RoundTripTime() == 2 * kMillisecond,
		"10: a clamped client on a 2 ms link reports 2 ms, not 1002 ms");
	check(!estimator.BackpressureSignalled(),
		"11: one frame answered slowly is not backpressure");
}


static void
testDecodeTimeIsNotSubtractedTwice()
{
	// decode_ms is inside held_ms, so varying it must not move the estimate.
	// Subtracting both would bias every sample low by the decode cost.
	RemoteAckEstimator first;
	roundTrip(first, 1, 0, 10 * kMillisecond, 40, 0);

	RemoteAckEstimator second;
	roundTrip(second, 1, 0, 10 * kMillisecond, 40, 40);

	check(first.RoundTripTime() == 10 * kMillisecond
			&& second.RoundTripTime() == 10 * kMillisecond,
		"12: decode_ms is reported, not subtracted");
	check(second.LastDecodeMillis() == 40,
		"13: decode_ms is recorded for the pacer");
}


static void
testClientReportsAreRecorded()
{
	RemoteAckEstimator estimator;
	roundTrip(estimator, 1, 0, kMillisecond, 7, 5, 3);

	check(estimator.LastDecodeMillis() == 5 && estimator.LastQueueDepth() == 3
			&& estimator.LastHeldMillis() == 7,
		"14: the client's own costs are kept as reported");
}


static void
testImplausibleHeldTimeIsDiscarded()
{
	RemoteAckEstimator estimator;
	roundTrip(estimator, 1, 0, 4 * kMillisecond, 0);
	check(estimator.RoundTripTime() == 4 * kMillisecond,
		"15: a first good sample sets the estimate");

	// Frame 2 goes out at 10 s and is acked 5 ms later, with the client
	// claiming to have held it for a minute.
	estimator.NoteFrameSent(2, 10000000);
	RemoteAckEstimator::ack_result result = estimator.NoteAck(2, 0, 0, 60000,
		10000000 + 5 * kMillisecond);

	check(result == RemoteAckEstimator::ACK_HELD_IMPLAUSIBLE,
		"16: a held time longer than the whole interval is rejected");
	check(estimator.RoundTripTime() == 4 * kMillisecond,
		"17: the rejected sample leaves the previous estimate standing");
	check(estimator.OutstandingFrames() == 0,
		"18: the frame is still retired -- the ack did arrive");
	check(statsOf(estimator).implausible == 1,
		"19: the rejection is counted");
}


static void
testLongHoldInRange()
{
	// held_ms is a uint16, so 65535 ms of holding is representable and must be
	// converted to µs without wrapping.
	RemoteAckEstimator estimator;
	roundTrip(estimator, 1, 0, 3 * kMillisecond, 65535);

	check(estimator.RoundTripTime() == 3 * kMillisecond,
		"20: a 65.5 s hold is subtracted, not wrapped");
}


static void
testSmoothingTracksTheLink()
{
	// A jittery link: the smoothed estimate must sit inside the range of the
	// samples and the minimum must find the floor.
	RemoteAckEstimator estimator;
	static const bigtime_t kSamples[] = { 20, 22, 19, 60, 21, 20, 23 };

	for (uint32 i = 0; i < sizeof(kSamples) / sizeof(kSamples[0]); i++) {
		roundTrip(estimator, i + 1, (bigtime_t)i * 1000000,
			kSamples[i] * kMillisecond, (uint16)(i * 100));
	}

	check(estimator.MinimumRoundTripTime() == 19 * kMillisecond,
		"21: the minimum is the smallest corrected sample");
	check(estimator.RoundTripTime() >= 19 * kMillisecond
			&& estimator.RoundTripTime() <= 26 * kMillisecond,
		"22: one outlier moves the smoothed estimate by about an eighth");
	check(estimator.LastRoundTripTime() == 23 * kMillisecond,
		"23: the last sample is reported unsmoothed");
}


// #pragma mark - backpressure counted in frames sent


static void
testIdleScreenRaisesNoSignal()
{
	// The acceptance criterion: zero frames sent produces no signal, whatever
	// the clock says and whatever the frame ids have done. Damage-gated capture
	// sends nothing for a still screen, so this is the common case, not the
	// corner one.
	RemoteAckEstimator estimator;

	check(estimator.OutstandingFrames() == 0 && !estimator.BackpressureSignalled(),
		"24: a connection that has sent nothing is not backpressured");

	// Ten seconds of a still screen later, the frame sequence has moved on by
	// 50000 (frames composed for other connections, and frames the flow queue
	// superseded, all consume ids) and the client has acked none of them --
	// because it was sent none of them.
	check(!estimator.BackpressureSignalled(),
		"25: elapsed time alone cannot raise the signal");

	RemoteAckEstimator::ack_result result = estimator.NoteAck(50000, 0, 0, 0,
		10000000);
	check(result == RemoteAckEstimator::ACK_UNKNOWN_FRAME,
		"26: an ack for a frame this connection never sent is not timed");
	check(!estimator.HasEstimate(),
		"27: and contributes no RTT sample");
	check(estimator.OutstandingFrames() == 0 && !estimator.BackpressureSignalled(),
		"28: a frame-id gap of 50000 is not 50000 frames owed");
	check(statsOf(estimator).unknown == 1, "29: the stray ack is counted");
}


static void
testOutstandingCountsFramesNotIds()
{
	// A client that attaches when the interface is already at frame 50000: the
	// ids are huge, the number of frames it owes is not.
	RemoteAckEstimator estimator;
	estimator.NoteFrameSent(50000, 0);
	estimator.NoteFrameSent(50001, 16000);

	check(estimator.OutstandingFrames() == 2,
		"30: outstanding is the count of frames sent, whatever the ids are");
	check(!estimator.BackpressureSignalled(),
		"31: two frames in flight is a healthy pipeline");

	estimator.NoteFrameSent(50002, 32000);
	check(estimator.BackpressureSignalled(),
		"32: a third unanswered frame is the signal");

	estimator.NoteAck(50002, 0, 0, 0, 34000);
	check(estimator.OutstandingFrames() == 0 && !estimator.BackpressureSignalled(),
		"33: an ack clears the signal");
}


static void
testAckIsCumulative()
{
	RemoteAckEstimator estimator;
	for (uint32 i = 1; i <= 4; i++)
		estimator.NoteFrameSent(i, (bigtime_t)i * 16000);

	// The client answers frame 3. Frames 1 and 2 cannot have failed to arrive on
	// an ordered stream, and frame 2 may be one the flow queue superseded and
	// that nothing will ever ack -- so both are retired here or never.
	RemoteAckEstimator::ack_result result = estimator.NoteAck(3, 0, 0, 0,
		48000 + 5000);
	check(result == RemoteAckEstimator::ACK_ACCEPTED,
		"34: the ack is accepted");
	check(estimator.OutstandingFrames() == 1,
		"35: an ack retires every frame sent before it as well");
	check(estimator.LastRoundTripTime() == 5000,
		"36: the sample is timed against the acked frame, not the oldest");
}


static void
testDuplicateAckChangesNothing()
{
	RemoteAckEstimator estimator;
	estimator.NoteFrameSent(1, 0);
	estimator.NoteFrameSent(2, 16000);
	estimator.NoteAck(1, 0, 0, 0, 3000);

	bigtime_t estimate = estimator.RoundTripTime();
	RemoteAckEstimator::ack_result result = estimator.NoteAck(1, 0, 0, 0,
		90000);

	check(result == RemoteAckEstimator::ACK_UNKNOWN_FRAME,
		"37: a repeated ack names a frame no longer in flight");
	check(estimator.RoundTripTime() == estimate,
		"38: and does not move the estimate");
	check(estimator.OutstandingFrames() == 1,
		"39: nor retire the frame that really is still owed");
}


static void
testRingIsBounded()
{
	RemoteAckEstimator estimator;
	const uint32 count = (uint32)RemoteAckEstimator::kMaxTracked + 10;
	for (uint32 i = 1; i <= count; i++)
		estimator.NoteFrameSent(i, (bigtime_t)i * 16000);

	check(estimator.OutstandingFrames() == RemoteAckEstimator::kMaxTracked,
		"40: the frames in flight saturate at the ring size");
	check(statsOf(estimator).forgotten == 10,
		"41: the frames forgotten to make room are counted");
	check(estimator.BackpressureSignalled(),
		"42: a saturated ring is emphatically backpressure");

	// The oldest frames are gone, so their acks can no longer be timed...
	check(estimator.NoteAck(1, 0, 0, 0, 16000 + 1000)
			== RemoteAckEstimator::ACK_UNKNOWN_FRAME,
		"43: a forgotten frame's ack is not timed");

	// ...but the newest is still there, and retires everything.
	check(estimator.NoteAck(count, 0, 0, 0, (bigtime_t)count * 16000 + 1000)
			== RemoteAckEstimator::ACK_ACCEPTED,
		"44: the newest frame is still timed");
	check(estimator.OutstandingFrames() == 0,
		"45: and its ack retires the whole ring");
}


static void
testResetIsAConnectionBoundary()
{
	RemoteAckEstimator estimator;
	roundTrip(estimator, 1, 0, 9 * kMillisecond, 10, 4, 2);
	estimator.NoteFrameSent(2, 100000);
	estimator.NoteFrameSent(3, 116000);

	estimator.Reset();

	check(estimator.OutstandingFrames() == 0,
		"46: a new connection owes nothing");
	check(!estimator.HasEstimate() && estimator.RoundTripTime() == 0
			&& estimator.MinimumRoundTripTime() == 0,
		"47: and the departed link's RTT is not evidence about it");
	check(estimator.LastDecodeMillis() == 0 && estimator.LastQueueDepth() == 0
			&& estimator.LastHeldMillis() == 0,
		"48: nor the departed client's reported costs");

	Stats stats = statsOf(estimator);
	check(stats.sent == 3 && stats.accepted == 1,
		"49: the lifetime counters survive the boundary");

	// An ack that was in flight when the connection ended names a frame the new
	// connection never sent.
	check(estimator.NoteAck(3, 0, 0, 0, 200000)
			== RemoteAckEstimator::ACK_UNKNOWN_FRAME,
		"50: a stale ack from the previous connection is ignored");
}


int
main()
{
	testOpcodeIsWhereThePayloadSaysItIs();

	testRoundTripIgnoresHeldTime();
	testBackgroundedTabReportsItsLink();
	testDecodeTimeIsNotSubtractedTwice();
	testClientReportsAreRecorded();
	testImplausibleHeldTimeIsDiscarded();
	testLongHoldInRange();
	testSmoothingTracksTheLink();

	testIdleScreenRaisesNoSignal();
	testOutstandingCountsFramesNotIds();
	testAckIsCumulative();
	testDuplicateAckChangesNothing();
	testRingIsBounded();
	testResetIsAConnectionBoundary();

	printf("%s  %d checks, %d failures\n", sFailures == 0 ? "PASS" : "FAIL",
		sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
