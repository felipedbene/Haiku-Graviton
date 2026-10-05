#!/bin/bash
export PATH=/boot/system/bin:$PATH
cd /boot/home
rm -rf lb
git clone https://github.com/LadybirdBrowser/ladybird.git lb && \
cd lb && git checkout 90998c5d
echo "LB_CLONE_RC=$? SENTINEL_DONE"
