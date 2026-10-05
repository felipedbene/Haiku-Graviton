# Ladybird on arm64 DeBeOS/Haiku — build recipe

Insurance capture of the reproduction setup for building the
[Ladybird](https://ladybird.org/) browser engine natively on arm64 DeBeOS/Haiku
(AWS Graviton). This directory preserves the **recipe** — scripts, wrapper
shims, pkg-config files, CMake/source patches, and exact dependency versions —
captured off the live Wave-3 builder before it could be lost. It is **not** the
built artifacts (multi-GB `lbdeps`/Skia blobs are intentionally excluded).

Tracking issue: **#599**. Spike/porting history: issue **#575**.

At capture time the build had cleared every dependency wall and was partway
through compiling (`~15%` of 2262 targets, past the Rust/WASM `cranelift-compiler`
stage), with the next frontier being a WebGL codegen step. No rendered PNG yet.
This recipe is enough that a fresh `c7g.8xlarge` from the canonical AMI can
rebuild the dependency environment and re-drive the Ladybird build to the same
point.

## Target coordinates

| Component | Version / ref | Notes |
|---|---|---|
| Builder | `c7g.8xlarge`, DeBeOS/Haiku arm64 (`hrev59996`) | native build, no cross |
| Ladybird | commit `90998c5d` | `LibJS: Read the cloned source in TypedArray.prototype.set` |
| Skia | `chrome/m148` (`13ffba253f`) | CPU raster, Ganesh no-GPU |
| libpng | `1.6.53` + APNG patch | pnggroup/libpng, APNG required by LibImageDecoders |
| ICU | system **icu74** (pin relaxed) | or icu78.3 from source if pin kept |
| ffmpeg | `n7.1` | real devel build (repo ships none) |
| libtommath | `1.3.0` | needs `mp_expt_n` (repo image had 1.2.0) |
| FastFloat | `v8.3.1` | header-only |
| simdutf | from source | built static, CXX_STANDARD=20 |
| simdjson | `5.0.2` (`simdjson_devel`) | from DeBeOS repo |
| wuffs | `v0.3.3` | single-file header |
| clang | `21.1.8` (`llvm21_clang`) | C++23, targets `aarch64-unknown-haiku` |
| gcc | `13.3.0` | used as the Haiku link driver (see below) |
| rustc / cargo | `1.100.0-nightly (787af2b8c 2026-08-25)` / `1.100.0` (`rust_bin`) | native; `rust-toolchain.toml` channel `1.98.0` is ignored (no rustup) |
| gn / ninja | `2385` / `1.13.2` | repo-native; `bin/fetch-gn` bypassed |

## Reproduction sequence

Run natively on the Graviton Haiku builder, as `baron`, from `/boot/home`.

1. **Install the toolchain** (pkgman):
   ```
   pkgman install -y llvm21_clang llvm21_lld llvm21_libs cmake ninja gn \
                     rust_bin pkgconf git
   ```
2. **Provision dependencies** into `/boot/home/lbdeps`:
   `scripts/provision-deps.sh` — installs the system `*_devel` set, builds
   FastFloat + simdutf into `lbdeps`, drops the wuffs header, and writes the
   ffmpeg `-ladybird` configure shims.
3. **Build real ffmpeg n7.1** into `lbdeps`: `scripts/run-ffmpeg.sh` (replaces
   the shims so LibMedia can link). The real `.pc` files are in `pkgconfig/`.
4. **Rebuild libpng16 1.6.53 with APNG**:
   ```
   git clone https://github.com/pnggroup/libpng.git pngbuild/libpng-1.6.53
   cd pngbuild/libpng-1.6.53 && git checkout v1.6.53
   patch -p1 < <repo>/graviton/ladybird/patches/libpng-1.6.53-apng.patch
   ```
   then `scripts/build-png.sh`.
5. *(optional)* **ICU 78.3 from source**: `scripts/run-icu2.sh` — only if you
   keep the upstream `ICU 78.3 EXACT` pin. The shipped patch relaxes it to the
   system icu74, so this is normally skipped.
6. **Clone Ladybird** at `90998c5d`: `scripts/run-lb-clone.sh`, then apply the
   port patch:
   ```
   cd /boot/home/lb && git apply <repo>/graviton/ladybird/patches/ladybird-haiku.patch
   ```
7. **Build Skia** (CPU raster) for arm64-Haiku:
   ```
   scripts/run-skia-fetch.sh                      # clone chrome/m148 + sync deps
   cd /boot/home/skia && git apply <repo>/graviton/ladybird/patches/skia-haiku.patch
   scripts/build-skia.sh                          # gn gen (skia/args.gn) + ninja skia
   ```
8. **Expose the lbdeps pkg-config shims**. Copy everything in `pkgconfig/` into
   `/boot/home/lbdeps/lib/pkgconfig/` (the `*-ladybird.pc` ffmpeg aliases,
   `skia.pc`, `simdjson.pc`, `simdutf.pc`, `libtommath.pc`, `libpng16.pc`;
   symlink `libpng.pc -> libpng16.pc`). `libavif-config-SHADOW.cmake` corrects
   the system libavif CMake config's bad Haiku include path;
   `libtommath-FIXED.pc` corrects the system libtommath 1.2.0 `.pc` prefix if
   you fall back to the system copy.
9. **Configure Ladybird** (no vcpkg, plain `-G Ninja`):
   `scripts/configure-ladybird.sh` → `build-wave2`.
10. **Build the headless render path**: `scripts/run-build.sh` builds
    `WebContent` + `WebDriver` (screenshot is built into WebDriver; there is no
    standalone headless-browser exe). Run it detached (`nohup`) so an SSH/agent
    disconnect does not kill the build.

`scripts/run-craneliftB.sh` is an auxiliary track that reconfigures a separate
`build-craneliftB` with `-DENABLE_CRANELIFT_JIT=ON` to prove LibWasm links the
Cranelift WASM JIT (not stubbed).

## Wrapper shims (`wrappers/`)

- `clang-lld` — `clang -fuse-ld=lld "$@"`.
- `haiku-cxx-link` — compile with clang, **link with g++**. clang-21's Haiku
  driver cannot link a runnable Haiku executable or a loadable `.so` (it emits
  an interp-less `-shared` ET_DYN that `runtime_loader` rejects with
  "Could not map image: Bad data"); g++ adds the Haiku crt + interp. g++
  `-fuse-ld=lld` is unusable (gcc passes `-m aarch64haiku`, which lld rejects),
  so it uses GNU ld + `--no-gc-sections` to dodge the ld-2.41 aarch64 stub bug,
  stripping clang/lld-only flags g++/ld reject. Mostly superseded by the CMake
  LLD mapping in the configure step; kept for reference.

## Patches (`patches/`)

- **`ladybird-haiku.patch`** — the full port diff against Ladybird `90998c5d`
  (11 files). Each change carries an inline rationale. Highlights:
  - `Meta/CMake/rust_crate.cmake`: link Rust host build-scripts/proc-macros with
    Haiku's `cc` (gcc) not bare clang (clang-linked host artifacts crash exit
    255); drop `-D warnings` (nightly std deprecation would be fatal).
  - `Meta/CMake/sync_rust_ffi_header.cmake`: match cargo 1.100's new
    `build/<crate>/<hash>/run/root-output` layout.
  - `Meta/CMake/compile_options.cmake`: `link_libraries(bsd network)` for
    Haiku's split libc; `-femulated-tls` (clang's `R_AARCH64_TLSDESC` relocs are
    unsupported by Haiku's arm64 runtime_loader); add `lbdeps/include`.
  - `Meta/CMake/check_for_dependencies.cmake`: relax `ICU 78.3 EXACT` to system
    ICU; make ANGLE optional (headless CPU raster needs no GLES).
  - `CMakeLists.txt` + `cmake_options.cmake`: add `ENABLE_QT_UI` and gate the Qt
    `UI/` subdir (Haiku has no Qt6; headless renders via Services).
  - `Libraries/LibJS/CMakeLists.txt`: `FLAP_ARCH` fallback via `uname -m`
    (Haiku `uname -p` returns `other`, so `CMAKE_SYSTEM_PROCESSOR` is wrong).
  - `LibCore/Environment.cpp`: no `secure_getenv` on Haiku → plain `getenv`.
  - `LibCore/LocalServer.cpp`: include `<sys/ioctl.h>` for `FIONBIO`.
  - `LibWasm/BytecodeInterpreter.cpp` / `CraneliftBridge.cpp`: guard the
    `<ucontext.h>` include and mark `serialize_insn` maybe_unused when
    `WASM_COMPILED_FAULT_RECOVERY_SUPPORTED=0` (Haiku interpreter-only path).
- **`skia-haiku.patch`** — 2 source edits (plus `skia/args.gn`): add `__HAIKU__`
  to `SkFeatures.h`'s Unix detection, and a `__HAIKU__` branch in
  `SkMemory_malloc.cpp` (`malloc_usable_size` is `_DEFAULT_SOURCE`-guarded on
  Haiku; return the requested size as a safe lower bound).
- **`libpng-1.6.53-apng.patch`** — the upstream animated-PNG patch for libpng
  1.6.53 (defines `PNG_APNG_SUPPORTED`). Vendored because DeBeOS's repo libpng16
  is built without it and LibImageDecoders hard-requires it.

## Key build settings (from the live `build-wave2` CMakeCache)

- Generator **Ninja**, **no vcpkg** (plain `-G Ninja` falls through to
  `find_package`/`pkg_check_modules`).
- `CMAKE_C/CXX_COMPILER = clang/clang++`; `CMAKE_BUILD_TYPE=Release`.
- Linker **LLD** via `CMAKE_LINKER_TYPE=LLD` +
  `CMAKE_C/CXX_USING_LINKER_LLD=-fuse-ld=lld` + `..._MODE=FLAG`.
- `CMAKE_AR/RANLIB` (and the `*_COMPILER_AR/RANLIB`) = binutils
  `/boot/system/bin/{ar,ranlib}` (no `llvm-ar` on the image).
- `ENABLE_LTO_FOR_RELEASE=OFF` (no `llvm-ar` → `check_ipo_supported` fails),
  `ENABLE_QT_UI=OFF`, `BUILD_TESTING=OFF`.
- `CMAKE_PREFIX_PATH=/boot/home/lbdeps`; explicit `FastFloat_DIR`,
  `simdutf_DIR`, `WUFFS_INCLUDE_DIR`, `PNG_PNG_INCLUDE_DIR`, `PNG_LIBRARY`.
- `PKG_CONFIG_PATH=/boot/home/lbdeps/lib/pkgconfig:/boot/system/develop/lib/pkgconfig`.

All paths are the builder's absolute `/boot/home/...` layout; adjust the prefix
if you provision elsewhere.
