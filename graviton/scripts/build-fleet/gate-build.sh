#!/bin/bash
# gate-build2.sh -- STEP-3 gate, second attempt.
#
# First attempt died on:
#   ...failed DownloadLocatedFile1 .../packaging/repositories/HaikuPorts
#   wget https://eu.hpkg.haiku-os.org/.../build-packages/<checksum>/repo -> 404
#
# Cause (not a compile error). RemotePackageRepository pins a checksum computed
# over the repo's package LIST, and upstream's CDN only hosts sets whose checksum
# matches a config upstream itself published. Fork commit e252764e7f
# ("build/jam: list the arm64 image libraries we build natively") added 8 entries
# -- jasper, libjpeg_turbo, libpng16, tiff and their _devel -- to the REMOTE list.
# That makes the checksum fork-local, so it can never resolve upstream. Measured:
#   fork-tip checksum 426f9b7b... -> HTTP 404
#   pre-e252764e7f    e18c479d... -> HTTP 200, 14575 bytes
# Those four libraries are built natively into /opt/haiku/hpkg-out/arm64/, so
# listing them as things to DOWNLOAD is wrong regardless of the checksum.
#
# For this gate only the remote package LIST is reverted, in the working tree.
# It selects which prebuilt HaikuPorts packages the build may fetch; it has no
# bearing on how libroot is compiled, so haiku.hpkg still gets the arm64
# __swap_float/__swap_double fix -- which is the whole point of the gate.
set -u
FLEET=/opt/haiku/fleet
LOG=$FLEET/gate-build.log
TREE=/opt/haiku/haiku
GEN=$TREE/generated.arm64
JAM=/opt/haiku/buildtools/jam/bin.linuxarm/jam
REPOCFG=build/jam/repositories/HaikuPorts/arm64
SERVABLE_REV=a74b07b6
exec >> "$LOG" 2>&1
echo "=== GATE BUILD 2 START $(date -u +%FT%TZ) ==="

( while :; do touch "$FLEET/heartbeat"; sleep 60; done ) &
HB=$!
trap 'kill $HB 2>/dev/null' EXIT
touch "$FLEET/heartbeat"

cd "$TREE" || exit 3
echo "--- HEAD ---"; git log --oneline -1

echo "--- reverting ONLY the remote package list to the servable revision ---"
# Plain file write from a git blob: no branch/index state is touched, and
# `git diff -- $REPOCFG` shows exactly what changed, so this is trivially undone.
git show "$SERVABLE_REV:$REPOCFG" > "$REPOCFG" || { echo "FATAL: could not restore $REPOCFG"; exit 9; }
echo "--- diff vs tip for that one file ---"
git diff --stat -- "$REPOCFG"

echo "--- ASSERT the swap fix is STILL present after the revert ---"
if grep -q 'fmov w0, s0' src/system/libroot/os/arch/arm64/byteorder.S \
   && grep -q 'fmov x0, d0' src/system/libroot/os/arch/arm64/byteorder.S; then
	echo "OK: byteorder.S still carries the fixed operand order"
else
	echo "FATAL: swap fix missing -- refusing to build"
	exit 7
fi

echo "--- clearing the stale repo artifacts so jam recomputes the checksum ---"
rm -f "$GEN/objects/haiku/arm64/packaging/repositories/HaikuPorts" \
      "$GEN/objects/haiku/arm64/packaging/repositories/HaikuPorts-checksum"

cd "$GEN" || exit 8
echo "PWD=$PWD"    # never via sudo: sudo strips PWD and the image build dies
$JAM -q -j16 haiku.hpkg haiku_datatranslators.hpkg
RC=$?
echo "=== JAM2 RC=$RC $(date -u +%FT%TZ) ==="

echo "--- new checksum actually used ---"
cat "$GEN/objects/haiku/arm64/packaging/repositories/HaikuPorts-checksum" 2>/dev/null

echo "--- resulting artifacts ---"
P=$GEN/objects/haiku/arm64/packaging/packages
ls -la "$P/haiku.hpkg" "$P/haiku_datatranslators.hpkg" 2>&1

if [ $RC -eq 0 ]; then
	mkdir -p "$FLEET/gate-out"
	for f in haiku.hpkg haiku_datatranslators.hpkg; do
		[ -f "$P/$f" ] && cp -p "$P/$f" "$FLEET/gate-out/" && echo "staged $f"
	done
	ls -la "$FLEET/gate-out/"
fi
echo "=== GATE BUILD 2 DONE rc=$RC $(date -u +%FT%TZ) ==="
touch "$FLEET/gate-build2.done"
