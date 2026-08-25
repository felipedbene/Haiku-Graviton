#!/bin/bash
# fleet-dispatch2.sh -- wave 2 of the functional-desktop closure.
#
# Wave 1 established that the pipeline works (giflib built and harvested: giflib,
# giflib_devel, giflib_tools) but that omitting --all-dependencies makes
# haikuporter resolve build-requires only against ALREADY-BUILT packages. Wave 1's
# failures were therefore unbuilt-DAG errors, not recipe defects:
#   libicns        -> devel:libopenjp2   (needs openjpeg)
#   ffmpeg6        -> devel:libmp3lame   (needs lame)
#   keymapswitcher -> cmd:mkdepend       (needs mkdepend)
#
# Wave 2 passes --all-dependencies AND orders the queue dependency-first. The
# ordering is not redundant with the flag: it is what keeps the gnulib/autoconf
# conftest-hang class individually bounded. If lame/libogg/libvorbis/speex were
# built implicitly inside the ffmpeg6 invocation, a wedged conftest in any one of
# them would silently consume ffmpeg6's entire 6-hour budget and the per-recipe
# timeout the brief asked for would never fire. Built as named recipes they each
# get their own 90-minute bound.
#
# 2235 is deliberately absent: llvm12 has been running there since wave 1 and must
# not be disturbed. mesa is a later wave -- it needs llvm12's hpkg published.
set -u
FLEET=/opt/haiku/fleet
mkdir -p "$FLEET/logs"
LOG=$FLEET/dispatch2.log
exec >> "$LOG" 2>&1

echo "=== DISPATCH 2 START $(date -u +%FT%TZ) ==="

declare -A ASSIGN=(
	# hazard class first, each individually bounded at 90 min
	[2229]="lame libogg libvorbis speex opus"
	[2227]="libwebp libavif"
	[2222]="openjpeg libicns libraw p7zip"
	[2230]="fluidlite openssh"
	# mkdepend first: it is what provides cmd:mkdepend for the makefile_engine
	# prerequisite that every haiku-app below needs.
	[2231]="mkdepend keymapswitcher pe vision"
)

for PORT in "${!ASSIGN[@]}"; do
	recipes=${ASSIGN[$PORT]}
	# Never dispatch on top of a guest that is still working: wave 1's worker may
	# not have exited, and two haikuporter runs in one guest corrupt each other's
	# packages/ directory.
	if [ ! -f "$FLEET/worker-$PORT.done" ]; then
		echo "SKIP port $PORT -- wave-1 worker still running"
		continue
	fi
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

echo "--- wave 2 launched $(date -u +%FT%TZ) ---"
wait
echo "=== DISPATCH 2 DONE $(date -u +%FT%TZ) ==="
touch "$FLEET/dispatch2.done"
