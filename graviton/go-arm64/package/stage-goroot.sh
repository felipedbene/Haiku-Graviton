#!/bin/bash
# stage-goroot.sh -- assemble the native haiku/arm64 Go distribution (a
# self-contained GOROOT) into a packagefs layout, on a Linux cross-host.
#
# Prereq: a korli/go fork checkout with the DeBeOS haiku/arm64 patchset applied
# (patches/0001..0004) whose host `bin/go` (linux/amd64) has already
# cross-installed the target toolchain:
#
#     GOOS=haiku GOARCH=arm64 CGO_ENABLED=0 GOTOOLCHAIN=local \
#         ./bin/go install std cmd
#
# That populates bin/haiku_arm64/{go,gofmt} and pkg/tool/haiku_arm64/* as
# AArch64 Haiku ELF -- the native toolchain this script packages. (Rebuild
# after any fork patch; the M6 fork+exec fix in patch 0004 MUST be present in
# the native `go`, since `go build` spawns compile/link via that path.)
#
# The package root maps to /boot/system; GOROOT = /boot/system/develop/lib/go.
# Output is a directory ($ROOT) ready for `package create -z zlib` on a Haiku
# box (the `package` tool is not available when cross-compiling from Linux).
set -e
SRC="${SRC:-/local/home/benfelip/go-m0/korli-go}"
ROOT="${ROOT:-/tmp/go-pkg-378/root}"
GOROOT_REL=develop/lib/go
G="$ROOT/$GOROOT_REL"

rm -rf "$ROOT"
mkdir -p "$G/bin" "$G/pkg" "$ROOT/bin" "$ROOT/data/profile.d" "$ROOT/data/licenses"

echo "== native go/gofmt -> GOROOT/bin =="
cp -a "$SRC/bin/haiku_arm64/go"    "$G/bin/go"
cp -a "$SRC/bin/haiku_arm64/gofmt" "$G/bin/gofmt"

echo "== native tools + include =="
cp -a "$SRC/pkg/tool" "$G/pkg/tool"
rm -rf "$G/pkg/tool/linux_amd64"          # drop the host tools; keep haiku_arm64
cp -a "$SRC/pkg/include" "$G/pkg/include"

echo "== std sources + lib + metadata =="
cp -a "$SRC/src"     "$G/src"
cp -a "$SRC/lib"     "$G/lib"
cp -a "$SRC/go.env"  "$G/go.env"
cp -a "$SRC/VERSION" "$G/VERSION"
cp -a "$SRC/LICENSE" "$G/LICENSE"
cp -a "$SRC/LICENSE" "$ROOT/data/licenses/Go-BSD-3-Clause"

echo "== bin/go symlink (relative) + profile.d =="
ln -sf "../$GOROOT_REL/bin/go"    "$ROOT/bin/go"
ln -sf "../$GOROOT_REL/bin/gofmt" "$ROOT/bin/gofmt"
cp -a "$(dirname "$0")/profile.d/go.sh" "$ROOT/data/profile.d/go.sh"

echo "== .PackageInfo =="
VER=$(sed -n '1s/^go//p' "$SRC/VERSION")
sed "s/@VER@/$VER/g" "$(dirname "$0")/golang.PackageInfo" > "$ROOT/.PackageInfo"

echo "== layout =="
du -sh "$G" "$G/src" "$G/pkg/tool" "$G/bin"
ls -l "$ROOT/bin"
echo "staged: $ROOT (version $VER)"
