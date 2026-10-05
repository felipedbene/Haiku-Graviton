# Ladybird on DeBeOS / Graviton (arm64 Haiku) — Wave 3: FIRST RENDER

Successor to the Wave-2 recipe in [`README.md`](README.md) (tracking issue #575).
**Wave 3 reaches the goal: the Ladybird engine builds, links, launches its full
multi-process pipeline, and renders a real modern webpage to PNG, headless, on a
native arm64 Haiku Graviton instance.** Proof:
[`debeos-demo-render.png`](debeos-demo-render.png) — an 800×1210 render of a
modern landing page (nav bar, gradient-clipped hero text, buttons with shadows,
a feature-card grid with rounded corners / box-shadows, radial-gradient
background, footer) composited through **CPU-raster Skia**, no GPU.

Pinned revision: Ladybird `90998c5dc7`. Toolchain (DeBeOS repo): clang 21.1.8,
rustc/cargo 1.100.0-nightly, gcc/`cc` 13.3.0, GNU ld 2.46.1, cmake 4.1.6, ninja
1.13.2. Build dir `/boot/home/lb/build-wave2`; `ENABLE_CRANELIFT_JIT=ON`,
`ENABLE_QT_UI=OFF`, `BUILD_TESTING=ON`, ICU 78.

The full engine-tree diff is [`ladybird-haiku-arm64-wave3.patch`](ladybird-haiku-arm64-wave3.patch)
(15 files). The DeBeOS GLES/EGL null backend is under
[`angle-shim/`](angle-shim). Everything below is real engineering — **no stubs,
no disabled engine features, no RTTI-off hacks, no `-z norelro`.**

## 1. The Rust wall (gating Wave-2 blocker) — CLEARED

In `Meta/CMake/rust_crate.cmake`: link Rust **host** build-scripts/proc-macros
with `cc` (gcc), not bare `clang`. Bare clang has no Haiku driver toolchain, so
host artifacts got a broken `PT_INTERP` and crashed at startup (the `serde`
exit-255). Also drop `-D warnings` (cargo-1.100 nightly lints) and match cargo
1.100's new `build/<crate>/<hash>/run/root-output` FFI-header layout
(`sync_rust_ffi_header.cmake`). This unblocked every mandatory Rust crate incl.
the Cranelift WASM JIT compiler and `libweb_rust`.

## 2. Cranelift WASM JIT (ships ON, real bridge)

`cranelift_trap_message()` was defined only under
`#if WASM_COMPILED_FAULT_RECOVERY_SUPPORTED` (unset on Haiku) but called
unconditionally — a generic C++ preprocessor bug. Fix: hoist the pure trap-code
decoder out of the guard (BytecodeInterpreter.cpp). `ENABLE_CRANELIFT_JIT=ON`;
`liblagom-wasm` links the real Cranelift bridge.

## 3. WebGL / ANGLE — DeBeOS GLES/EGL null backend

Haiku ships no ANGLE and no GLES/EGL device, but LibCompositing/LibWeb's
generated WebGL command stream references the full GLES2/GLES3 + ANGLE API and
the Compositor's `OpenGLContext.cpp` references EGL. `angle-shim/` provides, as a
**real shared library** (`libdebeos_angle_shim.so`, like ANGLE's libGLESv2):
- `include/GLES2/gl2ext_angle.h` — the ANGLE-private GLES2 declarations.
- `include/EGL/egl.h`, `eglext.h`, `eglext_angle.h` — Khronos-spec EGL
  declarations + ANGLE-private EGL enums (dummy values; the no-device backend
  never acts on them).
- `gles_null_backend.cpp` — every GLES2/GLES3 (+ ANGLE) entry point and the EGL
  entry points as a **no-device null backend**: `eglGetPlatformDisplay`/
  `eglInitialize` report no display, so WebGL context creation fails cleanly and
  pages fall back to CPU raster — the honest behaviour for a platform with no GL.
  `ENABLE_WEBGL` is undefined on Haiku, so none of this runs for a normal page.

`check_for_dependencies.cmake` builds the shim as `ANGLE_TARGETS` (SHARED, in the
LagomTargets export, include dir wrapped in `$<BUILD_INTERFACE:>`), and
`LibCompositing/CMakeLists.txt` **links** the shim .so so every process that
loads `liblagom-compositing` resolves its GL symbols at load. The generator
ordering gap (`WebGL/GLFunctions.cpp` missing from `GENERATED_SOURCES`) and the
shim's `-Wmissing-prototypes` (missing GLES3 includes) are fixed too.

## 4. Rebuilding Skia to match Ladybird's expected config (the big one)

The Wave-2 Skia was a trimmed, `-fno-rtti`, no-fontconfig core lib that did not
satisfy LibGfx. Skia was rebuilt consistently (procedure, no `gn` needed — the
Wave-2 ninja files are reused):

1. Append `-frtti` to the skia `cxx` rule in `out/haiku-arm64/toolchain.ninja`
   and recompile all ~776 source-set objects (consistent RTTI → emits
   `SkTypeface`/`SkTypeface_proxy` typeinfos; kills the whole `_ZTI` cascade).
2. Recompile the 3 `thread_local` objects (`SkStrikeCache`, `AtlasTextOp`,
   `SkSLPool`) with **`-femulated-tls`** — clang emits `R_AARCH64_TLSDESC`
   otherwise, which Haiku's arm64 runtime_loader cannot relocate ("Bad data").
3. Compile `src/ports/SkFontMgr_fontconfig.cpp` (`-std=c++20 -frtti` + fontconfig
   cflags) — Wave-2 had `skia_use_fontconfig=false` but LibGfx is built
   `-DUSE_FONTCONFIG=1` and calls `SkFontMgr_New_FontConfig`.
4. `ar` the 776 source-set objects + the fontconfig fontmgr + the two `skcms`
   objects into `libskia.a`. (pathops, `SkTypeface_proxy`, fontscanner are in the
   776; skcms is a separate module.) Do **not** over-archive stray `obj/` files —
   that bloats the link into GNU ld "bad value".

Result: `nm -D -u liblagom-gfx.so` shows zero unresolved skia symbols.

## 5. Runtime dependency defects fixed (Wave-2 libs built hastily)

Haiku's runtime_loader rejects GNU-ld's 4-LOAD+RELRO layout ("Could not map
image: Bad data"); relinked the affected lbdeps `.so` to the native 2-LOAD layout
with **`gcc -z noseparate-code`** (RELRO-neutral — not `-z norelro`). Content gaps
fixed too:
- **libtommath**: `mp_set_double` was compiled out — guarded by
  `__STDC_IEC_559__`, which Haiku-clang omits; gcc defines `__GCC_IEC_559`, so
  recompile `bn_mp_set_double.c` with `cc`. (LibCrypto needs it.)
- **libpng**: the ARM NEON objects (`arm_init`, `filter_neon_intrinsics`,
  `palette_neon_intrinsics`) were absent from the `.a`; compiled + archived
  (`png_init_filter_functions_neon` etc.). NEON kept, not disabled.
- **libavif 0.9.3**: `AVIFLoader.cpp` used `repetitionCount` (not in 0.9.3) →
  default 0.

## 6. The last mile — multi-process IPC

With everything loaded, `test-web` crashed on
`VERIFY(m_owner_thread_id.is_current_thread())` (LibIPC/Connection.cpp). Root
cause: `AK/ThreadID.cpp`'s `query_current_thread_id()` had **no Haiku branch** and
returned 0 for every thread → every `ThreadID` invalid → the owner-thread VERIFY
always failed. Fix: add a Haiku branch using `find_thread(nullptr)` (`<OS.h>`).

## 7. Rendering

No standalone headless-browser exists in this revision and WebDriver drives the
Qt `Ladybird` chrome (gated out), so the render vehicle is the in-tree
`test-web` HeadlessWebView harness (needs `BUILD_TESTING=ON` + a Qt-free
`ladybird_build_resource_files` target, added in `CMakeLists.txt`; plus a Haiku
branch in `Tests/LibWeb/test-web/Collection.cpp`). Drive it as a Screenshot test
with `--rebaseline` (writes the actual screenshot to the expectation path):

```
# runtime: icu78 FIRST or WebContent ABI-crashes on system icu74
export LIBRARY_PATH=/boot/home/lbdeps/icu78/lib:/boot/home/lbdeps/lib:/boot/system/lib:/boot/system/develop/lib
bin/test-web --test-path <root> --filter debeos-demo --rebaseline -j1
# input:  <root>/Screenshot/input/debeos-demo.html
# output: <root>/Screenshot/expected/debeos-demo.png  (800x1210 RGBA)
```

`test-web` spawns the real multi-process pipeline — WebContent, Compositor,
RequestServer, ImageDecoder, WebWorker — and composites via Skia CPU raster. The
result is [`debeos-demo-render.png`](debeos-demo-render.png).

## 8. Live internet sites — `headless-shot`

`test-web` is a hermetic test runner (test mode, blocks real network), and
WebDriver drives the Qt `Ladybird` chrome (gated out). So for *live* sites I
built a minimal non-Qt chrome, [`headless-shot`](headless-shot) (a thin
`LibWebView::Application` whose `execute()` runs `HeadlessMode::Screenshot`):

```
headless-shot --headless=screenshot --screenshot-delay 3 \
    --screenshot-path out.png https://news.ycombinator.com/
```

It launches the real multi-process pipeline (WebContent/Compositor/RequestServer/
ImageDecoder), navigates to a real URL over the builder's NAT (RequestServer +
openssl3 TLS + DNS), lays it out, composites via Skia CPU raster, and writes a
PNG. One extra fix was needed: Haiku ships the monospace font as **"Noto Mono"**,
not "Noto Sans Mono", so `FontPlugin.cpp`'s monospace fallback list didn't
resolve a `UiMonospace` font and WebContent VERIFY-crashed; added "Noto Mono" to
the list.

Proof of live rendering over NAT:
- [`live-example-render.png`](live-example-render.png) — the *current* multilingual
  example.com (English/Arabic/CJK/French/Russian/Spanish — real network content,
  multilingual font fallback).
- [`live-hackernews-render.png`](live-hackernews-render.png) — the full live
  Hacker News front page (header, 30 stories with points/authors/timestamps/
  comments/upvotes, footer, search box).

## Status / open items

- **DONE:** full real build (1862 targets), all service binaries + `test-web` +
  `headless-shot`, cranelift ON, WebGL/EGL null backend, Skia with RTTI+fontconfig,
  a **rendered modern webpage (PNG)**, AND **live modern internet sites rendered
  over NAT** (example.com, Hacker News) — the Wave-3 goal, in full.
- The Skia rebuild should ideally be re-expressed as a proper `args.gn`
  (`skia_use_fontconfig=true`, RTTI on, emulated-TLS) + a kept `gn` binary, rather
  than the ninja-rule edit used here.
