#!/bin/sh
# Build libtommath 1.3.0 into /boot/home/lbdeps for Ladybird's LibCrypto.
# 1.3.0 (not the repo's 1.2.0) because LibCrypto calls mp_expt_n, which was
# added after 1.2.0.
#
# -D__STDC_IEC_559__ is not cosmetic. libtommath compiles mp_set_double only
# when the compiler advertises IEEE 754 doubles:
#
#   bn_mp_set_double.c:6: #if defined(__STDC_IEC_559__) || defined(__GCC_IEC_559)
#
# Haiku's libc headers define neither macro, and clang -- unlike gcc, which
# defines __GCC_IEC_559 -- does not define one either, so with CC=clang the
# function disappears and the archive member carries no symbols at all:
#
#   $ clang -c bn_mp_set_double.c && nm bn_mp_set_double.o
#   bn_mp_set_double.c:44:6: warning: "mp_set_double implementation is only
#       available on platforms with IEEE754 floating point format"
#   (no output)
#
# There is no link error, because Ladybird's Haiku build needs
# --allow-shlib-undefined/-z undefs (split libc), so liblagom-crypto links with
# mp_set_double still undefined and every process that loads it dies at start:
#
#   runtime_loader: .../lib/liblagom-crypto.so.0.1.0:
#       Troubles relocating: Symbol not found
#
# aarch64 doubles *are* IEEE 754 binary64 and clang is not in a fast-math mode
# here, so asserting the macro states a fact about the target rather than
# forcing anything; the nm check below is what keeps that honest.
set -e
export PATH=/boot/system/bin:$PATH
PREFIX="${PREFIX:-/boot/home/lbdeps}"

cd /boot/home && rm -rf libtommath-src
git clone --depth 1 --template= -b v1.3.0 \
	https://github.com/libtom/libtommath.git libtommath-src
cd libtommath-src && mkdir build && cd build
cmake -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_C_FLAGS=-D__STDC_IEC_559__ \
  -DCMAKE_AR=/boot/system/bin/ar -DCMAKE_RANLIB=/boot/system/bin/ranlib \
  -DBUILD_SHARED_LIBS=OFF ..
ninja && ninja install

grep -q mp_expt_n "$PREFIX/include/tommath.h" \
  || { echo "libtommath headers lack mp_expt_n" >&2; exit 1; }
# Plain nm, not `nm -D`: a static archive has no dynamic symbol table, so
# `nm -D` reports "no symbols" for every member and the check would pass
# vacuously -- in both directions.
nm "$PREFIX/lib/libtommath.a" 2>/dev/null | grep -q ' T mp_set_double' \
  || { echo "libtommath.a does not define mp_set_double" >&2; exit 1; }
echo "TOMMATH BUILD+INSTALL DONE"
