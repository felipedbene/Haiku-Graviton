# graviton/docs/packages — Phase-1 triage manifest (classify only)

> STATUS: Phase 1 of the packages-docs triage. Classification only — **no doc in this
> directory was moved or had its body edited** by this run. Generated 2026-10-04 against
> `origin/graviton` (`880d1ba`). Issue/PR state taken from `gh` (not from the docs' own
> wording); docs citing no issue were checked against the fixing commit + live code + the
> green package pool (`debeos-repo-green/arm64/packages/`).
>
> Phase 2 (human-approved) moves RESOLVED/SUPERSEDED docs into
> `graviton/docs/packages/archive/` with a 3-line status header and updates the inbound
> references listed below. The LIVING-but-stale corrections at the end are lockstep edits,
> not moves.

## Totals (6 docs)

| bucket | count |
|---|---|
| LIVING | 4 |
| RESOLVED | 2 |
| SUPERSEDED | 0 |
| UNSURE | 0 |

Referenced-nowhere (safe to move with no inbound fixups): `native-package-creation.md`,
`packager-restamp.md`, `pool-gc.md` — zero inbound references anywhere in the repo
(verified by `grep -rI`, worktrees/`.git` excluded). Note: referenced-nowhere ≠ archivable
— `native-package-creation.md` is LIVING (see below), so only the two RESOLVED orphans
(`packager-restamp.md`, `pool-gc.md`) are Phase-2 move candidates.

Legend: `[SOLE]` = the only in-tree record of why current code/tooling is the way it is.

## Classification

| file | bucket | evidence | inbound references |
|---|---|---|---|
| native-package-creation.md | LIVING | How native `.hpkg` are built on arm64 today. The curl `8.21.0-1` vs `8.21.0-3` note (:49-54) is **correct**: it is a live **repo regression**, tracked OPEN in **#584** — green serves only `curl-8.21.0-1-arm64.hpkg`; the doc + cargo/git need `8.21.0-3`. All other version/command claims verified against green (gcc-13.3.0_bootstrap, binutils-2.41_bootstrap, haiku_devel r1~beta6_hrev59996, haikuwebkit 1.10.0-2, git-2.54.0-1, cmake-4.1.6, ninja-1.13.2). **Stale line (lockstep fix, bucket unchanged):** :34 "There is **no `make`** in the repo yet" — `make-4.4.1_bootstrap-1-arm64.hpkg` is in green (published 2026-09-16). | NONE |
| packager-restamp.md | RESOLVED | Problem fixed: **#41 CLOSED**, implemented by `e3beab57ad`; the doc's own "durable follow-up" (publish-path packager stamping) **shipped** in #285 / `dedf4964fc`. Core payload is a one-time 2026-09 dry-run census. Not `[SOLE]` — the WHY is duplicated in `graviton/scripts/haiku-repo-restamp-packager` header and in #285. | NONE |
| pool-gc.md | RESOLVED | Problem fixed: **#42 CLOSED**; residual closure-GC **#392 MERGED** (`d6ee691d19`). Payload is two one-time dry-run results. Not `[SOLE]` — WHY duplicated in `graviton/scripts/haiku-pool-gc` header and in #392. | NONE |
| vending-design.md | LIVING | Describes the current package-management & CDN-vending design; §1/§3/§5/§6 match `haiku-repo-publish` / `haiku-package-closure` / `haiku-s3` and the hardware record. **Stale section (lockstep fix, bucket unchanged):** §4 (:101-106) calls HTTPS/openssl a "parked gap — arm64 did not advertise `openssl3_devel`"; the arm64 pool now declares `openssl3-3.5.7-1` + `openssl3_devel-3.5.7-1` (`build/jam/repositories/HaikuPortsLocal/arm64:83-84`) and the `openssl` build feature enables (`build/jam/BuildFeatures:18,23,35`). The in-tree build gate is satisfied; hardware HTTPS proof state is unverified from this host. | packages/README.md:5; modular-ami-buildout.md; audit/README.md (×2); docs/feature-cook-flow.md (×2) |
| modular-ami-buildout.md | LIVING | The gated 6-phase lean-AMI runbook; all four tools in its table exist; #552 (`d4e28101ef`) baked pip+setuptools into the fatty image, consistent with the doc. **Stale line (lockstep):** :129 "until the parked openssl/TLS build feature lands" — same openssl staleness as above. | packages/README.md:10; vending-design.md:139 |
| README.md (packages index) | LIVING | Directory index; both bullets and the Related links resolve. Its two Related "Building the chain" links were repointed into `archive/` by the dangling-refs fix in this same PR. **Stale clause (lockstep):** :8 "the one remaining gap (HTTPS/openssl, parked)" — same openssl staleness. | top-level README.md:87 (dir link) |

## Phase-2 action register

Moves (RESOLVED, both referenced nowhere → no inbound fixups required):
- `packager-restamp.md` → `graviton/docs/packages/archive/` + 3-line header (fixed by #41/`e3beab57ad`, follow-up #285).
- `pool-gc.md` → `graviton/docs/packages/archive/` + 3-line header (fixed by #42/#392).

Lockstep LIVING corrections (edits, not moves — keep the docs in place):
- `native-package-creation.md:34` — drop/repair the "no `make`" claim (`make-4.4.1_bootstrap` is in green). Caveat: it is the `_bootstrap` variant; functionality unverified here.
- `vending-design.md` §4, `modular-ami-buildout.md:129`, `README.md:8` — rewrite the openssl/HTTPS "parked gap" to "build gate satisfied in-tree (`openssl3-3.5.7-1`, feature enabled as of `e63b49a7c2`); hardware proof <state>." Correct all three in one commit so they do not drift.

Do **not** archive `native-package-creation.md` on the strength of the curl line: that line
is a correct description of an open repo regression (#584), not doc staleness.
