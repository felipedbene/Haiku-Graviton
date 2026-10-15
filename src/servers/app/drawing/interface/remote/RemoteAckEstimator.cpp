/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */

#include "RemoteAckEstimator.h"

#include <string.h>


RemoteAckEstimator::RemoteAckEstimator()
	:
	fTrackedCount(0),
	fHasEstimate(false),
	fRoundTrip(0),
	fLastRoundTrip(0),
	fMinimumRoundTrip(0),
	fLastDecodeMillis(0),
	fLastQueueDepth(0),
	fLastHeldMillis(0),
	fSent(0),
	fAccepted(0),
	fUnknown(0),
	fImplausible(0),
	fForgotten(0)
{
	memset(fTracked, 0, sizeof(fTracked));
}


void
RemoteAckEstimator::Reset()
{
	fTrackedCount = 0;
	fHasEstimate = false;
	fRoundTrip = 0;
	fLastRoundTrip = 0;
	fMinimumRoundTrip = 0;
	fLastDecodeMillis = 0;
	fLastQueueDepth = 0;
	fLastHeldMillis = 0;
}


void
RemoteAckEstimator::NoteFrameSent(uint32 frameId, bigtime_t when)
{
	fSent++;

	if (fTrackedCount == kMaxTracked) {
		// Forget the oldest. It is the one whose ack is least likely to still
		// arrive, and dropping it costs only that frame's timing -- never the
		// outstanding count's direction, which is what the signal reads.
		memmove(fTracked, fTracked + 1,
			(kMaxTracked - 1) * sizeof(sent_frame));
		fTrackedCount--;
		fForgotten++;
	}

	fTracked[fTrackedCount].id = frameId;
	fTracked[fTrackedCount].when = when;
	fTrackedCount++;
}


RemoteAckEstimator::ack_result
RemoteAckEstimator::NoteAck(uint32 frameId, uint16 decodeMillis,
	uint8 queueDepth, uint16 heldMillis, bigtime_t when)
{
	// Recorded whatever the sample turns out to be: these are the client's
	// report of its own cost, and a report that comes with an unusable link
	// sample is still the client's report.
	fLastDecodeMillis = decodeMillis;
	fLastQueueDepth = queueDepth;
	fLastHeldMillis = heldMillis;

	int slot = _IndexOf(frameId);
	if (slot < 0) {
		// Not in flight. Retiring something on the strength of an id we never
		// sent would let a duplicate or a stale ack from the previous
		// connection clear frames the current client really does owe.
		fUnknown++;
		return ACK_UNKNOWN_FRAME;
	}

	bigtime_t elapsed = when - fTracked[slot].when;
	bigtime_t held = (bigtime_t)heldMillis * 1000;

	_Retire(slot);

	// The client claims to have held the frame for longer than the whole
	// interval between the send and the ack. One of the two clocks is wrong,
	// and there is no way to tell which from here. Clamping to zero would
	// report a zero-latency link, so report nothing.
	if (held > elapsed) {
		fImplausible++;
		return ACK_HELD_IMPLAUSIBLE;
	}

	bigtime_t sample = elapsed - held;

	fLastRoundTrip = sample;
	if (!fHasEstimate) {
		fHasEstimate = true;
		fRoundTrip = sample;
		fMinimumRoundTrip = sample;
	} else {
		// Integer EWMA with gain 1/kRoundTripGain, in the shape TCP's smoothed
		// RTT uses. The truncation leaves the estimate able to settle up to
		// kRoundTripGain µs short of a moved target, which is noise next to any
		// link this measures.
		fRoundTrip += (sample - fRoundTrip) / kRoundTripGain;
		if (sample < fMinimumRoundTrip)
			fMinimumRoundTrip = sample;
	}

	fAccepted++;
	return ACK_ACCEPTED;
}


void
RemoteAckEstimator::GetStatistics(uint64& _sent, uint64& _accepted,
	uint64& _unknown, uint64& _implausible, uint64& _forgotten) const
{
	_sent = fSent;
	_accepted = fAccepted;
	_unknown = fUnknown;
	_implausible = fImplausible;
	_forgotten = fForgotten;
}


int
RemoteAckEstimator::_IndexOf(uint32 frameId) const
{
	for (size_t i = 0; i < fTrackedCount; i++) {
		if (fTracked[i].id == frameId)
			return (int)i;
	}

	return -1;
}


/*!	Drops the frames in flight up to and including \a index.

	Cumulative, which an ordered reliable stream permits: the client cannot have
	received frame N without having received everything emitted before it, so an
	ack for N is evidence about all of them. It is also the only thing that
	retires a frame whose bytes the flow queue superseded -- that frame is never
	acked itself and would otherwise be counted as owed for the rest of the
	connection.
*/
void
RemoteAckEstimator::_Retire(int index)
{
	size_t remaining = fTrackedCount - (size_t)index - 1;
	if (remaining > 0)
		memmove(fTracked, fTracked + index + 1, remaining * sizeof(sent_frame));

	fTrackedCount = remaining;
}
