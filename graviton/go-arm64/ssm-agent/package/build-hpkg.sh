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

# Provenance (#846): stamp the digest of the source manifest into the package
# description, which is where the bake's check-pool reads it. Nothing else ties
# a pooled hpkg to its source. Two copies of the manifest can exist.
# $BINDIR/SOURCE-MANIFEST is written next to the binaries on the cross-host, so
# it records what they were BUILT from. The tree's copy is computed here, so it
# records what this checkout says they SHOULD be built from. If both are present
# they must agree, which catches old binaries packaged from a newer tree.
MANIFEST_GEN="${MANIFEST_GEN:-$PKGDIR/../../../scripts/hpkg-source-manifest}"
TREE_MANIFEST=""
BIN_MANIFEST=""
[ -f "$MANIFEST_GEN" ] && TREE_MANIFEST="$(sh "$MANIFEST_GEN" amazon_ssm_agent)"
[ -f "$BINDIR/SOURCE-MANIFEST" ] && BIN_MANIFEST="$(cat "$BINDIR/SOURCE-MANIFEST")"
if [ -n "$TREE_MANIFEST" ] && [ -n "$BIN_MANIFEST" ]; then
	if [ "$TREE_MANIFEST" != "$BIN_MANIFEST" ]; then
		echo "build-hpkg.sh: the binaries in $BINDIR were built from different source than this tree" >&2
		printf '%s\n' "$BIN_MANIFEST" > "$STAGING.bin-manifest"
		printf '%s\n' "$TREE_MANIFEST" > "$STAGING.tree-manifest"
		diff "$STAGING.bin-manifest" "$STAGING.tree-manifest" >&2 || true
		echo "Rebuild the binaries from this tree, or package from the tree they came from." >&2
		exit 1
	fi
	MANIFEST="$TREE_MANIFEST"
elif [ -n "$TREE_MANIFEST" ]; then
	echo "build-hpkg.sh: WARNING: no $BINDIR/SOURCE-MANIFEST; stamping this tree's manifest," \
		"which asserts the binaries were built from it" >&2
	MANIFEST="$TREE_MANIFEST"
elif [ -n "$BIN_MANIFEST" ]; then
	MANIFEST="$BIN_MANIFEST"
else
	echo "build-hpkg.sh: no source manifest: neither $MANIFEST_GEN nor $BINDIR/SOURCE-MANIFEST" >&2
	echo "Run from a checkout of the repo, or ship SOURCE-MANIFEST with the binaries." >&2
	exit 1
fi
mkdir -p "$STAGING/data/amazon_ssm_agent"
printf '%s\n' "$MANIFEST" > "$STAGING/data/amazon_ssm_agent/source-manifest"
DIGEST="$(sha256sum "$STAGING/data/amazon_ssm_agent/source-manifest" 2>/dev/null \
	|| shasum -a 256 "$STAGING/data/amazon_ssm_agent/source-manifest")"
DIGEST="${DIGEST%% *}"
sed "s/^\(description[[:space:]]*\".*\)\"[[:space:]]*\$/\1 DeBeOS-Source-Digest: sha256:$DIGEST\"/" \
	"$PKGDIR/amazon_ssm_agent.PackageInfo" > "$STAGING/.PackageInfo"
grep -q "DeBeOS-Source-Digest: sha256:$DIGEST\"" "$STAGING/.PackageInfo" || {
	echo "build-hpkg.sh: could not stamp the source digest into the description line" >&2
	exit 1
}
echo "source digest: sha256:$DIGEST"

echo "== package create (-z zlib) =="
rm -f "$OUT"
package create -z zlib -C "$STAGING" -i "$STAGING/.PackageInfo" "$OUT"

echo "== result =="
ls -la "$OUT"
package list "$OUT" | head -40 || true
echo "sha256:"
sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT"
