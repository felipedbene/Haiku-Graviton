#!/bin/sh
# Mutation test for the M2 flow-control policy.
#
# A test that still passes with the rule it tests removed is worth nothing. This
# script breaks each rule in RemoteFlowQueue.cpp, one at a time, in a throwaway
# copy, and requires the unit test to go RED for every one. A mutation that
# survives is printed as SURVIVED and the script exits non-zero -- that is a
# hole in the test, not a pass.
set -e

here=$(cd "$(dirname "$0")" && pwd)
remote=$here/../../../../servers/app/drawing/interface/remote
work=${OUT:-$here/build}/mutants
mkdir -p "$work"

survived=0
killed=0

mutate() {
	name=$1
	from=$2
	to=$3

	rm -rf "$work/$name"
	mkdir -p "$work/$name"
	cp "$remote/RemoteFlowQueue.cpp" "$work/$name/RemoteFlowQueue.cpp"

	printf '%s' "$from" > "$work/$name/from.txt"
	printf '%s' "$to" > "$work/$name/to.txt"
	python3 "$here/mutate.py" "$work/$name/RemoteFlowQueue.cpp" \
		"$work/$name/from.txt" "$work/$name/to.txt"

	if ! ${CXX:-g++} -std=c++11 -w -O1 -DREMOTE_FLOW_QUEUE_TEST \
			-I"$here" -I"$remote" \
			"$here/RemoteFlowQueueTest.cpp" "$work/$name/RemoteFlowQueue.cpp" \
			-o "$work/$name/test" 2> "$work/$name/build.log"; then
		echo "=== $name"
		echo "KILLED (did not compile)"
		killed=$((killed + 1))
		return
	fi

	echo "=== $name"
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

# Two sites at once, for a hazard that two guards cover: mutating either one
# alone is masked by the other, so the pair has to be broken together to show
# that the hazard is really guarded rather than accidentally unreachable.
mutate2() {
	name=$1
	from1=$2
	to1=$3
	from2=$4
	to2=$5

	rm -rf "$work/$name"
	mkdir -p "$work/$name"
	cp "$remote/RemoteFlowQueue.cpp" "$work/$name/RemoteFlowQueue.cpp"

	printf '%s' "$from1" > "$work/$name/from1.txt"
	printf '%s' "$to1" > "$work/$name/to1.txt"
	printf '%s' "$from2" > "$work/$name/from2.txt"
	printf '%s' "$to2" > "$work/$name/to2.txt"
	python3 "$here/mutate.py" "$work/$name/RemoteFlowQueue.cpp" \
		"$work/$name/from1.txt" "$work/$name/to1.txt"
	python3 "$here/mutate.py" "$work/$name/RemoteFlowQueue.cpp" \
		"$work/$name/from2.txt" "$work/$name/to2.txt"

	echo "=== $name"
	if ! ${CXX:-g++} -std=c++11 -w -O1 -DREMOTE_FLOW_QUEUE_TEST \
			-I"$here" -I"$remote" \
			"$here/RemoteFlowQueueTest.cpp" "$work/$name/RemoteFlowQueue.cpp" \
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

# M1 -- emit a partial message: hand out one byte less than the message is.
mutate emit-partial-message \
	'	_length = fEntries[0].length;' \
	'	_length = fEntries[0].length - 1;'

# M2 -- drop a frame nothing repaints: skip the coverage test.
mutate drop-non-superseded-frame \
	'			&& _Covers(coverage, coverageCount, damage, damageCount);' \
	'			&& true;'

# M3 -- exceed the bound: never enforce it.
mutate exceed-the-bound \
	'	if (fCount + 1 <= fMaxMessages && fBytes + incoming <= fMaxBytes)
		return true;

	fCoalesced += _Coalesce();' \
	'	return true;

	fCoalesced += _Coalesce();'

# M4 -- drop a durable state op.
mutate drop-a-state-op \
	'	switch (code) {
		// ---- Barriers' \
	'	if (code == RP_SET_HIGH_COLOR)
		return OP_DROPPABLE;

	switch (code) {
		// ---- Barriers'

# M5 -- ignore the barrier rule, so a scroll can read pixels that were dropped.
mutate ignore-barriers \
	'		if (fEntries[i].klass == OP_BARRIER)
			lastBarrier = (long)i;' \
	'		if (false)
			lastBarrier = (long)i;'

# M6 -- collapse without owing a resync: the original silent discard.
mutate collapse-without-resync \
	'	fResyncOwed = true;' \
	'	fResyncOwed = false;'

# M7 -- an unrecognised opcode becomes droppable.
mutate unknown-opcode-droppable \
	'		default:
			// Unknown, including every client -> server input opcode that has no
			// business in this queue and every opcode a later milestone adds.
			return OP_PINNED;' \
	'		default:
			return OP_DROPPABLE;'

# M8 -- trust a pixel op whose drawing mode is unknown or blending.
mutate trust-unknown-drawing-mode \
	'		if (!hasToken || _ModeOf(token) != kModeCopy)' \
	'		if (false)'

# M9 -- coalesce across an op that observed the state.
mutate coalesce-across-a-draw-op \
	'			if (later.klass != OP_COALESCABLE)
				break;' \
	'			if (false)
				break;'

# M10 -- frames stop being per-emitter.
mutate frames-not-per-emitter \
	'	e.owner = owner;' \
	'	e.owner = 0;'

# M11 -- drop a frame that declared no damage at all. Two guards cover this
# (the damageCount test in the candidate expression, and _Covers() refusing an
# empty inner region), so both have to go for the mutant to be alive at all.
mutate2 drop-frame-without-damage \
	'			&& damageCount > 0 && (lastBarrier < 0' \
	'			&& true && (lastBarrier < 0' \
	'	if (innerCount == 0)
		return false;' \
	'	if (innerCount == 0)
		return true;'

# M12 -- trust fractional damage coordinates in the inclusive-edge arithmetic.
mutate trust-fractional-damage \
	'		if (value != (float)(long)value)
			return false;' \
	'		if (false)
			return false;'

echo
echo "mutants killed $killed, survived $survived"
[ "$survived" -eq 0 ]
