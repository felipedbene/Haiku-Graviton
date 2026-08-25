# haikuports recipe patches (arm64 native build)

Recipe changes needed to build native arm64 packages inside the Haiku builder guest.
They live here because the guest is a **scratch VM** — its disk is recreated from
`haiku-mmc.image` on every image rebuild, so anything that exists only inside it is lost
the next time the guest is swapped. Three earlier versions of the autoconf cut were lost
exactly that way (see below).

These are **not** upstream submissions; per project policy nothing here is offered to
upstream Haiku or HaikuPorts.

## Where to apply them

**Not to the ports tree.** haikuporter prefers the recipe carried inside the
`*_source_rigged` hpkg under `input-source-packages/`, and it *silently overrules* the
tree copy — a fix applied to `haikuports/<category>/<port>/` is ignored without warning.
The file that is actually used is:

```
/boot/home/haikuports/input-source-packages/develop/sources/<port>-<ver>-1/<port>-<ver>.recipe
```

Do not delete `input-source-packages/` to escape this: the guest's bootstrap `curl`
reports `Protocol "https" disabled`, so those 116 source packages are the only way the
guest can obtain sources at all. Removing them is gated on a working HTTPS `curl`.

**Per-port removal is a different, safer thing than deleting the directory.** A single
source package can be moved aside if its sources are supplied another way, and that is how
the `llvm12` fix below was proven: `llvm12_source_rigged-12.0.1-8-arm64.hpkg` was moved out
of `input-source-packages/`, the eight upstream tarballs were copied into
`sys-devel/llvm/download/`, and haikuporter then used the **ports-tree** recipe and
patchset — logging `Skipping download of source for llvm-12.0.1.src.tar.xz` followed by a
passing checksum for all eight. No network was involved, so the disabled-HTTPS `curl` never
came into it. Use this when the fix you need lives in a `patches/*.patchset` rather than in
the recipe body, since the source package carries its own copy of both.

## A poisoned package can look exactly like a missing one

`bison-3.8.2_bootstrap-1-arm64.hpkg` ships an **x86-64 Linux** executable as `bin/bison`:

```
$ readelf -l bin/bison | grep interpreter
      [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
```

`bison_bootstrap-3.8.2-1-arm64.hpkg` is wrong in the same way but for aarch64 Linux
(`/lib/ld-linux-aarch64.so.1`). Both are inside `-arm64` Haiku packages and neither can
run. The genuine `bison-3.8.2-1-arm64.hpkg` is a real Haiku binary, has no `INTERP` segment
at all, ships both `bin/bison` and `bin/yacc`, and prints `bison (GNU Bison) 3.8.2` when
executed.

Two things make this expensive to diagnose:

1. **haikuporter prefers the broken one.** `3.8.2_bootstrap` sorts *above* `3.8.2`, so a
   pool holding both resolves `cmd:bison` to the poisoned package. The good package being
   present is not enough; the bad one has to be gone.
2. **The failure does not mention architecture.** What surfaces is a `runtime_loader`
   complaint about a program header type:

   ```
   runtime_loader: /boot/system/bin/bison: Unhandled pheader type in parse 0x6474e553
   ```

   `0x6474e553` is `PT_GNU_PROPERTY`, which Haiku's `elf.h` does not define and
   `parse_program_headers()` therefore rejects with `B_BAD_DATA`. That is a true statement
   about the loader, but it is **not the bug here** — the binary was for the wrong
   architecture *and* the wrong operating system and could never have run. Do not go fix
   the loader on the strength of this message; check `readelf -l` for an `INTERP` line
   first. (A loader that named the machine type before parsing segments would have made
   this a five-second diagnosis, which is worth remembering separately.)

The practical consequence is that **any port whose build runs `bison` fails until the
poisoned packages are out of the pool** — the visible symptom being a build tool dying with
no yacc-related message anywhere. `sys-devel/jam` was the case that exposed it. Both
poisoned files are still present in `hpkg-out/arm64/` and in the shared package repository;
they are build inputs rather than shipped output, but they should be quarantined at source
rather than worked around per guest.

## The mtime trap — read before editing a recipe in the guest

`HaikuPorter/Repository.py:_partiallyExtractSourcePackageIfNeeded` re-extracts the recipe
from its hpkg whenever `mtime(recipe) <= mtime(sourcePackage)`. The builder guest's clock
has run hours behind the image's file timestamps, so a freshly saved edit is *born* older
than the hpkg and is **reverted before `BUILD()` runs** — the build then fails with the
original error and the patch looks like it never applied. It is silent; the only evidence
is that the recipe's md5 matches the pristine copy afterwards.

After editing, pin the mtime forward past the hpkg:

```sh
touch -r <the source hpkg> -d "+1 day" <the recipe>
```

The underlying clock bug is fixed (`arm64: fix system_time() overflow …`), but an image
built before that fix still shows it, so keep pinning until the guest is known good.

## The patches

| Patch | Kind | Retire when |
|---|---|---|
| `perl-5.42.2-library-path.patch` | **RETIRED 2026-08-24 — the platform was fixed instead.** It made perl's `LDLIBPTH` additive to work around `runtime_loader` *replacing* the library search path whenever `LIBRARY_PATH` was set. That file's own upstream TODO is now implemented (`runtime_loader: make LIBRARY_PATH and ADDON_PATH additive`), and perl-5.42.2 builds on a post-fix host with the sed **removed** — proven with the workaround absent, not merely unused. Kept as the write-up of the defect. | Retired **as of the fixed loader only**. Still required on a pre-fix host — see "Which host you are on" below |
| `autoconf-2.72-doc-cut-stage1.patch` | **Stage-1 expedient.** Empties `HTMLS` so `install-html` cannot invoke `makeinfo`, which cannot run in this image at all: `texinfo_bootstrap` ships the `Texinfo/` directory but **zero `.pm` files**. Resulting package has no html docs (info docs survive, they ship prebuilt in the tarball). | A real `texinfo` exists → rebuild with a plain `make install-html`, expect real docs |
| `haikuporter-unpack-compressed-tar.patch` | **Patches haikuporter, not a recipe.** Adds `gz`/`bz2`/`xz` to `unpackArchive()`'s external-decompressor dispatch, because the guest python has no `zlib`/`_bz2`/`_lzma` and every downloaded `.tar.gz`/`.tar.bz2`/`.tar.xz` therefore died on *"Unrecognized archive type"* right after a **valid** checksum. Applied by `graviton/scripts/haiku-haikuporter-patch`, which also puts a real `patch(1)` on the guest PATH. | `python3.10` is built against zlib/libbz2/liblzma — then `tarfile` handles all three and the added branch is unreachable |
| `gettext-1.0-groff-doc-cut-stage1.patch` | **RETIRED 2026-08-24 — the condition it named came true.** It dropped `cmd:groff` from `BUILD_PREREQUIRES`; groff appears in gettext solely as `MAN2HTML = groff -mandoc -Thtml`, all 27 HTML man pages ship prebuilt and none is stale, so groff was never executed and the package was always complete. `groff-1.23.0` is now built natively, so the line was restored and gettext rebuilt: `RC=0`, five hpkgs, `_dirty` 0 on each, and the content inventory is **identical** to the cut build (96 html entries in `gettext_doc`, the same 31 `name.N.html` man pages, same per-subpackage entry counts) — which is the direct confirmation that the cut only ever falsified the declaration. haikuporter activated `groff-1.23.0-2-arm64.hpkg` into the build chroot, so the restored prerequisite genuinely resolves. | Retired |
| `cmake-4.1.6-bundled-libs-stage1.patch` | **Stage-1 expedient.** Builds cmake against its bundled `Utilities/cmcurl`, `cmexpat`, `cmlibrhash`, `cmlibuv` instead of system copies. `devel:libcurl` is the real cycle edge (`cmake → libcurl → openssl3 → libzstd → zstd → cmd:cmake`); the other three are simply unbuilt. Extends the technique this recipe already uses for libarchive/libcppdap/libjsoncpp. `--system-zlib` kept. | curl/expat/rhash/libuv are native — which this cmake is what unblocks. Bundled **curl** is the security-relevant one; keep out of shipping repos |

| `zstd-1.5.6-makefile-not-cmake-stage1.patch` | **Stage-1 expedient.** Builds zstd with its own upstream `Makefile` instead of cmake, which removes `cmd:cmake` — and with it the `cmake → libcurl → openssl3 → libzstd → zstd` cycle — from the picture entirely. Same `libzstd.so.1.5.6`, same headers, same `libzstd.pc`; what is lost is the CMake package-config files, so `find_package(zstd CONFIG)` will not work. Needs `CXX=g++` for `contrib/pzstd` and `MAN1DIR=`, not `MANDIR=`. | `cmd:cmake` exists → restore the cmake `BUILD()`/`INSTALL()` verbatim |

| `vim-9.1.1618-cli-only-no-ruby.patch` | **Two real cuts — the only deliberately reduced port in the netsurf chain.** vim exists in this tree solely as the affordable provider of `cmd:xxd`, which `netsurf-3.11` build-requires (the only other provider, `qvim`, wants Qt5). **Cut 1: no ruby interpreter** — cost is *vim has no `:ruby`*. Acceptable because the reason ruby is unbuildable here is an **arm64 kernel panic in `mprotect()`**, and that defect is separately owned and being fixed rather than concealed by this cut. **Cut 2: no GUI build** — cost is *no GUI vim*, i.e. `cmd:gvim`/`gview`/`gvimdiff`/`rgvim`/`rgview`, whose `PROVIDES` entries are removed in the same edit so the declaration cannot outlive the binaries. Needed because `make install` would reach `installglinks_haiku`, which reads back a `BEOS:ICON` attribute that `mimeset` does not produce in this chroot. Verified by **running** the extracted `xxd`, not by reading its `PROVIDES` line. | Cut 1: when the `VMSAv8TranslationMap::Query()` fix lands — then retry ruby, starting from `ruby-3.2.9-arm64-mcontext.patch`. Cut 2: when `mimeset` in the chroot produces `BEOS:ICON` |
| `json_c-0.15-cmake4-policy.patch` | **Toolchain compatibility flag, not a cut.** json-c 0.15 declares `cmake_minimum_required` below 3.5 and cmake 4 removed that compatibility outright, so configure dies at `CMakeLists.txt:3` before it looks at anything else. `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` restores the pre-3.5 policy defaults — exactly what cmake 3.x did with this project. **Nothing is removed from the build and no declared dependency changes**, so the resulting package is what json-c intends; it is not in the same class as the stage-1 cuts above. Needed because `hubbub`, netsurf's HTML parser, build-requires `devel:libjson_c`, and the tree's only other recipe (`json_c4-0.13.1`) is older still. | the recipe is updated to a json-c release declaring a cmake 3.5+ minimum |
| `llvm12-12.0.1-config-guess-arm64.patch` | **Portability fix, not a cut.** LLVM 12 bundles a `cmake/config.guess` dated **2011-08-20** that knows only `BePC` and `x86_64` Haiku hosts. On arm64 `uname -m` is `arm64`, nothing matches, the script exits non-zero and `cmake/modules/GetHostTriple.cmake` turns that into a fatal `Failed to execute .../cmake/config.guess` — configure dies before compiling anything. Adds an `arm64` case emitting `aarch64-unknown-haiku` (which is what `gcc -dumpmachine` reports) plus a generic `*:Haiku` fallback. Note there is **no `config.sub` in llvm12 at all**; `GetHostTriple.cmake` only ever runs `config.guess`. Appends to `sys-devel/llvm/patches/llvm-12.0.1.patchset`; recipe `REVISION` 8 &rarr; 9. | never — this is a straight portability fix, correct to keep |

| `pe-2.5.0-metrowerks-flags.patch` | **UNFINISHED — explanation only, `pe` still does not build.** Kept because chasing it found the poisoned `bison` above and the x86-only `jam` install step, both of which mattered. Pe's own Jamfiles pass mwcc's `-prefix <header>` and `-w nounusedvar`, which gcc rejects; respelling `-prefix` as `-include` is not sufficient because `PREFIX_FILE` is empty for some targets and the flag then eats the following `-O7`. | `PREFIX_FILE` is made conditional, `-w nounusedvar` dropped, and the built Pe has been *run* |


Any port whose build invokes `makeinfo` will fail the same way, so expect to repeat that
cut. Stage-1 artifacts go to `hpkg-out/arm64/stage1/`, never to a shipping repo — see the
ledger in `graviton/docs/sequencing.md`.

## Which host you are on — the `LIBRARY_PATH` retirements are host-conditional

The two `LIBRARY_PATH` workarounds (perl's `LDLIBPTH`, and `pyfix.sh`'s `RUNSHARED` half)
are retired **against a host carrying the fixed `runtime_loader`**, not retired
unconditionally. A guest booted from an image built before
`runtime_loader: make LIBRARY_PATH and ADDON_PATH additive` still replaces the search
path, and every one of those builds fails exactly as before.

It is the **host** loader that decides, not the chroot's `haiku.hpkg`: the acceptance run
that proved the retirement kept the chroot input package at its old checksum and changed
only `/boot/system/runtime_loader`, and that was sufficient. Test the host you are about to
build on, with a positive control, before trusting either retirement:

```sh
/bin/echo control-ok                  # negative control: must print
LIBRARY_PATH=/tmp/empty /bin/echo ok   # fixed loader prints ok; broken loader exits 3 silently
```

Exit code 3 with no output is the broken loader. That silence is the whole difficulty with
this defect — it reads as a corrupt binary rather than an environment problem.

**This is not only a workaround question.** `ruby-3.2.9`'s recipe already writes
`export LIBRARY_PATH=$LIBRARY_PATH:%A`, i.e. it was written assuming prepend semantics.
On the replacing loader `$LIBRARY_PATH` is empty, so the result contains no `libroot.so`
and the very next `make` dies with
`runtime_loader: Cannot open file libroot.so (needed by /boot/system/bin/make)` and exit
status 3. The recipe is correct; the loader was wrong.

**Correction to what this paragraph first claimed.** It went on to say that because `ruby`
gates `vim`, the only affordable provider of `cmd:xxd`, "on a pre-fix host the browser chain
simply cannot finish, and no recipe edit is the right answer to that". Both halves were
wrong. A recipe edit *was* the answer — ruby was cut out of `vim`, once the arm64 kernel
panic sitting behind ruby had been attributed and assigned, so the cut hides nothing (see
`vim-9.1.1618-cli-only-no-ruby.patch`). And the chain does **not** need a post-fix host:
`vim` and `netsurf-3.11` were both built on a **pre-fix** guest, with the post-fix guest run
as the other arm. The additive loader was only ever a `ruby` requirement. It is still needed
to consume the perl/python retirements above — just not to build the browser.

## `recipes/` — the whole edited recipe, not just the diff

`recipes/` holds the six recipes in their edited form:

```
perl-5.42.2.recipe  autoconf-2.72.recipe  libtool-2.5.4.recipe
tar-1.35.recipe     gettext-1.0.recipe    zstd-1.5.6.recipe
```

Two reasons, both learned by losing work:

1. **The guest copies get silently reverted.** During the 2026-08-24 rebuild a survey of
   all live guests found **no guest still carrying the libtool edit** and none carrying the
   perl or autoconf edits, even though all three ports had been built with them. The mtime
   pin is per-guest state; anything that resets it (a swap, a re-extract, a fresh guest)
   takes the edit with it, and the next build fails with the *original* error, which reads
   as a regression rather than as a lost patch.
2. **Not every patch file in this directory is machine-appliable.** The gettext, tar and
   zstd notes use illustrative hunk headers (`@@ BUILD_PREREQUIRES`) rather than real
   line-numbered ones, because the prose is the point. `patch(1)` cannot apply those. The
   libtool edit had to be reconstructed by hand from the diff in
   `libtool-2.5.4-no-bootstrap.patch` for exactly this reason.

So: the `.patch` files remain the explanation of *why*, and `recipes/` is what you install.
`graviton/builder/prepguest.sh` copies them into a guest and pins each mtime against the
matching `*_source_rigged-*.hpkg`. The metal keeps its working copies in
`/opt/haiku/recipe-overlay/`; this directory is the version-controlled one, so edit here and
copy out.

## Every non-ISP port must be built with `haikuporter -G`

Ports whose sources come from an input source package are unpacked from an hpkg and never
touch git. Everything that *downloads* a tarball does: `Source.patch()` creates an implicit
git repo per source dir and aborts with `Error: 'git' is not available, please install it`.
git is not buildable here yet (curl, openssl3, expat, libiconv …), so pass `-G`
(`--no-git-repo`), which makes haikuporter use `patch(1)` instead — it requires `patch`
unconditionally, even for a port with no patches, which is why
`haiku-haikuporter-patch` installs it. Guest-side pieces to reinstate after a guest swap,
in order:

```sh
haiku-source-proxy start                     # on the metal, no init script
haiku-source-proxy install-shim <sshport>    # wget + haiku-proxy-decompress
haiku-haikuporter-patch <sshport>            # unpack patch + patch(1)
```
