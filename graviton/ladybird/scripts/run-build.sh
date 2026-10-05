#!/bin/bash
# Build the headless render path: WebContent + WebDriver (screenshot is built
# into WebDriver; there is no standalone headless-browser exe). -j32 suits a
# c7g.8xlarge. Run detached (nohup) so an SSH/agent death does not kill it.
export PATH=/boot/home/config/non-packaged/bin:/boot/system/bin:$PATH
cd /boot/home/lb/build-wave2
ninja WebContent WebDriver -j32
echo "BUILD_RC=$? SENTINEL_DONE"
