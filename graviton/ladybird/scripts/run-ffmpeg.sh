#!/bin/bash
# Build a real ffmpeg n7.1 devel tree into /boot/home/lbdeps. This REPLACES the
# Wave-1 "-ladybird" pkg-config shims (which pointed at a nonexistent system
# ffmpeg) with real shared libs so LibMedia can actually LINK. The DeBeOS repo
# ships no ffmpeg devel, only a minimal ffmpeg_x264 binary.
export PATH=/boot/system/bin:$PATH
export CC=gcc
cd /boot/home
rm -rf ffmpeg-src
git clone --depth 1 -b n7.1 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src > /boot/home/ffmpeg-clone.log 2>&1
echo "CLONE_RC=$?"
cd ffmpeg-src
./configure --prefix=/boot/home/lbdeps --enable-shared --disable-static \
  --disable-programs --disable-doc --disable-static --cc=gcc \
  --disable-encoders --disable-muxers --disable-devices \
  > /boot/home/ffmpeg-cfg.log 2>&1
echo "CFG_RC=$?"
make -j32 > /boot/home/ffmpeg-make.log 2>&1
echo "MAKE_RC=$?"
make install > /boot/home/ffmpeg-install.log 2>&1
echo "INSTALL_RC=$? SENTINEL_DONE"
