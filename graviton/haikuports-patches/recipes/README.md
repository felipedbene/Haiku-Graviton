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
| `nodejs20-20.15.1.recipe` | yes (issue #93), **FULL — built native RC=0** | Node.js 20 (bundled V8) on arm64. The HaikuPorts recipe declares `cmd:python3` in `BUILD_PREREQUIRES`, which on the DeBeOS builder resolves to the default **python3.14**; Node 20's `configure` wrapper only accepts python 3.6–3.12 and hard-rejects 3.14 (`Please use python3.12 or … or python3.10 …`). Fix is **python-version selection only, no source/feature change**: `cmd:python3` → `cmd:python3.10` (mount 3.10 into the chroot; it is in the DeBeOS pool), and `BUILD()` calls `python3.10 configure.py …` + `make $jobArgs PYTHON=python3.10` so no unversioned `python3` (3.14) can leak into the gyp/V8 build. V8's arm64 backend and the in-patchset Haiku V8 port (`platform-haiku.cc`, pointer-compression + snapshot disabled on Haiku, gcc not clang) compose cleanly — build shows `-DV8_TARGET_ARCH_ARM64`, Torque links with `-lroot -lbsd -lnetwork`. **Not a feature cut.** Built on c7g.8xlarge (~39 min, `make $jobArgs`). Proven native RC=0: `nodejs20-20.15.1-2-arm64.hpkg` (11.5 MB, `cmd:node`+`cmd:corepack`, 36 MB `bin/node`) + `nodejs20_devel` (227 KB), harvested to the STAGING S3 pool. Post-install on the builder: `node --version`→`v20.15.1`, `node -e "2+2"`→`4`, fs read/write + http server+client roundtrips OK (`process.arch=arm64`, `platform=haiku`, V8 `11.3.244.8-node.23`, ICU `74.1`, OpenSSL `3.5.7`). Caveat: `Intl.NumberFormat('de-DE')` throws an ICU-data locale error — the linked `icu74_bootstrap` icudata lacks full locale data; basic Intl/`--with-intl=system-icu` links fine. No ISP (downloaded tarball) — operative copy is the tree recipe, no mtime pin |

| `opencc-1.1.4.recipe` | yes (issue #136), **FULL — built native RC=0** | CMake 4.x removed compatibility with `cmake_minimum_required(VERSION < 3.5)`; OpenCC's `CMakeLists.txt` predates 3.5, so configure aborts ("Compatibility with CMake < 3.5 has been removed"). `BUILD()` adds `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (same class as the `json_c` cmake4 fix). No feature change. Proven: `opencc-1.1.4-2-arm64.hpkg` (835 KB, `cmd:opencc`) + `opencc_devel`. |
| `polyclipping-6.4.2.recipe` | yes (issue #136), **FULL — built native RC=0** | same CMake < 3.5 policy removal; add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Proven: `polyclipping-6.4.2-3-arm64.hpkg` (`lib:libpolyclipping.so.22`) + `_devel` + `_debuginfo`. |
| `robin_map-0.6.3.recipe` | yes (issue #136), **FULL — built native RC=0** | same CMake < 3.5 policy removal; add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Header-only. Proven: `robin_map-0.6.3-2-any.hpkg` (installs `develop/headers`). |
| `recastnavigation-1.6.0.recipe` | yes (issue #136), **FULL — built native RC=0** | same CMake < 3.5 policy removal; add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Proven: `recastnavigation-1.6.0-1-arm64.hpkg` (198 KB, 5 shared libs) + `_devel`. |
| `portsmf-239.recipe` | yes (issue #136), **FULL — built native RC=0** | same CMake < 3.5 policy removal; add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Proven: `portsmf-239-3-arm64.hpkg` (`lib:libportSMF`) + `_devel`. |
| `primesieve-7.4.recipe` | yes (issue #136), **FULL — built native RC=0** | same CMake < 3.5 policy removal; add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Proven: `primesieve-7.4-2-arm64.hpkg` (106 KB) + `_devel`. |
| `qhull-8.0.2.recipe` | yes (issue #136), **FULL — built native RC=0** | same CMake < 3.5 policy removal; add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Proven: `qhull-8.0.2-3-arm64.hpkg` (608 KB) + `_devel` + `_debuginfo`. |
| `pystring-1.1.4.recipe` | yes (issue #136), **FULL — built native RC=0** | same CMake < 3.5 policy removal; add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Proven: `pystring-1.1.4-1-arm64.hpkg` (`libpystring.so.1.1.4`) + `_devel`. |
| `minisign-0.11.recipe` | yes (issue #136), **FULL — built native RC=0** | same CMake < 3.5 policy removal; add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Proven: `minisign-0.11-1-arm64.hpkg` (`bin/minisign`). |
| `mdate-1.7.0.3.recipe` | yes (issue #136), **FULL — built native RC=0** | the `installman` make target pipes the man page through `gzip`, which is not pulled into the haikuporter chroot (the recipe never declared it), so INSTALL exits 127 (`gzip: command not found`). Add `cmd:gzip` to `BUILD_PREREQUIRES`. No feature change. Proven: `mdate-1.7.0.3-1-arm64.hpkg` (90 KB). |
| `mm_common-1.0.6.recipe` | yes (issue #136), **FULL — built native RC=0** | two undeclared build tools: `configure` aborts "tar utility not found", and the `skeletonmm.tar.gz` generation step runs `gzip` (127). Add `cmd:tar` **and** `cmd:gzip` to `BUILD_PREREQUIRES`. GNOME C++-binding infra (unblocks the `*mm` tier). Proven: `mm_common-1.0.6-1-arm64.hpkg` (463 KB). |
| `musicpc-0.34.recipe` | yes (issue #136), **FULL — built native RC=0** | the ninja build succeeds; INSTALL then unconditionally `mv`s `share/doc/mpc`, which meson only emits when the optional docs are built, so INSTALL fails on `mv: cannot stat`. Guard the move (`[ -d ] && mv`). Program is complete; only the absent optional doc dir is skipped. Proven: `musicpc-0.34-2-arm64.hpkg` (`bin/mpc`). |
| `nesalizer-1.0~git.recipe` | yes (issue #136), **FULL — built native RC=0** | two defects: (1) the upstream Makefile hardcodes x86-only `-mfpmath=sse`/`-msse3` and `-flto`/`-fuse-linker-plugin` (Haiku's gcc has no LTO), which aarch64 gcc rejects — `BUILD()` strips all four on non-x86 arches (portability fix, no cut); (2) upstream has **no `install` make target**, so the stock recipe's `make ... INSTALL_DIR=` no-ops and shipped an empty 804-byte hpkg — INSTALL now copies `build/nesalizer` into `$binDir` explicitly. Proven: `nesalizer-1.0~git-1-arm64.hpkg` (59 KB, `bin/nesalizer` — was 804 B empty before the INSTALL fix). |

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

## `#136` s-z port-fix batch (CMake 4.x policy floor + a few small guards)

Seventeen previously-`failed` `missing-ports-136` recipes whose names start s-z were
converted to native RC=0 arm64 `.hpkg` and re-queued. Each is stored here **complete, as
actually built** on a native c7g builder (canonical `ami-04c29891a8fae17b5`, provisioned by
`haiku-provision-native-builder`, `haikuporter -j16`). All builds are proven by the harvested
`.hpkg` in `s3://haiku-graviton-<acct>-<region>/hpkg/arm64/`, not by an exit code.

The dominant blocker was uniform: **CMake 4.x removed the compatibility shim for
`cmake_minimum_required(<3.5)`**, so these ports failed at configure with *"add
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` to try configuring anyway."* The tooling-wide `#47`
default (in `haiku-provision-native-builder`, which appends the flag to haikuporter's shared
`cmakeDirArgs`) does **not** reach a recipe whose `BUILD()` calls `cmake` with its own
explicit argument list and never interpolates `$cmakeDirArgs` — which is the majority. So the
fix is per-recipe: the flag is added directly to the `cmake` invocation (all three subdir
calls, in `serious_sam`). This is the same one-liner `openal`/`libjxl` already carry; **not a
feature cut.**

Ports fixed by the policy flag alone: `squirrel`, `tidy`, `uchardet`, `zopfli`, `slack++`
(also uses `$cmakeDirArgs`, but its explicit list needed the flag too), `surgescript`,
`teeworlds`, `toluapp`, `unshield`, `sdl2_sound`, `serious_sam`, `vvvvvv`, `vc`.

Ports needing one additional small guard on top of the policy flag:

| Recipe | Extra fix | Why |
|---|---|---|
| `yaml_cpp0.7-0.7.0.recipe`, `yaml_cpp0.8-0.8.0.recipe` | `-DYAML_CPP_BUILD_TESTS=OFF` (0.7 also makes `rm test/gtest-*` tolerant) | the bundled test suite pulls an absent gtest submodule and its `binary_test.cpp` fails `-Wnarrowing`; the library itself is unaffected. Not a feature cut — tests are not shipped |
| `sais-1.6.3~git.recipe`, `sawteeth`* , `superfreecell-0.1.0.recipe` | `-DCMAKE_BUILD_TYPE=Release` | haikuporter's cmake wrapper aborts with *"invoking cmake without CMAKE_BUILD_TYPE specified!"* when the recipe passes none |

*`sawteeth` reached compile but then hit genuine `-Werror` source errors (narrowing + a
mismatched new/delete) that a flag alone did not clear — it is **not** in this batch; left as
a source-porting item.

> **Superseded for the CMAKE_BUILD_TYPE class (#136, playbook Class 8).** These
> per-recipe `-DCMAKE_BUILD_TYPE=Release` overlays are no longer the primary fix.
> The build type is now defaulted systemically in haikuporter's `cmake` wrapper by
> the provisioner (section 2a-4;
> `../haikuporter-cmake-build-type-default.patch`), which reaches every CMake port
> and does not depend on the input-source-package overlay landing with a pinned
> mtime. Four Class-8 ports (cmake_haiku, epoll_shim, sais, superfreecell) carried
> a correct overlay yet still failed the wave because that fragile path reverted
> the edit. The overlays remain valid and harmless — an explicit `Release` flows
> through the patched wrapper unchanged — but are no longer load-bearing.

Cross-links issue **#136**. Not merged pending human CR.

## Graviton3/4 ISA opt-in for ML/codec recipes (`_g3` / `_g4`)

A recipe that wants the Graviton3 (Neoverse-V1) or Graviton4 (Neoverse-V2)
instruction set — BF16, I8MM and SVE/SVE2, the paths ggml/llama.cpp and OpenBLAS
microkernels use — opts in **per-recipe**. The system baseline is not touched: it
stays `-mcpu=neoverse-n1+crypto` (`build/jam/ArchitectureRules`) so the OS and base
packages keep booting on Graviton2 and t4g.

The idiom, gated on arm64 in `BUILD()`:

```sh
case "$targetArchitecture" in
	arm64)
		debeosMcpu="-mcpu=neoverse-v1+crypto"   # neoverse-v2 for a _g4 variant
		export CFLAGS="$CFLAGS -O2 $debeosMcpu"
		export CXXFLAGS="$CXXFLAGS -O2 $debeosMcpu"
		;;
esac
```

`+crypto` is **re-carried** — a second `-mcpu` fully replaces the baseline's, and
crypto is never a compiler default. SVE is **not** suppressed: it is now enabled for
userland/EL0 with per-thread save/restore (commit `50f6a9be53`, #88), so a userland
ML port may emit it. (The older "SVE traps" note in
[`../codec-tier-arm64.md`](../codec-tier-arm64.md) predates that and is superseded.)
Such a build uses SVE + ARMv8.4 NEON that **faults on Neoverse-N1**, so it is a
non-default variant: give it a `_g3` / `_g4` name suffix and only ship it to
Graviton3+/Graviton4 targets — never as the plain fleet-portable package. Ports
needing only V1/V2 *scheduling* while staying fleet-portable use `-mtune=neoverse-v1`
(no ISA change, no suffix). Full rationale, the worked idiom, and the disassembly +
on-hardware proof owed before publishing a `_g3`/`_g4` package are in
[`../../docs/porting-playbook.md`](../../docs/porting-playbook.md) →
"Graviton3/4 ISA opt-in".

### `llama_cpp_g3-b4889.recipe` — the first `_g3` flavour (#331)

The concrete `_g3` variant of the baseline-NEON `llama_cpp-b4889` port, and the
worked example the convention above was written for. It is the same source and
patchset as the baseline plus (a) the `-mcpu=neoverse-v1+crypto` opt-in idiom in
`BUILD()` and (b) one extra patch that makes ggml's SVE paths build and run on
Haiku (guard `<sys/prctl.h>` off; read the SVE vector length with the `svcntb()`
intrinsic instead of `prctl(PR_SVE_GET_VL)`). `ARCHITECTURES="arm64"`, and it
`CONFLICTS` with the plain `llama_cpp` package — the two are the same commands
compiled for different ISAs, so a host installs one **or** the other.

Built and proven on a native Graviton3 (c7g): the shipped `libggml-cpu.so`
carries the I8MM/SVE kernels (172 `smmla`, 2782 SVE `z<n>` ops; the baseline has
none), `system_info` reports `MATMUL_INT8 = 1 | SVE = 1 | SVE_CNT = 32`, and
`llama-bench` on TinyLlama-1.1B Q4_0 shows **6.37x** prompt-eval throughput
(73.4 → 467.7 tok/s) over the baseline. Full numbers and the two portability
findings are in `../../docs/porting-playbook.md` → "First worked proof". **Ship
only to Graviton3+ images whose kernel enables EL0 SVE (#88)** — the `_g3` binary
SIGILLs where userland SVE is off. Built `.hpkg`s were staged, **not** published
to the green pool (held for human CR).

## `simde-0.8.2.recipe` — SSE/AVX→NEON translation for ports with no NEON path (#341)

A new (not upstream-derived) overlay recipe packaging **SIMDe** (SIMD Everywhere), the
header-only library that implements x86 SSE/AVX/AVX-512 intrinsics on top of NEON. It is
the drop-in for ports whose hot paths are written in x86 intrinsics and have no
hand-written NEON equivalent: build-depend on `devel:simde` and include `<simde/x86/…>`
instead of `<emmintrin.h>` etc. Header-only, so `ARCHITECTURES="any"` (cf. `robin_map`)
and no runtime dependency. The meson build installs 469 headers under
`develop/headers/simde/` and generates `simde.pc`; `-Dtests=false` keeps meson off the
large test/benchmark dependency graph (nothing is compiled for the install).

`v0.8.2`, sha256 `ed2a3268658f2f2a9b5367628a85ccd4cf9516460ed8604eed369653d49b25fb`
(tarball fetched, extracted, `SIMDE_VERSION_* = 0.8.2` in `simde/simde-common.h`
confirmed — the integrity rule). **Build status: review-verified + build-logic proven
locally** — the recipe's exact `meson setup … -Dtests=false` + `ninja install` was run
against the upstream `meson.build` and staged all 469 headers + a correct `simde.pc`
(`Version: 0.8.2`). All chroot prereqs (`cmd:meson`/`ninja`/`pkg_config`/`gcc`) are
already in the arm64 pool. A **native arm64 haikuporter `.hpkg` build is OWED** — for a
header-only package that compiles nothing, standing up a builder was judged
disproportionate. Cross-links `graviton/docs/port-hygiene.md` (the lint that recommends
it).

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

## Campaign #136 Class 3: gettext autoreconf / autopoint

The eleven `triage_class==3` ports of the #136 backlog. Each regenerates its
build system with `autoreconf` (or `./autogen.sh`) and uses gettext's macros in
`configure.ac`, but fails because the per-port chroot has neither `autopoint` nor
gettext's m4 macros on `ACLOCAL_PATH` — the macros install under
`/boot/system/data/gettext/m4`, not the default aclocal dir. See
`graviton/docs/porting-playbook.md#class-3-gettext-autoreconf--autopoint` for the
full analysis, including **why this class is per-recipe and not systemic** (both
halves of the fix — the `ACLOCAL_PATH` export and the `autopoint --force` before
`autoreconf` — are `BUILD()`-body actions that `scriptletPrerequirements` cannot
supply, and mounting the 16 MB gettext package into every chroot is the exact
thing #293 scoped out).

The systemic enabler is already in place: **#293 installs the full `gettext`
package on the builder host**, which is what makes a `cmd:autopoint` /
`cmd:gettext` prerequisite *resolvable* into the chroot. On the pre-#293
2026-09-15 AMI it was not, which is why the wave failed even for ports whose
overlay already declared it (libexif). These overlays therefore clear only on a
**rebake (#293 + these overlays) + re-wave** — build-proof is **OWED** (no builder
launched: an in-flight wave is running and native autoreconf builds are not a
quick check).

Ten follow the ACLOCAL_PATH + `autopoint --force` pattern; `rpcsvc_proto` is a
deeper variant fixed with a `PATCH()`.

| Recipe | Fix | Failure signature (from the wave log) |
|---|---|---|
| `aiksaurus-1.2.2~git.recipe` | `+cmd:autopoint +cmd:gettext`; ACLOCAL_PATH + `autopoint --force` before `./autogen.sh` | `possibly undefined macro: AM_NLS` |
| `axel-2.17.11.recipe` | `+cmd:autopoint`; ACLOCAL_PATH + `autopoint --force` before `autoreconf -fi` | `possibly undefined macro: AM_GNU_GETTEXT_VERSION` (m4/gettext.m4) |
| `dovecot-2.3.21.recipe` | `+cmd:autopoint`; ACLOCAL_PATH + `autopoint --force` | `possibly undefined macro: AC_LIB_PREPARE_PREFIX` (+ `AC_LIB_RPATH`/…) |
| `enca-1.19.recipe` | `+cmd:autopoint`; ACLOCAL_PATH + `autopoint --force` | `possibly undefined macro: AC_LIB_PREPARE_PREFIX` (m4/librecode.m4) |
| `libexif-0.6.22.recipe` | **strengthened** the #52 overlay: added ACLOCAL_PATH + `autopoint --force` | `Can't exec "autopoint": No such file` |
| `libmtp-1.1.22.recipe` | `+cmd:autopoint`; ACLOCAL_PATH + `autopoint --force` | `possibly undefined macro: AC_LIB_PREPARE_PREFIX` |
| `xcftools-1.0.7.recipe` | `+cmd:autopoint +cmd:gettext`; ACLOCAL_PATH + `autopoint --force` | `possibly undefined macro: AM_GNU_GETTEXT` + `AC_LIB_*` |
| `rpcsvc_proto-1.4.3.recipe` | `PATCH()` deletes configure.ac's dead `m4_ifndef` gettext-compat shim + redundant `AM_GNU_GETTEXT_VERSION`; `+cmd:gettext` | `autopoint: *** found more than one invocation of AM_GNU_GETTEXT_REQUIRE_VERSION` |

`libcddb-1.3.2`, `libggz-0.99.5`, `libhangul-0.1.0` already carry the full
pattern (committed 2026-09-15) and are unchanged here — they failed the wave only
because that AMI predated #293, and clear on the same rebake. `libggz`'s
`configure`-time `checking for msgmerge... no` is covered too: `cmd:autopoint`
resolves to the gettext package, which also provides `msgmerge`.

Cross-links issue **#136**. Not merged pending human CR; build-proof owed.

## `#337` video-encoding stack (x264 + minimal ffmpeg + x265; opus pristine)

The H.264/HEVC encode tier for native Graviton arm64, built and proven on a
native builder (`ssm-run` → `haiku-nativebuild`), harvested to the STAGING pool
(`s3://haiku-graviton-<acct>-<region>/hpkg/arm64/`, i.e. `build_state=built`,
**not** published to green). x264 is the hard prerequisite for the #95
remote-desktop x264 encode phase.

| Recipe | Change | Proof |
|---|---|---|
| `x264-20220222.recipe` | REVISION 4→5. Gate `cmd:nasm >= 2.13` in `BUILD_PREREQUIRES` to the x86 secondary arch only. x264's x86 SIMD is assembled with **nasm**; the **aarch64 SIMD path is assembled by the toolchain assembler (GAS, from binutils)**, so nasm is neither used nor needed on arm64 — and there is **no nasm package in the DeBeOS arm64 repo**, so the unconditional prereq made the port unresolvable. Not a feature cut. | Native RC=0: `x264-20220222-5-arm64.hpkg` (+ `_bin`/`_devel`/`_debuginfo`). `config.log`: `platform: AARCH64`, `asm: yes`; `config.h`: `HAVE_NEON 1`, `ARCH_AARCH64 1`. `x264 --version` → `0.164.x`, gcc 13.3.0. |
| `ffmpeg_x264-8.1.2.recipe` (+ `.patchset`) | **New minimal port.** FFmpeg 8.1.2 with `--enable-gpl --enable-libx264` and the native codecs; the large optional external-library closure the full `ffmpeg8` port pulls in (libass, dav1d, fdk-aac, vpx, webp, opus, vorbis, theora, …) is **not** enabled — none of it is published for arm64 yet. `--disable-avdevice` because its Haiku backend links the media_kit (`libmedia.so`), which the `@minimum` image does not ship. aarch64 SIMD is GAS-assembled, so no nasm prereq. Reuses the ffmpeg 8.1.2 source + Haiku patchset. Deliberately scoped as the x264 end-to-end proof, **not** a replacement for the full `ffmpeg8` port (that is a follow-up once its media-lib closure exists for arm64); it does not advertise the generic `lib:`/`devel:` av* resolvables. | Native RC=0: `ffmpeg_x264-8.1.2-1-arm64.hpkg` (11.6 MB). `ffmpeg -version` shows `--enable-libx264`; `objdump -p libavcodec.so` → `NEEDED libx264.so.164`; a `rawvideo → H.264 (libx264)` transcode of 30 frames exits 0 and `ffprobe` reports `h264 (Constrained Baseline)`. |
| `x265-3.5.recipe` | REVISION 9→10. Same nasm gating as x264 (x86-only). The recipe's pre-existing arm64 fixes (10/12-bit libs built without assembly; `dynamicHDR10` `ARM_ARGS` flattened to `-fPIC`) are unchanged. | Native RC=0: `x265-3.5-10-arm64.hpkg` (+ `_bin`/`_devel`/`_debuginfo`). `x265 --version` → `[HAIKU][GCC 13.3.0] 8bit+10bit+12bit`, `using cpu capabilities: NEON`; `CMakeCache`: `ENABLE_ASSEMBLY=ON`; a raw → HEVC encode of 30 frames exits 0. |
| `aom-3.12.1.recipe` | **New port, authored from scratch** — the AV1 codec deferred from the first #337 pass; HaikuPorts ships no `aom`. libaom 3.12.1, CMake+Ninja. Fixes: nasm gated to x86 (arm64 NEON is GAS-assembled); `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (#47); `-DCMAKE_SYSTEM_PROCESSOR=aarch64` (both `arm64`/`aarch64` map to aom's `arm64` NEON path, #71); `-fsigned-char` (#341); binary dir named `objdir` **not** `build` (aom keeps its CMake modules in the source tree's `build/cmake/`); the static `libaom.a` is moved aside so `prepareInstalledDevelLib` accepts the shared lib (draco pattern). **Portability fix (the real blocker):** aom's aarch64 CPU-detect (`aom_ports/aarch64_cpudetect.c`) has a probe only for Apple/Windows/Android/Linux/Fuchsia — none for Haiku, so `CONFIG_RUNTIME_CPU_DETECT` on `#error`s, and off, aom's rtcd statically binds the *highest* compiled Neon flavour (dotprod/I8MM/SVE), which would fault on a core lacking it. Built with `-DCONFIG_RUNTIME_CPU_DETECT=0` + `ENABLE_{ARM_CRC32,NEON_DOTPROD,NEON_I8MM,SVE,SVE2}=OFF` → pure baseline NEON, fleet-portable. Not a feature cut: NEON is the mandatory ARMv8-A baseline and stays fully enabled; the above-baseline extensions can only be used in a portable binary via runtime HWCAP dispatch, which needs getauxval (#329) — absent from the current builder AMI (hrev59996: no `sys/auxv.h`, no `getauxval`). **Follow-up:** once #329 is in the builder/fleet AMIs, reuse aom's `__linux__` getauxval branch on `__HAIKU__` (its hwcap bit layout already matches `headers/posix/sys/auxv.h`) for a single binary that lights up dotprod/I8MM/SVE at run time on Graviton3/4; a `_g3` `-mcpu=neoverse-v1` variant (#330) is a further step. | Native RC=0: `aom-3.12.1-1-arm64.hpkg` (2.15 MB, ships `bin/aomenc`+`bin/aomdec`+`lib/libaom.so.3`) + `aom_devel` (16.5 MB) + `aom_debuginfo` (12.3 MB). `objdump -d libaom.so.3.12.1`: 7855 NEON vector ops, **0** SVE, **0** dotprod/i8mm (portability confirmed). `aomenc --help`/`aomdec --help` exit 0; a 2-pass AV1 encode of 5×176×144 frames → valid IVF (`DKIF` magic) exit 0, and `aomdec` decodes it back to `YUV4MPEG2` exit 0. |

**opus** built native RC=0 from the **pristine** tree recipe (`opus-1.3.1`, no
DeBeOS change needed): `opus-1.3.1-2-arm64.hpkg` (+ `_devel`), `libopus.so.0.8.0`.
No overlay recipe is added for it (same rationale as `devil-1.8.0` above — a port
that needs no recipe change gets no overlay copy).

**Per-generation flavour status.** All four are the **baseline NEON, fleet-portable**
flavour (runs on every Graviton generation, G2/Neoverse-N1 and up). The per-gen
tuned `_g3`/`_g4` flavours (`-mcpu=neoverse-v1/-v2 +crypto`, SVE/SVE2) are a
follow-up via the #330 per-recipe ISA opt-in, and — per the playbook — owe a
disassembly + on-Graviton3/4-hardware check before publishing; not done here.

**AV1 (`aom`) now landed.** The libaom AV1 recipe (deferred from the first pass) was
authored from scratch and built native RC=0 — see the `aom-3.12.1.recipe` row above.
A follow-up can wire it into ffmpeg via `--enable-libaom` (the `ffmpeg_x264` recipe
already demonstrates the external-encoder link pattern; not rebuilt this pass).

**Remaining (deferred).** The full-codec `ffmpeg8` port is blocked on its
media-library closure (dav1d, fdk-aac, vpx, webp, vorbis, theora, soxr, openmpt,
gme, …) being built and published for arm64.

Cross-links issue **#337**. Not merged pending human CR; STAGING-harvested, not
published to green.
