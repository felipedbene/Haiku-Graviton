#!/bin/sh
# build-hpkg.sh -- assemble amazon_ssm_agent-<ver>-1-arm64.hpkg on a DeBeOS
# (Haiku) arm64 box, using the native `package` tool.
#
# Runs ON a Haiku arm64 instance (the `package` tool is not available when
# cross-compiling from Linux). Inputs:
#   BINDIR   dir holding the 8 cross-built release binaries (default
#            /boot/home/ssm-real/bin) -- amazon-ssm-agent, ssm-agent-worker,
#            ssm-document-worker, ssm-session-worker, ssm-session-logger,
#            ssm-cli, updater, ssm-setup-cli
#   PKGDIR   dir holding this script's siblings (.PackageInfo, the launch job,
#            agent-env.sh) -- default: the directory this script lives in
#   OUTDIR   where to write the hpkg (default /boot/home)
#
# CRITICAL: -z zlib. The lean image's packagefs has no zstd reader, so a
# zstd-compressed hpkg (Haiku's `package create` default) lands in
# system/packages but FAILS to activate at first boot. zlib activates in both
# the lean image and the full system.
set -e

VERSION="${VERSION:-3.3.3270.0-1}"
BINDIR="${BINDIR:-/boot/home/ssm-real/bin}"
PKGDIR="${PKGDIR:-$(cd "$(dirname "$0")" && pwd)}"
OUTDIR="${OUTDIR:-/boot/home}"
STAGING="${STAGING:-/boot/home/ssm-pkg/root}"
OUT="$OUTDIR/amazon_ssm_agent-${VERSION}-arm64.hpkg"

BINARIES="amazon-ssm-agent ssm-agent-worker ssm-document-worker ssm-session-worker ssm-session-logger ssm-cli updater ssm-setup-cli"

echo "== assembling packagefs layout in $STAGING =="
rm -rf "$STAGING"
mkdir -p "$STAGING/bin" "$STAGING/data/launch" "$STAGING/data/amazon_ssm_agent" \
	"$STAGING/data/licenses"

# Apache-2.0 is not a Haiku system license, so `package create` requires it to
# be bundled in the package (data/licenses/<name>) or it fails validation with
# "License 'Apache-2.0' isn't contained in package!".
cp "$PKGDIR/Apache-2.0" "$STAGING/data/licenses/Apache-2.0"

for b in $BINARIES; do
	cp "$BINDIR/$b" "$STAGING/bin/$b"
	chmod 555 "$STAGING/bin/$b"
done

cp "$PKGDIR/launch-amazon_ssm_agent" "$STAGING/data/launch/amazon_ssm_agent"
cp "$PKGDIR/agent-env.sh"            "$STAGING/data/amazon_ssm_agent/agent-env.sh"
cp "$PKGDIR/amazon_ssm_agent.PackageInfo" "$STAGING/.PackageInfo"

echo "== package create (-z zlib) =="
rm -f "$OUT"
package create -z zlib -C "$STAGING" -i "$STAGING/.PackageInfo" "$OUT"

echo "== result =="
ls -la "$OUT"
package list "$OUT" | head -40 || true
echo "sha256:"
sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT"
