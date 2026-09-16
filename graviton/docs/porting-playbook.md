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

**Root cause (two parts).** A port with a `po/` tree runs `autopoint` during
`autoreconf`, and (a) `cmd:autopoint` was never declared (Class 2), **and** (b)
gettext's `gettext.m4`/`lib-*.m4` live under `/boot/system/data/gettext/m4`, not
the default aclocal dir, so even with autopoint present the macros aren't found.

**Fix** (from `recipes/libcddb-1.3.2.recipe`):
```sh
BUILD_PREREQUIRES="
	…
	cmd:autopoint
	"

BUILD()
{
	export ACLOCAL_PATH="/boot/system/data/gettext/m4${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
	autopoint --force
	libtoolize --force --copy --install
	autoreconf -fi
	runConfigure ./configure --disable-static
	make $jobArgs
}
```
`autopoint --force` must run **before** `autoreconf -fi`.

**Example ports:** libcddb, libhangul, libggz, libexif (#52). Deeper variant
**not** covered by this pattern: `libmetalink`/`libspectrum` need `AM_PATH_XML2`
(libxml2's m4 macro) on the aclocal path — same mechanism, different macro dir.

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
**Example ports:** draco, nesalizer.

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
**Example port:** bonnie++.

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
(`16fcbe5dda`), which puts the arm64 triplet compilers on the in-chroot PATH.
**Example port:** re2 (#52).

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
