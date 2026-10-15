#!/bin/sh
# The #616 controlled A/B on the Graviton target, over SSM.
#
# Arm U is the AMI's own packaged tcp module, which genuinely predates the fix:
# the canonical AMI was baked 2026-10-13 20:51 and #657 landed 2026-10-14
# 13:52. Arm F is this branch's module built with -sTCP_TIME_WAIT_DIAG=1.
#
# The target loads a non-packaged override ahead of the packaged module, from
#   /boot/home/config/non-packaged/add-ons/kernel/network/protocols/tcp
# so switching arms means putting a module there (F) or taking it away (U), and
# rebooting -- the module is bound at boot.
#
# WATCH OUT for a stale override. The target was found carrying one from an
# earlier session (sha fd8eaa27..., 2026-10-14 18:21) matching neither the AMI's
# packaged module nor any build of trunk, so three harness phases had already
# been run against an unidentified kernel. Every arm below prints the sha256 of
# what is actually loaded, and that is the only acceptable evidence of which
# kernel produced a result.
#
# Usage:  timewait-ref-ab.sh <instance-id> <module-path> [rounds]

set -e

INSTANCE="${1:?instance id required}"
MODULE="${2:?path to the diagnostics-on tcp module required}"
ROUNDS="${3:-400}"
OVERRIDE=/boot/home/config/non-packaged/add-ons/kernel/network/protocols/tcp

: "${AWS_PROFILE:=haiku-graviton}"
export AWS_PROFILE

strings "$MODULE" | grep -q 'pre-#616 code' || {
	echo "REFUSING: $MODULE has no diagnostics compiled in." >&2
	echo "Build it with: jam -qa -sTCP_TIME_WAIT_DIAG=1 tcp" >&2
	exit 1
}

ssm() {
	id=$(aws ssm send-command --instance-ids "$INSTANCE" \
		--document-name AWS-RunShellScript --timeout-seconds 900 \
		--parameters "commands=$1" --query 'Command.CommandId' --output text)
	# Poll rather than wait: a reboot arm makes the invocation unreachable for a
	# while and `aws ssm wait` gives up on that.
	n=0
	while [ "$n" -lt 90 ]; do
		sleep 10
		s=$(aws ssm get-command-invocation --command-id "$id" \
			--instance-id "$INSTANCE" --query Status --output text 2>/dev/null \
			|| echo Unreachable)
		case "$s" in
			InProgress|Pending|Unreachable) ;;
			*) break ;;
		esac
		n=$((n + 1))
	done
	aws ssm get-command-invocation --command-id "$id" --instance-id "$INSTANCE" \
		--query StandardOutputContent --output text 2>/dev/null || true
}

# The harness phases that reach the two sites. `quick` is left out: its three
# phases passed on both kernels and are regression guards, not discriminators.
run_harness() {
	ssm "[\"cd /boot/home\",\"./tcp616 burst $ROUNDS 4 2>&1 | tail -20\",\"./tcp616 twhold 64 150 2>&1 | tail -20\"]"
}

report_loaded() {
	ssm '["echo LOADED:","listimage 2>/dev/null | grep protocols/tcp || echo none","sha256sum /boot/home/config/non-packaged/add-ons/kernel/network/protocols/tcp 2>/dev/null || echo no-override","sha256sum /boot/system/add-ons/kernel/network/protocols/tcp"]'
}

# The counters only ever go up and the module is reloaded on each boot, so the
# syslog of one boot is the measurement for one arm. Read it whole rather than
# diffing a cumulative value.
report_counters() {
	ssm '["echo COUNTERS:","grep -c \"pre-#616 code\" /var/log/syslog 2>/dev/null || echo 0","grep \"pre-#616 code\" /var/log/syslog 2>/dev/null | tail -20 || true"]'
}

echo "=============== ARM U: packaged module, no override (pre-#657) ==========="
ssm "[\"test -e $OVERRIDE && mv $OVERRIDE /boot/home/tcp-override.saved || true\",\"sync\",\"shutdown -r\"]" >/dev/null || true
sleep 120
report_loaded
run_harness
report_counters
echo "NOTE: arm U carries no diagnostics, so a zero count here means only that"
echo "      the module cannot count -- judge arm U by whether it survived."

echo "=============== ARM F: this branch, diagnostics on ======================="
B64=$(base64 -w0 "$MODULE")
# SSM caps a parameter, so the module goes over in chunks and is reassembled
# and checksummed on the far side.
echo "$B64" | fold -w 40000 | while read -r chunk; do
	ssm "[\"echo -n '$chunk' >> /boot/home/tcp.b64\"]" >/dev/null
done
ssm "[\"base64 -d /boot/home/tcp.b64 > /boot/home/tcp.new\",\"rm -f /boot/home/tcp.b64\",\"sha256sum /boot/home/tcp.new\",\"mkdir -p $(dirname $OVERRIDE)\",\"cp /boot/home/tcp.new $OVERRIDE\",\"chmod 755 $OVERRIDE\",\"sync\",\"shutdown -r\"]" >/dev/null || true
echo "expected sha256: $(sha256sum "$MODULE" | cut -d' ' -f1)"
sleep 120
report_loaded
run_harness
report_counters

echo "=========================================================================="
echo "A non-zero count in arm F is the result: each one is a release the"
echo "pre-#657 kernel would have made without owning the reference. A zero count"
echo "means this load does not reach the hazard, and the harness needs changing"
echo "rather than the conclusion being that the bug is not real."
