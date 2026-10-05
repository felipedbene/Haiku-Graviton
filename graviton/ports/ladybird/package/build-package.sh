#!/bin/sh
# Package the natively-built Ladybird browser into a DeBeOS/Haiku arm64 .hpkg.
#
# Dependency policy ------------------------------------------------------------
# Every runtime dependency is a normal `requires` against a repository package.
# Five of them diverge from what the repository shipped, so they are delivered
# as their own pool packages (see build-dep-packages.sh) rather than bundled
# privately: icu78 (net-new, coexists with icu74), libtommath 1.3.0 and
# simdjson 5.0.2 (in-place bumps, no reverse dependencies), libpng16-1.6.53-3
# (revision bump adding the ABI-additive APNG patch, required through its
# `libpng16_apng` capability) and ffmpeg7 (net-new).
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
#     The browser binary itself must therefore stay in apps/Ladybird/bin: at
#     apps/Ladybird/Ladybird the resource root would become apps/share/Lagom.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

B=${B:-/boot/home/lb/build-wave2}
OUT=${OUT:-/boot/home/lbpkg}
COMMIT=$(cut -c1-8 "$B/COMMIT")
VERSION="0~git${COMMIT}"
# Revision 2: same payload as 1; libpng16 is now required through the
# `libpng16_apng` capability instead of `libpng16 >= 1.6.53-2` (see step 7).
# Revision 3: adds the native DeBeOS browser front end (UI/Haiku, binary
# "Ladybird"), its Deskbar entry and the `ladybird` command; requires are now
# derived mechanically (derive-requires.sh) instead of typed.
REVISION=3
PKG="ladybird-${VERSION}-${REVISION}-arm64.hpkg"
R="$OUT/pkgroot"
P="$R/apps/Ladybird"

rm -rf "$R"
mkdir -p "$P/bin" "$P/lib" "$P/share" "$R/bin" "$R/data/licenses" \
         "$R/data/deskbar/menu/Applications"

# --- 1. binaries -------------------------------------------------------------
# Ladybird is the browser (the native front end in UI/Haiku); headless-shot is
# the screenshot tool; the rest are the helper processes the multi-process
# engine launches (WebContent/Compositor/RequestServer/ImageDecoder/WebWorker),
# plus WebDriver, and MediaServer/WasmCompiler/cranelift-compiler which
# WebContent spawns lazily for media and WASM.
# Once the UI is part of the build, UI/CMakeLists.txt moves the helpers to
# $B/libexec (set_helper_process_properties); everything else stays in $B/bin.
# The package keeps revision 1's flat bin/ layout, which find_prefix() and the
# helper search accept (Utilities.cpp:62-71, 96-110).
# libexec is searched FIRST, and a binary present in both directories is an
# error: a build dir that once built without the UI keeps its old helpers in
# $B/bin after a UI rebuild writes fresh ones to $B/libexec, and packaging the
# bin/ copy would ship stale helpers next to a new browser with nothing to
# notice it. Remove the stale bin/ copies (or use a fresh build dir) instead.
for b in Ladybird headless-shot WebContent Compositor RequestServer ImageDecoder \
         WebWorker WebDriver MediaServer WasmCompiler cranelift-compiler; do
    if [ -f "$B/bin/$b" ] && [ -f "$B/libexec/$b" ]; then
        echo "stale duplicate: $b is in both $B/bin and $B/libexec" >&2
        exit 1
    fi
    src=""
    for d in "$B/libexec" "$B/bin"; do
        if [ -f "$d/$b" ]; then src="$d/$b"; break; fi
    done
    [ -n "$src" ] || { echo "missing binary: $b (looked in $B/libexec and $B/bin)" >&2; exit 1; }
    cp "$src" "$P/bin/$b"
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

# --- 4b. Haiku resources of the browser binary ------------------------------
# Resources are trailing data after the ELF image and patchelf rewrites the
# file, so they are (re)attached after the rpath rewrite and checked for.
xres -o "$P/bin/Ladybird" "$B/UI/Haiku/Ladybird.rsrc"
listres "$P/bin/Ladybird" > "$OUT/Ladybird.listres"
grep -q 'BEOS:APP_SIG' "$OUT/Ladybird.listres" \
    || { echo "Ladybird: application signature resource missing" >&2; exit 1; }
if grep -q 'BEOS:ICON' "$OUT/Ladybird.listres"; then
    ICON_NOTE=""
else
    echo "WARNING: Ladybird has no vector icon; Deskbar shows the generic application icon" >&2
    ICON_NOTE=" It does not carry its own icon yet, so Deskbar and Tracker show the generic \
application icon."
fi

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
cat > "$R/bin/ladybird" <<'EOF'
#!/bin/sh
exec /boot/system/apps/Ladybird/bin/Ladybird "$@"
EOF
chmod +x "$R/bin/ladybird-headless-shot" "$R/bin/ladybird-webdriver" "$R/bin/ladybird"

# Post-install: rebuild the fontconfig cache. fontconfig validates its cache by
# directory mtime, but a packagefs directory keeps the package's build time when
# a font package is activated, so fonts installed alongside Ladybird (dejavu,
# noto_sans_cjk_sc) stay invisible to WebContent until something runs
# `fc-cache -f` (measured: fc-list showed 14 faces, 48 after a forced rebuild).
# package_daemon runs post-install scripts right after a live activation
# (src/servers/package/CommitTransactionHandler.cpp:436-441).
mkdir -p "$R/boot/post-install"
cat > "$R/boot/post-install/ladybird-fontconfig-cache.sh" <<'EOF'
#!/bin/sh
# Make fonts activated together with Ladybird visible to fontconfig.
fc-cache -f >/dev/null 2>&1 || true
EOF
chmod +x "$R/boot/post-install/ladybird-fontconfig-cache.sh"

# Deskbar > Applications > Ladybird. A symlink to the ELF, not to the wrapper:
# BRoster launches the resolved target (src/kits/app/Roster.cpp:2242-2249) and a
# shell script is not a BApplication. Same convention as WebPositive
# (build/jam/packages/WebPositive).
ln -s ../../../../apps/Ladybird/bin/Ladybird "$R/data/deskbar/menu/Applications/Ladybird"

# --- 6. licences (mandatory: every name in licenses{} must be bundled) -------
# Ladybird and SerenitySans are BSD-2; Skia, simdutf and fast_float are linked
# statically into liblagom-gfx / liblagom-ak, so their licences ship here too.
for l in "BSD (2-clause)" "BSD (3-clause)" "Apache v2" \
         "SIL Open Font License v1.1"; do
    cp "/boot/system/data/licenses/$l" "$R/data/licenses/$l"
done

# --- 6b. requires, derived from the payload -----------------------------------
# Revision 2's hand-typed list is kept only as the baseline the derivation is
# compared against: an entry that disappears needs a human to agree.
# derive-requires.sh attributes each library to the package packagefs says it
# came from, so it must run where the dependency packages are INSTALLED as
# packages. On the builder that means `pkgman install icu78 libtommath simdjson
# ffmpeg7` from the repository plus libpng16-1.6.53-3 (pool or local file)
# before this script; /boot/home/lbdeps is not packagefs and reports UNRESOLVED.
"$HERE/derive-requires.sh" "$R" "$OUT/requires"
sort -u > "$OUT/requires/baseline-r2.txt" <<'EOF'
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
libpng16_apng >= 1.6.53
libavif
brotli
libsdl3
EOF
# lib:libicudata >= 78 is DERIVED, not carried over by hand: the revision-3
# payload links libicudata.so.78 directly (measured: derive-requires.sh resolves
# it to icu78), so the libicudata policy line in derive-requires.sh produces it
# and the removed-entries gate below has nothing to excuse for it. Should a
# later payload stop linking it, the gate fires and a human decides; icu78 would
# still arrive through lib:libicuuc >= 78, which only icu78 provides.
comm -13 "$OUT/requires/baseline-r2.txt" "$OUT/requires/requires.txt" > "$OUT/requires/added.txt"
comm -23 "$OUT/requires/baseline-r2.txt" "$OUT/requires/requires.txt" > "$OUT/requires/removed.txt"
echo "=== requires added relative to revision 2:"
cat "$OUT/requires/added.txt"
echo "=== requires removed relative to revision 2:"
cat "$OUT/requires/removed.txt"
if [ -s "$OUT/requires/removed.txt" ] && [ "${ACCEPT_REQUIRES_DELTA:-0}" != 1 ]; then
    echo "requires lost entries relative to revision 2; review them, then rerun with ACCEPT_REQUIRES_DELTA=1" >&2
    exit 1
fi
REQUIRES=$(sed 's/^/\t/' "$OUT/requires/requires.txt")

# --- 7. .PackageInfo ---------------------------------------------------------
# libpng16: require `libpng16_apng`, NOT `libpng16 >= 1.6.53-2`.  Provides
# carry no revision, so the base image's libpng16-1.6.53-1 (`libpng16 =
# 1.6.53`) satisfies a revision-qualified expression and the solver never pulls
# the APNG build -- liblagom-imagedecoders then fails to resolve
# png_get_next_frame_fcTL at load.  Only libpng16 >= 1.6.53-3 provides
# libpng16_apng.  derive-requires.sh keeps that capability form.
cat > "$R/.PackageInfo" <<EOF
name			ladybird
version			${VERSION}-${REVISION}
architecture	arm64
summary			"The Ladybird web browser, native Haiku front end"
description		"Ladybird is an independent, standards-first web browser built on the \
LibWeb engine. This is a native DeBeOS/Haiku arm64 build (git ${COMMIT}) with a native \
Haiku front end written against the Be API: browser windows with tabs, a location \
field, back/forward/reload, JavaScript alert/confirm/prompt panels, and mouse, keyboard \
and scroll input. Start it from Deskbar > Applications > Ladybird or as \
'ladybird'.${ICON_NOTE} Pages render through the full multi-process pipeline \
(WebContent, Compositor, RequestServer, ImageDecoder, WebWorker) with Skia CPU \
rasterisation and live HTTPS fetching through RequestServer. DejaVu and Noto Sans CJK \
are pulled in so Arabic, Hebrew and CJK text has a fallback face. The headless screenshot \
tool ('ladybird-headless-shot') and a WebDriver server ('ladybird-webdriver') are \
included. There is no GL/GLES/EGL device on Haiku, so WebGL contexts fail honestly \
through a no-device ANGLE/EGL backend and pages fall back to CPU raster.

Ladybird's own libraries live under apps/Ladybird/lib and are reached through the \
binaries' \$ORIGIN-relative RPATH; nothing is installed into the shared library \
directory. Every third-party runtime dependency is a normal package requirement. \
Five of those needed new pool packages because the engine cannot load against what \
the repository shipped: icu78 (system icu74 has an incompatible SONAME), \
libtommath 1.3 (1.2.0 lacks mp_expt_n), libpng16-1.6.53-3 (the APNG build, required \
through its libpng16_apng capability; earlier revisions export no APNG symbols, which \
LibImageDecoders requires), simdjson 5.0.2 and ffmpeg7."
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
	app:Ladybird = ${VERSION}
	cmd:ladybird = ${VERSION}
	cmd:ladybird_headless_shot = ${VERSION}
	cmd:ladybird_webdriver = ${VERSION}
}
requires {
${REQUIRES}
}
post-install-scripts {
	"boot/post-install/ladybird-fontconfig-cache.sh"
}
EOF

# --- 8. build ----------------------------------------------------------------
cd "$OUT"
rm -f "$PKG"
package create -C pkgroot "$PKG"
echo "=== built $OUT/$PKG"
ls -l "$OUT/$PKG"
