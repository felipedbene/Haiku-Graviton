# Ladybird browser on DeBeOS / Graviton (arm64 Haiku) — Wave 2

Tracking issue: #575. This directory holds the reproducible recipe for building
the [Ladybird](https://github.com/LadybirdBrowser/ladybird) browser engine
natively on DeBeOS (arm64 Haiku) on AWS Graviton, with a **CPU-raster Skia**
backend (no GPU/GL/Vulkan) and **APNG-capable libpng**.

Pinned revisions (what this recipe was validated against):

- Ladybird `74cbb799abae7ae01c50b242628f91c7c09a49c1`
- Skia `chrome/m148`, commit `e7c90ecca9444fe09598f1630ab7cee2c0ee027a`
  (the milestone Ladybird's `vcpkg.json` pins: `skia 148`)
- Toolchain: clang 21.1.8, cmake 4.1.6, ninja 1.13.2, gn 2385, rustc/cargo 1.100,
  gcc/`cc` 13.3.0, GNU ld 2.46.1 — all from the DeBeOS package repo.

## Status

| Step | Result |
|------|--------|
| Skia m148 CPU-raster built for arm64-Haiku (GN) | **DONE** — `libskia.so` + skcms + modules; installed with a real `skia.pc` (Version 148). `pkg-config skia=148` resolves. |
| libpng APNG blocker | **DONE** — libpng 1.6.53 rebuilt with the APNG patch; `png_get_acTL` et al. verified present in the library (not just the header). |
| Full Ladybird configure (nothing unresolved) | **DONE** — clean configure, 0 CMake errors: `Found skia, version 148`, `LIBPNG_HAS_APNG - Success`, `Build files have been written`. |
| Build toward headless first paint | **BLOCKED** on an ICU version skew (see below). Driven through the AK / LibCrypto / LibTextCodec+LibUnicode (Rust + C++) layers first. |

## The pieces

Run on a native arm64 Haiku Graviton builder, in order:

1. Wave-1 recipe (toolchain + devel libs + FastFloat/simdutf/wuffs + `libav*-ladybird`
   pkg-config shims into `/boot/home/lbdeps`).
2. `provision-wave2.sh` — host tools (gzip/tar/patch/expat_devel), a `python3`
   symlink for Skia's GN, and **libtommath 1.3.0** (the DeBeOS 1.2.0 lacks
   `mp_expt_n` that LibCrypto needs, and its `.pc` has a wrong prefix).
3. `build-libpng-apng.sh` — APNG-enabled libpng into `/boot/home/lbdeps`.
4. `build-skia.sh` — Skia m148 CPU-raster into `/boot/home/lbdeps` (+ `skia.pc`).
   Apply `skia-haiku-arm64.patch` to the Skia checkout first.
5. Apply `ladybird-haiku-arm64.patch` to the Ladybird checkout.
6. `configure-wave2.sh` — the full, clean CMake configure.
7. `ninja -C build-wave1 test-web` (the headless render harness).

## Platform patches

### Skia (`skia-haiku-arm64.patch`) — Haiku has no Skia port

Skia is built with `target_os="linux"`; Haiku is classified `SK_BUILD_FOR_UNIX`
through `__unix__`, which is the correct minimal surface. Three fixes:

- `third_party/{freetype2,harfbuzz}/BUILD.gn`: Skia's `system()` targets hardcode
  `/usr/include/<lib>`; rewrite to Haiku's `/boot/system/develop/headers/<lib>`.
- `BUILD.gn`: drop `libs += [ "dl" ]` — Haiku provides `dlopen` in `libroot`,
  there is no separate `libdl`.
- `src/ports/SkMemory_malloc.cpp`: Haiku `libroot` has no `malloc_usable_size`;
  report the requested size instead.

### Ladybird (`ladybird-haiku-arm64.patch`)

- `Meta/CMake/check_for_dependencies.cmake`:
  - relax `find_package(ICU 78.3 EXACT ...)` to the coherent system ICU;
  - make ANGLE optional (empty `ANGLE_TARGETS` when absent — CPU raster needs no GLES);
  - resolve **avif** through pkg-config instead of `find_package(LIBAVIF)` — the
    system libavif CMake config computes a non-existent `/boot/system/include`
    (Haiku headers live under `develop/headers`); the `.pc` is correct.
- `Libraries/LibJS/CMakeLists.txt`: `uname -m` fallback for `FLAP_ARCH` (Haiku's
  `uname -p` is "other", so `CMAKE_SYSTEM_PROCESSOR` isn't recognised).
- `CMakeLists.txt`: `DEBEOS_HEADLESS_ONLY` builds Services (WebContent) + the
  headless render path but skips the Qt UI (no Qt6 for arm64), with a stand-in
  `ladybird_build_resource_files` target so the headless tests still configure.
- `Meta/CMake/compile_options.cmake`: add Haiku to the relaxed-link branch
  (`--allow-shlib-undefined -z undefs`). Haiku's `libnetwork.so` carries
  dangling single-underscore resolver symbols (`_res_init`, …) that resolve only
  at runtime; the strict `-z defs --no-undefined --no-allow-shlib-undefined`
  default rejects them. (The build also links `-lbsd` for `arc4random_buf` /
  `getprogname`, which Haiku puts in `libbsd`.)
- `Meta/CMake/rust_crate.cmake`: set the cargo linker driver to `cc` (gcc), **not**
  clang. The Haiku clang 21 driver does **not** emit a `PT_INTERP` for PIE
  executables (even with an explicit `-Wl,--dynamic-linker`), so cargo-run host
  build-script binaries fail Haiku's `runtime_loader` with "Could not map image:
  Bad data". gcc links them correctly.
- `Meta/CMake/sync_rust_ffi_header.cmake`: also match cargo ≥ 1.100's nested
  build-dir layout (`<crate>/<hash>/run/root-output`) when locating the
  cbindgen-generated `RustFFI.h`; the old glob only matched `<crate>-<hash>/`.

## The remaining wall — ICU 74 vs ICU 78

The build compiles AK, LibCrypto, the Rust crates (LibTextCodec, LibUnicode, …)
and reaches **`Libraries/LibUnicode/Calendars`**, where it fails:

```
AdjustedEraCalendar.h: error: non-virtual member function marked 'override'
  hides virtual member function      (handleGetExtendedYear(UErrorCode&),
                                      handleComputeMonthStart(..., UErrorCode&))
AdjustedEraCalendar.h: error: abstract class is marked 'final'
AdjustedEraCalendar.cpp: error: allocating an object of abstract class type
```

Ladybird HEAD's `icu::Calendar` subclasses use the virtual signatures that gained
a trailing `UErrorCode&` in **ICU 76+** (Ladybird's `vcpkg.json` pins ICU 78.3).
DeBeOS ships **ICU 74** (hard-pinned transitively by `harfbuzz_devel`'s
`devel:libicuuc`), whose `Calendar` vtable has the old signatures — so the
overrides don't match, the subclasses stay abstract, and they can't be built.

This is a genuine source-level API-version requirement, and ICU-78 usage is
pervasive across LibUnicode/LibJS `Intl`, so patching individual calendars would
not unblock the build. Resolving it properly needs either:

- an **ICU 78** in the DeBeOS repo, with harfbuzz/… rebuilt against it (a
  larger package-landscape change, out of scope for this wave); or
- a Ladybird revision whose vcpkg pinned an ICU-74-era release.

Everything up to and including a full, clean configure and the entire non-ICU
build graph (toolchain, Skia integration, Rust, APNG) is in place; the ICU bump
is the single outstanding dependency.
