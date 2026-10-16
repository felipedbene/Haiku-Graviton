#!/bin/sh
# Build libavif 1.4.2 into /boot/home/lbdeps for Ladybird's LibImageDecoders.
#
# The DeBeOS arm64 repo ships libavif 0.9.3 (`AVIF_VERSION_MAJOR 0` /
# `_MINOR 9` / `_PATCH 3` in /boot/system/develop/headers/avif/avif.h), and
# Ladybird's AVIFLoader reads `avifDecoder::repetitionCount`, which 0.9.x does
# not have. Ladybird's own pin is 1.4.2 (vcpkg.json), but the CMake check is
# `find_package(LIBAVIF REQUIRED)` with no version
# (Meta/CMake/check_for_dependencies.cmake:21), so the 0.9.3 config satisfies
# configure and the build dies much later in the ImageDecoder service:
#
#   AVIFLoader.cpp:92:49: error: no member named 'repetitionCount' in
#       'avifDecoder'
#
# Decode-only is enough for a browser, so this builds against the system dav1d
# (1.5.4) and leaves the aom encoder, libyuv and libsharpyuv out -- none of the
# three is packaged for arm64 here, and libavif's bundled-dependency mode would
# pull in a second copy of each.
#
# Installing into $PREFIX also retires the libavif-config shadow this recipe
# used to need: #709's broken config (an _IMPORT_PREFIX walk that lands on
# /boot/system and advertises the non-existent /boot/system/include) is the
# *system* package's. libavif's installer derives the same prefix from
# $PREFIX/lib/cmake/libavif, so the config it writes here advertises
# $PREFIX/include, which exists.
set -e
export PATH=/boot/system/bin:$PATH
PREFIX="${PREFIX:-/boot/home/lbdeps}"
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:/boot/system/develop/lib/pkgconfig

cd /boot/home && rm -rf libavif-src
git clone --depth 1 --template= -b v1.4.2 \
	https://github.com/AOMediaCodec/libavif.git libavif-src
cd libavif-src && mkdir build && cd build
# -z max-page-size=4096 for the same reason as every other image in this
# recipe: runtime_loader's inter-segment hole budget (DeBeOS #720).
cmake -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DCMAKE_LINKER_TYPE=LLD \
  -DCMAKE_C_USING_LINKER_LLD=-fuse-ld=lld \
  -DCMAKE_CXX_USING_LINKER_LLD=-fuse-ld=lld \
  -DCMAKE_C_USING_LINKER_MODE=FLAG \
  -DCMAKE_CXX_USING_LINKER_MODE=FLAG \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=4096" \
  -DBUILD_SHARED_LIBS=ON \
  -DAVIF_CODEC_DAV1D=SYSTEM \
  -DAVIF_LIBYUV=OFF -DAVIF_LIBSHARPYUV=OFF -DAVIF_JPEG=OFF -DAVIF_ZLIBPNG=OFF \
  -DAVIF_BUILD_APPS=OFF -DAVIF_BUILD_EXAMPLES=OFF -DAVIF_BUILD_TESTS=OFF ..
ninja && ninja install

# Check the header that LibImageDecoders will actually see, not the version
# macro alone: the macro would also be satisfied by a 1.x header installed
# without the decoder struct Ladybird reads.
grep -q 'define AVIF_VERSION_MAJOR 1' "$PREFIX/include/avif/avif.h" \
  || { echo "installed avif.h is not 1.x" >&2; exit 1; }
grep -q repetitionCount "$PREFIX/include/avif/avif.h" \
  || { echo "installed avif.h has no repetitionCount" >&2; exit 1; }
test -e "$PREFIX/lib/cmake/libavif/libavif-config.cmake" \
  || { echo "libavif CMake config not installed into PREFIX" >&2; exit 1; }
echo "AVIF BUILD+INSTALL DONE"
