#!/bin/sh
# Canonical DeBeOS Ladybird CMake configure (run in /boot/home/lb AFTER applying
# patches/ladybird-haiku.patch, running provision-deps.sh, and building+exposing
# Skia, the APNG libpng, ICU and ffmpeg into /boot/home/lbdeps). NO vcpkg: a
# plain `-G Ninja` falls through to find_package/pkg_check_modules against the
# system + lbdeps prefix. (This is the build-wave2 configure.)
set -e
export PATH=/boot/system/bin:$PATH
export CC=clang CXX=clang++
export PKG_CONFIG_PATH=/boot/home/lbdeps/lib/pkgconfig:/boot/system/develop/lib/pkgconfig
cd /boot/home/lb
rm -rf build-wave2
cmake -G Ninja -B build-wave2 -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_LTO_FOR_RELEASE=OFF \
  -DENABLE_QT_UI=OFF \
  -DBUILD_TESTING=OFF \
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
