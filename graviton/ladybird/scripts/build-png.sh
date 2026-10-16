#!/bin/sh
# Rebuild libpng16 1.6.53 WITH the APNG patch into /boot/home/lbdeps. DeBeOS's
# repo libpng16 is built WITHOUT APNG, but Ladybird's LibImageDecoders hard-
# requires PNG_APNG_SUPPORTED. Apply patches/libpng-1.6.53-apng.patch to a
# pristine pnggroup/libpng 1.6.53 checkout BEFORE running this:
#   git clone https://github.com/pnggroup/libpng.git pngbuild/libpng-1.6.53
#   cd pngbuild/libpng-1.6.53 && git checkout v1.6.53
#   patch -p1 < <repo>/graviton/ladybird/patches/libpng-1.6.53-apng.patch
#
# The max-page-size link flags are not optional on arm64: lld defaults to a
# 64 KB max-page-size and emits four PT_LOADs, so the image carries three
# ~60 KB inter-segment holes, and the runtime_loader rejects any image whose
# reserved span exceeds its mapped segments by more than 0x20000
# (src/system/runtime_loader/images.cpp:339-342, DeBeOS #720):
#   runtime_loader: /boot/home/lbdeps/lib/libpng16.so.16.53.0:
#       Could not map image: Bad data
# That is a load-time failure of every consumer, so WebContent and WebDriver
# would not start even once they linked. Laying the segments out at the 4 KB
# page the system really uses shrinks each hole below a page.
#
# #720 is fixed in DeBeOS trunk (067dce7ff5), but the loader ships inside the
# booted image, so these flags only become redundant once the builder AMI runs
# a DeBeOS build that contains the fix. The retirement condition, and the
# one-line check for it, are recorded once in
# patches/ladybird-haiku.patch (Meta/CMake/compile_options.cmake hunk);
# retire that site, build-avif.sh and this one together.
set -e
export PATH=/boot/system/bin:$PATH
cd /boot/home/pngbuild/libpng-1.6.53
rm -rf bld
cmake -G Ninja -B bld -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/boot/home/lbdeps \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=4096" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=4096" \
  -DPNG_SHARED=ON -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
  -DZLIB_INCLUDE_DIR=/boot/system/develop/headers \
  -DZLIB_LIBRARY=/boot/system/develop/lib/libz.so
ninja -C bld -j32
ninja -C bld install
echo "PNG BUILD+INSTALL DONE"
