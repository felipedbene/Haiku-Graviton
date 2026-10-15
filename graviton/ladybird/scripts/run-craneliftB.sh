#!/bin/sh
# Track B: fresh build dir with ENABLE_CRANELIFT_JIT=ON to prove LibWasm
# compiles+links with the Cranelift WASM JIT enabled (not stubbed).
exec >/boot/home/craneliftB.log 2>&1
export PATH=/boot/system/bin:$PATH
export CC=clang CXX=clang++
export PKG_CONFIG_PATH=/boot/home/lbdeps/lib/pkgconfig:/boot/system/develop/lib/pkgconfig

# ---- single-instance cargo guard (DeBeOS #638) -----------------------------
# Ladybird's build runs cargo (ninja's depth-1 "cargo" job pool serialises it
# *within* one ninja, but a job pool does not bind across processes). This is a
# long detached build, so one that looks stalled gets re-launched -- which puts
# a second cargo on the same ${CMAKE_BINARY_DIR}/cargo/build target dir. That is
# silently unsafe here: std's File::lock returns ErrorKind::Unsupported on Haiku
# and cargo treats an unsupported lock as ACQUIRED without warning, so neither
# side blocks and both interleave writes. The torn rlib then stays cached and
# rustc keeps ICEing on it. Keyed on $CARGO_HOME, so this also excludes the
# other cargo drivers on the box, which share the registry. Drop once rust_bin
# ships >= nightly-2026-10-13 (upstream rust-lang/rust#164031).
# See graviton/docs/rust-file-locking.md.
LOCK_DIR="${CARGO_HOME:-$HOME/.cargo}/.debeos-cargo.lock"
mkdir -p "${CARGO_HOME:-$HOME/.cargo}"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
	owner=$(cat "$LOCK_DIR/pid" 2>/dev/null || true)
	if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
		echo "== stale cargo lock from pid $owner -- taking it over =="
		rm -rf "$LOCK_DIR"
		mkdir "$LOCK_DIR"
	else
		echo "== ABORT: cargo already running here (pid ${owner:-unknown}); concurrent cargo corrupts the shared target dir on DeBeOS (#638) =="
		exit 1
	fi
fi
printf '%s\n' "$$" > "$LOCK_DIR/pid"
trap 'rm -rf "$LOCK_DIR"' EXIT

cd /boot/home/lb
echo "=== CONFIGURE START $(date) ==="
cmake -G Ninja -B build-craneliftB -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_LTO_FOR_RELEASE=OFF \
  -DENABLE_QT_UI=OFF \
  -DBUILD_TESTING=OFF \
  -DENABLE_CRANELIFT_JIT=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=/boot/home/lbdeps \
  -DFastFloat_DIR=/boot/home/lbdeps/share/cmake/FastFloat \
  -Dsimdutf_DIR=/boot/home/lbdeps/lib/cmake/simdutf \
  -DWUFFS_INCLUDE_DIR=/boot/home/lbdeps/include \
  -DPNG_PNG_INCLUDE_DIR=/boot/home/lbdeps/include \
  -DPNG_LIBRARY=/boot/home/lbdeps/lib/libpng16.so \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DCMAKE_C_COMPILER_AR=/boot/system/bin/ar -DCMAKE_CXX_COMPILER_AR=/boot/system/bin/ar \
  -DCMAKE_C_COMPILER_RANLIB=/boot/system/bin/ranlib -DCMAKE_CXX_COMPILER_RANLIB=/boot/system/bin/ranlib \
  -DCMAKE_LINKER_TYPE=LLD \
  -DCMAKE_C_USING_LINKER_LLD=-fuse-ld=lld -DCMAKE_CXX_USING_LINKER_LLD=-fuse-ld=lld \
  -DCMAKE_C_USING_LINKER_MODE=FLAG -DCMAKE_CXX_USING_LINKER_MODE=FLAG
echo "CONFIGURE_RC=$?"
grep -i "ENABLE_CRANELIFT_JIT" build-craneliftB/CMakeCache.txt
cd build-craneliftB
echo "=== BUILD liblagom-wasm.so START $(date) ==="
ninja -j32 liblagom-wasm.so
echo "WASM_BUILD_RC=$? SENTINEL_DONE $(date)"
