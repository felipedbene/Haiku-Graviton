#!/bin/sh
# DeBeOS Ladybird Wave-2 CMake configure (run in /boot/home/lb after applying
# ladybird-haiku-arm64.patch and provisioning Skia + APNG libpng + libtommath).
#
# Result: a FULL, clean configure — no CMake errors. Verified markers:
#   -- Found skia, version 148
#   -- Performing Test LIBPNG_HAS_APNG - Success
#   -- Build files have been written to: .../build-wave1
#
# LIBRARY_PATH must keep /boot/system/lib so cargo-run host build-script
# binaries can map libroot/libgcc_s (Haiku LIBRARY_PATH replaces, not prepends).
set -e
export PATH=/boot/home/bin:/boot/system/bin:$PATH
export CC=clang CXX=clang++
export PKG_CONFIG_PATH=/boot/home/lbdeps/lib/pkgconfig:/boot/system/develop/lib/pkgconfig
export LIBRARY_PATH=/boot/system/lib:/boot/system/develop/lib
cd /boot/home/lb
rm -rf build-wave1
cmake -G Ninja -B build-wave1 -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_LTO_FOR_RELEASE=OFF \
  -DDEBEOS_HEADLESS_ONLY=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=/boot/home/lbdeps \
  -DFastFloat_DIR=/boot/home/lbdeps/share/cmake/FastFloat \
  -Dsimdutf_DIR=/boot/home/lbdeps/lib/cmake/simdutf \
  -DWUFFS_INCLUDE_DIR=/boot/home/lbdeps/include \
  -DPython3_EXECUTABLE=/boot/system/bin/python3.10 \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DCMAKE_LINKER_TYPE=LLD \
  -DCMAKE_C_USING_LINKER_LLD=-fuse-ld=lld -DCMAKE_CXX_USING_LINKER_LLD=-fuse-ld=lld \
  -DCMAKE_C_USING_LINKER_MODE=FLAG -DCMAKE_CXX_USING_LINKER_MODE=FLAG \
  -DCMAKE_EXE_LINKER_FLAGS="-lbsd -lnetwork" \
  -DCMAKE_SHARED_LINKER_FLAGS="-lbsd -lnetwork" \
  -DCMAKE_MODULE_LINKER_FLAGS="-lbsd -lnetwork"

# Then: ninja -j"$(nproc)" -C build-wave1 test-web
#   (export the same PATH/PKG_CONFIG_PATH/LIBRARY_PATH for the build too).
