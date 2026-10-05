#!/bin/sh
# Package Ladybird's five DIVERGING runtime dependencies as proper DeBeOS pool
# packages, so the ladybird hpkg can `requires` them normally instead of
# shipping private copies.
#
# Why each one, and what the blast radius is:
#   icu78       NET-NEW, coexists.  The repo already carries nine versioned ICU
#               runtimes (icu, icu66, icu67, icu70, icu73..icu77); icu78 is the
#               tenth.  Distinct SONAMEs (libicuuc.so.78 vs .74), so nothing
#               that links icu74 is touched.
#   libtommath  IN-PLACE BUMP 1.2.0 -> 1.3.0.  System 1.2.0 does not export
#               mp_expt_n (verified with nm -D); LibCrypto needs it.  Only
#               reverse dependency in the repo is libtommath_devel.
#   simdjson    IN-PLACE BUMP 3.11.5 -> 5.0.2.  Different SONAME anyway
#               (libsimdjson.so.24 -> .so.34); zero reverse dependencies.
#   libpng16    REVISION BUMP 1.6.53-1 -> 1.6.53-3, adding the upstream APNG
#               patch and the ARM NEON objects, and a `libpng16_apng` provides
#               that names the APNG capability (see the libpng16 section).  APNG is ABI-ADDITIVE: it adds
#               png_get_acTL()/png_get_next_frame_fcTL() and keeps every
#               existing symbol, so the ~70 existing consumers keep working.
#               Verified: system 1.6.53-1 exports 0 acTL symbols, this build
#               exports 4 -- which is exactly why LibImageDecoders cannot load
#               against the repo build.
#   ffmpeg7     NET-NEW.  Nothing in the repo provides lib:libavcodec at all
#               (ffmpeg_x264 is not a shared-library package), so there is
#               nothing to collide with.  LibMedia needs libswresample.
#
# PROVENANCE NOTE: these hpkgs are hand-packaged from the artifacts that the
# Ladybird engine on this builder was actually linked and render-verified
# against.  haikuports recipes for all five are staged in the PR as the
# rebuild path; they are not used here because (a) the engine binaries are
# linked against *these* builds and a haikuporter rebuild would need a full
# Ladybird relink before it could be re-verified, and (b) all of these had to
# be relinked with `-z noseparate-code` for Haiku's runtime_loader, which the
# stock recipes do not yet do.
set -eu

D=${D:-/boot/home/lbdeps}
OUT=${OUT:-/boot/home/lbpkg}
mkdir -p "$OUT"
cd "$OUT"

ICU_VER=$(sed -n 's/^Version: *//p' "$D/icu78/lib/pkgconfig/icu-uc.pc" | head -1)
: "${ICU_VER:=78.3}"
FFM_VER=$(sed -n 's/.*FFMPEG_VERSION *"\(.*\)".*/\1/p' "$D/include/libavutil/ffversion.h" 2>/dev/null | head -1)
FFM_VER=${FFM_VER#n}           # ffversion.h carries the git tag ("n7.1"); a
: "${FFM_VER:=7.1}"            # Haiku package version must start with a digit
echo "ICU_VER=$ICU_VER FFMPEG_VER=$FFM_VER"

# copy_lib <srcfile> <destdir>: copy the real .so and recreate the SONAME
# symlinks that pointed at it (NEEDED names are the symlinks, not the realname)
copy_lib() {
    src=$1; dst=$2; dir=$(dirname "$src"); base=$(basename "$src")
    mkdir -p "$dst"; cp "$src" "$dst/$base"
    for l in "$dir"/*.so "$dir"/*.so.*; do
        [ -L "$l" ] || continue
        [ "$(basename "$(readlink "$l")")" = "$base" ] || continue
        ln -sf "$base" "$dst/$(basename "$l")"
    done
}

mklic() { # mklic <pkgroot> <license-name>...
    r=$1; shift; mkdir -p "$r/data/licenses"
    for l in "$@"; do cp "/boot/system/data/licenses/$l" "$r/data/licenses/$l"; done
}

################################################################ icu78
R=$OUT/root-icu78; rm -rf "$R"; mkdir -p "$R/lib"
for f in "$D"/icu78/lib/libicuuc.so.78.3 "$D"/icu78/lib/libicui18n.so.78.3 \
         "$D"/icu78/lib/libicudata.so.78.3 "$D"/icu78/lib/libicuio.so.78.3 \
         "$D"/icu78/lib/libicutu.so.78.3; do copy_lib "$f" "$R/lib"; done
mklic "$R" "ICU"
cat > "$R/.PackageInfo" <<EOF
name			icu78
version			${ICU_VER}-1
architecture	arm64
summary			"Libraries to support Unicode and globalization"
description		"International Components for Unicode (ICU) is a mature, widely used set of C/C++ libraries providing Unicode and Globalization support for software applications. This is the ICU ${ICU_VER} runtime for DeBeOS arm64; it installs alongside the other versioned ICU runtimes in the repository (its SONAMEs are libicu*.so.78, distinct from icu74's libicu*.so.74), so existing icu74 consumers are unaffected."
packager		"DeBeOS Haiku-Graviton"
vendor			"DeBeOS"
copyrights {
	"2016 and later: Unicode, Inc. and others."
	"1995-2020 IBM Corporation and others."
}
licenses { "ICU" }
provides {
	icu78 = ${ICU_VER} compat >= 78
	lib:libicudata = ${ICU_VER} compat >= 78
	lib:libicui18n = ${ICU_VER} compat >= 78
	lib:libicuio = ${ICU_VER} compat >= 78
	lib:libicutu = ${ICU_VER} compat >= 78
	lib:libicuuc = ${ICU_VER} compat >= 78
}
requires {
	haiku
	lib:libstdc++
}
EOF
package create -C "$R" "icu78-${ICU_VER}-1-arm64.hpkg" >/dev/null

################################################################ libtommath
R=$OUT/root-libtommath; rm -rf "$R"; mkdir -p "$R/lib"
copy_lib "$D/lib/libtommath.so.1.3.0" "$R/lib"
mklic "$R" "Public Domain"
cat > "$R/.PackageInfo" <<'EOF'
name			libtommath
version			1.3.0-1
architecture	arm64
summary			"A theoretic integer library written entirely in C"
description		"LibTomMath is a free open source portable number theoretic multiple-precision integer library written entirely in C. This 1.3.0 build supersedes the repository's 1.2.0, which does not export mp_expt_n(); Ladybird's LibCrypto needs it. The SONAME is unchanged (libtommath.so.1) and 1.3.0 is API/ABI-additive over 1.2.0."
packager		"DeBeOS Haiku-Graviton"
vendor			"DeBeOS"
copyrights { "2010-present Tom St. Denis" }
licenses { "Public Domain" }
provides {
	libtommath = 1.3.0
	lib:libtommath = 1.3.0 compat >= 1
}
requires {
	haiku
}
EOF
package create -C "$R" "libtommath-1.3.0-1-arm64.hpkg" >/dev/null

################################################################ simdjson
R=$OUT/root-simdjson; rm -rf "$R"; mkdir -p "$R/lib"
copy_lib "$D/lib/libsimdjson.so.34.0.0" "$R/lib"
mklic "$R" "Apache v2"
cat > "$R/.PackageInfo" <<'EOF'
name			simdjson
version			5.0.2-1
architecture	arm64
summary			"Parsing gigabytes of JSON per second"
description		"The simdjson library uses commonly available SIMD instructions and microparallel algorithms to parse JSON very quickly. This 5.0.2 build supersedes the repository's 3.11.5; the SONAME moves from libsimdjson.so.24 to libsimdjson.so.34, and nothing in the repository depends on the old one."
packager		"DeBeOS Haiku-Graviton"
vendor			"DeBeOS"
copyrights { "2018-present Daniel Lemire, Geoff Langdale and John Keiser" }
licenses { "Apache v2" }
provides {
	simdjson = 5.0.2
	lib:libsimdjson = 34.0.0 compat >= 34
}
requires {
	haiku
	lib:libstdc++
}
EOF
package create -C "$R" "simdjson-5.0.2-1-arm64.hpkg" >/dev/null

################################################################ libpng16 (APNG)
R=$OUT/root-libpng16; rm -rf "$R"; mkdir -p "$R/lib"
# NOT $D/lib: that copy was linked WITHOUT a symbol-version script, so it
# carried no PNG16_0 version node.  freetype, pngfix and PNGTranslator all
# record "File: libpng16.so.16 / Name: PNG16_0" version requirements, and
# Haiku's loader only tolerates an unversioned library there by accident of the
# file basename differing from the SONAME (elf_symbol_lookup.cpp:124, whose own
# comment says "That should actually be kind of fatal!").  $PNGVERS is a rebuild
# with --version-script, so rev 2 is a genuine ABI superset of rev 1: same 257
# exported names under the same PNG16_0 node, plus the 22 APNG entry points.
#
# Revision 3 has the same payload as revision 2 plus one provides entry,
# `libpng16_apng`.  Revision 2 relied on ladybird requiring
# `libpng16 >= 1.6.53-2`, which does not work: provides carry no revision, so
# revision 1's `libpng16 = 1.6.53` satisfies that expression and pkgman kept
# the non-APNG library (runtime_loader: could not resolve
# png_get_next_frame_fcTL).  The capability a consumer needs has to be its own
# resolvable.  Every existing provides line stays as it was, so
# libpng16_devel's `libpng16 == 1.6.53` is still satisfied.  The description
# is intentionally left as revision 2's so the rebuilt payload is unchanged.
PNGVERS=${PNGVERS:-/boot/home/pngbuild/vers/.libs}
copy_lib "$PNGVERS/libpng16.so.16.53.0" "$R/lib"
rm -f "$R/lib/libpng.so" "$R/lib/libpng16.so"   # unversioned links belong to _devel
mklic "$R" "LibPNG"
cat > "$R/.PackageInfo" <<'EOF'
name			libpng16
version			1.6.53-3
architecture	arm64
summary			"Portable Network Graphics library"
description		"libpng is the official PNG reference library. Revision 2 adds the upstream libpng-apng patch (PNG_APNG_SUPPORTED: png_get_acTL(), png_get_next_frame_fcTL(), ...) and the ARM NEON optimised objects. Ladybird's LibImageDecoders hard-requires APNG, and revision 1 exports none of those symbols. APNG is ABI-additive -- every symbol revision 1 exported is still exported -- so existing consumers are unaffected."
packager		"DeBeOS Haiku-Graviton"
vendor			"DeBeOS"
copyrights {
	"1995-2025 The PNG Reference Library Authors"
	"2018-2024 Cosmin Truta"
	"2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson"
	"1996-1997 Andreas Dilger"
	"1995-1996 Guy Eric Schalnat, Group 42, Inc."
}
licenses { "LibPNG" }
provides {
	libpng16 = 1.6.53 compat >= 1.6
	lib:libpng16 = 16.53.0 compat >= 16
	libpng16_apng = 1.6.53
}
requires {
	haiku
	lib:libz
}
EOF
package create -C "$R" "libpng16-1.6.53-3-arm64.hpkg" >/dev/null

################################################################ ffmpeg7
R=$OUT/root-ffmpeg7; rm -rf "$R"; mkdir -p "$R/lib"
for f in "$D"/lib/libavutil.so.59.39.100 "$D"/lib/libavcodec.so.61.19.100 \
         "$D"/lib/libavformat.so.61.7.100 "$D"/lib/libavfilter.so.10.4.100 \
         "$D"/lib/libavdevice.so.61.3.100 "$D"/lib/libswresample.so.5.3.100 \
         "$D"/lib/libswscale.so.8.3.100; do copy_lib "$f" "$R/lib"; done
rm -f "$R"/lib/libav*.so "$R"/lib/libsw*.so   # unversioned links belong to _devel
mklic "$R" "GNU LGPL v2.1"
cat > "$R/.PackageInfo" <<EOF
name			ffmpeg7
version			${FFM_VER}-1
architecture	arm64
summary			"Audio and video recording, conversion and streaming libraries (7.x)"
description		"FFmpeg is a collection of libraries and tools to process multimedia content. This is the ${FFM_VER} shared-library runtime for DeBeOS arm64, built LGPL (no --enable-gpl). It is net-new: nothing in the repository provided lib:libavcodec or lib:libswresample before, so there is nothing to collide with. Ladybird's LibMedia links libswresample."
packager		"DeBeOS Haiku-Graviton"
vendor			"DeBeOS"
copyrights { "2000-present the FFmpeg developers" }
licenses { "GNU LGPL v2.1" }
provides {
	ffmpeg7 = ${FFM_VER} compat >= 7
	lib:libavcodec = 61.19.100 compat >= 61
	lib:libavdevice = 61.3.100 compat >= 61
	lib:libavfilter = 10.4.100 compat >= 10
	lib:libavformat = 61.7.100 compat >= 61
	lib:libavutil = 59.39.100 compat >= 59
	lib:libswresample = 5.3.100 compat >= 5
	lib:libswscale = 8.3.100 compat >= 8
}
requires {
	haiku
	lib:libbz2
	lib:libiconv
	lib:libz
}
EOF
package create -C "$R" "ffmpeg7-${FFM_VER}-1-arm64.hpkg" >/dev/null

echo "=== dep packages built ==="
ls -l "$OUT"/*.hpkg
for p in "$OUT"/icu78-*.hpkg "$OUT"/libtommath-*.hpkg "$OUT"/simdjson-*.hpkg "$OUT"/libpng16-*.hpkg "$OUT"/ffmpeg7-*.hpkg; do
    echo "---- $(basename "$p")"; package list -i "$p" | grep -E '^\tprovides|^\trequires|^\tversion'
done
