#!/bin/sh
set +e
L=/opt/haiku/logs/bootstrap-mmc-build.log
echo "=== last non-compile status lines ==="
grep -aE "^\.\.\.| failed | Building | updated | skipped |JAM_EXIT|BUILD FAILURE|Grepping|Creating|Welcome|checking|configure:|^Making|^make" "$L" 2>/dev/null | tail -15
echo "=== errors ==="
grep -aiE "BUILD FAILURE|does not exist|Traceback|Error:|No such file|cannot |JAM_EXIT" "$L" 2>/dev/null | tail -8
echo "=== jam alive? ==="
pgrep -af "[j]am" | head -1
echo "cc1plus procs: $(pgrep -c cc1plus 2>/dev/null)"
echo "=== mmc image? ==="
ls -la /opt/haiku/haiku/generated.arm64/haiku-bootstrap.mmc 2>/dev/null
echo "=== cross pkgs built so far ==="
ls /opt/haiku/haiku/generated.arm64/objects/haiku/arm64/packaging/repositories/HaikuPortsCross-build/packages/ 2>/dev/null
