#!/bin/sh
# build-hpkg-golang.sh -- runs ON a DeBeOS/Haiku arm64 box. Builds
# golang-<ver>-1-arm64.hpkg from the pre-assembled packagefs layout in ./root
# (produced by stage-goroot.sh on the Linux cross-host). zlib-compressed: the
# lean image's packagefs has no zstd reader, so a zstd hpkg (Haiku's default)
# lands in system/packages but fails to activate at first boot.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${ROOT:-$HERE/root}"
VER=$(sed -n '1s/^go//p' "$ROOT/develop/lib/go/VERSION")
OUT="${OUT:-$HERE/golang-${VER}-1-arm64.hpkg}"

echo "== package create (-z zlib) golang ${VER} =="
rm -f "$OUT"
package create -z zlib -C "$ROOT" -i "$ROOT/.PackageInfo" "$OUT"

echo "== result =="
ls -la "$OUT"
echo "sha256:"
sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT"
package list -i "$OUT" 2>/dev/null | head -30 || true
