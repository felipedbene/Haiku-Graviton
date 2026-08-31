# WebPositive on HaikuWebKit for arm64 — scoping and de-risking

Status: **BUILT AND RENDERS (2026-08-25)** — HaikuWebKit 1.10.0 compiles with GCC on
arm64 and WebPositive renders JavaScript plus modern CSS (grid, flexbox, gradients,
transforms), screenshot-verified. The "GO (staged) / nothing has been built" verdict
this document was written to reach is therefore **superseded**: the build took ~55 min
(the estimates below ran 3-5x pessimistic). Still open: `example.com`'s body does not
paint, and no image has been baked or promoted with it yet.
Date of original scoping evidence: 2026-08-25.

The current browser on arm64 is `netsurf-3.11` (built, works, screenshot-verified).
It has no modern JS engine and no modern CSS, which is why this exists. Firefox is
out (`firefox-154.0.recipe` declares `ARCHITECTURES="x86_64"` and lives under
`repository/.skipped/`); `bezilla` is a ~2010 Gecko. The native path is
**WebPositive on HaikuWebKit**, and `haikuwebkit-1.10.0.recipe` already declares
`ARCHITECTURES="all !x86_gcc2"` — arm64 is *supported*, not opt-in gated.

Throughout: **VERIFIED** = read out of a file or measured here. **INFERRED** =
reasoned from verified facts. **NOT MEASURED** = I could not check it, and say so.

---

## 1. The `llvm_config >= 21` verdict: it is dead weight. No second LLVM.

`haikuwebkit-1.10.0.recipe` carries:

```
# Note: llvm_ar is the command needed from llvm, not llvm_config, but only llvm_config
# has a version constraint in current llvm packages.
BUILD_PREREQUIRES="
	...
	cmd:llvm_config >= 21
	...
```

**That constraint is residue from a reverted Clang experiment, and the current
GCC build never touches LLVM at all.** Four independent lines of evidence:

**(a) Provenance — the recipe history says so.** Two upstream commits, read via the
public recipe-tree history:

| commit | date | what it did |
|---|---|---|
| `ace5bc103` | 2026-07-28 | *"Attempt build with Clang."* Added `cmd:clang_21` **and** `cmd:llvm_config >= 21`; added `PATCHES=`; set `CC=clang`/`CXX=clang++`; passed `--lto-mode=none -DUSE_HEADER_MAPS=OFF -DUSE_SYSTEM_MALLOC=ON`. |
| `5b5295d02` | 2026-08-06 | *"Revert changes for 1.10 (but update patch)."* Removed `cmd:clang_21`, removed `CC`/`CXX`, removed `PATCHES=`, restored the GCC `BUILD()`. **Left `cmd:llvm_config >= 21` behind.** |

The version constraint and the compiler that needed it were added in the *same
commit*; the revert dropped the compiler and forgot the constraint. A later commit
(`163520dcb`) renamed the patchset to `haikuwebkit-1.10.0-clang.patchset` purely so
the porting tool would stop flagging an unreferenced patch — confirming the Clang
work is parked, not active. VERIFIED.

**(b) The current `BUILD()` is GCC-only.** It passes `-ftrack-macro-expansion=0`
and `--param ggc-min-expand=10` — both GCC-only flags that Clang rejects — and
`BUILD_PREREQUIRES` lists `cmd:gcc` and no `cmd:clang`. VERIFIED.

**(c) The source tree never invokes `llvm-ar`.** I downloaded
`haikuwebkit-1.10.0.tar.gz` (39,161,678 bytes) and grepped the whole unpacked
tree (25,111 files, 240,841,370 bytes). The **only** hit for
`llvm-ar` / `llvm-ranlib` / `llvm_ar` anywhere is:

```
Source/cmake/OptionsMSVC.cmake:49:    find_program(CMAKE_RANLIB NAMES llvm-ranlib-20 llvm-ranlib REQUIRED)
```

— the MSVC / clang-cl Windows path, unreachable on Haiku. VERIFIED.

**(d) The archiver is auto-detected and GNU `ar` already qualifies.** The thin-archive
machinery the comment alludes to is in `Source/cmake/OptionsCommon.cmake` (lines
111–171 of the shipped tarball). It probes `${CMAKE_AR} -V` and accepts *either*
variant:

```cmake
if (AR_VERSION MATCHES "^GNU ar ")
    set(AR_VARIANT BFD)
    set(AR_SUPPORTS_THIN_ARCHIVES TRUE)
elseif (AR_VERSION MATCHES "(^|[ \t])LLVM ")
    set(AR_VARIANT LLVM)
    set(AR_SUPPORTS_THIN_ARCHIVES TRUE)
```

`CMAKE_AR` comes from the GCC toolchain (binutils `ar`), reports `GNU ar`, and gets
thin archives with no LLVM present. And LTO — the other reason to want `llvm-ar` —
is gated on Clang: `WebKitCompilerFlags.cmake:354` reads
`if (LTO_MODE AND COMPILER_IS_CLANG AND NOT MSVC)`. Under GCC, `-flto` is never
added, so there are no LLVM bitcode members for GNU `ar` to choke on. VERIFIED.

### The fix, and its cost impact

Drop the line from our recipe fork:

```diff
 BUILD_PREREQUIRES="
 	cmd:bison
 	cmd:cmake
 	cmd:flex
 	cmd:gcc$secondaryArchSuffix
 	cmd:gperf
-	cmd:llvm_config >= 21
 	cmd:m4
```

If you want belt-and-braces instead of removal, replace it with unversioned
`cmd:llvm_ar`: `llvm12-12.0.1.recipe:77` provides `cmd:llvm_ar` with **no** version
constraint (only `cmd:llvm_config` at line 84 is versioned, as
`= 12.0.1 compat >= 12`), so `llvm12` would satisfy it. But removal is correct —
the build does not use it.

**Cost avoided:** an entire second LLVM source build (`llvm21-21.1.8`). For scale,
`llvm12` is the single largest item in the in-flight desktop closure. **This is the
biggest single saving in this plan.**

**Bonus finding — WebKit does not need mesa or llvm12 either.** `haikuwebkit`'s GL
requirements are `devel:libgl` and (from `OptionsHaiku.cmake:47`,
`find_package(OpenGLES2 REQUIRED)`) `devel:libglesv2`. The already-built
`libglvnd_devel-1.7.0-4-arm64.hpkg` provides both — verified by reading the built
package's metadata:

```
provides: devel:libgl = 1.0.0 (compatible >= 1)
provides: devel:libegl = 1.1.0 (compatible >= 1)
provides: devel:libglesv2
```

So **the WebKit build is fully decoupled from the mesa/llvm12 chain.** mesa still
matters for accelerated rendering at runtime, but it is not a build gate. VERIFIED.

### Confidence

The provenance (a) and the source grep (c) are hard evidence. The claim "a GCC build
will therefore configure and link cleanly with no LLVM installed" is **INFERRED**
from (b)(c)(d) — strong, but it is a prediction until measured. It is cheap to
falsify: the CMake configure prints

```
Archiver variant in use: BFD
  Archiver supports thin archives - TRUE
```

Check that line in the first two minutes of the smoke build (§6, Phase B). If it
says `UNKNOWN`, stop and reconsider — not before.

---

## 2. Dependency gap

Checked against the 267 built arm64 packages, and against the recipe tree.

### Already satisfied by what is built (no action)

| Requirement | Satisfied by |
|---|---|
| `haiku`, `haiku_devel` | `haiku.hpkg`, `haiku_devel.hpkg` |
| `gcc_syslibs`, `gcc_syslibs_devel`, `lib:libatomic` | `gcc_syslibs*-13.3.0_..._bootstrap-1` |
| `lib:libcurl`, `devel:libcurl` | `curl-8.21.0-1` / `curl_devel` |
| `lib:libgl`, `devel:libgl`, `devel:libglesv2` | `libglvnd-1.7.0-4` / `libglvnd_devel` |
| `lib:libicuuc >= 74`, `devel:libicuuc >= 74` | `icu74-74.1_bootstrap-1` / `icu74_devel` — see note below |
| `lib:libjpeg`, `devel:libjpeg` | `libjpeg_turbo-3.1.4.1-1` |
| `lib:libpng16`, `devel:libpng16` | `libpng16-1.6.53-1` |
| `lib:libsqlite3`, `devel:libsqlite3` | `sqlite-3.50.4.0-1` |
| `lib:libssl`, `devel:libssl >= 3` | `openssl3-3.5.7-1` |
| `lib:libxml2`, `devel:libxml2` | `libxml2-2.15.3-1` |
| `lib:libz`, `devel:libz` | `zlib-1.3.2-1` |

**ICU version constraint is met.** Reading the built package's metadata directly:
`icu74_devel-74.1_bootstrap-1-arm64.hpkg` provides
`devel:libicudata`, `devel:libicui18n`, `devel:libicuio`, `devel:libicuuc`, all
`= 74.1 (compatible >= 74)`, and ships the `unicode/` headers plus the
`libicu*.so.74.1` symlinks. `OptionsHaiku.cmake:41` asks for
`find_package(ICU REQUIRED COMPONENTS data i18n uc)` — all three are present.
`>= 74` is satisfied. VERIFIED.
Caveat: the package ships only `icu-uc.pc` in `pkgconfig`, not `icu-i18n.pc` /
`icu-data.pc`. WebKit's `FindICU.cmake` uses `find_library`, and every other
`Find*.cmake` in the tree calls `pkg_check_modules(... QUIET ...)` as a hint only,
so this should not break discovery — INFERRED, low risk. A non-bootstrap
`icu74-74.1.recipe` (`REVISION="6"`) exists in the tree if ICU data or collation
turns out short; treat it as optional, not blocking.

### Build-tools: all fourteen prerequisites are already built

`cmd:bison` (`bison-3.8.2-1`), `cmd:cmake` (`cmake-4.1.6-1`), `cmd:flex`
(`flex-2.6.4-4`), `cmd:gcc`, `cmd:gperf` (`gperf-3.1-1`), `cmd:m4` (`m4-1.4.19-1`),
`cmd:make`, `cmd:ninja` (`ninja-1.13.2-3`), `cmd:perl` (`perl-5.42.2-1`),
`cmd:pkg_config` (`pkgconf-1.5.3-2`), `cmd:python3` (`python3.14-3.14.7-1`),
`cmd:ruby` (`ruby-3.2.9-1`), `cmd:which` (`which-2.21-6`). VERIFIED — **zero
tooling gap.** (`cmd:llvm_config >= 21` is the fourteenth and is being deleted, §1.)

Watch item: `cmake-4.1.6`. CMake 4 hard-errors on
`cmake_minimum_required(VERSION <3.5)`. HaikuWebKit 1.10.0 is a 2026 tree so it is
almost certainly fine, but several of the small new deps are old — `woff2-1.0.2`
already passes `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` for exactly this reason, so the
workaround is known and one flag wide.

### Genuinely missing — ten packages, all small

Your guess was right on all four, and there are six more (three of them
sub-dependencies of `libpsl`, which you could not have seen without expanding it).
Every one has a recipe in the tree.

| # | Package | Recipe | Build system | New sub-deps | Note |
|---|---|---|---|---|---|
| 1 | `libexecinfo-1.1` | `sys-libs/libexecinfo` | **three raw `gcc` calls**, no configure | none | trivial, no hazard |
| 2 | `libgpg_error-1.61` | `dev-libs/libgpg_error` | autotools (`runConfigure ./configure`) | none | in `REQUIRES` only |
| 3 | `brotli-1.2.0` | `app-arch/brotli` | cmake | none | needed by woff2 |
| 4 | `woff2-1.0.2` | `media-libs/woff2` | cmake | brotli | provides `lib:libwoff2dec` |
| 5 | `lcms-2.19.1` | `media-libs/lcms` | autotools + `autoreconf -fi` | none (jpeg/tiff/z built) | provides `lib:liblcms2` |
| 6 | `libxslt-1.1.45` | `dev-libs/libxslt` | autotools + autoreconf/libtoolize | none (`libxml2` built) | `ARCHITECTURES="all"` |
| 7 | `libunistring-1.3` | `dev-libs/libunistring` | autotools + autoreconf | none | **gnulib — top hang risk** |
| 8 | `libidn2-2.0.5` | `net-dns/libidn/libidn2-2.0.5.recipe` | autotools/gnulib | libunistring | **gnulib — top hang risk** |
| 9 | `libpsl-0.21.5` | `net-libs/libpsl` | autotools + autoreconf/autopoint | libunistring, libidn2 | **mandatory**, see below |
| 10 | AVIF — see §2.1 | | | | two options |

`libpsl` is **not optional.** `OptionsHaiku.cmake:236` does
`SET_AND_EXPOSE_TO_BUILD(USE_CURL ON)` unconditionally, and inside that block
line 239 is `find_package(LibPSL 0.20.2 REQUIRED)`. There is no `USE_LIBPSL` knob.
The recipe selects its IDN backend by architecture:
`IDNA=libidn2`, overridden to `libidn` only when
`effectiveTargetArchitecture = x86_gcc2` — so **arm64 takes the `libidn2` path**,
which is why `libunistring` and `libidn2` come along. VERIFIED.

Note `libnspsl` in the built set is **not** `libpsl` — it is NetSurf's own
public-suffix library and does not provide `lib:libpsl`.

### Coming from the in-flight desktop closure (do not rebuild)

`giflib` (need `giflib-5.2.2`, which provides `lib:libgif = 7.2.0 compat >= 7`) and
`libwebp` (`libwebp-1.6.0`, provides `lib:libwebp` and `lib:libsharpyuv`) are both
in the ~40-package run already underway, so items that would otherwise be #11/#12
should arrive free. **NOT MEASURED** — I did not inspect that run's package output,
because doing so meant touching files it is writing. Re-check
`/opt/haiku/hpkg-out/arm64/` for `giflib-5.2.2-*` and `libwebp-1.6.0-*` before
starting Phase A, and only build them if absent.

`libwebp >= 7` **is** satisfied by `libwebp-1.6.0` (soname 7). `OptionsHaiku.cmake:49`
asks `find_package(WebP REQUIRED COMPONENTS demux)`, and the recipe provides
`lib:libwebpdemux`. VERIFIED from the recipes.

### 2.1 AVIF: a hard blocker, with two clean ways round it

`libavif >= 16` cannot be satisfied as the recipes stand.

- `media-libs/libavif/libavif1.0-1.4.2.recipe` provides
  `lib:libavif = 16.4.2 compat >= 16` — the right version — but requires
  `devel:librav1e` and `lib:librav1e`.
- **`rav1e` is Rust, and Rust does not exist on arm64 Haiku.**
  `dev-lang/rust_bin-1.94.1.recipe` declares `ARCHITECTURES="!x86_gcc2 ?x86 x86_64"`
  (no arm64; its sources are prebuilt i686/x86_64 tarballs) and
  `dev-lang/rust-1.79.0.recipe` declares `ARCHITECTURES="!x86_gcc2 ?x86 !x86_64"`
  (also no arm64). VERIFIED. Bootstrapping Rust for arm64 Haiku is a project in its
  own right and is out of scope here.
- The older `libavif-0.9.3.recipe` is not a way out either: `libVersion="13.0.0"`,
  so it provides `compat >= 13` and fails `>= 16`; and its
  `devel:libavif` provide is **commented out** (line 34), so it exposes no devel
  subpackage at all.

Two workarounds, both cheap:

**Option A (phase 1, recommended): turn AVIF off.** Add `-DUSE_AVIF=OFF` to the
recipe's `--cmakeargs` and delete `lib:libavif` / `devel:libavif >= 16` from
`REQUIRES` / `BUILD_REQUIRES`. `WebKitFeatures.cmake:319` defines `USE_AVIF` as a
normal toggle and `OptionsHaiku.cmake:91` merely defaults it ON, so switching it off
is supported, not a hack. Cost: no AVIF image decoding. Zero extra packages.

**Option B (phase 2): build `libavif1.0-1.4.2` with the encoder dropped.** Patch our
recipe fork to `-DAVIF_CODEC_RAV1E=OFF` and remove `rav1e` from `REQUIRES` /
`BUILD_REQUIRES`. WebKit only ever *decodes* AVIF, and the decoder is `dav1d`. That
adds two builds: `dav1d-1.5.4` (`media-libs/dav1d`, meson + ninja, both built) and
`libavif1.0-1.4.2` (cmake), plus `libsharpyuv` which arrives with `libwebp`. Both
are short. This restores AVIF properly and satisfies `>= 16` honestly.

Recommendation: ship Phase 1 with Option A, then do Option B as a follow-up. Do not
let AVIF gate the browser.

Also verified while here: `USE_JPEGXL` is defaulted **OFF** for Haiku
(`OptionsHaiku.cmake:93`), which is why `1.10.0` dropped `libjxl` from `REQUIRES`.
So no `libjxl`, no `libtasn1` — those belong to the older `1.9.19` recipe.

`USE_LCMS` and `USE_WOFF2` are ON with `FATAL_ERROR` if not found
(`OptionsHaiku.cmake:124–135`). Both have `-DUSE_*=OFF` escapes, but **build them**:
`lcms` is one small autotools port, and `woff2` + `brotli` are two small cmake ports.
Turning `USE_WOFF2` off would break WOFF2 web fonts, which nearly every modern site
serves — a visible regression for almost no saving.

---

## 3. Exact ordered build list

Every step is a `haikuporter` invocation for arm64. Ordering respects
dependencies. Steps marked **[skip if present]** may already be done by the
in-flight desktop closure.

```
# --- Phase A.0: the recipe edit that saves an LLVM build (no build) ---
  edit haiku-libs/haikuwebkit/haikuwebkit-1.10.0.recipe:
    - remove  cmd:llvm_config >= 21   from BUILD_PREREQUIRES
    - (Option A) add -DUSE_AVIF=OFF to --cmakeargs; drop lib:libavif and
      devel:libavif >= 16 from REQUIRES / BUILD_REQUIRES

# --- Phase A.1: trivial, isolated, no autotools risk (minutes each) ---
 1. libexecinfo-1.1        # 3 gcc commands
 2. brotli-1.2.0           # cmake
 3. woff2-1.0.2            # cmake, needs brotli
 4. giflib-5.2.2           # [skip if present] plain make
 5. libwebp-1.6.0          # [skip if present] needs libgif

# --- Phase A.2: autotools, one at a time, hard timeouts (see §5) ---
 6. libgpg_error-1.61
 7. lcms-2.19.1
 8. libxslt-1.1.45

# --- Phase A.3: the gnulib chain. DISPOSABLE GUEST. One at a time. ---
 9. libunistring-1.3
10. libidn2-2.0.5
11. libpsl-0.21.5

# --- Phase B: the go/no-go gate (see §6) ---
12. haikuwebkit-1.10.0 -- configure + partial compile only, then stop

# --- Phase C: the real thing ---
13. haikuwebkit-1.10.0    # produces haikuwebkit + haikuwebkit_devel

# --- Phase D: the image (see §4) ---
14. three-file jam change + bake

# --- Phase E: optional follow-ups, not blocking ---
15. dav1d-1.5.4  +  libavif1.0-1.4.2 (rav1e removed)  -> re-enable USE_AVIF
16. icu74-74.1 non-bootstrap, only if ICU data proves short
17. mesa-22.0.5 (via llvm12) for accelerated GL at runtime — NOT a build gate
```

**Genuinely-missing count: 9 small packages** (10 with AVIF via Option B, 11 with
`dav1d`). Not forty, and no second LLVM.

---

## 4. The WebPositive gate: three files, and only one of them is the guard you asked about

There are two independent gates plus the app itself.

### 4.1 The profile guard — `build/jam/DefaultBuildProfiles`, lines 127–132 and 172–177

Identical block in the `release-*` and `nightly-*` cases. Quoted verbatim
(`release-*` copy):

```
127			# WebPositive can only built for x86_gcc2, x86 and x86_64
128			if $(HAIKU_PACKAGING_ARCHS) in x86_gcc2 x86 x86_64 {
129				AddOptionalHaikuImagePackages WebPositive ;
130			} else {
131				Echo "WebPositive not available on $(HAIKU_PACKAGING_ARCHS)" ;
132			}
```

and again at 172–177 for `nightly-*`. The minimal edit is one token per site:

```diff
-			# WebPositive can only built for x86_gcc2, x86 and x86_64
-			if $(HAIKU_PACKAGING_ARCHS) in x86_gcc2 x86 x86_64 {
+			# WebPositive can only built for x86_gcc2, x86, x86_64 and arm64
+			if $(HAIKU_PACKAGING_ARCHS) in x86_gcc2 x86 x86_64 arm64 {
```

Apply at **line 128** and at **line 173**, and fix the stale comments at 127 and 172.
That is the whole profile-guard change.

### 4.2 The real gate — `haikuwebkit_devel` must be visible to jam

`AddOptionalHaikuImagePackages WebPositive` only *requests* the package. What
actually decides whether WebPositive compiles is `build/jam/BuildFeatures:303`:

```
303	if [ IsPackageAvailable haikuwebkit_devel ] {
...
314		EnableBuildFeatures webkit ;
```

`IsPackageAvailable` consults `build/jam/repositories/HaikuPorts/<arch>`.
`build/jam/repositories/HaikuPorts/x86_64` lists, at lines 108–109:

```
	haikuwebkit-1.10.0-1
	haikuwebkit_devel-1.10.0-1
```

`build/jam/repositories/HaikuPorts/arm64` is 71 lines of **bootstrap** packages
and lists neither. So the second edit is to add both to the
"primary architecture (arm64)" list in that file — noting the recipe is now
`REVISION="2"`, so the correct entries are `haikuwebkit-1.10.0-2` and
`haikuwebkit_devel-1.10.0-2`.

**Open operational question — NOT RESOLVED.** The arm64 repository file points at
the upstream bootstrap repository, which will not serve our locally-built arm64
hpkgs. `netsurf-3.11` reaches our images without appearing anywhere in `build/jam`,
so the project already has a sideload path for locally-built packages — but
WebPositive is different: it is compiled *from our tree* and needs the
`haikuwebkit_devel` **headers and `libWebKitLegacy.so` at jam time**, not just an
hpkg copied into the image. Before Phase D, confirm which of these applies:
(i) the local hpkg directory is registered as a jam-visible repository, in which
case add the two lines and go; or (ii) it is not, in which case the `webkit`
build feature needs a small local shim to point `ExtractBuildFeatureArchives` at
the locally-built `haikuwebkit_devel` hpkg. Cheap to determine — one `jam -q -n`
dry run once the package exists.

### 4.3 The app itself needs no change

- `src/apps/webpositive/Jamfile` has **no architecture gating**. It self-gates on
  the build feature at line 48 (`if ! [ FIsBuildFeatureEnabled webkit ] { continue; }`)
  and, on success, sets `EnableBuildFeatures webpositive` at line 90 — which is what
  `build/jam/packages/WebPositive:1`, `build/jam/OptionalPackages:142` and
  `build/jam/repositories/Haiku:43` then consume. Nothing to edit. VERIFIED.
- **Exactly one piece of architecture-specific code exists in the whole app**:
  `src/apps/webpositive/BrowserApp.cpp:85`, `#ifdef __i386__`, guarding a `get_cpuid()`
  SSE2 capability check that pops an alert recommending NetSurf. It is compiled out
  on arm64 (and on x86_64). A grep of the whole app for `__i386__`, `__x86_64`,
  `<immintrin.h>`, `<xmmintrin.h>`, `__SSE`, and `_mm_` finds nothing else. So the
  app should compile for arm64 unchanged — VERIFIED for arch-specific code,
  **INFERRED** for "compiles clean", since it has never been compiled for arm64 here.

**Summary of the change: two lines in `DefaultBuildProfiles` (128, 173) plus two
comment lines, two lines in `repositories/HaikuPorts/arm64`, zero lines in the app.**

---

## 5. Size, time, memory, and whether the current guests suffice

> **Note (infra retired):** the capacity analysis in §5 and the sequencing in §8 were
> written against the metal + QEMU-guest fleet (the "current guests" / "the host"),
> which has since been retired. Builds now run on native Graviton Haiku EC2 instances
> over SSM (`graviton/scripts/haiku-nativebuild`), where the box is sized per build
> rather than shared across standing guests. Read the guest/host figures below as the
> original estimate on that retired fleet; WebKit has since been built (see the WebKit
> arm64 work). The size/time/memory estimates themselves still hold.

### Measured inputs

| Quantity | Value | How |
|---|---|---|
| HaikuWebKit 1.10.0 tarball | 39,161,678 B (37.3 MiB) | downloaded |
| unpacked source | 240,841,370 B (230 MiB), 25,111 files | `tar -tzv` sum |
| on-disk after extract | 278 MiB (`Source` 276 M, `Tools` 2.4 M) | `du -sh` |
| `Source/WebCore/Sources.txt` | 5,390 entries | `wc -l` |
| `Source/JavaScriptCore/Sources.txt` | 1,314 entries | `wc -l` |
| build host | 64 vCPU, 125 GiB RAM, 95 GiB free on `/` | `nproc`, `free`, `df` |
| host RAM already committed to guests | ~102 GiB (6 x 16 GiB + 1 x 6 GiB) | qemu command lines |
| host load at time of survey | 1.06 / 0.86 / 0.52 | `uptime` |
| build guest shape | **12 vCPU, 16 GiB RAM** | qemu `-smp 12 -m 16384` |
| build guest disk | **19.6 GiB raw, fully allocated** | `stat` = 21,009,268,736 B |

WebKit is far smaller than feared: 230 MiB, not gigabytes — the release tarball has
the test suites stripped. **Source size is not the problem. Object files and RAM are.**

### Estimate (labelled: this is an ESTIMATE, error bar roughly ±2x)

WebKit compiles unified source bundles (~8 files each), so ~5,390 WebCore +
~1,314 JavaScriptCore entries plus generated IDL bindings and WTF land at roughly
**1,100–1,400 translation units**, each a large, template-heavy bundle.

- Per-TU: ~60–180 s wall, **1.0–2.5 GiB peak RSS** under GCC 13 at `-O2`.
- Total compute: ~1,200 TU x ~100 s ≈ **33 CPU-hours**.
- Plus IDL bindings generation (perl/python, poorly parallel) ~20–40 min, and the
  final single-threaded links of `libWebKitLegacy.so` and `libJavaScriptCore.so`
  ~10–25 min each.
- Guest I/O is BFS over emulated `usb-storage`; add ~30–50%.

**The governor is memory, not cores.** At 2.5 GiB/job, a 16 GiB guest supports about
`-j6`; a 32 GiB guest about `-j12`. Twelve vCPUs cannot be fed at 16 GiB. So:

| Guest shape | Practical `-j` | Estimated wall clock |
|---|---|---|
| 12 vCPU / 16 GiB (**today's guests**) | 6 | **12–20 h, and likely OOMs first** |
| 32 vCPU / 32 GiB | 12 | **6–10 h** |
| 32 vCPU / 64 GiB | 24 | **4–6 h** |

Two independent corroborations that memory pressure is real, not imagined: the recipe
passes `--param ggc-min-expand=10` (a GCC flag whose only purpose is to trade time
for peak memory), and the parked Clang patchset wraps two WebCore unified subtargets
in `if(FALSE) # Disabled for Haiku 32-bit, OOMs`.

**Peak disk inside the guest: budget 40 GiB, absolute floor ~25 GiB.**
~1,200 objects at 5–25 MiB plus generated bindings, the two large shared objects, the
porting tool's source copy and install staging, and the output hpkgs.

### Do the current guests suffice? **No — on both axes.**

- **Disk: fails outright.** The guest image is 19.6 GiB *total*, and a Haiku builder
  install with 267 packages seeded already consumes several GiB of that. I could
  **NOT MEASURE** the exact free space inside a guest — the `ssh` route needed
  credential digging I stopped rather than force, and reading a live raw image from
  the host would be unreliable. But 19.6 GiB is a hard ceiling and the requirement is
  25–40 GiB, so the conclusion does not depend on the missing number.
- **RAM: fails in practice.** 16 GiB caps parallelism at ~6 of the 12 vCPUs and leaves
  no margin for the peak bundles.

### Good news: the provisioning recipe already exists on the host

`/opt/haiku/builder-boot.sh` opens with:

```
# Builder VM: like boot-ssh-test.sh but 32G/32 SMP for WebKit builds.
```

and runs `-m 32768 -smp 32`, with an optional `$SCRATCH` second `usb-storage`
drive. Someone anticipated exactly this. Concretely:

1. Clone a fresh guest from the standard 19.6 GiB image (`mkguest.sh` shows the
   procedure: copy `haiku-mmc.image`, seed `haikuports.conf`, extract the recipe
   tree, `scp` the 267 hpkgs into `packages/`).
2. Boot it with **`-m 49152 -smp 24`** (or 32 GiB if RAM is tight) **plus a 60 GiB
   `$SCRATCH` raw disk** mounted for the build tree. The host has 95 GiB free, so
   60 GiB fits with ~35 GiB to spare; 80 GiB would leave too little.
3. **RAM is the contended resource, not disk or CPU.** ~102 GiB of the host's
   125 GiB is already committed to the seven running guests, leaving ~19 GiB. So a
   32–48 GiB WebKit guest **requires shutting down two or three of the existing
   build guests first**, or waiting for the desktop closure to finish. Do not start
   Phase C while that run is live.
4. Cap jobs explicitly rather than trusting `$jobArgs`: set the porting tool's job
   count to `floor(guestRAM_GiB / 2.5)`.

A 16 vCPU c7g.4xlarge is *not* a better host for this: it has 32 GiB total, so the
guest would get at most ~28 GiB, and it has fewer cores. **Run WebKit on the
64-vCPU metal host, in one properly-sized guest, with the other guests stopped.**

---

## 6. Hazards, and how each is caught early

### H1 — Guest OOM or disk exhaustion mid-build *(most likely to kill this)*

WebCore's unified bundles are the largest C++ TUs in the ports tree, and the link
of `libWebKitLegacy.so` is single-threaded and multi-gigabyte.

*Detect early:* right after `cmake` configure, run `ninja -n | wc -l` to get the
real TU count instead of my estimate. Then watch RSS across the first ~20 WebCore
bundles; if any single `g++` exceeds ~2.5 GiB, drop `-j` to
`guestRAM / observed_peak` immediately. Check free space on the build volume once
bindings generation completes — **if under 20 GiB remain at that point, abort and
enlarge rather than discovering it eight hours in.**

### H2 — gnulib / autoconf native-configure hang in the `libpsl` chain

This is the known class on this platform: a native `configure` runs a test that
never returns, the `conftest` becomes unkillable, and the wedge takes the **whole
guest** down, not just the build. Three of the nine missing packages are
gnulib-flavoured autotools — `libunistring-1.3`, `libidn2-2.0.5`, and
`libpsl-0.21.5` — and they are mandatory (`libpsl` has no `USE_*` escape, §2).
Three more are ordinary autotools with lower exposure: `libgpg_error-1.61`,
`lcms-2.19.1`, `libxslt-1.1.45`.

*Detect early:* build the three gnulib ports **first, one at a time, in a
disposable guest**, never batched with anything expensive. Put a hard wall-clock
timeout on each `configure` (~10 min with no new output = wedged) and recover by
**killing the guest**, not the process. Note the current topic branch
`fix/haikuports-libtool-and-sources` already exists for the autotools/libtool class
in this tree — reuse whatever it established rather than rediscovering it.

The safe five have no such exposure: `libexecinfo` (three raw `gcc` calls, no
configure at all), `brotli` and `woff2` (cmake), `giflib-5.2.2` (plain make),
`libwebp` (cmake). Start there to bank progress.

### H3 — HaikuWebKit 1.10.0 has never been proven to build with GCC

The only patchset shipped for 1.10.0 is a **Clang** patchset, and the recipe does
**not** apply it (`PATCHES=` was removed in `5b5295d02`, and the file was renamed to
`...-clang.patchset` in `163520dcb` purely to silence the tooling). The reference
builders' last recorded intent for this version was a Clang attempt that got
reverted. So it is entirely possible 1.10.0 contains GCC compile errors that nobody
has fixed — and this is the one risk that no amount of dependency work removes.

*Detect early — this is Phase B and it is the real go/no-go:* before committing to a
multi-hour run, do a **configure + first-few-hundred-objects smoke build**
(~30–60 min). Three checkpoints, in order:

1. CMake configure completes and prints `Archiver variant in use: BFD` /
   `Archiver supports thin archives - TRUE` — this closes out §1's one inference.
2. Every `find_package` in `OptionsHaiku.cmake` succeeds — ICU (data/i18n/uc),
   JPEG, LibXml2 >= 2.8.0, LibXslt >= 1.1.7, OpenGLES2, PNG, SQLite3,
   WebP (demux), ZLIB, CURL >= 7.77.0, LibPSL >= 0.20.2, LCMS2, WOFF2 (dec).
   Any `FATAL_ERROR` here is a missing package, caught in minutes, not hours.
3. `WTF` and `JavaScriptCore` compile clean. If those two are clean, GCC-viability
   risk drops sharply; if they are not, stop and reassess before spending a day.

*Fallback if 1.10.0 will not build with GCC:* `haikuwebkit-1.9.19` exists in the
tree. It is **not** a cheap fallback: `ARCHITECTURES="?all !x86_gcc2"` means every
architecture is opt-in (arm64 would need an explicit un-gate), and its dependency
set is **larger** — it additionally wants `libjxl`, `libtasn1`, `libidn2`,
`libunistring`, and `libidn2`. Treat it as a retreat, not a shortcut.

### Lesser hazards, noted

- **CMake 4** may reject an old `cmake_minimum_required` in `brotli` / `woff2` /
  `libavif`; the fix is one flag (`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`), which
  `woff2-1.0.2` already carries.
- **Bootstrap ICU** ships only `icu-uc.pc`; if a `find_package` unexpectedly relies
  on pkg-config for `icu-i18n`, build the non-bootstrap `icu74-74.1` (`REVISION="6"`).
- **`libgpg_error`** appears in `REQUIRES` with no matching `BUILD_REQUIRES` —
  probably vestigial, but build it anyway; it is one of the smallest ports here.

---

## 7. Alternatives, priced

| Option | Price | Cheaper than haikuwebkit? |
|---|---|---|
| **haikuwebkit + WebPositive** | 9 small ports + 1 large build (4–10 h) + 3 jam lines; native Haiku UI; ships `cmd:jsc` as a bonus | — baseline |
| **Otter Browser / Falkon** | Needs the whole Qt stack **plus** an engine: QtWebEngine is a bundled Chromium (~5 GiB source, wants Clang **and Rust** — and Rust does not exist on arm64 Haiku, §2.1), or the abandoned QtWebKit. Strictly a superset. | **No — far more** |
| **epiphany** | Needs **WebKitGTK** — the same WebKit engine, so the identical large build — **plus** GTK4, GLib, Pango, Cairo, GDK-Pixbuf, libsoup and their chains, none built here. Strict superset. | **No — strict superset** |
| **stay on netsurf-3.11** | Free (already built and working) | Cheaper, but it is the status quo this document exists to replace |
| **`jsc` shell only** | Falls out of the same haikuwebkit build (`PROVIDES: cmd:jsc`) | Not a browser, but free once we build |

I checked rather than assumed, and the assumption held for a sharper reason than
"Qt/GTK are big": **both alternatives require WebKit-or-Chromium *and* a foreign
widget toolkit, and the Chromium route re-introduces the Rust blocker that §2.1
already established is unsolved on arm64 Haiku.** haikuwebkit is the cheapest modern
engine on this platform by a wide margin.

---

## 8. Recommendation: **GO, staged**

The two things that could have made this a NO-GO both dissolved under evidence:

- The `llvm21` requirement is a **reverted-experiment artefact**, not a real
  dependency — removing one line avoids an entire second LLVM build, and the same
  finding also decouples WebKit from the mesa/llvm12 chain.
- The dependency gap is **nine small packages**, not forty, and every one has a
  recipe in the tree. All fourteen build tools are already built.

One genuine hard blocker turned up — **no Rust on arm64, so `rav1e` and therefore
`libavif1.0` cannot be built as written** — and it has two clean workarounds, the
cheaper of which (`-DUSE_AVIF=OFF`) costs only AVIF image decoding and can be
reversed later.

The residual risk is **machine capacity**, which is a provisioning decision rather
than an unknown: today's 12 vCPU / 16 GiB / 19.6 GiB guests **cannot** build WebKit,
but the host has the cores and the disk, and `/opt/haiku/builder-boot.sh` already
carries a `32G/32 SMP for WebKit builds` shape. The one real constraint is that
~102 GiB of the host's 125 GiB is committed to the seven running guests, so **Phase C
must wait for the desktop closure to finish, or for two or three guests to be shut
down.**

Suggested sequencing:

- **Phase A** — the recipe edit plus nine small ports. Fits in existing guests,
  no contention with the desktop run, ~1 day with the gnulib chain isolated.
- **Phase B** — configure + smoke build on a properly-sized guest, ~1 h.
  **This is the real go/no-go.** It closes the one open inference in §1 and tests
  H3 (GCC-viability of 1.10.0) for the price of an hour instead of a day.
- **Phase C** — the full build, 4–10 h, on a 32-vCPU / 32–48 GiB guest with a
  60 GiB scratch disk, with the other guests stopped.
- **Phase D** — the three-file jam change and an image bake; resolve the §4.2
  repository-visibility question with a `jam -q -n` dry run first.
- **Phase E** — optional: `dav1d` + `libavif1.0` (rav1e removed) to restore AVIF;
  non-bootstrap ICU if needed; mesa for accelerated GL at runtime.

Do not start Phase C before Phase B passes, and do not start either while the
desktop-closure build holds the host's RAM.

---

## 9. What I did not verify

Stated plainly, so none of it reads as measured:

- **Free space inside a running build guest.** Not measured. The bound (19.6 GiB
  total image) is enough to reach the conclusion, but the exact figure is unknown.
- **Whether `giflib-5.2.2` and `libwebp-1.6.0` have landed** from the in-flight
  desktop closure. Not checked — inspecting that run's output meant touching files
  it is writing. Re-check before Phase A.
- **Whether HaikuWebKit 1.10.0 actually compiles with GCC on arm64.** Never
  attempted anywhere, as far as the recipe history shows. This is H3 and Phase B
  exists to answer it.
- **Whether WebPositive itself compiles for arm64.** Its only architecture-specific
  code is compiled out on arm64 (verified), but the app has never been built for
  this architecture.
- **Whether libavif 0.9.x would satisfy WebKit's source-level `find_package(AVIF 0.9.0)`.**
  Moot — `libavif-0.9.3` exposes no `devel:libavif` at all, so the question does not
  arise.
- **All wall-clock, RAM-per-TU and disk figures in §5 are estimates**, derived from
  measured source metrics and measured machine shapes, not from a WebKit build on
  this platform. Error bar roughly ±2x. `ninja -n | wc -l` after Phase B's configure
  replaces the TU estimate with a fact.
