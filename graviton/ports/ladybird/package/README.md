# Packaging Ladybird for DeBeOS arm64

How the natively-built Ladybird engine becomes installable `.hpkg` packages, and
why the dependency split is what it is. The general native-packaging workflow is
in [`graviton/docs/packages/native-package-creation.md`](../../../docs/packages/native-package-creation.md);
this document only records what is specific to Ladybird.

Upstream reference point: Ladybird `90998c5d`. There are no release tags, so the
package version follows the haikuports git-snapshot convention:
`ladybird-0~git90998c5d-1-arm64.hpkg`.

## Two scripts

| Script | Produces |
|---|---|
| `build-dep-packages.sh` | the five dependency pool packages (`icu78`, `libtommath`, `simdjson`, `libpng16`, `ffmpeg7`) |
| `build-package.sh` | `ladybird-0~git90998c5d-1-arm64.hpkg` |

Both run on the builder, against the CMake build tree (`bin/`, `lib/`,
`share/Lagom`) and the external dependency prefix the build used. Run
`build-dep-packages.sh` first — `build-package.sh`'s `requires` are written
against the versions it produces.

## Package layout, and why it is one self-contained prefix

Everything Ladybird owns lands under `apps/Ladybird`:

```
apps/Ladybird/bin/     headless-shot WebContent Compositor RequestServer
                       ImageDecoder WebWorker WebDriver MediaServer
                       WasmCompiler cranelift-compiler
apps/Ladybird/lib/     liblagom-*.so.0*  libdebeos_angle_shim.so
apps/Ladybird/share/Lagom/   fonts icons themes ladybird/{about-pages,templates,
                             site-compatibility,ladybird.css,utils.js}
bin/ladybird-headless-shot   wrapper
bin/ladybird-webdriver       wrapper
```

Three things make that layout the right one, and all three are properties of
code rather than preference:

1. **`$ORIGIN` already works.** The binaries come out of CMake with
   `DT_RPATH = $ORIGIN:$ORIGIN/../lib`, and Haiku's `runtime_loader` searches
   `DT_RPATH`/`DT_RUNPATH` *before* `LIBRARY_PATH` and before the system paths
   (`src/system/runtime_loader/runtime_loader.cpp`, `open_executable()`), with
   `$ORIGIN` resolved against the requesting object. So `bin/` finds `../lib`
   with no relink and with no environment variable. That matters because on
   Haiku `LIBRARY_PATH` *replaces* the loader path instead of prepending to it —
   telling a user to export it would drop `/boot/system/lib` and break the
   binary outright. rpath is the correct mechanism here, not a convenience.
2. **Ladybird computes its own prefix.** `platform_init()` sets the resource
   root to `find_prefix(application_directory()) + "share/Lagom"`, and
   `get_paths_for_helper_process()` looks in `<prefix>/libexec` then
   `<prefix>/bin` then the application directory
   (`Libraries/LibWebCommon/WebView/Utilities.cpp`). `bin/` + `share/Lagom`
   under one prefix is literally what the code expects; nothing is patched to
   make the package work.
3. **`bin/` entry points are wrappers, not symlinks.** `$ORIGIN` must resolve
   against the real binary's directory; `exec`ing the absolute path guarantees
   it regardless of how the wrapper was invoked.

`liblagom-*` are Ladybird's own libraries and stay inside the package. Nothing
in the package is installed into `/boot/system/lib`.

`build-package.sh` also rewrites `DT_RPATH` with `patchelf --set-rpath` to drop
the build host's absolute `/boot/home/lbdeps/lib`, which CMake had added to some
targets. That only *shortens* the existing string, so the LOAD/RELRO segment
layout — which Haiku's loader is strict about — is untouched.

## The dependency split

The closure is taken mechanically: `readelf -d` over **every** shipped binary
and library, union the `NEEDED` names, subtract what the package ships, and
resolve each remainder to the package that `provides lib:<name>`. The resulting
set is 72 `NEEDED` names, 71 supplied by the package, 37 external.

> A caution worth leaving in writing: the first pass of this got `libavif.so.13`,
> `libSDL3.so.0` and the three `libbrotli*` wrong — not because the method is
> weak but because the dump was read through `tail`, which silently cut every
> name sorting before `libbsd`. The missing `requires` only surfaced as a
> `runtime_loader: Cannot open file libavif.so.13` on the clean box. Read the
> whole closure, or compute the difference in a file.

### Satisfied by existing repository packages

`haiku` (libroot, libnetwork, libbsd) · `gcc_syslibs` (libstdc++, libgcc_s) ·
`openssl3` · `curl` · `libfmt` · `fontconfig` · `harfbuzz` · `libjpeg_turbo` ·
`mimalloc` · `libpsl` · `sqlite` · `libwebp` · `woff2` · `libxml2` · `zlib` ·
`libavif` · `brotli` · `libsdl3`

### Needed new pool packages

These five diverge from what the repository shipped. They are **not** bundled
privately — each is a proper package and `ladybird` requires it normally — but
each needed a deliberate decision about blast radius first:

| Package | Change | Why | Blast radius |
|---|---|---|---|
| `icu78` 78.3-1 | net-new | the engine links ICU 78; the system ships icu74 and the SONAMEs differ (`libicuuc.so.78` vs `.74`) | none — the repository already carries nine coexisting ICU runtimes, this is the tenth, and `icu74` keeps serving its consumers |
| `libtommath` 1.3.0-1 | in-place bump from 1.2.0 | 1.2.0 does not export `mp_expt_n()`, which LibCrypto calls (verified with `nm -D`) | only reverse dependency in the repository is `libtommath_devel` |
| `simdjson` 5.0.2-1 | in-place bump from 3.11.5 | the engine links simdjson 5; SONAME moves `libsimdjson.so.24` → `.so.34` | zero reverse dependencies |
| `libpng16` 1.6.53-**2** | revision bump | LibImageDecoders hard-requires APNG; revision 1 exports no `png_*_acTL`/`fcTL` at all, so `liblagom-imagedecoders` cannot load against it | ~70 consumers — see below |
| `ffmpeg7` 7.1-1 | net-new | LibMedia links `libswresample`; nothing in the repository provided `lib:libavcodec` or `lib:libswresample` (`ffmpeg_x264` is a command-line package) | none |

`requires` names a *capability* wherever possible (`lib:libicuuc >= 78`,
`lib:libtommath >= 1.3`, `lib:libsimdjson >= 34`, `lib:libswresample >= 5`) so
the old versions cannot satisfy it. libpng is the exception: APNG is additive
inside one SONAME version, so there is no `lib:` version that distinguishes the
two revisions, and the requirement has to be on the package —
`libpng16 >= 1.6.53-2`. A future upstream bump to 1.6.54 would satisfy that
expression without necessarily carrying the patch; the APNG recipe is the thing
that has to be kept, not the comparison.

### Nothing to ship

`simdutf`, `fast_float` and `wuffs` are linked statically (or are header-only),
and `skia` is a static `libskia.a` baked into `liblagom-gfx`. None appears in
any `NEEDED`, so none needs a package. Their licences are bundled in the
`ladybird` package instead, which is where the obligation actually lands.

## libpng16 revision 2 — the part that needed real work

The first APNG build was linked **without** a symbol-version script, so it
carried no `PNG16_0` version node while the repository's revision 1 does. That
is not cosmetic: `freetype`, `pngfix` and `PNGTranslator` all record
`File: libpng16.so.16 / Name: PNG16_0` in `.gnu.version_r`, and
`elf_symbol_lookup.cpp:113-131` rejects a versioned request against an
unversioned image of the same name. In practice it *appeared* to work only
because `equals_image_name()` compares the on-disk basename
(`libpng16.so.16.53.0`) against the Verneed file name (`libpng16.so.16`), they
differ, and the loader therefore falls through to "some other image, accept the
symbol" — past a branch whose own comment reads *"TODO: That should actually be
kind of fatal!"*. Shipping to ~70 consumers on the strength of that is not a
decision anyone should make on purpose.

So revision 2 is rebuilt with autotools and the version script
(`--enable-arm-neon=api`, `LDFLAGS=-Wl,-z,noseparate-code`), which gives a
library that is a genuine ABI superset:

- version definitions: `libpng16.so.16` (BASE) + `PNG16_0`, same as revision 1;
- exported base names: 257 in revision 1, **279** here, **0 missing**; the 22
  added are exactly the APNG entry points (`png_get_acTL`, `png_set_acTL`,
  `png_get_next_frame_*`, `png_read_frame_head`, `png_write_frame_{head,tail}`,
  `png_set_progressive_frame_fn`, …) plus the two Haiku ABI markers;
- `PNG_ARM_NEON_OPT 1` with the NEON filter routines present (and local, exactly
  as in revision 1 — the version script hides internals);
- 2 LOAD segments, which is what Haiku's `runtime_loader` accepts (its default
  GNU ld 4-LOAD + RELRO layout is rejected with "Bad data");
- all 32 `png_*` symbols `liblagom-imagedecoders` imports are satisfied, and
  Ladybird records no version requirement of its own, so it takes the
  unversioned-request path and binds to the base version.

`-Wl,-z,noseparate-code` is RELRO-neutral and is the documented fix for the
Haiku loader's layout check. `-z norelro` is **not** an acceptable substitute.

## Verification

`package create` succeeding proves nothing. The bar is a *fresh* canonical
instance — no build tree, no external dependency prefix, empty `LIBRARY_PATH` —
and three checks on it:

1. **Negative control first.** Installing `ladybird` alone must fail:
   `nothing provides lib:libicuuc>=78 needed by ladybird-0~git90998c5d-1`. If it
   installs, the `requires` are decorative.
2. **Live render.** Install all six from local files, then
   `ladybird-headless-shot --headless=screenshot --screenshot-delay 5
   --screenshot-path out.png https://news.ycombinator.com/` and *look at the
   PNG*. No `LIBRARY_PATH`, nothing on the box but installed packages.
3. **libpng16 consumer regression.** An existing pool consumer must still run
   against revision 2 — `pngfix` on a real PNG, byte-identical output before and
   after the bump — on top of the mechanical superset proof above, which is what
   actually covers all ~70 consumers rather than the one that was executed.
