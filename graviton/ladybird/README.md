# Ladybird on arm64 DeBeOS/Haiku — build recipe

Insurance capture of the reproduction setup for building the
[Ladybird](https://ladybird.org/) browser engine natively on arm64 DeBeOS/Haiku
(AWS Graviton). This directory preserves the **recipe** — scripts, wrapper
shims, pkg-config files, CMake/source patches, and exact dependency versions —
captured off the live Wave-3 builder before it could be lost. It is **not** the
built artifacts (multi-GB `lbdeps`/Skia blobs are intentionally excluded).

Tracking issue: **#599**. Spike/porting history: issue **#575**.

**Status.** The whole default target builds, and every binary it produces
loads. On the rev-6 builder `ninja` reports `[2995/2995]` with no failed edges
(`[2803/2803]` for `WebContent` + `WebDriver` alone), and all 22 executables
under `build-599/bin/` start — `Compositor`, `ImageDecoder`, `MediaServer`,
`RequestServer`, `WasmCompiler`, `WebContent`, `WebDriver`, `WebWorker`, `dns`,
`image`, `js`, `wasm`, `xml`, `style-replay`, `test262-runner`, the two
`dump-html-*`, and the four build-time generators. For scale: the figure
recorded on the capture builder was `[363/2262]`.

The WASM Cranelift JIT is **compiled in and exercised**, not stubbed:
`bin/wasm --dump-native` emits native arm64 code per function, arithmetic is
correct, and both fault-recovery paths return WASM traps instead of killing the
process (a guard-page read gives `Memory access out of bounds`, `i32.div_s`
by zero gives `Integer division overflow`). See "WASM Cranelift JIT" below.

Not yet done: no rendered PNG, because nothing in this build can open a page.
`WebDriver` starts a session by launching a browser process called `Ladybird`
(`Services/WebDriver/main.cpp:216`), and `UI/` only has `Qt` and `Android` —
the DeBeOS arm64 repo has no `qt6`, so no `Ladybird` binary exists and
`ENABLE_QT_UI=OFF` is not a choice. The next wall for a usable browser is
either qt6 for arm64 or a native `UI/Haiku` on the Interface Kit.

Fonts are a hard runtime dependency, not a nicety: `FontPlugin`'s constructor
`VERIFY`s that a fixed-width font resolved
(`Libraries/LibWeb/Platform/FontPlugin.cpp:55`), and a stock DeBeOS image makes
that fail — see "Monospace fonts" below.

## Target coordinates

| Component | Version / ref | Notes |
|---|---|---|
| Builder | `c7g.8xlarge`, DeBeOS/Haiku arm64 (`hrev59996`) | native build, no cross |
| Ladybird | commit `90998c5d` | `LibJS: Read the cloned source in TypedArray.prototype.set` |
| Skia | `chrome/m148` (`13ffba253f`) | CPU raster, Ganesh no-GPU |
| libpng | `1.6.53` + APNG patch + arch patch | APNG required by LibImageDecoders; arch patch so the NEON paths are compiled, not just referenced |
| ICU | **78.3 from source**, into `lbdeps/icu78` | mandatory: LibUnicode uses ICU 78 C++ internals, so relaxing the pin to the system icu74 does not compile |
| ffmpeg | `n7.1` | real devel build (repo ships none) |
| libtommath | `1.3.0`, `-D__STDC_IEC_559__` | needs `mp_expt_n` (repo image had 1.2.0); the macro is what makes `mp_set_double` exist under clang |
| libavif | `1.4.2` from source | repo has 0.9.3, which has no `avifDecoder::repetitionCount` |
| FastFloat | `v8.3.1` | header-only |
| simdutf | from source | built static, CXX_STANDARD=20 |
| simdjson | `5.0.2` (`simdjson_devel`) | from DeBeOS repo |
| wuffs | `v0.3.3` | single-file header |
| clang | `21.1.8` (`llvm21_clang`) | C++23, targets `aarch64-unknown-haiku` |
| gcc | `13.3.0` | used as the Haiku link driver (see below) |
| rustc / cargo | `1.100.0-nightly (787af2b8c 2026-08-25)` / `1.100.0` (`rust_bin`) | native; `rust-toolchain.toml` channel `1.98.0` is ignored (no rustup) |
| gn / ninja | `2385` / `1.13.2` | repo-native; `bin/fetch-gn` bypassed |

## Reproduction sequence

Normally you do not run these by hand. `scripts/run-wall-build.sh all` drives
the whole sequence in order, each phase logging to `$LOGDIR/<phase>.log` and
printing a `SENTINEL_<phase>_OK` line, so a detached run can be polled without
a live SSH session. It also encodes the ordering constraints that are easy to
get wrong by hand (the ffmpeg shims must be replaced before LibMedia links;
Skia must exist before configure). Individual phases:

```
toolchain deps png icu ffmpeg lb skiafetch skiabuild pc configure build
tommath avif          # re-runs of `deps` subsets, for iterating on one dep
lbpatch               # re-apply the Haiku patch without re-cloning
```

The by-hand sequence it replaces, run natively on the builder as `baron` from
`/boot/home`:

1. **Install the toolchain** (pkgman):
   ```
   pkgman install -y llvm21_clang llvm21_lld llvm21_libs cmake ninja gn \
                     rust_bin pkgconf git
   ```
2. **Provision dependencies** into `/boot/home/lbdeps`:
   `scripts/provision-deps.sh` — installs the system `*_devel` set, builds
   FastFloat, simdutf, simdjson, libtommath (`scripts/build-tommath.sh`) and
   libavif (`scripts/build-avif.sh`) into `lbdeps`, drops the wuffs header, and
   writes the ffmpeg `-ladybird` configure shims. `simdjson_devel`,
   `libtommath_devel` and `libavif_devel` are deliberately *not* installed from
   the repo — the first two are unsatisfiable there (DeBeOS #708) and the third
   is too old (0.9.3).
3. **Build real ffmpeg n7.1** into `lbdeps`: `scripts/run-ffmpeg.sh` (replaces
   the shims so LibMedia can link). The real `.pc` files are in `pkgconfig/`.
4. **Rebuild libpng16 1.6.53 with APNG and with its NEON paths compiled**:
   ```
   git clone https://github.com/pnggroup/libpng.git pngbuild/libpng-1.6.53
   cd pngbuild/libpng-1.6.53 && git checkout v1.6.53
   patch -p1 < <repo>/graviton/ladybird/patches/libpng-1.6.53-apng.patch
   patch -p1 < <repo>/graviton/ladybird/patches/libpng-1.6.53-haiku-arch.patch
   ```
   then `scripts/build-png.sh`. The second patch is not optional: without it
   `uname -p` = `other` keeps libpng's CMake from compiling `arm/*.c` while
   `pngpriv.h` still enables the NEON code paths from `__ARM_NEON`, and the
   resulting `libpng16.so` links but cannot be loaded.
5. **ICU 78.3 from source**: `scripts/run-icu2.sh`, into `lbdeps/icu78`
   beside the system icu74. Mandatory — LibUnicode uses ICU 78 C++ internals
   (`UnicodeSet::strings()`, the `Calendar::internalGetMonth` signature), so
   relaxing the `ICU 78.3 EXACT` pin to icu74 does not compile however the pin
   is spelled. The two coexist because ICU version-suffixes every exported
   symbol and namespace, which is also why Ladybird can use 78 while linking a
   harfbuzz built against 74.
6. **Clone Ladybird** at `90998c5d`: `scripts/run-lb-clone.sh`, then apply the
   port patch:
   ```
   cd /boot/home/lb && git apply <repo>/graviton/ladybird/patches/ladybird-haiku.patch
   ```
7. **Build Skia** (CPU raster) for arm64-Haiku:
   ```
   scripts/run-skia-fetch.sh                      # clone chrome/m148 + sync deps
   cd /boot/home/skia && git apply <repo>/graviton/ladybird/patches/skia-haiku.patch
   scripts/build-skia.sh                          # gn gen (skia/args.gn) + ninja skia + PathOps
   ```
8. **Expose the lbdeps pkg-config shims**. Copy everything in `pkgconfig/` into
   `/boot/home/lbdeps/lib/pkgconfig/` (the `*-ladybird.pc` ffmpeg aliases,
   `skia.pc`, `simdjson.pc`, `simdutf.pc`, `libtommath.pc`, `libpng16.pc`;
   symlink `libpng.pc -> libpng16.pc`). `libtommath-FIXED.pc` corrects the
   system libtommath 1.2.0 `.pc` prefix if you fall back to the system copy.
   `find_package(LIBAVIF)` is a *config* lookup, not pkg-config: it reads
   `lbdeps/lib/cmake/libavif/libavif-config.cmake`, which step 2's libavif
   build installs.
9. **Configure Ladybird** (no vcpkg, plain `-G Ninja`):
   `scripts/configure-ladybird.sh` → `build-wave2`.
10. **Build**: `scripts/run-build.sh` builds `WebContent` + `WebDriver`
    (screenshot is built into WebDriver; there is no standalone
    headless-browser exe). A bare `ninja` builds the full default target,
    including the `Compositor`, `ImageDecoder`, `MediaServer`, `RequestServer`,
    `WasmCompiler` and `WebWorker` services. See "Detaching a long build"
    below — `nohup` alone is not enough here.

`scripts/run-craneliftB.sh` is an auxiliary track that reconfigures a separate
`build-craneliftB` with `-DENABLE_CRANELIFT_JIT=ON` to prove LibWasm links the
Cranelift WASM JIT (not stubbed).

## Detaching a long build

`nohup ninja … &` does **not** survive an SSH disconnect on this box. nohup
sets `SIGHUP` to `SIG_IGN`, but ninja installs its own `SIGHUP` handler
(`SetInterruptHandlers`), which replaces the inherited `SIG_IGN`; ninja's
handler means "stop starting edges and wait for the running ones", and that
wait never returns. The symptom is a single-threaded `ninja` left in `ps`
forever with no children and a log that stops mid-build. Three of them
accumulated in one build tree before this was understood — and because they
die holding the cargo lock, the next run aborts too.

Put the build in its own session instead, so the teardown never reaches it:

```python
pid = os.fork()                 # parent waits and exits
if pid: os.waitpid(pid, 0); sys.exit(0)
os.setsid()                     # new session, no controlling terminal
if os.fork(): sys.exit(0)       # so it can never reacquire one
# redirect 0/1/2 to /dev/null and the logfile, then execvp(ninja, ...)
```

There is no `setsid(1)`, `screen` or `tmux` in the arm64 repo, but
`python3.10` is on the image and `os.setsid` works.

Unrelated to signals, and worth knowing before diagnosing anything with it:
Haiku's `ps` takes a *team-name filter*, not BSD flags. `ps ax` means "show
teams named `ax`" and prints nothing at all, which reads exactly like "the
build is gone".

> **Do not run two `ninja` invocations against the same build tree, and do not
> run the `build-wave2` and `build-craneliftB` tracks at the same time.** On
> DeBeOS, cargo's file locking is a silent no-op (std's `File::lock` returns
> `Unsupported` on Haiku and cargo reads that as *acquired*), so concurrent
> cargo processes corrupt shared build state without any warning — see
> [../docs/rust-file-locking.md](../docs/rust-file-locking.md), DeBeOS #638.
> `ninja -j32` on its own is safe: Ladybird's `Meta/CMake/rust_crate.cmake`
> declares a depth-1 `cargo` job pool and tags every cargo command with it, so
> one ninja run never starts two cargos even though all library crates share
> `${CMAKE_BINARY_DIR}/cargo/build`. That pool only binds within a single ninja
> process; it does not protect a second ninja, or the second build tree, which
> shares `~/.cargo/registry`.

## Monospace fonts (why `dejavu` is in the pkgman list)

A stock DeBeOS arm64 install cannot start any Ladybird process that builds a
`FontPlugin`:

```
VERIFICATION FAILED: m_default_fixed_width_font
    at Libraries/LibWeb/Platform/FontPlugin.cpp:55
```

`FontPlugin`'s constructor resolves `GenericFont::UiMonospace` and `VERIFY`s the
result. `compute_generic_font_name()` tries Skia's font manager first and then
walks a hard-coded fallback list — `Andale Mono`, `Courier New`, `Courier`,
`FreeMono`, `OCR A Std`, `Noto Sans Mono`, `DejaVu Sans Mono`,
`Droid Sans Mono`, `Liberation Mono` — and if nothing matches it returns the
*first* name anyway, so `FontDatabase::get()` answers null and the `VERIFY`
fires. The image ships exactly one monospace family and it is named
**`Noto Mono`** (`fc-query`: family `Noto Mono`, spacing 100), which is on
neither that list nor fontconfig's own `60-latin.conf` monospace preferences.
`pkgman install dejavu` supplies `DejaVu Sans Mono`, which is on both; after it
the `FontPlugin` `VERIFY` passes.

Do not expect fontconfig to paper over this. On DeBeOS every file in
`/boot/system/settings/fonts/conf.d/` is a **dangling symlink** —
`../../../../../.self/data/fontconfig/conf.avail/<name>.conf`, five levels up
from `conf.d` lands at `/`, and `/.self` does not exist — so `fc-conflist`
reports only `fonts.conf` as active and every generic-family, metric-alias and
spacing rule is inert. Measured consequence: `fc-match monospace` answers
`Noto Sans`, and `fc-match serif` answers `Noto Sans` where the same files
included by absolute path answer `Noto Serif`. Filed separately; the recipe
works around it by naming a real family rather than a generic one.

## Skia PathOps (found on the rev-4 build, #622)

With `skia_enable_pdf = false`, Skia m148 leaves PathOps out of `libskia.a`: the `:pathops` source set reaches the
`skia` component only through `:pdf`. LibGfx calls `Op(SkPath, SkPath, SkPathOp)`, so the build links but
`liblagom-gfx` fails to load (`Could not resolve symbol '_Z2OpRK6SkPathS1_8SkPathOp'`). `scripts/build-skia.sh`
builds the `pathops` target and adds its objects to the archive, and checks that `Op` is defined. The first rev-4
package (sha256 `313114347c...`, never launched) had this defect.

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
  - `Meta/CMake/check_for_dependencies.cmake`: make ANGLE optional (headless
    CPU raster needs no GLES). The `ICU 78.3 EXACT` pin is *kept* — see step 5.
  - `CMakeLists.txt` + `cmake_options.cmake`: add `ENABLE_QT_UI` and gate the Qt
    `UI/` subdir (Haiku has no Qt6; headless renders via Services).
  - `Libraries/LibJS/CMakeLists.txt`: `FLAP_ARCH` fallback via `uname -m`
    (Haiku `uname -p` returns `other`, so `CMAKE_SYSTEM_PROCESSOR` is wrong).
  - `LibCore/Environment.cpp`: no `secure_getenv` on Haiku → plain `getenv`.
  - `LibCore/LocalServer.cpp`: include `<sys/ioctl.h>` for `FIONBIO`.
  - `LibWasm/CMakeLists.txt` + `LibWasm/BytecodeInterpreter.cpp`: turn the
    Cranelift JIT **on** for Haiku arm64 and add the two `uc_mcontext.elr`
    sites its fault recovery needs — see "WASM Cranelift JIT" below.
- **`skia-haiku.patch`** — 2 source edits (plus `skia/args.gn`): add `__HAIKU__`
  to `SkFeatures.h`'s Unix detection, and a `__HAIKU__` branch in
  `SkMemory_malloc.cpp` (`malloc_usable_size` is `_DEFAULT_SOURCE`-guarded on
  Haiku; return the requested size as a safe lower bound).
- **`libpng-1.6.53-apng.patch`** — the upstream animated-PNG patch for libpng
  1.6.53 (defines `PNG_APNG_SUPPORTED`). Vendored because DeBeOS's repo libpng16
  is built without it and LibImageDecoders hard-requires it.
- **`libpng-1.6.53-haiku-arch.patch`** — fall back to `uname -m` when
  `CMAKE_SYSTEM_PROCESSOR` arrives as `other`, so libpng's ARM block actually
  runs. See the patch's own preamble for the four undefined NEON symbols it
  removes.

## WASM Cranelift JIT

The JIT was never a version-skew problem. `WASM_COMPILED_FAULT_RECOVERY_SUPPORTED`
is set only for Windows, Apple, and Linux on x86_64/aarch64
(`Libraries/LibWasm/CMakeLists.txt`), and when it is 0
`try_cranelift_compile()` returns `false` outright
(`Libraries/LibWasm/CraneliftBridge.cpp:1293`) — so on Haiku the entire JIT was
off, and the `cranelift_trap_message` build error was a knock-on of that: the
function sat inside the `#if` guard while `interpret()` calls it
unconditionally.

Two changes turn it on. `LibWasm/CMakeLists.txt` sets the flag for Haiku when
`uname -m` is aarch64/arm64 (not `CMAKE_SYSTEM_PROCESSOR`, which is `other`
here), and `BytecodeInterpreter.cpp` gets the Haiku arm64 branch of the
fault-recovery context switch. Haiku's `mcontext_t` is `struct vregs`
(`headers/posix/arch/arm64/signal.h`) and the resume address is `elr`, which
`arch_restore_signal_frame()` copies back into the interrupted frame
(`src/system/kernel/arch/arm64/arch_thread.cpp:289`) — so `elr` is both where
the handler writes the trampoline address and where it reads the faulting PC.

Verified at runtime, not just at link time:

| check | result |
|---|---|
| `build.ninja` | `WASM_COMPILED_FAULT_RECOVERY_SUPPORTED=1`, `CraneliftBridge.cpp.o` (not `CraneliftStubs.cpp.o`) |
| `wasm --dump-native -i` | `Function #0 'add' (100 bytes)` — real arm64 code |
| 13 binary `i32` ops | all correct (`div_s(12,5)` → 2, `shl(12,5)` → 384, …) |
| guard-page read | `Execution trapped: Memory access out of bounds` (SIGSEGV → `elr`) |
| `i32.div_s` by zero | `Execution trapped: Integer division overflow` (SIGILL/SIGFPE → trap table) |

> `bin/wasm` pushes `--arg` values onto a stack, so the **last** `--arg` becomes
> parameter 0. Reading that backwards makes a correct JIT look like it swaps
> every non-commutative operand.

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
  `simdutf_DIR`, `WUFFS_INCLUDE_DIR`, `PNG_PNG_INCLUDE_DIR`, `PNG_LIBRARY`,
  `ICU_ROOT=lbdeps/icu78`, `LIBAVIF_DIR=lbdeps/lib/cmake/libavif`.
- `-fPIC`, never `-fPIE`, for executables too (DeBeOS #715), and
  `-z max-page-size=4096` on every image (DeBeOS #720).
- `PKG_CONFIG_PATH=/boot/home/lbdeps/lib/pkgconfig:/boot/system/develop/lib/pkgconfig`.

All paths are the builder's absolute `/boot/home/...` layout; adjust the prefix
if you provision elsewhere.
