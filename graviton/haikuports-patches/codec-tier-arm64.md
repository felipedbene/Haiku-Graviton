# The video codec tier on arm64, and the SIMD trap they all share

Five recipes, edited to build on Haiku/arm64 **and** to actually contain their
hand-written NEON. The edited recipes are in `recipes/`; this file is the *why*.

Every claim below was checked by disassembling the shipped `.so` inside the built
hpkg, never by the build's exit status. That distinction earned its keep here: the
first libjpeg-turbo fix returned `RC=0`, produced a fresh package, and changed
nothing at all.

## The trap: `uname -p` is `other`, so CMake arch detection fails silently

On Haiku/arm64 `uname -p` returns the literal string `other`, so
`CMAKE_SYSTEM_PROCESSOR` is `other` — measured directly:

```
-- PROC=[other] SYSNAME=[Haiku] SIZEOF_VOID_P=[8]
```

Any CMake project that selects its SIMD or assembly from
`CMAKE_SYSTEM_PROCESSOR` therefore matches none of its own patterns and quietly
takes the "unknown CPU" branch. This is not specific to one port; it is a
property of the platform, and it silently costs SIMD in *every* cmake-built
package that gates on the processor name. libjpeg-turbo, x265 and svt-av1 were
all affected. Anything else cmake-based that ships SIMD should be re-checked
against this — `libwebp`, `libavif` and `openjpeg` are the obvious candidates.

### Overriding it takes two flags, not one

`-DCMAKE_SYSTEM_PROCESSOR=aarch64` **on its own does nothing**. CMake's platform
detection during `project()` sets the *normal* variable, which shadows the
*cache* entry a `-D` creates. Measured, same guest, three variants:

| invocation | resulting `CMAKE_SYSTEM_PROCESSOR` |
|---|---|
| `-DCMAKE_SYSTEM_PROCESSOR=aarch64` | `other` — inert |
| `-DCMAKE_SYSTEM_NAME=Haiku -DCMAKE_SYSTEM_PROCESSOR=aarch64` | `aarch64` |
| `-DCMAKE_TOOLCHAIN_FILE=...` setting both | `aarch64` |

Naming the system is what makes the override bind. It also sets
`CMAKE_CROSSCOMPILING=TRUE`, which disables `try_run`. That was checked rather
than assumed: libjpeg-turbo contains exactly one `check_c_source_runs`, and it is
already guarded by `if(CMAKE_CROSSCOMPILING)` with the fallback
`RIGHT_SHIFT_IS_UNSIGNED 0` — the correct answer for aarch64. So nothing is lost.

### And a stale build directory silently defeats both

Even with both flags the fix appeared not to work. Cause: `cmake -S. -Bbuild`
against an existing `build/` reloads
`build/CMakeFiles/<ver>/CMakeSystem.cmake`, which re-sets
`CMAKE_SYSTEM_PROCESSOR` and overrides the command line. The tell is the absence
of `-- The C compiler identification is ...` from the log, which only appears on
a fresh configure. Hence the `rm -rf build` in the affected recipes. Without it
the build succeeds, the package is rebuilt, and the flag is ignored.

## Per-port

### `libjpeg_turbo-3.1.4.1` — SIMD was compiled out entirely

`simd/CMakeLists.txt` reached `simd_fail("SIMD extensions not available for this
CPU (other)")`, which sets `WITH_SIMD 0`, **overriding its own default of TRUE**.

Verified by symbols in the shipped `libjpeg.so.62.4.0`:

| | `jsimd_*_neon` symbols | any `jsimd` | NEON insns | size |
|---|---|---|---|---|
| before | **0** | 0 | 15,872 | 643,496 |
| after | **62** | 120 | 21,728 | 717,064 |

Two measurement notes worth keeping, because both produced a wrong answer first:

- **Counting NEON instructions is not a test for this.** The pre-fix library
  already contained 15,872 vector instructions — GCC auto-vectorises libjpeg's C
  fallback paths perfectly happily. Only the `jsimd_*` symbols distinguish "has
  the hand-written SIMD" from "was auto-vectorised".
- **`nm -D` cannot see it.** `jsimd_*` are internal, so they never appear in
  `.dynsym`. A dynamic-symbol check reports 0 whether SIMD is on or off. Use
  `nm -a`, and confirm the library is not stripped before believing a zero.

### `x264-20220222` — two unrelated blockers

1. `CFLAGS="-fPIC"` tripped haikuporter's `runConfigure` guard, which rejects an
   overridden `CFLAGS` carrying no `-O`, and then a second guard requiring `-g`
   when the recipe declares a debuginfo package. Now `-fPIC -O2 -g`.
2. `config.guess` in this snapshot cannot name arm64 Haiku, so `config.sub` was
   invoked with an empty argument (`Unknown system , edit the configure`). Fixed
   by passing `--host=$(gcc -dumpmachine)`.

x264 uses its own build system, not cmake, so it never had the processor problem:
**433** `*neon*` symbols, 13,219 NEON instructions.

### `x265-3.5` — needed the cmake fix, then exposed two upstream arm64 bugs

With the processor override in place x265 sets `ARM 1` and `ARM64 1` and starts
compiling NEON. That surfaced two defects in 3.5:

1. **The high-bit-depth variants do not have aarch64 assembly.** The 10/12-bit
   libraries reference `x265_12bit_*_neon` / `x265_10bit_*_neon`, which do not
   exist in this release — only the unprefixed 8-bit entry points do. So
   `enableAsmHdr` is now `OFF` for arm64, which affects only those two static
   variants; the main 8-bit library does not pass `ENABLE_ASSEMBLY` at all and
   takes x265's own default of ON. That is where essentially all encoding
   happens.
2. **`dynamicHDR10` applies 32-bit ARM flags to an ARM64 build.**
   `source/CMakeLists.txt` guards its ARM flag block with `if(ARM64)` and uses a
   plain `-fPIC` there. `source/dynamicHDR10/CMakeLists.txt` repeats the same
   block *without* the guard, so aarch64 gets `-marm -mfpu=vfp
   -mfloat-abi=hard -mcpu=native` and every translation unit fails. The recipe
   now seds that subproject to match what the main list already does. HDR10+ is
   kept.

Result: **50** `x265_*_neon` symbols, 39,649 NEON instructions.

### `svt_av13-3.0.2` — only needed the cmake fix

**1,453** `*neon*` symbols, 77,386 NEON instructions in 563,646 — 13.7% of the
library.

### `libvpx1.16-1.16.0` — built fine, contained no NEON at all

libvpx is not cmake, so it failed differently and more quietly: it has no Haiku
target, so its configure selected `generic-gnu`, which disables **every**
architecture-specific assembly path. The package built and installed with
`RC=0`.

The give-away was that the `.so` still had an unstripped symbol table (2,199
symbols) and *zero* of them matched `neon` — so the zero was informative rather
than an artefact of stripping. The `*_neon.cc` files visible in the build log are
`third_party/libyuv`, reached only by the unit tests.

Fixed with `--target=arm64-linux-gcc`; the OS half of that triple only selects
threading and detection glue. That then failed on
`vpx_ports/aarch64_cpudetect.c`, whose own `#error` names the remedy, so
`--disable-runtime-cpu-detect` is passed too. There is nothing to detect: NEON /
ASIMD is mandatory in ARMv8-A.

| | `*neon*` symbols | NEON insns | total insns | size |
|---|---|---|---|---|
| `generic-gnu` | **0** | 9,409 | 304,820 | 1,549,696 |
| `arm64` | **436** | 47,873 | 382,304 | 1,907,448 |

## No SVE, checked in every one

SVE traps on Haiku — nothing clears `CPACR_EL1.ZEN` — and ifunc-based runtime
dispatch is unavailable on Haiku arm64, so a package cannot select an SVE path at
load time either. Every library above was disassembled for `z<n>.`, `ptrue` and
`whilelo`: **zero in all five.**

This is a live hazard, not a theoretical one. On this exact compiler,
`-mcpu=neoverse-v1` turned a plain float loop into **31 SVE instructions and zero
NEON ones**, and `-mcpu=neoverse-v2` also emits SVE. Naming a Graviton 3/4 core
via `-mcpu` is therefore unsafe *regardless* of dropping Graviton 1/2 support.
`-mtune` is the safe knob — it changes scheduling without changing the required
ISA.

## Not built

- **`libaom` has no recipe anywhere in the tree.** AV1 encoding is served by
  `svt_av13` (built) and decoding by `dav1d` (already built), but the reference
  AV1 encoder is simply unavailable until someone writes the recipe.
- **`ffmpeg8-8.1.2` cannot resolve its dependencies** — the same chain that
  blocked `ffmpeg6`, unchanged by moving to 8.x:

  ```
  pygments_python310  ->  harfbuzz_devel-14.2.0  ->  libass-0.17.5  ->  ffmpeg8
  ```

  `libass` is a hard `REQUIRES` (`lib:libass`), not only a build dependency, so
  it cannot be dropped without also cutting `--enable-libass` and the
  `PROVIDES`/`REQUIRES` entry — a real feature cut (no ASS/SSA subtitle
  rendering) that has not been made here. The tractable fix is upstream of
  ffmpeg: give `harfbuzz` a resolvable `pygments`, which is a python-packaging
  problem rather than a codec one.
