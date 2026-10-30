#!/bin/bash
# Provision a stock Ubuntu 24.04 arm64 box for the extraction baseline. Every
# download is checksum-verified against the vendor's published SHASUMS.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq unzip zstd python3 file binutils >/dev/null
W=/opt/cc && mkdir -p $W/dl && cd $W/dl
BUNV=1.4.4
curl -fsSLO https://github.com/oven-sh/bun/releases/download/bun-v$BUNV/bun-linux-aarch64.zip
curl -fsSLO https://github.com/oven-sh/bun/releases/download/bun-v$BUNV/SHASUMS256.txt
grep ' bun-linux-aarch64.zip$' SHASUMS256.txt | sha256sum -c -
unzip -oq bun-linux-aarch64.zip && install -m755 bun-linux-aarch64/bun /usr/local/bin/bun
curl -fsSL https://nodejs.org/dist/latest-v22.x/SHASUMS256.txt -o node-SHASUMS256.txt
NT=$(grep -oE 'node-v22\.[0-9.]+-linux-arm64\.tar\.xz' node-SHASUMS256.txt | head -1)
curl -fsSLO https://nodejs.org/dist/latest-v22.x/$NT
grep " $NT\$" node-SHASUMS256.txt | sha256sum -c -
tar -xJf $NT -C /opt && ln -sfn /opt/${NT%.tar.xz} /opt/node22
ln -sf /opt/node22/bin/node /usr/local/bin/node && ln -sf /opt/node22/bin/npm /usr/local/bin/npm
echo "bun $(bun --version)  node $(node --version)  npm $(npm --version)"
