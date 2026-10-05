# Ladybird on DeBeOS arm64 — Wave 1

Wave 1 of porting the [Ladybird](https://github.com/LadybirdBrowser/ladybird)
browser (independent engine, not WebKit) to DeBeOS on AWS Graviton (arm64).

**Scope of Wave 1: get Ladybird's CMake to a full, clean configure against
DeBeOS arm64 system libraries.** This is a configure-only milestone — nothing is
compiled or linked yet. It establishes that every dependency Ladybird's build
system looks for is satisfiable on the platform *except* the two deliberately
deferred to later waves (Skia and an APNG-capable libpng).

Upstream reference point: Ladybird `HEAD` at commit `90998c5d`.

## What Wave 1 achieved

Running the canonical configure (`configure-wave1.sh`) against a provisioned
builder stops with **exactly one** CMake error — the Skia pkg-config lookup
(`skia=148`) — which is the intended Wave-2 wall. Everything upstream of that
resolves against the DeBeOS/Haiku system:

- Toolchain: Clang 21.1.8, LLD linker.
- Core libs found from the system: CURL, ICU (74.1), LibXml2, OpenSSL, SQLite3,
  JPEG, ZLIB, PNG, WebP, Fontconfig, Threads, Python3 (3.10), Git.
- pkg-config modules found: libedit, libwoff2dec, brotli (enc/dec/common),
  libpsl, libtommath, and the four `libav*-ladybird` ffmpeg shims.
- Provisioned-prefix libs found: FastFloat, simdutf, wuffs.
- ANGLE (GLES): absent, now treated as optional (see patch rationale below).

The clean configure log is captured verbatim in `configure-wave1.log`; its sole
`CMake Error` is the Skia block at the tail.

## Files in this directory

| File | What it is |
|---|---|
| `ladybird-cmake-wave1.patch` | `git diff` against Ladybird (2 files) — the only changes to Ladybird's own tree. |
| `provision-deps.sh` | Dependency provisioning run on the builder; builds an external `/boot/home/lbdeps` prefix. Not a Ladybird patch. |
| `configure-wave1.sh` | The canonical configure command (exact flags). |
| `configure-wave1.log` | Clean configure log; the sole CMake error is `skia=148`. |

## Patch rationale (per hunk)

The patch touches **two** files in Ladybird's own tree. All of it is DeBeOS's
own implementation; none of it is offered as or intended to be an upstream
Ladybird change.

### 1. `Meta/CMake/check_for_dependencies.cmake`

**(a) ICU — drop the exact version pin.**
Upstream pins `find_package(ICU 78.3 EXACT REQUIRED COMPONENTS data i18n uc)`.
On DeBeOS the coherent system ICU is **icu74**: `harfbuzz_devel` and the rest of
the C++ stack are built against it (`devel:libicuuc` from icu74). Installing
`icu77_devel` is *rejected* by the package manager because it conflicts with
`harfbuzz_devel`'s ICU dependency — so icu77/78 cannot coexist with the system
harfbuzz. The pin is relaxed to `find_package(ICU REQUIRED COMPONENTS data i18n
uc)`, which then resolves to the system icu74 (confirmed in the log:
`Found ICU ... version "74.1"`).

**(b) ANGLE (GLES) — make it optional.**
Upstream unconditionally does `pkg_check_modules(angle REQUIRED IMPORTED_TARGET
angle)`. GLES/ANGLE is not available on the headless DeBeOS builder and is not
needed for CPU raster. The hunk changes it to a non-REQUIRED probe and sets
`ANGLE_TARGETS ""` when ANGLE is absent. This is safe because `ANGLE_TARGETS` is
only ever consumed through `foreach()` / link lists (in LibCompositing, LibWeb,
and the Compositor), where an empty list is a no-op. A GPU-less configure now
succeeds.

### 2. `Libraries/LibJS/CMakeLists.txt`

**(c) arm64 detection fallback.**
LibJS selects its JIT/interpreter arch from `CMAKE_SYSTEM_PROCESSOR`. On a native
Haiku build `uname -p` returns the string `"other"`, so CMake leaves
`CMAKE_SYSTEM_PROCESSOR` as `"other"` and passing `-DCMAKE_SYSTEM_PROCESSOR=aarch64`
does **not** stick (CMake overwrites it during the native toolchain probe). The
upstream `else()` branch then fatal-errors. The hunk adds a fallback that runs
`uname -m` (which returns `arm64` on Haiku arm64) and sets `FLAP_ARCH=aarch64`
(or `x86_64`) accordingly, preserving the original fatal error for genuinely
unknown machines.

## Dependency provisioning (`provision-deps.sh`)

This is **not** a patch to Ladybird; it builds an external prefix at
`/boot/home/lbdeps` that satisfies the dependencies the system packages don't
provide. Steps:

1. **System `*_devel` packages** installed via the package manager — including
   **`icu74_devel`** (explicitly *not* icu77, per rationale (a)): libfmt,
   mimalloc, simdjson, libavif, SDL3, fontconfig, libedit, libtommath, harfbuzz,
   freetype, libpng16, libjpeg-turbo, libwebp, brotli, woff2, openssl3, curl,
   libxml2, sqlite, zlib, libpsl, dav1d, libdwarf.
2. **FastFloat v8.3.1** — cloned and installed with its real CMake config package
   (`-DFASTFLOAT_INSTALL=ON`).
3. **simdutf (master)** — built as a static lib with its CMake config package
   (C++20).
4. **wuffs v0.3.c** — the single-file header that `LibImageDecoders`'
   `find_path()` needs, dropped at `$PREFIX/include/wuffs/wuffs-v0.3.c`.
5. **Four `libav*-ladybird` pkg-config shims** — `avcodec`, `avformat`, `avutil`,
   `swresample`. The DeBeOS repo ships **no** ffmpeg devel (only a minimal
   `ffmpeg_x264` binary), so these `.pc` files satisfy Ladybird's
   `pkg_check_modules(... libav*-ladybird)` at **configure time only**. They
   point at headers/libs that are not actually present — Wave 2 must replace them
   with a real ffmpeg devel build before `LibMedia` can *link*.

## Required configure flags

See `configure-wave1.sh` for the exact command. The load-bearing choices:

- **`-DENABLE_LTO_FOR_RELEASE=OFF`** — the builder image has no `llvm-ar`, so
  clang's thin-LTO archiver test fails if LTO is on. Must stay off until an
  `llvm-ar` is on the image.
- **`-DCMAKE_LINKER_TYPE=LLD`** plus the `*_USING_LINKER_LLD` / `_MODE=FLAG`
  variables — select LLD explicitly.
- **`-DCMAKE_AR` / `-DCMAKE_RANLIB`** (and the per-compiler `*_COMPILER_AR` /
  `*_COMPILER_RANLIB`) pinned to the system `ar`/`ranlib`.
- **`-DCMAKE_PREFIX_PATH=/boot/home/lbdeps`** plus `FastFloat_DIR`,
  `simdutf_DIR`, `WUFFS_INCLUDE_DIR` pointing into the provisioned prefix.
- **`PKG_CONFIG_PATH`** must include both `/boot/home/lbdeps/lib/pkgconfig` (for
  the ffmpeg shims) and `/boot/system/develop/lib/pkgconfig`.

## How to reproduce on a builder

On a native Graviton (arm64) DeBeOS/Haiku builder:

```sh
# 0. Toolchain (once):
pkgman install -y llvm21_clang llvm21_lld llvm21_libs cmake ninja gn \
                  rust_bin pkgconf git

# 1. Provision the dependency prefix:
sh provision-deps.sh                 # builds /boot/home/lbdeps

# 2. Get Ladybird and apply the Wave-1 patch:
cd /boot/home && git clone https://github.com/LadybirdBrowser/ladybird.git lb
cd lb && git checkout 90998c5d
git apply /path/to/ladybird-cmake-wave1.patch

# 3. Configure (expect exactly one error: skia=148):
sh /path/to/configure-wave1.sh
```

Expected terminal state: `Configuring incomplete, errors occurred!` whose only
`CMake Error` is the `skia=148` / `skia` pkg-config block.

## Known Wave-2 blockers ("next")

1. **Skia** — the headline Wave-2 task. `pkg_check_modules(skia=148)` has no
   system or provisioned package yet; Skia must be built for arm64 and exposed to
   pkg-config.
2. **libpng without APNG** — the system `libpng16` is built **without** the APNG
   (animated PNG) feature, which `LibImageDecoders` hard-requires. This needs
   libpng rebuilt with the APNG patch. **Do not feature-cap** around it — the fix
   is to produce an APNG-capable libpng, not to disable the decoder.
3. **ffmpeg devel** — the `libav*-ladybird` shims are configure-only stand-ins.
   A real ffmpeg devel build for arm64 is required before `LibMedia` links.

Tracked under DeBeOS issue #575.
