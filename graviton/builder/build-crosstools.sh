#!/bin/bash
set -euxo pipefail
cd /opt/haiku/buildtools/jam
make
sudo ./jam0 install
cd /opt/haiku/haiku
rm -rf generated.arm64
mkdir -p generated.arm64
cd generated.arm64
../configure -j64 --cross-tools-source ../../buildtools --build-cross-tools arm64
echo "CROSSTOOLS_OK"
