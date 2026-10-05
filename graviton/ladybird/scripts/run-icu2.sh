#!/bin/bash
# ICU 78.3 from source into /boot/home/lbdeps/icu78 (log-redirected, headless).
# Optional: only needed if the "78.3 EXACT" ICU pin is kept rather than relaxed
# to the system icu74 (see patches/ladybird-haiku.patch).
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
make -j32 > /boot/home/icu-make.log 2>&1
echo "MAKE_RC=$?"
make install > /boot/home/icu-install.log 2>&1
echo "ICU_INSTALL_RC=$? SENTINEL_DONE"
