# uutils/coreutils — uucore Haiku portability patchset (DeBeOS #93)

[uutils/coreutils](https://github.com/uutils/coreutils) 0.12.0 builds natively on
DeBeOS arm64 as a single multi-call binary, but a handful of `#[cfg]` cascades in
`uucore` and a few applet crates "forgot Haiku" and cap which applets can be
included. This directory carries the fixes as a **patchset applied at build time**
to the unmodified crates.io crates, plus a driver that produces the `.hpkg`.

See `graviton/docs/uutils-coreutils-arm64.md` for the background and the applet
inventory.

## Why a patchset (and not `haiku-crate-to-hpkg` directly)

`graviton/scripts/haiku-crate-to-hpkg` builds a crates.io crate **unmodified**, so
`cargo install coreutils 0.12.0` pulls `uucore` and the `uu_*` applet crates from
crates.io with their Haiku gaps intact. `build-uutils-hpkg.sh` instead vendors each
crate that needs a fix, applies its patches, wires the patched copies in with a
`[patch.crates-io]` override in the `coreutils` crate, and hands the checkout to
`haiku-crate-to-hpkg --src-dir` (a small `cargo install --path` option added for
exactly this) so packaging/`requires` derivation stays in one place.

## Layout

- `patches/<crate>/*.patch` — one subdirectory per vendored crate; unified diffs
  applied with `patch -p1`. The driver auto-discovers every `patches/<crate>/`.
- `build-uutils-hpkg.sh` — the driver (run on a native Graviton Haiku builder).

## The gaps (all the same shape: an OS enum that never lists `target_os = "haiku"`)

`uucore/`
1. `0001` **i18n/datetime** — the ABMON month-name path calls
   `libc::nl_langinfo(ABMON_*)`, but libc defines no `ABMON_*` for Haiku; route
   Haiku to the pure-Rust ICU fallback. Unlocks `date`, `sort`.
2. `0002` **fsext** — alias `StatFs`/`statfs_fn` to `libc::statvfs` for Haiku, add
   i64→u64 cast arms to `FsUsage`/`FsMeta` (Haiku's `fsblkcnt_t`/`fsfilcnt_t` are
   *signed*), and implement `read_fs_list()` via the BeOS `fs_info` API
   (`next_dev`/`fs_stat_dev`). Lets `ls`/`dir`/`vdir` compile and `df`/`du`/`stat`
   build and run.
3. `0003` **signals** — add Haiku to both signal-module gates (`features.rs` and
   `lib.rs`) and add a Haiku `ALL_SIGNALS` table (SIGHUP=1 … SIGRESERVED2=32, incl.
   Haiku's SIGKILLTHR=21). Unlocks `dd`, `seq`, `split`, `tail`, `tee`, `tty`.
4. `0004` **fs::major/minor/makedev** — uucore excluded Haiku from
   `pub use libc::{major, makedev, minor}` without a replacement, breaking
   `uu_stat`; add Haiku shims (dev_t is a mount id, not a major/minor pair).

`uu_ls/` `uu_sort/` `uu_date/` — gaps that surface in the applet crates once uucore
builds:
5. `uu_ls` — `uucore::libc::{major, minor}` are undefined on Haiku; provide fallbacks.
6. `uu_sort` — `check_readable` took the `rustix` arm on Haiku while the crate's
   Cargo.toml excludes `rustix` there; widen the cfg to use the `open()` fallback.
7. `uu_date` — `locale.rs` hit `libc::D_T_FMT` (absent for Haiku); route Haiku to
   the default-format fallback like android/cygwin/redox.

## Build

On a native Graviton Haiku builder with the Rust closure
(`pkgman install rust_bin haiku_devel`, plus `cmd:tar cmd:patch cmd:gzip`), driven
over SSM with `bash -lc`:

```sh
graviton/uutils/build-uutils-hpkg.sh [outdir]
```

It fetches coreutils/uucore/uu_ls/uu_sort/uu_date 0.12.0, applies the patchset,
builds the `feat_common_core df du stat` applet set, and emits
`uutils_coreutils-0.12.0-1-arm64.hpkg`. The driver pins the release profile to
`lto = off` + `codegen-units = 256` for the current on-box aarch64 toolchain (the
crate's `lto = "fat", codegen-units = 1` does not build there — see the comments in
the script).

Publishing to the green pool is a separate, gated step
(`graviton/scripts/haiku-repo-publish*`); this driver only produces the artifact.
