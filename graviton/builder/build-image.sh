#!/bin/bash
# Wait for the cross-toolchain job, then build the arm64 nightly MMC image.
set -uxo pipefail
while pgrep -f build-crosstools.sh > /dev/null; do sleep 15; done
if ! grep -q CROSSTOOLS_OK /opt/haiku/crosstools.log; then
  echo "CROSSTOOLS_FAILED"; exit 1
fi
echo "=== crosstools ok, starting image build ==="
cd /opt/haiku/haiku/generated.arm64
# shallow clone has no tags, so pin the revision explicitly
HAIKU_REVISION=hrev59996 jam -q -j64 @nightly-mmc
rc=$?
echo "JAM_EXIT=$rc"
ls -lh haiku-nightly.mmc 2>/dev/null || true
echo "IMAGE_BUILD_FINISHED"
