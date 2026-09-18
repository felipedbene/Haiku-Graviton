#!/usr/bin/env bash
#
# build.sh -- cross-build debeos-aws for Haiku arm64 with the korli-go toolchain.
#
# Runs on an amd64 Linux host that (a) can run the amd64-hosted korli-go
# toolchain and (b) has egress to the Go module proxy (the workstation's proxy
# is corp-sinkholed, so this is done on a throwaway builder EC2). The output is
# a single CGO-free AArch64 Haiku ELF that runs on a Graviton Haiku box.
#
#   GOROOT_HAIKU  path to the korli-go toolchain (default /opt/korli-go)
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GOROOT_HAIKU="${GOROOT_HAIKU:-/opt/korli-go}"
export GOROOT="$GOROOT_HAIKU"
export PATH="$GOROOT/bin:$PATH"
export GOOS=haiku GOARCH=arm64 CGO_ENABLED=0

cd "$HERE"
go mod tidy
go build -trimpath -ldflags="-s -w" -o debeos-aws .
file debeos-aws || true
echo "built: $HERE/debeos-aws"
