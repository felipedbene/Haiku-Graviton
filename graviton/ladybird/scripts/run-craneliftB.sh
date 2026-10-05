#!/bin/sh
# Track B: fresh build dir with ENABLE_CRANELIFT_JIT=ON to prove LibWasm
# compiles+links with the Cranelift WASM JIT enabled (not stubbed).
exec >/boot/home/craneliftB.log 2>&1
export PATH=/boot/system/bin:$PATH
export CC=clang CXX=clang++
export PKG_CONFIG_PATH=/boot/home/lbdeps/lib/pkgconfig:/boot/system/develop/lib/pkgconfig
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
