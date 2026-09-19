#!/bin/sh
# build-uutils-hpkg.sh -- build the uutils/coreutils multi-call binary for
# DeBeOS arm64 with the uucore Haiku portability fixes applied (DeBeOS #93),
# and package it as an installable .hpkg.
#
# WHY THIS EXISTS: `graviton/scripts/haiku-crate-to-hpkg` builds a crates.io
# crate UNMODIFIED. `cargo install coreutils 0.12.0` therefore pulls uucore (and
# the uu_* applet crates) from crates.io with their Haiku `#[cfg]` gaps still
# present (see graviton/docs/uutils-coreutils-arm64.md), which caps the applet
# set. This driver carries those fixes as a patchset -- one subdirectory per
# crate under graviton/uutils/patches/<crate>/*.patch -- vendors each patched
# crate, points the coreutils crate at them with [patch.crates-io] overrides,
# and hands the resulting checkout to `haiku-crate-to-hpkg --src-dir` (so
# packaging/requires derivation stays in ONE place). All crates are fetched over
# the box's online cargo/curl closure -- no git host is needed.
#
# Patched crates (each at the same 0.12.0 version, so [patch.crates-io] matches):
#   uucore  -- the three uucore Haiku gaps (i18n-datetime ABMON, fsext statvfs +
#              read_fs_list, signals gate + ALL_SIGNALS).
#   uu_ls   -- major()/minor() are undefined in libc for Haiku (a fourth gap in
#              the applet crate, surfaced once uucore builds).
#
# WHERE IT RUNS: on a native Graviton Haiku builder with the Rust closure
# (`pkgman install rust_bin haiku_devel`), the same box haiku-crate-to-hpkg
# needs. Drive it over SSM with `bash -lc` so rust_bin's profile.d is sourced.
# `patch` is required to apply the patchset; the driver installs it if missing.
#
# USAGE: build-uutils-hpkg.sh [outdir]        (default outdir: ~/crate-hpkg)
#
# It PRODUCES an hpkg (STAGING artifact). Publishing to the green pool is a
# separate, gated step (graviton/scripts/haiku-repo-publish*) and is NOT done here.

set -eu

CRATE=coreutils
VERSION=0.12.0
# Every patched crate is vendored at this version (matches coreutils 0.12.0's
# own dependency versions, so the [patch.crates-io] overrides resolve).
PATCHED_VERSION=0.12.0
PKGNAME=uutils_coreutils
REVISION=1
OUTDIR="${1:-$HOME/crate-hpkg}"

# The applet set: feat_common_core (77 applets: includes date, sort, ls, dir,
# vdir, dd, seq, split, tail, tee, tty -- the gap-a and gap-c unlocks and the ls
# side of gap-b) plus df/du/stat (the fsext statfs/read_fs_list side of gap-b).
FEATURES="feat_common_core df du stat"

# Release-profile overrides for the current on-box aarch64 toolchain. The
# coreutils crate pins `[profile.release] lto = "fat", codegen-units = 1`, but on
# Haiku arm64 that combination does not build:
#   * codegen-units = 1 makes one enormous object and GNU ld's aarch64 long-branch
#     stub sizer chokes ("can not size stub section"), so we raise codegen-units.
#   * fat LTO + many codegen units then intermittently yields an empty codegen
#     unit whose bitcode fails to load ("failed to load bitcode ... code size is
#     0", a different CGU each run) -- a parallel-codegen flake in this toolchain.
# Turning LTO off removes the cross-module bitcode step entirely while the high
# codegen-units keeps every object small enough for ld. The binary is a touch
# larger/less-optimised than a fat-LTO build but correct.
export CARGO_PROFILE_RELEASE_LTO="${CARGO_PROFILE_RELEASE_LTO:-off}"
export CARGO_PROFILE_RELEASE_CODEGEN_UNITS="${CARGO_PROFILE_RELEASE_CODEGEN_UNITS:-256}"

progname=build-uutils-hpkg
say() { printf '%s: %s\n' "$progname" "$*" >&2; }
die() { printf '%s: %s\n' "$progname" "$*" >&2; exit 1; }

HERE=$(cd "$(dirname "$0")" && pwd)
PATCHDIR="$HERE/patches"
C2H="$HERE/../scripts/haiku-crate-to-hpkg"

[ -x "$C2H" ] || C2H=$(command -v haiku-crate-to-hpkg 2>/dev/null) \
	|| die "haiku-crate-to-hpkg not found next to this script or on PATH"
command -v cargo >/dev/null 2>&1 || die "cargo not found -- 'pkgman install rust_bin haiku_devel'"
command -v curl  >/dev/null 2>&1 || die "curl not found -- ships with rust_bin"
[ -d "$PATCHDIR" ] || die "patch dir $PATCHDIR missing"

# `patch` is needed to apply the uucore fixes; pull it if the box lacks it.
if ! command -v patch >/dev/null 2>&1; then
	say "patch(1) not found -- installing it with pkgman"
	pkgman install -y cmd:patch >&2 || pkgman install -y patch >&2 \
		|| die "could not install patch"
fi

WORK=$(mktemp -d /tmp/uutils93.XXXXXX) || die "cannot make a work dir"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

fetch_crate() {  # <name> <version>  -> extracts <name>-<version>/
	say "fetching $1 $2 from crates.io"
	curl -fsSL "https://crates.io/api/v1/crates/$1/$2/download" -o "$1.crate" \
		|| die "download of $1 $2 failed"
	tar xzf "$1.crate" || die "extract of $1 $2 failed"
	[ -d "$1-$2" ] || die "expected $1-$2/ after extracting $1.crate"
}

fetch_crate "$CRATE" "$VERSION"

# One subdirectory per crate under patches/. Vendor each, apply its patches, and
# record a [patch.crates-io] override. [patch] applies transitively, so every
# uu_* crate that depends on the patched crate at 0.12.0 builds against the copy.
PATCH_BLOCK="
[patch.crates-io]"
for cratedir in "$PATCHDIR"/*/; do
	[ -d "$cratedir" ] || continue
	pcrate=$(basename "$cratedir")
	fetch_crate "$pcrate" "$PATCHED_VERSION"
	say "applying $pcrate Haiku patches"
	for p in "$cratedir"*.patch; do
		[ -f "$p" ] || continue
		say "  $(basename "$p")"
		( cd "$pcrate-$PATCHED_VERSION" && patch -p1 < "$p" ) \
			|| die "failed to apply $(basename "$p")"
	done
	PATCH_BLOCK="$PATCH_BLOCK
$pcrate = { path = \"../$pcrate-$PATCHED_VERSION\" }"
done

printf '%s\n' "$PATCH_BLOCK" >> "$CRATE-$VERSION/Cargo.toml"

say "building via haiku-crate-to-hpkg --src-dir (features: $FEATURES)"
HG_PKG_SUMMARY="uutils/coreutils (Rust GNU-coreutils, native arm64, DeBeOS #93 uucore Haiku fixes)" \
HG_PKG_DESC="uutils/coreutils $VERSION multi-call binary, native arm64, with the uucore Haiku portability patchset (DeBeOS #93) so date/sort/ls/df/du/stat/dd/seq/split/tail/tee/tty are included." \
"$C2H" \
	-n "$PKGNAME" -r "$REVISION" -o "$OUTDIR" \
	--no-default-features --features "$FEATURES" \
	--src-dir "$WORK/$CRATE-$VERSION" \
	"$CRATE" "$VERSION"
