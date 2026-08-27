# DeBeOS bug-audit tooling

An agentic, **propose-only** pipeline for systematically working the static-analysis findings from
the bug-audit flow. It confirms, root-causes, and proposes fixes for ranked findings — but never
writes to the working tree, never commits, and never builds (there is no `aarch64-haiku`
cross-toolchain on the analysis host). Every fix comes back as a reviewable diff plus a Graviton
hardware-verification plan.

## Pieces

| File | Role |
|---|---|
| `../../FINDINGS.md` | Phase 1+2 output: full static-sweep table + known-upstream cross-reference. |
| `../../PRIORITY.md` | Phase 3 output: top-28 ranked candidates for a deep pass. |
| `agent-sops/debeos-bugfix.sop.md` | The per-finding unit of work (RFC-2119 SOP). Confirm → classify → minimal-fix-as-diff → static self-verify → commit message + hardware plan. Runnable standalone by any agent. |
| `agent-sops/debeos-hardware-proof.sop.md` | **Morning runbook** (gated, mutating): takes the overnight scoreboard's surviving real-bugs → topic branch → **build** on the cross-compile host → bake `candidate=true` AMI → boot a disposable target → run the subsystem workload → **human promotion gate** → `haiku-canonical promote` → teardown. Fail-closed; canonical untouched until approval. |
| `debeos-bugfix.workflow.js` | Interactive fan-out: parse `PRIORITY.md` → one SOP agent per finding (own scratch worktree) → one review batch. Good for a small, targeted slice. |
| `collect_candidates.py` | **Deterministic** collect+cluster: `FINDINGS.md` → active-surface bug-shaped findings, near-duplicates clustered, FP-prone value-flow checks on vendored/BSD down-ranked. Emits the `findings` array for the overnight run. |
| `debeos-bugfix-overnight.workflow.js` | **Unattended overnight** orchestrator: SOP per cluster → **two-sided adversarial verify** (refuters attack real-bugs; a **prosecutor** attacks each false-positive → `disputed-FP` for human review) → morning **scoreboard** + review. Idempotent (skips finished reports), resumable, propose-only, no AWS. |
| `review/latest-review.md`, `review/overnight-review.md`, `review/overnight-scoreboard.md` | Generated review artifacts (gitignored) — the human approval gate. |

## Overnight run

```
python3 graviton/audit/collect_candidates.py FINDINGS.md > /tmp/candidates.json   # deterministic collect+cluster
# then pass its contents as args.findings:
Workflow({ scriptPath: ".../graviton/audit/debeos-bugfix-overnight.workflow.js",
           args: { findings: <contents of candidates.json>, refuters: 2 } })
```
- **Unattended-safe:** propose-only (no writes/commits) and **needs no AWS** — the 1-hour STS creds
  can lapse overnight without affecting it. Hardware proof stays a separate daytime gated step.
- **Resumable:** relaunch with `resumeFromRunId: <run>`; unchanged agents replay from cache, and each
  finding's report persists on disk.
- **Scale note:** processes all clusters (≈139 for the current tree) — far above the ≤15-agent
  workflow-size guideline, which is expected for an explicitly-requested overnight run (raise/relax
  it in `/config` → Dynamic workflow size if the harness caps it).
- **Morning:** read `review/overnight-scoreboard.md` first (surviving real-bugs, FP rate), then the
  full `review/overnight-review.md` for diffs.

## Running it

```
Workflow({ scriptPath: "<repo>/graviton/audit/debeos-bugfix.workflow.js",
           args: { subsystems: ["bfs"], limit: 6 } })
```

`args` (all optional):
- `subsystems`: array of `ena` / `network-stack` / `bfs` / `app_server-remote` / `kernel/arm64`. Omit = all active-surface.
- `limit`: max findings this run (default 10). Keeps the fan-out near the ≤15-agent guideline; the workflow `log()`s whatever it defers rather than silently dropping.
- `repo_root` (default `/local/home/benfelip/Haiku-Graviton`), `scratch_root` (default `/tmp/debeos-bugfix`).
- `findings`: supply a pre-parsed findings array to skip the `PRIORITY.md` parse (for re-runs).

Standalone (one finding, no workflow): dispatch an agent to read
`agent-sops/debeos-bugfix.sop.md` and follow it with `finding`, `repo_root`, and `output_file`.

## Morning run (hardware-proof & promotion)

After the overnight run completes, follow `agent-sops/debeos-hardware-proof.sop.md` (needs **refreshed
AWS creds** — the overnight STS token will have expired):

```
Pre-flight (creds live? tree synced? read scoreboard) → topic branch + apply approved diffs
 → build on haiku-builder3 (fail-closed) → bake candidate=true AMI → boot disposable target
 → run subsystem workload (BFS durability / jumbo iperf / ena_fault / metal boot)
 → PROMOTION GATE (human "yes") → haiku-canonical promote → teardown + AUDIT_LOG entry
```

Every mutating step (start builder, bake, launch, SG rule, promote) is **confirmed**; a build
failure or failed workload **rejects** the fix; canonical is never touched until the gate. This is a
sequential gated runbook, not an autonomous fan-out — the mutations and cost make human gates the
right control.

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
