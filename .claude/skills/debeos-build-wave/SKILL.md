---
name: debeos-build-wave
description: Plan, run, or resume a DeBeOS missing-ports build wave or campaign on native Graviton builders — the gated scale-up sequence. Use for "run a build wave", "resume the missing-ports campaign", "drain the build backlog", "scale up the builders now that <fix> landed". Drives the existing tooling under graviton/ops/SOPs.md; it does not replace it.
---

# DeBeOS build wave / campaign resume

Run the DeBeOS package build-out under `graviton/ops/SOPs.md`. **Read that SOP
first** — this skill is the entry sequence, the SOP is the authority. You decide;
the deterministic scripts (`state-sync.py`, `depclosure.py`,
`haiku-package-closure`, the `debeos-build-wave` Step Functions,
`haiku-repo-publish-ephemeral`) execute. Never reimplement them.

## Environment (state it to any worker you spawn)
- `export AWS_PROFILE=haiku-graviton`; acct 668984504585; us-west-2.
- Trunk is `graviton` — never commit there; work in your own worktree off
  `origin/graviton`, land code via PR. Run tracked tooling from `origin/graviton`,
  not the parked primary checkout (SOP §8).
- Live builder AMI: SSM `/haiku-graviton/builder-ami-id`. State + backlog:
  DynamoDB `debeos-package-state` (PK `pkg`, GSI `by-build-state`). Pools: green
  `debeos-repo-green/arm64`, blue `debeos-repo/arm64` (frozen rollback).

## Sequence
1. **Gate the scale-up (SOP §10).** List the fixes a wide run depends on
   (tooling, closure-crediting, AMI capability, source reachability). Prove +
   merge each; if it changes the builder image, rebake and promote the AMI
   (SOP §11) before scaling. First wide run is a human go/no-go; once authorized,
   clearing the gate and resuming is yours.
2. **Compute the closure (SOP §2)** with fresh `depclosure.py` and `--base` from a
   real builder's `pkgman list-installed`. A `NOPROV:` on a base-supplied name is a
   crediting bug (#174), not a missing port — fix the input, don't escalate.
3. **Re-queue only manufactured blockers** (now-fixed crediting/tooling gaps);
   never re-queue §4 backoff (dead-upstream / repeated-failure).
4. **Drive dependency-ordered waves** with `graviton/scripts/haiku-build-wave-driver`
   at the authorized builder budget (default 4, `c8g.2xlarge` spot; >4 only with
   `--ack-budget` sign-off). The driver is the committed, resumable fan-out loop:
   it reads the deps-first `wave` attribute off the `queued` items (or a
   `depclosure` plan file), chains each wave, and drives `StartExecution`
   ≤budget-at-a-time, advancing wave by wave. It is **dry-run by default** — run it
   with no flags first to see the plan + the exact payloads; `--execute` launches
   for real (a human go/no-go, honoring the §6 shadow gate). Re-run to resume a
   stalled campaign; `ClaimBatch` leasing prevents double-building. Defer
   mega-builds (§2.4) — the driver skips them. Success = the `.hpkg` exists (§7);
   health = wall-clock progress, NEVER CloudWatch CPU% / `top` (under-reports ~55x, #140).
5. **Publish each wave to the pool before the next** — use the
   `debeos-publish-green` skill (chunked, single-flight).
6. **Fan out with the delegation contract (SOP §9)** for parallelism; exactly one
   publisher across the fan-out.
7. **Stop and report** on a new blocker class affecting many ports — don't
   mass-park.

## End with
A run summary: green count before→after (verified against S3), ports built, waves
run, escalations, and confirmation all builders/publishers were reaped.
