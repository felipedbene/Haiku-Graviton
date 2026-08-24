#!/bin/bash
set -uxo pipefail
cd /opt/haiku/haiku/generated.arm64
HAIKU_REVISION=hrev59996 jam -q -j64 @minimum-mmc
echo "JAM_EXIT=$?"
ls -lh haiku-minimum.mmc 2>/dev/null
echo "MIN_BUILD_FINISHED"
