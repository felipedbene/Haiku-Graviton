#!/bin/sh
# Build Skia (chrome/m148) as a CPU-raster static lib for arm64-Haiku.
# Prereqs: run-skia-fetch.sh has cloned+synced /boot/home/skia, and
# patches/skia-haiku.patch has been applied to it:
#   cd /boot/home/skia && git apply <repo>/graviton/ladybird/patches/skia-haiku.patch
# The repo's native `gn` (2385) and `ninja` are used; bin/fetch-gn is bypassed.
set -e
export PATH=/boot/system/bin:$PATH
cd /boot/home/skia
mkdir -p out/haiku-arm64
# skia-args.gn is the GN arg set (no GPU, no vendored codecs, system zlib).
cp /boot/home/ladybird-wave2/skia-args.gn out/haiku-arm64/args.gn
gn gen out/haiku-arm64
ninja -C out/haiku-arm64 -j32 skia
echo "SKIA BUILD DONE (libskia.a in out/haiku-arm64)"
# Then expose it to Ladybird via the pkg-config shim:
#   cp <repo>/graviton/ladybird/pkgconfig/skia.pc /boot/home/lbdeps/lib/pkgconfig/
