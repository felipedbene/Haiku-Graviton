# Distro convergence — base image ⨯ package repo

The curated backbone for DeBeOS's two "cooks" — the **base image** and the
**package repo** — and for the dormant-code triage that came out of the
2026-08-31 audit. This is the design of record for the retired DeBeOS #58.

It lives here rather than in the issue tracker on purpose. #58 described itself as
"the human-curated backbone", and a curated backbone is not a defect: it has no
reproduction, no `file:line`, and nothing to close. The tracker holds defects and
actionable work (see **AGENTS.md → Issue tracking**); standing state belongs in a
doc that can be edited in place. Live fleet, bake, repo and backlog numbers come
from `graviton/scripts/haiku-status`, which the SessionStart hook prints — read
that for current state, not this file.

## Dormant / stray code — the 2026-08-31 audit is fully discharged

Every item the audit raised has landed and its issue is closed:

| Item | Issue | Outcome |
|---|---|---|
| ENA multiqueue — affinity + RSS infra | #56 | closed |
| Receive-path ECN (RX-FIFO shrink, CoDel, ECN) | #57 | closed |
| BFS auto-grow | #54 | closed; grow verified, see below |
| Native-build tooling | #55 | closed (haiku-nativebuild driver + arm64 recipe patches) |
| DIAG-E2 measurement build | — | archived tag `archive/diag-e2-tcp-flock` |

The repo build-out blockers and recipe fixes are likewise all closed: #51
(build-tooling layer), #50 (audio-codec base layer), #40 (`_devel` subpackages),
#43 (`@minimum` dropping `media_kit`/`game_kit`), #52 and #44 (recipe fixes), #53
(port-name derivation), #48 (haikuporter self-dependency). The M1 `wss` broker
(#415) has shipped.

Forward work on the exercised surface is tracked as its own enhancements in
[`../audit/ENHANCEMENTS.md`](../audit/ENHANCEMENTS.md), not here.

## BFS auto-grow — verified 2026-10-15

The one convergence item that had never been demonstrated was BFS growing past
the canonical AMI's small root. It now has been, on `i-01886408618b237de`
(`c7g.8xlarge`, launched from canonical `ami-0eee11d43faa67d8f` with
`haiku-launch --disk 500`):

```
 Total Blocks:  500.0 GiB (262125559 blocks)
  Used Blocks:   63.1 GiB (33100935 blocks)
  Free Blocks:  436.8 GiB (229024624 blocks)
```

**Convergence took one boot, not two.** `graviton/scripts/haiku-launch`'s header
still says it takes two — first boot's `launch_daemon` job grows the GPT
partition, the next root mount grows BFS — and advises `--wait` plus a reboot to
get the space before starting a build. On this image the full 500 GiB was present
and mounted on the first boot, with no reboot. That matters beyond tidiness: the
SSM agent **does not survive a reboot** on this image (only a fresh launch
re-registers it), so a reboot that is not actually needed costs the SSM command
channel for nothing. Prefer `--disk` plus one boot, and verify with `df` before
assuming either way.

## Residual operational work

Not defects — pipeline work, run through the existing gated paths:

- **The heavy tail**: `llvm21` → `mesa-25` → `webkit`, built natively on a
  Graviton builder per [`native-ec2-builds.md`](native-ec2-builds.md) and
  published into the repo. These are the largest remaining ports and each needs
  real root-disk headroom, which is what the grow verification above unblocks.
- **Final pool-delta publish** — blue (`debeos-repo/arm64`) into green
  (`debeos-repo-green/arm64`). Green is **live and production**; promotion is
  human-gated. `haiku-repo-promote-green --plan` is read-only and prints the
  delta — use it to review before anything is promoted.
- **Build-wave backlog** — scale it with `debeos-build-wave`. Distinguish genuine
  blocks (dead upstream, repeated failure) from manufactured ones; a now-fixed
  crediting or tooling gap that parked a port is re-queued, not left in
  `needs_human`.

The stale SSM ping in `haiku-mgmt-agent` (that repo's issue #3) is tracked
upstream in [felipedbene/haiku-mgmt-agent](https://github.com/felipedbene/haiku-mgmt-agent),
not here.

## Related

- [`native-ec2-builds.md`](native-ec2-builds.md) — the native arm64 build method
- [`crate-to-hpkg.md`](crate-to-hpkg.md) — Rust crate → native arm64 → hpkg
- [`../ops/SOPs.md`](../ops/SOPs.md) — bake, publish and promote procedures
- [`port-hygiene.md`](port-hygiene.md), [`porting-playbook.md`](porting-playbook.md)
