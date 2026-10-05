#!/bin/bash
export PATH=/boot/system/bin:$PATH
cd /boot/home
rm -rf skia
git clone https://skia.googlesource.com/skia.git skia && \
cd skia && git checkout chrome/m148 && \
python3.10 tools/git-sync-deps
echo "SKIA_FETCH_RC=$? SENTINEL_DONE"
