#!/bin/bash
# DEPRECATED -- this drives the metal + QEMU-guest build fleet, which has been
# retired along with the shared metal host. Package builds now run natively on
# Graviton Haiku EC2 instances over SSM (see graviton/scripts/haiku-nativebuild
# and graviton/docs/native-ec2-builds.md). Kept for reference only.
#
# fleet-dispatch.sh -- farm the functional-desktop closure across the metal's
# QEMU arm64 Haiku build guests.
#
# WHY THE FLEET LIVES INSIDE ONE INSTANCE. The unit of parallelism for this fork
# is a QEMU guest, not an EC2 instance: haikuporter builds these recipes NATIVELY
# inside an arm64 Haiku guest, and those guests need KVM. Only .metal instances
# expose /dev/kvm -- a c7g.4xlarge has no nested virtualisation, so it cannot run
# a build guest at all (measured: no /dev/kvm, no kvm module loadable). So the
# metal's 6 live guests ARE the slave fleet, and a second EC2 node can only help
# with work that cross-compiles on Linux, which is why builder3 was given the
# jam correctness gate instead of recipes.
#
# haikuporter resolves the dependency DAG itself, so only leaf CONSUMERS are
# requested here and the closure is pulled in behind them.
set -u
FLEET=/opt/haiku/fleet
mkdir -p "$FLEET/logs"
LOG=$FLEET/dispatch.log
exec >> "$LOG" 2>&1

echo "=== DISPATCH START $(date -u +%FT%TZ) ==="

# Guest assignment. Ports are the existing, already-seeded guests; 2240 is
# excluded because its packages/ directory is empty (0 hpkgs) and it would have
# to bootstrap the whole seed set before it could build anything.
#
# llvm12 gets a guest to itself: it is many core-hours and saturates one box by
# itself, so it starts FIRST and runs alongside the wide tiers rather than after
# them. mesa is deliberately NOT queued here -- it needs llvm12's hpkg published
# first, so it is a second wave.
declare -A ASSIGN=(
	[2235]="llvm12"
	[2229]="ffmpeg6"
	[2227]="giflib libwebp libavif"
	[2222]="libicns libraw p7zip"
	[2230]="fluidlite openssh"
	[2231]="keymapswitcher pe vision"
)

for PORT in "${!ASSIGN[@]}"; do
	recipes=${ASSIGN[$PORT]}
	# Only dispatch to a guest that actually answers. A dead guest silently
	# swallowing its queue is the failure this check exists to prevent.
	if timeout 30 ssh -n -p "$PORT" -o StrictHostKeyChecking=no \
		-o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 \
		-i /home/ubuntu/.ssh/haiku-ed25519 baron@127.0.0.1 true 2>/dev/null; then
		echo "dispatch port $PORT <- $recipes"
		rm -f "$FLEET/worker-$PORT.done"
		setsid nohup "$FLEET/fleet-worker.sh" "$PORT" $recipes >/dev/null 2>&1 &
		sleep 2
	else
		echo "SKIP port $PORT -- unreachable"
	fi
done

echo "--- all workers launched $(date -u +%FT%TZ) ---"
wait
echo "=== DISPATCH DONE $(date -u +%FT%TZ) ==="
touch "$FLEET/dispatch.done"
