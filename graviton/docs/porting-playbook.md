# DeBeOS arm64 porting playbook (symptom → root cause → fix)

Issue: #90 (part of #58). A living, greppable reference for porting HaikuPorts
recipes to native Graviton (arm64). Grounded in the 54+ ports actually converted
`failed → built` across the #136 waves — PRs #245/#246/#247 (keystone recipe
unblockers), #275 (a–e), #276 (m–r), #277 (f–l), #280 (s–z), and #271/#272
(source recovery). Every fix below shipped as a DeBeOS overlay recipe under
`graviton/haikuports-patches/recipes/` and was proven native `RC=0` with a
non-empty `.hpkg`.

**How to use this.** Read the exact build-log line, grep this file for a phrase
from it, jump to the failure class, apply the fix, cite the example port. When a
new port fails, triage with the [TARGET-SPEC template](#target-spec-template) at
the end and, once fixed, add a row to the class table here — this document is the
memory that keeps per-port flags from being re-derived.

**Ground rules (read once).**

- These are **DeBeOS overlay recipes, not upstream submissions.** Nothing here is
  offered back to HaikuPorts (project policy). Frame every fix as a
  portability/build-system correction for our tree.
- **Success is a harvested non-empty `.hpkg`, never an exit code.** See
  [the empty-package trap](#class-9-the-empty-package-trap) and
  [build_state=built ≠ published](#build_statebuilt--published).
- **None of the fixes below is a feature cut.** Feature-capped builds are a
  separate, discouraged category (see `haikuports-patches/README.md`); the classes
  here are missing tools, policy floors, arch guards, and portability edits that
  leave the package doing what upstream intends.

---

## Quick index (grep the symptom)

| # | Class | One-line log signature |
|---|---|---|
| 1 | [CMake 4.x pre-3.5 policy removal](#class-1-cmake-4x-pre-35-policy-removal) | `Compatibility with CMake < 3.5 has been removed` |
| 2 | [Undeclared build tool in the chroot](#class-2-undeclared-build-tool-in-the-haikuporter-chroot) | `<tool>: command not found` / `Error 127` |
| 3 | [gettext autoreconf / autopoint](#class-3-gettext-autoreconf--autopoint) | `AM_GNU_GETTEXT_VERSION` / `Can't exec "autopoint"` |
| 4 | [static/shared lib install](#class-4-staticshared-lib-install-conflict) | `rm: cannot remove lib*.a` / `prepareInstalledDevelLibs` |
| 5 | [x86-only compiler flags on arm64](#class-5-x86-only-compiler-flags-on-arm64) | `unrecognized command-line option '-msse2'` |
| 6 | [config.guess can't name arm64](#class-6-configguess-cannot-name-the-arm64-host) | `uname -m = arm64` → `cannot guess build type` |
| 7 | [LTO without a linker plugin](#class-7-lto-without-a-linker-plugin) | `LTO support has not been enabled` / `only with linker plugin` |
| 8 | [Missing / no CMAKE_BUILD_TYPE](#class-8-missing-cmake_build_type) | `invoking cmake without CMAKE_BUILD_TYPE specified` |
| 9 | [The empty-package trap](#class-9-the-empty-package-trap) | build "succeeds", `.hpkg` is ~800 B |
| 10 | [Missing autotools aux files](#class-10-missing-autotools-aux-files) | `required file 'install-sh' not found` |
| 11 | [Install target references missing files](#class-11-install-target-references-optional-outputs) | `mv: cannot stat …` / `sed: can't read …x86_64…` |
| 12 | [Source availability / checksum drift](#class-12-source-availability--checksum-drift) | `checksum` mismatch / 403 / dead DNS / HTML error page |
| 13 | [Hardcoded cross-compiler prefix](#class-13-hardcoded-cross-compiler-prefix) | `aarch64-unknown-haiku-g++: No such file or directory` |
| 14 | [Poisoned build-tool package](#class-14-poisoned-build-tool-package) | `Unhandled pheader type in parse 0x6474e553` |
| 15 | [runConfigure CFLAGS needs -O](#secondary-blocker-tail-136-re-wave-of-classes-3846710113) | `runConfigure: Must specify optimization flags when overriding CFLAGS` |
| 16 | [multiple definition (GCC10 -fno-common)](#secondary-blocker-tail-136-re-wave-of-classes-3846710113) | `ld: multiple definition of '<sym>'` |
| 17 | [CMake FetchContent offline fetch](#secondary-blocker-tail-136-re-wave-of-classes-3846710113) | `FetchContent_MakeAvailable` / `__FetchContent_populateSubbuild` |
| 18 | [Missing link-time library](#class-18-missing-link-time-library-ld-cannot-find--llib) | `ld: cannot find -l<lib>: No such file or directory` |
| 19 | [Build system does not recognise aarch64](#class-19-build-system-does-not-recognise-aarch64arm64) | `"Platform '' not supported"` / `unknown word-size for arch: aarch64` |
| 20 | [Prerequisite package/tool not published](#class-20-prerequisite-packagetool-not-published) | `unable to resolve (pre)required packages` / `Package 'X' … not found` |
| 21 | [Python setup.py needs setuptools](#class-21-python-setuppy-needs-setuptools--pkg_resources) | `ModuleNotFoundError: No module named 'pkg_resources'` |
| 22 | [Removed/renamed Haiku API or moved header](#class-22-removedrenamed-haiku-api-or-moved-header) | `'BSplitView' does not name a type` / `fatal error: <X.h>: No such file` |
| 23 | [Install writes to read-only /packages](#class-23-install-writes-to-read-only-packages) | `mkdir: cannot create directory '/packages/…_devel-…': Read-only file system` |
| 24 | [Source-acquisition tool missing on host](#class-24-source-acquisition-tool-missing-on-the-builder-host) | `Error: '<hg\|svn\|lha>' is not available, please install it` |

Classes 15–17 were surfaced by the #136 re-wave secondary-blocker triage; see
[that section](#secondary-blocker-tail-136-re-wave-of-classes-3846710113) at the
end of this document. **Classes 18–24 were mined from the real UNMATCHED backlog by
`graviton/scripts/haiku-pattern-miner`** (see
[How classes 18–24 were mined](#how-classes-1824-were-mined--the-unmatched-frontier)).

---

## Class 1: CMake 4.x pre-3.5 policy removal

**By far the largest class this session** (opencc, polyclipping, robin_map,
recastnavigation, portsmf, primesieve, qhull, pystring, minisign, gflags,
squirrel, tidy, uchardet, zopfli, slack++, surgescript, teeworlds, toluapp,
unshield, sdl2_sound, serious_sam, vvvvvv, vc, yaml_cpp0.7, yaml_cpp0.8, cmake_haiku,
openal, libjxl — and json_c earlier).

**Symptom (configure aborts at `CMakeLists.txt` line 1-3):**
```
CMake Error … Compatibility with CMake < 3.5 has been removed from CMake.
… add -DCMAKE_POLICY_VERSION_MINIMUM=3.5 to try configuring anyway.
```

**Root cause.** The builder's CMake is 4.x. CMake 4 removed the compatibility
shim for `cmake_minimum_required(VERSION <3.5)`, so any project (or bundled
`add_subdirectory()` tree) that declares an old minimum hard-errors before it
looks at anything else.

**Fix.** Add the policy floor to the `cmake` invocation:
```sh
cmake . \
    -DCMAKE_INSTALL_PREFIX=$prefix \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```
(as in `recipes/opencc-1.1.4.recipe`). It only raises the floor, so projects
requesting a newer minimum are unaffected, and a duplicate `-D` of the same value
is harmless.

**The #47 caveat — where the systemic fix reaches and where it doesn't.**
`haikuporter-cmake-policy-minimum.patch` appends the flag **once** to
haikuporter's shared `cmakeDirArgs` shell variable (`HaikuPorter/Port.py`), so
every recipe that passes `$cmakeDirArgs` — and every bundled tree it pulls in,
since it's a cache variable — gets the floor for free.
- **An env var cannot do this**: `HaikuPorter/Utils.py:filteredEnvironment()`
  strips everything but `PATH`/`LIBRARY_PATH`/`LC_ALL`/`TERM`, so exporting
  `CMAKE_POLICY_VERSION_MINIMUM` is dropped before `cmake` runs.
- **It does NOT reach a recipe whose `BUILD()` calls `cmake` with its own
  explicit arg list** (never interpolating `$cmakeDirArgs`). That is most of the
  #280 ports, and is why the per-recipe `-D` is still needed there.
- `serious_sam` needed the flag on **all three** of its `cmake` subdir calls.

---

## Class 2: Undeclared build tool in the haikuporter chroot

The #27 "coreutils" class — the single most common non-CMake failure.

**Symptom:** a common utility dies mid-build even though it exists in the base
image:
```
gzip: command not found            (Error 127)
unzip: command not found
which: command not found
msgfmt: No such file or directory  (Error 127)
configure: tar utility not found
```

**Root cause.** For its in-chroot phases (`BUILD()`, `INSTALL()`, `TEST()`)
haikuporter mounts **only the recipe's declared `BUILD_PREREQUIRES`** plus a fixed
base set into the build chroot. A recipe that shells out to a tool it never
declared dies with `command not found`, regardless of whether the tool is
installed on the builder host.

**A phase nuance that decides the fix.** `PATCH()` (and `downloadSource`/
`unpackSource`) run on the builder **HOST**, *before* the build chroot exists —
`Main.py` calls `port.patchSource()` ahead of `port.build()`, and only `build()`
enters `ChrootSetup`. So a tool used in `PATCH()` must be on the **host**, and a
`cmd:` in `BUILD_PREREQUIRES` will not help it (that reaches only the chroot).
`hexcompare` is the trap: its recipe already declares `cmd:dos2unix`, yet its
`PATCH()` (`dos2unix general.h …`) still failed `command not found` because the
lean builder host had no dos2unix. Its host provisioning was the fix, not the
recipe.

**Fix — two shapes; pick by whether the tool is universal or feature-specific.**

1. **Systemic (universal build utilities).** `gzip`, `tar`, `unzip`, `which` are
   the coreutils-tier tools any `make install` / configure / autogen.sh assumes.
   They are seeded into **every** chroot via haikuporter's own base set,
   `HaikuPorter/ShellScriptlets.py:scriptletPrerequirements` (which already carries
   coreutils/bash/sed/grep/…), patched idempotently by
   `graviton/scripts/haiku-provision-native-builder` — see
   `haikuports-patches/haikuporter-scriptlet-build-tools.patch` (#136). No
   per-recipe edit is needed for these, and it reaches ports never touched. The
   same four (plus dos2unix) are also added to the provisioner's host install list
   so host-side `PATCH()` has them too. This is the analogue of the CMake-policy
   systemic fix (#47/#136) and clears the whole recurring class on a rebake.
2. **Per-recipe (feature-specific tools).** A tool only some ports use stays a
   declared `cmd:` prerequisite in the overlay recipe — putting it in every chroot
   would over-broaden (gettext is a 16 MB package):
   ```
   BUILD_PREREQUIRES="
   	haiku_devel
   	cmd:msgfmt
   	cmd:pkg_config
   	"
   ```

**Tool → provider map (measured against the #136 Class-2 wave logs).** All
providers confirmed present in the DeBeOS arm64 pool.

| Missing tool | Fix shape | Declare / provider | Example ports |
|---|---|---|---|
| `gzip` (manpage compress on `make install`) | **systemic** | scriptletPrereq → `gzip` | atari++, convmv, djvu, kakoune, libtermkey, mdate, unibilium |
| `tar` (install step / `configure` "tar utility not found") | **systemic** | scriptletPrereq → `tar` | instead, mm_common |
| `unzip` (unpack a bundled archive) | **systemic** | scriptletPrereq → `unzip` | betterspades |
| `which` (autogen.sh / pkg-config probe) | **systemic** | scriptletPrereq → `which` | autotrace |
| `dos2unix` (used in host-side `PATCH()`) | **host provision** | provisioner TOOLS → `dos2unix` | hexcompare (recipe already declared it) |
| `msgfmt` | per-recipe | `cmd:msgfmt` (+`cmd:pkg_config`) → gettext | blobwars |
| `msgattrib` | per-recipe | `cmd:msgattrib` → gettext | neverball |
| `perl` (dcgen / generated headers) | per-recipe | `cmd:perl` | coreutils (#27) |
| `yacc` / bison-generated parser | per-recipe | `cmd:yacc` (bison provides it) | tmux |

**Not this class — do not mis-tag as auto-recoverable.** A bare `Error 127` also
appears when: a `configure` conftest binary fails to build then "runs" (`cdrtools`:
`OBJ/.../align_test: No such file or directory`); an empty cross-prefix expands to
`-gcc` (`gdb`, really a Class-13/toolchain issue); a `*-config` probe returns `no`
and the shell tries to run it (`libassuan`, whose real error is `libgpg-error was
not found` — a missing dependency); a Makefile hardcodes a secondary-arch compiler
that does not exist on arm64 (`protrekkr`: `g++-x86`); or an `autogen.sh` demands a
version-pinned tool the pool does not carry (`libmypaint`: `automake-1.16`, a
Class-3 autoreconf fix). These need per-port judgement, not a `cmd:` add.

**Note on coreutils' second symptom.** `ln: failed to create hard link …:
Operation not allowed` is a **non-fatal** gnulib configure probe ("whether rename
manages hard links correctly"): Haiku returns EPERM for that same-name hard-link
test, configure records it and continues. Only the missing `cmd:perl` was fatal.
Don't chase the hard-link message.

---

## Class 3: gettext autoreconf / autopoint

**Symptom (autoreconf phase):**
```
configure.ac: error: possibly undefined macro: AM_GNU_GETTEXT_VERSION
      AC_LIB_* / AM_GNU_GETTEXT undefined
autoreconf: Can't exec "autopoint" … autopoint failed
configure: error: Internationalization tools missing
```

**Root cause (two parts).** A port that regenerates its build system with
`autoreconf` and whose `configure.ac` uses the gettext macros fails because
(a) `cmd:autopoint` is not in the chroot — the recipe never declared it, so even
though `gettext` is installed on the builder **host** (#293) it is not mounted
into the per-port chroot — **and** (b) even with autopoint present, gettext's m4
macros (`gettext.m4`, `nls.m4`, `lib-prefix.m4`, `lib-link.m4`, …) install under
`/boot/system/data/gettext/m4`, **not** the default aclocal dir, so `aclocal`
cannot expand `AM_GNU_GETTEXT` / `AM_GNU_GETTEXT_VERSION` / `AM_NLS` / the
`AC_LIB_*` helpers unless that dir is on `ACLOCAL_PATH`.

**Why the fix is per-recipe, not systemic (contrast with Class 2).** Class 2 put
`gzip`/`tar`/`unzip`/`which` into every chroot by appending to
`scriptletPrerequirements`. That lever **cannot** solve Class 3: both halves of
the fix are `BUILD()`-body actions — an `export ACLOCAL_PATH=…` and running
`autopoint --force` **before** `autoreconf` — and `scriptletPrerequirements` only
*mounts packages*; it can neither set an env var inside a recipe's `BUILD()` nor
reorder that recipe's own `autoreconf` call. Mounting `gettext` into every chroot
(the only systemic option) would still leave part (b) unfixed — `aclocal` would
not look in `/boot/system/data/gettext/m4` — while re-broadening the exact 16 MB
gettext package #293 deliberately kept per-recipe (and, because `autopoint` ships
in the *same* package as `msgfmt`/`msgmerge`, a systemic `cmd:autopoint` is a
systemic `cmd:msgfmt` — the thing #293 scoped out). `autopoint` is also
consistent with its sibling autotools (`autoconf`/`automake`/`libtool`), which
are themselves declared **per-recipe**, not in the base set. So Class 3 is fixed
with a per-recipe overlay per port.

**Systemic enabler that IS required (already landed).** The overlays only resolve
because #293 installs the full `gettext` package on the builder **host**: a
`cmd:autopoint` prerequisite is mounted into the chroot only if it is resolvable
against the host pool. On a pre-#293 AMI `cmd:autopoint` was unresolvable, which
is why the 2026-09-15 wave failed even for ports whose overlay already declared
it (e.g. libexif). **Class 3 overlays therefore clear only on a rebake that
includes #293 + these overlays, then a re-wave.**

**Fix** (from `recipes/libcddb-1.3.2.recipe`):
```sh
BUILD_PREREQUIRES="
	…
	cmd:autopoint       # provided by the gettext package (host-installed by #293)
	cmd:gettext         # same package; declare where the recipe had neither
	"

BUILD()
{
	export ACLOCAL_PATH="/boot/system/data/gettext/m4${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
	autopoint --force        # MUST precede autoreconf; refreshes the m4 macros
	libtoolize --force --copy --install
	autoreconf -fi
	runConfigure ./configure --disable-static
	make $jobArgs
}
```
`autopoint --force` must run **before** `autoreconf -fi`. For a port driven by
`./autogen.sh`, export `ACLOCAL_PATH` before the script (it runs `aclocal`
itself); autopoint is invoked inside.

**Tool → provider map.** One package covers the whole class:

| Needed in chroot | Provider (arm64 pool) | Reaches chroot via |
|---|---|---|
| `cmd:autopoint`, the `gettext.m4`/`nls.m4`/`lib-*.m4` macros | `gettext` | per-recipe `BUILD_PREREQUIRES` (host-installed by #293 → resolvable) |
| `cmd:msgfmt` / `cmd:msgmerge` (configure-time i18n probes) | `gettext` (same package) | same declaration — no extra line needed |

**The 11 Class-3 ports (measured against the wave logs).** All get a per-recipe
overlay in `haikuports-patches/recipes/`; 10 use the ACLOCAL_PATH+autopoint
pattern, 1 (rpcsvc_proto) is a deeper variant. All 11 are expected to clear on
**rebake (#293 + overlays) + re-wave**.

| Port | Exact log signature | Fix |
|---|---|---|
| aiksaurus | `undefined macro: AM_NLS` | declare `cmd:autopoint`+`cmd:gettext`; ACLOCAL_PATH + `autopoint --force` before `./autogen.sh` |
| axel | `undefined macro: AM_GNU_GETTEXT_VERSION` (m4/gettext.m4) | had `cmd:gettext`; add ACLOCAL_PATH + `autopoint --force` before `autoreconf -fi` |
| dovecot | `undefined macro: AC_LIB_PREPARE_PREFIX` (+`AC_LIB_RPATH/…`) | had `cmd:gettext`; add ACLOCAL_PATH + `autopoint --force` |
| enca | `undefined macro: AC_LIB_PREPARE_PREFIX` (m4/librecode.m4) | had `cmd:gettext`; add ACLOCAL_PATH + `autopoint --force` |
| libcddb | `AM_GNU_GETTEXT_VERSION` + `AC_LIB_*` undefined | existing overlay (full pattern) — clears on rebake |
| libexif | `Can't exec "autopoint": No such file` | #52 overlay declared `cmd:autopoint`; **strengthened** here with ACLOCAL_PATH + `autopoint --force` |
| libggz | `checking for msgmerge... no` → `Internationalization tools missing` | existing overlay: `cmd:autopoint`→gettext also supplies msgmerge; clears once resolvable (#293) |
| libhangul | `AM_GNU_GETTEXT` + `AM_GNU_GETTEXT_VERSION` + `AC_LIB_*` undefined | existing overlay (full pattern) — clears on rebake |
| libmtp | `undefined macro: AC_LIB_PREPARE_PREFIX` | had `cmd:gettext`; add ACLOCAL_PATH + `autopoint --force` |
| xcftools | `AM_GNU_GETTEXT` + `AC_LIB_*` undefined | declare `cmd:autopoint`+`cmd:gettext`; add ACLOCAL_PATH + `autopoint --force` |
| rpcsvc_proto | `autopoint: *** found more than one invocation of AM_GNU_GETTEXT_REQUIRE_VERSION` | **deeper variant** — see below |

**Deeper variant — rpcsvc_proto.** Here autopoint *is* present but aborts: the
port's `configure.ac` carries an old-gettext compat shim
(`m4_ifndef([AM_GNU_GETTEXT_REQUIRE_VERSION], …)`) *plus* the real
`AM_GNU_GETTEXT_REQUIRE_VERSION([0.19.2])` *plus* a redundant
`AM_GNU_GETTEXT_VERSION([0.20.2])`, so the string appears more than once and
autopoint's textual scan rejects it. DeBeOS ships a modern gettext where the
macro is always defined, so the overlay's `PATCH()` deletes the dead `m4_ifndef`
block and the redundant `AM_GNU_GETTEXT_VERSION`, leaving a single invocation.
The `libmetalink`/`libspectrum` case (needing `AM_PATH_XML2`, libxml2's macro, on
the aclocal path) is the same *mechanism* (macro dir off the default path) with a
different provider.

---

## Class 4: static/shared lib install conflict

**Symptom:**
```
rm: cannot remove '…/libfoo.a': No such file or directory
```
or `prepareInstalledDevelLibs` fails because both a shared and a static lib are
present.

**Root cause.** Upstream `makefile.shared` (or a shared-only build) installs only
the `.so`, so a recipe `INSTALL()` that does `rm $libDir/libfoo.a` fails because
the `.a` was never produced. Conversely, some builds emit both and
`prepareInstalledDevelLibs` chokes on the static one.

**Fix.** Make the removal tolerant, and drop the static lib **before**
`prepareInstalledDevelLibs`:
```sh
rm -f $libDir/libtomcrypt.{a,la}      # tolerant: rm -f, not rm
…
rm -f $libDir/liblinenoise.a          # drop static before:
prepareInstalledDevelLibs liblinenoise
```

**Example ports:** libtomcrypt, libtommath, libvterm (`rm`→`rm -f`); linenoise
(drop static before `prepareInstalledDevelLibs`).

---

## Class 5: x86-only compiler flags on arm64

**Symptom:**
```
c++: error: unrecognized command-line option '-msse2'
cc1plus: … '-mfpmath=sse' / '-msse3'
```

**Root cause.** The recipe or upstream Makefile hardcodes x86 SIMD flags
(`-msse2`, `-msse3`, `-mfpmath=sse`) that aarch64 gcc rejects.

**Fix.** Gate the flag on `x86`/`x86_64` (use `$effectiveTargetArchitecture` for
CMake toggles, `$targetArchitecture` for Makefile seds):
```sh
# CMake toggle (recipes/libsquish-1.15.recipe)
case "$effectiveTargetArchitecture" in
	x86|x86_64) SSE2="-DBUILD_SQUISH_WITH_SSE2=ON" ;;
	*)          SSE2="-DBUILD_SQUISH_WITH_SSE2=OFF" ;;
esac

# Makefile sed (recipes/nesalizer-1.0~git.recipe)
case "$targetArchitecture" in
	x86|x86_64) ;;
	*) sed -i 's/-mfpmath=sse//g; s/-msse3//g' Makefile ;;
esac
```

**Example ports:** libsquish (`-DBUILD_SQUISH_WITH_SSE2=OFF`), nesalizer (sed the
Makefile — also Class 7 and Class 9).

**Not every `-msse2` port is a flag gate — know when the signature lies.** The
Class-5 fix only works when there is a **non-SIMD (or NEON) code path to fall back
to.** `embree` 3.12.2 fails with the same `unrecognized command-line option
'-msse2'` line, but embree ≤ 3.12 has **no ARM/NEON ISA at all** — SSE2 is baked
into its only backend, and `-DEMBREE_MAX_ISA=DEFAULT` has nothing to select on
aarch64. Gating the flag would just yield a library with no working kernel. NEON
support only arrived in embree 3.13, so embree is a **version-bump / real port,
not a Class-5 one-liner — deferred-hard** (see the deferred list at the end).

**Catch this (and the related x86 assumptions) up front.**
`graviton/scripts/haiku-port-lint` (see [port-hygiene.md](port-hygiene.md), #341) greps a
source tree / recipe for x86-only flags and preprocessor arms, x86 SIMD intrinsics with
no NEON path, IFUNC, `char`-signedness, and CPU-string blind spots — the mechanical,
before-the-build companion to this reactive playbook. When SSE/AVX intrinsics have no
NEON equivalent, build-depend on **SIMDe** (`devel:simde`,
`haikuports-patches/recipes/simde-0.8.2.recipe`) rather than cutting the feature.

---

## Class 6: config.guess cannot name the arm64 host

The #71 class.

**Symptom:**
```
uname -m = arm64
configure: error: cannot guess build type; you must specify one
      -- or, from CMake --
GetHostTriple.cmake:54 (message): Failed to execute … /cmake/config.guess
```

**Root cause.** Haiku's `uname -m` reports **`arm64`**; GNU `config.guess`/
`config.sub` expect **`aarch64`**. An old vendored `config.guess` can't match,
exits non-zero, and the caller aborts.

**Two sub-cases with different fixes:**

1. **Autotools port that runs both `config.guess` and `config.sub`.** `config.sub`
   already canonicalises `arm64-unknown-haiku` → `aarch64-unknown-haiku` on our
   box, so the fix is just to route through `runConfigure` (which supplies the
   host triple) instead of a bare `./configure $configureDirArgs`:
   ```sh
   runConfigure ./configure --program-suffix=-$portVersion
   ```
   *Example:* autoconf2.71 (mirrors the working sibling autoconf-2.72).

2. **LLVM-style consumer that reads `config.guess` RAW (never runs
   `config.sub`).** Here `-DLLVM_HOST_TRIPLE=` does **not** help, because
   `get_host_triple()` still runs the vendored script and still aborts. Replace
   the vendored `config.guess` outright, scoped to arm64
   (`recipes/llvm21-21.1.8.recipe`, #71):
   ```sh
   if [ "$effectiveTargetArchitecture" = arm64 ]; then
       printf '#!/bin/sh\necho aarch64-unknown-haiku\n' > llvm/cmake/config.guess
       chmod +x llvm/cmake/config.guess
   fi
   ```
   *Example:* llvm21 (and via it, mesa). `llvm12-12.0.1-config-guess-arm64.patch`
   is the same fix applied as a `config.guess` `PATCHES` hunk for the older tree.
   **Any LLVM-derived tree hits this**, at whatever path it vendors the script —
   confirm it from the log line (`Failed to execute …/config.guess`) and replace
   that exact path: `haiku_format` reads `llvm/cmake/config.guess` (it builds
   clang-format from a full llvm-project tree via `-S llvm`), and `keystone` reads
   `llvm/cmake/config.guess` from its bundled LLVM MC subset. Both took the
   identical arm64-scoped `printf … config.guess` block in `BUILD()` before the
   `cmake` call.

---

## Class 7: LTO without a linker plugin

**Symptom:**
```
cc1plus: error: LTO support has not been enabled in this configuration
lto1: … '-fno-fat-lto-objects' … only with linker plugin
```

**Root cause.** The recipe requests LTO (`-flto`,
`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`, `-fuse-linker-plugin`) but our gcc is
built without the LTO linker plugin.

**Fix.** Drop LTO — turn the CMake flag off, or sed `-flto`/`-fuse-linker-plugin`
out of the Makefile (same sed block as Class 5):
```sh
-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF        # draco
sed -i 's/-flto//g; s/-fuse-linker-plugin//g' Makefile   # nesalizer
```
The CMake toggle catches the common case where a project's own `CMakeLists.txt`
runs `check_ipo_supported()` and turns interprocedural optimisation on by default:
`unarr` failed with `cc1: error: LTO support has not been enabled in this
configuration` on every object until `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF`
was added to its `cmake` line, exactly as for draco.

**Example ports:** draco, unarr (CMake `IPO=OFF`); nesalizer (Makefile sed).

---

## Class 8: Missing CMAKE_BUILD_TYPE

**Symptom:**
```
invoking cmake without CMAKE_BUILD_TYPE specified!
      -- or --
cmake … -DCMAKE_BUILD_TYPE=RelWithDebInfo without debug info packages specified
```

**Root cause.** The message is **not** CMake's own — it comes from haikuporter's
`cmake` wrapper (`HaikuPorter/ShellScriptlets.py:cmake()`), which shadows `cmake`
in every `BUILD()` shell and `exit 1`s when the configure invocation either omits
`CMAKE_BUILD_TYPE` entirely, or passes `RelWithDebInfo` without a matching
`_debuginfo` subpackage. Two sub-signatures:

- `invoking cmake without CMAKE_BUILD_TYPE specified!` — no type at all.
- `-DCMAKE_BUILD_TYPE=RelWithDebInfo without debug info packages specified` — a
  type, but the wrong one for a recipe with no debug-info subpackage.

**Fix (systemic — preferred, #136).** Default the build type in the wrapper
itself, on the native builder, via
`graviton/scripts/haiku-provision-native-builder` (section 2a-4; see
`graviton/haikuports-patches/haikuporter-cmake-build-type-default.patch`). It
rewrites only the wrapper's two `exit 1` branches: a missing type becomes
`-DCMAKE_BUILD_TYPE=Release`, and `RelWithDebInfo`-without-`_debuginfo` is
rewritten to `Release` — both are exactly the wrapper's own printed advice. This
clears the whole class on a rebake, reaching every CMake port regardless of how it
spells the `cmake` call.

**Why not the Class-1 channels.** Unlike `CMAKE_POLICY_VERSION_MINIMUM`, this
cannot ride an environment default: the wrapper inspects the command line only,
and CMake reads `CMAKE_BUILD_TYPE` from `-D`/cache, never the env. And a blanket
`-DCMAKE_BUILD_TYPE=Release` on `cmakeDirArgs` would *regress* known-good ports —
a library passing its own `RelWithDebInfo` **with** a `_debuginfo` subpackage would
then trip the wrapper's "`Release` **with** debug info packages specified" guard
and start aborting. Patching only the two always-fail (`exit 1`) branches has a
**zero** no-regression surface — no port that built successfully ever traversed
them — and leaves the `Release`/`_debuginfo`-mismatch guard byte-for-byte intact.

**Fix (per-recipe — fallback).** Set the type on the recipe's own `cmake` call
(usually alongside the Class 1 policy flag):
```sh
cmake . … -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```
Note this path is *fragile* for this class: a per-recipe overlay reaches the
builder only through the input-source-package path, which haikuporter silently
re-extracts (reverting the edit) unless the recipe mtime is pinned forward of the
source package (see `recipes/README.md`). Four Class-8 ports carried a correct
per-recipe overlay yet still failed the wave for exactly this reason — which is
why the systemic wrapper fix is preferred.

**Example ports (all 10 #136 Class-8):** `invoking cmake without
CMAKE_BUILD_TYPE`: cmake_haiku, freegish, lensfun, libmirage, nogravity, sais,
sawteeth, superfreecell. `RelWithDebInfo` without debug info: epoll_shim, flac13.

---

## Class 9: The empty-package trap

**The most dangerous class — the build "succeeds" and ships nothing.**

**Symptom.** haikuporter exits 0, but the harvested `.hpkg` is ~800 B and
contains no binary. nesalizer shipped an **804 B** hpkg this way.

**Root cause.** Upstream's Makefile has **no `install` target**, so the recipe's
`make … INSTALL_DIR=` no-ops. Nothing lands in `$binDir`, but nothing errors.

**Fix.** Install the built artifact explicitly
(`recipes/nesalizer-1.0~git.recipe`):
```sh
INSTALL()
{
	mkdir -p $binDir
	install -m0755 build/nesalizer $binDir/nesalizer
}
```

**Detection rule.** Never trust the exit code. After a build, assert the `.hpkg`
is non-trivial in size **and** `listAttr`/unpack shows the expected `bin/…`.
This is the operational form of "success is a harvested non-empty hpkg."

---

## Class 10: Missing autotools aux files

**Symptom:**
```
configure.in: error: required file 'install-sh' not found
try running autoreconf --install
```

**Root cause.** The recipe runs a bare `autoreconf` (or `./configure` on a tree
lacking the aux scripts), so `install-sh`/`missing`/`depcomp` are never copied in.

**Fix.** Use `autoreconf -fi` (force + install aux files):
```sh
autoreconf -fi        # was: autoreconf
```
This also covers ports whose `BUILD()` calls a vendored **`./autogen.sh`** that
regenerates `configure` without installing the aux scripts: `screen` (which does
`cd src; ./autogen.sh`) aborted with `required file 'install-sh' not found — try
running autoreconf --install`; replacing `./autogen.sh` with `autoreconf -fi` (and
declaring `cmd:autoreconf`) fixed it.

**Example ports:** bonnie++ (was `autoreconf`), screen (was `./autogen.sh`).

---

## Class 11: Install target references optional outputs

**Symptom:**
```
mv: cannot stat 'share/doc/mpc': No such file or directory
sed: can't read …/lvp_icd.x86_64.json: No such file or directory
```

**Root cause.** `INSTALL()` references a path that only exists in some
configurations — an optional doc dir the build didn't emit, or an
architecture-named file hardcoded to the x86 spelling (mesa names its Vulkan ICD
manifest `lvp_icd.aarch64.json` on arm64, not `lvp_icd.x86_64.json`).

**Fix.** Guard the optional move, or resolve arch-named files by glob:
```sh
[ -d share/doc/mpc ] && mv share/doc/mpc …          # musicpc: guard
lvpIcd=$(ls …/lvp_icd.*.json)                        # mesa: glob, not hardcoded
```
**Example ports:** musicpc (guard doc move), mesa (glob the ICD manifest).

---

## Class 12: Source availability / checksum drift

The #271/#272 source-recovery class. See also `graviton/docs/download-mirror.md`.

**Symptom:** the fetch fails **before** any compile:
```
Expected: CHECKSUM_SHA256=…   Got: …        (checksum mismatch)
HTTP 403 / 404 / HTTP 300 "Multiple Choices"
served a 3266-byte HTML error page (not a tarball)
Cloudflare "Just a moment…" challenge
```

**Root causes (distinguish them — the fix differs):**

| Sub-cause | What happened | Fix |
|---|---|---|
| **Benign recompression drift** | Forge (codeberg/bitbucket/github) regenerates the tag tarball non-deterministically; bytes drift, tree is unchanged | Update the recipe `CHECKSUM_SHA256` **after the integrity gate**; seed the verified tarball to the mirror. e.g. quicklaunch, samedi, uploadit, windowtailor, lnlauncher |
| **Version removed upstream** | The pinned version is gone (HTTP 300/404) | Version-bump to the current release; also fix any hardcoded `SOURCE_DIR`. e.g. libdsk 1.5.8→1.5.22, libfossil rev bump |
| **Provenance changed** | Mirror re-imported (cvs2git), top-dir + bytes differ | Move `SOURCE_URI` to the canonical distfile, fix `SOURCE_DIR`. e.g. mksh → MirBSD distfile |
| **Recipe bug (not drift)** | `SOURCE_URI_2` used `$SOURCE_FILENAME_2` before it was defined → fetched a directory-listing HTML page | Reorder so the filename is defined first. e.g. png2ico |
| **Egress-blocked** | Cloudflare challenge / unreachable port | **Not fixable by recipe** — needs a browser-solved fetch or maintainer-supplied tarball. e.g. speed_dreams, retro |

**THE INTEGRITY RULE (non-negotiable).** Never bump a checksum on trust. Fetch the
tarball, confirm its sha256, **extract it and verify the in-tree version matches
the pinned tag/version** (`configure.ac` `AC_INIT`, a `.rdef` `app_version`, a
`manifest.uuid`, `MKSH_VERSION`, …) **before** updating the recipe or seeding the
mirror. A blind bump pins garbage or content of unknown provenance. The recovery
agents correctly **rejected** ports where the gate failed (mksh's re-import,
libfossil's HTML page) rather than bump them.

---

## Class 13: Hardcoded cross-compiler prefix

**Symptom:**
```
make: aarch64-unknown-haiku-g++: No such file or directory (Error 127)
```

**Root cause.** A recipe (or raw Makefile) hardcodes the `aarch64-unknown-haiku-`
cross prefix. On a **native** builder only unprefixed `gcc`/`g++` exist.

**Fix.** Pass native `CXX=g++`/`CC=gcc`. Class-wide this is cleared by #186
(`16fcbe5dda`), which puts the arm64 triplet compilers on the in-chroot PATH — so
a recipe that goes through `runConfigure`/`$cmakeDirArgs` inherits the working
compiler and needs nothing. **A raw Makefile that picks `CC`/`CXX` out of the
environment does not**: `freeimage` builds with `make -f Makefile.gnu`, the env
`CXX` resolves to `aarch64-unknown-haiku-g++`, and it dies `No such file or
directory (Error 127)`. Force the native compiler on the make command line —
command-line assignment overrides both the environment and the Makefile — gated to
the primary arch so an x86 secondary build keeps haikuporter's compiler:
```sh
case "$targetArchitecture" in
	x86_gcc2|x86) ;;
	*) nativeCC="CC=gcc CXX=g++" ;;
esac
make -f Makefile.gnu $nativeCC …
```
**Example ports:** re2 (#52), freeimage (raw-Makefile variant).

---

## Class 14: Poisoned build-tool package

**Symptom:**
```
runtime_loader: /boot/system/bin/bison: Unhandled pheader type in parse 0x6474e553
```

**Root cause — NOT the loader.** `0x6474e553` is `PT_GNU_PROPERTY`, which Haiku's
`elf.h` doesn't define — a true statement, but a red herring. The real problem:
`bison-3.8.2_bootstrap` and `bison_bootstrap-3.8.2` ship a **Linux x86-64 / aarch64
ELF** as `bin/bison` inside an `-arm64` hpkg. It can never run. Worse,
`3.8.2_bootstrap` **sorts above** the genuine `3.8.2`, so haikuporter resolves
`cmd:bison` to the poisoned one even when the good package is present.

**Fix.** Quarantine the poisoned packages out of the pool (the good one being
present is not enough). Diagnose with `readelf -l <bin> | grep interpreter`: a
genuine Haiku binary has **no `INTERP` segment**. Any port whose build runs
`bison` fails until the bad packages are gone (`sys-devel/jam` exposed this).

---

## Classes 15–17 (reserved, PR #309)

Classes **15** (CFLAGS override drops default optimization —
`runConfigure: Must specify optimization flags when overriding CFLAGS`, fix: add
`-O2`), **16** (duplicate symbol / `multiple definition of` — fix: `-fcommon` or a
real dedup), and **17** (CMake `FetchContent` offline fetch — pre-seed the dep) are
owned by **PR #309** (the re-triage-tail work). They are only reserved here so the
numbering stays stable across the two PRs. **The mining below deliberately does NOT
re-add them**, even though the miner sees their signatures in the UNMATCHED pile
(class 15 = 18 ports, class 16 = 13 ports as of the 2026-09-17 mining run) — see #309
for the class bodies.

---

## Class 18: Missing link-time library (`ld: cannot find -l<lib>`)

**Mined class (11 ports).**

**Symptom (link step):**
```
… /bin/ld: cannot find -lmidi: No such file or directory
… /bin/ld: cannot find -lscreensaver: No such file or directory
collect2: error: ld returned 1 exit status
```

**Root cause.** The recipe or upstream Makefile links a library whose **developer
symlink** (`lib<name>.so`, unversioned, under `/system/develop/lib`) is not in the
build chroot. Two sub-cases, distinguished by whether the library is a **Haiku
system** library or a **third-party** one:

- **Haiku's own libraries** — `libmidi`/`libmidi2` (MIDI kit), `libscreensaver`
  (screensaver kit), `libgame`, etc. Their runtime `.so` is in the base image, but the
  **unversioned devel symlink** ships in a *devel* package the recipe never declared.
  `-lGL` is the same shape (provided by a `libgl`/`mesa` devel package).
- **Third-party libraries** that genuinely are not in the arm64 pool — that is
  really [Class 20](#class-20-prerequisite-packagetool-not-published) (dep unpublished),
  not this class. Tell them apart by whether `find /system/develop/lib -name 'lib<x>.so'`
  exists on a booted image: present-but-unlinked ⇒ Class 18; absent ⇒ Class 20.

**Fix.** Declare the providing **`devel:` / `lib:` build requirement** so the devel
symlink is mounted into the chroot (e.g. the midi-kit devel package for `-lmidi`), or,
for a Haiku library whose devel symlink is simply missing from the pool, add the
symlink in the recipe’s `INSTALL()`/`BUILD()` before the link step. Do **not** “fix” it
by dropping the `-l` — that silently cuts the feature.

**Example ports:** allegro, billardgl, drumcircle, fbneo, internalmidi, lite_xl,
midikeyboard, rtmidi, symetrie, textsaver, wolle (wolle is also a
[Class 22](#class-22-removedrenamed-haiku-api-or-moved-header) narrowing case).

**PASS 2 sub-routing (measured — the fix differs by which library is missing).** The 10
link failures split cleanly by the `-l<name>` in the log:

- **`-lmidi`/`-lmidi2` (allegro, drumcircle, internalmidi, midikeyboard, rtmidi) and
  `-lscreensaver` (symetrie, wolle).** These are Haiku's own MIDI/screensaver kits. **The
  evidence says the runtime library itself is ABSENT from the lean image, not merely
  missing its devel symlink**: the sibling port `edgar` fails at *runtime* with
  `runtime_loader: Cannot open file libmidi.so (needed by libSDL2_mixer)`. If the `.so`
  is genuinely absent, a provisioner "create the devel symlink" overlay (the shape used
  for the #171 gnu-header fix) would point at a **nonexistent target** and still fail — so
  **no such overlay is shipped in PASS 2**. The correct fix must be decided on a booted
  builder: `find /system/lib /system/develop/lib -name 'libmidi*.so' -o -name
  'libscreensaver.so'` → **present-but-unlinked** ⇒ a devel-symlink overlay clears it;
  **absent** ⇒ the MIDI/screensaver kits are dropped by the image build profile (an
  image-composition fix in the bake, analogous to the minimum-build translator-library
  drop) or must be built+published (Class 20). Deferred to the hardware pass.
- **`-lGL` (fbneo, lite_xl) and `-lglut` (billardgl).** Third-party OpenGL/GLUT devel not
  in the arm64 pool ⇒ **[Class 20](#class-20-prerequisite-packagetool-not-published)**:
  build+publish the `mesa`/`libglvnd` and `glut` devel packages, then re-wave.
- **textsaver** clustered here by a different salient line (no `-l` captured in the tail);
  re-triage after the PASS 2 log-capture fix lands.

---

## Class 19: Build system does not recognise aarch64/arm64

**Mined class (8 ports).** Distinct from [Class 6](#class-6-configguess-cannot-name-the-arm64-host):
Class 6 is GNU `config.guess`/`config.sub`; **this** is the project’s *own* hand-rolled
architecture switch.

**Symptom (before or early in the build, from the project’s own logic):**
```
Makefile:40: *** "Platform '' not supported".  Stop.
Make.inc:448: *** "unknown word-size for arch: aarch64".  Stop.
[ERROR!] Cannot guess host type. You must specify one with the -host option.
Your platform is unsupported
```
(often preceded by the project echoing `UNAME_MACHINE = arm64`.)

**Root cause.** The project ships its own `uname -m`/`$(ARCH)` switch that enumerates
`i386`/`x86_64`/`ppc`/… and has **no `aarch64` (or `arm64`) case**, so an empty/unknown
arch variable aborts a `Makefile`, a `Make.inc`, or a bespoke `configure`. Haiku
reporting `arm64` (not `aarch64`) from `uname -m` compounds it. `-DLLVM_HOST_TRIPLE=`
and `runConfigure` do **not** help — the switch is inside the project, not autotools.

**Fix.** Patch the project’s arch/platform detection to add an `arm64`/`aarch64` branch
(map it to the closest existing 64-bit little-endian target, usually the `x86_64`
codegen path plus `-DHAVE_*` word-size defines the project keys off), gated to the
primary arch. For a bespoke `configure` that only accepts `--host`, pass
`--host=aarch64-unknown-haiku` via `runConfigure`.

**Example ports:** glew, glew2.1, imagetoicon, jamvm, julia, ocaml, tbb2018.5,
vncserver.

---

## Class 20: Prerequisite package/tool not published

**Mined class (~30 ports).** This is the **dependency-not-published** root cause
([build_state=built ≠ published](#build_statebuilt--published)) surfacing under
haikuporter’s *native* wording rather than the literal `UNRESOLVABLE` marker — which is
exactly why these evaded the `haiku-triage-failures` `has_unresolvable` filter and
landed in UNMATCHED. **They are NOT recipe/code defects.** The fix is to build **and
publish** the dependency, or provision the host tool — never a source edit.

**Three signatures, one root cause:**
```
Error: unable to resolve required packages for build for <port>
    Reason: build-requires "devel:libsdl3" of package "<port>" could not be resolved
Error: unable to resolve prerequired packages for build for <port>
    Reason: build-prerequires "cmd:dot" of package "<port>" could not be resolved
Package 'python3', required by 'virtual:world', not found     (pkg-config, no .pc)
configure: error: libgcrypt not found on system / libgcrypt is too old
```

**Root cause & fix by sub-signature:**

| Sub-signature | What is missing | Fix |
|---|---|---|
| `unable to resolve … "devel:X"` | a library dep not built+published in the arm64 pool | build & **publish** `X` to green, then re-wave the consumer |
| `unable to resolve … "cmd:Y"` | a build **tool** not in the pool (`cmd:dot`/graphviz, `cmd:makeinfo`) | build & publish the tool package (or, for a host-only fetch tool, [Class 24](#class-24-source-acquisition-tool-missing-on-the-builder-host)) |
| pkg-config `Package 'X' … not found` | dep present but no `.pc`, or dep truly absent | publish the dep’s devel package (ships the `.pc`); `python3.pc` ⇒ publish python devel |
| `libgcrypt not found` / dropped `libgcrypt-config` | configure macro removed upstream | **deferred-hard** — needs configure-macro porting (see deferred list; libotr/gsasl) |

**Example ports:** crawl, devilutionx, ecwolf, endless_sky, eternal_lands, exiv2
(`cmd:dot`), ffmpeg6, grafx2, gst_plugins_bad, hikounomizu, libjxl, lighttpd, lugaru,
minetest, ocp (`devel:libsdl3`), python3.13, qemu, radare2, spice, vice, warzone2100
(resolve-step); bezilla, libdvdnav, lsdvd, mailnews, poezio (pkg-config); gsasl,
libaacs, libbdplus, libsigrokdecode (configure).

---

## Class 21: Python setup.py needs setuptools / `pkg_resources`

**Mined class (4 ports).**

**Symptom (packaging step, after the source is staged):**
```
Traceback (most recent call last):
  File "…/setup.py", line 18, in <module>
    import pkg_resources
ModuleNotFoundError: No module named 'pkg_resources'
```

**Root cause — CORRECTED IN PASS 2 (measured against the real logs, do not trust the
original one-liner).** The obvious reading — "the recipe forgot to declare setuptools" —
is **wrong for our builder**. The upstream `html5lib`/`pbr` recipes *already* declare
`setuptools_python310` in `BUILD_REQUIRES` and `cmd:python3.10` in `BUILD_PREREQUIRES`,
and `haiku-nativebuild` even logs `attempt 1 installing: setuptools_python310`. The build
still dies in `setup.py` with `ModuleNotFoundError: No module named 'pkg_resources'`. The
real cause is a **python-version mismatch**: these recipes pin `PYTHON_VERSIONS=(3.10)`,
but the DeBeOS native builder ships **python3.14** (see `haiku-provision-native-builder`
section 1). `setuptools_python310` installs `pkg_resources` onto **3.10's**
vendor-packages path, which is not on the **3.14** interpreter's `sys.path` — so the
import fails. The two remaining "Class 21" ports are not even setuptools: **librnp**
needs `distutils` (removed from stdlib in 3.12, now a setuptools shim) and **libplacebo**
needs `jinja2` (a real codegen dependency), so the classifier regex was broadened in PASS
2 to `No module named '[^']+'`.

**Fix (per-recipe overlay + a Class-20 publish, verify on hardware — NOT a one-liner).**
Bump the recipe's `PYTHON_VERSIONS` to `3.14` (and the `setuptools_python314` /
`jinja2_python314` build-requires to match), which requires those `_python314` packages
to be **built and published** to the arm64 green pool first (a Class-20 dependency, not a
recipe edit). Whether modern setuptools still ships `pkg_resources` importably must be
confirmed on a builder — several ports' `setup.py` use the deprecated
`from pkg_resources import parse_version` and may additionally need a source patch. This
class therefore straddles per-recipe (version bump) **and** Class 20 (publish the
`_python314` deps) and is left for the hardware pass rather than shipped as a fabricated
overlay.

**Example ports:** html5lib, pbr (pkg_resources), librnp (distutils), libplacebo (jinja2).

---

## Class 22: Removed/renamed Haiku API or moved header

**Mined class (7 ports).** Real source porting (some genuinely
[deferred-hard](#ports-deferred-as-hard-not-this-playbooks-quick-win-classes)), grouped
because the root cause is one thing: **the port targets a Haiku/BeOS API that current
Haiku renamed, removed, or moved.**

**Two symptoms:**
```
… /interface/SplitLayoutBuilder.h:30: error: 'BSplitView' does not name a type; did you mean 'SplitView'?
src/ip.cpp:165: error: 'host' was not declared in this scope
   -- or a header that moved / needs an include path --
src/MapsData.h:12: fatal error: UrlRequest.h: No such file or directory
/boot/system/develop/headers/bsd/stdlib.h:9: fatal error: stdlib.h: No such file or directory  (#include_next)
```

**Root cause.** A removed/renamed symbol (`BSplitView` → `BSplitLayoutBuilder`
semantics changed), or a header that moved into a **private** subtree
(`<UrlRequest.h>` now under `headers/private/netservices`) or needs the standard-C
include path repaired (a `makefile_engine` app that clears the default `-I` so a
`bsd/stdlib.h` `#include_next <stdlib.h>` can’t find the next header).

**Fix.** Update the source to the current API (a `PATCH()` hunk), or add the missing
include path (`CPPFLAGS+=-I/system/develop/headers/private/netservices`, or restore the
default C include dir for a makefile_engine app). Where the API delta is large, mark it
**deferred-hard** rather than half-porting it.

**Example ports:** album (`BSplitView`), mda_vst, workspacenumber, zeromq (removed
symbol); ducksaver, maps, sdl_gfx (moved/missing header). Many of the truncated-tail
`makefile_engine` legacy BeOS apps (batchrename, bemines, bescreencapture, dockbert, …)
are very likely this class — their root error scrolled off the captured log tail.

---

## Class 23: Install writes to read-only /packages

**Mined class (3 ports).**

**Symptom (packaging/harvest step, after the build succeeded):**
```
/packages/pystring0_devel-1.1.3_git-3/.self
mkdir: cannot create directory '/packages/pystring0_devel-1.1.3_git-3': Read-only file system
```

**Root cause.** The `_devel` (or another split) subpackage harvest tries to `mkdir`
directly under the **read-only packagefs mount** `/packages` instead of the recipe’s
work/install dir. Several cases carry a **malformed subpackage identifier** — note the
stray trailing space in `libu2f_server_devel-1.1.0-6 ` — which throws the harvest off
its expected path. The binary is already built; only packaging fails.

**Fix (per-port — needs a look at the recipe).** Correct the subpackage identifier
(strip stray whitespace in `PROVIDES`/version), and ensure the split/`INSTALL()` writes
under `$developInstallDir`/`$prefix`, not an absolute `/packages/...` path. Verify by
re-running the harvest, not just the compile.

**Example ports:** libu2f_server, pystring0, wrapt.

---

## Class 24: Source-acquisition tool missing on the builder host

**Mined class (4 ports).** The HOST-side analogue of
[Class 2](#class-2-undeclared-build-tool-in-the-haikuporter-chroot) (which is the
*chroot* nuance): the tool is needed to **fetch/unpack the source**, which runs on the
builder host *before* the chroot exists, so a `cmd:` prerequisite cannot help.

**Symptom (during download/unpack, before any compile):**
```
Downloading: hg+https://bitbucket.org/…
Error: 'hg' is not available, please install it
   -- or --
Downloading: https://aminet.net/dev/asm/ira.lha …
Error: 'lha' is not available, please install it
```

**Root cause.** The recipe’s `SOURCE_URI` uses a VCS or archive format (`hg+`, `svn+`,
`bzr+`, `.lha`) whose client (`hg`, `svn`, `bzr`, `lha`) is not installed on the
builder host.

**Fix.** Add the tool to the host install list in
`graviton/scripts/haiku-provision-native-builder` (exactly as `dos2unix` was added for
the Class 2 host case), **not** as a recipe `cmd:` prereq. Prefer, where possible,
moving the recipe to a static archive download with a checksum (the wave tooling already
warns `UNSAFE SOURCES … SHOULD NOT BE USED`).

**Example ports:** cube2tesseract, ira (`lha`), previous, xemacs (`hg`).

---

## Triage-tool signature gaps (found while mining) — CLOSED in PASS 2

The mining run turned up ports that **belong to an existing class but the
`haiku-triage-failures` ruleset missed**, because the live log wording differed from the
encoded regex. These are ruleset bugs, not new classes — fixing them shrinks UNMATCHED
without any porting work. **All six were closed in the PASS 2 ruleset edit (#136/#90)**
and each was verified against the real cached log for the named port(s):

| Existing class | Live signature the regex missed | Ports | Fix (PASS 2) |
|---|---|---|---|
| **has_unresolvable** (dep filter) | `unable to resolve (pre)required packages …` (no literal `UNRESOLVABLE` token) | ~21 (see Class 20) — **the biggest gap** | new **Class 20** rule (+ the literal token) routes them; `_UNRESOLVABLE` regex broadened |
| **Class 4** static/shared | `prepareInstalledDevelLib error: …` (singular, not `prepareInstalledDevelLibs`) | linenoise, libspectrum, sdl_net | regex → `prepareInstalledDevelLibs?` |
| **Class 10** autotools aux | `configure.in/.ac: … required file 'X' not found` (regex keyed on literal `install-sh`) | libmcrypt (`ltmain.sh`), libsrtp (`ar-lib`) | added `required file '[^']+' not found` |
| **Class 3** gettext macro | `possibly undefined macro: AM_PATH_XML2` (libxml2’s macro off the aclocal path) | libmetalink | added `AM_PATH_XML2` to the macro alternation |
| **Class 5** x86 flags | `-mmmx` / other x86 `-m*` flags (regex keyed on `-msse`) | opendune | added `-mmmx`/`-m3dnow`/`-mavx` |
| **Class 2** missing tool | `Could NOT find UnixCommands (missing: GZIP)` (tool absence via CMake) | zziplib | added `Could NOT find UnixCommands` (NARROW — `Could NOT find <Lib>` for a real dep, e.g. partio's GLUT, is Class 20, not this) |

`haiku-playbook-parity`’s hard gate now passes with **21** classifier classes (1–14 +
18–24) all resolving to a playbook section; the previous “documented-but-not-detected”
INFO for 18–24 is gone. Re-running the classifier + miner (the operator’s rebake→re-wave
step) will re-tag these out of UNMATCHED — measured locally against the 563 cached
UNMATCHED logs, this pass reclassifies **384** of them — 37 into actionable
code/recipe/tool classes and 347 into Class 20 (dep-unpublished) — leaving 179 (see the
PASS 2 census below).

---

## Small/emerging clusters (mined, below class threshold)

Real but too small (or too opaque) to codify as a class yet — recorded so they are not
re-derived. Add a class if a future wave grows one past ~4 ports:

- **autotools modernization (2):** `configure.ac: AC_CONFIG_MACRO_DIR can only be used
  once` / `AM_INIT_AUTOMAKE expanded multiple times` — the port’s `configure.ac` is
  written for an older autotools and the builder’s `autoconf-2.72`/`automake-1.18`
  reject the double expansion. Fix: patch out the redundant macro invocation
  (mechanically similar to Class 3’s rpcsvc_proto variant). *fswatch, irrxml.*
- **catkeys/locale link (2):** `couldn't load source-catalog ….catkeys — error: Bad
  data` from `linkcatkeys`, *after* the binary built (exit 255). A Haiku locale-tool
  parse failure on a specific `.catkeys`. *unreal_speccy_portable, wpa_supplicant.*
- **recipe patch file missing (2):** `Error: patch file "…" not found` — the recipe’s
  `PATCHES=` references a file absent from the port’s `patches/` dir. *criticalmass,
  veesem.*
- **meson without `--buildtype` (1):** `error: invoking meson without --buildtype
  argument` — the meson analogue of Class 8; a systemic fix would default it in
  haikuporter’s meson wrapper. *vmaf.*
- **library archiver / OpenMP / endian (1 each):** `working library archiver is
  required` (blis), `OpenMP … not supported` (libimagequant), `#error Neither
  LITTLE_ENDIAN nor BIG_ENDIAN` (unrar — real endian source porting).

---

## How classes 18–24 were mined — the UNMATCHED frontier

Classes 18–24 above were **not** hand-derived; they were mined from the real failure
backlog by `graviton/scripts/haiku-pattern-miner`, so the playbook grows from data
instead of archaeology. The method, and the numbers from the 2026-09-17 run
(#90/#136):

- **Input.** `haiku-triage-failures` left **563** ports at `triage_class=UNMATCHED`. Of
  those, **317** carry the dependency-not-published marker (`UNRESOLVABLE`) and are
  excluded — not code patterns. That leaves **246** real un-captured failure signatures.
- **Method.** For each of the 246 the miner fetches the S3 build log, extracts the most
  specific failure line, normalises it (strips paths, versions, hex, line numbers, the
  port’s own name, `-l<lib>` names), clusters by signature, and ranks by port count.
- **Result.** **87 of the 246 (35%) are truncated-tail** — the captured log is only the
  *tail* of the native build, so the root compiler error scrolled off the top and the
  log exposes only the generic `make: *** Error N` cascade. These are **not
  characterisable** and were left as one honest bucket, not invented into classes.
  *(Operational fix — DONE in PASS 2: `haiku-nativebuild` used to echo only
  `tail -20 "$log"` on failure, and `run_ssm._wrapper` gzips exactly that stdout as the
  S3 `log_url` — so the root error, which is at the TOP of a large build, was thrown away
  before it ever left the scratch builder. PASS 2 replaces both failure emissions with an
  `emit_log_capture` that prints the log HEAD + every diagnostic line (grep) + the TAIL,
  bounded so a huge ninja build doesn't balloon the upload. Verified: on a synthetic
  6051-line log with the root error at line 50, the miner's own `salient_line` recovers
  it from the new capture. This makes the 87 truncated-tail ports minable on the next
  re-wave.)*
- **The remaining ~159 clustered into the classes above.** Net new fixable classes
  yielded by the frontier: **7** (classes 18–24), covering **≈67 ports** — plus ~30 more
  routed to the dep-unpublished root cause (Class 20 / the `has_unresolvable` gap) and
  ~11 folded into real source porting (deferred-hard). Re-run any time:
  ```sh
  AWS_PROFILE=haiku-graviton graviton/scripts/haiku-pattern-miner --out /tmp/miner.json
  ```
  and let `graviton/scripts/haiku-playbook-parity` nag when a new cluster crosses the
  threshold with no matching class.

---

## PASS 2 — systemic fixes, log capture, and the honest per-class ledger (#136/#90)

PASS 2 acts on the mining above. It is **code-first**: every change here is committed and
verified as far as it can be **without** a builder (regex parity, shell syntax, and the
classifier replayed against the 563 cached UNMATCHED logs); the native `RC=0` and the
re-tag of DDB are **OWED** to the operator's rebake→re-wave, which is a separate step.

**What shipped (and how it was verified):**

1. **Classifier now detects classes 18–24** (`haiku-triage-failures` RULES) **and the six
   signature gaps above are closed.** Replayed locally against the 563 cached UNMATCHED
   logs, PASS 2 reclassifies **384** (563 → **179** still UNMATCHED): class 2 ×1
   (zziplib), 3 ×1 (libmetalink), 4 ×3, 5 ×1 (opendune), 10 ×2, 18 ×10, 19 ×7, 20 ×347,
   21 ×4, 22 ×1 (album), 23 ×3, 24 ×4. Of the 384, **37 are actionable
   code/recipe/tool classes** and **347 are Class 20** (317 with the literal
   `UNRESOLVABLE` marker + ~30 under haikuporter's native resolve wording / pkg-config /
   libgcrypt — all auto=NO, *publish the dep*, not a recipe edit). The 179 residual
   UNMATCHED are the 87 truncated-tail (minable after fix #2) plus ~92 genuine one-off
   source ports. `haiku-playbook-parity` hard gate passes (21 classes resolve).
2. **Wave-logger capture depth** (`haiku-nativebuild` `emit_log_capture`) — reclassifies
   the 87 truncated-tail ports on the next re-wave (see the mining note above).
3. **Class 24 host source-acq clients** — systemic provisioner add
   (`haiku-provision-native-builder` §1a: tolerant install of mercurial/subversion/breezy/
   lha/cvs on the HOST). Clears cube2tesseract, ira, previous, xemacs (**4**) on rebake.

**Per-class clearable ledger (measured this pass) — systemic vs per-recipe vs deferred:**

| Class | Ports (count) | Nature | PASS 2 disposition |
|---|---|---|---|
| 24 source-acq tool | cube2tesseract, ira, previous, xemacs (4) | **systemic** | provisioner §1a shipped; clears on rebake |
| 20 dep-not-published | ~347 (incl. GL/glut from Class 18, the gtk+/libdvdcss/libgcrypt configure/pkg-config cases) | **not code** | classifier routes them (auto=NO); fix = build+**publish** the dep to green, then re-wave |
| 18 `-lmidi/-lmidi2/-lscreensaver` | allegro, drumcircle, internalmidi, midikeyboard, rtmidi, symetrie, wolle (7) | **image-profile / hardware-gated** | evidence says the kit `.so` is absent (edgar runtime_loader); needs a booted-builder check → symlink overlay **or** image-profile/Class-20. Deferred |
| 19 own-arch switch | glew, glew2.1, imagetoicon, tbb2018.5 (mechanical) ; julia, ocaml, jamvm, vncserver (hard) | **per-recipe source** | per-port arch-branch patch; several deferred-hard (codegen). Needs upstream build system + hardware verify |
| 21 python-module | html5lib, pbr, librnp, libplacebo (4) | **per-recipe + Class 20** | recipe already declares setuptools; real cause = builder python 3.14 vs recipe-pinned 3.10 → bump `PYTHON_VERSIONS`=3.14 **and** publish the `_python314` deps. Deferred to hardware |
| 22 removed/renamed API | album (`BSplitView`) + moved-header/`does not name a type` residue | **per-recipe source** | mostly deferred-hard real porting; classifier narrowly detects `does not name a type` only |
| 23 read-only `/packages` | libu2f_server, pystring0, wrapt (3) | **per-recipe** | per-port subpackage-identifier fix (strip stray whitespace in PROVIDES/version); needs the recipe + a harvest re-run to verify. Deferred |

**Honest headline.** The ≈67 ports the miner attributed to classes 18–24 are **dominated
by Class-20 dep-publish work and genuinely-hard per-port source ports** — the clean,
verifiable, ship-now fix is the **Class 24 provisioner add (4 ports)** plus the two
tooling changes (classifier detection of all 7 classes; the log-capture that unblocks the
87 truncated). The per-recipe overlays for 19/21/22/23 were **deliberately not fabricated
here**: measured against the real logs their "obvious" one-liner fixes are wrong or
incomplete (Class 21 is a python-version/pool problem, not a missing `BUILD_PREREQUIRES`;
Class 18's midi libs appear absent, not unlinked), so they are enumerated precisely above
for the hardware pass rather than shipped as unverifiable recipe files.

**Deferred-hard (needs more than a recipe one-liner, confirmed this pass):**
julia/ocaml/jamvm (Class 19 codegen ports), the moved-header/removed-API Class-22 residue,
the libgcrypt-config configure-macro ports (gsasl/libotr/libaacs/libbdplus, routed to
Class 20), and the Class-18 midi/screensaver image-profile question.

---

## DeBeOS overlay-recipe convention

- **Recipes live in `graviton/haikuports-patches/recipes/`** as **complete recipe
  files**, not diffs. The sibling `*.patch` files are **explanatory notes** — some
  have illustrative hunk headers (`@@ BUILD()`) and are **not appliable by
  `patch(1)`**. Read the patch for *why*; **apply the recipe.**
- **Where a recipe goes on a builder** — the input-source-package path silently
  overrules the ports tree:
  ```
  input-source-packages/develop/sources/<port>-<ver>-<rev>/<port>-<ver>.recipe
  ```
  Editing the ports-tree copy has **no effect and no warning** when an ISP exists.
  Check the `<source-package>::` line in the haikuporter log. Ports with **no**
  ISP (ruby, jam, libsdl2, openal, mesa, tk, …) use the tree copy directly.
- **The mtime trap.** haikuporter re-extracts (silently reverting your edit)
  whenever `mtime(recipe) <= mtime(sourcePackage)`. After editing, pin the mtime
  forward of the source package — computed from the package's own mtime, not the
  wall clock:
  ```sh
  touch -d @$(( $(stat -c %Y "$srcpkg") + 172800 )) "$recipe"
  ```
  Ports without an ISP need no pin (nothing to compare against).
- **Harvest immediately.** A working edit that lives only on a builder's disk is
  not saved — builders are scratch VMs, recreated on every image rebuild. Three
  versions of the autoconf cut and the entire ruby recipe were lost this way. The
  moment an edit is proven, copy it back into `recipes/` and commit.
- **`haikuporter -G` for every non-ISP port.** A downloaded-tarball port makes an
  implicit git repo and aborts if `git` is absent; `-G` (`--no-git-repo`) uses
  `patch(1)` instead.

---

## Graviton3/4 ISA opt-in (per-recipe, NOT the baseline)

This is the **opt-in path for ML and codec ports** — llama.cpp / ggml, OpenBLAS,
libjpeg-turbo, x264, and the like — to build with the Graviton3 (Neoverse-V1) or
Graviton4 (Neoverse-V2) instruction set. It is deliberately **per-recipe**. The
system `-mcpu` baseline (`build/jam/ArchitectureRules`, `-mcpu=neoverse-n1+crypto`)
and the userland `armv8.2-a` floor **do not change** — the OS and base packages stay
bootable on Graviton2 and t4g (Neoverse-N1). Only a port that explicitly asks for it
gets the newer ISA.

### What the newer cores add

Neoverse-V1 (G3) and Neoverse-V2 (G4) are ARMv8.4-A supersets of N1's ARMv8.2-A.
Over the baseline they add, among others:

- **BF16** (`FEAT_BF16`) — NEON `BFMMLA`/`BFDOT`/`BFCVT`.
- **I8MM** (`FEAT_I8MM`) — NEON integer matrix-multiply `SMMLA`/`UMMLA`/`USMMLA`.
- **SVE** (V1) / **SVE2** (V2) — scalable vectors.

All three are wins for ML kernels: ggml/llama.cpp and OpenBLAS have hand-written
aarch64 microkernels for both the NEON MMLA/BF16 path and the SVE path.

**SVE is supported here — that changed.** Older notes in this tree
(`graviton/docs/graviton-optimization-plan.md` "The `-mcpu` question, settled" and
`haikuports-patches/codec-tier-arm64.md` "No SVE") say SVE traps because the kernel
never cleared `CPACR_EL1.ZEN` and had no SVE save/restore. **That is stale.** Commit
`50f6a9be53` ("arm64: enable SVE with EL0-only, off-stack context save/restore",
#88) enables SVE for userland: `arch_sve_init_percpu()` (`arch_cpu.cpp`) clears the
`CPACR_EL1.ZEN` trap on every SVE-capable core, the EL0 exception path in
`arch_asm.S` saves/restores each thread's Z/P/FFR, and fork + signal frames carry the
SVE state (`arch_thread.cpp`). The effective vector length is clamped to
`SVE_MAX_VL_BYTES` = 32 (256-bit), which covers Graviton3 (256-bit SVE) and Graviton4
(128-bit SVE2); SVE code is vector-length-agnostic, so a clamp only caps width, it
does not miscompile. Userland SVE is exercised by `src/bin/sve_test/` (incl.
`sve_fork_test.c`). The kernel itself still runs **NEON-only at EL1** and never
executes SVE, so none of this touches the N1 baseline above. So a userland ML port
**may** emit SVE now — you do not suppress it.

### The idiom

Add this to the port's `BUILD()` (or its CMake/configure flag list), gated on
arm64, **before** the upstream build reads `CFLAGS`/`CXXFLAGS`:

```sh
# DeBeOS Graviton3/4 ISA opt-in (see graviton/docs/porting-playbook.md).
# Neoverse-V1 = Graviton3; use neoverse-v2 for a Graviton4-only (_g4) variant.
# Enables ARMv8.4-A + BF16 + I8MM + SVE/SVE2; NOT portable to Graviton2/t4g.
case "$targetArchitecture" in
	arm64)
		debeosMcpu="-mcpu=neoverse-v1+crypto"
		export CFLAGS="$CFLAGS -O2 $debeosMcpu"
		export CXXFLAGS="$CXXFLAGS -O2 $debeosMcpu"
		;;
esac
```

Two things the flag string is doing, each load-bearing:

1. **`neoverse-v1`** (or `neoverse-v2` for G4) selects the ARMv8.4-A core, which is
   what turns on BF16, I8MM and SVE/SVE2.
2. **`+crypto` is re-carried, not inherited.** A second `-mcpu` on the command line
   **fully replaces** the baseline `-mcpu=neoverse-n1+crypto` (last `-mcpu` wins),
   and `crypto` is never a compiler default — omit it and the port silently loses
   AES/SHA/PMULL. Always spell `+crypto`.

Keep the `-O2` — haikuporter's `runConfigure` rejects an overridden `CFLAGS` that
carries no optimization level (playbook Class 15), and appending rather than
replacing preserves any flags the recipe already set.

### These are NOT fleet-portable packages — name them `_g3` / `_g4`

A binary built this way uses SVE and ARMv8.4 NEON instructions (i8mm, bf16) that
**Neoverse-N1 does not implement**, so it will **fault on Graviton2 and t4g**. That
is expected and is the whole reason this is opt-in rather than the baseline. Such a
package is a distinct, non-default variant: give it a `_g3` (Neoverse-V1) or `_g4`
(Neoverse-V2) suffix in its package name / revision so it is never confused with the
fleet-portable default, and only install/publish it where the target is known to be
Graviton3+ (respectively Graviton4). Do not promote a `_g3`/`_g4` build into the
default green pool as the plain package.

### Portable middle tier: `-mtune` only (no ISA change)

If a port wants Neoverse-V1/V2 **instruction scheduling** but must still run on the
whole fleet, change tuning without changing the ISA:

```sh
export CFLAGS="$CFLAGS -O2 -mtune=neoverse-v1"    # runs on G2/t4g too; no new ISA
```

`-mtune` never raises the required instruction set (codec-tier-arm64.md: "`-mtune`
is the safe knob"), so the result stays a normal fleet-portable package with no
`_g3`/`_g4` suffix. Expected payoff on non-vector code is small; measure before
shipping.

### Verification owed

The flag string is derived from the GCC 13.3 AArch64 manual; a live native
compile-check on a Graviton builder — build an opting port, then `objdump -d` the
shipped `.so` and confirm the intended extension is present (`smmla`/`ummla` for
I8MM, `bfmmla`/`bfdot` for BF16, `ptrue`/`whilelo`/`z<n>.` operands for SVE) — plus a
runtime smoke test of the `_g3`/`_g4` package **on Graviton3/4 hardware** is **owed**
before publishing, per the "success is a disassembled `.so` (and a run), not an exit
code" rule that `codec-tier-arm64.md` established for this exact class.

---

## build_state=built ≠ published

Two distinct states, and conflating them is a recurring error:

- **`build_state=built`** in `debeos-package-state` (DDB) means a native builder
  produced a non-empty `.hpkg` and harvested it to
  `s3://haiku-graviton-<acct>-<region>/hpkg/arm64/`. That is **all** it means.
- **Published** means the hpkg is in the **green** package pool
  (`debeos-repo-green/` prefix, served by `packages.debene.dev`) with a refreshed
  repo index, so `pkgman` on a booted instance can install it. The wave PRs
  explicitly note "recipes are build-verified but **not published to green**;
  publishing is out of scope." A consumer port stays `UNRESOLVABLE` until its dep
  is *published*, not merely *built* — several #276/#277 "skipped" ports are
  blocked exactly here (`devel:libvorbis`, `devel:xproto` built-but-unpublished).
- After a wave, ports are **requeued** (`build_state=queued`, `attempt_count=0`,
  `requeued_by=…`) so the pipeline re-runs them; that is orthogonal to publishing.

---

## Builder hygiene

- **Parallelism: cap at `-j16` (#262).** A large ninja build (llvm21) at `-j64`
  dies at a random target with `ninja: fatal: waitpid(…): No child process` — an
  ECHILD child-reaping race on Haiku under high parallelism, **zero compile
  errors**. `-j16` built the full 6633-target project clean in one pass. Cap
  parallelism or wrap big builds in a resume loop; don't misread the race as a
  build defect.
- **Disk: launch at the final root size, don't `--disk`-override a canonical
  builder (#254).** A builder launched with a `--block-device-mappings` disk
  override (e.g. 40 GiB) boots healthy but is **unreachable** — the first-boot
  `partition_grow` races the network-gated `sshd`/SSM `launch_daemon` jobs, so no
  control channel ever comes up. A default-disk (20 GiB) builder is Online in ~1
  min. Real builds still want a big root, so provision it at launch rather than
  growing on first boot, until the grow is sequenced before the network jobs.
- Native builders are provisioned with `haiku-provision-native-builder` (seeds the
  download shims and applies `haikuporter-cmake-policy-minimum.patch`), driven over
  SSM. Reap the builder after the run.

---

## TARGET-SPEC template

Copy this block when triaging a new failed port. Fill it as you go; once the port
builds, promote the resolved fix into the matching class table above.

```
# TARGET-SPEC: <port>-<version>
port:            <category>/<port>            # e.g. media-libs/opencc
version:         <version>
recipe path:     ports-tree | ISP  (check the <source-package>:: log line)
builder AMI:     <canonical ami-…>   instance: <c8g.4xlarge etc>   -j: 16

## 1. Failure signature (paste the exact fatal log line)
<log line>

## 2. Class (grep this playbook for the signature)
class:           <1-14 or NEW>
matched example: <port that failed the same way>

## 3. Root cause (one sentence — WHY, not what)
<…>

## 4. Fix (the recipe delta actually applied)
<BUILD_PREREQUIRES additions / BUILD() edit / checksum+seed / …>

## 5. Integrity + non-cut check
[ ] not a feature cut (or: cut justified + separately tracked)
[ ] (Class 12 only) tarball fetched, sha256 confirmed, extracted, version verified
[ ] built RC=0 AND .hpkg is non-empty with expected bin/ (Class 9 guard)

## 6. Evidence
built hpkg:      <name>-<ver>-<rev>-arm64.hpkg  (<size>)
harvested to:    s3://…/hpkg/arm64/
requeued:        build_state=queued  requeued_by=<…>
published?:      no  (built ≠ published — see §build_state)

## 7. If NOT fixed — why (for the maintainer)
[ ] dep unbuilt/unpublished in repo (name it)
[ ] source unreachable / egress-blocked
[ ] real source porting (BSD types, endian, ncurses, -Werror, …)
```

---

## Ports deferred as hard (not this playbook's quick-win classes)

For completeness, the wave PRs classified the residue so the next operator
doesn't re-triage it. These need more than a recipe one-liner:

- **Dep unbuilt/unpublished in the repo** (`UNRESOLVABLE`): the leaf dependency
  must be built and *published* first — e.g. neverball (`devel:libvorbis`), the
  xproto consumers (`devel:xproto`), the glib2/audiofile/libavcodec chains.
- **Real source porting:** BSD integer types (`u_int32_t`, nogravity), endianness
  (`BYTE_ORDER`, redis), ncurses `WINDOW` (retawq), `-Werror`/narrowing
  (sawteeth, yaml_cpp test files → `-DYAML_CPP_BUILD_TESTS=OFF`), dropped
  `libgcrypt-config` (libotr/gsasl → needs configure-macro porting).
- **Egress-blocked source** (Class 12 tail): Cloudflare challenge (speed_dreams),
  unreachable port (retro).
- **Arch has no code path (masquerades as a flag gate):** embree 3.12.2 — the
  `-msse2` Class-5 signature is real, but the only backend is x86 SIMD; NEON
  landed in embree 3.13, so this is a version bump + real port, not a one-liner.

---

## Small-class frontier status (#136)

The low-count auto-recoverable classes, and what clears on a **rebake +
re-wave** (recipe committed to the overlay, native `RC=0` still OWED — it needs
the builder AMI rebaked with these recipes and the ports re-waved):

| Class | Ports (count) | Recipe status | Clears on re-wave |
|---|---|---|---|
| 4 static/shared install | libtomcrypt, libtommath, libvterm (3) | committed (#277) | 3 |
| 5 x86 flags | nesalizer (committed #276); **embree deferred-hard** | 1 committed | 1 (embree deferred) |
| 6 config.guess arm64 | autoconf2.71 (committed #275); **haiku_format, keystone (this PR)** | 3 committed | 3 |
| 7 LTO no plugin | draco (committed #275); **unarr (this PR)** | 2 committed | 2 |
| 10 autotools aux | bonnie++ (committed #275); **screen (this PR)** | 2 committed | 2 |
| 11 optional install target | musicpc (committed #276) | committed | 1 |
| 13 hardcoded cross prefix | **freeimage (this PR)** | committed | 1 |

**13 of the 14 small-class ports clear on the next rebake+re-wave; embree (1) is
deferred-hard.** This PR adds the five new overlay recipes (classes 6, 7, 10, 13);
the rest were committed in the earlier wave PRs #275–#280.

---

## Secondary-blocker tail (#136 re-wave of classes [3,8,4,6,7,10,11,13])

The 2026-09-17 re-wave (`wd-triage-r1-*`) rebuilt the auto-recoverable backlog
with the class fixes applied. This section re-triages the **14 ports whose
recorded class fix took but that then failed on a *different*, secondary
blocker** — the whole point being to scope whether one more
systemic-fix→rebake→re-wave pass is worth it for the tail. Every row below is
measured against that port's own re-wave log (`s3://haiku-graviton-<acct>-<region>/logs/wd-triage-r1-*/<port>.log.gz`),
not inferred.

**Headline: the tail is mostly still cheap.** Of 14, one is a *false failure*
(already built), ~7 collapse into two new systemic-ish classes plus re-application
of existing classes 2/5 and a class-7 hardening, three are genuine per-recipe
source ports (two of them one-liners), one is a captured-log gap, and one CMake
FetchContent case needs a dep pre-seed. Only `nogravity` is genuinely hard. **One
more pass is worth it.**

### Port → prior class (fixed) → current blocker → proposed fix

| Port | Prior class (took) | Current blocker signature | Proposed fix | New class? |
|---|---|---|---|---|
| axel | 3 gettext | `runConfigure: Must specify optimization flags when overriding CFLAGS` — overlay's own `CFLAGS=-Wno-error runConfigure …` lacks `-O` | add `-O2`: `CFLAGS="-O2 -Wno-error"` | **NEW class 15** |
| dovecot | 3 gettext | same — overlay's `CFLAGS=-D_BSD_SOURCE runConfigure …` lacks `-O` | `CFLAGS="-O2 -D_BSD_SOURCE"` | **NEW class 15** |
| freegish | 8 build-type | `ld: multiple definition of 'fwrite2'/'fread2'` across TUs | build with `-fcommon` (GCC10 `-fno-common` default) | **NEW class 16** |
| libmirage | 8 build-type | `ld: multiple definition of 'crc32_d8018001_lut'/'crc16_1021_lut'/'ecma_130_scrambler_lut'` | `-fcommon` | **NEW class 16** |
| lensfun | 8 build-type | `c++: error: unrecognized command-line option '-msse'/'-msse2'` | **existing Class 5** — gate SSE flags to x86 (`cpuid.cpp` path) | no (class 5) |
| keystone | 6 config.guess | `CMake Error … Unable to find Python interpreter` (llvm/CMakeLists.txt:340) | **existing Class 2** — the overlay's `cmd:python` is commented out; declare it (or `-DPYTHON_EXECUTABLE`) | no (class 2) |
| unarr | 7 LTO (IPO=OFF) | **identical** `cc1: error: LTO support has not been enabled`, no hpkg | class-7 fix **insufficient**: `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF` (a *cache default*) is overridden by an explicit per-target IPO property; harden by sed-ing `-flto` out / `-fno-lto` in `CMAKE_C_FLAGS`, then rebake-verify | no (class 7, harden) |
| libhangul | 3 gettext | `Makefile.am: error: required file './ChangeLog' not found` → automake fails | automake strict mode wants GNU boilerplate; `touch ChangeLog` (+NEWS/AUTHORS/README if demanded) or add `foreign` to `AM_INIT_AUTOMAKE` | sibling of class 10 |
| betterspades | 8 build-type | CMake `FetchContent_MakeAvailable` (src/CMakeLists.txt:11) tries a network fetch in the offline chroot | pre-seed the sub-dep as a package / `-DFETCHCONTENT_SOURCE_DIR_*=…` | **NEW class 17** |
| sawteeth | 8 build-type | `cc1plus: all warnings being treated as errors` (`-Wwrite-strings` on `new char[]`) | drop `-Werror` (`-Wno-error`/`-fpermissive`) — matches the deferred-hard note already in this playbook | per-recipe (known) |
| freeimage | 13 cross-prefix | bundled OpenEXR `Half/half.h`: `register` storage class → C++17 `-Wregister`, hard `Error 1` (old IlmImf also uses removed dynamic-exception specs) | build the bundled tree with `-std=gnu++14` (or `-Wno-register`) on the `make -f Makefile.gnu` line | per-recipe (C++17-on-old-code; theme-shares class 16) |
| nogravity | 8 build-type | compile error in `rlx32/src/_stub.cpp` (BSD integer types; `-Wwrite-strings`) | **deferred-hard** real source porting (BSD `u_int32_t`), already on the deferred list | no (deferred-hard) |
| cmake_haiku | 8 build-type | **none — already built.** Log shows `harvested cmake_haiku-git-4-arm64.hpkg` (14 465 B, real) and the hpkg is in `hpkg/arm64/`; DDB `build_state=failed` is a **false failure** (chunk-level `rc=1` from sibling ports, plus the benign headless `mimeset: application init failed` no-op) | fix the state record / publish; **no build needed** | no (false failure) |
| autoconf2.71 | 6 config.guess | reaches INSTALL (`make install-data-hook` / `install-info standards.info`), then `rc=2`; **the fatal line is truncated out of the captured snippet** | needs the full `nb-autoconf2.71.log` (builder-local, scratch VM — gone); re-run with full-log capture to classify | indeterminate |

### Proposed new classes (candidates for the class table)

- **Class 15 — `runConfigure` requires an optimization flag when CFLAGS is
  overridden.** Haiku's `runConfigure` wrapper aborts with *"Must specify
  optimization flags when overriding CFLAGS"* whenever a recipe passes
  `CFLAGS=<non-empty>` without an `-O` level, because autotools' own default `-g
  -O2` is dropped once CFLAGS is set. **This is self-inflicted by the earlier
  fix waves:** the class-3/BSD overlays that add `CFLAGS=-Wno-error` or
  `CFLAGS=-D_BSD_SOURCE` before `runConfigure` regressed exactly here. Blast
  radius is larger than the two ports that surfaced it — `tar-1.35.recipe`
  (`CFLAGS="-D_BSD_SOURCE" … runConfigure`) is a **latent** third victim.
  *Rule:* any `CFLAGS=` passed to `runConfigure` must include `-O2` (cmake ports
  that `export CFLAGS` are unaffected — the wrapper guards `runConfigure` only,
  which is why `epoll_shim`'s bare `export CFLAGS=…` built fine). Grep the
  overlays for `CFLAGS=.*runConfigure` lacking `-O` before the next bake.
- **Class 16 — multiple definition (GCC ≥10 `-fno-common` default).** Old C that
  defines a global in a header without `extern` (tentative definitions) now
  collides at link: `ld: multiple definition of '<sym>'`. Fix: compile with
  `-fcommon`. freegish and libmirage both hit it; freeimage's bundled OpenEXR is
  the C++ cousin (C++17 removed `register`/dynamic-exception-specs) — same
  "modern default toolchain vs. old bundled source" theme, different flag
  (`-std=gnu++14`).
- **Class 17 — CMake `FetchContent`/`ExternalProject` network fetch in the
  offline chroot.** Distinct from Class 12 (that is the recipe's own
  `SOURCE_URI`); this is a *build-time sub-dependency* the upstream `CMakeLists`
  pulls at configure time, which the sandboxed chroot cannot reach. Fix by
  supplying the sub-dep as a real package dependency and pointing
  `FETCHCONTENT_SOURCE_DIR_<name>` at it, or vendoring it into the recipe.

### Campaign verdict (does another systemic pass pay off?)

| Bucket | Ports | Count |
|---|---|---|
| Already built (false failure) | cmake_haiku | 1 |
| New systemic class 15 (CFLAGS `-O2`) | axel, dovecot (+tar latent) | 2 |
| New class 16 (`-fcommon`) | freegish, libmirage | 2 |
| Existing class re-applied (5, 2) | lensfun, keystone | 2 |
| Class 7 hardening | unarr | 1 |
| Per-recipe, cheap | sawteeth (`-Werror`), freeimage (`-std=gnu++14`), libhangul (ChangeLog/`foreign`) | 3 |
| New class 17 (FetchContent), medium | betterspades | 1 |
| Deferred-hard real source | nogravity (BSD types) | 1 |
| Indeterminate (log truncated) | autoconf2.71 | 1 |

**≈11 of 14 are a cheap recipe edit or free (cmake_haiku is already built).** Two
clean new near-systemic classes (15, 16) plus re-applying classes 5/2 and a
class-7 hardening cover the bulk; three more are per-recipe one-liners.
`betterspades` needs a dep pre-seed (medium). Only `nogravity` is genuinely hard,
and `autoconf2.71` just needs a full-log re-capture to classify. **A second
systemic-fix → rebake → re-wave pass is worth running** — with the important
caveat that class 15 is a *regression the last wave introduced*, so the first
action is to grep every overlay for a `CFLAGS=`-to-`runConfigure` without `-O`
and fix them in bulk before rebaking.
