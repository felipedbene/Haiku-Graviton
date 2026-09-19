# Pool GC: superseded-revision cleanup (issue #42)

Superseded, old-revision `.hpkg` can linger in the pool alongside their
replacements — pool bloat, and a risk of an old revision being resolved or
published by mistake. `graviton/scripts/haiku-pool-gc` identifies them.

## What it flags

Within a single package identity — the **same `name` AND same `architecture`** —
it keeps the highest `(version, revision)` and flags every strictly-older one.

- Package `name` is the field before the first hyphen (Haiku names never contain
  `-`; they use `_`). So versioned library slots that are *different packages* —
  `libvpx1.15` vs `libvpx1.16`, `python3.10` vs `python3.12` — are kept side by
  side, and a `_devel` / `_bin` sub-package is only ever compared with other
  revisions of that same sub-package.
- Version ordering follows Haiku's `BPackageVersion` closely enough to be safe:
  dotted release segments compared numerically when both are numeric (so
  `2.10 > 2.9`, not lexically), a release with no `~prerelease` outranks the same
  release with one, then the integer revision breaks ties.
- If two entries cannot be strictly ordered they land in an **ambiguous** bucket
  and NEITHER is flagged — the tool never GCs on an uncertain compare.

Validated on a synthetic pool: `foo-2.9-5` ⇐ `foo-2.10-1`, `bar-1.0~beta1-1` ⇐
`bar-1.0-1`, `libvpx1.16-1.16.0-1` ⇐ `libvpx1.16-1.16.0-3`, while `libvpx1.15`,
the `-any` vs `-arm64` split, and single-revision packages are left untouched.

## Dry-run result (2026-09)

Both published pools and the staging prefixes were scanned:

| pool | packages | GC candidates |
|------|---------:|--------------:|
| `debeos-repo/arm64/packages` (blue) | 1885 | **0** |
| `debeos-repo-green/arm64/packages` (green) | 2420 | **0** |
| `drain-seeds/`, `scratch/` | 0 hpkg | — |

There are currently **no superseded revisions** in the live pools — each
`(name, arch)` has a single revision. The specific candidates named in the issue
have already turned over: `libvpx1.16` is now a single copy at revision **3** (the
`-1` the issue named is gone), and `libjpeg_turbo-3.1.4.1` exists only at revision
1. The pool has been kept clean since the issue was filed; the tool is in place to
catch supersession the next time a revision bumps.

Reproduce:

```sh
export AWS_PROFILE=haiku-graviton
graviton/scripts/haiku-pool-gc --s3 s3://<bucket>/debeos-repo/arm64/packages
graviton/scripts/haiku-pool-gc --s3 s3://<bucket>/debeos-repo-green/arm64/packages
```

## Closure GC (dependency-graph side)

Superseded-revision GC (above) only compares versions of the *same* name. The
other half of pool hygiene is the **dependency closure**: which packages are
reachable from the set the project actually ships, which are pure leaves, and
which declare a dependency the pool can't satisfy. Pass `--repo <the published
repo index>` and the tool also reads every package's `provides`/`requires` —
straight out of the HPKR `repo` index via `hpkg_meta` (no Haiku host, one small
download instead of a ranged read of every `.hpkg`) — and reports:

- **leaf packages** — nothing in the pool depends on them. These are the natural
  top-level installables (applications, end tools). A leaf is **not** garbage —
  it is a root — so the default run never flags one.
- **unreachable orphans** — packages reachable from *no* root. Roots default to
  the leaf set, so a well-formed repo reports **0**. Pass `--roots FILE` (one
  package name per line — e.g. a lean image's want-set, or the curated set of
  top-level apps the project ships) to compute the orphans **relative to what you
  actually ship**: everything the roots don't transitively need. That set is the
  real closure-GC candidate list.
- **dangling requires** — a `requires` token that *nothing* in the pool provides.
  The requiring package cannot resolve/install from this repo. This is a
  repo-health signal (a missing dependency to build/publish), **not** an auto-GC
  target — the fix is usually to add the provider, not delete the requirer.

> Caution on `--roots`: the orphan count is only meaningful against a *complete*
> root set. A tiny root list marks almost everything "unreachable" — that means
> "not needed by those few roots", not "garbage". Supply the roots that represent
> everything the pool is meant to serve.

## Dry-run result — closure (2026-09)

Scanned with the live `repo` indexes (leaf-default roots):

| pool  | index pkgs | leaves | unreachable orphans | dangling requires |
|-------|-----------:|-------:|--------------------:|------------------:|
| green | 2964       | 2001   | **0**               | 50                |
| blue  | 1885       | 1264   | **0**               | 75                |

No unreachable orphans in either pool under the default (leaf) roots — the graph
is well-formed, every package is reachable from some top-level package. The
`--roots` path was exercised with a 3-package root set (`nano,curl,openssl3` → 25
in closure, 2939 unreachable, ~7.5 GiB) to confirm the closure math and that the
`--apply` plan then lists the closure orphans as well; that huge number is the
*illustrative* "not needed by three packages", not a delete recommendation.

The dangling-requires lists are the actionable output today: green needs 50
providers it doesn't carry (a `*_python310` cluster, `retroarch`, `tk`, `yab`,
`vendor_tesseract`, `cmd:login`/`cmd:passwd` for openssh, …); blue needs 75.
Those packages can't currently resolve from the repo — the fix is to build and
publish the missing providers (build-wave work), not to GC.

Reproduce:

```sh
export AWS_PROFILE=haiku-graviton
graviton/scripts/haiku-pool-gc \
  --s3   s3://<bucket>/debeos-repo-green/arm64/packages \
  --repo s3://<bucket>/debeos-repo-green/arm64/repo
# closure relative to a real ship-set:
graviton/scripts/haiku-pool-gc --s3 ... --repo ... --roots lean-image.want
```

## Deleting (human-gated)

Deletion from a live pool is **not** done by this tool. `--apply` is inert: it
prints the exact `aws s3 rm` commands (superseded revisions first, then any
closure orphans) and exits non-zero, so no automated caller can treat it as a
completed delete. To actually GC:

1. Re-review the candidate list (and confirm no publisher is mid-flight — SOP §3).
2. Run the printed `aws s3 rm` commands for the agreed candidates.
3. Rebuild the repo index over the remaining set and invalidate the CDN with the
   existing publish tooling — a deleted package must also leave the `repo` index,
   or pkgman will 404 on a package the index still advertises.
