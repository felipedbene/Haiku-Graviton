# DeBeOS feature-cook — end-to-end flow

How a change gets from an idea to a promoted canonical AMI. This is the design of record for the
"feature cook" loop; the pieces live in `graviton/audit/` (develop workflows + SOPs) and
`graviton/pipeline/` (the bake pipeline).

**Design principle:** *everything automated is propose-only; everything that mutates trunk, infra, or
the canonical AMI is human-gated.* Automation finds, fixes, and adversarially verifies — it never
commits, never builds, never touches AWS. Humans approve the merge, the bake, and (always) the
promotion.

## The loop

```
                          ┌─────────────────────── SOURCES OF WORK ───────────────────────┐
  bugs:         static sweep → FINDINGS.md → PRIORITY.md
  enhancements: curated ENHANCEMENTS.md
  features:     a spec / design doc  (e.g. graviton/docs/packages/vending-design.md)
                          └───────────────────────────────┬────────────────────────────────┘
                                                           ▼
╔══════════════ STAGE 1 · DEVELOP  (automated · propose-only · no build · no AWS) ══════════════╗
║  pick the matching workflow — each runs agents in isolated git worktrees, static self-checks,  ║
║  adversarially verifies, and emits reviewable diffs + a named hardware plan. None touch trunk. ║
║                                                                                                ║
║   debeos-bugfix.workflow.js      collect → SOP-fix → refuters + weighted prosecutors           ║
║                                   → ADJUDICATE disputes → batch                                ║
║   debeos-enhance.workflow.js     collect → SOP-implement (full diff) → champion-vs-skeptic → batch║
║   debeos-feature-cook.workflow.js  decompose → investigate (fan-out) → synthesize diffs         ║
║                                   → adversarial verify → bake-ready batch                        ║
╚════════════════════════════════════════════════╤═══════════════════════════════════════════════╝
                                                  ▼   (verify REFUTES a change? → refine, re-verify)
┌──────────────── STAGE 2 · REVIEW & STAGE  (HUMAN) ─────────────────┐
│  read the batch → stage confirmed diffs on a topic branch          │
│  → PR → merge to `graviton`                                        │
└──────────────────────────────┬─────────────────────────────────────┘
                                ▼
╔═══════════ STAGE 3 · PROVE & PROMOTE  (gated SOP: debeos-hardware-proof) ═══════════╗
║  the graviton merge triggers the CDK bake pipeline:                                 ║
║     Source → CrossBuild → Register(candidate=true) → perf-gate Test → Approve → Promote║
║  Register → candidate AMI    (CANONICAL UNTOUCHED)                                   ║
║        ▼                                                                             ║
║  boot a DISPOSABLE target from the candidate → run the feature's hardware plan       ║
║  (the workload that actually proves it)                                              ║
║        ▼                                                                             ║
║  ██ HUMAN PROMOTION GATE ██  →  haiku-canonical promote  (single-canonical invariant)║
║        ▼                                                                             ║
║  teardown disposable · AUDIT_LOG entry                                               ║
╚═════════════════════════════════════════════════════════════════════════════════════╝
                                ▼
                     new CANONICAL AMI  (what every future instance boots)

  Feedback loops:  verify refutes → back to Stage 2 refine.   build/hardware fails → back to Stage 1/2.
  Cross-cutting:   private-S3 + CloudFront(OAC) package vending · memory → S3 exit-hook sync ·
                   discipline: static-verify ≠ proof; adversary catches overclaims; HARDWARE is the arbiter.
```

## The three invariants that make it safe

1. **Propose-only automation.** Stage 1 works only in throwaway `git worktree`s — no writes to the
   tree, no commits, no AWS, no build (there is no cross-toolchain on the analysis host). Output is
   always a reviewable diff plus a named hardware workload.
2. **Adversarial verification before a human looks.** Every candidate change is attacked
   (refuters / prosecutor / adjudicator for bugs; champion-vs-skeptic for enhancements; per-change
   verify for features). Overclaims and mislocated fixes are caught here, not on hardware.
3. **Human gates on everything irreversible.** Merge to trunk is a PR. A bake is billable and
   explicit. **Promotion of canonical requires a live human "yes."** The pipeline may bake candidates
   freely; only the gate moves the `canonical=true` tag, and the single-canonical invariant is
   verified after.

## Stages in detail

### Stage 1 — Develop (automated)
Three engines, one shape (isolated worktree → static self-check → adversarial verify → batch):
- **`graviton/audit/debeos-bugfix.workflow.js`** — static-analysis findings. Two-sided verify
  (refuters attack real-bugs; weighted prosecutors attack false-positives) then an **adjudicator**
  resolves every dispute to a verdict.
- **`graviton/audit/debeos-enhance.workflow.js`** — curated enhancements. A **pragmatic
  champion-vs-skeptic** review (ship unless a concrete, material blocker).
- **`graviton/audit/debeos-feature-cook.workflow.js`** — a feature spec / design doc. **Decompose →
  investigate (fan-out) → synthesize diffs → adversarial verify → bake-ready batch.**

### Stage 2 — Review & stage (human)
Read the batch, stage the confirmed diffs on a topic branch, PR, merge to `graviton`. This is where a
human refines anything the verifier flagged and re-scopes as needed — automation proposes, the human
disposes.

### Stage 3 — Prove & promote (gated SOP)
`graviton/audit/agent-sops/debeos-hardware-proof.sop.md`. The merge triggers the CDK bake pipeline
(`graviton/pipeline/`, which builds from `graviton`): **Source → CrossBuild → Register
(`candidate=true`) → perf-gate Test → manual Approve → Promote.** Register yields a candidate AMI;
canonical is untouched. Boot a disposable target from the candidate, run the feature's hardware plan,
and only on an explicit human "yes" does `haiku-canonical promote` move the tag. Cross-compiled changes
go through the pipeline; native/KVM (haikuporter) builds run on native Graviton Haiku EC2 instances
over SSM (`haiku-nativebuild`), not a shared metal builder — that host has been retired.

## Worked example (this is not hypothetical)

The pkgman remote-repo feature ran the whole loop, including its feedback edges:
1. **Design** — `graviton/docs/packages/vending-design.md` (private S3 + CloudFront/OAC vending; the
   measured client gap; the fix plan).
2. **Develop** — `debeos-feature-cook` produced two diffs (EINTR restart; enable the arm64 openssl
   build feature). **Adversarial verify refuted both** — correctly: it flagged that the EINTR fix
   left `connect()` uncovered, and (implicitly) that the openssl diff risked the wrong file.
3. **Review & stage** — a human narrowed the EINTR fix to `recv/send` (POSIX-correct) and corrected
   the openssl change to `HaikuPortsLocal/arm64` (not the remote list, whose checksum selects a
   published index). PR → merge.
4. **Bake** — CrossBuild caught an incomplete change (*"nothing provides `lib:libzstd`"*) at the image
   solver — a real defect the build gate stopped before any bootable image existed. Root-caused,
   added `zstd` to the local list, re-merged, re-baked. **No candidate was ever mis-registered; the
   canonical never moved.**
5. **Prove → promote** — boot a disposable from the candidate, run the hardware plan
   (`pkgman add-repo https://…` + `http://…` → refresh → install), promote on green.

The lesson embodied in the flow: **the gates are the point.** Static and adversarial checks kill
overclaims cheaply; the build gate kills incomplete changes; the hardware gate is the only thing that
says "true"; and the human promotion gate is the only thing that ships.
