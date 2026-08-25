#!/bin/bash
# recycle-watchdog.sh -- self-stop guard for a functional-desktop fleet slave.
#
# Runs from cron every 5 min. Stops THIS instance when any of:
#   1. the DRAINED flag exists          -- driver says the queue is empty
#   2. the heartbeat is stale           -- driver (or the agent driving it) died
#   3. the hard wall-clock deadline hit -- backstop against every other bug
#
# It uses `shutdown -h`, not the EC2 API, deliberately: the instance's
# InstanceInitiatedShutdownBehavior is "stop" (verified before relying on it), so
# a halt STOPS the instance and needs no IAM permission. That means the recycle
# still fires if credentials expire, if the instance profile lacks
# ec2:StopInstances -- which builder3's does, it cannot even PutObject -- or if
# the network is gone: exactly the failure modes an API-based stop would share
# with whatever wedged the build in the first place.
#
# Install: copy to /opt/haiku/fleet/, touch INSTALLED, write DEADLINE_EPOCH, and
# add /etc/cron.d/haiku-fleet-recycle running it every 5 minutes.
FLEET=/opt/haiku/fleet
LOG=$FLEET/watchdog.log
HEARTBEAT=$FLEET/heartbeat
DRAINED=$FLEET/DRAINED
DEADLINE=$FLEET/DEADLINE_EPOCH
IDLE_MAX=${IDLE_MAX:-2700}      # 45 min with no heartbeat
now=$(date +%s)

say() { echo "$(date -u +%FT%TZ) $*" >> "$LOG"; }
halt_now() {
	say "RECYCLE: $1 -- issuing shutdown -h now (instance will STOP)"
	sync
	/sbin/shutdown -h now "haiku fleet recycle: $1" || systemctl poweroff -i
	exit 0
}

[ -f "$DRAINED" ] && halt_now "DRAINED flag present ($(cat $DRAINED 2>/dev/null))"

if [ -f "$DEADLINE" ]; then
	d=$(cat "$DEADLINE")
	if [ "$now" -ge "$d" ]; then
		halt_now "hard deadline passed (deadline=$d now=$now)"
	fi
fi

if [ -f "$HEARTBEAT" ]; then
	hb=$(stat -c %Y "$HEARTBEAT")
	age=$((now - hb))
	if [ "$age" -ge "$IDLE_MAX" ]; then
		halt_now "heartbeat stale ${age}s >= ${IDLE_MAX}s"
	fi
	say "ok: heartbeat age ${age}s"
else
	# No heartbeat file yet. Grace period from watchdog install, so a slave that
	# never gets work does not sit idle forever.
	inst=$(stat -c %Y "$FLEET/INSTALLED" 2>/dev/null || echo "$now")
	age=$((now - inst))
	if [ "$age" -ge "$IDLE_MAX" ]; then
		halt_now "no heartbeat ever; ${age}s since watchdog install"
	fi
	say "ok: no heartbeat yet, ${age}s since install"
fi
