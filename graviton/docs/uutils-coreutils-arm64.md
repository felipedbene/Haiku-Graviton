# uutils/coreutils → native arm64 hpkg (#93)

[uutils/coreutils](https://github.com/uutils/coreutils) is a Rust re-implementation of
GNU coreutils, published on crates.io as the `coreutils` crate — a single multi-call
binary that dispatches to ~100 applets (`coreutils ls`, `coreutils cat`, …). Because it
is a crate, it is a natural fit for `graviton/scripts/haiku-crate-to-hpkg`
(see `graviton/docs/crate-to-hpkg.md`, #116): build the native arm64 binary with the
on-box Rust closure and package it as an installable DeBeOS `.hpkg`.

This document records the reproducible build, the two workarounds it needs on the
current canonical AMI, and the two Haiku portability gaps in `uucore` that cap which
applets can be included today.

## Reproducible build

On a native Graviton Haiku builder launched from the canonical AMI
(`graviton/scripts/haiku-launch`, default `c7g.large`, driven with
`graviton/scripts/ssm-run`), with the Rust closure installed:

```
pkgman install rust_bin haiku_devel
```

build the portable applet set:

```sh
export CARGO_PROFILE_RELEASE_CODEGEN_UNITS=256      # see "linker" below
haiku-crate-to-hpkg --no-default-features \
  --features "arch base32 base64 basename basenc cat cksum b2sum md5sum nice \
    sha1sum sha224sum sha256sum sha384sum sha512sum comm cp csplit cut \
    dircolors dirname echo expand expr factor false fmt fold head hostid join \
    link ln mkdir mktemp mv nl numfmt od paste pathchk pr printenv printf ptx \
    pwd readlink realpath rm rmdir shred shuf sleep sum test \
    touch tr true truncate tsort uname unexpand uniq unlink wc yes" \
  -r 1 -o ~/crate-hpkg coreutils 0.12.0
```

cargo builds the single multi-call `coreutils` binary (8.77 MB) with 67 applets.

### One required rename before packaging

The base `haiku` package declares `requires coreutils >= 9.9` (GNU coreutils is
packaged as `coreutils`), so an hpkg **named** `coreutils` at version `0.12.0` fails to
install — 0.12.0 does not satisfy `>= 9.9`. Package it under a distinct name
(`-n uutils_coreutils`) and drop the `coreutils = <ver>` provide, keeping the unique
`cmd:coreutils` provide (GNU coreutils ships no `coreutils` binary, so there is no
`cmd:` clash). The shipped artifact is `uutils_coreutils-0.12.0-1-arm64.hpkg`
(3,196,524 bytes, `requires { haiku; lib:libgcc_s }`).

Install and use it multi-call:

```
pkgman install -y uutils_coreutils-0.12.0-1-arm64.hpkg
coreutils cat FILE ; coreutils cp A B ; coreutils sha256sum FILE ; coreutils wc -l FILE
```

(The crate installs the single `coreutils` binary; the per-applet names — `ls`,
`cat`, … — are GNU-style `make install` symlinks in the upstream distro, not produced
by `cargo install`, so on DeBeOS the applets are invoked as `coreutils <applet>`.)

## Why the two flags

1. `CARGO_PROFILE_RELEASE_CODEGEN_UNITS=256` — coreutils 0.12 pulls a heavy ICU/locale
   dependency graph (`icu_locale_core`, `zerovec`, `zerocopy`), whose `zerocopy-derive`
   proc-macro links the very large `syn` crate. At the default 16 codegen units, the
   resulting object is big enough that the AArch64 long-branch **veneer/stub sizer in
   GNU ld chokes** (`can not size stub section: bad value`, alongside a bogus
   `bad reloc symbol index`). Raising codegen-units shrinks each object below the stub
   threshold and the link succeeds. This reproduced with ld 2.46.1 (the current on-box
   linker), so it is a large-object stub-sizing limit, not the older ld-2.41 bug that
   `haiku-crate-to-hpkg` already guards against with `--no-gc-sections`.
2. `--no-default-features --features "<curated set>"` — the escape hatch documented in
   crate-to-hpkg's failure modes, used here to exclude the applets that pull the two
   unportable `uucore` modules below.

## The three Haiku gaps in `uucore` 0.12.0

All three are the same shape: a `#[cfg]` cascade in `uucore` that enumerates specific
operating systems and never lists `target_os = "haiku"`, so Haiku (which *is* `unix`,
and has the underlying POSIX facility) falls into a branch that references symbols its
`libc` binding does not expose, or is excluded from a module it needs. They are what
force the curated feature set.

**These are now fixed.** The follow-up (#93) carries them as a patchset applied to the
crates.io crates at build time — `graviton/uutils/` (patches + a driver). Because
`haiku-crate-to-hpkg` builds crates.io crates *unmodified*, the driver vendors the
crates that need a fix, applies the patches, and points the `coreutils` crate at the
patched copies with a `[patch.crates-io]` override (built through
`haiku-crate-to-hpkg --src-dir`). Building the *full* set surfaced four more gaps of the
same shape outside `uucore`'s three — in `uucore/fs` and in the `uu_ls`, `uu_sort` and
`uu_date` applet crates — covered by the same patchset (see `graviton/uutils/README.md`
for the per-crate list). The two build flags below are still required, and the release
profile must be forced to `lto = off` + `codegen-units = 256`: the crate pins
`lto = "fat", codegen-units = 1`, which does not build on the current on-box aarch64
toolchain (codegen-units = 1 trips GNU ld's aarch64 stub sizer; fat LTO with many units
then intermittently emits an empty/corrupt codegen unit).

### a. `features/i18n/datetime.rs` — `ABMON_*` (excludes `date`, `sort`)

The abbreviated-month-name path is gated
`#[cfg(all(unix, not(android), not(cygwin), not(redox)))]` and calls
`libc::nl_langinfo(libc::ABMON_1..ABMON_12)`. Haiku's `libc` crate binding does not
export the `ABMON_*` `nl_item` constants, so it fails to compile. A numeric-fallback
`else` branch already exists for android/cygwin/redox. **Fix:** add
`not(target_os = "haiku")` to the `nl_langinfo` block and `target_os = "haiku"` to the
fallback — a ~2-line change. `uucore/i18n-datetime` is enabled by `uu_date` and by
`uu_sort` (the latter unconditionally, via its `uucore` dependency-features, even though
its default is only `i18n-collator`), so both `date` and `sort` are excluded until this
is fixed. (`uu_sort` already lists `target_os = "haiku"` in an unrelated `rustix`
exclusion, so the project has partial Haiku awareness here.)

### b. `features/fsext/mod.rs` — `StatFs` / `statfs_fn` / `read_fs_list` (excludes `ls`, and `df`/`du`/`stat`)

`StatFs` and `statfs_fn` are aliased to `libc::statfs`/`libc::statvfs` only for
enumerated OSes; Haiku is in neither list, so both are undefined, and `read_fs_list()`
has no Haiku arm and returns `()` where a `Result` is expected. Haiku has POSIX
`statvfs`, so the type/function aliases are a small addition; `read_fs_list` needs a
real Haiku mount-enumeration (via Haiku's `fs_stat_dev`/`next_dev` APIs) or a stub.
Notably `uu_ls` only uses `fsext::metadata_get_time`/`MetadataTimeField` (which are in a
portable `MetadataExt` block) — it does **not** use the `statfs`/mount code — but merely
enabling the `fsext` feature compiles the whole module. **Fix:** add `target_os = "haiku"`
to the statvfs groups and provide a Haiku `read_fs_list`; that unlocks `ls` immediately
and `df`/`du`/`stat` once the mount enumeration is real.

### c. `lib.rs` signals gate — `uucore::signals` (excludes `dd`, `seq`, `split`, `tail`, `tee`, `tty`)

`pub use crate::features::signals;` is gated to
`any(windows, apple, aix, android, cygwin, freebsd, hurd, illumos, linux, netbsd,
openbsd, redox, solaris)` — Haiku is not in the list, even though Haiku has full POSIX
signals. Any applet that imports `uucore::signals` (for `SIGPIPE`/`SIGUSR1` handling)
then fails with `E0433: could not find signals in uucore`. In our set that is `dd`,
`seq`, `split`, `tail`, `tee`, `tty`. (`cat` and `tr` enable the `signals` *feature* but
never import the module, so they build fine.) **Fix:** add `target_os = "haiku"` to that
gate — a 1-line change that unlocks all six.

## Status

- **Built + installed + applets proven** on native Graviton arm64 (c7g.large,
  canonical AMI `haiku-graviton`, hrev59996). The curated multi-call `coreutils`
  binary carries **67 applets**. `pkgman install` of
  `uutils_coreutils-0.12.0-1-arm64.hpkg` activated live and `coreutils --version`
  reported `coreutils 0.12.0 (multi-call binary)`. Exercised on-box:
  - `echo`/`cat` (round-trip a file), `printf` (`width=42`), `cp` + `wc -c`
    (34/34/68), `sha256sum` (identical hashes for identical files:
    `c2ce295e…b5b5a`), `sha1sum`, `cksum`, `base64` encode|decode round-trip,
    `cut -d: -f2` → `two`, `tr a-z A-Z` → `LOWER`, `head`, `mkdir`/`mv`/`rm`,
    `uname -s -m` → `Haiku arm64`. All correct.
- **Excluded, pending the `uucore` Haiku fixes above:**
  `date`/`sort` (i18n-datetime ABMON), `ls` (fsext) and the always-fsext
  `df`/`du`/`stat`, and `dd`/`seq`/`split`/`tail`/`tee`/`tty` (signals gate).
- **Staged, not published.** The hpkg is a STAGING artifact only
  (`s3://<hpkg-bucket>/scratch/uutils93/uutils_coreutils-0.12.0-1-arm64.hpkg`,
  sha256 `04a7a7d092a533db6720804406c8df09468395b17a2ac1b09ccff3db86e73465`).
  Green-pool publication is a separate, gated step
  (`graviton/scripts/haiku-repo-publish*`) and is deliberately not done here.

## #93 follow-up — the Haiku portability patchset (`graviton/uutils/`)

The gaps above (and four more of the same shape found while building the full set —
`uucore/fs::major/minor/makedev`, `uu_ls`, `uu_sort`, `uu_date`) are fixed as a patchset
applied to the crates.io crates at build time; see `graviton/uutils/README.md`.

**Compile-proven on native Graviton arm64** (`c7g.2xlarge`, canonical AMI
`ami-04493ac7c3fe0d304`, `pkgman install rust_bin haiku_devel`): with the patchset
applied, **every dependency crate and every `uu_*` applet crate — including the
previously-excluded `uu_date`, `uu_sort`, `uu_ls`, `uu_df`, `uu_du`, `uu_stat`, `uu_dd`,
`uu_seq`, `uu_split`, `uu_tail`, `uu_tee`, `uu_tty` — compiles cleanly.** The seven
`#[cfg]` gaps are closed at the source level; nothing Haiku-specific fails to build.

**Open blocker (toolchain, not portability).** Linking the applets into the single
multi-call `coreutils` binary crate (`src/bin/coreutils.rs`) reliably crashes the
on-box `rustc 1.100.0-nightly (aarch64-unknown-haiku)` with an internal compiler error,
in a *different* internal query each run — `resolver_for_lowering_raw`, `rustc_serialize`,
and `adt_def` for `rayon_core::ThreadPoolBuilder` were all seen. It reproduces with a
reduced 20-applet feature set and is independent of the release profile
(`lto = fat` vs `off`, `codegen-units` 1 / 16 / 256, `opt-level` 0 / 3, `RUST_MIN_STACK`
256 MB); under `lto = "fat"` the frontend instead reaches codegen and dies with
`failed to load bitcode … code size is 0` on a different codegen unit each run. Disk and
stack limits were ruled out (40 GiB volume at 27 %, 256 MB stack — 1 GB exceeds Haiku's
512 MB cap and is itself rejected). This is a defect in the large-crate path of this
nightly on aarch64-haiku, not in the applet code, and it blocks producing the hpkg for
the expanded applet set. The fix belongs in the toolchain (a newer `rust_bin`), after
which the patchset here builds the full set unchanged.
