#!/bin/bash
# Build ICU 78.3 from source into /boot/home/lbdeps/icu78.
# Only needed if the upstream Ladybird "find_package(ICU 78.3 EXACT)" pin is
# kept. The shipped patches/ladybird-haiku.patch instead relaxes that pin to the
# coherent system icu74, so this is optional. run-icu2.sh is the log-redirected
# (non-interactive) variant and is preferred on a headless builder.
export PATH=/boot/system/bin:$PATH
export CC=gcc CXX=g++
cd /boot/home
rm -rf icu icu78build
curl -fsSL -o icu4c-src.tgz https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz || { echo "DOWNLOAD_FAIL"; exit 1; }
tar xzf icu4c-src.tgz   # extracts to icu/
cd /boot/home/icu/source
chmod +x configure runConfigureICU install-sh 2>/dev/null
./configure --prefix=/boot/home/lbdeps/icu78 --enable-static --enable-shared \
  --disable-samples --disable-tests CC=gcc CXX=g++ 2>&1 | tail -15
echo "CONFIGURE_RC=$?"
make -j32 2>&1 | tail -8
echo "MAKE_RC=$?"
make install 2>&1 | tail -5
echo "ICU_INSTALL_RC=$? SENTINEL_DONE"
