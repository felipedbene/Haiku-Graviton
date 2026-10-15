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
# skia/args.gn is the GN arg set (no GPU, no vendored codecs, system zlib). It
# ships next to this script in the recipe; the live builder's copy under
# /boot/home/ladybird-wave2/ that this used to read does not exist on a freshly
# provisioned box.
ARGS_GN="${ARGS_GN:-$(cd -- "$(dirname -- "$0")/.." && pwd)/skia/args.gn}"
cp "$ARGS_GN" out/haiku-arm64/args.gn
gn gen out/haiku-arm64
ninja -C out/haiku-arm64 -j"${JOBS:-32}" skia
# PathOps (Op(SkPath, SkPath, SkPathOp)) is not in libskia.a with this arg set: m148 links the :pathops source set
# into the "skia" component only through :pdf (BUILD.gn: skia_component("skia") -> :pdf -> :pathops), and
# skia_enable_pdf is false. LibGfx calls Op(), so without this liblagom-gfx fails to load with
# "Could not resolve symbol '_Z2OpRK6SkPathS1_8SkPathOp'". Build the source set and add its objects.
ninja -C out/haiku-arm64 -j"${JOBS:-32}" pathops
ar rcs out/haiku-arm64/libskia.a $(find out/haiku-arm64/obj/src/pathops -name '*.o' | sort)
nm -C --defined-only out/haiku-arm64/libskia.a | grep -q " T Op(SkPath const&, SkPath const&, SkPathOp)" \
    || { echo "PathOps missing from libskia.a" >&2; exit 1; }
echo "SKIA BUILD DONE (libskia.a in out/haiku-arm64)"
# Then expose it to Ladybird via the pkg-config shim:
#   cp <repo>/graviton/ladybird/pkgconfig/skia.pc /boot/home/lbdeps/lib/pkgconfig/
