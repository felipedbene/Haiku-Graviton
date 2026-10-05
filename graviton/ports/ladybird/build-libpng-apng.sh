#!/bin/sh
# DeBeOS Ladybird Wave-2: build libpng 1.6.53 WITH the APNG patch into
# /boot/home/lbdeps, so Ladybird's LibImageDecoders LIBPNG_HAS_APNG check
# (requires PNG_APNG_SUPPORTED / PNG_READ_APNG_SUPPORTED in png.h) passes.
#
# The DeBeOS system libpng16 is built WITHOUT APNG; per project policy we do
# NOT feature-cap LibImageDecoders — we rebuild libpng with APNG and point
# Ladybird's find_package(PNG) at it via CMAKE_PREFIX_PATH=/boot/home/lbdeps.
#
# Key detail: the libpng-apng patch for 1.6.53 modifies png.h + the C sources
# but NOT scripts/pnglibconf.dfa or the prebuilt config, so PNG_APNG_SUPPORTED
# is never defined by the normal CMake build and the APNG code gets #ifdef'd
# out. We inject the three APNG feature macros into scripts/pnglibconf.h.prebuilt
# and force CMake to USE that header verbatim via -DPNG_LIBCONF_HEADER=..., which
# both exposes the API in png.h AND compiles the APNG code into the library.
# Verify with: nm libpng16.a | grep png_get_acTL
set -e
export PATH=/boot/system/bin:$PATH
export CC=clang
PREFIX=/boot/home/lbdeps
WORK=/boot/home/pngbuild
VER=1.6.53   # match the DeBeOS system libpng16 version (pkg-config --modversion libpng16)

# tar/gzip/patch are not in the minimal image; pkgman install -y gzip tar patch
mkdir -p "$WORK"; cd "$WORK"
[ -s libpng.tar.gz ] || curl -fsSL -o libpng.tar.gz "https://download.sourceforge.net/libpng/libpng-${VER}.tar.gz"
[ -s apng.patch.gz ] || curl -fsSL -o apng.patch.gz "https://downloads.sourceforge.net/project/libpng-apng/libpng16/${VER}/libpng-${VER}-apng.patch.gz"
rm -rf "libpng-${VER}"; tar xzf libpng.tar.gz
cd "libpng-${VER}"
gunzip -c ../apng.patch.gz > ../apng.patch
patch -p1 < ../apng.patch

PRE=scripts/pnglibconf.h.prebuilt
if ! grep -q PNG_APNG_SUPPORTED "$PRE"; then
  awk '/#define PNGLCONF_H/ && !d {print; print "#define PNG_APNG_SUPPORTED"; print "#define PNG_READ_APNG_SUPPORTED"; print "#define PNG_WRITE_APNG_SUPPORTED"; d=1; next} {print}' "$PRE" > "$PRE.n" && mv "$PRE.n" "$PRE"
fi

rm -rf build && mkdir build && cd build
cmake -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DPNG_LIBCONF_HEADER="$WORK/libpng-${VER}/scripts/pnglibconf.h.prebuilt" \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DPNG_FRAMEWORK=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
  -DSKIP_INSTALL_EXECUTABLES=ON -DSKIP_INSTALL_PROGRAMS=ON \
  -DPNG_SHARED=ON -DPNG_STATIC=ON \
  -DZLIB_INCLUDE_DIR=/boot/system/develop/headers ..
ninja && ninja install
# Proof: both the macro and the compiled symbol must be present.
grep -q PNG_APNG_SUPPORTED "$PREFIX/include/libpng16/pnglibconf.h" && echo "APNG macro OK"
nm "$PREFIX/lib/libpng16.a" | grep -q png_get_acTL && echo "APNG symbols OK"
