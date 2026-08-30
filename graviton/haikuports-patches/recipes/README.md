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
| `gettext-1.0.recipe` | yes, **still needed** | drops `cmd:groff`. Cannot be retired until a real `groff` exists |
| `zstd-1.5.6.recipe` | **stale — cut retired** | kept as the record of the Makefile-instead-of-cmake cut. Now built from the **pristine** cmake-based recipe |
| `cmake-4.1.6.recipe` | **NO — unmodified** | cmake needs no change at all once `expat`/`rhash`/`libuv`/`curl` exist. Stored to pin the exact upstream text, since the builder's tree is not a pristine reference |
| `curl-8.21.0.recipe` | yes | `--without-libpsl` on every arch; `libpsl` → `libidn2` → `cmd:gtkdocize`, unbuildable here |
| `libxml2-2.15.3.recipe` | yes, **stage-1** | forces `pythonModuleEnabled=false`; loses only the `libxml2_python3.14` subpackage. Retire when `cmd:python3.14` exists |
| `ruby-3.2.9.recipe` | yes | adds an `__aarch64__` arm to `signal.c`'s `mcontext_t` read — ruby took the x86 `esp`/`ebp` names on every non-amd64 Haiku and failed to compile. **A portability fix, not a cut**: the handler keeps working and the x86 lines are untouched |
| `jam-2.5_2021_10_29.recipe` | yes | `INSTALL()` ran `install -v bin.haikux86/g/jam`, but jam builds into `bin.$OSPLAT` and that is plain `bin.haiku` on an architecture whose spelling its Jamfile does not know — arm64 among them. Now locates the binary under `bin.haiku*/g/jam` and fails loudly if there is none, instead of being silently x86-only. **A portability fix, not a cut.** Has no input source package, so the operative copy is the tree one at `sys-devel/jam/` and no mtime pin applies. Proven: `jam-2.5_2021_10_29-3-arm64.hpkg` |
| `libsdl2-2.32.10.recipe` | yes, **still needed** | the DeBeOS `@minimum` `haiku` package ships **no `libmedia.so` or `libgame.so`** (media_kit + game_kit dropped), so SDL2's Haiku native audio (`BSoundPlayer`) and joystick (`BJoystick`) backends cannot link. `BUILD()` now `-DSDL_AUDIO=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF`, seds `media game` out of the Haiku `EXTRA_LIBS`, and stubs the lone `set_mouse_position()` call (a `<game/WindowScreen.h>` symbol) in `src/{video,main}/haiku/SDL_BApp.h`. Video / render (`libGL`) / events / timers / filesystem are intact; native audio, joystick and relative-mouse re-centering are lost until the media/game kits exist. Proven: `libsdl2-2.32.10-3-arm64.hpkg` installs and `dlopen`s. No input source package — operative copy is the tree one at `media-libs/libsdl2/`, no mtime pin |
| `openal-1.21.1.recipe` | yes, **still needed** | base package only — drops the `_tools` subpackage (the `alsoft-config` Qt5 GUI) and its `Qt5`/`libsndfile` `BUILD_REQUIRES` (neither is in this image), so `-DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF`, and `-DALSOFT_BACKEND_HAIKU=OFF` because the Haiku output backend links the absent `libmedia` (null/wave backends remain). Also `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`: the guest's CMake is 4.x, which removed pre-3.5 compatibility (same class as the `json_c` cmake4 policy fix). Proven: `openal-1.21.1-5-arm64.hpkg` installs and `dlopen`s. No input source package — operative copy is the tree one at `media-libs/openal/`, no mtime pin |

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
