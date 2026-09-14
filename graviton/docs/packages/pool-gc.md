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

## Deleting (human-gated)

Deletion from a live pool is **not** done by this tool. `--apply` is inert: it
prints the exact `aws s3 rm` commands and exits non-zero, so no automated caller
can treat it as a completed delete. To actually GC:

1. Re-review the candidate list (and confirm no publisher is mid-flight — SOP §3).
2. Run the printed `aws s3 rm` commands for the agreed candidates.
3. Rebuild the repo index over the remaining set and invalidate the CDN with the
   existing publish tooling — a deleted package must also leave the `repo` index,
   or pkgman will 404 on a package the index still advertises.
