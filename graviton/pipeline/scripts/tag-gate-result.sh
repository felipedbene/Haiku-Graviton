#!/usr/bin/env bash
#
# tag-gate-result.sh -- stamp the hardware perf gate's outcome onto the AMI it
# tested, as EC2 tags.
#
# Why tags and not the build log: choosing which already-baked candidate to make
# canonical is a decision taken days after the bake. CodeBuild logs are retained for
# a month and CodePipeline execution history ages out sooner, so "which of these six
# candidates actually passed, and at what throughput?" was answerable only for a
# little while and then not at all. The tags travel with the image for as long as
# the image exists, so `aws ec2 describe-images` answers it.
#
# This matters most for the OVEN pipeline, which ends at the Test stage and has no
# promotion of its own: its whole value is producing a stack of tested candidates
# that a human can pick the best of and hand to `haiku-canonical promote`. A
# candidate whose test result cannot be read is no better than an untested one.
#
# Usage:
#   tag-gate-result.sh <ami-id> <succeeding: 1|0> [result-file]
#
# <succeeding> is CODEBUILD_BUILD_SUCCEEDING. It is the authority on pass/fail: the
# gate exits non-zero on any failed assertion, so a build that is not succeeding is
# a failed gate whatever the sidecar says (and on an early failure -- the image
# never booted -- there is no sidecar at all).
#
# Best-effort by contract. It runs in post_build to cover the failure path too, and
# it must never be the reason a build is marked failed: a tag is bookkeeping and the
# gate's verdict has already been delivered by the exit status. Every path here
# exits 0.
set -uo pipefail

AMI_ID="${1:-}"
SUCCEEDING="${2:-0}"
RESULT_FILE="${3:-/tmp/haiku-perf-gate-result}"
REGION="${AWS_REGION:-${AWS_DEFAULT_REGION:-us-west-2}}"

if [ -z "$AMI_ID" ]; then
	echo "tag-gate-result: no AMI id given; nothing to tag" >&2
	exit 0
fi

# Sidecar values, defaulted so a missing or truncated file still produces a usable
# verdict tag rather than no tags at all.
mtu=unknown
rx=0
tx=0
verdict=""
if [ -r "$RESULT_FILE" ]; then
	while IFS='=' read -r k v; do
		case "$k" in
			mtu) mtu="$v" ;;
			rx) rx="$v" ;;
			tx) tx="$v" ;;
			verdict) verdict="$v" ;;
		esac
	done < "$RESULT_FILE"
fi

# The build status wins. A gate that failed an assertion exits non-zero, so
# SUCCEEDING=0 is a fail even if the sidecar recorded measurements first -- which it
# deliberately does, to keep the numbers that failed it.
if [ "$SUCCEEDING" != "1" ]; then
	verdict=fail
elif [ -z "$verdict" ]; then
	# Succeeded but wrote no verdict: the gate was skipped or replaced. Say so
	# rather than claiming a pass nobody measured.
	verdict=unknown
fi

echo "tag-gate-result: $AMI_ID perf-gate=$verdict mtu=$mtu rx=$rx tx=$tx"
if aws ec2 create-tags --region "$REGION" --resources "$AMI_ID" --tags \
		"Key=perf-gate,Value=${verdict}" \
		"Key=perf-gate-at,Value=$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
		"Key=perf-mtu,Value=${mtu}" \
		"Key=perf-rx-mbps,Value=${rx}" \
		"Key=perf-tx-mbps,Value=${tx}"; then
	echo "tag-gate-result: tagged $AMI_ID"
else
	echo "tag-gate-result: WARNING could not tag $AMI_ID (verdict stands regardless)" >&2
fi
exit 0
