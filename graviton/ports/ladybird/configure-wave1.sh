#!/bin/sh
# Canonical DeBeOS Ladybird Wave-1 CMake configure (run in /boot/home/lb after
# applying ladybird-cmake-wave1.patch and running provision-deps.sh).
# Expected result: configure stops with EXACTLY ONE CMake Error -- Skia
# (pkg_check_modules skia=148) -- the intended Wave-2 wall.
set -e
export PATH=/boot/system/bin:$PATH
export CC=clang CXX=clang++
export PKG_CONFIG_PATH=/boot/home/lbdeps/lib/pkgconfig:/boot/system/develop/lib/pkgconfig
cd /boot/home/lb
rm -rf build-wave1
cmake -G Ninja -B build-wave1 -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_LTO_FOR_RELEASE=OFF \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=/boot/home/lbdeps \
  -DFastFloat_DIR=/boot/home/lbdeps/share/cmake/FastFloat \
  -Dsimdutf_DIR=/boot/home/lbdeps/lib/cmake/simdutf \
  -DWUFFS_INCLUDE_DIR=/boot/home/lbdeps/include \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DCMAKE_C_COMPILER_AR=/boot/system/bin/ar -DCMAKE_CXX_COMPILER_AR=/boot/system/bin/ar \
  -DCMAKE_C_COMPILER_RANLIB=/boot/system/bin/ranlib -DCMAKE_CXX_COMPILER_RANLIB=/boot/system/bin/ranlib \
  -DCMAKE_LINKER_TYPE=LLD \
  -DCMAKE_C_USING_LINKER_LLD=-fuse-ld=lld -DCMAKE_CXX_USING_LINKER_LLD=-fuse-ld=lld \
  -DCMAKE_C_USING_LINKER_MODE=FLAG -DCMAKE_CXX_USING_LINKER_MODE=FLAG
