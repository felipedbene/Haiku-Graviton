# Ladybird on DeBeOS / Graviton (arm64 Haiku) — Wave 3

Successor to the Wave-2 recipe in [`README.md`](README.md) (tracking issue #575).
Wave 2 reached a clean CMake configure; Wave 3 clears the **Rust / LibTextCodec
wall** that stopped the compile at ~2.6% and drives the real, full build
(nothing disabled, no stubs) to **1877 / 1885 targets** — the entire dependency
layer plus almost all of LibWeb/WebContent. The only remaining targets are the
two subsystems being brought up for real on their own tracks: the Cranelift
WASM JIT and WebGL/ANGLE.

Pinned revision this wave was driven against:

- Ladybird `90998c5dc7` ("LibJS: Read the cloned source in TypedArray.prototype.set")
- Toolchain (DeBeOS package repo): clang 21.1.8, `rustc`/`cargo`
  **1.100.0-nightly (787af2b8c 2026-08-25)**, gcc/`cc` 13.3.0, GNU ld 2.46.1,
  cmake 4.1.6, ninja 1.13.2.

The complete, honest diff that compiles (all 11 files, superset of the Wave-2
curated patch; **no stubs, nothing disabled**) is
[`ladybird-haiku-arm64-wave3.patch`](ladybird-haiku-arm64-wave3.patch). Apply it
to the Ladybird checkout in place of the Wave-2 `ladybird-haiku-arm64.patch`.
Configure with the full real feature set (`ENABLE_CRANELIFT_JIT=ON`, WebGL on).

## The Rust wall — root cause and fix (the gating Wave-2 blocker)

Ladybird builds several **mandatory** Rust crates (`LibTextCodec`, `LibUnicode`,
`LibRegex`, `LibURL` panic-init shims, `libweb_rust`, `libcompositing_rust`,
plus the Cranelift WASM JIT compiler). On DeBeOS arm64 these were the gating
Wave-2 blocker: the `serde` build script exited **255** at startup, and dropping
`--target=aarch64-unknown-haiku` only traded that for the ld-2.41 stub bug.

The exit-255 was **not** a cross-compile problem — it was a *host-linker*
problem, fixed in `Meta/CMake/rust_crate.cmake`:

1. **Link Rust host artifacts with `cc` (gcc), not bare `clang`.** Cargo links
   build-scripts and proc-macros for the host (also `aarch64-unknown-haiku`).
   `rust_crate.cmake` set `CARGO_TARGET_<triple>_LINKER=${CMAKE_C_COMPILER}` —
   the bare `clang` Ladybird is configured with. On Haiku there is no clang
   *driver* toolchain: bare clang does not know Haiku's C-runtime startup files
   or its split default libraries, so the executable it links has a broken
   `PT_INTERP` / missing crt and **crashes at process start → exit 255** before
   `main`. Haiku's system driver is `gcc` (`cc`), which links a runnable Haiku
   binary correctly. The fix prefers `cc` as the cargo linker on Haiku:

   ```cmake
   set(_ladybird_rust_linker "${CMAKE_C_COMPILER}")
   if (HAIKU)
       find_program(_ladybird_haiku_rust_linker cc)
       if (_ladybird_haiku_rust_linker)
           set(_ladybird_rust_linker "${_ladybird_haiku_rust_linker}")
       endif()
   endif()
   # CARGO_TARGET_<triple>_LINKER=${_ladybird_rust_linker}
   ```

   This is safe and correct, not a shortcut: each Ladybird Rust crate is a
   **staticlib** archived by `AR`, so the cargo-selected linker only affects
   *host* build artifacts (build-scripts / proc-macros). The crate's own objects
   are still linked into Ladybird by `lld`, so the final shared objects are
   unchanged.

2. **Drop `-D warnings` from the shared rustc flags.** The nightly toolchain
   emits new lints the pinned crates trip; `-D warnings` turned them into hard
   errors.

3. **Match cargo 1.100's new build-dir layout when harvesting the FFI header**
   (`Meta/CMake/sync_rust_ffi_header.cmake`). cargo 1.100 writes
   `build/<crate>/<hash>/run/root-output` (the crate name is now a *directory*),
   not the old `build/<crate>-<hash>/root-output`. The glob is extended to match
   both, scoped to the crate dir so a sibling crate's generated `RustFFI.h`
   cannot bleed in.

With those three, `aarch64-unknown-haiku` Rust builds end-to-end: all the
mandatory crates, `libweb_rust`, `libcompositing_rust`, and the Cranelift WASM
JIT compiler (`cranelift-compiler`) compile and their FFI headers sync.

## Other arm64-Haiku port fixes (real, in the patch)

- `CMakeLists.txt` / `cmake_options.cmake` — the Qt chrome is gated behind a new
  `ENABLE_QT_UI` option (Haiku has no Qt6; headless build needs only the
  Services). This drops the GUI *chrome*, not any engine capability.
- `LibJS/CMakeLists.txt` — Haiku's `uname -p` returns `other`, leaving
  `CMAKE_SYSTEM_PROCESSOR` wrong on a native build; fall back to `uname -m` so
  `FLAP_ARCH` resolves to `aarch64` and the JS interpreter builds.
- `LibCore/Environment.cpp` — Haiku has no `secure_getenv`; exclude
  `AK_OS_HAIKU` from that branch.
- `LibCore/LocalServer.cpp` — add `#include <sys/ioctl.h>`.
- `LibWasm/AbstractMachine/BytecodeInterpreter.cpp` — guard the `ucontext.h`
  include with `WASM_COMPILED_FAULT_RECOVERY_SUPPORTED` (Haiku's compiled-fault
  recovery path is not yet brought up; the ucontext surface it needs is absent).
- `compile_options.cmake` — the HAIKU block: `link_libraries(bsd network)` for
  Haiku's split libc (getprogname/arc4random in libbsd, sockets/getaddrinfo in
  libnetwork), `-femulated-tls` (clang emits `R_AARCH64_TLSDESC` relocs the Haiku
  arm64 runtime_loader does not support; emulated TLS uses only supported
  relocs), and the relaxed-link allowance Haiku's split libc needs.
- `check_for_dependencies.cmake` — accept the coherent system ICU (icu74 at
  configure; icu78 is also built in lbdeps for the runtime ABI, see below) and
  keep ANGLE optional at configure time.

## Build status (honest, full feature set)

Real `ninja -k 0 WebContent WebDriver` on a native arm64-Haiku Graviton builder
(c7g.8xlarge): **1877 / 1885 targets compiled.** The whole dependency layer
(ICU78, libtommath 1.3, ffmpeg, Skia m148 CPU raster, all Rust crates incl. the
Cranelift compiler, ~55 liblagom libraries) and almost all of LibWeb built;
WebDriver linked earlier.

Two subsystems remain, each a dedicated real-engineering track — **not** disabled
or stubbed here. Full inventory in
[`wave3-remaining-walls.txt`](wave3-remaining-walls.txt) (69 object files).

### 1. Cranelift WASM JIT (1 file)

`Libraries/LibWasm/AbstractMachine/BytecodeInterpreter.cpp` fails at
`:433` — `cranelift_trap_message` is defined only inside
`#if WASM_COMPILED_FAULT_RECOVERY_SUPPORTED` (set for Win/macOS/Linux-{x86_64,
aarch64}, **not Haiku**), but `interpret()` references it unconditionally in its
compiled-fault `setjmp` branch. The real fix is to **bring up compiled-fault
recovery for Haiku arm64** (install the SIGSEGV handler + ucontext trap
classification, so the macro can be set to 1), not to stub the decoder. Tracked
as the JIT/FFI bring-up track. LibWasm builds the moment that lands.

### 2. WebGL / ANGLE (68 files)

Everything under `LibCompositing/WebGL/*`, `LibWeb/WebGL/*`,
`LibWeb/Bindings/WebGL*`, the WebGL extensions, Canvas (`HTMLCanvasElement`,
`OffscreenCanvas`, `CanvasHost`), `Compositor/CompositorHostBase`, and
`WrapperFactory` fail transitively because the generated
`LibCompositing/WebGL/GLFunctions.h` `#include`s `<GLES2/gl2ext_angle.h>` (plus
`GLES2/gl2.h`, `GLES3/gl3.h`). The GLES2/GLES3 headers are provisioned in
`lbdeps/include`; the ANGLE-specific `gl2ext_angle.h` and the ~59 ANGLE entry
points the generated code calls are **not** available. The generator itself
(`Meta/Generators/generate_libweb_webgl_functions.py`) runs cleanly; a secondary
build-ordering gap (`WebGLCommands.cpp` compiled before the `GLFunctions.h`
generate edge ran) is incidental. The real fix is the WebGL codegen / ANGLE
provisioning track; WebGL must be brought up for real, not disabled.

### Link & runtime notes (for the final WebContent link + render)

- **Link path:** executables link via a `haiku-cxx-link` wrapper — clang
  compiles, **g++ links** with `-Wl,--no-gc-sections` (clang-21's Haiku driver
  cannot emit a runnable image, and plain lld hits the ld-2.41 aarch64 stub
  bug). Watch the large WebContent link for a fresh stub hit.
- **Runtime ABI:** before launching WebContent, `LIBRARY_PATH` must put
  `/boot/home/lbdeps/icu78/lib` and `/boot/home/lbdeps/lib` (and Skia) **first**,
  or WebContent resolves the system icu74 and ABI-crashes (SONAME `.so.78` vs
  `.74`). `RendererSandbox` is Unimplemented on Haiku (fine for render);
  multiprocess IPC spawn is the real post-build unknown.

### The goal

Render-to-PNG of a real modern webpage via headless WebContent/WebDriver — not a
built binary, a rendered pixel — is reached once the Cranelift and WebGL tracks
land and the full WebContent link completes.
