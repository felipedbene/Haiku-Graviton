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

## Two recipes here are kept only as history

`autoconf-2.72.recipe` and `zstd-1.5.6.recipe` describe cuts that have been
**retired** — the packages in `hpkg-out/arm64/` are now built from pristine recipes.
They stay because the reasoning is worth keeping and because a future chroot change
could make either cut necessary again. **Do not install them onto a guest** expecting
current behaviour.
