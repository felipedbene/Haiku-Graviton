#!/bin/bash
exec >> /opt/haiku/opt-image.log 2>&1
set -x
export HAIKU_REVISION=hrev59996
G=/opt/haiku/haiku-opt/generated.arm64
cd $G || exit 1
grep -n 'case arm64 : archFlags' ../build/jam/ArchitectureRules
echo "=== IMAGE BUILD START $(date) ==="
jam -q -j16 @minimum-mmc
echo "IMAGE_EXIT=$?"
ls -lh $G/*.image $G/*.mmc 2>/dev/null
echo "=== IMAGE BUILD DONE $(date) ==="
touch /opt/haiku/opt-image.done
