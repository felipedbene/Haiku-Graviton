#!/bin/sh
# build-hpkg.sh -- build debeos-aws from the in-tree source and package it as
# debeos_aws-<ver>-<rev>-arm64.hpkg, natively on a DeBeOS (Haiku) arm64 box.
#
# Issue 867: the build-wave wrapper (run_ssm.py), haiku-nativebuild and the
# provisioner all resolve their S3 client as /boot/system/bin/debeos-aws first.
# /boot/system/bin is packagefs, so the only way a binary lands there is inside a
# package; this script makes that package. haiku-bake-builder runs it on every
# builder bake, so a builder AMI always carries a client built from the tree it
# was baked from -- there is no prebuilt binary to drift.
#
# Inputs, all from the checkout (staged onto the box by the bake):
#   SRCDIR   the debeos-aws module (main.go, go.mod, go.sum). Default: ..
#   TOP      repo root, for graviton/scripts/hpkg-source-manifest. Default: ../../../..
#   OUTDIR   where to write the hpkg. Default: /boot/home
#
# Needs `go` (the DeBeOS golang package, cmd:go) and `package`. The build is
# -mod=readonly: go.sum pins every module by hash, and a build that would have to
# change go.mod or go.sum fails instead of fetching something unpinned.
#
# CRITICAL: -z zlib, for the same reason as the amazon_ssm_agent package: the
# lean image's packagefs has no zstd reader.
set -eu

PKGDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="${SRCDIR:-$(cd "$PKGDIR/.." && pwd)}"
TOP="${TOP:-$(cd "$PKGDIR/../../../.." && pwd)}"
OUTDIR="${OUTDIR:-/boot/home}"
STAGING="${STAGING:-/boot/home/debeos-aws-pkg/root}"
PKGINFO="$PKGDIR/debeos_aws.PackageInfo"
MANIFEST_GEN="$TOP/graviton/scripts/hpkg-source-manifest"

die() { echo "build-hpkg.sh: $*" >&2; exit 1; }

[ -r "$PKGINFO" ] || die "cannot read $PKGINFO"
VERSION="$(sed -n 's/^version[[:space:]]*//p' "$PKGINFO" | head -1)"
printf '%s\n' "$VERSION" | grep -Eqx '[0-9A-Za-z_]+(\.[0-9A-Za-z_]+)*-[0-9]+' \
	|| die "malformed version '$VERSION' in $PKGINFO"
# The binary reports its own version; the package must not claim another one.
SRC_VERSION="$(sed -n 's/^const version = "\(.*\)"$/\1/p' "$SRCDIR/main.go" | head -1)"
[ "$SRC_VERSION" = "${VERSION%-*}" ] \
	|| die "main.go says version '$SRC_VERSION' but $PKGINFO says '$VERSION'"

# The SSM shell's PATH omits /boot/system/bin, so look there, not on PATH.
GO=/boot/system/bin/go
[ -x "$GO" ] || die "no go toolchain (install the golang package: pkgman install golang)"
command -v package >/dev/null 2>&1 || die "no 'package' tool on this host"
# The toolchain must be the one this tree describes: its golang package carries no
# source digest of its own yet, so tie it by version to the tree's recipe and
# PackageInfo revision, and record what was used inside the package.
GO_VER="$(ls "$TOP/graviton/go-arm64/" | sed -n 's/^golang-\(.*\)\.recipe$/\1/p' | head -1)"
GO_REV="$(sed -n 's/^version[[:space:]]*@VER@-\([0-9]*\)$/\1/p' "$TOP/graviton/go-arm64/package/golang.PackageInfo")"
[ -n "$GO_VER" ] && [ -n "$GO_REV" ] || die "cannot read the golang version from $TOP/graviton/go-arm64"
GO_PKG="$(ls /boot/system/packages/ | sed -n '/^golang-/p')"
[ "$GO_PKG" = "golang-$GO_VER-$GO_REV-arm64.hpkg" ] \
	|| die "installed golang package is '${GO_PKG:-<none>}', this tree expects golang-$GO_VER-$GO_REV-arm64.hpkg"
[ -f "$MANIFEST_GEN" ] || die "no $MANIFEST_GEN (stage the tree, not just the module)"

OUT="$OUTDIR/debeos_aws-${VERSION}-arm64.hpkg"
echo "== building debeos-aws $VERSION from $SRCDIR with $("$GO" version) =="
rm -rf "$STAGING"
mkdir -p "$STAGING/bin" "$STAGING/data/licenses" "$STAGING/data/debeos_aws"
(
	cd "$SRCDIR"
	# The SSM shell has no HOME, so go cannot derive its module or build cache.
	GOPATH="${GOPATH:-/boot/home/go}" GOCACHE="${GOCACHE:-/boot/home/.cache/go-build}" \
	CGO_ENABLED=0 GOTOOLCHAIN=local GOFLAGS=-mod=readonly \
		"$GO" build -trimpath -ldflags="-s -w" -o "$STAGING/bin/debeos-aws" .
)
chmod 555 "$STAGING/bin/debeos-aws"
GOT="$("$STAGING/bin/debeos-aws" version)"
[ "$GOT" = "debeos-aws $SRC_VERSION" ] || die "built binary reports '$GOT'"

# Apache-2.0 is not a Haiku system license, so it must ship in the package.
cp "$PKGDIR/Apache-2.0" "$STAGING/data/licenses/Apache-2.0"

# Provenance (#846): the manifest names every in-tree input that ends up in the
# binary; its digest goes into the description (`package list -i` shows it), and
# the manifest itself ships as data/debeos_aws/source-manifest.
MANIFEST="$(sh "$MANIFEST_GEN" --top "$TOP" debeos_aws)"
printf '%s\n' "$MANIFEST" > "$STAGING/data/debeos_aws/source-manifest"
{ "$GO" version; echo "$GO_PKG"; } > "$STAGING/data/debeos_aws/toolchain"
DIGEST="$(sh "$MANIFEST_GEN" --top "$TOP" --digest debeos_aws)"
sed "s/^\(description[[:space:]]*\".*\)\"[[:space:]]*\$/\1 DeBeOS-Source-Digest: $DIGEST\"/" \
	"$PKGINFO" > "$STAGING/.PackageInfo"
grep -q "DeBeOS-Source-Digest: $DIGEST\"" "$STAGING/.PackageInfo" \
	|| die "could not stamp the source digest into the description line"
echo "source digest: $DIGEST"

echo "== package create (-z zlib) =="
rm -f "$OUT"
package create -z zlib -C "$STAGING" -i "$STAGING/.PackageInfo" "$OUT"
package list "$OUT"
echo "built: $OUT"
