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

Any port whose build invokes `makeinfo` will fail the same way, so expect to repeat that
cut. Stage-1 artifacts go to `hpkg-out/arm64/stage1/`, never to a shipping repo — see the
ledger in `graviton/docs/sequencing.md`.
