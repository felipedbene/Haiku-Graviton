#!/bin/sh
# Rebuild libpng16 1.6.53 WITH the APNG patch into /boot/home/lbdeps. DeBeOS's
# repo libpng16 is built WITHOUT APNG, but Ladybird's LibImageDecoders hard-
# requires PNG_APNG_SUPPORTED. Apply patches/libpng-1.6.53-apng.patch to a
# pristine pnggroup/libpng 1.6.53 checkout BEFORE running this:
#   git clone https://github.com/pnggroup/libpng.git pngbuild/libpng-1.6.53
#   cd pngbuild/libpng-1.6.53 && git checkout v1.6.53
#   patch -p1 < <repo>/graviton/ladybird/patches/libpng-1.6.53-apng.patch
set -e
export PATH=/boot/system/bin:$PATH
cd /boot/home/pngbuild/libpng-1.6.53
rm -rf bld
cmake -G Ninja -B bld -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/boot/home/lbdeps \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DPNG_SHARED=ON -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
  -DZLIB_INCLUDE_DIR=/boot/system/develop/headers \
  -DZLIB_LIBRARY=/boot/system/develop/lib/libz.so
ninja -C bld -j32
ninja -C bld install
echo "PNG BUILD+INSTALL DONE"
