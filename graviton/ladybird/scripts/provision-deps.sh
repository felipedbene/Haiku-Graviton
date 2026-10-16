#!/bin/sh
# DeBeOS Ladybird dependency provisioning (run natively on a Graviton
# Haiku builder). Produces a /boot/home/lbdeps prefix that satisfies every
# Ladybird CMake dependency EXCEPT Skia (built separately, see README).
#
# Toolchain assumed already installed via:
#   pkgman install -y llvm21_clang llvm21_lld llvm21_libs cmake ninja gn \
#                     rust_bin pkgconf git
set -e
export PATH=/boot/system/bin:$PATH
export CC=clang CXX=clang++
PREFIX=/boot/home/lbdeps
mkdir -p $PREFIX/lib/pkgconfig $PREFIX/include/wuffs

# 1. System devel deps. NOTE: icu74_devel here is for harfbuzz_devel and the
#    rest of the C++ stack, which are built against icu74; forcing icu77_devel
#    conflicts with harfbuzz_devel's devel:libicuuc dep. Ladybird itself needs
#    ICU 78.3, which run-icu2.sh builds from source into lbdeps/icu78 -- that
#    step is mandatory, not optional. LibUnicode uses ICU 78 C++ internals
#    (UnicodeSet::strings(), the Calendar::internalGetMonth signature), so
#    pointing it at icu74 does not compile however the version pin is set.
#    simdjson_devel and libtommath_devel are deliberately NOT in this list: the
#    DeBeOS arm64 repo publishes each _devel subpackage at an older version than
#    its own runtime package (simdjson 5.0.2-1 vs simdjson_devel 3.11.5-1;
#    libtommath 1.3.0-1 vs libtommath_devel 1.2.0-3), so each _devel's
#    `<name>==<old version>` requirement cannot be satisfied and pkgman fails
#    the whole transaction with "nothing provides". Both are built from source
#    into $PREFIX below instead, at the versions pkgconfig/simdjson.pc and
#    pkgconfig/libtommath.pc already declare.
#    libavif_devel is deliberately not here either: the repo's is 0.9.3 and
#    AVIFLoader.cpp needs avifDecoder::repetitionCount, so 1.4.2 is built from
#    source below. dav1d_devel stays -- that build links the system dav1d.
pkgman install -y \
  libfmt_devel mimalloc_devel libsdl3_devel \
  fontconfig_devel libedit_devel harfbuzz_devel freetype_devel \
  libpng16_devel libjpeg_turbo_devel libwebp_devel brotli_devel woff2_devel \
  openssl3_devel curl_devel libxml2_devel sqlite_devel zlib_devel libpsl_devel \
  dav1d_devel libdwarf_devel icu74_devel

# 2. FastFloat (header-only) -- build+install its real CMake config package.
cd /boot/home && git config --global core.fsync none || true
rm -rf fast_float && git clone --depth 1 --template= https://github.com/fastfloat/fast_float.git
cd fast_float && mkdir build && cd build
cmake -G Ninja -DCMAKE_INSTALL_PREFIX=$PREFIX \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DFASTFLOAT_TEST=OFF -DFASTFLOAT_INSTALL=ON ..
ninja install

# 3. simdutf -- build the static lib + its CMake config package.
cd /boot/home && rm -rf simdutf
git clone --depth 1 --template= https://github.com/simdutf/simdutf.git
cd simdutf && mkdir build && cd build
cmake -G Ninja -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DBUILD_SHARED_LIBS=OFF -DSIMDUTF_TESTS=OFF -DSIMDUTF_TOOLS=OFF \
  -DSIMDUTF_BENCHMARKS=OFF -DSIMDUTF_CXX_STANDARD=20 ..
ninja && ninja install

# 3b. simdjson 5.0.2 -- matches the repo's runtime package, and the version
#     pkgconfig/simdjson.pc declares. THREADS_ENABLED to match that .pc's
#     -DSIMDJSON_THREADS_ENABLED=1, or the header and the lib disagree about
#     simdjson::dom::parser's layout.
cd /boot/home && rm -rf simdjson-src
git clone --depth 1 --template= -b v5.0.2 https://github.com/simdjson/simdjson.git simdjson-src
cd simdjson-src && mkdir build && cd build
cmake -G Ninja -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DBUILD_SHARED_LIBS=OFF -DSIMDJSON_THREADS_ENABLED=ON \
  -DSIMDJSON_DEVELOPER_MODE=OFF ..
ninja && ninja install

# 3c. libtommath 1.3.0 -- LibCrypto needs mp_expt_n, added after 1.2.0, so the
#     repo's 1.2.0 devel headers would not compile even if they installed. Its
#     own script because the IEEE-754 macro it has to assert needs explaining
#     and because a failed WebContent start is the only symptom of getting it
#     wrong, which makes it worth re-running on its own.
PREFIX=$PREFIX sh "$(dirname "$0")/build-tommath.sh"

# 3d. libavif 1.4.2 -- the repo's 0.9.3 predates avifDecoder::repetitionCount,
#     which AVIFLoader.cpp reads, and find_package(LIBAVIF) has no version
#     constraint so configure cannot catch it. Own script; see build-avif.sh.
PREFIX=$PREFIX sh "$(dirname "$0")/build-avif.sh"

# 4. wuffs -- single-file header the LibImageDecoders find_path() needs.
curl -fsSL -o $PREFIX/include/wuffs/wuffs-v0.3.c \
  https://raw.githubusercontent.com/google/wuffs/v0.3.3/release/c/wuffs-v0.3.c

# 5. ffmpeg "-ladybird" pkg-config shims. The DeBeOS repo ships NO ffmpeg devel
#    (only a minimal ffmpeg_x264 binary), so these satisfy Ladybird's
#    pkg_check_modules(... libav*-ladybird) at CONFIGURE time only. These get
#    REPLACED by a REAL ffmpeg n7.1 devel build (run-ffmpeg.sh) before LibMedia
#    can LINK; the real .pc files point at $PREFIX, see pkgconfig/.
for m in avcodec:61.19.101 avformat:61.7.100 avutil:59.39.100 swresample:5.3.100; do
  name=${m%%:*}; ver=${m##*:}
  cat > $PREFIX/lib/pkgconfig/lib${name}-ladybird.pc <<EOF
prefix=/boot/system
exec_prefix=\${prefix}
libdir=\${exec_prefix}/develop/lib
includedir=\${prefix}/develop/headers

Name: lib${name}-ladybird
Description: DeBeOS configure shim for lib${name} (NO real libs yet)
Version: ${ver}
Libs: -L\${libdir} -l${name}
Cflags: -I\${includedir}
EOF
done
echo "provisioning done: $PREFIX"
