#!/bin/bash
cd /tmp
echo "=== 3b: node $(node --version), extracted 2.1.314 bundle ==="
node '/$bunfs/root/cli' --version > /tmp/n1.out 2>&1; echo "rc=$?"; head -c 1500 /tmp/n1.out; echo
echo "--- bun:* importers ---"; grep -l -E 'from"bun:|import\("bun:' /\$bunfs/root/* 2>/dev/null | head
echo "=== 5: 2.1.112 (last cli.js) under node ==="
mkdir -p /opt/cc/v2.1.112 && cd /opt/cc/v2.1.112 && rm -rf package *.tgz
npm view @anthropic-ai/claude-code@2.1.112 dist.integrity bin engines --json 2>/dev/null | tr -d '\n'; echo
npm pack --silent @anthropic-ai/claude-code@2.1.112 >/dev/null && tar xzf *.tgz && ls package | head -20
cd /tmp && node /opt/cc/v2.1.112/package/cli.js --version; echo "rc=$?"
node /opt/cc/v2.1.112/package/cli.js --help 2>&1 | head -4
