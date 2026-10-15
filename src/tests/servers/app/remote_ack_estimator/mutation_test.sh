#!/bin/sh
# Mutation test for the RP_FRAME_ACK estimator.
#
# A green unit test proves nothing on its own: it may be agreeing with itself.
# This script breaks each rule the estimator exists for, one at a time, in a
# throwaway copy of RemoteAckEstimator.{h,cpp}, and requires the unit test to go
# RED for every one. A mutation that survives is printed as SURVIVED and the
# script exits non-zero -- that is a hole in the test, not a pass.
#
# The two rules the acceptance criteria name are the first mutants here: stop
# subtracting held_ms (M1), and read the signal off frame ids instead of frames
# sent (M16).
set -e

here=$(cd "$(dirname "$0")" && pwd)
remote=$here/../../../../servers/app/drawing/interface/remote
work=${OUT:-$here/build}/mutants
mkdir -p "$work"

survived=0
killed=0

# Both the header and the implementation are copied, and the copy's directory
# comes first on the include path: some of the rules below (the backpressure
# threshold, the signal's input) live in inline accessors in the header.
mutate() {
	name=$1
	file=$2
	from=$3
	to=$4

	rm -rf "$work/$name"
	mkdir -p "$work/$name"
	cp "$remote/RemoteAckEstimator.h" "$work/$name/RemoteAckEstimator.h"
	cp "$remote/RemoteAckEstimator.cpp" "$work/$name/RemoteAckEstimator.cpp"

	printf '%s' "$from" > "$work/$name/from.txt"
	printf '%s' "$to" > "$work/$name/to.txt"
	python3 "$here/mutate.py" "$work/$name/$file" \
		"$work/$name/from.txt" "$work/$name/to.txt"

	echo "=== $name"
	if ! ${CXX:-g++} -std=c++11 -w -O1 -DREMOTE_ACK_ESTIMATOR_TEST \
			-I"$work/$name" -I"$here" -I"$remote" \
			"$here/RemoteAckEstimatorTest.cpp" \
			"$work/$name/RemoteAckEstimator.cpp" \
			-o "$work/$name/test" 2> "$work/$name/build.log"; then
		echo "KILLED (did not compile)"
		killed=$((killed + 1))
		return
	fi

	if "$work/$name/test" > "$work/$name/out.txt" 2>&1; then
		echo "SURVIVED -- the test passes with this rule broken"
		tail -1 "$work/$name/out.txt"
		survived=$((survived + 1))
	else
		sed -n '1,6p' "$work/$name/out.txt"
		tail -1 "$work/$name/out.txt"
		killed=$((killed + 1))
	fi
}

# M1 -- the headline rule: do not subtract the client's held time, i.e. report
# the naive "ack arrived minus frame sent" interval as the link's RTT.
mutate ignore-held-time RemoteAckEstimator.cpp \
'	bigtime_t sample = elapsed - held;' \
'	bigtime_t sample = elapsed;'

# M2 -- subtract decode_ms as well, double-counting the decode that held_ms
# already contains and biasing every estimate low.
mutate subtract-decode-twice RemoteAckEstimator.cpp \
'	bigtime_t sample = elapsed - held;' \
'	bigtime_t sample = elapsed - held - (bigtime_t)decodeMillis * 1000;'

# M3 -- clamp an implausible held_ms instead of discarding the sample, which
# folds a fabricated zero-latency measurement into the estimate.
mutate clamp-implausible-held RemoteAckEstimator.cpp \
'	if (held > elapsed) {
		fImplausible++;
		return ACK_HELD_IMPLAUSIBLE;
	}' \
'	if (held > elapsed) {
		held = elapsed;
	}'

# M4 -- subtract held_ms in the wrong unit (milliseconds against a µs clock),
# which is subtracting approximately nothing.
mutate held-in-milliseconds RemoteAckEstimator.cpp \
'	bigtime_t held = (bigtime_t)heldMillis * 1000;' \
'	bigtime_t held = (bigtime_t)heldMillis;'

# M5 -- convert held_ms in 16-bit arithmetic, so a clamped client's long hold
# wraps and the subtraction removes the wrong amount.
mutate held-conversion-wraps RemoteAckEstimator.cpp \
'	bigtime_t held = (bigtime_t)heldMillis * 1000;' \
'	bigtime_t held = (bigtime_t)(uint16)(heldMillis * 1000);'

# M6 -- trust any ack id: an id that is not in flight retires the newest frame
# instead of being refused, so a duplicate or a stale ack clears real debt.
mutate trust-any-ack-id RemoteAckEstimator.cpp \
'	for (size_t i = 0; i < fTrackedCount; i++) {
		if (fTracked[i].id == frameId)
			return (int)i;
	}

	return -1;' \
'	for (size_t i = 0; i < fTrackedCount; i++) {
		if (fTracked[i].id == frameId)
			return (int)i;
	}

	return fTrackedCount > 0 ? (int)fTrackedCount - 1 : -1;'

# M7 -- retire only the acked frame, not its predecessors. A frame whose bytes
# the flow queue superseded is then owed for the rest of the connection.
mutate retire-only-the-acked-frame RemoteAckEstimator.cpp \
'	size_t remaining = fTrackedCount - (size_t)index - 1;
	if (remaining > 0)
		memmove(fTracked, fTracked + index + 1, remaining * sizeof(sent_frame));

	fTrackedCount = remaining;' \
'	size_t remaining = fTrackedCount - (size_t)index - 1;
	if (remaining > 0)
		memmove(fTracked + index, fTracked + index + 1,
			remaining * sizeof(sent_frame));

	fTrackedCount--;'

# M8 -- time the sample against the oldest frame in flight rather than the one
# the ack names, which reports the pipeline depth as link delay.
mutate time-against-the-oldest-frame RemoteAckEstimator.cpp \
'	bigtime_t elapsed = when - fTracked[slot].when;' \
'	bigtime_t elapsed = when - fTracked[0].when;'

# M9 -- the minimum never falls below the first sample.
mutate minimum-never-falls RemoteAckEstimator.cpp \
'		if (sample < fMinimumRoundTrip)
			fMinimumRoundTrip = sample;' \
'		if (false)
			fMinimumRoundTrip = sample;'

# M10 -- a new connection inherits the previous one's frames in flight, so it
# starts life owing frames it was never sent.
mutate reset-keeps-frames-in-flight RemoteAckEstimator.cpp \
'	fTrackedCount = 0;
	fHasEstimate = false;' \
'	fHasEstimate = false;'

# M11 -- a new connection inherits the departed link's RTT as a measurement of
# its own.
mutate reset-keeps-the-estimate RemoteAckEstimator.cpp \
'	fHasEstimate = false;
	fRoundTrip = 0;
	fLastRoundTrip = 0;
	fMinimumRoundTrip = 0;' \
'	fLastRoundTrip = 0;'

# M12 -- a new connection inherits the departed client's reported costs.
mutate reset-keeps-the-client-report RemoteAckEstimator.cpp \
'	fLastDecodeMillis = 0;
	fLastQueueDepth = 0;
	fLastHeldMillis = 0;' \
'	fLastDecodeMillis = 0;'

# M13 -- the lifetime counters are zeroed at the connection boundary, so the
# statistics can no longer show how often acks were refused.
mutate reset-clears-the-lifetime-counters RemoteAckEstimator.cpp \
'	fTrackedCount = 0;
	fHasEstimate = false;' \
'	fSent = 0;
	fAccepted = 0;
	fTrackedCount = 0;
	fHasEstimate = false;'

# M14 -- the client's own reported cost is not recorded, so the pacer that
# lands with Tier P would read a held time of zero from every client.
mutate drop-the-client-report RemoteAckEstimator.cpp \
'	fLastHeldMillis = heldMillis;' \
'	fLastHeldMillis = 0;'

# M15 -- the backpressure threshold is set so high that no session reaches it.
mutate backpressure-threshold-raised RemoteAckEstimator.h \
'kBackpressureFrames = 3;' \
'kBackpressureFrames = 50;'

# M16 -- the second acceptance rule: read the signal off the frame *ids* rather
# than the count of frames sent. A client that attaches when the interface is
# already at frame 50000 is then permanently backpressured.
mutate signal-from-frame-ids RemoteAckEstimator.h \
'			bool				BackpressureSignalled() const
									{ return OutstandingFrames()
										>= kBackpressureFrames; }' \
'			bool				BackpressureSignalled() const
									{ return fTrackedCount == 0 ? false
										: fTracked[fTrackedCount - 1].id
											>= kBackpressureFrames; }'

echo
echo "mutants killed $killed, survived $survived"
[ "$survived" -eq 0 ]
