# DeBeOS revision / version identity

Status: the user-visible and source-build parts are **implemented and landed** on
`graviton` (issue #201). The package-version line is **deliberately deferred** to the
distribution/version-model decision (issue #92). This document is the design of record:
it maps every place the identity comes from, states the DeBeOS scheme, records what has
already changed, and — most importantly — spells out what has **not** changed yet and
why, so the deferred work is picked up safely.

## Background

DeBeOS descends from Haiku but does **not** track or merge upstream (see `AGENTS.md`).
Yet every build historically stamped itself `hrev59996` — the Haiku base revision
frozen at the point of the split. That string described neither this tree nor which
DeBeOS commits a build contains. Two root causes:

1. `build/scripts/determine_haiku_revision` derives the revision with
   `git describe --tags --match=hrev*`. The DeBeOS branch carries **no `hrev*` tags**,
   so `git describe` found nothing and the script errored out — which forced the bake
   to hardcode `-sHAIKU_REVISION=hrev59996` just to make the build proceed.
2. That one string then feeds two coupled surfaces: the **user-visible version banner**
   and the **`r1~beta6_<revision>` package version** — both read from the same
   `_haiku_revision` ELF section.

## Identity map — where the string comes from

The revision travels through the build as follows. The value produced by
`determine_haiku_revision` is written to `<build-output>/haiku-revision`, embedded into
the `_haiku_revision` ELF section of `libroot.so` (and the kernel), and read back at
runtime through `get_haiku_revision()` / `__get_haiku_revision()`.

### Build system — where the revision is produced and embedded

| File:line | What it sets |
|---|---|
| `build/scripts/determine_haiku_revision:32` | Primary path: `git describe --dirty --tags --match=hrev* --abbrev=1` — resolves an `hrev` on a Haiku-tagged clone. |
| `build/scripts/determine_haiku_revision:33-58` | **DeBeOS fallback** (landed): when no `hrev*` tag exists, emit `debeos-r<commit-count>-g<short-sha>[-dirty]` from HEAD instead of erroring. Only a truly non-git tree still errors. |
| `build/jam/FileRules:365-397` (`CopySetHaikuRevision` / `SetHaikuRevision`) | Locates `haiku-revision`; if `HAIKU_REVISION` is set uses it verbatim (`DetermineHaikuRevision2`), else runs the script (`DetermineHaikuRevision1`). |
| `build/jam/FileRules:400-409` | The two actions: script vs `echo $(HAIKU_REVISION)`. |
| `build/jam/BuildSetup:15` | `HAIKU_VERSION = r1~beta6` — the marketing/version prefix. |
| `build/jam/PackageRules:137-145` (`PreprocessPackageOrRepositoryInfo1`) | Assembles the package version: `revision=$(cat haiku-revision | sed 's/[+-]/_/g')`, then `version=$(HAIKU_VERSION)_${revision}` → **`r1~beta6_hrev59996`**. The `-/+` → `_` rewrite is why `debeos-r..-g..` is well-formed here. |

### Source — where it is stored and read at runtime

| File:line | What it does |
|---|---|
| `src/system/libroot/os/system_revision.c:12-24` | Declares `sHaikuRevision[128]` in the `_haiku_revision` section; exposes it as `get_haiku_revision()` (kernel/boot) / `__get_haiku_revision()` (userland). |
| `headers/private/libroot/system_revision.h` | `SYSTEM_REVISION_LENGTH 128` and the accessor prototypes. |
| `headers/private/kernel/ksystem_info.h:22` | Kernel-side `get_haiku_revision()` prototype. |
| `headers/config/HaikuVersion.h:43-45` | `B_HAIKU_VERSION` API level (`..._1_PRE_BETA_7`). **API-level, not a revision** — ports test against it; deliberately unchanged (see Branding.h). |

### Human-readable surfaces (banners)

| File:line | Surface | State |
|---|---|---|
| `src/apps/aboutsystem/AboutSystem.cpp` (`_GetOSVersion`) | About System GUI version field | **DeBeOS-branded** (landed): reads `"DeBeOS %s"` with the revision, instead of `"Version: %s"`. |
| `src/system/boot/loader/main.cpp:62-63` | Boot loader welcome (serial/syslog) | **DeBeOS-branded** (landed): `"Welcome to the " OS_DISPLAY_NAME " boot loader! (" OS_ATTRIBUTION_STRING ")"`. |
| `src/system/boot/loader/main.cpp:66` | Boot loader revision line | **Intentionally verbatim**: `"Haiku revision: %s"` kept exactly — tooling greps for it. Identity is on the adjacent welcome line. |
| `src/system/boot/platform/generic/text_menu.cpp:229,236` | Boot menu header + revision footer | **DeBeOS-branded** (landed): `OS_DISPLAY_NAME " Boot Loader"` + live `get_haiku_revision()`. |
| `src/system/kernel/debug/debug.cpp:838-839, 1444-1445` | KDL welcome + syslog header | KDL prints `"revision: %s"` (no OS name); syslog prints `"Haiku revision: %s"` — kept verbatim for the same grep reason as the boot loader. |
| `src/system/kernel/system_info.cpp:54` | `dump_system_info` KDL command | `"revision: %s"` from `get_haiku_revision()`. |
| `src/add-ons/kernel/drivers/network/ether/ena/ena.cpp:2671-2672` | ENA attach banner (serial) | **Fixed** (landed): prints the live `get_haiku_revision()`, was previously a hardcoded `hrev59996` constant. |

### Machine-readable identifiers — deliberately still "Haiku"

Per `headers/private/shared/Branding.h`, every *machine-readable* identifier stays
"Haiku" on purpose and must keep doing so:

| File:line | Identifier | Why it stays |
|---|---|---|
| `src/system/libroot/posix/sys/uname.c:39` | `uname()` `sysname = "Haiku"` | `config.guess`, CMake and autotools detect the OS from it; changing it breaks every port's `configure`. |
| `headers/config/HaikuVersion.h` | `B_HAIKU_VERSION*` | API level; HaikuPorts recipes test against it. |
| `x-vnd.Haiku-*` app signatures | MIME types in launch declarations, settings, packages. |
| `haiku*.hpkg` package names | Structural to the build and to `packagefs`. |
| `src/add-ons/kernel/drivers/network/ether/ena/ena.h:46` (`ENA_HAIKU_REVISION 59996`) | ENA `host_info.kernel_ver` / `kernel_ver_str` reported to the Nitro card (`ena.cpp:521-522`) | A machine field the device reads; not user-facing. Left as an inherited constant. Candidate for a future cleanup, not part of the user identity. |

### AMI tags (bake)

| File:line | Tag | Value |
|---|---|---|
| `graviton/pipeline/scripts/import-and-register.sh:156` | `haiku-revision` | **Pinned** to `HAIKU_REVISION` — the package-version line; `haiku-devel-publish` resolves the devel hpkg by this exact tag. |
| `.../import-and-register.sh:157` (lines 122-143) | `debeos-revision` | **Additive, HEAD-tracking**: `debeos-r<count>-g<sha>` (degrades to `debeos-g<sha>` on a shallow clone). Mirrors `determine_haiku_revision`. |
| `.../import-and-register.sh:158` | `source-commit` | Full HEAD sha. |
| `graviton/pipeline/lib/config.ts` (`haikuRevision`) | Pipeline input | Documents *why* `HAIKU_REVISION` is pinned rather than derived. |

## The DeBeOS scheme

`debeos-r<commit-count>-g<short-sha>[-dirty]`

- **DeBeOS's own, not `hrevNNNNN`** — the `debeos-` prefix makes provenance unambiguous.
- **Encodes git provenance** — `-g<short-sha>` pins the exact commit for debugging;
  `-dirty` flags an uncommitted tree.
- **Monotonic ordinal** — `r<commit-count>` (`git rev-list --count HEAD`) increases with
  each commit, giving a sortable version component.
- **Well-formed for the package-version line** — `PreprocessPackageOrRepositoryInfo1`
  rewrites `-`/`+` to `_`, so `debeos-r1234-gabcdef1` becomes
  `r1~beta6_debeos_r1234_gabcdef1` without breaking the `.hpkg` version grammar.
- **Degrades gracefully** — a shallow CI clone with no commit count falls back to
  `debeos-g<sha>`; a non-git tree still requires an explicit `HAIKU_REVISION`.

This is the same scheme in three places (the build script, the AMI-tag script, and the
documented intent in the pipeline config), so a build, its AMI, and its source commit
line up.

## What is implemented (landed on `graviton`)

Delivered by PR #227 (branding) and PR #257 (revision scheme), on `graviton`:

1. `determine_haiku_revision` emits `debeos-r<n>-g<sha>` for tagless builds instead of
   erroring (source/local builds now track HEAD).
2. AboutSystem, the boot-loader welcome, and the boot-menu header/footer are
   DeBeOS-branded (`OS_DISPLAY_NAME` / `OS_ATTRIBUTION_STRING` from the new
   `headers/private/shared/Branding.h`).
3. The ENA attach banner prints the live revision, not a frozen `hrev59996`.
4. The bake adds an additive `debeos-revision` (HEAD-tracking) AMI tag alongside the
   pinned `haiku-revision`.
5. `uname` and every machine-readable identifier deliberately still read "Haiku".
   Note the `struct utsname` `version` field is only 32 bytes and already carries
   `<revision> <build-date> <build-time>` — on a DeBeOS source build the revision
   alone is `debeos-r<n>-g<sha>` (~21 chars), so the field is already at its limit.
   That is a second, independent reason not to brand `uname`: there is no room to
   prepend a name without truncating the build stamp, and `uname -v` already conveys
   DeBeOS provenance through the revision it embeds.

## pkgman safety — proof the branding cannot brick an image

The branding is safe against `pkgman`'s package-version comparison **by construction**,
and this is now also confirmed on shipped hardware:

- **The compared version string is byte-identical to before the change.** The package
  version is assembled by `PreprocessPackageOrRepositoryInfo1` as
  `$(HAIKU_VERSION)_${revision}` where `HAIKU_VERSION = r1~beta6` (unchanged) and
  `${revision}` is the contents of `<build-output>/haiku-revision` with `[+-]`→`_`.
  The pipeline pins `HAIKU_REVISION=hrev59996`, and `SetHaikuRevision` uses a pinned
  value verbatim, so every published `.hpkg` is still `r1~beta6_hrev59996`. None of the
  landed work — AboutSystem, the boot-loader/menu banners, the ENA banner, the
  `determine_haiku_revision` tagless fallback, or the additive `debeos-revision` AMI
  tag — touches that arithmetic. `pkgman` therefore compares exactly the same version it
  did before #201, so it cannot be induced to "upgrade" or "downgrade" the base by the
  branding.
- **Confirmed live on the canonical AMI.** The current canonical image is baked from a
  `graviton` commit that already contains `Branding.h` and the DeBeOS-branded
  `AboutSystem` (its `source-commit` tag is an ancestor of HEAD). That image carries
  both the pinned `haiku-revision: hrev59996` **and** the additive
  `debeos-revision: debeos-r<n>-g<sha>` tag, and it only became canonical by passing the
  pipeline's Test and perf gates — which exercise package resolution on a booted box.
  So a branded image with the pinned version boots and resolves packages cleanly; the
  DeBeOS identity and pkgman-safety hold together on real Graviton hardware, not just in
  the reasoning above.

## What this does NOT change yet — and why (blast radius)

The change deliberately stops short of the **package-version arithmetic**. The line
`r1~beta6_hrev59996` is **not** moved to `r1~beta6_debeos_...`. Reasons:

- **`pkgman` version ordering against the published pool.** Every `.hpkg` already
  published in the DeBeOS pool carries `...r1~beta6_hrev59996...`. `pkgman` orders
  versions lexically/numerically by these components. Switching the revision component
  from `hrev59996` to `debeos_r<n>_g<sha>` changes how *already-published* packages sort
  relative to *new* ones. Get it wrong and either new packages look older than what is
  installed (updates silently skipped) or vice-versa. This must be designed against the
  pool, not in isolation — it is version arithmetic, not a string.
- **Native-dev-closure version lock.** The `haiku_devel` / base package versions are
  **version-locked to the AMI's hrev**: the native dev closure (`rust_bin` + `ld` +
  `haiku_devel` + cargo config) resolves the devel hpkg by the exact `haiku-revision`
  tag, and `graviton/scripts/haiku-devel-publish` keys off it. A version-string change
  is therefore **not free** — it would need the devel-publish path and the closure
  resolution updated in lockstep, or native builds on the lean AMI break.
- **`#82` interaction (now CLOSED, but the surface remains).** #82 was resolved by
  baking **only** the DeBeOS repo into the arm64 image, so the "upstream `pkgman update`
  swaps in stock Haiku" brick is closed at the *repo* level. But the version-comparison
  surface still exists *within* the DeBeOS pool itself, which is why the version line is
  still governed by the version-model decision rather than a mechanical rename.

For those reasons `HAIKU_REVISION` stays **pinned** in the pipeline (documented in
`graviton/pipeline/lib/config.ts`), the `haiku-revision` AMI tag stays pinned
(documented in `import-and-register.sh:122-131`), and the DeBeOS-native, HEAD-tracking
value is carried in **additive** surfaces (the `debeos-revision` tag, the AboutSystem
banner, source-build revisions) that nothing compares for ordering.

## Recommended follow-up sequence (deferred work)

Owned by the distribution/version-model decision, issue #92. In order:

1. **Decide the version-model in #92** — hobby-complete vs community-distributable. This
   sets whether DeBeOS keeps `r1~beta6` (Haiku's beta line) or adopts its own
   marketing/version base, and whether the pool is ever reset. Everything below depends
   on this.
2. **Choose the package-version revision component** once #92 settles it — e.g.
   `r1~beta6_debeos_r<n>_g<sha>`. Verify it sorts **strictly greater** than the current
   `...hrev59996...` for the existing pool (a one-time ordering test against published
   `.hpkg` names) so `pkgman update` still moves forward, never backward.
3. **Update the native-dev-closure resolution in lockstep** — `haiku-devel-publish` and
   the closure's devel-hpkg lookup must key off the new version, or pin an explicit
   compatibility alias, so lean-AMI native builds keep resolving.
4. **Flip the pin** — derive `HAIKU_REVISION` from HEAD (drop the hardcoded value in the
   pipeline) and let `haiku-revision` = `debeos-revision`, collapsing the two tags.
5. **Re-bake and verify on hardware** — confirm the version banner, package versions,
   and AMI tags all show the DeBeOS-native value, and that `pkgman update` against the
   pool is a no-op on an up-to-date box and a forward move on a stale one. (The landed
   banners are compile-verified, and the canonical AMI's provenance — see "pkgman safety"
   above — proves the branded binaries ship and boot on real hardware and pass the test
   gate. A dedicated serial-console capture of the boot-loader welcome text via
   `get-console-output` is the one piece of banner verification still nominally owed.)

Only after step 5 does #201 fully close; until then the additive identity above is the
shipped state.
