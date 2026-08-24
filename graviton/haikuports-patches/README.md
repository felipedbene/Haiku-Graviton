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
| `perl-5.42.2-library-path.patch` | **Real fix.** Makes perl's `LDLIBPTH` additive, because Haiku's `runtime_loader` *replaces* the library search path when `LIBRARY_PATH` is set instead of prepending to it. Without it every `$(MINIPERL)`/`$(RUN_PERL)` dies with loader exit code 3 and the build reports the misleading *"Failed to build miniperl"* — while `miniperl` is a perfectly good binary. | Never (unless the loader is changed to prepend, which is the upstream TODO) |
| `autoconf-2.72-doc-cut-stage1.patch` | **Stage-1 expedient.** Empties `HTMLS` so `install-html` cannot invoke `makeinfo`, which cannot run in this image at all: `texinfo_bootstrap` ships the `Texinfo/` directory but **zero `.pm` files**. Resulting package has no html docs (info docs survive, they ship prebuilt in the tarball). | A real `texinfo` exists → rebuild with a plain `make install-html`, expect real docs |
| `haikuporter-unpack-compressed-tar.patch` | **Patches haikuporter, not a recipe.** Adds `gz`/`bz2`/`xz` to `unpackArchive()`'s external-decompressor dispatch, because the guest python has no `zlib`/`_bz2`/`_lzma` and every downloaded `.tar.gz`/`.tar.bz2`/`.tar.xz` therefore died on *"Unrecognized archive type"* right after a **valid** checksum. Applied by `graviton/scripts/haiku-haikuporter-patch`, which also puts a real `patch(1)` on the guest PATH. | `python3.10` is built against zlib/libbz2/liblzma — then `tarfile` handles all three and the added branch is unreachable |
| `gettext-1.0-groff-doc-cut-stage1.patch` | **Stage-1 expedient, declaration-only.** Drops `cmd:groff` from `BUILD_PREREQUIRES`. groff appears in gettext solely as `MAN2HTML = groff -mandoc -Thtml`; `make all` *does* reach `$(man_HTML)`, but all 27 HTML man pages ship prebuilt and none is stale, so groff is never executed and **the package is complete**. This one line released the whole `gettext → xz_utils → zstd → openssl3` chain. | a real `groff` exists → restore the line. If a gettext man page is ever patched the rule fires and fails loudly — flatten timestamps then, never stub groff |
| `cmake-4.1.6-bundled-libs-stage1.patch` | **Stage-1 expedient.** Builds cmake against its bundled `Utilities/cmcurl`, `cmexpat`, `cmlibrhash`, `cmlibuv` instead of system copies. `devel:libcurl` is the real cycle edge (`cmake → libcurl → openssl3 → libzstd → zstd → cmd:cmake`); the other three are simply unbuilt. Extends the technique this recipe already uses for libarchive/libcppdap/libjsoncpp. `--system-zlib` kept. | curl/expat/rhash/libuv are native — which this cmake is what unblocks. Bundled **curl** is the security-relevant one; keep out of shipping repos |

Any port whose build invokes `makeinfo` will fail the same way, so expect to repeat that
cut. Stage-1 artifacts go to `hpkg-out/arm64/stage1/`, never to a shipping repo — see the
ledger in `graviton/docs/sequencing.md`.

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
