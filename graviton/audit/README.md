# DeBeOS bug-audit tooling

An agentic, **propose-only** pipeline for systematically working the static-analysis findings from
the bug-audit flow. It confirms, root-causes, and proposes fixes for ranked findings — but never
writes to the working tree, never commits, and never builds (there is no `aarch64-haiku`
cross-toolchain on the analysis host). Every fix comes back as a reviewable diff plus a Graviton
hardware-verification plan.

> **v2 (2026-08-27).** The interactive and overnight workflows are now **one** parameterized
> workflow (`debeos-bugfix.workflow.js`). The v1 split let the interactive path drift *without* the
> adversarial-verify stage — its shakedown returned 4/4 false-positives with no cross-check. v2 folds
> in the overnight run's lessons: **verify is always on**, the **prosecutor is weighted** toward the
> high-surface / FP-prone findings where the missed bugs hid, and a new **Adjudicate** stage resolves
> every disputed false-positive to a verdict instead of leaving it for the human. See
> `AUDIT_LOG.md` for the run that motivated each change.

## Pieces

| File | Role |
|---|---|
| `../../FINDINGS.md` | Phase 1+2 output: full static-sweep table + known-upstream cross-reference. |
| `../../PRIORITY.md` | Phase 3 output: top-28 ranked candidates for a deep pass. |
| `agent-sops/debeos-bugfix.sop.md` | The per-finding unit of work (RFC-2119 SOP). Confirm → classify → minimal-fix-as-diff → static self-verify → commit message + hardware plan. Runnable standalone by any agent. |
| `agent-sops/debeos-hardware-proof.sop.md` | **Morning runbook** (gated, mutating): surviving real-bugs → topic branch → **build** on the cross-compile host → bake `candidate=true` AMI → boot a disposable target → run the subsystem workload → **human promotion gate** → `haiku-canonical promote` → teardown. Fail-closed; canonical untouched until approval. |
| `agent-sops/debeos-metal-build-debug.sop.md` | **DEPRECATED** (the shared metal builder it drove is terminated): fast `jam` build/solve iteration — reproduce a CrossBuild/package-solve failure and fix it in minutes instead of one bake at a time. The "verify it builds" loop between staging a change and the hardware-proof bake; now belongs on a spun-up native Graviton builder. Still documents the traps (clone-trap tree, HAIKU_REVISION, proper local-package staging, the sudo/PWD jam trap). |
| `collect_candidates.py` | **Deterministic** collect+cluster: `FINDINGS.md` → active-surface bug-shaped findings, near-duplicates clustered, FP-prone value-flow checks on vendored/BSD down-ranked. Emits the `findings` array for the overnight mode. |
| `debeos-bugfix.workflow.js` | **The workflow.** Collect → SOP-per-finding (isolated worktree) → two-sided adversarial verify (refuters attack real-bugs; weighted prosecutors attack false-positives) → **Adjudicate** each dispute → scoreboard + review. Propose-only, idempotent (skips finished reports), resumable, no AWS. Runs both a targeted slice and the full overnight sweep — pick with `args.mode`. |
| `ENHANCEMENTS.md` | The curated **enhancement backlog** (the `PRIORITY.md` analogue for features). Hand-written; each entry carries a measurable acceptance criterion. |
| `agent-sops/debeos-enhance.sop.md` | The per-enhancement unit of work: assess worth+feasibility → implement as a **full diff** → static self-check → commit message + hardware plan. Propose-only. |
| `debeos-enhance.workflow.js` | **The enhancement workflow.** Collect (`ENHANCEMENTS.md`) → SOP-per-enhancement (isolated worktree, full diff) → **pragmatic champion-vs-skeptic** review → scoreboard + review. Shorter than the bug-fix pass (no false-positive branch, no prosecutor/adjudicator). |
| `debeos-feature-cook.workflow.js` | **The feature-cook front half.** Given a feature spec (+ optional design doc): decompose → investigate the source (fan-out) → synthesize diffs → adversarially verify each → assemble a **bake-ready batch**. Propose-only; hands off to the hardware-proof SOP for the gated tail. Generalises the per-feature investigate→diff→verify pattern. |
| `review/*-review.md`, `review/*-scoreboard.md`, `review/candidates.json` | Generated review artifacts (gitignored) — the human approval gate. |

## The workflow (one file, two modes)

`args.mode` is inferred from what you pass, or forced explicitly:

- **`slice`** — a small, targeted set parsed from `PRIORITY.md`. Default when you pass
  `subsystems`/`limit` (or nothing). Keeps the fan-out near the ≤15-agent guideline.
- **`overnight`** — the full clustered sweep. Default when you pass `findings` or `candidates_file`.

### Slice (targeted)

```
Workflow({ scriptPath: "<repo>/graviton/audit/debeos-bugfix.workflow.js",
           args: { subsystems: ["bfs"], limit: 6 } })
```

### Overnight (unattended, full sweep)

```
python3 graviton/audit/collect_candidates.py FINDINGS.md > graviton/audit/review/candidates.json
Workflow({ scriptPath: "<repo>/graviton/audit/debeos-bugfix.workflow.js",
           args: { mode: "overnight", refuters: 2 } })          # loads candidates.json by default
```

- **Unattended-safe:** propose-only (no writes/commits) and **needs no AWS** — the 1-hour STS creds
  can lapse overnight without affecting it. Hardware proof stays a separate daytime gated step.
- **Resumable:** relaunch with `resumeFromRunId: <run>`; unchanged agents replay from cache, and each
  finding's report persists on disk (the Fix stage skips any finding whose report already exists).
- **Scale note:** overnight processes all clusters (≈139 for the current tree) — far above the
  ≤15-agent guideline, which is expected for an explicitly-requested overnight run (raise/relax it in
  `/config` → Dynamic workflow size if the harness caps it).
- **Morning:** read `review/overnight-scoreboard.md` first — the **adjudicated-real** section is the
  highest value (disputes the first pass got wrong), then the full `review/overnight-review.md`.

`args` (all optional):
- `mode`: `slice` | `overnight` (inferred if omitted).
- `subsystems`: array of `ena` / `network-stack` / `bfs` / `app_server-remote` / `kernel/arm64` (slice). Omit = all active-surface.
- `limit`: max findings for a slice run (default 10); the workflow `log()`s whatever it defers rather than silently dropping.
- `findings` / `candidates_file`: pre-parsed clustered array (overnight); defaults to `graviton/audit/review/candidates.json`.
- `refuters` (default 2): defenders per real-bug (majority-refute kills it).
- `prosecutors` (default 1): baseline offense per false-positive; **auto-raised** to 2–3 on high-surface (`ena`/`network-stack`) and FP-prone/vendored findings.
- `adjudicators` (default 1): judges per disputed-FP (majority verdict; any `real-bug` majority promotes it).
- `repo_root` (default `/local/home/benfelip/Haiku-Graviton`), `scratch_root` (default `/tmp/debeos-bugfix`).

Standalone (one finding, no workflow): dispatch an agent to read
`agent-sops/debeos-bugfix.sop.md` and follow it with `finding`, `repo_root`, and `output_file`.

## Adversarial verify + adjudicate (why two-sided, then a judge)

The base SOP verdict is not trustworthy at scale — the overnight run hit a **94.9% false-positive
rate** *and* mislabelled ≥4 real-ish findings as false-positive. Two roles attack it, and a third
resolves the fight:

- **Refuters** (defense) attack each `real-bug` — try to prove the fix over-reaches or the bug is an
  FP. Majority-refute demotes it.
- **Prosecutors** (offense) attack each `false-positive` — try to build a concrete triggering path
  (callers, error paths, lock scopes, ARM64 weak-memory reordering) that makes the dismissal wrong.
  A credible path marks the finding **disputed**. These are where the missed bugs live, so v2 spends
  more prosecutors exactly on the high-surface / FP-prone findings.
- **Adjudicator** (judge) reads the *code* plus both arguments for every dispute and rules it
  `real-bug` (says whether the prosecutor located the root cause correctly), `false-positive`
  (dismissal upheld), or `needs-hardware`. Disputes come back **resolved**, not dumped on the human.

## Enhancements (debeos-enhance)

The same shape as the bug-fix pass, but for *chosen* improvements rather than static-analysis
findings — so it is **shorter**. An enhancement is not a maybe-noise finding, so there is **no
false-positive branch, no prosecutor, and no adjudicator**; the Verify phase becomes a single
**pragmatic champion-vs-skeptic** review.

```
Workflow({ scriptPath: "<repo>/graviton/audit/debeos-enhance.workflow.js",
           args: { subsystems: ["ena"], limit: 4 } })
```

- **Collect** — parse `ENHANCEMENTS.md` (curated backlog; filter by `args.subsystems`/`args.limit`).
- **Design** — one SOP agent per enhancement, isolated worktree, implements a **full unified diff**
  (or returns `needs-decomposition` / `reject` / `out-of-scope`).
- **Review** — a **champion** (does it deliver the acceptance criterion? worth shipping?) and a
  **skeptic** (concrete blocker only: real regression, scope-creep, a genuinely simpler equivalent,
  or it doesn't deliver — *not* taste). Pragmatic rule: **ship-ready unless there's a material
  blocker**.
- **Report** — `review/enhance-scoreboard.md` + `review/enhance-review.md`.

`args`: `subsystems`, `limit` (default 8 — full diffs are heavier), `enhancements` (pre-parsed array
to skip the backlog parse), `backlog_file`, `repo_root`, `scratch_root`. Same guardrails as the
bug-fix pass (propose-only, active-surface only, one enhancement per commit); a ship-ready diff still
goes through the same `debeos-hardware-proof` pipeline runbook before it lands in canonical.

## Feature cooking (end to end)

"Cooking a feature" = taking an idea to a promoted canonical AMI. It is deliberately split into an
**automated front half** (a workflow) and a **human-gated tail** (an SOP) — because the tail mutates
trunk, bakes images, and moves the canonical tag, which must not be a headless fan-out.

```
spec / design doc
   │
   ▼  debeos-feature-cook.workflow.js          (automated, propose-only, no AWS)
   │    decompose → investigate (fan-out) → synthesize diffs → adversarial verify → bake-ready batch
   ▼
stage confirmed diffs on a topic branch  →  PR → merge to graviton   (human review)
   │
   ▼  agent-sops/debeos-hardware-proof.sop.md   (gated, mutating)
        bake candidate=true (pipeline) → boot disposable target → run the hardware_plan
        → HUMAN promotion gate → haiku-canonical promote → teardown
```

- The workflow never builds (no cross-toolchain on the analysis host) and never commits — it emits
  diffs + the hardware plan.
- The pipeline builds from `graviton`, so the confirmed diffs land via a normal PR before the bake.
- Canonical only moves behind the explicit human gate in the SOP.

Run the front half:

```
Workflow({ scriptPath: "<repo>/graviton/audit/debeos-feature-cook.workflow.js",
           args: { spec: "<what to build + measured ground truth>",
                   design_doc: "graviton/docs/packages/vending-design.md" } })   # design_doc optional
```

`args`: `spec` (feature brief) and/or `design_doc` (repo-relative path read as authoritative);
`max_angles` (default 4); `repo_root`; `out` (scratch dir). The worked reference example is the
pkgman remote-repo feature (`graviton/docs/packages/vending-design.md` §4).

## Morning run (hardware-proof & promotion)

After a run completes, follow `agent-sops/debeos-hardware-proof.sop.md` (needs **refreshed AWS
creds** — the overnight STS token will have expired):

```
Pre-flight (creds live? tree synced? read scoreboard) → topic branch + apply approved diffs
 → build on the cross-compile host (fail-closed) → bake candidate=true AMI → boot disposable target
 → run subsystem workload (BFS durability / jumbo iperf / ena_fault / metal boot)
 → PROMOTION GATE (human "yes") → haiku-canonical promote → teardown + AUDIT_LOG entry
```

Every mutating step (start builder, bake, launch, SG rule, promote) is **confirmed**; a build
failure or failed workload **rejects** the fix; canonical is never touched until the gate.

## Guardrails (enforced by the SOP)

- **Propose-only.** All editing/analysis happens in a disposable detached `git worktree`; the real
  tree, index, and the `graviton` branch are never touched. Nothing is committed or pushed.
- **Active surface only.** Findings outside kernel/arm64, ENA, network stack, BFS, or app_server
  remote are rejected as out-of-scope.
- **One finding per run**, one diff per finding — no bundling, no drive-by fixes (logged as
  incidental observations instead).
- **Static verification is not proof.** A cleared checker means the tool is satisfied, not that the
  code builds or boots. Each real-bug carries a named Graviton workload to run before it ships.

## Prerequisites

- `cppcheck`, `clang`, `clang-tools-extra` installed (`sudo dnf install -y ...`).
- A git checkout that supports worktrees (this repo).
- The Workflow runtime validates the script's syntax on first invocation.
