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
#                                      # skiafetch skiabuild pc configure
#                                      # craneliftcheck build wasmjit
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
#
# ENABLE_CRANELIFT_JIT=ON is passed explicitly for the same reason as the two
# chrome options: it is the upstream default, but a default is not a statement of
# intent, and the WASM JIT is the one feature on this recipe that can switch
# itself off without failing anything -- see phase_craneliftcheck.
phase_configure() {
	cd /boot/home/lb
	rm -rf "$BUILDDIR"
	cmake -G Ninja -B "$BUILDDIR" -S . \
		-DCMAKE_BUILD_TYPE=Release \
		-DENABLE_LTO_FOR_RELEASE=OFF \
		-DENABLE_CRANELIFT_JIT=ON \
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

	# Assert the WebGL codegen edge here, in the graph, rather than finding out
	# in a compile two hours away. LibCompositing compiles WebGL/WebGLCommandList.cpp
	# and the generated WebGL/WebGLCommands.cpp; both reach
	# <LibCompositing/WebGL/GLFunctions.h> through
	# WebGLCommands.h:8 (emitted by Meta/Generators/generate_libweb_webgl_commands.py).
	# Upstream never gives LibCompositing an edge to the generator that writes
	# that header -- only Services/Compositor declares GLFunctions.cpp, and the
	# generate_GLFunctions.cpp custom target is not ALL (Meta/CMake/utils.cmake,
	# add_custom_target without ALL), so `ninja ladybird WebContent WebDriver`
	# never asks for it and dies with
	# "'LibCompositing/WebGL/GLFunctions.h' file not found". patches/
	# ladybird-haiku.patch adds the missing
	# `add_dependencies(LibCompositing generate_GLFunctions.cpp)`; this checks
	# that it survived the last re-cut of that patch. Of the 63 generators in the
	# graph it is the only one whose output has no path from its consumer, which
	# is exactly why nothing else catches a dropped hunk (issue 599).
	if ! grep '^build cmake_object_order_depends_target_LibCompositing:' \
			"$BUILDDIR/build.ninja" \
			| grep -q 'Libraries/LibCompositing/generate_GLFunctions\.cpp'; then
		echo "LibCompositing has no order-only edge to generate_GLFunctions.cpp --" >&2
		echo "the ladybird-haiku.patch LibCompositing/CMakeLists.txt hunk is gone" >&2
		return 1
	fi
}

# Assert the WASM Cranelift JIT is really in the graph, straight after configure
# and before an hour of compiling.
#
# This exists because the JIT is the one feature here whose absence is
# *completely silent*. WASM_COMPILED_FAULT_RECOVERY_SUPPORTED is set by
# Libraries/LibWasm/CMakeLists.txt from a platform list that upstream writes as
# WIN32 / APPLE / Linux-on-x86_64|aarch64; Haiku is not on it, and the DeBeOS
# `elseif(HAIKU)` branch that puts it there keys off `uname -m` because
# CMAKE_SYSTEM_PROCESSOR arrives as "other" (DeBeOS #701). If that probe ever
# comes back empty the flag goes to 0, and a 0 flag does not drop fault recovery
# alone -- try_cranelift_compile() returns false outright
# (Libraries/LibWasm/CraneliftBridge.cpp:1293), so every function falls back to
# the bytecode interpreter. Nothing fails. CMake prints nothing, ninja goes
# green, liblagom-wasm still links the bridge translation unit, and the recipe
# ships a browser whose WASM JIT never compiles a single function. The earlier
# ports/ladybird/ladybird-haiku-arm64-wave3.patch stack did exactly that for a
# whole wave while its notes recorded "cranelift ON" (issue 599, Track B).
#
# So: check the switch, the flag, and the object file. The object file is the
# part that cannot be argued with -- CraneliftStubs.cpp is the no-JIT
# translation unit, and if it is the one in the graph there is no JIT at all.
phase_craneliftcheck() {
	cd "/boot/home/lb/$BUILDDIR"
	grep -q '^ENABLE_CRANELIFT_JIT:BOOL=ON$' CMakeCache.txt \
		|| { echo "ENABLE_CRANELIFT_JIT is not ON in CMakeCache.txt" >&2; return 1; }
	grep -q 'WASM_COMPILED_FAULT_RECOVERY_SUPPORTED=1' build.ninja \
		|| { echo "WASM_COMPILED_FAULT_RECOVERY_SUPPORTED is not 1 -- the JIT would be inert (uname -m probe in LibWasm/CMakeLists.txt?)" >&2; return 1; }
	# `if`, not `cmd && { ...; }`: under `set -e` a grep that correctly finds
	# nothing would otherwise abort the phase as a false failure.
	if grep -q 'WASM_COMPILED_FAULT_RECOVERY_SUPPORTED=0' build.ninja; then
		echo "some target still compiles with WASM_COMPILED_FAULT_RECOVERY_SUPPORTED=0" >&2
		return 1
	fi
	grep -q 'CraneliftBridge\.cpp\.o' build.ninja \
		|| { echo "CraneliftBridge.cpp.o is not in the build graph" >&2; return 1; }
	if grep -q 'CraneliftStubs\.cpp\.o' build.ninja; then
		echo "CraneliftStubs.cpp.o is in the build graph -- this is the stubbed, no-JIT LibWasm" >&2
		return 1
	fi
	echo "cranelift JIT is in the graph: flag 1, CraneliftBridge.cpp.o, no stubs"
}

# Single-instance cargo guard (DeBeOS #638). mkdir is the atomic primitive here:
# flock/File::lock is the thing that does not work on BFS. Keyed on CARGO_HOME,
# which is also the shared registry, so this excludes the other cargo drivers on
# the box too. Every phase that can reach a Rust crate has to take it.
take_cargo_lock() {
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
}

phase_build() {
	take_cargo_lock || return 1
	cd "/boot/home/lb/$BUILDDIR"
	# `ladybird` is the native chrome (OUTPUT_NAME "Ladybird"); it is the target
	# WebDriver needs on PATH to start a session, and the one the headless-only
	# stack could not produce.
	ninja -j"$JOBS" ladybird WebContent WebDriver
	test -x bin/Ladybird \
		|| { echo "bin/Ladybird not produced" >&2; return 1; }
}

# Run the WASM Cranelift JIT, having asserted in phase_craneliftcheck that it is
# in the graph. The graph check catches the switch regressing; this catches the
# JIT regressing -- a bridge that compiles and links but emits nothing, or a
# fault-recovery path that kills the process instead of raising a WASM trap.
#
# The module is 80 bytes of hand-assembled WASM rather than a checked-in fixture
# or a wat2wasm dependency: three exported functions is the whole fixture, and
# `i32.div_s` and `i32.load` are chosen because they are the two instructions
# that reach the signal paths the DeBeOS patch adds. A division by zero is a
# cranelift-emitted trap instruction (SIGILL/SIGFPE) decoded through the
# function's trap table by cranelift_trap_message(); a load past the memory is a
# guard-page SIGSEGV resumed through uc_mcontext.elr. Both of those are
# DeBeOS-specific code, and neither is exercised by anything else in the recipe.
phase_wasmjit() {
	take_cargo_lock || return 1
	cd "/boot/home/lb/$BUILDDIR"
	ninja -j"$JOBS" wasm
	test -x bin/wasm || { echo "bin/wasm not produced" >&2; return 1; }

	mod=/boot/home/jit-probe.wasm
	python3.10 - "$mod" <<-'PY'
		import struct, sys
		def sec(i, p): return bytes([i, len(p)]) + p
		def exp(n, i): return bytes([len(n)]) + n.encode() + bytes([0, i])
		def body(b): return bytes([len(b)]) + b
		types = bytes([2, 0x60, 2, 0x7f, 0x7f, 1, 0x7f, 0x60, 1, 0x7f, 1, 0x7f])
		mod = (b"\0asm" + struct.pack("<I", 1)
		    + sec(1, types)                                   # (i32,i32)->i32, (i32)->i32
		    + sec(3, bytes([3, 0, 0, 1]))                     # 3 funcs
		    + sec(5, bytes([1, 0, 1]))                        # 1 memory, min 1 page
		    + sec(7, bytes([3]) + exp("add", 0) + exp("dvs", 1) + exp("ld", 2))
		    + sec(10, bytes([3])
		        + body(bytes([0, 0x20, 0, 0x20, 1, 0x6a, 0x0b]))      # i32.add
		        + body(bytes([0, 0x20, 0, 0x20, 1, 0x6d, 0x0b]))      # i32.div_s
		        + body(bytes([0, 0x20, 0, 0x28, 2, 0, 0x0b]))))       # i32.load
		open(sys.argv[1], "wb").write(mod)
	PY

	export LIBRARY_PATH="/boot/home/lb/$BUILDDIR/lib:$LIBRARY_PATH"

	# --dump-native prints a byte count per cranelift-compiled function. A 0 flag
	# makes try_cranelift_compile() return false and this list comes out empty,
	# which is the inert-JIT signature. (The disassembly itself needs objdump at
	# the hard-coded /usr/bin/objdump, which does not exist here; the byte count
	# is the part that matters.)
	./bin/wasm --dump-native -i "$mod" > jit-dump.txt 2>&1 || true
	grep -q "Function #0 'add' ([1-9][0-9]* bytes)" jit-dump.txt \
		|| { echo "no native code emitted for 'add' -- the JIT is inert" >&2; cat jit-dump.txt >&2; return 1; }

	# bin/wasm pushes --arg onto a stack, so the LAST --arg is parameter 0.
	# Reading that backwards makes a correct JIT look like it swaps every
	# non-commutative operand: these two are div_s(12, 5) and div_s(12, 0).
	./bin/wasm -e dvs --arg i32.const:5 --arg i32.const:12 "$mod" 2>&1 | grep -q '^  -> 2$' \
		|| { echo "i32.div_s(12, 5) did not return 2" >&2; return 1; }
	./bin/wasm -e dvs --arg i32.const:0 --arg i32.const:12 "$mod" 2>&1 \
		| grep -q 'Execution trapped: Integer division overflow' \
		|| { echo "i32.div_s by zero did not raise a WASM trap (cranelift trap table?)" >&2; return 1; }
	./bin/wasm -e ld --arg i32.const:1048576 "$mod" 2>&1 \
		| grep -q 'Execution trapped: Memory access out of bounds' \
		|| { echo "out-of-bounds load did not raise a WASM trap (uc_mcontext.elr resume?)" >&2; return 1; }
	echo "cranelift JIT emits native code and both fault-recovery paths trap"
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
		craneliftcheck build wasmjit
fi
test $# -gt 0 || { echo "usage: $0 <phase>... | all" >&2; exit 2; }

for p in "$@"; do
	run_phase "$p" || { echo "STOPPED at phase $p"; exit 1; }
done
echo "ALL PHASES OK SENTINEL_ALL_OK $(date)"
