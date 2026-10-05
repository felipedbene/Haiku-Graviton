#!/bin/sh
# Package the natively-built Ladybird browser into a DeBeOS/Haiku arm64 .hpkg.
#
# Dependency policy ------------------------------------------------------------
# Every runtime dependency is a normal `requires` against a repository package.
# Five of them diverge from what the repository shipped, so they are delivered
# as their own pool packages (see build-dep-packages.sh) rather than bundled
# privately: icu78 (net-new, coexists with icu74), libtommath 1.3.0 and
# simdjson 5.0.2 (in-place bumps, no reverse dependencies), libpng16-1.6.53-2
# (revision bump adding the ABI-additive APNG patch) and ffmpeg7 (net-new).
# Nothing in this package lands in /boot/system/lib.
#
# Layout rationale ------------------------------------------------------------
# Ladybird's own libraries (liblagom-*) and its no-device ANGLE/EGL backend
# (libdebeos_angle_shim) are Ladybird components, not system libraries, so they
# stay inside the package under apps/Ladybird/lib.  That works unmodified
# because:
#   * the binaries already carry RPATH "$ORIGIN:$ORIGIN/../lib" from the CMake
#     build, and Haiku's runtime_loader searches DT_RPATH/DT_RUNPATH *before*
#     LIBRARY_PATH and the system paths (runtime_loader.cpp, open_executable()),
#     with $ORIGIN resolved against the requesting object -- so no relink and no
#     LIBRARY_PATH is needed (and on Haiku LIBRARY_PATH *replaces* the loader
#     path rather than prepending to it, which is why rpath is the right tool);
#   * Ladybird computes its resource root as find_prefix(application_directory)
#     + "share/Lagom" and looks for helper processes in <prefix>/bin
#     (Libraries/LibWebCommon/WebView/Utilities.cpp), so bin/ + lib/ +
#     share/Lagom under one prefix is exactly the layout the code expects.
set -eu

B=${B:-/boot/home/lb/build-wave2}
OUT=${OUT:-/boot/home/lbpkg}
COMMIT=$(cut -c1-8 "$B/COMMIT")
VERSION="0~git${COMMIT}"
REVISION=1
PKG="ladybird-${VERSION}-${REVISION}-arm64.hpkg"
R="$OUT/pkgroot"
P="$R/apps/Ladybird"

rm -rf "$R"
mkdir -p "$P/bin" "$P/lib" "$P/share" "$R/bin" "$R/data/licenses"

# --- 1. binaries -------------------------------------------------------------
# headless-shot is the chrome we drive; the rest are the helper processes the
# multi-process engine launches (WebContent/Compositor/RequestServer/
# ImageDecoder/WebWorker), plus WebDriver, and MediaServer/WasmCompiler/
# cranelift-compiler which WebContent spawns lazily for media and WASM.
for b in headless-shot WebContent Compositor RequestServer ImageDecoder \
         WebWorker WebDriver MediaServer WasmCompiler cranelift-compiler; do
    cp "$B/bin/$b" "$P/bin/$b"
done

# --- 2. Ladybird's own libraries --------------------------------------------
# copy_lib <srcfile>: copy a real .so and recreate every SONAME symlink that
# pointed at it (the NEEDED names are the symlinks, not the real names).
copy_lib() {
    src=$1; dir=$(dirname "$src"); base=$(basename "$src")
    cp "$src" "$P/lib/$base"
    for l in "$dir"/*.so "$dir"/*.so.*; do
        [ -L "$l" ] || continue
        [ "$(basename "$(readlink "$l")")" = "$base" ] || continue
        ln -sf "$base" "$P/lib/$(basename "$l")"
    done
}
for f in "$B"/lib/liblagom-*.so.0.1.0; do copy_lib "$f"; done
cp "$B/lib/libdebeos_angle_shim.so" "$P/lib/"

# --- 3. runtime resources ----------------------------------------------------
# ladybird_build_resource_files stages fonts/icons/themes/about-pages/
# site-compatibility here; HeadlessWebView needs them to lay out any page.
cp -r "$B/share/Lagom" "$P/share/Lagom"

# --- 4. drop the build-host absolute path out of RPATH -----------------------
# The build set RPATH to "$ORIGIN:$ORIGIN/../lib:/boot/home/lbdeps/lib" on some
# targets.  A shipped artifact must not reference the builder's home.  patchelf
# only shortens the existing DT_RPATH string, so the LOAD/RELRO segment layout
# (which Haiku's runtime_loader is strict about) is untouched.
for f in "$P"/bin/* "$P"/lib/*.so "$P"/lib/*.so.*; do
    [ -L "$f" ] && continue
    readelf -d "$f" 2>/dev/null | grep -q 'RPATH\|RUNPATH' || continue
    patchelf --set-rpath '$ORIGIN:$ORIGIN/../lib' "$f"
done

# --- 5. launchers in bin/ ----------------------------------------------------
# Wrappers, not symlinks: $ORIGIN must resolve against the real binary's own
# directory, and exec'ing the absolute path guarantees that.
cat > "$R/bin/ladybird-headless-shot" <<'EOF'
#!/bin/sh
exec /boot/system/apps/Ladybird/bin/headless-shot "$@"
EOF
cat > "$R/bin/ladybird-webdriver" <<'EOF'
#!/bin/sh
exec /boot/system/apps/Ladybird/bin/WebDriver "$@"
EOF
chmod +x "$R/bin/ladybird-headless-shot" "$R/bin/ladybird-webdriver"

# --- 6. licences (mandatory: every name in licenses{} must be bundled) -------
# Ladybird and SerenitySans are BSD-2; Skia, simdutf and fast_float are linked
# statically into liblagom-gfx / liblagom-ak, so their licences ship here too.
for l in "BSD (2-clause)" "BSD (3-clause)" "Apache v2" \
         "SIL Open Font License v1.1"; do
    cp "/boot/system/data/licenses/$l" "$R/data/licenses/$l"
done

# --- 7. .PackageInfo ---------------------------------------------------------
cat > "$R/.PackageInfo" <<EOF
name			ladybird
version			${VERSION}-${REVISION}
architecture	arm64
summary			"Ladybird, an independent web browser engine (DeBeOS arm64 build)"
description		"Ladybird is an independent, standards-first web browser built on the \
LibWeb engine. This is a native DeBeOS/Haiku arm64 build (git ${COMMIT}): the full \
multi-process pipeline (WebContent, Compositor, RequestServer, ImageDecoder, \
WebWorker) with Skia CPU rasterisation, live HTTPS fetching through RequestServer, \
and a headless screenshot front end driven as 'ladybird-headless-shot'. A WebDriver \
server is included. There is no GL/GLES/EGL device on Haiku, so WebGL contexts fail \
honestly through a no-device ANGLE/EGL backend and pages fall back to CPU raster.

Ladybird's own libraries live under apps/Ladybird/lib and are reached through the \
binaries' \$ORIGIN-relative RPATH; nothing is installed into the shared library \
directory. Every third-party runtime dependency is a normal package requirement. \
Five of those needed new pool packages because the engine cannot load against what \
the repository shipped: icu78 (system icu74 has an incompatible SONAME), \
libtommath 1.3 (1.2.0 lacks mp_expt_n), libpng16-1.6.53-2 (revision 1 exports no \
APNG symbols, which LibImageDecoders requires), simdjson 5.0.2 and ffmpeg7."
packager		"DeBeOS Haiku-Graviton"
vendor			"DeBeOS"
copyrights {
	"2018-present the Ladybird developers"
	"2018-present the SerenityOS developers"
	"2011-present Google Inc. (Skia)"
	"2021-present The simdutf authors"
}
licenses {
	"BSD (2-clause)"
	"BSD (3-clause)"
	"Apache v2"
	"SIL Open Font License v1.1"
}
provides {
	ladybird = ${VERSION}
	cmd:ladybird_headless_shot = ${VERSION}
	cmd:ladybird_webdriver = ${VERSION}
}
requires {
	haiku
	gcc_syslibs
	openssl3
	curl
	libfmt
	fontconfig
	harfbuzz
	libjpeg_turbo
	mimalloc
	libpsl
	sqlite
	libwebp
	woff2
	libxml2
	zlib
	lib:libicuuc >= 78
	lib:libicui18n >= 78
	lib:libicudata >= 78
	lib:libtommath >= 1.3
	lib:libsimdjson >= 34
	lib:libavcodec >= 61
	lib:libavformat >= 61
	lib:libavutil >= 59
	lib:libswresample >= 5
	libpng16 >= 1.6.53-2
	libavif
	brotli
	libsdl3
}
EOF

# --- 8. build ----------------------------------------------------------------
cd "$OUT"
rm -f "$PKG"
package create -C pkgroot "$PKG"
echo "=== built $OUT/$PKG"
ls -l "$OUT/$PKG"
