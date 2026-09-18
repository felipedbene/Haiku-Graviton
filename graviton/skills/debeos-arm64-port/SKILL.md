---
name: debeos-arm64-port
description: Port a package to DeBeOS on Haiku/arm64 (AWS Graviton). Use when adding or fixing a HaikuPorts overlay recipe, triaging a native-build failure, or auditing source for x86-only assumptions before an arm64 build. Captures the recurring arm64 gotchas (uname -m=arm64, CMake 4.x policy floor, -fsigned-char, x86 ifdef audit, HWCAP runtime dispatch, the Graviton3/4 ISA opt-in) so a port is done consistently. Drives the existing playbook and tooling under graviton/ — it does not replace them.
---

# DeBeOS arm64 porting runbook

A checklist for porting a package to DeBeOS (Haiku, arm64, AWS Graviton). The
**authority is `graviton/docs/porting-playbook.md`** — 24 documented failure
classes with the exact fix and an example port each. This skill is the entry
sequence and the per-port hygiene checklist; when a build fails, grep the
playbook for the log line and jump to the class. Do not re-derive fixes the
playbook already records.

Cross-references (GitHub): #71 (`uname -m`), #47 (CMake policy floor),
#329 (HWCAP auxv), #330 (per-generation ISA opt-in), #341 (recipe hygiene:
`-fsigned-char`, `__x86_64__` audit, SIMDe), #90/#136 (playbook + waves).

## Ground rules (read once)

- **These are DeBeOS overlay recipes, not upstream submissions.** Nothing here
  goes back to HaikuPorts (project policy). Frame every fix as a
  portability/build-system correction for our tree.
- **Success is a harvested, non-empty `.hpkg` — never an exit code.** haikuporter
  can exit 0 and ship an ~800 B package with no binary (playbook Class 9). After a
  build, assert the `.hpkg` size is non-trivial **and** unpack/`listAttr` shows the
  expected `bin/`…. See "Verify" below.
- **Never ship a feature-capped build** (a backend/loader/subpackage disabled just
  to go green). Fix the real blocker. Feature cuts are throwaway diagnostics only
  (`graviton/haikuports-patches/README.md`).
- **No number, no claim.** Any performance or correctness claim needs a measurement
  you took — throughput, `objdump` of the shipped `.so`, a run on the target
  hardware. A green exit code is not a measurement.

## Environment

- `export AWS_PROFILE=haiku-graviton`; region us-west-2.
- Trunk is `graviton` — **never commit there.** Work in your own topic branch off
  `origin/graviton`; land via PR.
- Overlay recipes live in `graviton/haikuports-patches/recipes/` as **complete
  recipe files** (not diffs). Sibling `*.patch`/`*.patchset` files are explanatory
  notes or a real patch series — read the patch for *why*, apply the recipe.
- Builds run **native**: `haikuporter` on a Graviton Haiku EC2 instance driven
  over AWS SSM. See `graviton/docs/native-ec2-builds.md`. There is **no arm64 repo
  upstream** (HaikuPorts ships only riscv64/x86_64/x86_gcc2), so building it
  ourselves is required — there is nothing to pull.

---

## Step 0 — source audit BEFORE you build (arm64 hygiene, #341)

Cheaper to grep than to boot a builder. Run these over the unpacked source first:

1. **`uname -m` returns `arm64`, not `aarch64` (#71).** Haiku's `uname -m` reports
   **`arm64`**; GNU `config.guess`/`config.sub` and many hand-rolled arch switches
   expect **`aarch64`**. Grep for `uname -m`, `config.guess`, `UNAME_MACHINE`,
   `$(ARCH)` switches.
   - Autotools that runs both `config.guess` **and** `config.sub`: route through
     `runConfigure` (it supplies the host triple; `config.sub` canonicalises
     `arm64-unknown-haiku`→`aarch64-unknown-haiku` here). Playbook **Class 6**.
   - LLVM-style consumer reading `config.guess` **raw**: replace the vendored
     script, arm64-scoped — `printf '#!/bin/sh\necho aarch64-unknown-haiku\n' >
     <path>/config.guess; chmod +x`. `-DLLVM_HOST_TRIPLE=` does **not** help.
     Playbook Class 6 (llvm21, keystone, haiku_format).
   - Project's own arch switch with no `aarch64`/`arm64` case (`"Platform ''
     not supported"`, `unknown word-size for arch: aarch64`): patch in an
     `arm64`/`aarch64` branch mapped to the 64-bit LE path. Playbook **Class 19**.

2. **`char` is unsigned on arm64 (`-fsigned-char`).** On x86 `char` is signed; on
   aarch64 it is **unsigned** by default. Code that stores `-1` in a `char` sentinel,
   does `if (c == -1)` on a `getchar()`-into-`char`, or `(char)` down-casts and tests
   `< 0`, silently breaks. Symptom: wrong parse/EOF/hash behaviour, not a compile
   error. Grep the source for `char` compared to a negative literal or `EOF`. Fix:
   add `-fsigned-char` to the port's `CFLAGS`/`CXXFLAGS` (keep `-O2`, see Class 15),
   gated to arm64, restoring x86 semantics for code that assumed them. #341.

3. **`__x86_64__` / `__SSE__` / `__i386__` ifdef audit.** Grep:
   `grep -rnE '__x86_64__|__i386__|__SSE|__AVX|__MMX__|_M_X64|<[a-z]*mmintrin\.h>|-msse|-mavx|-mmmx|-mfpmath' <src>`
   - A hardcoded `-msse2`/`-msse3`/`-mfpmath=sse`/`-mmmx` on the compile line: gate
     it to x86 (playbook **Class 5**), *provided there is a NEON or scalar fallback
     path*. If SSE is the **only** backend (e.g. embree ≤3.12), gating just yields a
     library with no working kernel — that is a version-bump/real port, **not** a
     one-liner (playbook deferred-hard).
   - An `#ifdef __SSE__ … #else …` with a portable `#else`: usually builds as-is.
   - SSE intrinsics with no fallback: **SIMDe** (SIMD-Everywhere, a header-only
     SSE→NEON translation layer) is the sanctioned route to keep the vectorised path
     on arm64 rather than cutting the feature. Tracked in #341; prefer it over
     disabling SIMD.

4. **LTO / `-flto` / `-fuse-linker-plugin`.** Our gcc has no LTO linker plugin;
   these fail (`LTO support has not been enabled`). Drop them —
   `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF`, or sed `-flto`/`-fuse-linker-plugin`
   out. Playbook **Class 7** (note the unarr hardening: a cache-var OFF can be
   overridden by a per-target IPO property).

---

## Step 1 — write / fix the recipe (the common classes)

Grep the failing build's exact log line against
`graviton/docs/porting-playbook.md` and apply the matching class. The high-frequency
ones:

- **CMake 4.x drops <3.5 policy compat (#47, Class 1 — the largest class).**
  Symptom: `Compatibility with CMake < 3.5 has been removed`. Fix: add
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` to the `cmake` line. It only raises the
  floor; a duplicate `-D` is harmless.
  - The systemic fix appends the flag **once** to haikuporter's shared
    `cmakeDirArgs`, so any recipe interpolating `$cmakeDirArgs` gets it free.
  - It does **NOT** reach a `cmake` call with its own explicit arg list that never
    interpolates `$cmakeDirArgs`, **nor a nested/bundled configure**. The llama.cpp
    case is the canonical trap: its bundled **mimalloc** is configured by a separate
    `cmake -S$sourceDir2` that does not use `$cmakeDirArgs`, so the floor had to be
    spelled on that call explicitly (see
    `graviton/haikuports-patches/recipes/llama_cpp-b4889.recipe`). Thread the flag
    to **every** `cmake` invocation the recipe issues, including sub-builds.
  - An env var cannot carry it — haikuporter's `filteredEnvironment()` strips all
    but `PATH`/`LIBRARY_PATH`/`LC_ALL`/`TERM`.
- **Missing CMAKE_BUILD_TYPE (Class 8).** haikuporter's `cmake` wrapper aborts
  without a build type. Add `-DCMAKE_BUILD_TYPE=Release` (systemic wrapper fix
  preferred; per-recipe as fallback).
- **Undeclared build tool in the chroot (Class 2).** `<tool>: command not found` /
  `Error 127`. haikuporter mounts only declared `BUILD_PREREQUIRES` into the build
  chroot. Universal utilities (`gzip`/`tar`/`unzip`/`which`) are seeded systemically;
  feature-specific tools (`cmd:msgfmt`, `cmd:yacc`, `cmd:perl`) stay per-recipe.
  Note: `PATCH()` and source fetch/unpack run on the **HOST** before the chroot
  exists, so a tool used there must be host-provisioned, not a `cmd:` prereq
  (Classes 2/24).
- **gettext autoreconf / autopoint (Class 3), static/shared lib install (Class 4),
  autotools aux files — `autoreconf -fi` (Class 10), install target references
  optional/x86-named outputs (Class 11), source/checksum drift (Class 12 — obey THE
  INTEGRITY RULE: fetch, sha256, extract, verify in-tree version before bumping),
  hardcoded cross-prefix `aarch64-unknown-haiku-g++` (Class 13), missing link lib
  `-l<x>` (Class 18), dep not published (Class 20), read-only `/packages` harvest
  (Class 23).** See the playbook for each — do not duplicate here.
- **CFLAGS override needs `-O` (Class 15).** haikuporter's `runConfigure` aborts:
  `Must specify optimization flags when overriding CFLAGS`. Any `CFLAGS=` passed to
  `runConfigure` must include `-O2`. This bit earlier fix waves — if you add
  `-fsigned-char`/`-Wno-error`/`-D_BSD_SOURCE`, write `CFLAGS="-O2 -fsigned-char …"`.
  (CMake ports that `export CFLAGS` are unaffected; the guard is on `runConfigure`.)

### Recipe placement & the mtime trap (do not lose your edit)

- A port with an **input-source-package (ISP)** reads the recipe from
  `input-source-packages/develop/sources/<port>-<ver>-<rev>/<port>-<ver>.recipe`;
  editing the ports-tree copy has **no effect and no warning**. Check the
  `<source-package>::` line in the haikuporter log.
- **mtime trap:** haikuporter silently re-extracts (reverting your edit) whenever
  `mtime(recipe) <= mtime(sourcePackage)`. Pin forward:
  `touch -d @$(( $(stat -c %Y "$srcpkg") + 172800 )) "$recipe"`.
- Use `haikuporter -G` (`--no-git-repo`) for every downloaded-tarball (non-ISP) port.
- **Harvest immediately.** Builders are scratch VMs. The moment an edit builds,
  copy it back into `graviton/haikuports-patches/recipes/` and commit — recipes have
  been lost to reprovisioned builders.

---

## Step 2 — Graviton3/4 ISA opt-in (per-recipe, NOT the baseline) (#330)

The system `-mcpu` baseline stays **`neoverse-n1+crypto` / armv8.2-a** so the OS and
base packages stay bootable on Graviton2 and t4g. **Do not raise the system
baseline.** Only an ML/codec port that explicitly opts in gets the newer ISA, and
its binary faults on Graviton2/t4g — so it is a distinct variant.

- Neoverse-V1 (Graviton3) / V2 (Graviton4) are ARMv8.4-A supersets adding **BF16**
  (`BFMMLA`/`BFDOT`), **I8MM** (`SMMLA`/`UMMLA`/`USMMLA`), and **SVE**(V1)/**SVE2**(V2).
- **SVE is supported here now** (kernel commit enabling EL0 SVE, #88): a userland ML
  port **may** emit SVE — do not suppress it. The kernel runs NEON-only at EL1; this
  does not touch the N1 baseline.
- The idiom — gated on arm64, **before** the upstream build reads flags:
  ```sh
  case "$targetArchitecture" in
      arm64)
          debeosMcpu="-mcpu=neoverse-v1+crypto"   # neoverse-v2 for a _g4 variant
          export CFLAGS="$CFLAGS -O2 $debeosMcpu"
          export CXXFLAGS="$CXXFLAGS -O2 $debeosMcpu"
          ;;
  esac
  ```
  - A second `-mcpu` **fully replaces** the baseline (last one wins), and `crypto`
    is never a compiler default — **always re-spell `+crypto`** or the port silently
    loses AES/SHA/PMULL.
  - Keep `-O2` (Class 15).
- **Name the variant `_g3` (V1) / `_g4` (V2)** in the package name/revision; never
  promote it into the default green pool as the plain package. Install/publish only
  where the target is known Graviton3+.
- **Portable middle tier:** if a port must still run fleet-wide, use
  `-mtune=neoverse-v1` only (scheduling, no ISA change) — stays a normal package, no
  suffix. Payoff on non-vector code is small; measure before shipping.
- **Verification owed before publishing a `_g3`/`_g4` build:** `objdump -d` the
  shipped `.so` and confirm the intended extension is present (`smmla`/`ummla` =
  I8MM, `bfmmla`/`bfdot` = BF16, `ptrue`/`whilelo`/`z<n>.` = SVE), **plus** a runtime
  smoke test on Graviton3/4 hardware. "Success is a disassembled `.so` (and a run),
  not an exit code."

---

## Step 3 — runtime feature dispatch via HWCAP (#329)

For a port that ships **one** fleet-portable binary but wants to use a newer ISA
extension where the CPU has it (rather than building a separate `_g3`/`_g4`), select
the code path **at runtime** from the auxiliary vector. DeBeOS arm64 now serves
`getauxval(AT_HWCAP)` / `getauxval(AT_HWCAP2)` from a kernel-published commpage word,
with the Linux/glibc bit layout, so software keying off HWCAP works unchanged (#329;
see `headers/posix/sys/auxv.h`, `src/tests/system/libroot/os/getauxval_test.cpp`).

- Include `<sys/auxv.h>`; test the bit before entering the specialised kernel:
  ```c
  #include <sys/auxv.h>
  unsigned long hw  = getauxval(AT_HWCAP);
  unsigned long hw2 = getauxval(AT_HWCAP2);
  if (hw2 & HWCAP2_I8MM)  { /* SMMLA/UMMLA integer-matmul path */ }
  else if (hw & HWCAP_ASIMDDP) { /* NEON dotprod (asimddp) path */ }
  else { /* baseline NEON path */ }
  ```
  Useful bits: `HWCAP_ASIMDDP` (dotprod), `HWCAP2_I8MM`, `HWCAP2_BF16`, `HWCAP_SVE`,
  `HWCAP2_SVE2`, plus `HWCAP_AES`/`HWCAP_SHA2`/`HWCAP_PMULL` (crypto),
  `HWCAP_ATOMICS` (LSE).
- **Caveat — Haiku has no glibc IFUNC resolver.** Do the dispatch with an explicit
  runtime branch or a function pointer set once at init; do not rely on
  `__attribute__((ifunc))` (not supported on Haiku arm64). Many upstream libraries'
  ifunc dispatch must be replaced with a plain `getauxval`-gated branch when ported.
- This keeps the package fleet-portable (no `_g3`/`_g4` suffix): the baseline path
  runs on Graviton2/t4g, the accelerated path lights up on G3/G4 at run time.

---

## Step 4 — build, verify, harvest, publish

- **Build** on a native Graviton builder over SSM (`graviton/docs/native-ec2-builds.md`;
  driver `graviton/scripts/haiku-nativebuild <port>…`). Cap parallelism at **`-j16`**
  — high `-j` trips an ECHILD child-reaping race on Haiku (`ninja: fatal:
  waitpid(...): No child process`) with zero compile errors; don't misread it as a
  build defect (builder hygiene in the playbook).
- **Verify (the Class-9 guard, mandatory):** never trust the exit code. Assert the
  harvested `.hpkg` is non-trivial in size **and** unpack/`listAttr` shows the
  expected `bin/`…. For a `_g3`/`_g4` variant also run the Step-2 `objdump` +
  hardware smoke test.
- **`built` ≠ `published`.** `build_state=built` means a non-empty `.hpkg` was
  harvested to the S3 pool. **Published** means it is in the **green** pool
  (`debeos-repo-green/` prefix, served over the package CDN) with a refreshed index,
  so `pkgman` on a booted instance can install it. A consumer port stays
  `UNRESOLVABLE` until its dep is *published*, not merely *built*. Publish with the
  green-pool tooling (`graviton/scripts/haiku-repo-publish*`; see the
  `debeos-publish-green` skill and `graviton/ops/SOPs.md`).

---

## Step 5 — record it

Once a new port builds, add a row to the matching class table in
`graviton/docs/porting-playbook.md` (or open a new class via the TARGET-SPEC
template at the end of that doc if it is genuinely new — the miner
`graviton/scripts/haiku-pattern-miner` + `haiku-playbook-parity` gate the class
set). This document is the memory that keeps per-port flags from being re-derived.

## Anti-patterns (do not do these)

- Raising the **system** `-mcpu` baseline to get G3/G4 ISA (breaks G2/t4g boot) —
  opt in per-recipe with a `_g3`/`_g4` suffix instead (#330).
- Dropping an `-l<lib>`, a SIMD backend, or a subpackage to make a build go green
  (feature cut — forbidden; use SIMDe / declare the devel dep / publish the dep).
- Bumping a `CHECKSUM_SHA256` on trust without extracting and verifying the in-tree
  version (Class 12 integrity rule).
- Editing the ports-tree recipe when an ISP exists, or editing without pinning the
  mtime forward (silent revert).
- Trusting `RC=0` as success, or reporting a speedup without a measurement.
