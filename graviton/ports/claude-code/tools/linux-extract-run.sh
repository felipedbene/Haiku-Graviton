#!/bin/bash
set -uo pipefail
V=${1:-2.1.314}; W=/opt/cc/v$V; rm -rf $W; mkdir -p $W && cd $W
npm view @anthropic-ai/claude-code-linux-arm64@$V dist.integrity dist.shasum
npm pack --silent @anthropic-ai/claude-code-linux-arm64@$V >/dev/null && tar xzf *.tgz
sha256sum package/claude
python3 -I /opt/cc/tools/extract-bun-bundle.py package/claude $W/bunfs || exit 1
ln -sfn $W/bunfs '/$bunfs'
echo "=== reference: shipped binary ==="; ./package/claude --version
echo "=== 3a: stock bun, extracted bundle ==="
cd /tmp && bun --zero-fill-buffers '/$bunfs/root/cli' --version; echo "rc=$?"
bun --zero-fill-buffers '/$bunfs/root/cli' --help 2>&1 | head -8; echo "rc=${PIPESTATUS[0]}"
