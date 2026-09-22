#!/usr/bin/env bash
#
# haiku-publish-lock.sh -- a small, sourceable advisory lock around the DeBeOS
# repo-publish critical section (issue #164).
#
# WHY THIS EXISTS
# ---------------
# `haiku-repo-add` and `haiku-repo-publish` both do the same read-modify-write on
# the live repo: pull the ENTIRE published pool down, add/re-stamp the new
# package(s), rebuild the single `repo` index over the whole set, then mirror the
# pool back up with `--delete` and overwrite the index. That whole sequence is one
# critical section: if two publishers run it against the same prefix at once, the
# second `s3 sync --delete` mirrors ITS (stale) view of the pool over the first's,
# stranding the packages the first one added, and the last index write wins over a
# pool it no longer matches. The SOP called this out as "publishing is
# single-flight ... no concurrency lock until #164" and asked operators to
# eyeball `ec2 describe-instances` for a running publisher first. That is a manual
# guard that fails exactly when it matters (two agents in a fan-out, a pipeline
# publish overlapping a hand publish). This library is the machine-enforced guard.
#
# MECHANISM (and why this shape)
# ------------------------------
# An S3 object is the lock: `${HG_REPO_S3%/}/.publish.lock`, so every publisher --
# whichever host it runs on -- contends on one key beside the repo it protects.
# Acquisition is deliberately built from ONLY `s3 cp` and `s3 rm`, the two verbs
# that BOTH back ends understand: the stock `aws` CLI (Linux builders, the
# pipeline) and the native `debeos-aws` client (a Haiku Graviton box, via
# `haiku-aws`). A single uniform mechanism is required for correctness -- you
# cannot mix an atomic conditional-PutObject publisher with an advisory one on the
# same key without reintroducing the clobber -- and `debeos-aws` has no `s3api`,
# so the lowest common denominator (cp/rm) is what all publishers must speak.
#
# Acquire = check-then-write-then-read-back-verify, with jitter:
#   1. cp the lock object down. Missing (cp fails) -> free, or STALE (its epoch is
#      older than HG_LOCK_TTL, i.e. a crashed holder) -> steal. Fresh + not ours
#      -> someone holds it; sleep with jitter and retry until HG_LOCK_WAIT.
#   2. write our lock object (unique holder id + current epoch).
#   3. settle HG_LOCK_SETTLE seconds, then read it back. If the holder is ours we
#      own it; if a racing writer's PUT landed after ours we read THEIRS, lose the
#      race, and back off. S3 is strongly read-after-write consistent, so the
#      read-back reliably reflects the last PUT: of two simultaneous acquirers,
#      exactly one sees its own id and proceeds.
#
# Crash safety = TTL + heartbeat + steal. A live holder runs a background
# heartbeat that re-stamps the lock's epoch every HG_LOCK_HEARTBEAT seconds, so a
# slow-but-alive publish (a full-pool sync is minutes) never looks stale and is
# never stolen mid-flight. A holder that crashes stops heartbeating; its lock goes
# stale after HG_LOCK_TTL and the next publisher steals it. Release deletes the
# object only if we still own it (a stealer may have taken it), so we never delete
# someone else's lock.
#
# This is an ADVISORY lock: it protects cooperating publishers (all of ours), not
# an adversary. S3 conditional writes (If-None-Match) are the stronger primitive
# and would make step 2/3 atomic; once `debeos-aws` grows that verb this file can
# switch acquisition to it wholesale (one mechanism for all back ends -- never a
# mix). Until then, the read-back-verify + generous settle closes the
# simultaneous-write window for the operational reality here (publishes are
# minutes apart, human/wave-driven).
#
# USAGE (from a `set -euo pipefail` script)
#   AWS=( ... )                       # the resolved s3 client array (haiku-aws/aws)
#   . "$HERE/haiku-publish-lock.sh"
#   publish_lock_acquire              # blocks (or aborts) until we hold it
#   ... critical section ...
#   publish_lock_release              # also runs on EXIT via the trap we install
#
# CONFIG (env)
#   HG_REPO_S3         required by the caller anyway; the lock key derives from it
#   HG_LOCK_KEY        override the full s3:// lock-object uri (default <repo>/.publish.lock)
#   HG_LOCK_TTL        seconds before an un-refreshed lock is considered stale/stealable (default 300)
#   HG_LOCK_HEARTBEAT  seconds between epoch refreshes by the holder (default TTL/3, min 30)
#   HG_LOCK_WAIT       max seconds to wait to acquire before giving up (default 3600)
#   HG_LOCK_SETTLE     seconds to wait after our write before the read-back verify (default 5)
#   HG_LOCK_MODE       wait|abort -- on contention, block until free (default) or exit 1 at once
#   HG_LOCK_DISABLE    set to 1 to bypass the lock entirely (escape hatch; logs a warning)
#   AWS_DEFAULT_REGION / AWS_REGION (default us-west-2)
#

# ---- resolved once at source time ----------------------------------------
: "${HG_REPO_S3:?haiku-publish-lock: set HG_REPO_S3 before sourcing}"
_PL_REGION="${AWS_DEFAULT_REGION:-${AWS_REGION:-us-west-2}}"
_PL_KEY="${HG_LOCK_KEY:-${HG_REPO_S3%/}/.publish.lock}"
_PL_TTL="${HG_LOCK_TTL:-300}"
_PL_WAIT="${HG_LOCK_WAIT:-3600}"
_PL_SETTLE="${HG_LOCK_SETTLE:-5}"
_PL_MODE="${HG_LOCK_MODE:-wait}"
_PL_HEARTBEAT="${HG_LOCK_HEARTBEAT:-}"
if [ -z "$_PL_HEARTBEAT" ]; then
	_PL_HEARTBEAT=$(( _PL_TTL / 3 ))
	[ "$_PL_HEARTBEAT" -lt 30 ] && _PL_HEARTBEAT=30
fi

# The s3 client. Callers set AWS=( ... ); fall back to plain aws if they did not.
# Guard the array reference so this is safe under `set -u` whether or not the
# caller defined AWS.
_pl_aws() {
	if [ -n "${AWS+x}" ] && [ "${#AWS[@]}" -gt 0 ]; then
		"${AWS[@]}" "$@"
	else
		aws "$@"
	fi
}

# A holder id unique to this process/host/run. Random component defends against
# two runs on the same host+pid across a reboot.
_PL_HOLDER=""
_pl_new_holder() {
	local rnd
	rnd="$( (od -An -N6 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n') || echo "$RANDOM$RANDOM" )"
	echo "$(hostname 2>/dev/null || echo host)-$$-${rnd}"
}

_PL_WORK=""          # scratch dir for lock-content files
_PL_HELD=0           # 1 once we own it
_PL_HB_PID=""        # heartbeat background pid

_pl_log() { echo "publish-lock: $*" >&2; }

_pl_now() { date -u +%s; }

# Write the lock content (holder + epoch) to a file and upload it, overwriting.
_pl_write() {
	local epoch="$1" f="$_PL_WORK/lock.put"
	{
		echo "holder=$_PL_HOLDER"
		echo "epoch=$epoch"
		echo "host=$(hostname 2>/dev/null || echo '?')"
		echo "pid=$$"
		echo "iso=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo '?')"
	} > "$f"
	_pl_aws s3 cp "$f" "$_PL_KEY" --region "$_PL_REGION" --only-show-errors
}

# Read the current lock object. On success sets _PL_R_HOLDER/_PL_R_EPOCH and
# returns 0; if the object is absent returns 1.
_PL_R_HOLDER=""; _PL_R_EPOCH=""
_pl_read() {
	local f="$_PL_WORK/lock.get"
	rm -f "$f"
	if ! _pl_aws s3 cp "$_PL_KEY" "$f" --region "$_PL_REGION" --only-show-errors >/dev/null 2>&1; then
		return 1
	fi
	[ -f "$f" ] || return 1
	_PL_R_HOLDER="$(sed -nE 's/^holder=(.*)$/\1/p' "$f" | head -1)"
	_PL_R_EPOCH="$(sed -nE 's/^epoch=([0-9]+)$/\1/p' "$f" | head -1)"
	return 0
}

# Background heartbeat: re-stamp the epoch so a live-but-slow holder never looks
# stale. Kept intentionally simple (a sleep loop) for Haiku's shell.
#
# Two details are load-bearing, both learned from issue #459, where releasing the
# lock left a caller's output pipe open for up to HG_LOCK_HEARTBEAT seconds
# (TTL/3, i.e. 100s by default -- measured at exactly 100s, three runs):
#
#   1. The subshell's stdio is redirected to /dev/null, NOT inherited. A
#      background job inherits the caller's stdout and stderr, so anything
#      reading the caller's output -- a pipe, a command substitution, a
#      CodeBuild log -- cannot see EOF while any process in this job still holds
#      those descriptors. Redirecting here means the heartbeat can never hold a
#      caller's pipe open, whatever else happens to it.
#   2. `sleep` runs as a separate child and is killed explicitly on TERM.
#      `kill $!` reaps only the subshell; its `sleep` survives to its timer and
#      was the process actually holding the descriptors. Backgrounding the sleep
#      and `wait`-ing lets the TERM trap interrupt the wait and take the sleep
#      down with it, so release leaves nothing behind.
#
# Deliberately not using process groups (`set -m` plus `kill -- -PID`): job
# control is not dependable across the shells this library runs under -- it is
# sourced on Haiku as well as on Linux builders -- and the trap does the job with
# only POSIX features.
_pl_heartbeat_start() {
	(
		_pl_hb_sleep=""
		trap 'if [ -n "$_pl_hb_sleep" ]; then kill "$_pl_hb_sleep" 2>/dev/null || true; fi; exit 0' TERM
		while :; do
			sleep "$_PL_HEARTBEAT" &
			_pl_hb_sleep=$!
			wait "$_pl_hb_sleep" 2>/dev/null || true
			_pl_hb_sleep=""
			_pl_write "$(_pl_now)" >/dev/null 2>&1 || true
		done
	) >/dev/null 2>&1 &
	_PL_HB_PID=$!
}
_pl_heartbeat_stop() {
	if [ -n "$_PL_HB_PID" ]; then
		kill "$_PL_HB_PID" >/dev/null 2>&1 || true
		wait "$_PL_HB_PID" 2>/dev/null || true
		_PL_HB_PID=""
	fi
}

# publish_lock_acquire -- block (or abort) until we hold the lock.
publish_lock_acquire() {
	if [ "${HG_LOCK_DISABLE:-0}" = 1 ]; then
		_pl_log "HG_LOCK_DISABLE=1 -- skipping the publish concurrency lock (UNSAFE if another publisher runs)"
		return 0
	fi
	_PL_WORK="$(mktemp -d)"
	_PL_HOLDER="$(_pl_new_holder)"
	# Ensure release runs however the caller exits.
	trap 'publish_lock_release' EXIT
	_pl_log "acquiring $_PL_KEY (holder=$_PL_HOLDER, ttl=${_PL_TTL}s, mode=$_PL_MODE)"

	local deadline=$(( $(_pl_now) + _PL_WAIT ))
	local waited_note=0
	while :; do
		# 1. is there a live holder?
		if _pl_read; then
			if [ "$_PL_R_HOLDER" = "$_PL_HOLDER" ]; then
				: # already ours (shouldn't happen pre-acquire, but harmless)
			else
				local age=$(( $(_pl_now) - ${_PL_R_EPOCH:-0} ))
				if [ "${_PL_R_EPOCH:-0}" -gt 0 ] && [ "$age" -lt "$_PL_TTL" ]; then
					# fresh lock held by someone else
					if [ "$_PL_MODE" = abort ]; then
						_pl_log "lock held by $_PL_R_HOLDER (age ${age}s < ttl) -- aborting (mode=abort)"
						rm -rf "$_PL_WORK"; trap - EXIT
						return 3
					fi
					if [ "$(_pl_now)" -ge "$deadline" ]; then
						_pl_log "timed out after ${_PL_WAIT}s waiting for $_PL_R_HOLDER -- giving up"
						rm -rf "$_PL_WORK"; trap - EXIT
						return 4
					fi
					if [ "$waited_note" = 0 ]; then
						_pl_log "held by $_PL_R_HOLDER (age ${age}s); waiting (up to ${_PL_WAIT}s)"
						waited_note=1
					fi
					sleep $(( 3 + RANDOM % 5 ))
					continue
				fi
				_pl_log "stealing stale lock from $_PL_R_HOLDER (age ${age}s >= ttl ${_PL_TTL}s)"
			fi
		fi
		# 2. write our claim, 3. settle + read-back verify.
		_pl_write "$(_pl_now)" || { _pl_log "lock write failed; retrying"; sleep 3; continue; }
		sleep "$_PL_SETTLE"
		if _pl_read && [ "$_PL_R_HOLDER" = "$_PL_HOLDER" ]; then
			_PL_HELD=1
			_pl_heartbeat_start
			_pl_log "acquired (holder=$_PL_HOLDER)"
			return 0
		fi
		# lost the race; back off and retry
		if [ "$(_pl_now)" -ge "$deadline" ]; then
			_pl_log "timed out after ${_PL_WAIT}s (lost the write race to ${_PL_R_HOLDER:-?})"
			rm -rf "$_PL_WORK"; trap - EXIT
			return 4
		fi
		sleep $(( 2 + RANDOM % 4 ))
	done
}

# publish_lock_release -- stop the heartbeat and delete the lock iff we own it.
publish_lock_release() {
	_pl_heartbeat_stop
	if [ "$_PL_HELD" = 1 ]; then
		# Only delete if the live object is still ours; a stealer may have taken it
		# (e.g. our heartbeat died and we overran the TTL). Never delete another
		# holder's lock.
		if _pl_read && [ "$_PL_R_HOLDER" != "$_PL_HOLDER" ]; then
			_pl_log "not releasing: lock now held by $_PL_R_HOLDER (we lost it)"
		else
			_pl_aws s3 rm "$_PL_KEY" --region "$_PL_REGION" --only-show-errors >/dev/null 2>&1 || true
			_pl_log "released (holder=$_PL_HOLDER)"
		fi
		_PL_HELD=0
	fi
	[ -n "$_PL_WORK" ] && rm -rf "$_PL_WORK"
	_PL_WORK=""
	trap - EXIT
}
