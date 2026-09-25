# uv 0.12.18 port to Haiku aarch64 (DeBeOS) — partial

The **rustls crypto-backend swap** (aws_lc_rs → ring) that unblocks the prior
run's aws-lc-sys wall, plus two reusable upstream-crate patches that any Rust
port on Haiku aarch64 will hit.

## Status

- **Prior wall:** `aws-lc-sys` v0.44 fails to link on Haiku aarch64 — no
  `OPENSSL_cpuid_setup` per-OS C file matches Haiku.
- **This wall:** replaced by a rustc rmeta corruption ICE
  (`Expected header tag [79, 68, 72, 84] but found [0, 0, 0, 0]` while decoding
  `def_path_hash_map`) hit during the top-level `uv` bin crate's
  `resolver_for_lowering_raw` pass. See the "Second blocker" section.

The patches here compile cleanly through `cap-primitives`, `uv-unix`, and every
other Rust source file. **The ring backend swap DOES clear the aws-lc-sys
blocker.** The remaining blocker is environmental (BFS parallel-build rlib
corruption on Haiku), not code-level portability.

## What to apply

Apply these to a fresh `git clone --branch 0.12.18 https://github.com/astral-sh/uv`
on a Haiku aarch64 build box (`graviton/scripts/haiku-launch --type c7g.2xlarge`).
`apply.sh` in this directory drives the whole sequence.

1. `workspace-Cargo.toml.diff` — swap the `rustls` feature `aws_lc_rs` → `ring`
   (drops `prefer-post-quantum`; ring can't do PQ) and the reqwest feature
   `rustls` → `rustls-no-provider` (which no longer force-enables aws-lc-rs on
   hyper-rustls / tokio-rustls / rustls / quinn).  Adds a workspace-level
   `hyper-rustls = { version = "0.27.7", ..., features = ["ring"] }` so
   feature unification enables ring on the hyper-rustls half of the graph
   (reqwest 0.13.4 does not have a native rustls-ring feature).
2. `uv-audit-Cargo.toml.diff` — dev-dep of `reqwest` uses the same
   `"rustls"` name; swap to `"rustls-no-provider"`.
3. `uv-client-Cargo.toml.diff` — add `hyper-rustls.workspace = true` so the
   graph pulls it (feature unification cascade).
4. `uv-Cargo.toml.diff` — add `rustls.workspace = true` to the uv crate so
   we can reference `rustls::crypto::ring` from main.
5. `uv-lib-crypto-install.diff` — install ring as the process default at
   the top of `uv::main` — `rustls-no-provider` no longer registers one.
6. `uv-unix-Cargo.toml.diff` — swap `nix` for `libc` on `target_os = "haiku"`;
   the `nix::sys::resource` module is not implemented for Haiku.
7. `uv-unix-resource_limits.rs` — full replacement file with a Haiku shim.
   Only the `#[cfg(target_os = "haiku")]` `haiku_shim` block is new; the rest
   of the module is unchanged from upstream 0.12.18.
8. `cap-primitives-4.0.3.patch` — five Haiku aarch64 gaps in the rustix
   backend, applied in-place under `~/.cargo/registry/src/…/cap-primitives-4.0.3/`:
     - `src/rustix/fs/dir_entry_inner.rs` — extend the `illumos, solaris`
       cfg to also cover `haiku` (Haiku's rustix DirEntry has no `file_type()`,
       fall back to the metadata path like illumos/solaris do).
     - `src/rustix/fs/dir_utils.rs` — add `target_os = "haiku"` to the
       `OFlags::empty()` cfg block (Haiku has no `O_PATH`, so the block that
       returns `OFlags::PATH` doesn't fire, and the code falls through to
       `()` instead of returning `OFlags`).
     - `src/rustix/fs/metadata_ext.rs` — Haiku's `struct stat` has signed
       `st_ino` (`i64`) and signed `st_nlink` (`i32`); replace `.into()`
       and `u64::from(...)` with `as u64`.
     - `src/rustix/fs/oflags.rs` — add `target_os = "haiku"` to the
       NOT-list for the `RSYNC` gate (rustix on Haiku has no `OFlags::RSYNC`).

## Build settings that DO work

The mandatory flags from the prior run are all still required:

```
CARGO_INCREMENTAL=0
CARGO_PROFILE_RELEASE_DEBUG=0
CARGO_PROFILE_RELEASE_LTO=false
CARGO_BUILD_RUSTFLAGS="-C codegen-units=1 -C link-arg=-Wl,--no-gc-sections"
RUST_MIN_STACK=268435456
cargo build --release --no-default-features -p uv
```

Source `/boot/system/data/profile.d/rust-devel.sh` first — it sets the
`--no-gc-sections` link-arg and the 256 MiB thread stack that the ld 2.41
aarch64 stub bug and heavy macro expansion need respectively.

**LTO must be OFF.** With `-C lto=fat` rustc 1.100.0-nightly ICEs at
`exported_generic_symbols` during LTO on aarch64-unknown-haiku for this graph;
the panic in that mode is a distinct rustc bug from the rmeta corruption
one described below.

## Second blocker — rustc rmeta corruption at the top uv bin

After the ring swap compiles cleanly through cap-primitives, uv-unix, and
every other Rust source file, the FINAL rustc invocation on
`crates/uv/src/bin/uv.rs` panics:

```
thread 'rustc' panicked at compiler/rustc_metadata/src/rmeta/def_path_hash_map.rs:56:13:
decode error: Expected header tag [79, 68, 72, 84] but found [0, 0, 0, 0]
```

The header `[79, 68, 72, 84]` is `ODHT` — one of the dep rlibs was written to
BFS with a zeroed `def_path_hash_map` section header. Re-running the top
uv-bin rustc with `-j 1` reproduces the same ICE (the on-disk rlib is genuinely
corrupted), so a `cargo clean` + fully-serial `cargo build --jobs 1` is needed
to work around it. This is the memoried "parallel rustc corrupts a `.rmeta`"
BFS issue, not code portability.

## Reproduction summary

- Instance: `c7g.2xlarge` from canonical AMI `ami-032bdab7af564b2ea` (baked
  DeBeOS repo, rust_bin 1.100.0-nightly, ld Binutils 2.46.1).
- Toolchain: `pkgman install rust_bin haiku_devel git cmd:perl`.
- Time to full-rebuild: ~55-90 min at `-j 4`; ~4-6 h at `-j 1`.
