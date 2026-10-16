#!/bin/bash
# ICU 78.3 from source into /boot/home/lbdeps/icu78 (log-redirected, headless).
#
# NOT optional. LibUnicode's sources use ICU 78 C++ internals that the system
# icu74 does not have, so relaxing Ladybird's "78.3 EXACT" pin only moves the
# failure from configure to compile -- see patches/ladybird-haiku.patch and
# README.md ("ICU").
set -e
export PATH=/boot/system/bin:$PATH
export CC=gcc CXX=g++
cd /boot/home
rm -rf icu icu78build
[ -f icu4c-src.tgz ] || curl -fsSL -o icu4c-src.tgz https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz
python3.10 -c "import tarfile; tarfile.open(\"icu4c-src.tgz\").extractall()"
echo "EXTRACT_RC=$?"
cd /boot/home/icu/source
chmod +x configure runConfigureICU install-sh 2>/dev/null
./configure --prefix=/boot/home/lbdeps/icu78 --enable-static --enable-shared \
  --disable-samples --disable-tests CC=gcc CXX=g++ > /boot/home/icu-cfg.log 2>&1
echo "CONFIGURE_RC=$?"
# This box is shared with another builder agent; -j32 pinned every core.
make -j"${JOBS:-24}" > /boot/home/icu-make.log 2>&1
echo "MAKE_RC=$?"
make install > /boot/home/icu-install.log 2>&1
echo "ICU_INSTALL_RC=$? SENTINEL_DONE"
