# Native arm64 HaikuPorts chain — status

State of the native package build running in the QEMU Haiku guests on the c7g.metal
builder (`i-0f7f6f3e8922acffd`). Companion to `graviton/docs/sequencing.md` (Phase 2)
and to the recipe patches in `graviton/haikuports-patches/`.

## Where this stands — **2026-08-24 19:19Z**

## **`netsurf-3.11` IS BUILT.**

`netsurf-3.11-3-arm64.hpkg`, 4,003,742 B, `_dirty` requirement count **0**, containing a
12,144,892 B **ELF64 / AArch64** executable that links all twenty chain libraries
(`libcss`, `libdom`, `libhubbub`, `libparserutils`, `libwapcaplet`, `libnsbmp`, `libnsgif`,
`libnslog`, `libnspsl`, `libnsutils`, `libsvgtiny`, `libutf8proc`, `libcurl`, `libssl`,
`libcrypto`, `libpng16`, `libjpeg`, `libexpat`, `libz`, `libiconv`) plus Haiku's own
`libbe`/`libtranslation`/`libtracker`/`libnetwork`. `depclosure.py` now answers **wave 0,
minimal build set 0 ports**.

> **What that claim now covers (updated 2026-08-25): the browser RENDERS.** It is built and
> linked (verified from inside the hpkg) **and it has now run and rendered a real page** —
> screenshot-verified on Graviton silicon: a local `file://` page (title parsed, headings,
> an em-dash decoded, a blue div measured to the pixel) and `example.com` over the network.
> The render was done in a KVM guest with a synthetic framebuffer (`-device ramfb`), because
> a *bare EC2* Graviton instance has no display device at all (proven: the EFI loader gets no
> GOP) — so the browser paints via `RemoteHWInterface`/ramfb, not a local EC2 framebuffer.
> "A browser builds" and "a browser works" were once different statements here; both are now
> true. See `framebuffer-guest-capture.md` and the netsurf work for the capture evidence.

**Every numbered blocker, 1 through 10, is closed.** Blocker 6 (cmake) was solved at
04:41Z; the header of this document went on calling it "the open one" for ten hours
afterwards, and Blocker 3's heading said `OPEN` for a day after its fix merged. Both are
corrected below. Blocker 9 closed at 18:05Z with the `gettext` cut retired; **Blocker 10
closed at 19:16Z**, with `vim` reduced to a CLI-only, ruby-less `xxd` provider and netsurf
built on top of it. **Both of those vim cuts were RETIRED on 2026-08-25** — vim is built from
the pristine recipe again, with ruby and the GUI; see "The two `vim` cuts" below.

**The fixed `LIBRARY_PATH` loader turned out *not* to be a prerequisite for the browser.**
That was measured rather than assumed, and it corrects an earlier statement in this very
file — see "Does the chain need the fixed loader?" below. One genuine platform defect was
found on the way and handed off: an **arm64 kernel panic in `mprotect()`** — since **fixed,
baked and verified on hardware against its own reproducer** (2026-08-25), see
[arm64-mprotect-query-present.md](arm64-mprotect-query-present.md). **The ruby cut below is
therefore now un-blocked and due for retry.**

### Counts, with the units named

The three numbers this document used to mix are genuinely different. Measured on the
builder at **19:19Z**:

| Number | Value | What it counts |
|---|---|---|
| **ports built natively** | **106** (69 at 15:05Z; **+37** in the netsurf pass) | distinct recipes we have built on arm64. Count with care: a naive strip of subpackage suffixes reports **113** because `git` emits nine subpackages (`git_arch`, `git_svn`, `git_web`, …) that are not separate ports |
| **hpkgs produced natively** | **227** (156 at 15:05Z) | package files those ports emitted (base + `_devel`/`_debuginfo`/`_doc`/per-Python-flavour/…) |
| **`depclosure.py` "ports already built"** | **119** (82 before the pass) | its own basis, which counts what it can resolve provides from — **not** the same as the row above, and the +35 delta is the number to quote for this session |
| **`.hpkg` files in `hpkg-out/arm64/`** | **263** | the 225 above **+ 28** cross-built `_bootstrap` inputs **+ 8** chroot inputs (`haiku*.hpkg`, `makefile_engine`, `netfs`, `userland_fs`) — build *inputs*, not our output |
| **files under `hpkg-out/` entirely** | **~330** | the above **plus two other directories**: `arm64-nondirty/` (the shared build pool, a duplicate) and `arm64-dirty-20260824/` (the 52-file pre-clock-fix snapshot kept as evidence). Mostly duplicates and superseded files |

> **Do not read an mtime as a build date here.** `rebuild.sh` harvests with
> `cp --remove-destination` and then `aws s3 sync`, so a listing sorted by mtime can show
> the entire directory as "just modified". Ports built in a session are identified from the
> `rebuild-<guest>.log` markers, not from `ls -lt`.

So a "~254 files in `hpkg-out`" reading — which is where that figure came from — is
**not** a package count; it is one directory of output plus a duplicate pool plus a
snapshot of packages that have been *replaced*. Quote `hpkg-out/arm64/*.hpkg` minus
`_bootstrap` minus the chroot inputs, or quote ports. Always with a timestamp: this
number moved four times in one day.

**Superseded earlier figures, for orientation only:** "23 ports / 52 hpkgs" was the
Blocker 8 rebuild at 01:20Z; the cmake pass took it to 38 ports / 82 hpkgs by 04:32Z.
**This session built 31 ports, 38 → 69**, and closed the groff chain end to end:

```
autoconf_archive libedit libffi libpng16 nasm python3.10 python3.14 file
flit_core libjpeg_turbo ninja setuptools installer tiff pyproject_hooks tomli
wheel psutils packaging meson build puremagic pypdf typing_extensions libpaper2
itstool libxml2(recut) libglvnd glu jasper netpbm groff
```

Everything is built against the repaired non-dirty chroot `haiku`; five healthy guests; no
`_dirty`/non-dirty split; zero clock-skew warnings. All 47 ports carry a non-`_dirty`
`requires haiku` and so *resolve*, but **`pkgman`-installable is not a blanket claim** —
`python3.10-3.10.20` currently will not install, because it wants `lib:libbz2` activated
and a `file_data` subpackage the `file` build did not emit. That is a runtime-activation
chore, not a build failure; see Blocker 9.

### The one open item

**Blocker 9 — the remaining chain is deep but it contains no cycles.** Measured with
`graviton/builder/depclosure.py` against haikuporter's own graph, not read off the
recipes. **There is no cycle anywhere on the path to a browser.**

The previous write-up's *mechanism* for why `groff` was out of reach — "`netpbm` → `jasper`
→ OpenGL → `mesa-25.3.6` → `libLLVM` + `libvulkan` + `cmd:git`" — is **wrong**.
`devel:libgl` comes from **`libglvnd-1.7.0`** (~70 s to build, with `glu`), and jasper's
cmake **does find OpenGL**. But its *conclusion* — that jasper needs an OpenGL cut — is
**right**, for an unrelated reason: jasper fails on **GLUT**, and **GLUT has no recipe
anywhere in the tree**. So `-DJAS_ENABLE_OPENGL=OFF` **is** needed, and it is a cut against
GLUT's absence, not against a mesa/LLVM/`cmd:git` wall. Separately, `libglvnd` needed a
**two-line Haiku portability fix** — a real fix, not a cut. Details in Blocker 9.

> **Why this sentence is worded so carefully.** An earlier revision of this very line said
> "no OpenGL cut is needed", which contradicted the body once GLUT was measured — the same
> header-versus-body split this document was just corrected for, reintroduced hours later.
> **A conclusion that survives while its mechanism is replaced is the most dangerous kind of
> correction**, because the summary sentence keeps on looking right and nothing prompts you
> to re-read it. When you replace a mechanism, re-check every sentence that asserted the
> conclusion, not just the paragraph that explained it.

The whole remainder converged on one chokepoint, **`python3.10`/`python3.14`**, and **both
were built this session** after fixing two real defects that were not dependency problems at
all (a missing-LTO toolchain gap, and `LIBRARY_PATH` replacing rather than prepending the
loader path). **`groff` is now built and verified by rendering** — the deliverable this
document called "the one that did not land" and scoped as its own piece of work. That
retires the `gettext` cut. What remains is a browser, and its dominant cost is LLVM, not any
knot. See Blocker 9.

## Blocker 1 — `libtool`: `Error 127` on `aclocal.m4` — **FIXED**

Patch: `graviton/haikuports-patches/libtool-2.5.4-no-bootstrap.patch`.

### What was actually missing

Not `aclocal`. **`aclocal-1.17`** — the *version-suffixed* name. From the generated
`Makefile` (lines 447 and 456, which is where this should have been read on day one):

```
ACLOCAL  = ${SHELL} '/sources/libtool-2.5.4/build-aux/missing' aclocal-1.17
AUTOMAKE = ${SHELL} '/sources/libtool-2.5.4/build-aux/missing' automake-1.17
```

The tarball's `configure`/`Makefile.in` were generated by automake 1.17, so
`am__api_version` is baked in at 1.17. The image has automake **1.18.1**, which
installs `aclocal` and `aclocal-1.18` and nothing else. `build-aux/missing` prints its
warning and exits **127**.

This is precisely why adding `cmd:aclocal` to `BUILD_PREREQUIRES` changed nothing: the
prerequisite resolved fine — automake 1.18.1 really does provide `cmd:aclocal` — and
the build went on calling a binary that will never exist. No recipe flag can fix a
hardcoded versioned tool name; only reading the generated `Makefile` shows it.

### Why a maintainer rule fired in a distribution tarball

A release tarball is meant to build with no autotools installed. Two separate things
were making a prerequisite newer than its target:

1. **The recipe did it to itself.**

   ```sh
   cp m4/libtool.m4 m4/libtool.m4.bak     # no -p: the copy is stamped "now"
   ./bootstrap --force
   mv m4/libtool.m4.bak m4/libtool.m4     # puts "now" onto m4/libtool.m4
   ```

   `m4/libtool.m4` is listed in `am__aclocal_m4_deps` (Makefile:104-113), so
   `aclocal.m4` became out of date and Makefile:1069 went live:

   ```
   $(ACLOCAL_M4):  $(am__aclocal_m4_deps)
   	$(am__cd) $(srcdir) && $(ACLOCAL) $(ACLOCAL_AMFLAGS)
   ```

   Observed in the kept chroot: `aclocal.m4` 01:17 vs `m4/libtool.m4` 05:05.

2. **`./bootstrap --force` cannot run in this chroot and does not say so loudly.**
   The full log — which every earlier attempt threw away by piping haikuporter
   through `tail -18` — shows:

   ```
   ./bootstrap: line 4118: git: command not found
   bootstrap: warning: Consider installing git-merge-changelog from gnulib.
   bootstrap: running: git clone 'git://git.sv.gnu.org/gnulib' 'gnulib'
   ./bootstrap: line 4984: git: command not found
   ```

   There is no `git` in the image at all — haikuporter says so on its own first log
   line, *"deactivating creation of source packages as 'git' is not available"* — and
   even with git there is no network inside the chroot to clone gnulib from. bootstrap
   gives up after those two lines **without failing the build**, so the recipe's stated
   strategy (regenerate with the newer automake so the stale versioned references go
   away) silently never happened, while step 1 guaranteed make would try.

3. **The tarball's own timestamps are internally inconsistent.** Removing bootstrap and
   equalising the top-level files got past Makefile:1069 and straight into Makefile:2390:

   ```
   $(lt_Makefile_in): $(lt_Makefile_am) $(lt_aclocal_m4) $(lt_config_h_in)
   	$(AM_V_GEN)cd '$(srcdir)/$(ltdl_dir)' && $(AUTOMAKE) Makefile
   ```

   because libtool-2.5.4.tar.gz ships the embedded libltdl subtree as

   ```
   libltdl/Makefile.am   2024-11-20 19:01
   libltdl/Makefile.in   2024-11-20 19:01
   libltdl/aclocal.m4    2024-11-20 19:41   <- newer than Makefile.in
   libltdl/config-h.in   2024-11-20 19:41   <- newer than Makefile.in
   ```

   That fires on *any* build of this tarball, independently of the recipe. Upstream
   never notices because bootstrap regenerates the lot first.

### The fix

Drop `./bootstrap` and its `cp`/`mv` — bootstrap accomplishes nothing here and the
cleanup it requires is what arms Makefile:1069 — then flatten **every** timestamp in
the pristine tree to one shared value. make only remakes a target when a prerequisite
is *strictly* newer, so one identical mtime disables every maintainer rule at once,
everywhere, without enumerating them. Everything make generates afterwards is stamped
2026 and is unambiguously newer than the flattened 2024 tree.

```sh
touch -r configure.ac /tmp/lt-mtime-ref
find . -type f -exec chmod u+w {} +
find . -exec touch -r /tmp/lt-mtime-ref {} +
```

`touch -r <reference>` and not a bare `touch`, because **the chroot's clock is not
trustworthy**. In the same run make reports `File 'Makefile' has modification time
66159 s in the future` and `Clock skew detected`, and configure's autobuild timestamp
(`20260823T025840Z`) is 18h22m behind the guest's own `date` (21:21:10Z). Taking the
stamp from another file makes it clock-independent. **This is also why the earlier
"just pre-`touch` the generated files" attempt could not have worked even with a
surviving recipe edit** — it stamped targets from a clock that runs backwards, making
them *older* than their prerequisites.

`chmod u+w` is load-bearing, not cosmetic: the tarball ships `m4/ltversion.m4` and
`libltdl/Makefile.am` mode 444 and `touch` would fail and abort the `find`.

Rejected: shimming `aclocal-1.17`/`automake-1.17` onto automake 1.18.1. automake
refuses a version mismatch, and forcing it would regenerate `aclocal.m4`, then
`configure`, then every `Makefile.in` mid-build, at which point `am__api_version`
disagrees with the `configure` that already ran.

### Result

```
libtool-2.5.4-1-arm64.hpkg          683035 bytes
libtool_libltdl-2.5.4-1-arm64.hpkg   52974 bytes
```

Verified present in the guest's `packages/` directory and in
`s3://haiku-graviton-668984504585-us-west-2/hpkg/arm64/`, not merely inferred from
`RC=0`.

### On the earlier negative results

They were re-verified from a clean slate rather than trusted:

- Guest clocks were checked against the host before drawing any conclusion. Guests
  **2227 / 2229 / 2230 / 2231 agree with the host to within 2 s**. Guest **2222 is
  24 h 24 m behind** — it is a stale pre-clock-fix image (`hrev59996+dirty`) and must
  not be used.
- The recipe's md5 was compared *after* the build, not just after saving:
  `a9e76cd781d44f052969404a75fc1bf8` before and after, so the edit genuinely reached
  `BUILD()`.
- The work directory was deleted before each attempt, because haikuporter prints
  `Skipping unpack of ...` and reuses a polluted tree otherwise — the first re-run was
  measuring sources that three earlier failed runs had already rewritten.

The recipe-revert trap is real and still needs guarding: pin the recipe forward with
`touch -r <the source hpkg> -d '+2 days' <the recipe>` after every edit.

## Blocker 2 — recipes that fetch their own sources — **FIXED (fetch), then a new wall**

The guests have **no `wget` and no `curl` at all**, so any recipe whose `SOURCE_URI` is
an http(s) URL dies at
`Error: 'wget' is not available, please install it`. Building wget is circular — it is
itself a recipe that would have to be downloaded.

### Design: the metal fetches, the guest asks

Two committed scripts, stdlib-only Python 3, no daemon framework:

| Script | Runs on | Role |
|---|---|---|
| `graviton/scripts/haiku-source-proxy` | the metal | HTTP service on `127.0.0.1:8079`, `GET /fetch?url=<percent-encoded>`; fetches over the metal's real HTTPS, caches, streams back over plain HTTP |
| `graviton/scripts/haiku-wget-shim` | each guest | installed as `/boot/home/config/non-packaged/bin/wget`; relays the URL to the proxy at `10.0.2.2:8079` |

Why this shape:

- **`10.0.2.2` is the QEMU user-net gateway** and slirp aliases it to the metal's
  loopback, so the proxy can bind `127.0.0.1` — reachable from every guest, unreachable
  from the VPC. No auth needed because there is nothing to authenticate on loopback.
- **A shim, not a real wget.** haikuporter needs only three things from wget, and
  `HaikuPorter/Utils.py:isCommandAvailable` is a literal
  `os.path.exists(dir + '/' + command)` walk over `$PATH` — no packagefs `provides`
  entry is involved, so a plain file named `wget` on PATH satisfies it. The shim parses
  argv properly rather than trusting that haikuporter's
  `-c --tries=1 --timeout=10 --progress=dot:mega -O <target> <url>` keeps its order.
- **Nothing truncated is ever kept.** Both sides write to a temp/`.part` file and rename
  only after the byte count matches the advertised `Content-Length`. A short or failed
  fetch leaves *no* file. A truncated cache entry would resurface as a checksum
  mismatch several steps later and look like an upstream problem.
- **No silent success.** Failed upstream fetches answer non-2xx, the shim exits nonzero,
  and haikuporter raises rather than proceeding with a zero-byte tarball. Every request
  logs one line (`HIT`/`MISS`/`FAIL`, bytes, status, elapsed) to
  `/opt/haiku/logs/haiku-source-proxy.log`.
- Only `http`/`https` are accepted; `file://` is refused on scheme.
- Cache keys are `sha256(full-url)-<real-basename>` under `/opt/haiku/srccache/` —
  collision-proof but still readable by a human doing forensics.

### How to run it

```bash
# on the metal
/opt/haiku/scripts/haiku-source-proxy start          # also: serve | stop | status | fetch <url>
/opt/haiku/scripts/haiku-source-proxy install-shim 2229 2230
```

`install-shim` self-checks: it confirms the guest's `$PATH` resolves to the shim and
that `wget --proxy-check` reaches `/health` from inside the guest, and returns nonzero
if not. There is **no init script**, so the proxy does not survive a metal reboot —
re-run `start`.

### Evidence the fetch class is fixed

haikuporter's own log, in order: `Downloading: .../pkgconf-1.5.3.tar.xz` →
`wget-shim: saved ... 290240 bytes` → **`Validating checksum of pkgconf-1.5.3.tar.xz`**
(passed) → `Unpacking source of ...`. The guest-side sha256
`d3468308553c94389dadfd10c4d1067269052b5364276a9d24a643c88485f715` equals the recipe's
`CHECKSUM_SHA256`. Generalised across four hosts — pkgconf, `ftp.gnu.org`, a
`downloads.sourceforge.net` redirect chain, and a 4.4 MB `curl.se` tarball — with
guest sha256 matching an independent metal-side fetch each time. Negative paths also
checked: upstream 404 → HTTP 502, shim exit 8, no target file, no `.part` left, nothing
cached.

### Scope note

The fetch fix matters for the **long tail**, not for the current chain. Of the ports in
play, only `pkgconf` lacks an input source package; `tar`, `libiconv`, `openssl3`,
`libxml2`, `gettext`, `sqlite`, `zstd`, `xz_utils`, `python3.10` and `gzip` are all
among the **116 input source packages** already on the guest and build with no download
at all.

## Blocker 3 — haikuporter cannot unpack compressed tarballs — **FIXED (routed around), two named residuals**

Fix: **`56106d5afb graviton: let haikuporter unpack compressed tarballs in the guest`**,
merged on `graviton`, with `graviton/haikuports-patches/haikuporter-unpack-compressed-tar.patch`
and `graviton/scripts/haiku-decompress-shim`.

> **This heading said `OPEN` for a day after the fix merged.** The body below described
> the defect accurately and never mentioned that it had been fixed, so anyone reading the
> heading would have rebuilt a working mechanism. Corrected 2026-08-24.

**Evidence it is fixed** — two ports fetched *and* unpacked end to end, each verified by a
resulting hpkg and not by an exit code: `pkgconf-1.5.3` (`.tar.xz`) and `zip-3.0`
(`.tar.gz`), both listed as built in the port table below. The fetch → checksum → unpack →
source-tree path was additionally walked for `wdiff` (`.tar.gz`), `libgpg-error`
(`.tar.bz2`) and `zip30` (`.tar.gz`).

`unpackArchive`'s existing external-tool dispatch was extended to
`gz/tgz/bz2/tbz/tbz2/xz/txz`, preferring a real tool when present and otherwise handing the
archive to the metal — which does have `zlib`/`bz2`/`lzma` — and reading back a plain
`.tar` that stdlib `tarfile` opens with no compression module. Same division of labour as
the wget shim. The helper is installed as `haiku-proxy-decompress` and deliberately **not**
as `gzip`/`xz`: a fake `gzip` on `PATH` would be picked up by configure scripts and by
`make install` man-page rules and would silently corrupt packages.

**Two residuals, both still real** (these are why the heading says "routed around"):

1. `zlib`/`_bz2`/`_lzma` are still missing from the Python that haikuporter runs under.
   This routes around them; it does not fix them.
   `haiku-haikuporter-patch --check` prints all three modules' status on every run so it
   cannot be quietly forgotten. The real fix is item 4 of "What remains" — and note that
   `python3.10` is now **one build away** (Blocker 9), so this residual is close to
   retirable for the first time.
2. **`.zip` sources (312 recipes) remain unsupported.** `zipfile.is_zipfile()` succeeds
   without `zlib` and extraction only *then* raises `Compression requires the (missing)
   zlib module`, and that path has no external-tool dispatch to hook into.

### What the defect was

Immediately *after* the successful fetch and checksum, `pkgconf` died with

```
Error: Unrecognized archive type in file .../pkgconf-1.5.3.tar.xz
```

The guest's `python3.10_bootstrap` has **no compression extension modules**: `zlib`,
`_bz2` and `_lzma` all fail to import.

```
- method gz:  CompressionError('gzip module is not available')
- method bz2: CompressionError('bz2 module is not available')
- method xz:  CompressionError('lzma module is not available')
```

`HaikuPorter/Utils.py:unpackArchive` shells out to external tools only for `.lz`, `.7z`
and `.zst`; for `.tar.gz` / `.tar.bz2` / `.tar.xz` / `.zip` it relies entirely on
Python's `tarfile`/`zipfile`. So installing an `xz` or `tar` **binary does not help** —
haikuporter never invokes one for these formats. `libz.so.1` *is* in the image, so gzip
is purely a Python build-config gap; liblzma and libbz2 are genuinely absent.

That gated every download-fetching recipe: no recipe in the tree ships an uncompressed
`.tar`. It never gated the chain of the time, all of which was served by input source
packages — which is exactly why the stale `OPEN` heading was able to survive so long
without anyone tripping over it.

The fallback listed here as "worth evaluating" — teach `unpackArchive` to shell out for
the compressed tar formats the way it already does for `.lz`/`.7z`/`.zst` — **is the route
that was taken**, and it is the merged fix above. The deeper route (a non-bootstrap
`python3.10` picking up zlib + libbz2 + liblzma, then running haikuporter under it) remains
the way to retire residual 1, and it is now much closer than when this was written:
`xz_utils`, `bzip2`, `bzip2_devel`, `zlib_devel`, `sqlite`, `openssl3` and `libedit` are
all built, so `python3.10` is a **single build** away (Blocker 9).

## Blocker 4 — gnulib's `re_compile_pattern` run test hangs, and strands the guest — **FIXED**

Patch: `graviton/haikuports-patches/tar-1.35-included-regex.patch`.

`tar` did not fail, it **hung**. This is why its status was "unknown": the 04:54 run
never returned, and `worker.sh` was still waiting on it seventeen hours later. The log
stops at

```
checking for working re_compile_pattern...
```

with no error to grep for, and two `wc -l` samples 30 s apart are byte-identical.
`ps` shows `./conftest` wedged under two nested `configure` shells. That check is
gnulib's `AC_RUN_IFELSE` for the regex module — it compiles and *runs* a program against
the system `re_compile_pattern`, and on arm64 Haiku that program never returns.

**Nothing hit this before because every gnulib tool in the image was cross-compiled.**
From `tar-1.35/configure:40985`:

```
*)      gl_cv_func_re_compile_pattern_working="$gl_cross_guess_normal" ;;
```

Cross compiling cannot run the test, so autoconf substitutes a guess. The test executes
only on a **native** build. It will therefore hang every native gnulib-based port, and it
is the first thing to suspect whenever a native build goes *quiet* instead of failing.

Fix: pass `--with-included-regex`. An explicit yes/no short-circuits the check, because
the `checking for working re_compile_pattern` block lives only in the default branch of
the `case` at `configure:40963`. `yes` rather than `no`, because the probe never produced
a result — asserting the system regex works would be claiming an answer nobody got.

Verified: `grep re_compile_pattern` over the successful build log returns **nothing** —
the check is skipped, not merely passed.

### The hang is unrecoverable, and it takes the whole guest

This is the expensive part, and it cost two guests:

1. The hung `conftest` is **unkillable**. `kill` does not reap it (pid 43877 on 2227 was
   still alive 17 h later).
2. With it alive, `unmount -f` on the chroot's packagefs **also hangs** — so the
   documented `-f` escape does not work here.
3. Once a `mount`/`unmount` is wedged, the next chroot setup's
   `mount -t bindfs -p "source /dev" dev` hangs too — **guest-wide**. Every subsequent
   build on that guest, of any port, blocks forever in chroot setup before compiling
   anything.

Guests **2227** and **2231** were lost this way. Detection: the log stops growing with
no error, and `ps` shows `./conftest` plus a stuck `mount -t bindfs`.

Partial recovery: a wedged work directory cannot be unmounted or deleted, but it **can be
renamed** — `mv work-<ver> work-<ver>.wedged` succeeds, and haikuporter then creates a
fresh one. That recovers the *port*. It does **not** recover a guest whose bindfs layer
is already wedged, so the only real remedy there is a different guest.

## Operational hazards found along the way

- **An aborted haikuporter run wedges the guest for that port.** The chroot's build
  processes survive the abort — `haikuporter -y tar` (pids 38644/38707) and its
  `bash -c . /wrapper-script` were still alive hours later — and they pin the chroot's
  packagefs. The next build of that port then fails in chroot *setup*, before any
  compilation, with `unmount: unmounting failed: Device/File/Resource busy`, and plain
  `unmount` cannot clear it. `df | grep work-` shows it; `unmount -f <workdir>/boot/system`
  clears it *provided no process is stuck in the kernel* (that is what `-f` is for:
  "forces unmounting in case of open files left"). If the holder is an unkillable
  `conftest`, see Blocker 4 — `-f` hangs and the guest is gone.
- **A guest can be idle and still be broken.** Guest 2222 is idle and unwedged but its
  haikuporter repository cache is corrupt (`invalid load key, '{'.` — a pickle error from
  `repository/recipeCache`/`hpkgInfoCache`), and its clock is 24 h 24 m behind because it
  predates the `system_time()` fix. Do not use it.
- **`tail` on a build log destroys the diagnosis.** Both `worker.sh` and the two earlier
  libtool attempts piped haikuporter through `tail -14`/`-18`, which cut off exactly the
  `./bootstrap: git: command not found` lines that explained the failure. Redirect the
  full log to a file in the guest, scp it out, then grep the region you need.
  `/opt/haiku/gworker.sh` is `worker.sh` with that fixed, and asserts success by the
  hpkg existing rather than by an exit code.
- **A fresh guest is not a configured guest.** Guest 2231 had no
  `/boot/home/config/settings/haikuports.conf` and every build failed instantly with
  `Unable to find haikuports.conf`; copy `/opt/haiku/hp.conf` there.
- **haikuporter reuses a polluted work directory** (`Skipping unpack of ...`). Delete
  `<sources>/<port>-<ver>-N/work-<ver>` before re-testing a recipe change, or you are
  measuring the debris of previous failures.

## Blocker 5 — the "netpbm cycle" is **two** independent knots — **BROKEN**

Patches: `gettext-1.0-groff-doc-cut-stage1.patch`,
`zstd-1.5.6-makefile-not-cmake-stage1.patch`.

**A correction found by measuring instead of assuming.** The autoconf stage-1 note
implied the `makeinfo` stub produced empty documentation. It did not: `autoconf.info`
is **1,221,536 bytes in both** the stage-1 and the retired build, byte-for-byte the
same size, because the tarball **ships prebuilt `.info` files** and `make` installed
those regardless of what the stub wrote. What the cut actually cost was only
`make install-html` — the stage-1 hpkg has no `.html` entry whatsoever, and the
rebuild adds `autoconf.html` (2,274,731 B) and `standards.html` (412,748 B). So the
debt was real but a third the size it was written up as, and the same caution applies
to the gettext cut, whose HTML man pages ship prebuilt too.

The single biggest error in the earlier write-up was treating this as one cycle.
It is two, sharing no edge, and only one of them was ever on the path to
`zstd`/`openssl3`.

**Knot A — documentation (gates `gettext` only).**

```
groff -> cmd:pnmcrop, cmd:pnmtopng, cmd:pnmtops (netpbm) + cmd:psselect (psutils)
      -> netpbm -> libjasper, libjpeg, libpng16, libtiff, libxml2
groff also needs cmd:makeinfo, which is the non-functional texinfo_bootstrap stub
```

`groff` therefore needs **five** unavailable commands, not one, and `cmd:pnmcrop`
was never special. But `gettext` needs `groff` for *one thing*: `MAN2HTML = groff
-mandoc -Thtml` (`gettext-{runtime,tools}/man/Makefile.in:2299`/`:3957`). Dropping
`cmd:groff` from gettext's BUILD_PREREQUIRES — **one line** — released the entire
downstream chain, and `groff` never had to be built at all. The HTML man pages
ship prebuilt in the tarball, so the package is complete; see the patch for why
`all-am` *does* reach `$(man_HTML)` and why that still does not invoke groff.

**Knot B — linkage (gates `zstd`, `openssl3`, and cmake itself).**

```
cmake    --BUILD_REQUIRES devel:libcurl--> libcurl
libcurl  --BUILD_REQUIRES devel:libssl --> openssl3
openssl3 --BUILD_REQUIRES devel:libzstd--> zstd        (openssl3-3.5.7.recipe:80)
zstd     --BUILD_PREREQUIRES cmd:cmake --> cmake       (zstd-1.5.6.recipe:102)
```

This knot has nothing to do with netpbm or groff. **It was broken by leaving cmake
alone and building `zstd` with its own upstream Makefile**, which removes
`cmd:cmake` from the picture entirely. `openssl3` then follows from `devel:libzstd`.

Corollary worth keeping: `zstd` was recorded here as "blocked on `xz_utils`
(`devel:liblzma`)". liblzma was real but not binding — `cmd:cmake` was. Read the
whole `BUILD_PREREQUIRES`, not just the dependency the last failure happened to
name.

## Blocker 6 — cmake — **SOLVED (2026-08-24). There was never a cycle.**

Recipe (source of truth): `graviton/haikuports-patches/recipes/cmake-4.1.6.recipe`.
`cmake-4.1.6-bundled-libs-stage1.patch` is kept **only** as a record of a route that
was taken for the wrong reason; do not use it.

**The headline: cmake 4.1.6 builds natively on arm64 with the *pristine* upstream
recipe and no cut at all.** What was missing was never a cycle — it was five
unbuilt leaves.

### The cycle in this document was real once and had already dissolved

Every earlier write-up, including this one, asserted:

```
cmake --devel:libcurl--> curl --devel:libssl--> openssl3 --devel:libzstd--> zstd --cmd:cmake--> cmake
```

That loop was genuine when it was written. **It stopped existing the moment
`zstd` and `openssl3` were built** (Blocker 5/8) — the `zstd -> cmd:cmake` edge was
removed by building zstd from its own Makefile, and with it the only path back to
cmake. Afterwards `devel:libcurl` was just an **unbuilt leaf**, and the document
went on calling it "the one genuine cycle edge" for another day. The
`cmake-4.1.6.recipe` comment even wrote `(unbuilt)` next to it and still called it
a cycle edge.

What actually stood between us and native curl was three more leaves and one
doc-dependency cut, all cheap:

| Port | What it needed | Time |
|---|---|---|
| `expat-2.8.2` | autotools; nothing missing | 46 s |
| `rhash-1.4.6` | plain `./configure`; nothing missing | 30 s |
| `libuv-1.52.1` | autotools; nothing missing | 41 s |
| `nghttp2-1.63.0` | `haiku_devel` only | 49 s |
| `ca_root_certificates` | pure data package, no build deps at all | 13 s |
| `libssh2-1.11.1` | `devel:libssl/libcrypto` (openssl3, built) | 36 s |
| `curl-8.21.0` | the three above + a `libpsl` cut | 1 m 51 s |

**Total: under six minutes of build time for the thing that had blocked the chain
for days.** Not one of them needed `cmd:cmake`; not one was in a cycle.

### `libuv` does not need cmake — the question that gated everything

It is **autotools**: `./autogen.sh` then `./configure`, and its
`BUILD_PREREQUIRES` is `awk autoconf automake gcc ld libtoolize make pkg_config`,
every one already built. Its recipe *already* carries `LDFLAGS="-lnetwork"`, so
upstream knows about Haiku. There was no libuv/cmake sub-cycle to settle and no
`_bootstrap` recipe variant was needed.

### The `libpsl` cut, and why it is the only one left

`curl` wants `devel:libpsl`; `libpsl` wants `libidn2`; `libidn2-2.0.5.recipe`
**does exist in the tree** (`net-dns/libidn/libidn2-2.0.5.recipe` — an earlier note
that it was absent is wrong) but needs `cmd:gtkdocize` from gtk-doc, which is not
buildable here. curl's recipe already carried `--without-libpsl` behind an
`x86_gcc2` guard, so the cut is to apply it on every architecture: three
guarded blocks, in `graviton/haikuports-patches/recipes/curl-8.21.0.recipe`.
This is exactly the class of cut that worked for `cmd:groff` in gettext — it drops
a dependency edge, not a capability curl needs here. PSL supplies public-suffix
checking for cookie-domain validation only.

### The bundled-curl route worked, and shipped a cmake with no TLS

This is the part worth propagating, because the build **succeeded** and the defect
was invisible from the build log, from `RC=0`, and from a functional
"does cmake compile a project" test — all of which passed.

Bundling only curl (keeping `--system-expat --system-librhash --system-libuv`)
does build, and the resulting cmake configures and builds real projects. But
`Utilities/cmcurl/CMakeLists.txt:24` reads
`CURL_USE_OPENSSL="${CMAKE_USE_OPENSSL}"`, and **`CMAKE_USE_OPENSSL` is set
nowhere in cmake's top-level `CMakeLists.txt`** — so bundled cmcurl is compiled
with no TLS backend whatsoever, even though `openssl3` was built and
`devel:libssl` was available:

```
file(DOWNLOAD https://...)  ->  1;"Unsupported protocol"
file(DOWNLOAD http://...)   ->  0;"No error"
```

`FetchContent`, `ExternalProject_Add` and `ctest --submit` are all dead in that
build, and since nearly every modern CMake project fetches over https, the
breakage would have surfaced later as *the other port's* bug. Two further costs:
`CMakeLists.txt:96-97` forces system nghttp2 **only** when system curl is used, so
bundling curl also bundles nghttp2 (27 `cmnghttp2` + 177 `cmcurl` objects), and
the bundled curl is **8.14.1** against HaikuPorts' **8.21.0** — seven minor
releases behind in network-facing code, and invisible to `pkgman` because
`lib:libcurl` had been commented out of `REQUIRES`.

**Rule this yields: when a cut removes a library, test the capability that library
provided, not just that the build finished.** A build-tool package can pass every
build-level test and still have had a feature silently amputated.

### The `-lnetwork` finding — still true, still worth keeping

If you ever *do* bundle curl: `Utilities/cmcurl/lib/transfer.c:57` fires
`#error "We cannot compile without socket() support!"` because `HAVE_SOCKET` is
undefined. `Utilities/cmcurl/CMakeLists.txt:751-753` has a Haiku branch, but it
appends `network` only to `CURL_LIBS`, never to `CMAKE_REQUIRED_LIBRARIES`, which
is what the `check_symbol_exists("socket" ...)` probe at `:1987` links against —
and Haiku keeps `socket()` in `libnetwork`. `export LDFLAGS="-lbsd -lnetwork"`
seeds `CMAKE_EXE_LINKER_FLAGS` and fixes it (verified: `-- Looking for socket -
found`). Note this works only because Haiku's `ld` is not `--as-needed`, which
puts `-lnetwork` before the object on the probe's link line; and note `cmcurl` has
**no `HAVE_LIBNETWORK` probe at all**, so the `elseif(HAVE_LIBNETWORK)` at `:1976`
can never fire. **None of this is needed by the all-native build**, which is why
no patchset hunk was added: the right fix was to stop bundling curl.

### "cmlibuv is not cheaply fixable" was wrong, and nobody should trust it

Recorded so the word *dead end* is not trusted again. The failure was **109
reference lines over 9 distinct symbols**, not ~120 symbols, and
`Utilities/cmlibuv/src/unix/haiku.c` **is shipped in the tarball**. All 9 are
defined by four in-tree files — `posix-poll.c`, `posix-hrtime.c`,
`no-proctitle.c`, `no-fsevents.c` — which are byte-for-byte the **QNX** branch's
file list, sitting four lines above where a Haiku branch should be in cmlibuv's
CMake platform dispatch. A ~10-line CMake block would have fixed it. The route is
still not worth taking (all-native is better), but it was never a dead end.

The one genuinely useful signal from that episode stands: cmake's **bootstrap**
has its own hand-written libuv file list, which is why the bootstrap linked and
only the real build failed.

### Habits this blocker paid for twice

1. **Check whether an unresolved edge is a genuine cycle or just an unbuilt leaf.**
   `cmd:which` (15 KB, under a minute) taught this; `devel:libcurl` had to teach it
   again, and it cost more the second time because the ledger had written the wrong
   answer down as fact.
2. **Build a provides index before concluding anything is blocked.** Reading
   recipes alone, `gawk`, `gperf`, `bison`, `flex`, `yacc` and `lib:libicudata`
   all looked missing. Every one was already provided by a `_bootstrap` package.
   The index is one loop:

   ```sh
   for f in packages/*.hpkg /boot/system/packages/*.hpkg; do
     package list -i "$f" | grep provides:
   done | sed -e 's/^\s*provides:\s*//' -e 's/ .*//' | sort -u
   ```
3. **`provides` is not function.** `cmd:makeinfo` resolved happily for weeks from
   `texinfo_bootstrap`, which is a stub that dies in its `BEGIN` block. The index
   answers *resolvability*; only running the tool answers *works*. Where two
   packages provide the same `cmd:`, quarantine the bad one rather than trusting
   the solver's choice — that is how `autoconf` finally got real docs.
4. **Read the *first* failure.** `libssh2` failed on `lib:libcrypto`, which looked
   like an openssl3 problem. The real line above it was
   `requires "ca_root_certificates" of package "openssl3-3.5.7-1" could not be
   resolved` — a missing pure-data package.
## Blocker 7 — the chroot clock bug, and why it invalidated every package — **ROOT-CAUSED ELSEWHERE, CONSEQUENCES HERE**

The ~18-23 h backwards chroot clock was root-caused (by parallel work) to a
**stale `haiku.hpkg` in `/boot/home/haikuports/packages/`** — a different file
from the one the guest boots — whose `libroot.so` still had the pre-fix
`system_time()` reading `CNTPCT_EL0`. It was self-perpetuating because the harvest
step copied `guest:packages/*.hpkg` back into `/opt/haiku/hpkg-out/arm64/`, the
guest seed. Two consequences landed squarely on this work:

1. **It made cmake unbuildable for a reason that looks nothing like a clock bug.**
   `Source/Checks/cm_cxx_features.cmake:67`

   ```cmake
   if(check_output MATCHES "(^|[ :])[Ww][Aa][Rr][Nn][Ii][Nn][Gg]")
     set(CMake_HAVE_CXX_${FEATURE} OFF CACHE INTERNAL "TRY_COMPILE" FORCE)
   ```

   treats *any* unfiltered "warning" in a `try_compile` output as "feature
   broken". Its filter list (lines 39-65) covers ninja, MSBuild, MSVC, ld, distcc,
   icpc and xcodebuild — but **not GNU make**. So

   ```
   make: Warning: File 'Makefile' has modification time 82393 s in the future
   make[1]: warning:  Clock skew detected.
   ```

   turned all three of `make_unique`, `unique_ptr` and `filesystem` into "no", and
   cmake hard-errored at `CMakeLists.txt:168` *"The C++ compiler does not support
   C++11"*. `CMakeError.log` shows both the compile and the link succeeding with
   **zero compiler diagnostics** — there was never a C++ problem. Worked around by
   pre-seeding the three cache variables via `bootstrap --init=FILE` (line 6's
   `if(NOT DEFINED ...)` guard); the values are not guesses, cmake's own bootstrap
   had already proved them by passing `-DCMake_HAVE_CXX_MAKE_UNIQUE=1
   -DCMake_HAVE_CXX_FILESYSTEM=1`.

   **The seed is now retired, and that was verified rather than assumed.** cmake
   4.1.6 was rebuilt on the repaired guest 2227 from
   `cmake-4.1.6-bundled-libs-stage1.patch` **plus `LDFLAGS="-lbsd -lnetwork"` and
   with no `--init=` cache file at all**. Result: **0** occurrences of *"does not
   support C++11"*, **0** clock-skew or "in the future" warnings in a 2581-line log,
   and the build ran to **97%** before dying on the same **120 undefined `uv__*`
   references** documented below. So the C++11 error really was a clock artefact,
   nothing else was hiding behind it, and Blocker 6's dead end is independently
   reconfirmed at the same place.

2. **Fixing the clock invalidated the entire existing native package set.** Every
   base package built so far records

   ```
   requires haiku >= r1~beta6_hrev59996_dirty-1
   ```

   because it was built against the stale, `_dirty` chroot `haiku`/`haiku_devel`.
   A guest whose chroot has been repaired provides the **non-dirty**
   `r1~beta6_hrev59996-1`, so none of them resolve any more:

   ```
   requires "haiku_devel >= r1~beta6_hrev59996_dirty-1" of package "xz_utils_devel-5.8.3-1" could not be resolved
   requires "haiku >= r1~beta6_hrev59996_dirty-1" of package "libiconv-1.18-1" could not be resolved
   ```

   This is not confined to one package. A survey of all 48 non-bootstrap hpkgs on
   the guest shows **every base package carries a `_dirty` reference** (the
   `_devel`/`_debuginfo` subpackages mostly do not, because they require only their
   own base — `xz_utils_devel` is an exception, since its recipe lists
   `haiku_devel` explicitly). So on a repaired guest the chain cannot even rebuild
   `gettext`, because that needs `lib:libiconv` → `libiconv-1.18` → dirty `haiku`.

   **The whole set had to be rebuilt bottom-up against the fixed `haiku_devel`.**
   That was done on 2026-08-24 — see Blocker 8. All 52 hpkgs are now non-dirty and
   the two package sets are no longer mutually incompatible; the pre-fix set is kept
   only as evidence, in `/opt/haiku/hpkg-out/arm64-dirty-20260824/`.

## Blocker 8 — the bottom-up rebuild against the non-dirty `haiku` — **DONE (2026-08-24)**

**23 ports, 52 hpkgs, rebuilt in 34 minutes of wall clock** (00:45:29 → 01:19 UTC)
across four QEMU guests, every one verified by `ls` of its hpkg and by the absence of
a `_dirty` requirement inside it. Nothing was relabelled:
**0 of the 52 files is byte-identical to its pre-fix predecessor.**

### What was actually decisive

Three proofs, in increasing order of how much they settle:

1. **Per-hpkg requirement check.** Every rebuilt package answers
   `requires: haiku>=r1~beta6_hrev59996-1`. Across all 52, `package list -i | grep -c
   _dirty` is **0**.
2. **`pkgman install` works.** The old note in this document — *"nothing built in these
   guests can be `pkgman install`ed"* — no longer holds. On a repaired guest,
   `pkgman install gzip-1.14-1-arm64.hpkg` completes and `gzip --version` prints
   `gzip 1.14`.
3. **The negative control on the same guest.** A pre-fix hpkg from the snapshot is
   still refused there, with the exact original error:

   ```
   problem 1: nothing provides haiku>=r1~beta6_hrev59996_dirty-1 needed by lz4-1.9.4-2
   ```

   Same guest, same `pkgman`, one package installs and the other does not. That rules
   out "the solver got more permissive" as an explanation.

Beyond that, the chain proved itself as it went: `gettext` consumed the rebuilt
`libiconv`, `zstd` consumed the rebuilt `xz_utils`/`lz4`/`zlib`, and `openssl3`
consumed the rebuilt `zstd`. A bottom-up rebuild that resolves at every step *is* the
consistency test.

### The corrected clock, measured

**Zero** clock-skew warnings — `grep -c -i 'clock skew\|modification time.*in the
future'` over all **25** full build logs returns 0 everywhere. Before the fix these
appeared in essentially every make-based port (`66159 s in the future` during libtool,
`64825 s` during sqlite, `82393 s` during cmake). This is the first time the whole set
has been built under a correct clock, and the one thing that visibly changed as a
result is cmake's C++11 check (Blocker 6/7).

No port behaved differently in any other way: every one that had built before built
again, with the same recipe edits and no new failures.

### How the work was split

The graph is a **spine plus leaves**, so four guests do not give four-way speedup:

```
perl -> help2man -> m4 -> autoconf -> automake -> tar -> libtool -> libiconv
     -> gettext -> xz_utils -> zstd -> openssl3          (12 serial steps)
```

| Guest | Ports |
|---|---|
| 2230 | the spine: `perl help2man m4`… then `diffutils autoconf automake tar libtool libiconv gettext xz_utils zstd openssl3` |
| 2222 | `which patch gzip`, then `zlib lz4 zip pkgconf bzip2`, then `readline sqlite`, then `m4` |
| 2231 | recycled mid-run (see below); ended as the verification guest |
| 2227 | the cmake C++11 experiment |

`graviton/builder/rebuild.sh` is what makes several guests cooperate on one dependency
graph: before each port it pushes every package in the shared pool
(`/opt/haiku/hpkg-out/arm64-nondirty/`) that the guest does not yet have, and after each
port it harvests back into that pool **and** over the canonical `hpkg-out/arm64/`. The
canonical copy is overwritten with `cp --remove-destination`, because the file of the
same name there is the pre-fix build and a plain `cp -p` onto a hardlink would also
rewrite the snapshot kept as evidence.

`graviton/builder/prepguest.sh` is the other half: a rebuild guest must start with a
`packages/` directory holding **only** the chroot's inputs — the 5 repaired
`haiku*.hpkg`, the 28 cross-built `_bootstrap` packages and
`makefile_engine`/`netfs`/`userland_fs`: 36 files. Leaving the pre-fix
hpkgs there is what blocked the repaired guests in the first place: haikuporter picks
`libiconv-1.18` to satisfy `lib:libiconv` and *then* fails to resolve its `_dirty`
requirement. They are **moved**, to `/boot/home/dirtypkgs/`, not deleted, so
`package extract` still works on them and a wrong diagnosis destroys nothing.

### Four things that cost time, none of them the rebuild itself

1. **One missing package looked like five broken ports.** `autoconf`, `automake`, `tar`
   and `libtool` all failed within seconds of each other; the reasons read as a cascade
   (`cmd:cmp`, then `cmd:autoconf`, `cmd:automake`, `cmd:aclocal`) but the root was a
   single ordering mistake — **`cmd:cmp` comes from `diffutils`**, which had been queued
   on a different guest. `diffutils` first, and all four built. Read the *first*
   failure's reason, not the last.
2. **`ls | grep "^m4[-_]"` reported an `m4` that was never built.** It matched
   `m4-1.4.19_bootstrap-1-arm64.hpkg`, which is a build *input*. "Verify by `ls`, not by
   exit code" is right but insufficient: the check must exclude `_bootstrap`, and the
   real safety net was diffing the finished 52 names against the list being replaced.
   `m4` was then built on its own and the set completed.
3. **Guest 2231 was wedged and had to be recycled.** Its `zlib` sat for six minutes with
   a 238-byte log. `ps` showed the exact signature from Blocker 4: the unkillable
   `./conftest` (pid 69517, still alive from an earlier `tar` attempt), a stuck
   `unmount -f`, and a hung `mount -t bindfs -p "source /dev" dev`. Nothing builds on
   such a guest. `mkguest.sh 11` produced a working replacement in **4 minutes**, which
   is the real lesson: recycling a wedged guest is far cheaper than investigating one.
4. **Overwriting a running script corrupts the running copy.** `rebuild.sh` was
   reinstalled with the `_bootstrap` fix while guest 2230's copy was mid-loop; bash
   resumed at a shifted byte offset and executed fragments —
   `remove-destination: command not found`, `syntax error near unexpected token 'done'`.
   It happened after the last harvest so nothing was lost, but install under a new name
   or wait for the `.done` marker.

### Guest capacity

Four builders instead of two, which is the single biggest lever on wall clock here:

- **2222** — the pre-fix guest (24 h clock skew, corrupt haikuporter pickle cache) was
  killed and recreated from `mkguest.sh 2`. It also carried the stray
  `-object filter-dump` that had grown `/opt/haiku/logs/builder-boot.pcap` to **5.6 GB**;
  killing that qemu stopped it and the file was deleted.
- **2227** — was simply down (connection refused). Recreated with `mkguest.sh 7`.
- **2231** — recycled mid-run as above.
- **2229** — the last `_dirty`-consistent guest, deliberately untouched until the rebuild
  finished. **Now swapped**: its five chroot `haiku*.hpkg` were replaced with the repaired
  ones (`_dirty-1` → `r1~beta6_hrev59996-1`) and it was run through `prepguest.sh`. All
  five guests now report the same non-dirty chroot version.

Because `mkguest.sh` seeds from `hpkg-out/arm64/`, and that directory now holds the
repaired `haiku*.hpkg` **and** the rebuilt 52, any guest made from here on is consistent
by construction.

## Blocker 9 — the rest of the chain: deep, no cycles — **groff CLOSED, browser OPEN**

> **Resolved for groff on 2026-08-24 16:25Z.** `groff-1.23.0-2-arm64.hpkg` is built and
> **verified by rendering**, not by RC=0. The section below records the method and the
> corrections; the outcome is at the end under "What actually happened".


~~This is the only open blocker.~~ **Blocker 9 is now closed too, and there is a Blocker
10** — the `LIBRARY_PATH` loader, and behind it an arm64 kernel panic in `mprotect()`. Read
Blocker 10 for the current state. This section replaces "Why groff *used to be* out of
reach" below, which is wrong in its central claim and is kept only as a record.

### Method: stop reading recipes, saturate the graph

Every cycle claim in this document's history was made by reading recipes and following the
edge that the last failure happened to name. That method produced three wrong answers in a
row (`zstd -> cmd:cmake`, `devel:libcurl`, and the netpbm/OpenGL story below). So this pass
used haikuporter's own machine-readable graph instead — the 3667
`haikuports/repository/*.DependencyInfo` files — with
**`graviton/builder/depclosure.py`**.

It works by **saturation**, deliberately not by a backward walk from the target:

> Start from the provides that the built hpkgs actually supply. Repeatedly promote any
> recipe whose *every* build dependency is already satisfied, adding what it provides to
> the satisfied set. Run to a fixpoint.

That settles the cycle question with no judgement calls. If the target is reached it is
**not** behind a cycle — it is behind however many waves the saturation took, and each wave
is a set of ports that can be built in parallel. If the target is never reached, whatever
is still unreached at the fixpoint is the genuinely stuck set. A backward walk cannot do
this honestly, because when several ports provide the same name — `jasper` *and* `jasper7`
both provide `devel:libjasper`; `python3.10` *and* `python3.14` both provide a `cmd:python3*`
— unioning the providers inflates the answer. Saturation just needs one of them.

### Result as first measured: `groff` is 24 ports and 12 waves away, and not a cycle

> The "no cuts" this section originally claimed **did not survive contact with jasper**: one
> cut turned out to be needed, against GLUT's absence. Corrected under "The jasper OpenGL cut
> was needed" below. The port count and the absence of a cycle both held.

```
wave 1  (5)  autoconf_archive libedit libffi libpng16 nasm
wave 2  (3)  libjpeg_turbo python3.10 python3.14
wave 3  (4)  flit_core ninja setuptools tiff
wave 4  (1)  installer
wave 5  (4)  psutils pyproject_hooks tomli wheel
wave 6  (1)  build
wave 7  (1)  meson
wave 8  (1)  libglvnd
wave 9  (1)  glu
wave 10 (1)  jasper
wave 11 (1)  netpbm
wave 12 (1)  groff
```

**Wave 1 is built and verified** (2026-08-24 14:54–14:56Z): `autoconf_archive`, `libedit`,
`libffi`, `libpng16`, `nasm` — five ports in **101 seconds**, every one from its **pristine
recipe** with no edit, each confirmed by its hpkg and by `package list -i | grep -c _dirty`
returning 0. The graph said they were leaves and they were.

### Three claims in the old section that are wrong

1. **"`netpbm` → `jasper` → OpenGL → `mesa-25.3.6` → `libLLVM` + `libvulkan` +
   `cmd:glslangValidator` + `cmd:git`" — wrong.** `devel:libgl` in this tree is provided by
   **`libglvnd-1.7.0`**, whose entire build requirement list is
   `awk gcc ld meson ninja python3 sed haiku_devel`. No mesa, no LLVM, no vulkan, no git.
   `libglvnd` does need a two-line Haiku portability fix to compile at all (see below), but
   that is a fix, not a cut, and it costs about seventy seconds together with `glu`.

   **What this does *not* license, though an earlier revision of this document said it did:**
   the conclusion that jasper needs no OpenGL cut. It does. jasper's cmake finds OpenGL
   perfectly well and then fails on **GLUT**, which has no recipe in the tree at all, so
   `-DJAS_ENABLE_OPENGL=OFF` and dropping `jiv` are **still required** — for GLUT's absence,
   not for a mesa wall. Only the *mechanism* in this claim was wrong; the remedy it argued
   against is the remedy that worked.
2. **"`psutils` is the harder half, and it is the exact knot `--do-bootstrap` was abandoned
   over" — wrong; it is a ladder.** The PEP-517 set is
   `flit_core → installer → {setuptools, wheel, tomli, pyproject_hooks} → build`, seven
   ports, strictly ordered. The reason it is not a knot is one fact worth writing down:
   **`flit_core`'s only build requirements are the two Python interpreters** — it does *not*
   need `installer`. That is by upstream design; flit_core is the one PEP-517 backend that
   can install itself, which is precisely why the ecosystem bootstraps through it. Once
   `flit_core` exists, `installer` follows, and everything else follows from `installer`.
3. **"`git` is not buildable here" — wrong.** `git-2.54.0` is reachable at wave 14; its only
   unsatisfied requirements are `cmd:man` (mandoc), `cmd:nano` (nano) and
   `devel:libpcre2_8` (libpcre2). It is not needed for anything on the critical path, so
   this is a correction rather than a plan.

**What *is* right in the old section:** `groff` genuinely needs four commands beyond
`cmd:makeinfo` (`cmd:pnmcrop`, `cmd:pnmtopng`, `cmd:pnmtops`, `cmd:psselect`), and the
estimate that a real groff "should be scoped as its own piece of work" was sound. It was
wrong about *why*, and it under-counted the depth by about five times ("about five more
ports" versus 24). ~~the `gettext` cut therefore still stands~~ — **that clause is dead as
of 2026-08-24**: groff is built, `cmd:groff` was restored, and gettext was rebuilt from the
restored recipe. See the cut ledger.

### The single chokepoint is Python, and it has a real defect under it

Everything above wave 2 hangs off `python3.14`/`python3.10`. Both are **one build** from
done — three of their four dependencies were wave 1, now built — and both **failed on
first attempt for two different reasons, neither of them a missing dependency**. All
dependencies resolved; the builds reached the compiler.

| Port | First failure | Cause |
|---|---|---|
| `python3.14` | `cc1: error: LTO support has not been enabled in this configuration` | the recipe sets `--enable-optimizations --with-lto` when `optimizedBuild=true`, and the cross-built bootstrap **gcc 13.3.0 in this image has no LTO support compiled in**. `configure` cheerfully reports `checking for --with-lto... yes` — it probes the flag, not the capability |
| `python3.10` | `runtime_loader: Cannot open file libnetwork.so (needed by /sources/Python-3.10.20/python)` then `generate-posix-vars failed` | PGO runs the freshly linked `./python` to produce `pybuilddir.txt`, and **`LIBRARY_PATH` replaces the loader path rather than prepending to it**, so `/boot/system/lib` drops out. Same defect class as `perl-5.42.2-library-path.patch` |

Both are the *optimisation* machinery, not Python. The cut — `graviton/builder/pyfix.sh` —
blanks `maybeEnableOptimizations` in the one line of each recipe that sets it, and
**deliberately keeps `-O3`**: the alternative switch `optimizedBuild=false` also drops to
`-O0`, and this interpreter is about to build the whole PEP-517 ladder, meson and ninja.

Fixing only that exposed a **second, independent defect underneath**, which is the more
generally useful of the two because it is version-independent and it is *ours*, not
upstream's. With PGO gone, both versions then failed at the identical place:

```
LIBRARY_PATH=/sources/Python-3.10.20 ./python -E -S -m sysconfig --generate-posix-vars
runtime_loader: Cannot open file libnetwork.so (needed by .../python)
generate-posix-vars failed
make: *** [Makefile:1159: pybuilddir.txt] Error 3
```

Python's generated `Makefile` sets `RUNSHARED= LIBRARY_PATH=<build dir>` (line 199 in 3.10)
so the freshly linked `./python` can find its own `libpython3.x.so`. But **Haiku's
`runtime_loader` replaces the default library search path with `LIBRARY_PATH` instead of
prepending to it**, so `/boot/system/lib` drops out and `libnetwork.so` — which the recipe
links via `LIBS="-lnetwork -lintl -lbsd"` — becomes unfindable. `Error 3` is the loader's
own exit code, the same signature `perl-5.42.2-library-path.patch` documents for `LDLIBPTH`.
The remedy is the same: make the path **additive**, and specifically *not* empty, because
the build directory is genuinely needed.

**This is a third instance of one underlying Haiku defect** (`perl`'s `LDLIBPTH`, this
`RUNSHARED`, and the general note in "Operational hazards"). It will recur in any port whose
build runs a freshly linked binary out of its own build tree. It is worth fixing in
`runtime_loader` rather than patching recipe by recipe — there is an upstream `TODO` at
exactly that spot.

**Result — measured 2026-08-24 15:14–15:22Z, both from `RC=0` *and* the hpkg:**

| Port | Result | Evidence |
|---|---|---|
| `python3.10-3.10.20-3` | **built** | `python3.10-3.10.20-3-arm64.hpkg`, 17,178,266 B, `_dirty` count 0, 7371-line log (the failing runs were 1027–1290 lines) |
| `python3.14-3.14.7-1` | **built** | `python3.14-3.14.7-1-arm64.hpkg`, `_dirty` count 0, 5634-line log |
| `file-5.43-2` | **built** | needed to *activate* python3.10 (`cmd:file`); a wave-1 leaf, pristine recipe |

**The compression extensions are present**, confirmed by listing the package contents
rather than by the build log:

```
zlib.cpython-310.so     151936      _ssl.cpython-310.so      317752
_bz2.cpython-310.so     144480      _sqlite3.cpython-310.so  174952
_lzma.cpython-310.so    151760      readline.cpython-310.so  150168
```

So **Blocker 3's residual 1 is substantively answered** — an interpreter that can unpack
compressed tarballs unaided now exists as a package.

> **Not yet verified, and deliberately not claimed: that the interpreter *runs* with those
> modules.** Activating it needs two more runtime dependencies (`lib:libbz2` from `bzip2`,
> and a `file_data` subpackage that the `file` build did not emit), so `pkgman install`
> still refuses. Until then `python3.10` on `PATH` is the **bootstrap**, and the version is
> the discriminator: the bootstrap is **3.10.21**, the port is **3.10.20**.
>
> This nearly produced a confident false negative. The first capability run reported all of
> `zlib`/`bz2`/`lzma`/`ssl`/`sqlite3`/`readline` **missing** — it was testing the bootstrap,
> because the install had failed and both packages provide `cmd:python3.10`. `graviton/builder/pycheck.sh`
> now pins the expected version and **exits 2 rather than report module results for an
> unconfirmed binary**. Blocker 6's habit 3 was *"`provides` is not function"*; this is the
> next step down — **resolving a `cmd:` tells you nothing about which binary actually ran.**

**State of the critical path after this session** (same tool, re-measured): `groff` is down
from 24 ports / 12 waves to **17 ports / 10 waves**, and the four ports to build next are
all leaves with every dependency satisfied — **`flit_core`, `libjpeg_turbo`, `ninja`,
`setuptools`**. `flit_core` is the one that matters: it opens `installer`, and `installer`
opens the rest of the PEP-517 ladder.

> **Note the shape of the `python3.14` failure, because it generalises.** A `configure`
> check that tests whether the *compiler accepts a flag* is not a test of whether the
> feature works. This is the same lesson as "`provides` is not function" (Blocker 6, habit
> 3) one level down: `cmd:` resolvability does not imply the tool works, and flag
> acceptance does not imply the capability exists. Anything else in the tree that turns on
> `-flto` will fail identically on this image.

### Distance to a browser

Asked of the same graph, with the same method.

**`haikuwebkit-1.10.0` is reachable, and not behind a cycle** — but only after two
dependency cuts, and one of the 34 ports is LLVM:

```
wave 1  (8)  gdbm giflib gmp libexecinfo libjpeg_turbo libyaml python3.10 python3.14
wave 2  (6)  flit_core libxslt ninja ruby setuptools tiff
wave 3  (4)  brotli installer lcms libwebp
wave 4  (5)  psutils pyproject_hooks tomli wheel woff2
wave 5  (1)  build
wave 6  (1)  meson
wave 7  (2)  dav1d libglvnd
wave 8  (2)  glu libavif1.0
wave 9  (1)  jasper
wave 10 (1)  netpbm
wave 11 (1)  groff
wave 12 (1)  llvm          <- see the caveat below
wave 13 (1)  haikuwebkit
```

Only **two** edges are genuinely hard, and both have an established precedent for cutting:

- **`devel:libavif` → `libavif1.0` → `devel:librav1e` → `rav1e`, which needs
  `cmd:cargo`/`cmd:cargo_cbuild`/`cmd:cargo_cinstall` — nothing in the tree provides any of
  them.** That is a Rust toolchain, and it is the one genuinely absent thing found in this
  pass. But `rav1e` is an AV1 **encoder**; a browser needs to **decode** AV1, and the
  decoder `dav1d` is reachable at wave 7. So this should be an
  `-DAVIF_CODEC_RAV1E=OFF`-shaped cut, not a Rust bootstrap.
- **`devel:libpsl` → `libpsl` → `devel:libidn2` → `libidn2` → `cmd:gtkdocize` → `gtk_doc`.**
  `curl-8.21.0` already carries exactly this cut (`--without-libpsl` on every architecture),
  so the precedent is in this repository. Note that `gtk_doc` itself is *not* impossible —
  it needs `pygments`, `itstool`, `meson`, `libxslt` and the two docbook packages, all
  reachable — so this is a cut of convenience, not of necessity.

**Caveat on the LLVM row, stated because the tool cannot see it:** `depclosure.py`
deliberately ignores version constraints, so its minimal set picks `llvm12`. haikuwebkit
actually requires `cmd:llvm_config >= 21`, i.e. `llvm21`/`llvm22`. That is still reachable
(its unsatisfied deps are `cmd:groff`, `cmd:ninja`, `cmd:python3.10` and
`setuptools_python310`, all on the list above) but **the port count is misleading about the
time**: LLVM is a multi-hour build on its own, and it depends on `groff`, so a browser
inherits the entire groff chain rather than avoiding it.

**A lighter browser needs no cut of its own.** `netsurf-3.11` saturates at wave 15 with a
52-port minimal set and **no netsurf-specific recipe edit** — no LLVM, no Rust, no libpsl.

Two corrections to how that was first written up here, both found by pricing it properly:

- It is **not** "zero recipe edits" and **not** free of cut debt. netsurf build-requires
  `cmd:git`, `git` sits above `groff`, and groff carries the jasper `jiv`/GLUT cut. netsurf
  inherits that cut like everything else downstream of groff.
- It therefore does **not** avoid the groff chain, which was the main reason to prefer it.
  That chain was on the critical path to **both** browsers, so it was shared work and browser
  choice could safely be deferred until after it — which is what happened.

What survives, and is still the interesting part: netsurf needs **no LLVM**, so its cost is
breadth (~15 netsurf-specific libraries plus `git`, `vim`, `ruby`) rather than one multi-hour
build. Worth pricing against WebKit on that basis, not on cut debt.

### Honest summary of the distance

Counts are from 15:26Z, i.e. **after** this session's nine ports.

| Target | Cycle? | Ports left | Cuts needed | Real cost driver |
|---|---|---|---|---|
| `python3.10`/`python3.14` | no | **0 — DONE** | PGO/LTO cut + additive `RUNSHARED` | both built |
| PEP-517 ladder → `meson`/`ninja` | no | **0 — DONE** | none | 8 ports, ~4 min total |
| `groff` | no | **0 — DONE** | 1 (jasper `jiv`, for GLUT) | done; **verified by rendering** |
| `netsurf` | no | **0 — BUILT** (`depclosure.py` measured **38** for the chain; the earlier "~22" was wrong. All 38 built, in about 75 minutes) | **0 new of its own.** netsurf's recipe is unmodified. It *inherits* jasper's GLUT cut and vim's two cuts, and the `json_c` compatibility flag | done |
| `haikuwebkit` | no | **~14** | **1 new** (rav1e codec) + groff's | **LLVM ≥ 21** — hours, and it is most of the remaining cost |

**"Cuts needed" counts *new* cuts.** Everything downstream of `groff` inherits the jasper
`jiv`/GLUT cut, so neither browser is cut-free; the column says what each *adds*. That
distinction is the one this document got wrong twice, in opposite directions.

## Blocker 10 — the netsurf chain is gated on the *loader*, not on any recipe

**Measured 2026-08-24, 18:05Z–18:40Z.** Running the closure produced 35 of netsurf's 38
ports in about half an hour across four guests. What did not build says more than what did.

### `ruby` is a fourth instance of the `LIBRARY_PATH` defect — and it is on the critical path

`ruby-3.2.9` died with:

```
runtime_loader: Cannot open file libroot.so (needed by /boot/system/bin/make): No such file or directory
Warning: Command '['bash', '-c', '. /wrapper-script']' returned non-zero exit status 3.
```

immediately after configure, at the first `make`. The cause is in the recipe, but it is not
a recipe *defect* — `ruby-3.2.9.recipe` lines 120, 126 and 137 each do:

```sh
export LIBRARY_PATH=$LIBRARY_PATH:%A
```

which is written **assuming prepend semantics**. On the pre-fix loader, setting
`LIBRARY_PATH` at all *replaced* the search path, and since `$LIBRARY_PATH` was empty the
result held no `libroot.so` — so `make` itself could not start. The recipe is correct; the
platform was wrong. This is the same defect as perl's `LDLIBPTH` and python's `RUNSHARED`,
found for the third and fourth time, and it is the first time it has sat on the critical
path to a deliverable rather than merely cost a workaround.

`ruby` gates `vim`, and `vim` is the only affordable provider of `cmd:xxd`, which
`netsurf-3.11` build-requires. The tree's only other `cmd:xxd` provider is
`qvim-8.0.197`, which needs `devel:libQt5Core` and `devel:libQt5Gui` — far dearer than
ruby.

> ~~**So on a pre-fix host the browser chain cannot finish, and cutting a recipe is the
> wrong answer to that.**~~ **Both halves of that sentence turned out to be wrong, and it
> is corrected rather than deleted because the reasoning is instructive.** Cutting a recipe
> *was* the answer: ruby was cut out of vim once the kernel defect behind it had been
> attributed and assigned, so the cut conceals nothing. And the chain does **not** need a
> post-fix host — `vim` and `netsurf` were both built on a pre-fix guest, measured on both
> arms. The `LIBRARY_PATH` requirement was specific to ruby all along. See "The two `vim`
> cuts" and "Does the chain need the fixed loader?" below.

### The A/B, with the host loader as the single variable

The whole build fleet was still running the pre-fix loader — all five guests answered
`exit 3` and printed nothing to `LIBRARY_PATH=/tmp /bin/echo`, with the plain run as the
negative control. A sixth guest was cloned and given the fixed loader, and ruby rebuilt
there from the **pristine** recipe (verified: no `graviton` marker in it, the three
`export LIBRARY_PATH` lines untouched):

| | pre-fix guest | post-fix guest |
|---|---|---|
| `/boot/system/runtime_loader` | `2eca21fe…` | `1caa4250…` |
| `LIBRARY_PATH=<empty dir> /bin/echo` | exit 3, silent | prints |
| chroot input `haiku.hpkg` | `f684f67a…` | `f684f67a…` — **unchanged** |
| ruby recipe | pristine | pristine |
| `Cannot open file libroot.so` in the log | 1 | **0** |
| result | died at the first `make` | past configure, compiling |

**It is the host loader that decides, not the chroot's `haiku.hpkg`.** That is worth
stating plainly because the intuition runs the other way: haikuporter builds inside a
chroot whose `/boot/system` comes from an activated `haiku.hpkg`, so one expects the
chroot's copy to govern. It does not — the acceptance run left the chroot input package at
its old checksum and changed only the host's `/boot/system/runtime_loader`, and that was
sufficient both times.

### How to give an existing guest the fixed loader — and how not to

The wrong way, tried first and recorded because the failure is expensive to re-derive:
**do not drop in a whole `haiku-r1~beta6_hrev59996-1-arm64.hpkg` from a different build
tree.** The guest died with `Synchronous Exception at 0x…` immediately after
`Calling ExitBootServices`, i.e. in the boot path, before any kernel output — the EFI
`haiku_loader` on the disk belongs to the original build and the kernel it was handed did
not. Note also that the *version string is identical* (`hrev59996` either way), so nothing
warns you.

The right way is to change only the file under test, rebuilding the guest's **own** system
package in place:

```sh
cp -f $P /boot/home/haiku-system-orig.hpkg      # keep the original, outside packages/
cp -f $P /boot/home/haiku-mod.hpkg
package list /boot/home/haiku-mod.hpkg | grep -c '^runtime_loader'   # positive control: 1
package add -f -C /boot/home/rlstage /boot/home/haiku-mod.hpkg runtime_loader
cd /boot/home/rlcheck && package extract /boot/home/haiku-mod.hpkg runtime_loader
sha256sum runtime_loader                        # must be the new one, checked before swapping
sync; sync; rm -f $P; mv /boot/home/haiku-mod.hpkg $P; sync; sync
```

then `system_reset` through the QEMU monitor. Everything except `runtime_loader` stays
byte-identical to the image that is known to boot. `sync` on both sides of the rename is
load-bearing — see the note on file data never being flushed.

**Consequence for the fleet — narrower than first written.** ~~Finishing netsurf~~ and
consuming the perl/python workaround retirements require build guests whose loader is
additive. Either re-seed guests from a post-fix image, or apply the surgical swap above.
Until then those retirements are real but unusable on these guests.

**Finishing netsurf does *not* require it** — that clause was struck after measuring it on
both arms; only ruby ever needed the additive loader. So the re-seed is worth doing for the
perl/python retirements and for any future `LIBRARY_PATH`-setting port, but it never blocked
the browser. Keeping `run15` (ssh 2235) as the standing control for this class of defect is
the cheap half of that.

### And behind the loader there were two more layers — the second one is a kernel bug

Clearing the loader defect did not make ruby build. It made two further failures visible,
which is the point of clearing a blocker rather than working around it.

**Layer 2 — a real arm64 portability bug in ruby, fixed.**

```
signal.c:870:36: error: 'mcontext_t' {aka 'const struct vregs'} has no member named 'esp'; did you mean 'sp'?
signal.c:871:34: error: 'mcontext_t' {aka 'const struct vregs'} has no member named 'ebp'
make: *** [Makefile:468: signal.o] Error 1
```

ruby's stack-overflow handler reads the faulting SP and FP out of `mcontext_t`, and its
`__HAIKU__` arm handles only `__amd64__`, falling through to the **x86** names for
everything else. Haiku's arm64 `struct vregs` has `sp` and — for the AArch64 frame
pointer, x29 — `x[29]`. Adding an `__aarch64__` arm to that same `#if` compiles;
see `graviton/haikuports-patches/ruby-3.2.9-arm64-mcontext.patch`. **A fix, not a cut.**

**Layer 3 — `miniruby` panics the arm64 kernel.** ~~This is the current hard blocker.~~
**FIXED AND HARDWARE-VERIFIED 2026-08-25** — `36d365a594` plus `34093ec4bf`, proven with a
two-arm A/B on real Graviton in
[arm64-mprotect-query-present.md](arm64-mprotect-query-present.md). The `ESR` below matches
the reproducer's panic exactly. Kept in full because the diagnosis started here.

```
PANIC: area 0xffff0000dd38f8c0 looking up page failed for pa 0x0
Thread 15856 "miniruby" running on CPU 9
 4 ... <kernel_arm64> _user_set_memory_protection + 0x9e0
 5 ... <kernel_arm64> syscall_dispatcher + 0x750
 8 ... </boot/system/lib/libroot.so> _kern_set_memory_protection (nearest) + 0x04
 9 ... </sources/ruby-3.2.9/miniruby> rb_dbl_complex_new (nearest) + 0x144c
12 ... </sources/ruby-3.2.9/miniruby> ruby_setup + 0x180
13 ... </sources/ruby-3.2.9/miniruby> ruby_init + 0x10
15 ... </sources/ruby-3.2.9/miniruby> _start + 0x50
```

`ESR=0x560000d8`, `FAR=0x000000bc34576000`; the guest dropped into KDL and was recovered
with a monitor `system_reset`. So a plain `mprotect()` from a user program, during
`ruby_init`, panics the kernel — a userland process should not be able to do that whatever
it passes. Note ruby configures with **MJIT support: yes**, so JIT page protection is the
plausible caller.

This is generic arm64 kernel/VM territory (`src/system/kernel/`), **not** a packaging
problem, and it is not this track's to fix — handing it over rather than working around it,
because the obvious workaround (`--disable-jit-support`) would *hide* an OS defect that any
JIT-using application will hit, and a browser is exactly such an application. ~~**Recorded as
a single observation**: it has been seen once, with a full stack trace, and has not yet been
reduced to a minimal reproducer.~~ **Superseded:** it *was* reduced to a minimal reproducer —
`src/bin/mprotect_probe`, a few lines of unprivileged C — and that reproducer both panics an
unpatched kernel and returns cleanly on a patched one, measured on hardware.

### The two `vim` cuts, and why they are not shortcuts

> **BOTH CUTS RETIRED 2026-08-25 — the promises below were kept.** `vim-9.1.1618-1-arm64.hpkg`
> is built from the **pristine** upstream recipe again, with the ruby interpreter *and* the
> Haiku GUI, `_dirty` count 0. Cut 1 fell to the kernel `mprotect()` fix, which let
> `ruby-3.2.9` build natively (`ruby` + `ruby_devel` are in `hpkg-out/arm64/`); vim's log then
> shows `checking for ruby... /boot/system/bin/ruby` in both configure passes with
> `--enable-fail-if-missing=yes` still in force. Cut 2 fell to the **`mimeset`** fix — the
> icon failure was never about vim: `mimeset` is the sole writer of the `BEOS:ICON`
> *attribute* that `catattr` reads back, and it had been a silent no-op on every headless
> Haiku. `bin/gvim`/`gview`/`gvimdiff`/`rgvim`/`rgview` are real files and declared again.
> Everything below is retained as the record of why each cut was acceptable while it stood.

**Decision taken:** route around the kernel panic by cutting ruby out of `vim`. netsurf
wants `cmd:xxd` and nothing else from vim, and across the whole recipe tree only
`vim-9.1.1618` and `qvim-8.0.197` provide it — qvim wants `devel:libQt5Core` and
`devel:libQt5Gui`. So vim is built here as a *build tool*, to obtain one 33 KB hex dumper.
Full write-up: `graviton/haikuports-patches/vim-9.1.1618-cli-only-no-ruby.patch`.

**Cut 1 — no ruby interpreter. Cost, plainly: vim has no `:ruby` command.** Nothing else
changes; `REQUIRES` never mentioned libruby, so the package has no runtime change either.
Dropped: `devel:libruby`, `cmd:ruby`, and both `--enable-rubyinterp=dynamic` flags.
`--enable-fail-if-missing=yes` is deliberately kept — it is why a missing interpreter fails
loudly rather than being silently skipped, so the *flag* has to go rather than the
dependency merely being absent.

**Why this is acceptable, and the one clause that matters.** Cutting ruby removes an
**arm64 kernel panic in `mprotect()`** from the critical path. On its own that would be the
wrong trade — it would *mask* an OS defect that any JIT-using application will eventually
hit, and a browser is exactly such an application. **What makes the cut acceptable is that
the defect is not concealed by it: it is separately owned and ~~being fixed~~ **now FIXED and
hardware-verified**, diagnosed as `VMSAv8TranslationMap::Query()` setting `PAGE_PRESENT`
unconditionally with no valid-bit test, so an empty leaf PTE reports "present, pa=0" and the
generic VM's `vm_lookup_page(0)` panics.

> **Written down so it is not re-litigated.** If you are reading this and wondering whether
> the ruby cut was a shortcut past a hard problem: it was not, because the thing it would
> have concealed had already been found, attributed and assigned before the cut was taken.
> The `mprotect` observation is what *found* it. When that kernel fix lands, the honest
> move is to try ruby again — `ruby-3.2.9-arm64-mcontext.patch` is kept in the tree for
> exactly that, even though ruby is not currently built, because it is a real portability
> fix (ruby read the x86 `esp`/`ebp` out of an arm64 `mcontext_t`).
>
> **That condition is now met (2026-08-25).** The kernel fix is merged, baked, canonical and
> verified against its own reproducer on hardware —
> [arm64-mprotect-query-present.md](arm64-mprotect-query-present.md). **The ruby retry is
> owed.** Until it is attempted, the vim cuts stand on a promise that has come due rather
> than on one that is still pending, which is a weaker position than the paragraph above
> describes.

**Cut 2 — no GUI build. Cost, plainly: no GUI vim.** This one was *not* pre-planned; it was
forced, and it is disclosed here as a second cut rather than folded into the first. With the
GUI configured, `make install` selects `HAIKUGUI_INSTALL` and reaches `installglinks_haiku`
(`src/Makefile:3769`), whose first line dies:

```
@catattr -r "BEOS:ICON" $(DEST_BIN)/$(GVIMTARGET) > ~icon.attr
catattr: ".../bin/gvim", attribute "BEOS:ICON": No such file or directory
make[1]: *** [Makefile:3770: installglinks_haiku] Error 1
```

The GUI binary compiles and links fine; what fails is *installing its icon*. vim reads
`BEOS:ICON` back off the installed binary to stamp it onto the `g*` wrapper scripts, and in
this chroot the binary comes out of `xres` + `mimeset` with no such filesystem attribute.
`cmd:vim`, `cmd:vi`, `cmd:view`, `cmd:ex`, `cmd:vimdiff`, `cmd:vimtutor`, `cmd:rvim`,
`cmd:rview` and `cmd:xxd` all survive; `cmd:gvim`, `cmd:gview`, `cmd:gvimdiff`, `cmd:rgvim`
and `cmd:rgview` are gone — **and their `PROVIDES` entries were removed in the same edit.**
A `PROVIDES` line for a binary the build no longer produces is the same species of quiet
falsehood as the `gettext` groff cut, and that one cost a day to unpick.

*Ruled out, so nobody repeats it:* this is **not** a symlink-following problem. `gvim` is a
symlink to `vim` at that moment, which is the tempting explanation, but `catattr` follows
symlinks here — checked directly (`addattr` a string attribute to a file, read it back
through a symlink: reads fine). The attribute is genuinely absent from the binary.
*Not established:* **why** `mimeset` produces no `BEOS:ICON` here. The plausible cause is
that the chroot has no usable MIME database or registrar, so `mimeset` is a no-op — but
that was **not tested and is a hypothesis, not a finding**. It is worth chasing, because it
would affect any port whose install step depends on `mimeset` writing attributes. Dropping
the GUI pass removes the dependency on that step rather than silencing an unexplained
failure, which is why it was preferred to making the icon step non-fatal.

### New or inherited? — stated explicitly, because this column has been wrong twice

| Cut | New in | Inherited by | Does it degrade the *shipped browser*? |
|---|---|---|---|
| jasper `-DJAS_ENABLE_OPENGL=OFF` (GLUT absent) | `jasper` | `netpbm` → `groff` → everything downstream, incl. **netsurf** | **No.** It removes jasper's `jiv` viewer. groff uses jasper's library, not its viewer |
| ~~vim Cut 1 (no ruby)~~ **RETIRED 2026-08-25** | `vim` | **netsurf** (uses vim's `xxd` at build time) | **No** — and moot: ruby-3.2.9 builds now, so vim ships `+ruby` again |
| ~~vim Cut 2 (no GUI)~~ **RETIRED 2026-08-25** | `vim` | **netsurf** (same) | **No** — and moot: the cause was the `mimeset` no-op, now fixed, so the GUI builds |
| `json_c` cmake-4 policy flag | `json_c` | `hubbub` → **netsurf** | **No** — and it is a compatibility flag, not a cut: nothing removed, no declaration changed |

**`netsurf` itself adds zero cuts** — its recipe is unmodified. Everything in its column is
inherited. The distinction that matters and that this document has muddled in both
directions: all four items above are cuts to **build-time dependencies whose removed
feature is not used downstream**. None of them reduces the browser. Saying "netsurf is not
cut-free" is true about its *ancestry* and misleading about its *artifact*; say which.

### Does the chain need the fixed loader? — measured, and the earlier answer was wrong

This file previously said the fleet needed the additive-`LIBRARY_PATH` loader to finish
netsurf. **That was too strong.** The requirement was specific to `ruby`, whose recipe does
`export LIBRARY_PATH=$LIBRARY_PATH:%A`. With ruby out of the chain, it was tested rather
than assumed: `vim` was built on **both** a pre-fix guest (`runtime_loader` `2eca21fe…`,
`LIBRARY_PATH=<empty dir> /bin/echo` exits 3 silently) and the post-fix guest
(`1caa4250…`, prints), from the same recipe.

| | pre-fix loader guest | post-fix loader guest |
|---|---|---|
| `vim` | **built**, 14,513,009 B, `_dirty` 0 | **built**, RC=0, 14,512,906 B |
| `Cannot open file libroot.so` in log | 0 | 0 |
| `BEOS:ICON` failure after Cut 2 | 0 | 0 |
| `netsurf` | **built**, 4,003,742 B, `_dirty` 0 | not attempted — the pre-fix arm already answers it |

Both arms succeed, and `netsurf` itself was built on the **pre-fix** guest. (The size
differs by 103 bytes between arms, which is build-path/timestamp noise, not a content
difference.)

**Conclusion: re-seeding the build fleet with the fixed loader is OPTIONAL, not blocking.**
It remains worth doing — it is what retires the perl and python workarounds in practice, and
it is what any future `LIBRARY_PATH`-setting port will need — but nothing in the browser
chain is waiting on it. The one guest that has it (`run15`, ssh 2235) is worth keeping as
the standing control for that class of defect.

## Two build-infrastructure gaps found by walking the closure

Neither is a port problem, and both would have read as "unbuildable port".

**`ftp://` source URIs were refused outright.** `gdbm-1.26`'s only live `SOURCE_URI` is
`ftp://ftp.gnu.org/gnu/gdbm/gdbm-1.26.tar.gz`, and haikuporter's fallback location
`ports-mirror.haiku-os.org` **no longer resolves at all**. The guests have no egress of
their own: a `wget` shim hands every URL to `haiku-source-proxy` on the metal, and that
script answered `refusing scheme 'ftp' (http/https only)`. So the whole failure was one
`if`. It now rewrites `ftp://HOST/PATH` to `https://HOST/PATH` before fetching — every
mirror that publishes a tree over ftp publishes it over https too, and https is what
actually gets out. The rewrite happens before the cache key is computed, so the two
spellings share one entry, which is right: same bytes. Controls: the exact failing URL now
fetches (1,226,591 B, `sha256=6a24504a…`) and `gopher://` is still refused. `gdbm` then
built with **no recipe change**.

**`json_c-0.15` needs a cmake-4 compatibility flag.** It declares
`cmake_minimum_required` below 3.5 and cmake 4 removed that compatibility, so configure
dies at `CMakeLists.txt:3` before looking at anything else.
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` restores the pre-3.5 policy defaults — precisely what
cmake 3.x did with this project. **This is not a cut**: nothing is removed from the build,
no declared dependency changes, and the package that comes out is what json-c intends. It
is needed because `hubbub`, netsurf's HTML parser, build-requires `devel:libjson_c`, and
the tree's only alternative recipe (`json_c4-0.13.1`) is older still. Worth watching for
in other pre-2020 cmake ports.

**Correction to an earlier recommendation of mine.** I argued netsurf was attractive partly
because it skipped the groff chain. It does not: `netsurf-3.11` build-requires `cmd:git`,
and `git` requires `cmd:man`/`cmd:nano`/`devel:libpcre2_8` and sits above groff. So the
groff chain was on the critical path to **both** browsers, which is why doing it first was
right for a reason other than the one given — the work is unconditionally shared, and
browser choice could safely be deferred until after it. It now is.

**And `libpsl` is no longer a required cut for WebKit.** Retiring the `libxml2` cut gave
`libxml2_python3.14` → `itstool` (built) → `gtk_doc` → `libidn2` → `libpsl`, so that edge
can be built rather than cut. `rav1e` remains genuinely unbuildable (no Rust: nothing
provides `cmd:cargo`), but it is an AV1 **encoder** and `dav1d`, the decoder, is reachable —
so it should be an `-DAVIF_CODEC_RAV1E=OFF`-shaped cut, the same class as jasper's `jiv`.

**Nothing on the critical path is behind a real knot.** The chain is deep and serial, and
its cost is now dominated by two things that are not dependency problems at all: the
**PGO/LTO toolchain gap** above, and **LLVM's build time** if the target is WebKit.

### What actually happened — groff is built, and the last mile was all runtime deps

**`groff-1.23.0-2-arm64.hpkg`, RC=0, `_dirty` count 0, verified by rendering.** The
capability test (`graviton/builder/groffcheck.sh`) extracts the package rather than
installing it and feeds a man page through every device:

```
  -Tascii       368 bytes   marker: 1
  -Tutf8        372 bytes   marker: 1
  -Tps         7286 bytes   marker: 0   <- expected: PostScript emits glyph operators, not literal text
  -Thtml       1167 bytes   marker: 1   <- this is MAN2HTML, the one thing gettext wanted
```

All four of the commands that supposedly made groff unreachable — `pnmcrop`, `pnmtopng`,
`pnmtops`, `psselect` — are present and were exercised.

**Consequently the `gettext` stage-1 cut is now retirable** (it dropped only
`cmd:groff` for `MAN2HTML = groff -mandoc -Thtml`, and `-Thtml` demonstrably works).
That is the obvious next step and it is one rebuild.

#### The jasper OpenGL cut was needed — for the opposite reason to the one on record

This deserves its own note because "the cut was needed" would otherwise read as
confirmation of an explanation that is wrong.

The old account: jasper needs `devel:libGL` → `mesa-25.3.6` → `libLLVM` + `libvulkan` +
`cmd:glslangValidator` + `cmd:git`, hence a multi-hour wall, hence cut OpenGL.

What was measured: `libglvnd-1.7.0` and `glu-9.0.0` supply `devel:libGL` and
`devel:libglu` and **both build here in about seventy seconds combined**. jasper's cmake
then says

```
-- Found OpenGL: /boot/system/develop/lib/libGL.so
OpenGL libraries: /boot/system/develop/lib/libGL.so;/boot/system/develop/lib/libGLU.so
```

— OpenGL is fine and mesa was never in the picture. It fails one line later on
**GLUT**: `Could NOT find GLUT (missing: GLUT_glut_LIBRARY)` from
`build/cmake/modules/JasOpenGL.cmake:16`. And **there is no GLUT recipe anywhere in the
tree** — no `glut`, no `freeglut`, and nothing provides `devel:libglut`, `cmd:glut` or
`devel:libfreeglut`. That is the real blocker and, unlike mesa, it genuinely cannot be
built.

So `graviton/builder/jasperfix.sh` sets `-DJAS_ENABLE_OPENGL=OFF` and stops advertising
`cmd:jiv`. Note what it does *not* do: `devel:libGL`/`devel:libglu` stay in
`BUILD_REQUIRES`, because they now resolve and removing a satisfied dependency would be a
pointless divergence. **Capability cost, stated as a capability:** GLUT is used only by
jasper's `jiv` image *viewer*; netpbm wants `devel:libjasper`, the JPEG-2000 codec, which
is untouched — and netpbm building against it *is* that test, rather than jasper's exit
code.

#### libglvnd did not compile, and it was two lines

`libglvnd` failed at 77 of 102 objects with

```
../src/HGL/GLView.cpp:76:29: error: 'PTHREAD_RECURSIVE_MUTEX_INITIALIZER' was not declared
    in this scope; did you mean 'PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP'?
```

Haiku's `<pthread.h>` defines only the `_NP` spelling — read the header, line 81, no plain
form exists. The two uses are in `src/HGL/`, libglvnd's *Haiku* backend, so this is a
Haiku portability bug in the port and **not** an arm64 one: `entry_aarch64_tsd.c` in the
same build compiled cleanly. `graviton/builder/glvndfix.sh` rewrites both. This is a real
fix, not a cut, and it is upstream-worthy as-is.

#### The instrument was wrong in a new way: build edges are not the whole graph

`depclosure.py` originally followed only `buildRequires`/`buildPrerequires`. That is not
what haikuporter does: it has to **install** each build dependency into the chroot, so the
dependency's own runtime `requires` matter too. `meson` proved it, *after* `build` had
itself built at RC=0:

```
requires "packaging_python314" of package "build_python3.14-1.5.0-1" could not be resolved
build-requires "build_python314" of package "meson-1.11.1" could not be resolved
```

Note the ordering: haikuporter prints the **real cause first** and a misleading summary
last, so a `tail` blames `build_python314` when the missing port is `packaging`. That same
shape then repeated three more times on the way to groff, each one a runtime-only
dependency of `psutils`:

| Missing | Wanted by | Found because |
|---|---|---|
| `packaging` | `build_python3.14` | meson failed |
| `puremagic`, `pypdf` | `psutils` | surveyed the runtime requires after meson |
| `libpaper2` (`cmd:paper`) | `psutils` | groff failed — **my survey had filtered out `cmd:`-prefixed requires and hid it** |
| `typing_extensions` | `pypdf_python310` | groff failed again |

`saturate()` now requires a port's runtime `requires` to be satisfied before promoting it,
and `minimal_set()` walks `build + requires`. Two lessons, both cheap to state and
expensive to learn:

1. **A dependency that builds is not a dependency you can use.** Installability is part of
   reachability, and an estimate built from build edges alone is a lower bound.
2. **A filter in the diagnostic is a place for the answer to hide.** Excluding `cmd:` from
   a survey of runtime requires is exactly why `libpaper2` cost an extra build cycle. The
   project rule "a zero-row filter is not evidence of absence" applies to filters you wrote
   yourself, not just to empty query results.

### A limitation of this instrument, recorded rather than left to be discovered

`depclosure.py` credits a port with every provides its *recipe* declares. That makes it
over-optimistic for a port we built with a subpackage-dropping cut — and there is exactly
one such case, which matters:

**our `libxml2` was built with the stage-1 cut that drops `libxml2_python3.14`**, so the tool
reports `libxml2_python3.14` as satisfied when the hpkg set does not contain it. `itstool`
requires it, and `itstool` gates `gtk_doc → libidn2 → libpsl`. So retiring that libxml2 cut
(item 2 of "What remains" — "delete one line once `cmd:python3.14` exists") is **on the
critical path to a browser without the libpsl cut**, not the cosmetic cleanup it is filed as.
`cmd:python3.14` is one build away, so this is cheap to retire now.

Two smaller instrument notes, both fixed in the committed version and both worth knowing
because each produced a *confident wrong* answer first:

- Promoting a port must credit its **subpackages'** provides, not just its base package's.
  Crediting only the base made `flit_core_python310` and `jasper_devel` look unprovided and
  reported reachable ports as stuck. Symptom: reachable-port count 1329, saturation stopping
  after 4 rounds. After the fix: **2495 reachable, 16 rounds.**
- Eight of the 3667 markers (`pygments`, `pytest`, `sip`, `pluggy`, `iniconfig`, `mozfile`,
  `trove_classifiers`, `noto_serif_cjk`) are `ARCHITECTURES="any"` pure-Python ports with
  **no base `DependencyInfo`** — their build requirements live in the per-flavour subpackage
  files. Skipping them made `pygments_python310` read as *"nothing in the tree provides
  this"*, which is what turned `gtk_doc → libidn2 → libpsl` into a false hard stop and
  briefly made a browser look unreachable outright.

The first two versions of this analysis both said "UNREACHABLE" for things that are
reachable. The disagreement between the backward walk and the saturation is what exposed
the bug — neither alone would have. **Two methods that disagree are worth more than one
method that answers confidently.**

## Where every port stands

Built = a verified `.hpkg` on the builder **and** in
`s3://haiku-graviton-668984504585-us-west-2/hpkg/arm64/`, confirmed by `ls`/`s3 ls` and
never inferred from an exit code. All were built against the non-dirty `haiku`
(Blocker 8), so every one is `pkgman`-installable.

**The cmake pass of 2026-08-24 added 14 ports** on top of the 23 from Blocker 8:
`expat`, `rhash`, `libuv`, `nghttp2`, `ca_root_certificates`, `libssh2`, `curl`,
`cmake`, `gawk`, `gperf`, `texinfo`, `bison`, `flex`, `doxygen`. **Only one of the
fourteen needed a recipe change at all** — `curl`, for the `libpsl` cut. Every other
one built from its pristine recipe, which is the real measure of how wrong
"cmake is a dead end behind a cycle" was.

Two stage-1 cuts were **retired** in the same pass, both by rebuilding from the
pristine recipe rather than by writing a new patch:

| Cut | Retired how | Verified by |
|---|---|---|
| `autoconf-2.72` doc cut | real `texinfo-7.2` now provides a working `makeinfo`; rebuilt from the pristine recipe, `make install-html` restored | `autoconf.html` (2,274,731 B) and `standards.html` (412,748 B) now present; the stage-1 hpkg has **no** `.html` entry at all |
| `gettext-1.0` groff doc cut | **RETIRED 2026-08-24.** `groff-1.23.0` is built, so `cmd:groff` went back into `BUILD_PREREQUIRES` and gettext was rebuilt from that recipe | `RC=0`, five hpkgs, `_dirty` 0 on each, and the content inventory is **identical** to the cut build — 96 html entries in `gettext_doc`, the same 31 `name.N.html` man pages, same entry counts in all five subpackages. haikuporter listed `groff-1.23.0-2-arm64.hpkg` among the chroot's active packages, so the restored prerequisite genuinely resolves rather than being quietly ignored |
| `zstd-1.5.6` Makefile-instead-of-cmake cut | `cmd:cmake` now exists; rebuilt from the pristine cmake-based recipe | the cmake package-config files (`lib/cmake/zstd/zstdTargets*.cmake`) that the Makefile build cannot produce |

> **Consistency:** every hpkg in this set now requires
> `haiku >= r1~beta6_hrev59996-1` (non-dirty) and resolves — and `pkgman install`s —
> inside any repaired guest. All five guests carry the repaired chroot as of
> 2026-08-24 01:19 UTC, so there is no longer a split to keep track of. The pre-fix
> set is retained for evidence only, in `/opt/haiku/hpkg-out/arm64-dirty-20260824/`.

| Port | State | Note |
|---|---|---|
| perl 5.42.2 | built | needs `perl-5.42.2-library-path.patch` |
| diffutils 3.12 | built | |
| automake 1.18.1 | built | |
| autoconf 2.72 | built (stage 1) | `autoconf-2.72-doc-cut-stage1.patch`, no html docs |
| help2man 1.49.3 | built | |
| m4 1.4.19 | built | |
| bzip2 1.0.8 (+devel) | built | |
| zlib 1.3.2 (+devel) | built | |
| **libtool 2.5.4 (+libltdl)** | **built** | `libtool-2.5.4-no-bootstrap.patch` — Blocker 1 |
| **tar 1.35 (+debuginfo)** | **built** | `tar-1.35-included-regex.patch` — Blocker 4 |
| **libiconv 1.18 (+devel +debuginfo)** | **built** | was gated on tar via libtool's `REQUIRES cmd:tar` |
| **lz4 1.9.4 (+devel)** | **built** | |
| **readline 8.3.003 (+devel +debuginfo)** | **built** | |
| **pkgconf 1.5.3 (+devel +debuginfo)** | **built** | first port fetched *and* unpacked through the proxy |
| **zip 3.0** | **built** | second proof of the unpack fix, `.tar.gz` |
| **patch 2.7.6** | **built** | needed by `haikuporter -G`, see below |
| **sqlite 3.50.4.0 (+devel +debuginfo)** | **built** | unblocked by readline |
| **gzip 1.14** | **built** | no recipe change, 43 s |
| **gettext 1.0 (+devel +doc +libintl +libintl_devel)** | **built (stage 1)** | `gettext-1.0-groff-doc-cut-stage1.patch` — **Blocker 5**, the one line that opened the chain |
| **xz_utils 5.8.3 (+devel +debuginfo)** | **built** | no recipe change; `cmd:autopoint` came free with gettext |
| **which 2.21 (+debuginfo)** | **built** | no recipe change; nothing in the image provided `cmd:which`, which cmake needs |
| **zstd 1.5.6 (+bin +devel)** | **built (stage 1)** | `zstd-1.5.6-makefile-not-cmake-stage1.patch` — built with zstd's own Makefile, sidestepping `cmd:cmake` entirely |
| **cmake 4.1.6 (+debuginfo)** | **built** | **pristine recipe, no cut** — all five `--system-*` libs. See Blocker 6 |
| **curl 8.21.0 (+devel +debuginfo)** | **built** | `curl-8.21.0.recipe` — the `libpsl` cut, the only recipe change in the whole pass |
| **expat 2.8.2 (+devel +debuginfo)** | **built** | pristine; an unbuilt leaf, never a cycle member |
| **rhash 1.4.6 (+devel)** | **built** | pristine; unbuilt leaf |
| **libuv 1.52.1 (+devel +debuginfo)** | **built** | pristine; **autotools, does not need cmake** |
| **nghttp2 1.63.0 (+devel +debuginfo)** | **built** | pristine; `BUILD_REQUIRES` is `haiku_devel` only |
| **libssh2 1.11.1 (+devel)** | **built** | pristine; blocked only by a missing `ca_root_certificates` |
| **ca_root_certificates 2026_07_16** | **built** | pure data package, zero build deps; its absence made `openssl3` unresolvable |
| **texinfo 7.2** | **built** | pristine; a **functional** `makeinfo`, unlike the bootstrap stub. Retires the autoconf doc cut |
| **bison 3.8.2 (+debuginfo)** | **built** | pristine; the bootstrap bison's `--version` fails, which broke doxygen's `FindBISON` |
| **flex 2.6.4** | **built** | pristine; doxygen needs >= 2.5.37, the bootstrap is 2.5.35 |
| **doxygen 1.14.0** | **built** | pristine; needed real `bison` + `flex`, then built straight through |
| **gawk 5.3.0 (+debuginfo)** | **built** | pristine; upgrade over the bootstrap 3.1.8 (was **not** a blocker — see Blocker 6, habit 2) |
| **gperf 3.1** | **built** | pristine; likewise not a blocker |
| **openssl3 3.5.7 (+devel +man +debuginfo)** | **built** | **no recipe change at all** — it simply needed `devel:libzstd`. The whole four-port cascade turned on one line in gettext plus zstd's build system |
| **autoconf_archive 2024.10.16** | **built** | pristine; wave-1 leaf, 18 s. Needed by both Pythons |
| **libedit 20230828_3.1 (+devel +debuginfo)** | **built** | pristine; wave-1 leaf, 44 s |
| **libffi 3.4.6 (+devel)** | **built** | pristine; wave-1 leaf, 32 s |
| **libpng16 1.6.53 (+devel)** | **built** | pristine; wave-1 leaf, 55 s. On the netpbm side |
| **nasm 2.16.03 (+debuginfo)** | **built** | pristine; wave-1 leaf, 46 s. Needed by libjpeg_turbo |
| **python3.10 3.10.20** | **built (stage 1)** | `pyfix.sh` — PGO cut + additive `RUNSHARED`. **Carries `zlib`/`_bz2`/`_lzma`/`_ssl`/`_sqlite3`/`readline`**, verified inside the hpkg. Not yet activatable — see Blocker 9 |
| **python3.14 3.14.7** | **built (stage 1)** | `pyfix.sh` — PGO/**LTO** cut + additive `RUNSHARED`. Provides the unversioned `cmd:python3` that `meson`/`ninja` need |
| **file 5.43 (+devel +debuginfo)** | **built** | pristine; wave-1 leaf. Provides `cmd:file`, which python3.10 needs to activate |
| **groff 1.23.0** | **built — and verified by rendering** | `-Tascii`/`-Tutf8`/`-Thtml` all produce real output containing a marker; `-Thtml` is the `MAN2HTML` gettext wanted. **Retires the gettext cut.** See Blocker 9 |
| **libglvnd 1.7.0 (+devel)** | **built** | `graviton/builder/glvndfix.sh` — a two-line Haiku portability fix (`PTHREAD_RECURSIVE_MUTEX_INITIALIZER` → `..._NP`). **Provides `devel:libgl`; no mesa, no LLVM, no git** |
| **glu 9.0.0 (+devel +debuginfo)** | **built** | pristine; provides `devel:libglu` |
| **jasper 2.0.33 (+devel +doc +tools)** | **built (cut)** | `jasperfix.sh` — `-DJAS_ENABLE_OPENGL=OFF` and no `cmd:jiv`, because **GLUT has no recipe in the tree**. Codec unaffected; netpbm linking it is the proof |
| **netpbm 10.86.42 (+devel)** | **built** | pristine; provides `cmd:pnmcrop`/`pnmtopng`/`pnmtops` |
| **psutils 3.3.11** | **built** | pristine, 17 s. Provides `cmd:psselect`. Needed `puremagic`, `pypdf`, `typing_extensions`, `libpaper2` at *runtime* to be installable |
| **meson 1.11.1** | **built** | pristine; needed `packaging` via `build`'s runtime requires |
| **ninja 1.13.2 (+zsh)** | **built** | pristine; needed only `cmd:python3` |
| **flit_core 3.12.0**, **installer 1.0.1**, **setuptools 82.0.1**, **wheel 0.47.0**, **tomli 2.0.1**, **pyproject_hooks 1.2.0**, **build 1.5.0**, **packaging 26.2** | **built** | the PEP-517 ladder, all pristine, ~4 minutes total. `flit_core` first — it needs only the interpreters |
| **puremagic 1.27**, **pypdf 4.3.1**, **typing_extensions 4.12.2** | **built** | pristine; runtime deps of `psutils` |
| **libpaper2 2.2.6 (+devel +debuginfo)** | **built** | pristine; `cmd:paper`, a runtime dep of `psutils` |
| **itstool 2.0.7** | **built** | pristine; **functional proof the libxml2 recut worked** — it requires `libxml2_python3.14` |
| **libjpeg_turbo 3.1.4.1 (+devel +tools +debuginfo)** | **built** | pristine; needed `nasm` |
| **tiff 4.7.0 (+devel +tools +debuginfo)** | **built** | pristine |
| **libxml2 2.15.3 (+devel +doc +python3.14)** | **rebuilt — cut RETIRED** | `xmlfix.sh`; the `libxml2_python3.14` subpackage is back. This was **critical path**, not cosmetic |
| libxml2 2.15.3 | see below | `cmd:doxygen` was the only real edge and it now exists; `lib:libicudata` was already provided by the icu bootstrap and `cmd:python3.14` is gated off by `pythonModuleEnabled` |

### Why groff *used to be* out of reach — historical, and its verdict has since been overturned

> **FULLY SUPERSEDED (2026-08-24). Both its mechanism and its conclusion are now wrong.**
> This section is kept only as the record of how the estimate was arrived at. `groff-1.23.0`
> is **built and verified by rendering**, and the `gettext` cut it argued for is
> **RETIRED** — see the cut ledger above and Blocker 9. An earlier revision of this same
> note claimed "the conclusion survives — groff is not cheap and the gettext cut stands",
> which was true when written and false a few hours later; that sentence is exactly the
> failure mode this document keeps warning about, so it is replaced rather than annotated.
> **If you are reading this section for a current answer, you are in the wrong section.**
>
> The other measured corrections, still accurate:
> **`devel:libgl` comes from `libglvnd-1.7.0`, not from `mesa`**, so there is no
> mesa/LLVM/vulkan/`cmd:git` wall — but **the `-DJAS_ENABLE_OPENGL=OFF` cut below is still
> needed**, because jasper then fails on GLUT, which has no recipe in the tree at all. The
> cut this section proposed is right; its stated reason is not. And the `psutils`
> Python packaging is a **seven-port ladder, not a knot**, because `flit_core` needs only
> the interpreters. The depth was also under-counted by roughly five times: groff is
> **24 ports and 12 waves**, not "about five more ports". Read Blocker 9 instead.

`groff` needs five commands. `cmd:makeinfo` is now real, and that leaves four:
`cmd:pnmcrop`, `cmd:pnmtopng`, `cmd:pnmtops` (all `netpbm`) and `cmd:psselect`
(`psutils`). Both providers are expensive for reasons that have nothing to do with
cmake:

**`netpbm` → `jasper` → OpenGL.** netpbm needs `devel:libjasper`, and
`jasper-2.0.33.recipe:62-63` requires `devel:libGL` + `devel:libglu` with
`-DJAS_ENABLE_OPENGL=ON` hardcoded at `:86`. `devel:libGL` means **`mesa-25.3.6`**,
whose own `BUILD_REQUIRES`/`BUILD_PREREQUIRES` include `devel:libLLVM`,
`devel:libvulkan`, `libglvnd_devel`, `cmd:meson`, `cmd:ninja`,
`cmd:glslangValidator` and **`cmd:git`** — and git is documented in this file as
unbuildable here. LLVM alone is a multi-hour build.

There *is* a plausible shortcut: jasper's OpenGL is only for its `jiv` viewer, so
flipping `-DJAS_ENABLE_OPENGL=OFF`, dropping the two `devel:libGL*` lines and the
`jiv` subpackage would break that edge. netpbm's other four deps are all cheap or
now-available — `devel:libjpeg` from `jpeg-9c` (pure autotools leaf),
`devel:libpng16` (leaf), `devel:libtiff` (leaf once libjpeg exists), `devel:libxml2`
(now built). So netpbm is roughly four leaves plus one OpenGL cut.

**`psutils` is the harder half, and it is Python packaging, not C.**
`psutils-3.3.11` `BUILD_REQUIRES` is `installer_python310` and it `REQUIRES`
`puremagic_python310` + `pypdf_python310`. Those are PEP-517 Python packages — the
exact knot that `--do-bootstrap` was abandoned over (see `sequencing.md` Phase 2).
`cmd:python3.10` itself is present, so this is a packaging problem, not an
interpreter problem.

**Net:** a real `groff` is about five more ports, one new OpenGL cut, and a first
answer to PEP-517 packaging. It is tractable but it is not a follow-on to cmake, and
it should be scoped as its own piece of work.

**~~Consequently the `gettext` stage-1 cut is NOT retired.~~ — OVERTAKEN 2026-08-24: it
is retired.** What this paragraph got right is the *size* of the debt: gettext used groff
for exactly one thing, `MAN2HTML = groff -mandoc -Thtml`, and the HTML man pages **ship
prebuilt in the tarball**, so the cut was a *dependency-declaration* cut whose content
cost was plausibly zero. It asked for that to be confirmed by comparison rather than
assumed in either direction, and it now has been: rebuilding with `cmd:groff` restored
produces the **same 31 `name.N.html` man pages and the same 96 html entries** as the cut
build, in five hpkgs with `_dirty` 0. Content cost of the cut: zero, measured.

### Source packages are never checksum-verified, and that is a standing gap

`grep -ci 'validating checksum'` is **0** across every build log in this pass. It is
not a regression and not contamination — it is structural. The 116
`*_source_rigged-*.hpkg` input source packages hold an **uncompressed source tree**
rather than the upstream tarball, so the recipe's `CHECKSUM_SHA256` has nothing to
hash and haikuporter skips the check entirely. Only ports fetched through the
source proxy (Blocker 2) get a real checksum, and those *do* pass one.

Provenance was spot-checked rather than assumed: for `libuv`, 469 of 469 files
matched upstream, with exactly 3 differing, and those 3 are precisely the files the
port's own patchset touches. So the trees are good — but **the gate is absent, not
passing**, and anything rebuilt from these inputs inherits that. Either restore
verification (hash the extracted tree against a recorded manifest) or keep this
paragraph, but do not let a silent checksum bypass stay undocumented.

### The remaining chain is not a plain ladder

> **Superseded — kept for the reasoning, not the conclusion.** This section describes the
> picture *before* Blocker 5. `gettext`, `xz_utils`, `zstd` and `openssl3` are all built;
> `groff` never had to be. The part still worth reading is *why* it looked like one cycle
> when it was two. For the actual dependency order used in the rebuild, see Blocker 8.

`sqlite` fell out as soon as `readline` existed, but the rest converges on one point:

```
zstd, openssl3  ->  xz_utils  ->  gettext  ->  groff  ->  pnmcrop (netpbm)
libxml2         ->  python3.14
```

and `groff -> pnmcrop/netpbm` is the entrance to the **same 10-deep cycle already
documented for `texinfo`** in `graviton/haikuports-patches/README.md`:

```
texinfo -> libintl -> groff -> pnmcrop/netpbm -> libjasper -> cmake -> libcurl
        -> libpsl -> libidn2 -> gtkdocize
```

So `openssl3`, `libxml2`, `gettext` and `zstd` are **not** one build away — four of the
five original "leaf" ports sit behind that cycle. `groff` is only a *build-prerequisite*
of `gettext` (man pages), so the tractable move is the same stage-1 expedient used for
autoconf: cut the documentation step rather than try to build the cycle. That produces
packages that must be rebuilt later and belong in `hpkg-out/arm64/stage1/`.

## Two more traps worth propagating

- **Every non-input-source-package port must be built as `haikuporter -G`.** Without it
  `Source.patch()` dies on `Error: 'git' is not available`, and git is not buildable here.
  `-G` uses `patch(1)` instead, but demands it unconditionally — even for ports that have
  no patches at all — which is why `patch-2.7.6` had to be built first.
- ~~**Nothing built in these guests can be `pkgman install`ed.**~~ **No longer true**
  since the 2026-08-24 rebuild (Blocker 8). It was true while every hpkg recorded
  `REQUIRES haiku >= r1~beta6_hrev59996_dirty-1` against an image reporting a non-dirty
  `hrev59996`. `package extract <hpkg> bin/<tool>` is still the right move for a *pre-fix*
  hpkg out of the snapshot, and for putting a tool on PATH without activating a package.

## What remains

1. ~~Rebuild the whole set bottom-up against the fixed, non-dirty `haiku_devel`.~~
   **DONE 2026-08-24** — 23 ports, 52 hpkgs, verified non-dirty and `pkgman`-installable.
   See Blocker 8. The scripts that did it (`graviton/builder/prepguest.sh`,
   `rebuild.sh`) are reusable for the next time a chroot input changes version.
2. ~~`libxml2`~~ **DONE 2026-08-24.** `cmd:doxygen` was the only real edge and it now
   exists; `lib:libicudata` was already provided by the icu bootstrap. The
   `cmd:python3.14` half turned out **not** to be free — `pythonModuleEnabled` really
   does evaluate true on arm64 — so it is carried as a narrow stage-1 cut
   (`recipes/libxml2-2.15.3.recipe`) that drops only the `libxml2_python3.14`
   subpackage. **Retire by deleting one line once `cmd:python3.14` exists.**
3. ~~Build a real `cmake`~~ **DONE 2026-08-24 — see Blocker 6.** Built from the
   **pristine** recipe with all five `--system-*` libraries and no cut, after building
   `expat`, `rhash`, `libuv`, `nghttp2`, `ca_root_certificates`, `libssh2` and
   `curl-8.21.0` as the leaves they always were. `zstd` and `autoconf` were rebuilt
   from pristine recipes, **retiring both of those stage-1 cuts.** `doxygen` and
   `libxml2` followed.

   ~~Still open from this item: a real `groff`, and therefore the `gettext` cut.~~
   **CLOSED 2026-08-24.** `groff-1.23.0` is built and verified by rendering, and the
   `gettext` cut is retired — rebuilt with `cmd:groff` restored, five hpkgs, `_dirty` 0,
   content identical to the cut build. The estimate quoted here was wrong in both
   directions: it was 24 ports and 12 waves rather than "~5 more ports", and the jasper
   `-DJAS_ENABLE_OPENGL=OFF` cut was needed for **GLUT's total absence from the tree**,
   not because of mesa + LLVM + `cmd:git`.
3b. **The bundled-curl cmake shipped without TLS and every build-level test passed.**
   If any future cut removes a library, test the *capability* that library provided.
   See Blocker 6. Related standing gap: **input source packages are never
   checksum-verified** (`grep -ci 'validating checksum'` is 0 in every log), because the
   rigged hpkgs hold uncompressed trees and `CHECKSUM_SHA256` has nothing to hash.
4. ~~Build `python3.10` with zlib/`_bz2`/`_lzma` so the unpack fix can be retired.~~
   **BUILT 2026-08-24** — `python3.10-3.10.20-3`, and `python3.14-3.14.7-1` alongside it.
   Both needed `graviton/builder/pyfix.sh`: a PGO/LTO cut (the bootstrap gcc has **no LTO
   support at all**) and an additive `RUNSHARED` (`LIBRARY_PATH` replaces the loader path).
   The three modules are present in the hpkg. **Still to do before the unpack fix can
   actually be retired:** activate it — that needs `lib:libbz2` activated from the built
   `bzip2`, and a `file_data` subpackage the `file` build did not emit — then confirm
   `python3.10 -V` reports **3.10.20** (the bootstrap is 3.10.21) and re-run
   `graviton/builder/pycheck.sh`. Keep `haiku-haikuporter-patch --check` until that passes.
5. `.zip` sources (312 recipes) are still unsupported: `zipfile.is_zipfile()` succeeds
   without zlib and extraction only *then* raises `Compression requires the (missing)
   zlib module`, and that path has no external-tool dispatch to hook into.
6. Give the source proxy an init unit so it survives a metal reboot; today it must be
   restarted by hand with `haiku-source-proxy start`.
7. ~~Rebuild the stage-1 `autoconf` package once a real `texinfo` is buildable.~~
   **DONE 2026-08-24.** `texinfo-7.2` built with no recipe change at all — every one of
   its dependencies (`devel:libiconv`, `devel:libintl`, `devel:libncurses`, `cmd:gawk`,
   `cmd:gperf`, `cmd:gettext`) was already satisfied, and it had looked unreachable only
   because `cmd:makeinfo` *resolved* the whole time from the non-functional
   `texinfo_bootstrap` stub. `makeinfo` was verified by converting a `.texi` to real
   `.info` **and** `.html`, not by `--version`. The bootstrap stub is now quarantined out
   of `packages/` on the guests that build doc-producing ports, because both packages
   provide `cmd:makeinfo` and the solver's choice is otherwise arbitrary.
8. **Two real arm64 kernel defects were surfaced here and are not fixed:**
   - `re_compile_pattern` hangs and the resulting process is **unkillable**, which then
     wedges `unmount -f` and `mount -t bindfs` guest-wide. Blocker 4 routes around the
     hang; the underlying libroot/regex defect and the unkillable-process behaviour both
     remain.
   - ~~The chroot clock still steps backwards.~~ **Not observed any more.** Zero
     clock-skew and zero "modification time … in the future" warnings across all 25
     full build logs of the 2026-08-24 rebuild, where before the fix they appeared in
     essentially every make-based port (`66159 s` during libtool, `64825 s` during
     sqlite, `82393 s` during cmake). The `touch -r` discipline in the libtool recipe
     stays regardless: it costs nothing and it is what makes that recipe independent of
     the clock at all.
9. **Guest state changed by the cmake pass (2026-08-24 ~04:35Z).** Recorded because
   two of these will surprise the next person:

   - **Bootstrap packages quarantined** to `/boot/home/quarantine/` (moved, not
     deleted) so the *real* build tools win dependency resolution, since both
     versions provide the same `cmd:`:
     `texinfo-7.2_bootstrap` on **2222/2227/2230**, and
     `flex-2.5.35_bootstrap` + `bison-3.8.2_bootstrap` on **2230**.
     Without this, `autoconf` could silently get the stub `makeinfo` again and
     doxygen fails on `flex >= 2.5.37`. Move them back if a port genuinely needs the
     bootstrap version.
   - **Packages installed into boot environments for acceptance testing** (activation
     state backed up automatically, so each is one `pkgman` rollback away):
     **2222** libiconv/perl/gettext_libintl/texinfo; **2227** the full cmake closure
     incl. curl/openssl3/zstd; **2229** cmake/libuv/rhash/expat/zlib; **2230**
     cmake/expat/rhash/libuv/zlib. These guests are no longer pristine build hosts.
   - **2231** received the whole shared pool and was used only for the provides index
     and the seed-`haiku.hpkg` check; it built nothing.

10. **Guest inventory as of 2026-08-24 01:20Z.** The `_dirty`/non-dirty split that used to
   decide whether your dependencies resolved is **gone** — all five guests now carry the
   repaired chroot `haiku` (`r1~beta6_hrev59996-1`) and the rebuilt package set:

   | Guest | State |
   |---|---|
   | **2222** | **recreated** (`mkguest.sh 2`) after the pre-fix guest was killed. Built `which patch gzip zlib lz4 zip pkgconf bzip2 readline sqlite m4`. |
   | **2227** | **recreated** (`mkguest.sh 7`) after being found down. Used for the cmake C++11 experiment. |
   | **2229** | **swapped** to the repaired chroot packages once the rebuild was complete, then re-prepped. Was the last `_dirty`-consistent guest. |
   | **2230** | the spine guest: `perl help2man m4 diffutils autoconf automake tar libtool libiconv gettext xz_utils zstd openssl3`. |
   | **2231** | **recycled** (`mkguest.sh 11`) after wedging on the Blocker 4 `./conftest`; now the verification guest. |

   Two operational notes for whoever picks this up:

   - The recipes tarball on the metal (`haikuports-recipes.tar.gz`, 22 Aug) **predates
     `dev-util/pkgconf`**, so a guest made by `mkguest.sh` has no pkgconf recipe and
     `haikuporter pkgconf` dies with `Error: pkgconf not found in repository` — which
     looks like a broken port rather than a missing recipe. It was copied across from
     2230 by hand; refresh the tarball to fix this properly.
   - A fresh guest has **no extracted `input-source-packages/develop/sources/<port>-<ver>/`
     directories** — haikuporter creates each one on first use. So the recipe overlay
     cannot be pre-installed on a brand-new guest, and every port needing a recipe edit
     must be built on a guest that has already seen it once. During this rebuild all six
     edited ports were therefore put on 2230.

   The harvest loop that kept re-importing the stale `haiku.hpkg` is closed in all
   driver scripts — `gworker.sh`, `cwork.sh`, `worker.sh`, `worker-full.sh` and the new
   `rebuild.sh` all stage-and-drop `haiku*.hpkg`.
11. ~~`/opt/haiku/logs/builder-boot.pcap` is 5.6 GB and still growing.~~ **Fixed.** It was
    not `tcpdump`: the 2222 guest's qemu command line carried
    `-object filter-dump,id=f0,netdev=n0,file=/opt/haiku/logs/builder-boot.pcap`. Killing
    that guest stopped the growth and the file was deleted (5.96 GB reclaimed).
