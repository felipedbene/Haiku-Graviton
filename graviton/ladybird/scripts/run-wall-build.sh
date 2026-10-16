#!/bin/sh
# One-shot driver that takes a fresh canonical-AMI Graviton builder from bare to
# a driven Ladybird build, following README.md's reproduction sequence. The
# README's ten steps were only ever run by hand, one SSH command at a time; on a
# box that has to be re-provisioned from scratch that is hours of babysitting
# and the ordering constraints (ffmpeg shims must be replaced before LibMedia
# links, Skia must exist before configure) are easy to get wrong. This encodes
# them.
#
# Usage, run natively on the builder as baron:
#     run-wall-build.sh <phase>...     # toolchain deps png icu ffmpeg lb
#                                      # skiafetch skiabuild pc configure build
#                                      # tommath avif (re-runs of deps subsets)
#     run-wall-build.sh all
#
# Each phase appends to its own log under $LOGDIR and prints a SENTINEL line on
# completion, so a detached run can be polled without a live SSH session.
#
# Concurrency: this box is shared between agents, so the build tree, the cargo
# home and the job count are all deliberately *not* the README defaults -- see
# BUILDDIR/CARGO_HOME below and the cargo lock in phase `build`.
set -e

REPO="${REPO:-/boot/home/debeos/Haiku-Graviton-graviton}"
RECIPE="$REPO/graviton/ladybird"
PREFIX=/boot/home/lbdeps
LOGDIR="${LOGDIR:-/boot/home/wall-logs}"

# The native Interface Kit chrome, referenced where it already lives rather than
# copied in: it is one 148 KB patch and two copies would drift. See
# phase_lbpatch for why it applies on top of this directory's patch.
UI_PATCH="$REPO/graviton/ports/ladybird/ladybird-haiku-ui.patch"

# A build tree and cargo home unique to this driver. cargo's target-dir locking
# is a silent no-op on Haiku (DeBeOS #638: std's File::lock answers
# ErrorKind::Unsupported, which cargo reads as "acquired"), so a second cargo on
# a shared target dir corrupts rlibs with no warning. Separate trees mean the
# only shared state left is the registry, which the phase-`build` lock covers.
BUILDDIR="${BUILDDIR:-build-599}"
export CARGO_HOME="${CARGO_HOME:-/boot/home/cargo-599}"

# 32 vCPU shared with another builder agent; leave headroom rather than pinning
# every core.
JOBS="${JOBS:-24}"

export PATH=/boot/system/bin:$PATH
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:/boot/system/develop/lib/pkgconfig

mkdir -p "$LOGDIR"

phase_toolchain() {
	pkgman install -y llvm21_clang llvm21_lld llvm21_libs cmake ninja gn \
		rust_bin pkgconf
	for t in clang clang++ cmake ninja gn cargo rustc; do
		printf '%-10s %s\n' "$t" "$(which "$t" 2>/dev/null || echo MISSING)"
	done
	clang --version 2>&1 | head -2
	rustc --version
	cargo --version
}

phase_deps() {
	sh "$RECIPE/scripts/provision-deps.sh"
	# A monospace family Ladybird's fallback list actually names has to be on
	# disk before anything that builds a FontPlugin will start -- the stock
	# image's only mono font is "Noto Mono", which is on neither Ladybird's nor
	# fontconfig's list. Assert the file, not the fontconfig view: the conf.d
	# rules are inert on DeBeOS (dangling symlinks, DeBeOS #730), so fc-match is
	# no witness.
	test -e /boot/system/data/fonts/ttfonts/DejaVuSansMono.ttf \
		|| { echo "no DejaVu Sans Mono -- FontPlugin will abort" >&2; return 1; }
}

# A subset of phase `deps` (provision-deps.sh calls the same script), broken out
# because getting libtommath wrong costs a full relink of liblagom-crypto and
# shows up only as a WebContent that will not start -- see build-tommath.sh.
phase_tommath() {
	PREFIX="$PREFIX" sh "$RECIPE/scripts/build-tommath.sh"
}

# Also a subset of phase `deps`. Broken out for the same reason: the repo's
# libavif 0.9.3 passes configure and then fails a compile 1900 targets in.
# libavif1.0 (1.4.2) is a real pool package now, so this is an install and an
# assertion rather than a source build -- see provision-deps.sh step 1b.
phase_avif() {
	pkgman uninstall -y libavif_devel || true
	pkgman install -y libavif1.0_devel
	grep -q repetitionCount /boot/system/develop/headers/avif/avif.h \
		|| { echo "installed avif.h has no avifDecoder::repetitionCount" >&2; return 1; }
}

# libpng16 from the repo is built without APNG and LibImageDecoders hard-requires
# PNG_APNG_SUPPORTED. Fetch the 1.6.53 tarball over HTTPS rather than cloning:
# the whole libpng history is not needed to apply one patch.
phase_png() {
	mkdir -p /boot/home/pngbuild
	cd /boot/home/pngbuild
	rm -rf libpng-1.6.53 libpng-1.6.53.tar.gz
	/boot/system/bin/curl -fsSL -o libpng-1.6.53.tar.gz \
		https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.53
	tar xzf libpng-1.6.53.tar.gz
	cd libpng-1.6.53
	patch -p1 --no-backup-if-mismatch < "$RECIPE/patches/libpng-1.6.53-apng.patch"
	# Without this one the build is arch-blind (uname -p says "other") and
	# produces a libpng16.so with undefined NEON symbols -- see the patch.
	patch -p1 --no-backup-if-mismatch < "$RECIPE/patches/libpng-1.6.53-haiku-arch.patch"
	sh "$RECIPE/scripts/build-png.sh"
	# The vendored patch writes the three APNG defines into png.h itself, not
	# into the generated pnglibconf.h, so png.h is where to check. Confirm the
	# library really exports an APNG entry point too: the defines alone would
	# also be satisfied by a header-only patch that never got compiled in.
	grep -q 'define PNG_APNG_SUPPORTED' "$PREFIX/include/png.h" \
		|| { echo "APNG missing from installed png.h" >&2; return 1; }
	nm -D "$PREFIX/lib/libpng16.so" | grep -q ' T png_read_frame_head' \
		|| { echo "libpng16.so does not export png_read_frame_head" >&2; return 1; }
	# The NEON paths are enabled by pngpriv.h from __ARM_NEON whatever CMake
	# thinks the architecture is, so an arch-blind configure yields a library
	# that links fine and then cannot be loaded at all. Catch that here rather
	# than in WebContent's runtime_loader output.
	nm -D -u "$PREFIX/lib/libpng16.so" | grep -q 'neon' \
		&& { echo "libpng16.so has undefined NEON symbols" >&2; return 1; }
	:
}

# ICU 78.3 into $PREFIX/icu78, beside (not over) the system icu74 that
# harfbuzz_devel and the rest of the C++ stack are built against. ICU suffixes
# every exported symbol and namespace with its major version (icu_74 vs
# icu_78), so the two coexist in one process by design -- which is the only
# reason Ladybird can use 78 while linking a harfbuzz that uses 74.
phase_icu() {
	JOBS="$JOBS" sh "$RECIPE/scripts/run-icu2.sh"
	grep -q '"78' "$PREFIX/icu78/include/unicode/uvernum.h" \
		|| { echo "icu78 headers are not 78.x" >&2; return 1; }
	test -e "$PREFIX/icu78/lib/libicuuc.so" \
		|| { echo "icu78 libicuuc.so missing" >&2; return 1; }
}

phase_ffmpeg() {
	sh "$RECIPE/scripts/run-ffmpeg.sh"
	for l in avcodec avformat avutil swresample; do
		test -e "$PREFIX/lib/lib$l.so" \
			|| { echo "ffmpeg lib$l.so missing" >&2; return 1; }
	done
}

phase_lb() {
	sh "$RECIPE/scripts/run-lb-clone.sh"
	phase_lbpatch
}

# Re-apply the Haiku patch stack to an existing checkout. Every iteration on
# ladybird-haiku.patch needs the tree back at 90998c5d first, and a re-clone of
# Ladybird's full history to undo a dozen edited lines is not a sensible price.
# `checkout -- .` only touches tracked files, so the build tree under
# /boot/home/lb/build-* (untracked) survives; deliberately no `clean`, which
# would delete it.
#
# TWO patches now, in this order, and the order is the whole point.
# ladybird-haiku.patch is the port; it leaves CMakeLists.txt at blob 8fa606df0a,
# which is exactly the pre-image ladybird-haiku-ui.patch was cut against (it was
# cut on top of ports/ladybird/ladybird-haiku-arm64-wave3.patch, whose
# CMakeLists.txt post-image is the same blob). LocalServer.cpp (34a9426c4d) and
# cmake_options.cmake (d290b5b2d6) likewise agree between the two stacks, and
# Process.cpp/UI/CMakeLists.txt are untouched by the first patch -- so all five
# shared files line up and the UI patch applies unmodified.
#
# The first patch keeps its original `patch -p1` invocation deliberately -- it is
# what every build so far has used against this exact checkout, and `git apply`
# is stricter about context, so swapping it would be an untested change to a
# working step. The UI patch gets `git apply` instead, because it creates 23 new
# files under UI/Haiku/ and that wants to be all-or-nothing: `patch` would
# cheerfully re-create them over a half-reverted tree, where `git apply` refuses
# an already-applied patch outright. Those untracked files also survive
# `checkout -- .`, so reverse the UI patch out before resetting, or the
# re-apply trips over its own output.
phase_lbpatch() {
	cd /boot/home/lb
	git apply -R --quiet "$UI_PATCH" 2>/dev/null || true
	git checkout -- .
	patch -p1 --no-backup-if-mismatch < "$RECIPE/patches/ladybird-haiku.patch"
	git apply "$UI_PATCH"
	test -f UI/Haiku/CMakeLists.txt \
		|| { echo "UI/Haiku missing -- the UI patch did not apply" >&2; return 1; }
}

# Split in two because the fetch is ~8 GB and run-skia-fetch.sh starts with
# `rm -rf skia`: anything that makes the compile fail (a bad patch, a missing
# gn) must not cost another full clone to retry. Note run-skia-fetch.sh exits
# non-zero even on success -- git-sync-deps' last step is bin/fetch-gn, whose
# platform map has no 'haiku' key, so it raises KeyError after every external
# has already been synced. gn comes from pkgman here, so that step is not
# needed; assert on the externals instead of on the exit status.
phase_skiafetch() {
	sh "$RECIPE/scripts/run-skia-fetch.sh" || true
	# DEPS lists ~46 externals; harfbuzz and icu are the ones args.gn actually
	# compiles, so a truncated sync shows up here rather than 40 minutes in.
	for d in harfbuzz icu libpng zlib; do
		test -d "/boot/home/skia/third_party/externals/$d" \
			|| { echo "skia external $d not synced" >&2; return 1; }
	done
}

phase_skiabuild() {
	cd /boot/home/skia
	patch -p1 --no-backup-if-mismatch < "$RECIPE/patches/skia-haiku.patch"
	JOBS="$JOBS" sh "$RECIPE/scripts/build-skia.sh"
}

# The *-ladybird.pc files provision-deps.sh wrote are configure-time shims
# pointing at a system ffmpeg that does not exist; the real ones in pkgconfig/
# point into $PREFIX. Copying after phase_ffmpeg is what makes LibMedia link.
phase_pc() {
	mkdir -p "$PREFIX/lib/pkgconfig"
	cp "$RECIPE"/pkgconfig/*.pc "$PREFIX/lib/pkgconfig/"
	ln -sf libpng16.pc "$PREFIX/lib/pkgconfig/libpng.pc"
	pkg-config --exists libavcodec-ladybird \
		|| { echo "libavcodec-ladybird.pc not visible" >&2; return 1; }

	# find_package(LIBAVIF) is a config-package lookup, not pkg-config, and
	# -DLIBAVIF_DIR points it at the pool package's own config under
	# /boot/system. That pin is explicit even though the package sits on the
	# default system prefix path and resolves without it: $PREFIX is on
	# CMAKE_PREFIX_PATH and outranks the system prefix, so a builder left over
	# from the retired from-source build would otherwise keep resolving to its
	# stale $PREFIX/lib/cmake/libavif. Asserted rather than shadowed:
	# libavif1.0 1.4.2's config advertises /boot/system/develop/headers, which
	# exists (the _IMPORT_PREFIX walk onto the non-existent
	# /boot/system/include, DeBeOS #709, was the 0.9.3 package's), and a
	# non-existent INTERFACE_INCLUDE_DIRECTORIES on an imported target is a
	# hard CMake generate-step error.
	test -e /boot/system/lib/cmake/libavif/libavif-config.cmake \
		|| { echo "libavif1.0_devel missing -- run phase avif" >&2; return 1; }
}

# ENABLE_HAIKU_UI=ON is what gets `bin/Ladybird` built, and without it WebDriver
# has no browser to launch (Services/WebDriver/main.cpp:216) -- that, not a
# missing port, is why this recipe had no rendered pixel. The two chrome options
# are mutually exclusive (cmake_options.cmake FATAL_ERRORs on both: each defines
# the `ladybird` target), so QT_UI stays OFF. Both are passed explicitly rather
# than leaning on the Haiku defaults, so the configure line says what it builds.
#
# The UI patch moves the helper services to libexec/. `rm -rf "$BUILDDIR"` above
# means that is never a mixed tree; a *reconfigure in place* of a previously
# headless tree would leave helper copies in both bin/ and libexec/, which
# package/build-package.sh refuses to package.
phase_configure() {
	cd /boot/home/lb
	rm -rf "$BUILDDIR"
	cmake -G Ninja -B "$BUILDDIR" -S . \
		-DCMAKE_BUILD_TYPE=Release \
		-DENABLE_LTO_FOR_RELEASE=OFF \
		-DENABLE_QT_UI=OFF \
		-DENABLE_HAIKU_UI=ON \
		-DBUILD_TESTING=OFF \
		-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
		-DCMAKE_PREFIX_PATH=$PREFIX \
		-DLIBAVIF_DIR=/boot/system/lib/cmake/libavif \
		-DICU_ROOT=$PREFIX/icu78 \
		-DFastFloat_DIR=$PREFIX/share/cmake/FastFloat \
		-Dsimdutf_DIR=$PREFIX/lib/cmake/simdutf \
		-DDEBEOS_ANGLE_SHIM_DIR="$REPO/graviton/ports/ladybird/angle-shim" \
		-DWUFFS_INCLUDE_DIR=$PREFIX/include \
		-DPNG_PNG_INCLUDE_DIR=$PREFIX/include \
		-DPNG_LIBRARY=$PREFIX/lib/libpng16.so \
		-DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
		-DCMAKE_C_COMPILER_AR=/boot/system/bin/ar \
		-DCMAKE_CXX_COMPILER_AR=/boot/system/bin/ar \
		-DCMAKE_C_COMPILER_RANLIB=/boot/system/bin/ranlib \
		-DCMAKE_CXX_COMPILER_RANLIB=/boot/system/bin/ranlib \
		-DCMAKE_LINKER_TYPE=LLD \
		-DCMAKE_C_USING_LINKER_LLD=-fuse-ld=lld \
		-DCMAKE_CXX_USING_LINKER_LLD=-fuse-ld=lld \
		-DCMAKE_C_USING_LINKER_MODE=FLAG \
		-DCMAKE_CXX_USING_LINKER_MODE=FLAG
}

phase_build() {
	# Single-instance cargo guard (DeBeOS #638). mkdir is the atomic primitive
	# here: flock/File::lock is the thing that does not work on BFS. Keyed on
	# CARGO_HOME, which is also the shared registry, so this excludes the other
	# cargo drivers on the box too.
	LOCK_DIR="$CARGO_HOME/.debeos-cargo.lock"
	mkdir -p "$CARGO_HOME"
	if ! mkdir "$LOCK_DIR" 2>/dev/null; then
		owner=$(cat "$LOCK_DIR/pid" 2>/dev/null || true)
		if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
			echo "== stale cargo lock from pid $owner -- taking it over =="
			rm -rf "$LOCK_DIR"
			mkdir "$LOCK_DIR"
		else
			echo "== ABORT: cargo already running here (pid ${owner:-unknown}) =="
			return 1
		fi
	fi
	printf '%s\n' "$$" > "$LOCK_DIR/pid"
	trap 'rm -rf "$LOCK_DIR"' EXIT

	cd "/boot/home/lb/$BUILDDIR"
	# `ladybird` is the native chrome (OUTPUT_NAME "Ladybird"); it is the target
	# WebDriver needs on PATH to start a session, and the one the headless-only
	# stack could not produce.
	ninja -j"$JOBS" ladybird WebContent WebDriver
	test -x bin/Ladybird \
		|| { echo "bin/Ladybird not produced" >&2; return 1; }
}

run_phase() {
	name=$1
	log="$LOGDIR/$name.log"
	echo "=== PHASE $name START $(date) ===" >> "$log"
	if ( set -x; "phase_$name" ) >> "$log" 2>&1; then
		echo "=== PHASE $name OK $(date) SENTINEL_${name}_OK ===" >> "$log"
		return 0
	fi
	echo "=== PHASE $name FAILED $(date) SENTINEL_${name}_FAIL ===" >> "$log"
	return 1
}

if [ "$1" = all ]; then
	set -- toolchain deps png icu ffmpeg lb skiafetch skiabuild pc configure \
		build
fi
test $# -gt 0 || { echo "usage: $0 <phase>... | all" >&2; exit 2; }

for p in "$@"; do
	run_phase "$p" || { echo "STOPPED at phase $p"; exit 1; }
done
echo "ALL PHASES OK SENTINEL_ALL_OK $(date)"
