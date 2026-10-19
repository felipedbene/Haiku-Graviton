#!/bin/sh
# build-hpkg.sh -- assemble amazon_ssm_agent-<ver>-<rev>-arm64.hpkg on a DeBeOS
# (Haiku) arm64 box, using the native `package` tool.
#
# Runs ON a Haiku arm64 instance (the `package` tool is not available when
# cross-compiling from Linux). Inputs:
#   BINDIR   dir holding the 8 cross-built release binaries (default
#            /boot/home/ssm-real/bin) -- amazon-ssm-agent, ssm-agent-worker,
#            ssm-document-worker, ssm-session-worker, ssm-session-logger,
#            ssm-cli, updater, ssm-setup-cli
#   PKGDIR   dir holding this script's siblings (.PackageInfo, the launch job,
#            Apache-2.0) -- default: the directory this script lives in
#   OUTDIR   where to write the hpkg (default /boot/home)
#
# CRITICAL: -z zlib. The lean image's packagefs has no zstd reader, so a
# zstd-compressed hpkg (Haiku's `package create` default) lands in
# system/packages but FAILS to activate at first boot. zlib activates in both
# the lean image and the full system.
set -e

BINDIR="${BINDIR:-/boot/home/ssm-real/bin}"
PKGDIR="${PKGDIR:-$(cd "$(dirname "$0")" && pwd)}"

# The version comes from .PackageInfo, the only place it is declared. It used to
# be a second hard-coded default here, so bumping one and not the other wrote a
# file whose name disagreed with the package inside it -- and the bake pools and
# UserBuildConfig select this package by FILE NAME. A VERSION override is still
# accepted, but only if it matches.
#
# Parse defensively, because a sloppy value becomes part of the file name and
# still matches the bake glob: take the FIRST `version` line only, drop a CR
# (CRLF checkout), a trailing `#` comment, surrounding whitespace and optional
# quotes, then insist on <ver>[~<pre>]-<rev>, the shape the file name needs.
PKGINFO="$PKGDIR/amazon_ssm_agent.PackageInfo"
if [ ! -r "$PKGINFO" ]; then
	echo "build-hpkg.sh: cannot read $PKGINFO" >&2
	exit 1
fi
PKGINFO_VERSION="$(awk '
	{ sub(/\r$/, "") }
	/^[ \t]*version([ \t]|$)/ {
		sub(/^[ \t]*version/, "")
		sub(/#.*$/, "")
		gsub(/^[ \t]+|[ \t]+$/, "")
		if ($0 ~ /^".*"$/)
			$0 = substr($0, 2, length($0) - 2)
		print
		exit
	}' "$PKGINFO")"
if [ -z "$PKGINFO_VERSION" ]; then
	echo "build-hpkg.sh: no version in $PKGINFO" >&2
	exit 1
fi
if ! printf '%s\n' "$PKGINFO_VERSION" | grep -Eqx \
		'[0-9A-Za-z_]+(\.[0-9A-Za-z_]+)*(~[0-9A-Za-z_.]+)?-[0-9]+'; then
	echo "build-hpkg.sh: malformed version '$PKGINFO_VERSION' in $PKGINFO" \
		"(want <ver>[~<pre>]-<rev>, e.g. 3.3.3270.0-2)" >&2
	exit 1
fi
if [ -n "${VERSION:-}" ] && [ "$VERSION" != "$PKGINFO_VERSION" ]; then
	echo "build-hpkg.sh: VERSION=$VERSION disagrees with .PackageInfo" \
		"($PKGINFO_VERSION); bump the PackageInfo instead" >&2
	exit 1
fi
VERSION="$PKGINFO_VERSION"
OUTDIR="${OUTDIR:-/boot/home}"
STAGING="${STAGING:-/boot/home/ssm-pkg/root}"
OUT="$OUTDIR/amazon_ssm_agent-${VERSION}-arm64.hpkg"

BINARIES="amazon-ssm-agent ssm-agent-worker ssm-document-worker ssm-session-worker ssm-session-logger ssm-cli updater ssm-setup-cli"

echo "== assembling packagefs layout in $STAGING =="
rm -rf "$STAGING"
mkdir -p "$STAGING/bin" "$STAGING/data/launch" "$STAGING/data/licenses"

# Apache-2.0 is not a Haiku system license, so `package create` requires it to
# be bundled in the package (data/licenses/<name>) or it fails validation with
# "License 'Apache-2.0' isn't contained in package!".
cp "$PKGDIR/Apache-2.0" "$STAGING/data/licenses/Apache-2.0"

for b in $BINARIES; do
	cp "$BINDIR/$b" "$STAGING/bin/$b"
	chmod 555 "$STAGING/bin/$b"
done

cp "$PKGDIR/launch-amazon_ssm_agent" "$STAGING/data/launch/amazon_ssm_agent"
cp "$PKGDIR/amazon_ssm_agent.PackageInfo" "$STAGING/.PackageInfo"

echo "== package create (-z zlib) =="
rm -f "$OUT"
package create -z zlib -C "$STAGING" -i "$STAGING/.PackageInfo" "$OUT"

echo "== result =="
ls -la "$OUT"
package list "$OUT" | head -40 || true
echo "sha256:"
sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT"
