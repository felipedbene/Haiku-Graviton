---
name: debeos-standup
description: The DeBeOS operator morning routine — read the state board, clear drift, reconcile in-flight work and GitHub, then surface a prioritized "what to do next". Use for "standup", "morning routine", "what's the state", "catch me up", "where are we". One command that folds the session-start reconcile into a single briefing.
---

# DeBeOS operator standup

The golden morning routine: **state → reconcile → act**. Produce ONE crisp
briefing, then stop and let the human choose what to run. `export
AWS_PROFILE=haiku-graviton`. Run tooling from `origin/graviton`, not the parked
primary checkout (SOPs.md §8). This skill *reports and proposes* — it does not
start waves, publish, promote AMIs, or touch prod. Those are separate,
authorized steps (`debeos-build-wave`, `debeos-publish-green`, SOP §10/§11).

## 1. State (the board)
The SessionStart hook usually prints the status block already — if it's present,
use it; otherwise regenerate with `graviton/scripts/haiku-status`. Read:
instances, canonical AMI + invariant (tag `canonical=true`, must match SSM),
native builders online, bake pipelines, repo package count, build-wave backlog
(queued / built / needs_human / failed / building / suppressed), last wave,
waves running now. **Flag anomalies** — invariant mismatch, a builder stopped
that should be up, failed > 0 climbing, backlog stalled.

## 2. Clear drift
Run `graviton/scripts/haiku-drift-check` (same logic the SessionStart hook uses).
For each remote branch carrying code with **no open PR**: open a PR, cross-link
it to its issue, or delete it if it's already merged (verify
`git merge-base --is-ancestor` first). For markdown drift, note it. Don't leave
drift unresolved across a session.

## 3. Reconcile in-flight work
- **Waves running now > 0 or background workers alive:** judge health by
  **wall-clock progress** (wave state advancing, `.hpkg`s appearing), NEVER by
  CloudWatch CPU% / `top` (under-reports ~55x on Haiku arm64, #140). Don't reap a
  "low CPU" builder — it's almost always busy.
- **GitHub vs reality:** close issues whose fix has landed + been verified;
  comment where state moved. GitHub is the tracking source of truth.
- **needs_human backlog:** distinguish real blocks (§4 dead-upstream /
  repeated-failure — leave them) from manufactured blockers (a now-fixed
  crediting/tooling gap, e.g. `NOPROV:` on a base-supplied name #174 — re-queue
  per §10.3).

## 4. Surface "what to do next"
End with a short prioritized list: the top 2–4 actions, each mapped to its path
(`debeos-build-wave` to scale the backlog, `debeos-publish-green` to ship built
packages, an AMI promote per §11, an escalation per §5). Recommend, don't
auto-run — the human picks. Keep the whole briefing tight; lead with anomalies.
