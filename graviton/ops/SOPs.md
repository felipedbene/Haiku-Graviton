# DeBeOS DevOps operator SOPs

Standard operating procedures for the **debeos-devops** agent. The agent runs
between the automated staleness detector and the human: it acts autonomously
within these rules and **escalates only what the rules say to escalate**. It
never improvises around a rule; when a situation isn't covered, it escalates.

Source of truth:
- **Staleness / vulnerability:** Repology (`~/repology`, weekly). Never re-derive
  freshness or CVEs by hand.
- **What DeBeOS ships:** the published pool
  `s3://haiku-graviton-hpkg-<acct>/debeos-repo/arm64/packages/`.
- **State + backlog:** DynamoDB `debeos-package-state` (PK `pkg`, GSI
  `by-build-state`). This is the only durable state — do not keep a side copy.

Hard invariants (violating any is an escalation, never a workaround):
- **Never publish a feature-capped package** (`no-feature-capped-builds.md`).
- **Never override a human `suppressed=true`.**
- **Never trigger a build the SOP didn't authorize.** No spawn-per-flag.
- Builds run **native EC2 only** (builder AMI from SSM
  `/haiku-graviton/builder-ami-id`); success = **hpkg exists**, not exit code.

## 1. Weekly triage (after the detector run)

`state-sync.py` has already upserted the snapshot and set `build_state`. The
agent's job is to review, not redo:
1. Query GSI `build_state=queued`. This is the candidate backlog (outdated /
   vulnerable, not suppressed).
2. Confirm auto-suppressions look right (spot-check `suppress_reason=local-ahead-
   of-target`). If a suppression looks wrong, escalate — do not un-suppress
   silently.
3. Leave `needs_human` items for the human; surface them in the run summary.

## 2. Planning a build wave

1. Take the queued set. Compute dependency order with
   `graviton/builder/depclosure.py` / `haiku-package-closure`. Split into
   independent dependency chains.
2. **Wave = one dependency chain on one builder** (intermediate deps resolve
   from the builder's local `packages/`; base layers are shared by publishing
   between waves). Independent chains → parallel builders.
3. **Builder budget (default):** at most **4** concurrent builders,
   `c8g.2xlarge`, **spot**. More than 4 chains → queue the rest for the next
   wave. Raising the budget is a human decision (escalate to ask).
4. **Do not queue mega-builds without human sign-off:** `gcc`, `binutils`,
   `llvm*`, `rust*`, `mesa`, `webkit`. If queued, move them to `needs_human`
   with reason `mega-build-approval` rather than starting them.
5. Start one Step Functions execution per chain with the ordered pkg list.

## 3. Interpreting build outcomes (from the state machine)

- **Success** (hpkg exists → published → next staleness run drops the flag):
  the machine sets `built`. Nothing to do.
- **`UNRESOLVABLE (build it first)`:** a dependency isn't in the repo yet.
  Ensure that dependency is itself `queued` (add it), and re-order so it builds
  first. This is normal, not a failure to escalate — unless the missing dep is
  itself already `failed` (then escalate the chain).
- **Timeout:** retry the single package **once** with the next-larger timeout
  bucket. Second timeout → `failed` + `error_class=timeout`, and escalate.
- **Other build error:** `failed` + `error_class=build-error`, `attempt_count++`.

## 4. Backoff & quarantine

- A `failed` item is **not** re-queued while its `target_version` is unchanged
  (backoff). It re-enters the backlog automatically only when Repology shows a
  newer `newest_upstream` (state-sync requeues on new target).
- `attempt_count >= 3` at the same target → set `needs_human` with
  `esc_reason=repeated-failure`. Stop retrying.

## 5. Escalation criteria (→ `build_state=needs_human` + `esc_reason`, notify)

Escalate ONLY these; everything else the agent handles:
- repeated failure (§4), mega-build approval (§2.4), a suspicious auto-
  suppression (§1.2), a feature-cap risk, a version anomaly (e.g. local ahead of
  every repo, or an un-parseable version), or any situation not covered here.
Escalation is minimal by design: record it in DDB, include it in the run
summary, optionally fire the escalation SNS topic. Never page for routine
outdated/vulnerable flags — those are the agent's job.

## 6. Rollout gate

Until the human flips the pipeline out of shadow: the agent may triage, plan,
and record intended waves, but **must not start Step Functions executions**. In
shadow it writes the plan to the run summary and stops. (This mirrors the
detector's own shadow default.)
