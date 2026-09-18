# crate → native arm64 → hpkg (DeBeOS #116)

`graviton/scripts/haiku-crate-to-hpkg` turns a [crates.io](https://crates.io)
crate into an installable native arm64 DeBeOS package. It is a Stage-1
self-sufficiency tool: it lets us build our own Rust CLI tooling on DeBeOS and
package it the same way the rest of the system is packaged, with no cross-host
step and no hand-written recipe.

Given a crate **name** and **version** it:

1. builds the native arm64 binary with the on-box Rust toolchain
   (`cargo install --root <staging>`, release profile),
2. reads the produced ELF to derive the package's `requires {}` (every NEEDED
   shared library that is *not* part of the base `haiku` package becomes a
   `lib:` requirement — e.g. `libgcc_s` → `lib:libgcc_s`),
3. writes a `.PackageInfo` (name, version, `arch=arm64`, `provides` for the
   package and each `cmd:<binary>`, the derived `requires`), and
4. runs Haiku's `package create` to emit `<name>-<version>-<rev>-arm64.hpkg`.

With `--install` it then `pkgman install`s the file and runs each binary to
prove the package activates.

## Prerequisites (the native Rust closure)

Run on a native Graviton Haiku builder — launch one with
`graviton/scripts/haiku-launch` (canonical AMI, 40 GiB root leaves room for a
big crate `target/`) and drive it with `graviton/scripts/ssm-run`. Install the
Rust closure once from the DeBeOS package CDN:

```
pkgman install rust_bin haiku_devel
```

`rust_bin` pulls `binutils`, `gcc`, `curl` and the online-capable `cargo`, and
ships `/boot/system/data/profile.d/rust-devel.sh`, which a **login** shell
sources to set `CARGO_BUILD_RUSTFLAGS=-C link-arg=-Wl,--no-gc-sections` and
`RUST_MIN_STACK`. `haiku_devel` supplies the C-runtime glue (`crti.o` etc.) the
linker needs. Drive the script through a login shell (`bash -lc`) so those are
set; it also re-asserts them itself for non-login shells.

## Usage

```
haiku-crate-to-hpkg [options] <crate> <version>

  -b, --bin NAME            binary the crate installs (repeatable; default: all)
  -n, --pkgname NAME        hpkg name (default: crate name, '-' → '_')
  -o, --outdir DIR          output dir (default: ~/crate-hpkg)
  -r, --revision N          hpkg revision suffix (default: 1)
      --features LIST       cargo --features
      --no-default-features cargo --no-default-features
      --serial              build with cargo --jobs 1 (see failure modes)
      --locked              cargo --locked
      --license NAME        hpkg license (default: MIT)
      --cargo-arg ARG       extra verbatim cargo-install arg (repeatable)
      --install             install the hpkg and run each binary to prove it
```

Proven end-to-end (build → hpkg → `pkgman install` → run) on a `c7g.large`
builder from the canonical AMI:

```
haiku-crate-to-hpkg --install ripgrep 14.1.1     # → rg, 2.8 MB hpkg, runs (NEON)
haiku-crate-to-hpkg --install hyperfine 1.19.0   # → hyperfine, 512 KB hpkg, runs
```

Both emitted `requires { haiku; lib:libgcc_s }`, correctly derived from the ELF.

## Failure modes it handles

- **GNU ld 2.41 aarch64 stub bug** ("can not size stub section" /
  unresolvable `ADR_GOT_PAGE`): rustc defaults to `--gc-sections`, which trips
  it. The script forces `-Wl,--no-gc-sections`.
- **ld 2.41 `.debug_info` bad-reloc on larger crates**: avoided by building
  release with debuginfo off (`CARGO_PROFILE_RELEASE_DEBUG=0`).
- **macro/const-heavy crates overflowing rustc's worker stack** (`SIGKILLTHR`):
  `RUST_MIN_STACK` is raised.
- **parallel-build flakes on a small builder** — parallel rustc intermittently
  fails to spawn a child (Haiku `B_BAD_VALUE`, cargo prints "never executed") or
  corrupts a parallel `.rmeta`. The script detects that signature and retries
  once with `--jobs 1`; `--serial` forces it up front.
- **a crate needing a C build tool / library**: reported as a clear blocker
  naming the failing command, with the ELF NEEDED list of whatever *did* link so
  the missing `cmd:`/`lib:`/`devel:` dependency can be named and installed. Some
  build tools live in the repo — e.g. a crate whose C dependency uses `make`
  needs `pkgman install cmd:make` first.
- **a crate that is simply not portable to Haiku**: `--no-default-features` /
  `--features` drop the offending optional dependency where the crate allows it.

### Known un-portable example

`fd-find` (the `fd` binary) does **not** build as of 10.2.0: it hard-depends on
`jemallocator`/`jemalloc-sys` through a target `cfg` (not a cargo feature, so it
cannot be turned off from the CLI), and `jemalloc-sys`'s bundled C fails on
Haiku (`fatal error: sys/syscall.h: No such file or directory` — Haiku has no
Linux syscall header). Building `fd` needs a Haiku-portable jemalloc or a fork
that makes jemalloc optional; the script reports this as a blocker rather than
producing a broken package.

## Publishing

This script only *produces* the hpkg (locally, under `--outdir`). Publishing to
the green package pool is a separate, gated step
(`graviton/scripts/haiku-repo-publish*`) and is intentionally not done here.
