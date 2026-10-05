#!/bin/sh
# DeBeOS Ladybird Wave-2 provisioning additions (run AFTER the Wave-1
# provision-deps recipe, natively on a Graviton arm64 Haiku builder).
#
# Wave-1 produced /boot/home/lbdeps with FastFloat, simdutf, wuffs and the
# libav*-ladybird pkg-config shims, and installed the toolchain + devel libs.
# Wave-2 adds everything the actual Skia build + full configure + build need.
set -e
export PATH=/boot/home/bin:/boot/system/bin:$PATH
PREFIX=/boot/home/lbdeps

# 1. Host tools the minimal image lacks (needed to unpack/patch sources).
pkgman install -y gzip tar xz_utils patch diffutils expat_devel

# 2. A `python3` (and `python`) on PATH — Skia's GN needs it; the image only
#    ships python3.10.
mkdir -p /boot/home/bin
[ -e /boot/home/bin/python3 ] || ln -s /boot/system/bin/python3.10 /boot/home/bin/python3
[ -e /boot/home/bin/python ]  || ln -s /boot/system/bin/python3.10 /boot/home/bin/python

# 3. libtommath 1.3.0 into lbdeps. The DeBeOS libtommath_devel is 1.2.0 and
#    LACKS mp_expt_n (added in 1.3.0) which Ladybird's LibCrypto requires; its
#    .pc also has a wrong prefix=/usr/local. Build 1.3.0 and override the .pc.
cd /boot/home
[ -d libtommath-1.3.0 ] || { curl -fsSL -o ltm.tar.xz https://github.com/libtom/libtommath/releases/download/v1.3.0/ltm-1.3.0.tar.xz && mkdir -p libtommath-1.3.0 && tar xJf ltm.tar.xz -C libtommath-1.3.0 --strip-components=1; }
cd libtommath-1.3.0 && rm -rf b && mkdir b && cd b
cmake -G Ninja -DCMAKE_INSTALL_PREFIX=$PREFIX -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DBUILD_SHARED_LIBS=ON .. && ninja install
cmake -DBUILD_SHARED_LIBS=OFF .. && ninja install
cat > $PREFIX/lib/pkgconfig/libtommath.pc <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: LibTomMath
Description: public domain library for manipulating large integer numbers
Version: 1.3.0
Libs: -L\${libdir} -ltommath
Cflags: -I\${includedir}
EOF

# 4. libpng-with-APNG and Skia m148 — see build-libpng-apng.sh and build-skia.sh.
echo "provision-wave2 done; now run build-libpng-apng.sh and build-skia.sh"
