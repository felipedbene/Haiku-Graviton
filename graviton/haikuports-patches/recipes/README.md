# Recipes — the source of truth

These are the **complete recipe files** as actually built, not diffs. They are
authoritative for two reasons:

1. **The sibling `.patch` files are explanatory notes, not reliable inputs to
   `patch(1)`.** Measured with `patch -p1 --dry-run -f` over all eight:
   `gettext-1.0-groff-doc-cut-stage1.patch` and
   `zstd-1.5.6-makefile-not-cmake-stage1.patch` are **not parseable at all** —
   `patch: **** Only garbage was found in the patch input.` The other six do parse a
   hunk header, so they are not malformed in that way, but none has been verified to
   apply cleanly to a pristine recipe and several carry prose above the diff plus
   hand-written hunk headers. Read the patches for the reasoning; **apply the
   recipes.**
2. **`/opt/haiku/haikuports` on the builder is edited in place** (it already carries
   the perl fix), so it is *not* a valid pristine reference to diff against. Some
   recipes here are therefore stored **unmodified**, purely to pin exactly which
   upstream text was used.

## Where these go on a guest

`input-source-packages/develop/sources/<port>-<version>-<revision>/<port>-<version>.recipe`

**That path overrules the ports tree silently.** All the ports in this chain have an
input source package, so editing the ports-tree copy has no effect and produces no
warning. Check the `<source-package>::` line in the haikuporter log if unsure.

After installing a recipe, **pin its mtime forward of the source package**, because
haikuporter re-extracts (and therefore silently reverts your edit) whenever
`mtime(recipe) <= mtime(sourcePackage)`:

```sh
touch -d @$(( $(stat -c %Y "$srcpkg") + 172800 )) "$recipe"
```

Compute it from the package's own mtime rather than the wall clock — the clock bug is
fixed, but this makes the step independent of it either way.

## What is here

| Recipe | Modified? | Why |
|---|---|---|
| `perl-5.42.2.recipe` | yes | `LIBRARY_PATH` replaces rather than prepends the loader path |
| `libtool-2.5.4.recipe` | yes | drop `./bootstrap`, flatten source mtimes (Blocker 1) |
| `tar-1.35.recipe` | yes | `--with-included-regex`, the hanging gnulib run test (Blocker 4) |
| `autoconf-2.72.recipe` | **stale — cut retired** | kept as the record of the doc cut. The shipping package is now built from the **pristine** recipe with real `makeinfo`; see Blocker 6 |
| `gettext-1.0.recipe` | yes (issue #26) | drops `cmd:groff` + `cmd:makeinfo` from `BUILD_PREREQUIRES`. Both only feed doc output the build never installs (MAN2HTML man-page HTML; texinfo `.info` regeneration behind `--disable-maintainer-mode`) and drag in a heavy texinfo/netpbm chain; the release tarball ships the prebuilt man/`.info`, and the `make` calls force `AUTOCONF/AUTOMAKE/AUTOHEADER/ACLOCAL=:`. gettext/libintl is a keystone (grep/findutils/nano/gawk/diffutils/tar need `devel:libintl`), so this keeps it buildable without first standing up the doc toolchain. Not a feature cut — the `.info`/man that ship in the tarball still install. Also strips the stale auto-appended `# Added by haikuporter:` tail that pinned `SOURCE_URI` to a local `gettext_source_rigged-*.hpkg` (present only on the original build box), which made the committed recipe unbuildable on a fresh builder; the real `ftpmirror.gnu.org` `SOURCE_URI` is restored. Proven native RC=0 with **both `groff` and `makeinfo` uninstalled**: `gettext-1.0-1` + `gettext_libintl` + `gettext_libintl_devel` (provides `devel:libintl`) + `gettext_devel` + `gettext_doc` |
| `zstd-1.5.6.recipe` | **stale — cut retired** | kept as the record of the Makefile-instead-of-cmake cut. Now built from the **pristine** cmake-based recipe |
| `cmake-4.1.6.recipe` | **NO — unmodified** | cmake needs no change at all once `expat`/`rhash`/`libuv`/`curl` exist. Stored to pin the exact upstream text, since the builder's tree is not a pristine reference |
| `curl-8.21.0.recipe` | yes | `--without-libpsl` on every arch; `libpsl` → `libidn2` → `cmd:gtkdocize`, unbuildable here |
| `libxml2-2.15.3.recipe` | yes (issue #32), **FULL — built native RC=0; stage-1 cut RETIRED** | removes the `pythonModuleEnabled=false` stage-1 override so the arm64 primary builds the Python binding again. libxml2 2.15's `configure.ac` requires `doxygen` **only** when `--with-python` (or `--with-docs`) is set — it drives the API-description doc build the binding needs; `--with-docs` is off by default and the tarball ships pre-built `dist-doc/`, so the C library + `devel:libxml2` never needed doxygen. The reason the cut existed was that `doxygen` "failed on arm64"; it now builds cleanly and installs from the DeBeOS repo (`pkgman install cmd:doxygen` → 1.14.0), which is the whole blocker for #32. `cmd:doxygen` stays in `BUILD_PREREQUIRES` and is now satisfiable. **No feature cut.** Proven native RC=0: `libxml2-2.15.3-1` (1.67 MB) + `libxml2_devel` (provides `devel:libxml2 = 16.1.3`, `cmd:xml2_config`) + `libxml2_python3.14` + `libxml2_doc`. `devel:libxml2` is the shared `BUILD_REQUIRES` unlock for the font/GUI tier (glib2, harfbuzz, cairo, pango, fontconfig). Tree recipe |
| `ruby-3.2.9.recipe` | yes | adds an `__aarch64__` arm to `signal.c`'s `mcontext_t` read — ruby took the x86 `esp`/`ebp` names on every non-amd64 Haiku and failed to compile. **A portability fix, not a cut**: the handler keeps working and the x86 lines are untouched |
| `jam-2.5_2021_10_29.recipe` | yes | `INSTALL()` ran `install -v bin.haikux86/g/jam`, but jam builds into `bin.$OSPLAT` and that is plain `bin.haiku` on an architecture whose spelling its Jamfile does not know — arm64 among them. Now locates the binary under `bin.haiku*/g/jam` and fails loudly if there is none, instead of being silently x86-only. **A portability fix, not a cut.** Has no input source package, so the operative copy is the tree one at `sys-devel/jam/` and no mtime pin applies. Proven: `jam-2.5_2021_10_29-3-arm64.hpkg` |
| `libsdl2-2.32.10.recipe` | yes, **still needed** | the DeBeOS `@minimum` `haiku` package ships **no `libmedia.so` or `libgame.so`** (media_kit + game_kit dropped), so SDL2's Haiku native audio (`BSoundPlayer`) and joystick (`BJoystick`) backends cannot link. `BUILD()` now `-DSDL_AUDIO=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF`, seds `media game` out of the Haiku `EXTRA_LIBS`, and stubs the lone `set_mouse_position()` call (a `<game/WindowScreen.h>` symbol) in `src/{video,main}/haiku/SDL_BApp.h`. Video / render (`libGL`) / events / timers / filesystem are intact; native audio, joystick and relative-mouse re-centering are lost until the media/game kits exist. Proven: `libsdl2-2.32.10-3-arm64.hpkg` installs and `dlopen`s. No input source package — operative copy is the tree one at `media-libs/libsdl2/`, no mtime pin |
| `openal-1.21.1.recipe` | yes, **still needed** | base package only — drops the `_tools` subpackage (the `alsoft-config` Qt5 GUI) and its `Qt5`/`libsndfile` `BUILD_REQUIRES` (neither is in this image), so `-DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF`, and `-DALSOFT_BACKEND_HAIKU=OFF` because the Haiku output backend links the absent `libmedia` (null/wave backends remain). Also `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`: the guest's CMake is 4.x, which removed pre-3.5 compatibility (same class as the `json_c` cmake4 policy fix). Proven: `openal-1.21.1-5-arm64.hpkg` installs and `dlopen`s. No input source package — operative copy is the tree one at `media-libs/openal/`, no mtime pin |
| `libavif-0.9.3.recipe` | yes, **still needed** | `PROVIDES_devel` had `devel:libavif` **commented out** (a bootstrap-era defect) so `libavif_devel` shipped only `libavif_devel` and no consumer could resolve `devel:libavif`. Uncommented it. Now provides `devel:libavif = 13.0.0`. Uses `libavif-0.9.3` (dav1d decoder) deliberately, NOT `libavif1.0-1.4.2`, which pulls `rav1e` → `cmd:cargo` (no Rust toolchain in the guest — hide `libavif1.0-1.4.2.recipe` so the resolver can't pick it). Runtime-requires `lib:libdav1d>=7.0.0`, so **dav1d must also be in the repo**. Proven: built + published; `dlopen(libavif.so.13)` resolves. Tree recipe, no mtime pin |
| `sdl2_image-2.8.12.recipe` | yes, **still needed** | `--disable-jxl` + dropped `devel:libjxl`/`lib:libjxl` — `libjxl-0.6.1` does not compile on arm64 (see below), so JXL is cut; AVIF/JPEG/PNG/TIFF/WebP loaders retained. Proven: `sdl2_image-2.8.12-2-arm64.hpkg` built + published. Tree recipe, no mtime pin |
| `harfbuzz-14.2.0.recipe` | yes, **FULL — built native RC=0** | **docs-only cut**, keeps gobject + introspection. `-Ddocs=disabled` and drop the `pygments_$pythonPackage` build-req + `cmd:gtkdoc_scan` build-prereq (gtk-doc/docbook are not in the repo; docs are HTML, not a functional feature), and guard the INSTALL `[ -d $prefix/share ] && cp -R $prefix/share/* $docDir \|\| true` (docs-off leaves no `share/`). **gobject + introspection stay ENABLED** — builds `harfbuzz_glib` + the `HarfBuzz-0.0.gir`/`.typelib`. Built full on the native EC2 builder once glib2/gobject-introspection were built. (Supersedes the earlier over-cut that disabled gobject/introspection — that was only needed on the broken QEMU guest.) Built + published: `harfbuzz-14.2.0-1` (+ `_devel`, `_glib`). Deps: freetype/graphite2/glib2/gobject_introspection + `icu74_devel` (only `74.1_bootstrap` in the pool). |
| `libjxl-0.6.1.recipe` | yes, **INCOMPLETE / blocked** | added `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (bundled `third_party/sjpeg` needs CMake&lt;3.5 compat, removed in CMake 4.x) — that clears configure, but the build then **fails to compile on arm64 NEON**: `libjxl-0.6.1`'s SIMD code calls `MinOfLanes`/`MaxOfLanes`/`InterleaveUpper` with signatures the guest's newer `libhwy`(1.4.0) NEON API no longer provides. Needs a newer libjxl or an older libhwy; not resolved. Tree recipe |
| `mesa-25.3.6.recipe` | yes, **still needed** | issue #139: mesa builds only once `llvm21` exists (its sole prior blocker). The recipe already build-prerequires `python3.10` + `mako_python310`/`pyyaml_python310`, but mesa's upstream `meson.build:939` does `find_program('python3', 'python', ...)` and picks the **system default** `python3`, which on this image is **3.14** — 3.14 has no `distutils` module and there are no `mako_python3.14`/`pyyaml_python3.14` packages, so meson dies at `meson.build:966` ("One of Python (3.x) packaging or distutils module is required"). `BUILD()` now creates a `$sourceDir/.pythonshim/python3 -> python3.10` symlink and prepends it to `PATH`, so meson and every python generator it drives run under 3.10 (which has `distutils` + the declared mako/pyyaml). **Not a feature cut — no `-Dgallium-drivers`/`-Dvulkan-drivers` change**; llvmpipe + lavapipe (swrast) stay as the recipe declares. Separately required a repo fix outside this recipe: the repo shipped `python3.10-3.10.20` but its `mako_python310`/`pyyaml_python310`/`*_python310` build helpers require `cmd:python3.10>=3.10.21`, so `python3.10-3.10.21-1-arm64.hpkg` was built native (from `dev-lang/python/python3.10-3.10.21.recipe`) and installed to close the skew. Also fixes a second, arm64-specific INSTALL defect: mesa names the Vulkan ICD manifest after the target CPU (`lvp_icd.aarch64.json` on arm64), but the upstream-derived recipe hardcoded the x86 name `lvp_icd.x86_64.json` in both the path-fixup `sed` and `packageEntries lavapipe`, so INSTALL died with `sed: can't read .../lvp_icd.x86_64.json: No such file or directory` after a fully successful compile+link (ninja `[1134/1134]`, 0 FAILED). Now resolves the manifest by glob (`lvpIcd=$(ls .../lvp_icd.*.json)`). Proven: built native RC=0, all 5 hpkgs produced — `mesa-25.3.6-1-arm64.hpkg` (metapackage, `provides mesa` + `lib:libglapi`, `requires lib:libllvm>=21.1.8`), `mesa_eglvnd` (libEGL_mesa.so, 18.9 MB), `mesa_lavapipe` (libvulkan_lvp.so + `lvp_icd.aarch64.json`), `mesa_devel`, `mesa_debuginfo`. Tree recipe, no mtime pin (no ISP for mesa) |
| `tk-8.6.10.recipe` | yes, **FULL — built native RC=0** | issue #52: four independent blockers, all portability/build-system fixes, no feature cut. (1) `SOURCE_DIR` carried a **trailing slash** (`androwish-c48f047f5b/jni/sdl2tk/`); haikuporter's subdir-fold does `os.rename(sourceDir + '/' + subdir, …)` and Haiku's `rename()` rejects a trailing-slash source path with `B_BAD_VALUE` — dropped the slash. (2) `BUILD()` overrode `CFLAGS` with only a `-D`, tripping haikuporter's `runConfigure: Must specify optimization flags when overriding CFLAGS` guard (same class as x264) — added `-O2`. (3) AndroWish's bundled AGG 2.4 declares a local `char* tags;` but this FreeType's `FT_Outline.tags` is `unsigned char*`, so `tags = outline.tags + first;` is an `invalid conversion` error under strict gcc — `sed` retypes the local to `unsigned char*` (a read-only tag byte, safe). (4) the AGG `libagg.a:` make target has **no prerequisites**, so a reused work dir (from a failed prior attempt) leaves `libagg.a` with only the `src/` objects — the `font_freetype` append is skipped — and `libtk8.6.so` then fails to link `agg::font_engine_freetype_base::{height,transform,load_font,flip_y}`; `BUILD()` now `rm -f libagg.a agg-2.4/*/*.o` before the final `make` to force a complete archive. Deps: `tcl`/`tcl_devel`, `libsdl2_devel` (provides `devel:libsdl2`), `freetype`, `zlib` — all from the DeBeOS repo. Proven: `tk-8.6.10-4-arm64.hpkg` (5.6 MB) + `tk_devel`. Operative copy is the ports-tree recipe (editing `dev-lang/tk/tk-8.6.10.recipe` took effect — no ISP override observed). |
| `graphicsmagick-1.3.40.recipe` | yes, **FULL — built native RC=0** | issue #52: `coders/jpeg.c` uses the IJG jpeg-9 SmartScale/lossless API (`jpeg_decompress_struct.process`, `JPROC_PROGRESSIVE`/`JPROC_LOSSLESS`, `jpeg_simple_lossless()`) inside `#ifdef D_LOSSLESS_SUPPORTED` / `#if defined(C_LOSSLESS_SUPPORTED)`. **libjpeg-turbo 3.1 also defines `C_/D_LOSSLESS_SUPPORTED`** (in `jmorecfg.h`) but ships a *different* lossless API (`jpeg_enable_lossless`, no `.process`/`JPROC_*`), so those branches compile against symbols that do not exist. `BUILD()` `sed`s both guards to also require `&& !defined(LIBJPEG_TURBO_VERSION)` (turbo advertises `LIBJPEG_TURBO_VERSION` in `jconfig.h`), which falls through to GraphicsMagick's own upstream non-lossless-libjpeg `#else` path. **Not a feature cut** — baseline + progressive JPEG read/write stay fully enabled; only the IJG-lossless codec (which libjpeg-turbo genuinely cannot provide via this API) is declined. Proven: `graphicsmagick-1.3.40-2-arm64.hpkg` (4.4 MB) + `_devel` + `_debuginfo`. Tree recipe. |
| `libgit2_1.9-1.9.1.recipe` | yes, **FULL — built native RC=0** | issue #52: libgit2's `CMakeLists.txt` sets `CMAKE_C_STANDARD 90` **and** forces `option(CMAKE_C_EXTENSIONS … OFF)`, so it compiles with strict `-std=c90` while defining `_GNU_SOURCE`. `_GNU_SOURCE` activates Haiku's `gnu/sched.h` `_DEFAULT_SOURCE` cpuset block, which uses `static inline` — but `inline` is not a keyword in strict ISO C90, so every TU that pulls in `pthread.h` fails with `sched.h:45: expected ';' before 'unsigned'`. `BUILD()` passes `-DCMAKE_C_EXTENSIONS=ON`, which switches the compile to `-std=gnu90` (verified: `gcc -std=c90` reproduces the error, `-std=gnu90` compiles clean) — `inline` becomes a keyword and the header compiles. C90 semantics are otherwise unchanged; **not a feature cut.** Proven: `libgit2_1.9-1.9.1-1-arm64.hpkg` + `_devel` + `_tools` (`cmd:git2`) + `_debuginfo`. Tree recipe. |
| `tmux-3.7c.recipe` | yes, **FULL — built native RC=0** | issue #135: `tmux-3.1c` (2020, the only recipe haikuports has ever carried) fails with a `closefrom` return-type conflict against Haiku's `bsd/unistd.h`. Bumped to current upstream `3.7c` (still no newer haikuports recipe to sync from — checked, only `3.1c` exists there too); that alone does **not** clear the conflict, because it is a live Haiku defect, not something upstream tmux "reconciled": `bsd/unistd.h` declares `int closefrom(int lowFd)` once `_DEFAULT_SOURCE` is implied (true here, since tmux's `configure` sets `_GNU_SOURCE`), but no such symbol actually links on Haiku, so tmux's own `void closefrom(int)` compat fallback (`compat/closefrom.c`) still gets compiled and its `compat.h` prototype collides with Haiku's in any file that includes both. `BUILD()` now retypes tmux's own fallback to `int` (matching Haiku's declared, if unimplemented, prototype) via `sed`/`cat` before `autoreconf` — same real close-every-fd behavior, just agreeing return types. Verified by compiling `client.c` standalone before wiring into the real build. The old haikuports Haiku-support patchset (`forkpty-haiku.c`, `osdep-haiku.c`, platform detection, `-lnetwork` `b64_ntop` fallback) is dropped, not carried forward — upstream tmux absorbed all of it natively by `3.7c`, so the patchset no longer applies and would be a no-op if it did. Also adds `cmd:yacc` to `BUILD_PREREQUIRES` (`cmd-parse.y` is new since 3.1c; `bison` in the repo provides it). Built + published: `tmux-3.7c-1-arm64.hpkg`; installed and smoke-tested, `tmux -V` reports `tmux 3.7c`. Tree recipe, no mtime pin |
| `coreutils-9.11.recipe` | yes (issue #27), **FULL — built native RC=0** | adds `cmd:perl` to `BUILD_PREREQUIRES`. coreutils generates `src/dircolors.h` with `$(PERL) -w -- src/dcgen src/dircolors.hin` (see `src/local.mk`), but the upstream recipe never declared `cmd:perl`, so on a builder without perl on PATH the `GEN src/dircolors.h` step exits 127 — the "dircolors.h Error 127" of #27. The other #27 symptom, `ln: failed to create hard link ...: Operation not allowed`, is a **non-fatal** gnulib `configure` probe (`checking whether rename manages hard links correctly`): Haiku returns EPERM for that specific same-name hard-link test, configure records the result and continues, and the build completes. Only the missing `cmd:perl` was fatal. **Not a feature cut** — every program builds. Proven: `coreutils-9.11-2-arm64.hpkg` (3.09 MB) + `_debuginfo`. Tree recipe, no mtime pin |
| `llvm21-21.1.8.recipe` | yes (issue #71), **FULL — built native RC=0** | `BUILD()` replaces the vendored `llvm/cmake/config.guess` with a two-line script emitting `aarch64-unknown-haiku` (arm64 only). LLVM's `config-ix.cmake` calls `get_host_triple()` **unconditionally**, and `GetHostTriple.cmake` `message(FATAL_ERROR)`s if that config.guess exits non-zero — which the 2024-06-07 vendored copy does on Haiku arm64 (it doesn't recognise `uname -m = arm64`). Unlike autotools ports, LLVM consumes config.guess **raw**, never running `config.sub` — which on this box already canonicalises `arm64-unknown-haiku` → `aarch64-unknown-haiku` — so `-DLLVM_HOST_TRIPLE` alone does **not** help (get_host_triple still runs and still aborts). Scoped to `effectiveTargetArchitecture = arm64`; other arches keep the vendored script. **Not a feature cut.** Unblocks `llvm21` and therefore `mesa` (#139). **Build-verified**: full llvm21 build RC=0 (ninja 6633/6633, 9 hpkgs) on c8g.24xlarge; the loose compiler-rt/openmp patches `git apply`-verified against the checksum-matched source. Tree recipe, no mtime pin |

## `ruby-3.2.9.recipe` does NOT go through the ISP path — read this before delivering it

Every other recipe here is delivered by `prepguest.sh`, which copies
`recipe-overlay/*.recipe` into `$ISP/develop/sources/<stem>-*/` and pins the mtime.
**Ruby has no input source package at all** (`ls input-source-packages/ | grep -i ruby` is
empty, where the `perl` control returns `perl_source_rigged-…hpkg`), so that loop finds no
directory, prints `no ISP source dir`, and **skips it silently**. Ruby's operative recipe is
the tree copy:

```
/boot/home/haikuports/dev-lang/ruby/ruby-3.2.9.recipe
```

Copy it there directly, and **do not** pin an mtime — there is no source package for the
mtime comparison to be made against.

This file was harvested from guest run15 on 2026-08-25 (md5
`e7d6af3271f8448b650472c210e0a74a`, 159 lines) after living **only inside that one guest's
filesystem** since 2026-08-24 18:42 — absent from `recipe-overlay/`, from this directory and
from git. A guest re-seed would have destroyed it and it would have been hand-derived a
second time. That is exactly the loss this directory exists to prevent, so the rule is worth
restating: **the moment a recipe edit is proven to work in a guest, harvest it. A working
edit that lives only on a guest disk is not saved.**

The `.patch` form in the parent directory (`ruby-3.2.9-arm64-mcontext.patch`) is a
**description, not an appliable patch** — its hunk header is `@@ BUILD()` rather than
line-numbered. Use this recipe, not that file.

The edit is **proven**: on run15 the `sed` landed (`signal.c` carries both `mctx->x[29]` and
the untouched `mctx->esp`), ruby compiled and linked, and the build then reached `miniruby`,
which panicked the guest kernel with the `mprotect` defect. So the compile blocker is closed
and the *only* thing between here and a ruby package is a guest running a kernel with the
`Query()` fix — see `graviton/docs/arm64-mprotect-query-present.md`.

## Two recipes here are kept only as history

`autoconf-2.72.recipe` and `zstd-1.5.6.recipe` describe cuts that have been
**retired** — the packages in `hpkg-out/arm64/` are now built from pristine recipes.
They stay because the reasoning is worth keeping and because a future chroot change
could make either cut necessary again. **Do not install them onto a guest** expecting
current behaviour.

## Campaign #136 quick-win port fixes (names a–e)

Ten failed `#136` ports whose failure was a single missing `BUILD_PREREQUIRES`
`cmd:` (the build chroot only mounts declared prerequisites, so a recipe that
shells out to an undeclared tool dies with `command not found` — the same class
as the `#27` `coreutils` `cmd:perl` fix), a CMake build-type/policy mismatch, a
missing autotools `--install`, a `config.guess` that could not name the host, or
a toolchain that lacks the LTO plugin. Each fix is a portability/build-system
correction, **not** a feature cut; all were proven native `RC=0` on an arm64
builder (hpkg exists) and harvested to the pool.

| Recipe | Fix | Failure signature |
|---|---|---|
| `convmv-2.05.recipe` | `+cmd:gzip` in `BUILD_PREREQUIRES` | `Makefile` manpage target pipes `pod2man` to `gzip`: `gzip: command not found` (Error 127) |
| `atari++-1.81.recipe` | `+cmd:gzip` | `make install` gzips the man page: `gzip: command not found` (Error 127) |
| `digiclock-1.0.recipe` | `+cmd:unzip` | `INSTALL()` runs `unzip fatelk`: `unzip: command not found` |
| `autotrace-0.40.0_20230301.recipe` | `+cmd:which` | `autogen.sh` uses `which pkg-config`; `which: command not found` → `*** No pkg-config found ***` |
| `blobwars-2.00.recipe` | `+cmd:msgfmt +cmd:pkg_config` | `msgfmt -o locale/ca.mo`: `msgfmt: No such file or directory` (Error 127) |
| `epoll_shim-0.0.20230411.recipe` | `-DCMAKE_BUILD_TYPE=RelWithDebInfo` → `Release` | haikuporter's cmake wrapper: `invoking cmake with -DCMAKE_BUILD_TYPE=RelWithDebInfo without debug info packages specified` |
| `cmake_haiku-git.recipe` | `+-DCMAKE_BUILD_TYPE=Release +-DCMAKE_POLICY_VERSION_MINIMUM=3.5` | bare `cmake .` → `invoking cmake without CMAKE_BUILD_TYPE specified`; then `Compatibility with CMake < 3.5 has been removed` (same class as the openal/libjxl policy floor) |
| `draco-1.5.6.recipe` | `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` → `OFF` | native gcc has no LTO plugin: `cc1plus: error: LTO support has not been enabled in this configuration` / `'-fno-fat-lto-objects' are supported only with linker plugin` |
| `bonnie++-2.00a.recipe` | `autoreconf` → `autoreconf -fi` | `configure.in: error: required file 'install-sh' not found; try running autoreconf --install` |
| `autoconf2.71-2.71.recipe` | bare `./configure $configureDirArgs` → `runConfigure ./configure` | `configure: error: cannot guess build type; you must specify one` — the sibling `autoconf-2.72.recipe` already uses `runConfigure` (which passes the host triple), so this mirrors the working pattern |

`devil-1.8.0` (media-libs) was also requeued failed→built in this pass with **no
recipe change**: its `#136` failure was a transient dep-resolution oscillation in
the batch tooling, not a recipe defect — all of its `devel:` prerequisites
(`jasper`, `libmng`, `tiff>=6`, `lcms2`, …) are in the pool, so it builds clean
once they are installed. No overlay recipe is needed for it.

`betterspades-0.1.6~git` was NOT fixed: its two shallow blockers (`cmd:unzip`,
`RelWithDebInfo`) were cleared, but it then fails at configure because its
`src/CMakeLists.txt` `FetchContent_MakeAvailable(cglm)` tries to build a vendored
`cglm` at configure time and that step fails — real porting work (package `cglm`
or de-vendor it), out of scope for a quick win.
