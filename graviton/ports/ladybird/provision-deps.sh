#!/bin/sh
# DeBeOS Ladybird Wave-1 dependency provisioning (run natively on a Graviton
# Haiku builder). Produces a /boot/home/lbdeps prefix that satisfies every
# Ladybird CMake dependency EXCEPT Skia (the Wave-2 wall).
#
# Toolchain assumed already installed via:
#   pkgman install -y llvm21_clang llvm21_lld llvm21_libs cmake ninja gn \
#                     rust_bin pkgconf git
set -e
export PATH=/boot/system/bin:$PATH
export CC=clang CXX=clang++
PREFIX=/boot/home/lbdeps
mkdir -p $PREFIX/lib/pkgconfig $PREFIX/include/wuffs

# 1. System devel deps. NOTE: ICU is icu74 (NOT icu77/78) because that is the
#    version harfbuzz_devel and the rest of the C++ stack are built against;
#    forcing icu77_devel conflicts with harfbuzz_devel's devel:libicuuc dep.
pkgman install -y \
  libfmt_devel mimalloc_devel simdjson_devel libavif_devel libsdl3_devel \
  fontconfig_devel libedit_devel libtommath_devel harfbuzz_devel freetype_devel \
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

# 4. wuffs -- single-file header the LibImageDecoders find_path() needs.
curl -fsSL -o $PREFIX/include/wuffs/wuffs-v0.3.c \
  https://raw.githubusercontent.com/google/wuffs/v0.3.3/release/c/wuffs-v0.3.c

# 5. ffmpeg "-ladybird" pkg-config shims. The DeBeOS repo ships NO ffmpeg devel
#    (only a minimal ffmpeg_x264 binary), so these satisfy Ladybird's
#    pkg_check_modules(... libav*-ladybird) at CONFIGURE time only. Wave 2 must
#    replace them with a REAL ffmpeg devel build before LibMedia can LINK.
for m in avcodec:61.19.101 avformat:61.7.100 avutil:59.39.100 swresample:5.3.100; do
  name=${m%%:*}; ver=${m##*:}
  cat > $PREFIX/lib/pkgconfig/lib${name}-ladybird.pc <<EOF
prefix=/boot/system
exec_prefix=\${prefix}
libdir=\${exec_prefix}/develop/lib
includedir=\${prefix}/develop/headers

Name: lib${name}-ladybird
Description: DeBeOS Wave-1 configure shim for lib${name} (NO real libs yet)
Version: ${ver}
Libs: -L\${libdir} -l${name}
Cflags: -I\${includedir}
EOF
done
echo "provisioning done: $PREFIX"
