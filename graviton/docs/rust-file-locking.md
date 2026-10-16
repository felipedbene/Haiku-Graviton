# Rust file locking on DeBeOS: `File::lock` is a silent no-op, and cargo believes it

DeBeOS issue 638. Status: root cause **VERIFIED**; the upstream code fix is **merged**;
the from-source `rust_bin` re-bake carrying it is **DONE** (2026-10-16, base commit
`e9bbadf68`, artifact `rust_bin-1.101.0~nightly_e9bbadf68-1-arm64.hpkg`), and the bake
is now reproducible from `graviton/scripts/haiku-rust-bake` instead of being
archaeology. What is still **OWED** is the gated publish into the green pool, the pin
bump that follows it, and the *after* half of the on-device reproduction. Interim
serialisation locks are **landed** in this tree. There is still no prebuilt arm64-Haiku
toolchain to bump a pin to — upstream neither publishes Haiku artifacts nor even defines
the `aarch64-unknown-haiku` target, so every refresh is a from-source cross-build.

## TL;DR — what to do

1. **Builders:** nothing to do if you only ever run one `cargo` at a time per box.
   If you run more than one, they corrupt each other. The four drivers in this repo
   that could overlap now take a lock themselves (see *Interim mitigation* below).
2. **Real fix:** re-bake `rust_bin` at an upstream base commit **≥ `nightly-2026-10-13`**
   by running `graviton/scripts/haiku-rust-bake all` from an aarch64 Linux desk. The fix
   is already upstream; DeBeOS does not need to carry a patch. But this is **not a pin
   bump and there is nothing to download** — upstream publishes no Rust artifact for any
   Haiku target, and `aarch64-unknown-haiku` is not even an upstream rustc target. It is
   a from-source cross-build. Done once already; see *Upstream state*, *What a re-bake
   requires*, and *The re-bake as actually performed*.
3. **Do not** set a global `build.target-dir` / `CARGO_TARGET_DIR` in
   `graviton/ssh/files/cargo-config.toml`. Today every cargo on the box gets its own
   default target dir; a global one would merge them all into a single *unlocked*
   shared directory and manufacture the very race this document is about.

## The defect, in three layers

**Layer 1 — std does not wire Haiku up to `flock()`.** `File::lock`, `lock_shared`,
`try_lock`, `try_lock_shared` and `unlock` are implemented in
`library/std/src/sys/fs/unix.rs` behind a `cfg_select!` allowlist of target OSes.
`target_os = "haiku"` was absent from all five, so every one of them fell through to
the `_ =>` arm and returned `ErrorKind::Unsupported`.

**Layer 2 — cargo treats "unsupported" as "acquired", silently.** In cargo's
`src/util/flock.rs`:

```rust
    match lock_try() {
        Ok(()) => Ok(true),

        // In addition to ignoring NFS which is commonly not working we also
        // just ignore locking on filesystems that look like they don't
        // implement file locking.
        Err(TryLockError::Error(e)) if error_unsupported(&e) => Ok(true),
```

and `error_unsupported()` ends with `_ => err.kind() == std::io::ErrorKind::Unsupported`.
`Ok(true)` is cargo's "lock acquired" value, so cargo proceeds as the sole owner and
never prints `Blocking waiting for file lock`. There is no warning of any kind.

**Layer 3 — consequence.** Two cargo invocations that share a `--target-dir`, or that
share `$CARGO_HOME` (`~/.cargo/registry`, `registry/src`, `registry/cache`,
`.package-cache`), both believe they hold the lock and interleave writes to
`deps/`, `.fingerprint/`, build scripts and `.rmeta`/`.rlib` files. The result is a
torn artifact that *stays cached*, so the resulting rustc ICE recurs on every
subsequent build until the artifact is deleted.

## The kernel premise: Haiku's `flock()` works — VERIFIED

This matters, because if `flock()` were a stub then adding Haiku to std's allowlist
would make things worse, not better. It is not a stub. The path is wired end to end
in this tree:

| Layer | Location |
| --- | --- |
| Constants + prototype | `headers/posix/sys/file.h:13-23` |
| Userland wrapper | `src/system/libroot/posix/sys/flock.c:16-29` |
| Syscall | `headers/private/system/syscalls.h:282` (`_kern_flock`) |
| Kernel entry | `src/system/kernel/fs/vfs.cpp:9454-9502` (`_user_flock`) |
| Lock machinery | `src/system/kernel/fs/vfs.cpp:1806-1888` (`acquire_advisory_lock`), `:1689-1794` (`release_advisory_lock`), `:181-214` (structs) |

`LOCK_SH/EX/NB/UN` are `0x01/0x02/0x04/0x08` — identical to Linux and the BSDs, so
`libc`'s constants for the Haiku target line up.

What std's flock-based implementation assumes, and what it gets:

| std assumes | DeBeOS kernel | |
| --- | --- | --- |
| `flock()` is real | `_kern_flock` → `_user_flock` | OK |
| `LOCK_SH` is genuinely shared | `advisory_lock::shared`, checked at `vfs.cpp:1841` | OK |
| `LOCK_NB` conflict → `EWOULDBLOCK`/`EAGAIN` | `B_WOULD_BLOCK` at `vfs.cpp:1852-1857`; `EAGAIN == EWOULDBLOCK == B_TO_POSIX_ERROR(B_WOULD_BLOCK)` | OK — so `try_lock` correctly yields `WouldBlock`, not an error |
| BSD per-open-file ownership; `dup`/`fork` share; released on last close | owner key is `struct file_descriptor*` (`vfs.cpp:1814`); release gated on `open_count` reaching 0 (`fd.cpp:230` → `file_close`, `vfs.cpp:5789-5810`) | OK |
| `LOCK_EX` on an `O_RDONLY` fd succeeds | no mode check in the flock path; the check exists only for `fcntl` (`vfs.cpp:6530-6535`) | OK |
| advisory, not mandatory | advisory only | OK |

`vfs.cpp:1797-1805` states the intent explicitly: "If `descriptor` is NULL, POSIX
semantics are used for this lock. Otherwise, BSD `flock()` semantics are used".

### Semantic mismatches worth knowing

These do not block the fix — cargo uses whole-file exclusive locks taken and
released on one fd — but they are real and are why upstream did *not* simply treat
Haiku like Linux in the test suite.

1. **`LOCK_EX` → `LOCK_SH` downgrade is a silent no-op.** `acquire_advisory_lock`
   skips same-owner locks (`vfs.cpp:1834-1847`, with the standing
   `// TODO: locks from the same team might be joinable!`) and then unconditionally
   appends a *new* record (`:1870-1884`). There is no merge and no conversion, so the
   exclusive record survives a downgrade and other processes stay blocked.
   Upstream's own test change reflects this: in `library/std/src/fs/tests.rs` the
   `file_lock_double` case now reads
   `if cfg!(any(windows, target_os = "haiku"))`, i.e. **Haiku is grouped with Windows,
   not with Unix, for downgrade behaviour.** Independent confirmation of the kernel
   reading above.
2. **A forked child re-locking the inherited fd conflicts with itself.** The owner
   test is `lock->team != team || lock->bound_to != boundTo`, so the `team_id` term
   makes a child's `flock(LOCK_EX)` on its parent's fd collide with its parent's own
   lock. BSD/Linux would treat that as the same holder and succeed.
3. **Repeated `lock()` without `unlock()` leaks one `advisory_lock` per call.**
   `LOCK_UN` does clean all of them up.
4. **No deadlock detection** — `vfs.cpp:1817` `// TODO: do deadlock detection!`. A
   blocking `File::lock` cycle hangs until a signal produces `EINTR`, which std
   surfaces as `ErrorKind::Interrupted` and does *not* retry.
5. **`EINVAL` for non-regular-file fds.** `_user_flock` rejects any fd whose
   `descriptor->ops != &sFileOps` (`vfs.cpp:9474`), so `flock()` on a directory,
   pipe or socket fails where Linux allows it.
6. **flock and fcntl locks share one per-vnode list**, so they conflict with each
   other across processes — stricter than Linux, where the two namespaces are
   independent.

Unrelated bug noticed in the same code, now **fixed** in this tree:
`release_advisory_lock` overwrote `lock->end` *before* copying it into the split-off
tail record, so an `fcntl` middle-range unlock of a lock that extended past both ends
of the released range produced a tail record of `[l_start + l_len, l_start - 1]` — an
inverted, never-matching range. The effect was that the tail region silently lost its
lock, and the phantom record stayed on the vnode until a whole-file release swept it.
The truncation now happens after the copy (`vfs.cpp`, `release_advisory_lock`, the
"divide the lock into two locks" branch). This was never reachable from `flock()`,
which only ever locks `[0, OFF_MAX)` and so never splits — so it is **not** a cause of
#638. It is reachable from `fcntl(F_SETLK, F_UNLCK)` byte-range users, SQLite being the
obvious one.

## Upstream state: already fixed — bump, do not patch

The fix landed upstream while this issue was open.

- PR **rust-lang/rust#164031**, "Adding File Lock Support for Haiku".
- Commit `5a1dda9adcb36e10281b5b9ed34fc7b0e98dce00`, authored 2026-10-09.
- Merged to `master` in rollup `443d6582c141f7228739032c9924ab2a3fd2731d`, **2026-10-12 09:30 UTC**.
- It adds `target_os = "haiku"` to all five `cfg_select!` lists in
  `library/std/src/sys/fs/unix.rs`, plus the five test `cfg`s and the
  `file_lock_double` grouping in `library/std/src/fs/tests.rs`.

Which toolchains have it (established by fetching `library/std/src/sys/fs/unix.rs` at
each revision and counting `target_os = "haiku"` inside the lock region):

| Toolchain | Has the fix |
| --- | --- |
| Every release tag `1.88.0` … `1.99.0` | **no** |
| `1.100.0-nightly (787af2b8c 2026-08-25)` — what `rust_bin` ships today | **no** |
| `nightly-2026-10-12` = rustc `1.101.0-nightly (080cc2714 2026-10-11)` | **no** |
| `nightly-2026-10-13` = rustc `1.101.0-nightly (e9bbadf68 2026-10-12)` | **yes** |

So **`nightly-2026-10-13` is the first good nightly.** A `rust_bin` built from that
or newer needs no DeBeOS patch at all.

### What a re-bake requires

**Checked 2026-10-15. There is no upstream artifact to fetch, so "bump the pin" is not
an available move.** Two independent verifications:

- **Upstream publishes nothing for Haiku, on any architecture.** The nightly channel
  manifest `https://static.rust-lang.org/dist/2026-10-13/channel-rust-nightly.toml`
  (923,466 bytes, `date = "2026-10-13"`) contains **zero** occurrences of `haiku` —
  no `rustc`, no `rust-std`, for `i686`/`x86_64` either. Haiku is tier 3 and tier-3
  targets are not distributed. That same manifest pins
  `git_commit_hash = e9bbadf6824e65b3583c2dda3a0926ec76df9143`, confirming
  `nightly-2026-10-13` is indeed the first nightly carrying rust-lang/rust#164031
  (merged 2026-10-12, title "Adding File Lock Support for Haiku").
- **`aarch64-unknown-haiku` is not an upstream rustc target at all.** Upstream
  `compiler/rustc_target/src/spec/mod.rs` on `master` today registers only
  `("i686-unknown-haiku", …)` and `("x86_64-unknown-haiku", …)` (`:1415-1416`). The
  arm64 target exists **solely** because DeBeOS added it.

So the shipped `rust_bin` is a **DeBeOS-built artifact, cross-compiled from the Linux
desk**, and its inputs are preserved out of band in
`s3://haiku-graviton-668984504585-us-west-2/rust/` (all dated 2026-08-26):

| key | what it is |
|---|---|
| `recipe/rust-haiku-recipe.tar.gz` | the whole port, 6 files (below) |
| `recipe/rust-haiku-sysroot.tar.gz` | the link sysroot the wrappers point `-L` at |
| `dist/rustc-nightly-aarch64-unknown-haiku.tar.{xz,gz}` | built rustc for the target |
| `dist/rust-std-nightly-aarch64-unknown-haiku.tar.{xz,gz}` | built std for the target |
| `hpkg/rust_bin-1.100.0~nightly_787af2b8c-{1,2}-arm64.hpkg` | the packed result; `-2` is what is pinned |
| `cargo-dyncurl/cargo-1.100.0-dyncurl-arm64` | cargo, built separately with dynamic curl |
| `SHA256SUMS` | digests for all of the above |

The port itself is small — `recipe/` holds `aarch64_unknown_haiku.rs` (the rustc target
spec: `+v8a,+lse,+rcpc,+dotprod,+aes,+sha2`, `tier: 3`, `host_tools: false`, PIE, no
SVE because nothing on Haiku sets `CPACR_EL1.ZEN`), `aarch64-unknown-haiku.json` (the
same thing as a JSON target), `mod.rs.registration.diff` (the one line registering it),
`haiku-rust-gcc` / `haiku-rust-g++` (wrappers invoking this tree's
`cross-tools-arm64/bin/aarch64-unknown-haiku-gcc` with `-lbsd -lnetwork -lgnu`), and
`rust-src-tree.base-commit.txt` = `787af2b8c80638c51a4fc8e44f84e6891f243ec7`.

**Procedure for the re-bake** (not attempted here — it produces a toolchain artifact):

1. Check out `rust-lang/rust` at a commit **≥ `e9bbadf6824e65b3583c2dda3a0926ec76df9143`**
   (`nightly-2026-10-13`). #164031 is already in it; **do not** apply the std patch
   reproduced above — at that commit it is redundant.
2. Re-apply the port from `recipe/`: drop `aarch64_unknown_haiku.rs` into
   `compiler/rustc_target/src/spec/targets/`, then register it. **The stored
   `mod.rs.registration.diff` will not apply as-is** — it was generated against
   `787af2b8c`, where the Haiku block sat near `:1403`; on master it is at
   `:1415-1416`. It is a one-line re-base, not a port.
3. Unpack `rust-haiku-sysroot.tar.gz` and install the two wrappers. Both **hardcode**
   `/opt/haiku/haiku/generated.arm64/cross-tools-arm64/bin/` and
   `/opt/haiku/rust-haiku-sysroot/lib`; either reproduce those paths or edit the
   wrappers.
4. Build **rustc, rust-std *and* cargo** for `aarch64-unknown-haiku`. Cargo is not
   optional: the defect is cargo's own `File::lock` call, which lives in the cargo
   binary and was compiled against cargo's own std. A refreshed `rust-std` alone fixes
   `File::lock` for *user* crates and leaves cargo's target-dir and package-cache
   locking exactly as broken as it is today. That is the whole point of this issue.
5. Repack as `rust_bin-1.101.0~nightly_<short-sha>-1-arm64.hpkg`.
6. Update the pinned filename at `graviton/ssh/UserBuildConfig:317`
   (today `rust_bin-1.100.0~nightly_787af2b8c-2-arm64.hpkg`).
7. Publish via `haiku-repo-add` → `haiku-repo-promote-green` (revision bump, cf.
   `haiku-repo-add:509`), and populate the `s3://<workBucket>/hpkg-pool/` prefix that
   `graviton/pipeline/lib/haiku-graviton-pipeline-stack.ts:206-210` notes is
   "Populated out-of-band".
8. Confirm `File::lock` blocks rather than returning `Unsupported`, and run the
   two-concurrent-`cargo build --target-dir T` reproduction.

> **Gap now closed.** The 2026-08-26 bake's *driving build script* was never preserved —
> only the six recipe inputs were in S3, `bake-scripts/` held nothing Rust-related, and
> the tree had no `x.py` invocation or `bootstrap.toml`. It was reconstructed on
> 2026-10-16 and committed as **`graviton/scripts/haiku-rust-bake`**, so steps 1–5 above
> are now `haiku-rust-bake all` and the next refresh is not another archaeology
> exercise. The reconstruction and the traps it encodes are written out in full in *The
> re-bake as actually performed* below.

A newer `rust_bin` is wanted for unrelated reasons too, so this is not a cost carried
only by issue 638: `graviton/docs/archive/uutils-coreutils-arm64.md:174-186` ("The fix
belongs in the toolchain (a newer `rust_bin`)"),
`graviton/patches/uv-0.12.18-arm64/README.md:85-101`,
`graviton/audit/doc-triage-phase1.md:91`.

## The re-bake as actually performed — 2026-10-16

Run from an aarch64 Amazon Linux 2023 desk, cross-compiling to
`aarch64-unknown-haiku`. Driver: `graviton/scripts/haiku-rust-bake`
(`fetch-inputs | port | std | dist | cargo | pack | all`). Base commit
**`e9bbadf6824e65b3583c2dda3a0926ec76df9143`** (`nightly-2026-10-13`, rustc
`1.101.0-nightly`). One package had to be installed: `ninja-build`. No rust was needed
on the desk — `x.py`'s `bootstrap.py` downloads its own stage0.

**The fix is gated by content, not by commit id**, so a different base cannot silently
skip it:

```sh
n="$(grep -c 'target_os = "haiku"' library/std/src/sys/fs/unix.rs)"
[ "$n" -ge 10 ] || die "base commit lacks rust-lang/rust#164031"
```

### `bootstrap.toml`

```toml
change-id = 162423

[llvm]
download-ci-llvm = false      # mandatory; see below
ninja = true
# Do NOT set llvm.targets / llvm.experimental-targets: they are global, so they would
# also narrow the Haiku-hosted LLVM and ship a rustc that can only emit AArch64.

[build]
build  = "aarch64-unknown-linux-gnu"
host   = ["aarch64-unknown-haiku"]
target = ["aarch64-unknown-haiku"]
docs = false
compiler-docs = false
extended = false
tools = ["rustdoc"]           # rustdoc only; cargo is built standalone
locked-deps = true
vendor = false
jobs = 16

[rust]
channel = "nightly"
download-rustc = false
debug = false
debuginfo-level = 0
backtrace = true
incremental = false

[dist]
compression-formats = ["gz"]

[target.aarch64-unknown-linux-gnu]
cc = "gcc"
cxx = "g++"

[target.aarch64-unknown-haiku]
cc     = "<workdir>/bin/haiku-rust-gcc"
cxx    = "<workdir>/bin/haiku-rust-g++"
linker = "<workdir>/bin/haiku-rust-gcc"
ar     = "<haiku-tree>/generated.arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-ar"
ranlib = "<haiku-tree>/generated.arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-ranlib"
crt-static = false
```

### The `x.py` command lines

```sh
cd <workdir>/rust
BOOTSTRAP_SKIP_TARGET_SANITY=1 python3 x.py build library --target aarch64-unknown-haiku
BOOTSTRAP_SKIP_TARGET_SANITY=1 python3 x.py dist rustc rust-std
```

`BOOTSTRAP_SKIP_TARGET_SANITY=1` is **not optional**. Bootstrap validates
`build.target` against the *stage0* rustc's target list (`src/bootstrap/src/core/sanity.rs:292`),
stage0 is an upstream nightly, and upstream only knows `i686`/`x86_64-unknown-haiku` —
so it rejects `aarch64-unknown-haiku` before the stage1 compiler that *does* know it has
been built:

```
thread 'main' panicked at src/bootstrap/src/core/sanity.rs:292:
aarch64-unknown-haiku: No such target exists in the target list,
did you accidentally make a typo? Use BOOTSTRAP_SKIP_TARGET_SANITY=1 to bypass this check.
```

### Why `download-ci-llvm` must be `false`

This is the one trap that costs an afternoon. The build host is tier 1 and CI LLVM
*does* exist for it, so the obvious setting is `true`. It cannot be. Cross-building host
tools hands `rustc_llvm`'s build script the **build host's** `llvm-config` (the target's
is a Haiku binary and will not execute on Linux) and then rewrites the build triple to
the target triple in every path it prints — `compiler/rustc_llvm/build.rs:286` for `-I`
flags, `:453` for link search dirs. That only works if the two LLVMs are laid out and
configured identically, which the build script admits in as many words ("This only
really works if the host LLVM and target LLVM are compiled the same way"). With CI LLVM
they are not, and it fails twice:

1. **Paths.** CI LLVM unpacks to `build/<build-triple>/ci-llvm`, the cross-built one
   lands in `build/<target>/llvm`, so the rewrite yields `build/<target>/ci-llvm/include`
   — nonexistent: `llvm-wrapper/LLVMWrapper.h:6:10: fatal error: llvm/Config/llvm-config.h: No such file or directory`.
2. **Link type**, which symlinking the path does *not* cure. CI LLVM is built
   `LLVM_LINK_LLVM_DYLIB=ON` and ships only `libLLVM-*.so`, no static archives, while
   bootstrap configures the cross LLVM `LLVM_LINK_LLVM_DYLIB=OFF`. So
   `llvm-config --link-static --libs …` on the CI LLVM answers
   `missing: …/ci-llvm/lib/libLLVMDemangle.a` for every component and exits 1.

Building LLVM for the build host locally (~20 min) puts a static `llvm-config` under
`build/<build-triple>/llvm`, and both the path rewrite and `--link-static` then agree.
`x.py dist rustc rust-std` took 0:23:34 after that.

Setting `llvm.targets` alongside `download-ci-llvm` is rejected outright anyway:
`Setting \`llvm.targets\` is incompatible with \`llvm.download-ci-llvm\``
(`src/bootstrap/src/core/download.rs:339`).

### The cross-gcc wrappers, re-pointed

The preserved wrappers hardcode `/opt/haiku/...` paths that exist on no current desk.
The committed script regenerates them against the local tree and a **private** sysroot:

```sh
#!/bin/sh
exec <xbin>/aarch64-unknown-haiku-gcc --sysroot=<sysroot> -D_DEFAULT_SOURCE \
  -L<sysroot>/boot/system/develop/lib "$@" -lbsd -lnetwork -lgnu
```

Changes versus the 2026-08-26 originals: the real local `cross-tools-arm64/bin` path
instead of `/opt/haiku/haiku/...`; an added `--sysroot=`; and `-L` pointing at the
private sysroot's `develop/lib` instead of `/opt/haiku/rust-haiku-sysroot/lib`.

The sysroot is assembled by copying **out of** the shared `generated.arm64` tree (never
into it): `haiku_devel.hpkg`'s `develop/{lib,headers}`, `haiku.hpkg`'s `lib`, overlaid
with the preserved `rust-haiku-sysroot`, plus libcurl/libnghttp2/libssh2 and `.so`/`.so.N`
symlinks. Every `.pc` file has to be re-prefixed away from `/packages/<pkg>/.self`, and
`libcurl.pc` has to be written by hand (Haiku's curl package ships none); a
`curl-config` shim answers `--features` from the real `libcurl.so.4`'s `NEEDED` list.

### Cargo is built standalone, not by `x.py`

`tools = ["cargo"]` would build cargo `all-static`, which compiles OpenSSL and curl from
source for the target. The shipped cargo is "dyncurl" — cargo's **default** features, so
`curl-sys`/`openssl-sys`/`libz-sys` link against Haiku's system libraries:

```sh
cargo build --release --target aarch64-unknown-haiku   # with the stage1 rustc + the wrappers
```

with `LIBSSH2_SYS_USE_PKG_CONFIG=1` in the environment. Without it,
`libssh2-sys-0.3.2/build.rs:117` compiles a vendored libssh2 at `-fvisibility=hidden`
while Haiku's `libcurl.so.4` `DT_NEEDED`s `libssh2.so.1` and asks the linker for
`libssh2_init`, and the link dies with `hidden symbol 'libssh2_init' … is referenced by
DSO` / `final link failed: bad value`. Pointing it at the system shared libssh2 (headers
+ a re-prefixed `libssh2.pc` from `libssh2_devel-1.11.1-1-arm64.hpkg`) fixes it, and
cargo links in 1m32s.

As a side effect this *restores* ssh support that the 2026-08-26 cargo silently lacked:
that binary has no `libssh2` in its `NEEDED` list and only libgit2's no-op
`git_transport_ssh_libssh2_global_init` stub, so `cargo` could never clone a `git+ssh://`
dependency. The new one `NEEDED`s `libssh2.so.1`.

### Packing

hpkg version grammar needs a `~` before a prerelease marker — `1.101.0-nightly_e9bbadf68-1`
is rejected with `Parse error in .PackageInfo(2:24) -> invalid character in micro version
string`, so the name is `1.101.0~nightly_e9bbadf68-1`, matching the old
`1.100.0~nightly_787af2b8c-2`. `tools = ["rustdoc"]` exists purely so the package's
promised `provides: cmd:rustdoc` has a binary behind it; with `tools = []` the rustc dist
component ships none, and the 2026-08-26 package has exactly that unresolvable-provides
defect (the same shape as issues 708 and 717).

## Toolchain provenance — why there is no in-tree std patch, and no place to put one

The DeBeOS Rust toolchain is **a prebuilt binary hpkg, installed on demand**:

```
pkgman install rust_bin haiku_devel
```

That one line is the whole contract (`graviton/docs/crate-to-hpkg.md:23-39`), repeated
by every consumer — `graviton/scripts/haiku-crate-to-hpkg:10-17` and its hard
precondition at `:150`, `graviton/uutils/build-uutils-hpkg.sh:23-25`,
`graviton/ports/ladybird/provision-deps.sh:6-8`,
`graviton/patches/uv-0.12.18-arm64/README.md:104-106`.

Nothing in this tree builds rustc, cargo or std:

- **No `rust`/`rust_bin` recipe exists *in this tree*.** The 174-recipe overlay in
  `graviton/haikuports-patches/recipes/` has none. There *is* an out-of-band port
  recipe, but it lives in S3 rather than in git — see *What a re-bake requires*. That
  is exactly why a std patch has no home here: the recipe that would consume it is not
  version-controlled.
- **Upstream HaikuPorts cannot supply one**: per
  `graviton/docs/archive/webpositive-arm64-plan.md:249-253`, `dev-lang/rust_bin` is
  `ARCHITECTURES="!x86_gcc2 ?x86 x86_64"` (prebuilt i686/x86_64 tarballs, no arm64)
  and `dev-lang/rust` is also arm64-disabled.
- **The jam build asserts the absence**:
  `build/jam/repositories/HaikuPortsLocal/arm64:94-98` — "there is no Rust for arm64
  Haiku".
- **`rust*` is on the mega-build denylist**:
  `graviton/scripts/haiku-build-wave-driver:85-96`, `graviton/ops/SOPs.md:112-114`.
- The commit that introduced it, `0e59ea4417`, says so in its own comment: added
  "as prebuilt package files (not build features), so this needs no in-tree recipe or
  big build."
- Tree-wide there is no `rustup`, no `-Zbuild-std`, no `rust-src`, no `x.py`, no
  `bootstrap.toml`. `graviton/ladybird/README.md:36` notes the Ladybird
  `rust-toolchain.toml` channel is simply ignored because there is **no rustup**.

A patch to `library/std/src/sys/fs/unix.rs` changes the *compiled* sysroot `libstd`
rlib that ships inside `rust_bin-…-arm64.hpkg`. None of this repo's three patch
conventions can reach it:

| Convention | Why it cannot carry a std patch |
| --- | --- |
| `graviton/uutils/patches/<crate>/*.patch` (auto-discovered, `patch -p1`, `build-uutils-hpkg.sh:101-118`) | Works only through `[patch.crates-io]`, which overrides crates.io dependencies. `std` is not one; `[patch.crates-io]` cannot override the sysroot. |
| `graviton/patches/uv-…/apply.sh` style | Hand-edits a source checkout on the box. There is no rust-lang/rust checkout and no `rust-src` component to edit. |
| `graviton/haikuports-patches/recipes/*.recipe` + `PATCHES=*.patchset` | The only from-source mechanism — but there is no `rust` recipe, and a from-source rustc needs a stage-0 bootstrap story that this project has never had. |

**So: the patch below has no home in this tree today, and deliberately has not been
given a fake one.** It is recorded here as the reference artifact. Prefer the bump.

### Reference patch (upstream `5a1dda9`, the `sys/fs/unix.rs` half)

Applies to `library/std/src/sys/fs/unix.rs`. Each hunk adds one line to one
`cfg_select!` allowlist: `lock`, `lock_shared`, `try_lock`, `try_lock_shared`,
`unlock`, in that order.

```diff
--- a/library/std/src/sys/fs/unix.rs
+++ b/library/std/src/sys/fs/unix.rs
@@ -1309,6 +1309,7 @@ impl File {
                 target_os = "cygwin",
                 target_os = "illumos",
                 target_os = "aix",
+                target_os = "haiku",
                 target_os = "android",
                 target_os = "redox",
                 target_vendor = "apple",
@@ -1332,6 +1333,7 @@ impl File {
                 target_os = "cygwin",
                 target_os = "illumos",
                 target_os = "aix",
+                target_os = "haiku",
                 target_os = "android",
                 target_os = "redox",
                 target_vendor = "apple",
@@ -1355,6 +1357,7 @@ impl File {
                 target_os = "cygwin",
                 target_os = "illumos",
                 target_os = "aix",
+                target_os = "haiku",
                 target_os = "android",
                 target_os = "redox",
                 target_vendor = "apple",
@@ -1390,6 +1393,7 @@ impl File {
                 target_os = "cygwin",
                 target_os = "illumos",
                 target_os = "aix",
+                target_os = "haiku",
                 target_os = "android",
                 target_os = "redox",
                 target_vendor = "apple",
@@ -1425,6 +1429,7 @@ impl File {
                 target_os = "cygwin",
                 target_os = "illumos",
                 target_os = "aix",
+                target_os = "haiku",
                 target_os = "android",
                 target_os = "redox",
                 target_vendor = "apple",
```

The companion `library/std/src/fs/tests.rs` half adds the same line to five test
`cfg`s and changes `if cfg!(windows)` to `if cfg!(any(windows, target_os = "haiku"))`
in `file_lock_double`. Take it from upstream `5a1dda9` rather than retyping it.

To make this patch *reachable through one of the three in-tree conventions* you would
need, in order: an arm64-enabled from-source `rust` recipe with a `PATCHES=` patchset;
a stage-0 rustc/cargo for `aarch64-unknown-haiku` and an LLVM (`llvm21-21.1.8` already
builds — `graviton/haikuports-patches/recipes/llvm21-21.1.8.recipe:671`); human sign-off
plus a non-wave build path, since `rust*` is denylisted; and the publish/pin/pool
updates listed above. That is a large piece of net-new plumbing to carry a five-line
change that upstream has already made — hence the recommendation to re-bake instead.

**And re-baking is the cheaper path for a concrete reason:** the bake has already been
done once, out of band, on 2026-08-26, and its inputs were kept (*What a re-bake
requires*). A stage-0 is not needed from scratch either — the existing `rust_bin` is
one. So the real cost is re-running a known bake at a newer base commit with a one-line
diff re-based, not inventing a from-source Rust port. What is genuinely missing is the
driving build script, which was never committed.

## Workaround until a fixed `rust_bin` ships

**Never run two `cargo` processes at once on one builder.** Either serialise them, or
give each an explicitly distinct `--target-dir` *and* a distinct `CARGO_HOME`. A
distinct target dir alone is not sufficient: cargo's package-cache lock on
`$CARGO_HOME` is equally a no-op, so two cargos with separate target dirs can still
interleave writes to `~/.cargo/registry/{src,cache}` and the registry index.

If you suspect a torn artifact, a `cargo clean` (or deleting the specific
`deps/*.rlib` / `.fingerprint` entry) is mandatory — the corrupt file is cached and
rustc will keep ICEing on it. Re-running with `--jobs 1` does **not** repair an
already-corrupt rlib; it only avoids writing a new one.

## Interim mitigation landed in this tree

Every cargo driver on a builder could overlap. All four now take an advisory lock
themselves, keyed on `$CARGO_HOME`, so they queue or abort instead of racing:

- `graviton/scripts/haiku-crate-to-hpkg` — the crate→hpkg packager. Its `cargo install`
  uses a fresh per-invocation `--root`, so two runs do not share a *target* dir, but
  they do share `$CARGO_HOME`.
- `graviton/patches/uv-0.12.18-arm64/apply.sh` — a *resume* driver that appends to a
  fixed log and builds into a fixed `~/uv/target`, with no lock and no pidfile. A
  55-90 minute detached build that looks stalled is routinely re-launched, which put
  two cargos on one target dir. This was the sharpest live exposure in the repo.
- `graviton/ladybird/scripts/run-build.sh` — `ninja WebContent WebDriver -j32` in the
  fixed `~/lb/build-wave2`. Ladybird's depth-1 `cargo` job pool makes this safe
  *within* one ninja, but a job pool binds only inside one ninja process, so a
  re-launched "stalled" build is the same hazard as `apply.sh`.
- `graviton/ladybird/scripts/run-craneliftB.sh` — the track-B driver. Its own build dir
  gives it a distinct target dir, but it shares `$CARGO_HOME` with `run-build.sh`, so
  the two must not overlap either.

All use the same `mkdir`-based lock (atomic on BFS) and take over a lock whose owning
pid is gone. The two packagers wait, honouring `CARGO_BUILD_LOCK_WAIT` (seconds,
default 7200; `0` = fail immediately); the two Ladybird drivers abort immediately
instead, since a second copy of a 1-2 hour detached build is never what was wanted.

## Exposure audit — what is *not* at risk

Recorded so the next person does not re-derive it.

- **Ladybird under `ninja -j32` is NOT exposed intra-build.** Upstream Ladybird at the
  pinned commit `90998c5d` already serialises cargo:
  `Meta/CMake/rust_crate.cmake:9` declares
  `set_property(GLOBAL PROPERTY JOB_POOLS "${JOB_POOLS};cargo=1")` — a pool of depth
  **1** — and both cargo-invoking commands carry `JOB_POOL cargo`
  (`:69` in `import_rust_crate`, `:165` in `build_rust_binary`). All library crates do
  share one `--target-dir` (`${CMAKE_BINARY_DIR}/cargo/build`, `:250-255`), but ninja
  will only ever run one of them at a time, so `-j32` does not produce concurrent
  cargos. `test_rust_crate` uses a per-crate target dir (`:189`) and `USES_TERMINAL`
  (the depth-1 `console` pool). **No DeBeOS patch to `rust_crate.cmake` is needed, and
  adding a second job pool would be redundant.**
  A job pool only binds *within* one ninja process, so two concurrent `ninja` runs in
  the *same* build tree would still race on the shared target dir. That is now
  prevented for the repo's own drivers by the `$CARGO_HOME` lock in
  `run-build.sh`/`run-craneliftB.sh` (see *Interim mitigation*); a hand-typed `ninja`
  still bypasses it, so do not do that.
- **`build-wave2` and `build-craneliftB` as parallel tracks** use different
  `CMAKE_BINARY_DIR`s and therefore different target dirs, but they share `$CARGO_HOME`.
  Run them sequentially, or give each its own `CARGO_HOME`.
- Cross-*instance* fan-out (`graviton/skills/debeos-build-wave`,
  `graviton/ops/SOPs.md:109` "at most 4 concurrent builders") is on separate
  filesystems and is not affected.
- `graviton/scripts/build-fleet/` is deprecated and runs haikuporter per QEMU guest
  with no Rust recipes.

### A caution about attributing past corruption to this bug

This bug explains **cargo-versus-cargo** races only. It does *not* explain corruption
produced by a single cargo invocation's internal `-jN` rustc parallelism, which cargo
coordinates with a jobserver and its dependency graph, not with `File::lock`.

That distinction matters because two already-documented incidents look similar but are
not obviously this bug:

- `graviton/patches/uv-0.12.18-arm64/README.md:85-101` — a dep rlib written with a
  zeroed `ODHT` `def_path_hash_map` header, reproducing under a serial re-run, filed as
  "the memoried 'parallel rustc corrupts a `.rmeta`' BFS issue". The uv build runs one
  cargo at `--jobs 4`. If the corruption came from two *overlapping* `apply.sh` runs
  (plausible — it had no lock, hence the mitigation above) then #638 explains it; if it
  came from one cargo, something else does.
- `graviton/scripts/haiku-crate-to-hpkg:38-40,191-200` — the same signature, worked
  around by an automatic serial retry.
- The Ladybird `encoding_rs` tear and the `invalid enum variant tag while decoding
  DefPathData` ICE that prompted #638 (briefly misdiagnosed as kernel memory
  corruption in #629) happened under `ninja -j32` — but, per the audit above, ninja
  does not run concurrent cargos there. So the mechanism for *that* incident is
  **not established**; a second `ninja` in the same tree, or a BFS/parallel-rustc
  write bug, are both live candidates.

The discriminating measurement is cheap once a builder is up: run the two-concurrent-
`cargo build --target-dir T` reproduction on the *current* `rust_bin` and then on a
`nightly-2026-10-13+` toolchain. The first should show two `Compiling`/`Finished`
pairs and no `Blocking waiting for file lock`; the second should block one of them.

## Cargo's own bug, for the record — not ours to fix

Cargo's `Ok(true)` on an unsupported lock is arguably the deeper defect: it converts a
missing safety mechanism into a *silent* one, so a platform gap becomes
indistinguishable from a successfully held lock and surfaces only as corrupted output
much later. A warning on that path, even once per process, would have turned #638 into
a five-minute diagnosis instead of a trip through #629's "kernel memory corruption".

There is precedent for cargo working around a std gap directly rather than waiting:
`src/util/flock.rs` carries a whole `#[cfg(target_os = "solaris")] mod imp` that
implements the five operations with `fcntl(F_SETLK/F_SETLKW)` because std lacks flock
on Solaris. A Haiku arm would have fit the same shape.

This is upstream cargo's call, not DeBeOS's. Noted, not pursued — std's fix has landed
and is the right layer.

## VERIFIED vs OWED

**VERIFIED here (static, no instance needed):**

- Haiku's kernel `flock()` is fully implemented, with BSD per-open-file semantics,
  real `LOCK_SH`, and `EWOULDBLOCK` on `LOCK_NB` conflict — read from `vfs.cpp` and
  the libroot wrapper in this tree, cited above.
- std through release `1.99.0`, and the shipped `1.100.0-nightly (787af2b8c)`, omit
  `target_os = "haiku"` from all five flock `cfg_select!` lists.
- Upstream rust-lang/rust#164031 adds exactly those five lines; merged 2026-10-12;
  present in `nightly-2026-10-13` and absent from `nightly-2026-10-12`, checked by
  reading `library/std/src/sys/fs/unix.rs` at the exact rustc commit each nightly
  manifest names.
- Cargo's silent `Ok(true)` on unsupported, at `src/util/flock.rs`.
- Upstream Ladybird `90998c5d` already serialises cargo with a depth-1 `cargo` job
  pool, so the `ninja -j32` intra-build exposure does not exist.
- **(2026-10-15) There is no prebuilt toolchain to bump to.** The `nightly-2026-10-13`
  channel manifest names zero Haiku targets, and upstream `rustc_target`'s
  `mod.rs:1415-1416` registers only i686/x86_64 Haiku — `aarch64-unknown-haiku` is
  DeBeOS's own target. Fetched/read directly, not inferred.
- **(2026-10-15) The 2026-08-26 bake's inputs survive** in
  `s3://haiku-graviton-668984504585-us-west-2/rust/` (target spec, registration diff,
  cross-gcc wrappers, link sysroot, base commit `787af2b8c`, plus the resulting dist
  tarballs and hpkgs). Listed and the recipe tarball unpacked and read.
- **(2026-10-15) The bake's driving build script does not survive** — not in
  `rust/`, not in `bake-scripts/`, not in the tree. **Superseded 2026-10-16:**
  reconstructed and committed as `graviton/scripts/haiku-rust-bake`.
- **(2026-10-16) The re-bake works.** `rust_bin-1.101.0~nightly_e9bbadf68-1-arm64.hpkg`
  (183,503,957 bytes) built from base commit `e9bbadf68` on an aarch64 Linux desk:
  stage1 std, `x.py dist rustc rust-std`, a standalone dyncurl cargo, and the hpkg.
  Inputs and outputs under
  `s3://haiku-graviton-668984504585-us-west-2/rust/rebake-2026-10-16/`.
- **(2026-10-16) The DeBeOS port needs no changes at the new base commit.** The
  preserved `aarch64_unknown_haiku.rs` compiles verbatim against `e9bbadf68`; only the
  one-line `mod.rs` registration had to be re-based (anchor on the
  `("i686-unknown-haiku", …)` line). No target-spec and no sysroot change was needed.
- **(2026-10-16) `host_tools: false` in the target spec does not block building host
  tools.** The contradiction in the preserved recipe (a `host_tools: false` target that
  nonetheless shipped a cargo binary) is only apparent: `host_tools` is consumed by
  `src/tools/build-manifest/build.rs` for the manifest and the platform-support table,
  and `grep -rn host_tools src/bootstrap/src/` finds nothing — it is metadata, not a
  bootstrap gate. The 2026-08-26 cargo's `strings` show `/home/cargo-rebuild/vendor/<crate>`
  paths, i.e. it was built standalone with vendored deps, not by `x.py`.
- **(2026-10-16) std-level A/B on the artifacts.** Old `libstd-*.rlib`: zero `flock`
  references and four `"lock() not supported"` strings. New `libstd-*.rlib`: one `flock`
  reference, zero such strings. A probe binary cross-compiled against the new std shows
  `U flock` in `nm -D` — the dynamic reference to Haiku's `flock(2)` that the old std
  never emitted.
- **(2026-10-16) The *before* half of the reproduction, on the real builder**
  (`i-01886408618b237de`, rustc `1.100.0-nightly (787af2b8c)`, cargo `1.100.0`):

  ```
  lock() -> Err kind=Unsupported msg="lock() not supported"  waited=0ms   (holder)
  lock() -> Err kind=Unsupported msg="lock() not supported"  waited=0ms   (contender)
  c1: Finished `release` profile [optimized] target(s) in 30.57s   rc1=0
  c2: Finished `release` profile [optimized] target(s) in 30.30s   rc2=0
  BLOCKING_COUNT=0
  FINISHED_COUNT=2
  ```

  Two cargos sharing `--target-dir T` on a crate whose `build.rs` sleeps 30 s both ran
  the build concurrently and both reported success. Neither printed `Blocking waiting
  for file lock`. That is the defect, measured.

**OWED:**

- **The *after* half of the reproduction on a native builder.** Not run: the only Haiku
  node with `rust_bin` installed (`i-01886408618b237de`) has a wedged SSM command worker
  — every command queued on it since 02:45 UTC on 2026-10-16, including other agents'
  trivial ones, is stuck `InProgress` while the agent still heartbeats `Online`; and it
  must not be rebooted (SSM does not survive a reboot there and sshd is ed25519-only).
  The alternative node `i-05ed98f7af2c4ba14` is responsive and has every shared library
  the new cargo needs, but no `rust_bin`, and staging the toolchain onto it was blocked
  by a tooling permission denial. The expected result, for whoever runs it: the probe's
  contender blocks for the holder's hold time instead of returning `Unsupported`, and
  `BLOCKING_COUNT=1` / `FINISHED_COUNT=2` for the two-cargo run. The scripts are written
  and need only a node to run on.
- **Publishing and the pin bump.** The hpkg is built but not published; publishing to
  the green pool is a gated human step. The pin at `graviton/ssh/UserBuildConfig:317`
  is deliberately **left at `rust_bin-1.100.0~nightly_787af2b8c-2-arm64.hpkg`** — it
  names a file that `HG_POOL_DIR` must actually contain, so bumping it before the
  package is in the pool would break every image build. Bump it to
  `rust_bin-1.101.0~nightly_e9bbadf68-1-arm64.hpkg` in the same change that publishes.
- Smoke-testing the four `mkdir`-lock mitigations on a real builder. The guard's
  three paths (acquire/release, abort while held, stale-pid takeover) were exercised
  on a Linux desk against the block as shipped, but `mkdir` atomicity *on BFS* and
  `kill -0` against a Haiku pid are the parts that only a builder can confirm.
- Whether a Haiku `cargo` build of anything non-trivial still hits the separate
  parallel-rustc/BFS corruption once cargo-versus-cargo races are excluded.
- A runtime check of the fixed `release_advisory_lock` split path — that an `fcntl`
  middle-range unlock now leaves a head `[S, l_start - 1]` *and* a tail
  `[l_start + l_len, E]`. The fix is build-verified only; it is not on any `flock()`
  path, so it cannot be exercised by the cargo reproduction above.
