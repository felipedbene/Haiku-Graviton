# Port hygiene: catching arm64 assumptions before the build fails

Issue: #341. A **mechanical, up-front** check for the small set of architecture
assumptions that are invisible on x86/PPC and only surface as an arm64 build failure —
or, worse, as a *silent* miscompile or a stub. It complements the reactive
[porting-playbook.md](porting-playbook.md) (symptom → class → fix, keyed on a build-log
line) by finding the same defects *before* the build, from the source tree and the
recipe.

Two pieces:

1. **`graviton/scripts/haiku-port-lint`** — a standalone, dependency-free (Python
   stdlib) static linter. Scans a source tree and/or a recipe and reports findings
   ranked ERROR / WARN / INFO. Exit status is non-zero when any ERROR or WARN is present,
   so it can gate a CI step or a pre-bake check.
2. **The checklist** below (also printed by `haiku-port-lint --checklist`) — the manual
   companion for the judgement the linter cannot make.

The `debeos-arm64-port` skill references this document and `haiku-port-lint` by name;
this file and the script are standalone and carry no dependency on that skill.

## What it catches (and why each matters on Graviton)

| Check | Signature | Why arm64 breaks | Fix |
|---|---|---|---|
| **char signedness** | bare `char` compared `< 0` / `>= 0`; getchar()/getc() stored in a `char` then tested against `EOF` | arm64 `char` is **unsigned** by default (x86/PPC signed). `c < 0` is always false; the `EOF` (-1) test never fires → infinite loop / truncated read. **Silent.** | build with `-fsigned-char` (add `-O2`, playbook Class 15), or fix the type |
| **x86 preprocessor arm** | `#if(def) __i386__ / __x86_64__ / __SSE*__ / __AVX*__ / __MMX__` with no arm64/NEON arm in the same file | arm64 falls through to a scalar stub, or a symbol is left undefined | add an `__aarch64__` / `__ARM_NEON` arm, or a SIMDe path |
| **x86 SIMD intrinsics** | `<xmmintrin.h>`/`<immintrin.h>`/…; `_mm_*`, `_mm256_*`, `_mm512_*` | these do not exist on arm64 | build-depend on **SIMDe** (`devel:simde`) and include `<simde/x86/…>`; or hand-write NEON — do **not** drop the feature |
| **x86-only compiler flags** | `-msse2`, `-mavx`, `-mfpmath=sse`, `-mmmx`, … in a recipe body or build file | aarch64 gcc: `unrecognized command-line option` | gate on x86/x86_64 (playbook Class 5) |
| **IFUNC / target_clones** | `--enable-ifunc`, `HAVE_IFUNC`, `__attribute__((ifunc))`, `target_clones`, `__builtin_cpu_supports` | ifunc *auto*-dispatch is unimplemented in the userland runtime_loader — an `R_AARCH64_IRELATIVE` reloc falls through, so the `.so` **fails to load silently** (diagnostic compiled out). **Explicit** runtime dispatch does work: `getauxval(AT_HWCAP/AT_HWCAP2)` is implemented (commpage-backed, NEON caps advertised as of #329). | disable ifunc; select the code path explicitly at runtime via `getauxval`, or decide SIMD at compile time |
| **CPU-string / config.guess** | CMake `CMAKE_SYSTEM_PROCESSOR` matching `x86\|aarch64\|arm`; `uname -m` arch switches | Haiku reports `arm64` (not `aarch64`), matching no branch → NEON silently disabled (the libjpeg-turbo defect); GNU config.guess/config.sub choke | `-DCMAKE_SYSTEM_PROCESSOR=aarch64`; add an arm64 case (playbook Class 6/19) |
| **`uname -p`** | `$(shell uname -p)` / `` `uname -p` `` feeding an arch switch (**not** autoconf's `## Platform. ##` log banner, which only echoes it) | `uname -p` names **no architecture at all** on Haiku. There is no processor field in `struct utsname`; the string is the coreutils port's own, and it is `other` with the image's bootstrap coreutils. The pool's non-bootstrap coreutils answers `arm64`, so a builder provisioned by `haiku-provision-native-builder` is correct and a bare image is not — which makes any branch keyed on `-p` **environment-dependent**, not reliably wrong. **Silent.** | re-key on `uname -m` (`arm64`), which is libroot's and always `arm64`, or state the answer explicitly — see issue 701 |
| **tecmake word size / byte order** | a vendored `tecmake.mak`; `TEC_SYSARCH`, `TEC_BYTEORDER`, `TEC_WORDSIZE` (ftgl, cd/canvasdraw, iup, im) | tecmake's arch table knows only `i686`/`i386`/`powerpc`/`x86_64`/`amd64`/`armv7l`, and both the byte-order and the word-size test fall through to **`-DTEC_BIGENDIAN -DTEC_32`** on little-endian LP64. Measured identical for `other`, `unknown`, `arm64` **and** `aarch64` — so an `uname -p` fix does **not** reach it. Not just compile defines: `tecmake.mak:503-530` reads both to pick the `be32`/`le64` object directory and the `_be32`/`_le64` filename suffixes, so a build and whatever later looks for its output can disagree. **Silent.** | patch `tecmake.mak` itself — insert an `ifeq ($(TEC_SYSARCH), arm64)` / `aarch64` pair setting both macros **before** the first `STDDEFS :=` (that line is `:=`, immediately expanded, so an append at end of file is read too late); see `ftgl-2.1.5.recipe`. `make TEC_WORDSIZE=TEC_64 TEC_BYTEORDER=TEC_LITTLEENDIAN` also works — tecmake assigns both with a plain `=`, which a command-line variable overrides and an exported environment variable does not — but has to be repeated, correctly, in every tecmake recipe forever, and leaves tecmake still *believing* the target is big-endian 32-bit everywhere else it asks |

The evidence behind these lives in [simd-vectorization-review.md](simd-vectorization-review.md)
(the IFUNC hazard §2.1, the `#ifdef __i386__` app_server gap §5.1, the libjpeg-turbo
`CPU_TYPE=other` find §6.2, and AWS's `-fsigned-char` note §1.4) and the
[porting-playbook.md](porting-playbook.md) (Class 5 x86 flags, Class 6/19 arch naming).

The last two rows are #701, and the reason they are *two* rows is that fixing the first
does not fix the second. Where `uname -p`'s answer comes from, measured rather than
assumed: `src/system/libroot/posix/sys/uname.c:88` already maps `B_CPU_ARM_64` to `arm64`
and `src/system/kernel/arch/arm64/arch_system_info.cpp:142` already reports that platform,
so `uname -m` is correct and **nothing in this tree is broken**. `-p` is not ours: POSIX has
no processor field, and the string comes from a HaikuPorts patch to coreutils' own
`src/uname.c` whose switch covered only `B_CPU_x86`/`B_CPU_x86_64` and answered `other` for
everything else. The image's `coreutils-9.9_bootstrap-1-arm64.hpkg` carries exactly that
(its `bin/uname` imports `get_cpu_topology_info` and contains the literals `other` and
`x86_64` but not `arm64`).

What does **not** hold, and what this file asserted until it was run: that the pool build
is equally broken. Measured on the live Graviton builder by extracting each hpkg and
running its own `bin/uname`, and by configuring one `project(p701 NONE)` three times with
each `uname` first on `PATH`:

| `uname` | `-p` | `-m` | `CMAKE_SYSTEM_PROCESSOR` |
|---|---|---|---|
| image `coreutils-9.9_bootstrap-1` | `other` | `arm64` | `other` |
| pool `coreutils-9.11-2` | `arm64` | `arm64` | `arm64` |
| native-built `coreutils-9.12-1` | `arm64` | `arm64` | `arm64` |

So the published pool has answered `arm64` all along and **no coreutils rebuild is
needed**; the defect was that `haiku-provision-native-builder`'s `TOOLS` set never
installed `coreutils` over the bootstrap package, so every builder kept the bootstrap
`uname`. Adding it there is what retires the first row. The consequence is worth spelling
out because it is CMake's and not tecmake's: `CMakeDetermineSystem.cmake` reaches
`uname -m` only through a `Linux|CYGWIN|MSYS|GNU|Android` branch, a Darwin branch and an
OpenBSD branch, so Haiku lands in the final `else()`, which runs `uname -p` and retries with
`uname -m` **only if `-p` exits non-zero** — and it exits zero while printing `other`. That is
how `CMAKE_SYSTEM_PROCESSOR` becomes `other` for every cmake port here (see
[codec-tier-arm64.md](../haikuports-patches/codec-tier-arm64.md)).

## Usage

```sh
# scan a source tree (default: current dir)
graviton/scripts/haiku-port-lint path/to/port-src

# scan a source tree AND audit the recipe together
graviton/scripts/haiku-port-lint --recipe media-libs/foo/foo-1.2.recipe path/to/foo-src

# machine-readable
graviton/scripts/haiku-port-lint --format json path/to/port-src

# print the manual checklist
graviton/scripts/haiku-port-lint --checklist

# only show the certain breakage (raise the floor)
graviton/scripts/haiku-port-lint --severity WARN path/to/port-src
```

It is a **heuristic** linter, not a compiler: INFO items in particular need a human look,
and a clean run is not a promise the port builds. A dirty run is a prioritised list of
the arm64 assumptions to check first.

## When SSE/AVX has no NEON equivalent: SIMDe

Where a port's hot path is x86 SSE/AVX intrinsics with no hand-written NEON, the linter
points at **SIMDe** — the header-only SSE/AVX→NEON translation library, packaged for
DeBeOS arm64 as `graviton/haikuports-patches/recipes/simde-0.8.2.recipe` (provides
`devel:simde`). Build-depend on it and replace the x86 intrinsic header with
`<simde/x86/…>`; SIMDe maps to NEON where it can and falls back to portable scalar
otherwise. It adds no runtime dependency (headers only).

## Checklist

Run `haiku-port-lint --checklist` for the current text; the checks are the eight rows in
the table above.
