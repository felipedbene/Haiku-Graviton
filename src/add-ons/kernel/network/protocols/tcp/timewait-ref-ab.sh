#!/bin/sh
# The #616 controlled A/B on the Graviton target, over SSH.
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
# WATCH OUT for a stale override. An earlier target was found carrying one from
# a previous session (sha fd8eaa27..., 2026-10-14 18:21) matching neither the
# AMI's packaged module nor any build of trunk, so three harness phases had
# already been run against an unidentified kernel. Every arm below prints the
# sha256 of what is actually loaded, and that is the only acceptable evidence of
# which kernel produced a result.
#
# THIS DRIVES THE TARGET OVER SSH, NOT SSM, AND THAT IS NOT A STYLE CHOICE.
# Measured 2026-10-15: the SSM agent on a Haiku arm64 instance does not survive
# *any* reboot. `shutdown -r` boots the machine fine -- the console carries a
# complete second boot and ena passes traffic -- but the agent never
# re-registers; PingStatus goes ConnectionLost and then the row disappears from
# describe-instance-information entirely. An EC2 stop/start does not rescue it
# either. Only a fresh launch registers the agent. Since this A/B needs two
# reboots to rebind the module, SSM cannot carry it: the first reboot ends the
# run. sshd, by contrast, comes back on every reboot.
#
# Getting an SSH key in is the one thing that needs SSM, exactly once, on a
# freshly launched instance while its agent is still alive:
#
#	ssh-keygen -t ed25519 -N '' -f /tmp/abkey
#	aws ssm send-command --instance-ids "$ID" \
#	    --document-name AWS-RunShellScript --parameters 'commands=["
#	      echo '"'"'<contents of /tmp/abkey.pub>'"'"' \
#	        >> /boot/home/config/settings/ssh/authorized_keys"]'
#
# sshd_config has `AuthorizedKeysFile config/settings/ssh/authorized_keys`,
# relative to baron's home, and baron is uid 0. Do NOT go hunting for the key
# the AMI was baked with: the image authorises ed25519 `haiku-graviton`
# (SHA256:+MxTFW3bfqvHjvCQpI5rZUTnIq1uudmCtXaemkILLQg, public half in SSM
# parameter /haiku-graviton/baron-ssh-key-pub) and ed25519 `haiku-rdclient`, and
# neither private half is still in anyone's possession. Note also that the
# target's sshd is OpenSSH 10.4 with RSA disabled -- it advertises only
# `ssh-ed25519,sk-ssh-ed25519` -- so the RSA EC2 key pair that the instance is
# launched under can never authenticate, no matter what
# PubkeyAcceptedAlgorithms says. The key must be ed25519.
#
# The dev desk has no route to the target's private IP, so SSH goes through an
# SSM port-forward on a second instance in the same VPC, which keeps the private
# key on the dev desk:
#
#	aws ssm start-session --target <peer-instance> \
#	  --document-name AWS-StartPortForwardingSessionToRemoteHost \
#	  --parameters '{"host":["<target-private-ip>"],"portNumber":["22"],
#	                 "localPortNumber":["2223"]}'
#
# Corollary on durability: every write must be flushed before a reboot, and
# `sync` is not enough on its own. On this build the page writer never flushes
# file data on a timeout at all, and /bin/sync comes from the haikuports
# coreutils package rather than the base image, so it may not even exist.
# Writes are therefore verified by reading the file back and checksumming it
# before the reboot, not assumed from `sync`.
#
# Usage:  timewait-ref-ab.sh <ssh-config-alias> <module-path> [rounds]
#         e.g. timewait-ref-ab.sh armtarget /tmp/tcp-diag-on.bin 400

set -e

HOST="${1:?ssh host alias (see -F config) required}"
MODULE="${2:?path to the diagnostics-on tcp module required}"
ROUNDS="${3:-400}"
OVERRIDE=/boot/home/config/non-packaged/add-ons/kernel/network/protocols/tcp
SSHCFG="${SSHCFG:-/tmp/ab616.sshcfg}"

strings "$MODULE" | grep -q 'pre-#616 code' || {
	echo "REFUSING: $MODULE has no diagnostics compiled in." >&2
	echo "Build it with: jam -qa -sTCP_TIME_WAIT_DIAG=1 tcp" >&2
	exit 1
}

sh_() { timeout 900 ssh -F "$SSHCFG" "$HOST" "$@"; }

# A reboot, and a wait for sshd to answer again rather than a fixed sleep: the
# whole point of the SSH path is that "did the target actually come back" is
# assertable, so assert it before measuring an arm.
reboot_wait() {
	echo "-- rebooting $HOST"
	# `shutdown -r` tears down the connection under us; that exit is expected.
	sh_ 'shutdown -r' >/dev/null 2>&1 || true
	sleep 20
	echo "-- waiting for sshd"
	n=0
	while [ "$n" -lt 40 ]; do
		if timeout 20 ssh -F "$SSHCFG" "$HOST" 'true' >/dev/null 2>&1; then
			echo "-- back after ~$((20 + n * 15))s"
			return 0
		fi
		sleep 15
		n=$((n + 1))
	done
	echo "FATAL: $HOST did not answer sshd within 10 minutes." >&2
	echo "       Do not interpret any later output as a measurement." >&2
	exit 1
}

# The harness phases that reach the two sites. `quick` is left out: its three
# phases passed on both kernels and are regression guards, not discriminators.
run_harness() {
	sh_ "cd /boot/home && ./tcp616 burst $ROUNDS 4 2>&1 | tail -20; ./tcp616 twhold 64 150 2>&1 | tail -20"
}

report_loaded() {
	sh_ 'echo LOADED:; listimage 2>/dev/null | grep "protocols/tcp$"; sha256sum '"$OVERRIDE"' 2>/dev/null || echo no-override; sha256sum /boot/system/add-ons/kernel/network/protocols/tcp'
}

# The counters only ever go up and the module is reloaded on each boot, so the
# syslog of one boot is the measurement for one arm. Read it whole rather than
# diffing a cumulative value.
report_counters() {
	sh_ 'echo COUNTERS:; grep -c "pre-#616 code" /var/log/syslog 2>/dev/null || echo 0; grep "pre-#616 code" /var/log/syslog 2>/dev/null | tail -20 || true'
}

echo "=============== ARM U: packaged module, no override (pre-#657) ==========="
# Move the override aside and PROVE it is gone before the reboot, so a lost
# write cannot be mistaken for arm U having run. A freshly launched instance has
# no override at all, in which case this is a no-op and that is the cleanest
# arm U available.
sh_ "test -e $OVERRIDE && mv $OVERRIDE /boot/home/tcp-override.saved || true; sync 2>/dev/null || true; test -e $OVERRIDE && echo OVERRIDE-STILL-PRESENT || echo OVERRIDE-MOVED-ASIDE"
reboot_wait
report_loaded
run_harness
report_counters
echo "NOTE: arm U carries no diagnostics, so a zero count here means only that"
echo "      the module cannot count -- judge arm U by whether it survived."

echo "=============== ARM F: this branch, diagnostics on ======================="
# scp, not a base64 chunk loop: SSH has no 100 KB parameter cap, so the module
# goes over in one piece. Read it back and checksum BEFORE the reboot -- `sync`
# is not trustworthy here (see the header), so the checksum is the flush
# evidence.
echo "expected sha256: $(sha256sum "$MODULE" | cut -d' ' -f1)"
timeout 300 scp -F "$SSHCFG" "$MODULE" "$HOST:/boot/home/tcp.new"
sh_ "mkdir -p $(dirname $OVERRIDE) && cp /boot/home/tcp.new $OVERRIDE && chmod 755 $OVERRIDE && sync 2>/dev/null || true; echo INSTALLED:; sha256sum $OVERRIDE"
echo "^^ the sha above MUST equal the expected sha. If it does not, stop here:"
echo "   arm F would measure whatever module is actually on disk, not this one."
reboot_wait
report_loaded
echo "^^ and LOADED must name the non-packaged path. If the kernel loaded the"
echo "   packaged module anyway, the override failed to bind (an ABI mismatch"
echo "   against this AMI's kernel does exactly that) and arm F is really a"
echo "   second arm U -- not a zero result."
run_harness
report_counters

echo "=========================================================================="
echo "A non-zero count in arm F is the result: each one is a release the"
echo "pre-#616 kernel would have made without owning the reference. A zero count"
echo "means this load does not reach the hazard, and the harness needs changing"
echo "rather than the conclusion being that the bug is not real."
