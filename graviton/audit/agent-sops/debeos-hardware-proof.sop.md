# DeBeOS Hardware-Proof & Promotion (morning runbook)

## Overview

Takes the **surviving real-bugs** from a propose-only run (`*-scoreboard.md`) and drives each proposed
diff to **real-Graviton proof** and, only behind an explicit human gate, into the **canonical AMI**.
This is the complement to `debeos-bugfix.sop.md`: that one proves the *checker* is satisfied and never
builds; this one **builds, bakes, boots, and exercises** the fix on hardware — the step the analysis
host cannot do.

**Build + bake go through the `haiku-graviton-bake` CodePipeline, not a hand-driven metal builder.**
The pipeline's stages — Source → CrossBuild (in CodeBuild) → Register (`candidate=true`) → Test
(perf-gate) → Approve (manual) → Promote (`haiku-canonical promote`) — already encode the fail-closed
build, the candidate registration, and the gated promotion. The old "start `haiku-builder3`, run `jam`
by hand" path is retired: a cross-compiled kernel/driver fix needs no builder host of its own — the
pipeline's CrossBuild stage compiles it in CodeBuild. **Native / KVM (haikuporter) builds** now run on
native Graviton Haiku EC2 instances over SSM (`haiku-nativebuild`), not a shared metal builder (that
host has been retired), and these audit fixes do not require them.

Unlike the static SOP, this runbook **mutates real infrastructure** (triggers a bake, launches
disposable targets, and can promote canonical). It is therefore **gated, sequential, and
fail-closed**: every mutating action is confirmed, canonical is never touched until a human approves
at the pipeline's Approve stage, and any failed build or failed hardware workload **rejects** the fix.

## Parameters

- **scoreboard_path** (required): Absolute path to the run scoreboard, normally `<repo>/graviton/audit/review/overnight-scoreboard.md` (or `slice-scoreboard.md`). Its "surviving/adjudicated real-bugs" list (plus the diffs in the matching `*-review.md`) is the work queue.
- **repo_root** (required): Absolute path to the DeBeOS checkout (e.g. `/local/home/benfelip/Projects/Haiku-Graviton`).
- **pipeline_name** (optional, default: `haiku-graviton-bake`): the CodePipeline that builds, bakes, and gates promotion. Its source branch and CDK config live with the pipeline; the deploy-branch discipline in `graviton/pipeline/README.md` applies (the pipeline builds from the branch it is pointed at, not necessarily `graviton`).
- **aws_region** (optional, default: `us-west-2`): Region of the pipeline and test fleet.
- **approve_promotion** (optional, default: `false`): MUST stay `false` until a human explicitly approves at the pipeline's Approve stage. No value other than an explicit human "yes" at runtime authorizes advancing the Promote stage.

**Constraints for parameter acquisition:**
- If all required parameters are already provided, You MUST proceed to the Steps
- If any required parameters are missing, You MUST ask for them before proceeding
- When asking for parameters, You MUST request all parameters in a single prompt
- When asking for parameters, You MUST use the exact parameter names as defined

## Steps

### 1. Pre-flight (credentials, tools, clean tree, work queue)
Confirm the environment is ready and read the surviving-real-bug queue.

**Constraints:**
- You MUST run `aws sts get-caller-identity` and confirm it returns an ARN; if it fails, You MUST STOP and ask the user to refresh credentials because every later step needs live AWS access and the propose-only run's STS token has likely expired.
- You MUST confirm the account is the non-production test account and MUST keep to `describe`/`list`/`get-pipeline-state` until a mutating step is explicitly reached and confirmed, per the production-safety rules.
- You MUST `git -C <repo_root> fetch origin` and confirm the local `graviton` is synced with `origin/graviton`; a stale checkout silently regresses the pipeline (per `graviton/pipeline/README.md`). You MUST NOT proceed on a diverged tree without flagging it.
- You MUST parse `scoreboard_path` into the list of **real-bugs** (those that survived refute or were adjudicated `real-bug`). If the list is empty, You MUST report "nothing to prove" and STOP.
- You MUST present the queue (file:line, subsystem, one-line) and the plan before any mutation.

### 2. Assemble the candidate branch (no writes to graviton)
Create one topic branch off synced `graviton` and apply the approved diffs locally — this is the branch the pipeline will build from.

**Constraints:**
- You MUST create a fresh topic branch (e.g. `audit/hw-proof-<date>`) from `origin/graviton`; You MUST NOT commit to, or force, the `graviton` branch, because it is DeBeOS's trunk and work lands only via topic branches.
- You MUST apply the diffs from the run's `*-review.md` for the approved real-bugs, one local commit per finding-cluster, each commit message stating the bug + that it is pending hardware proof.
- You SHOULD batch all approved fixes onto the one branch (one bake covers all) but keep them as separate commits so a failed workload can be attributed and reverted individually.
- If a diff no longer applies cleanly (tree moved since the run), You MUST re-run that finding through `debeos-bugfix.sop.md` rather than force-applying.

### 3. Build + bake through the pipeline (fail-closed; no metal)
Point the `haiku-graviton-bake` pipeline at the candidate branch and let it cross-compile and register a candidate AMI. A fix that does not compile is rejected here by CrossBuild.

**Constraints:**
- You MUST get the pipeline's Source to the candidate branch by the project's established path — push the topic branch and set it as the pipeline source (or merge into the pipeline's designated source branch per `graviton/pipeline/README.md`). Pushing a branch is **mutating**: You MUST confirm with the user first.
- You MUST NOT hand-start any build host (e.g. `haiku-builder3`) to compile these fixes; the pipeline's CrossBuild stage compiles in CodeBuild. Native/KVM (haikuporter) builds run on native Graviton Haiku EC2 instances over SSM and are out of scope for cross-compiled kernel/driver fixes (there is no shared metal builder — that host has been retired).
- You MUST watch the pipeline (`aws codepipeline get-pipeline-state` / execution) and treat a **CrossBuild failure as a rejection**: mark that fix `build-failed`, drop its commit from the branch, and MUST NOT carry it further. Report the failure with the CodeBuild log tail.
- You MUST NOT disable any build check or `-Werror` to force a green build, because that hides the very defect class this audit exists to catch.
- On a green CrossBuild, the Register stage tags the image `candidate=true` (NOT `canonical=true`). You MUST record the candidate AMI id and the pipeline execution id in the run report. You MUST NOT alter the current canonical AMI or its `auto-delete=off`/`auto-stop=no` protection tags.
- The pipeline's Test stage is the throughput **perf-gate**; You MUST note that it does NOT exercise most subsystem fixes (bluetooth/AF_UNIX/BFS/etc.) — that is Step 5.

### 4. Confirm the candidate and hold before promotion
Verify the pipeline produced a candidate and is parked at its manual Approve gate; canonical is untouched.

**Constraints:**
- You MUST confirm via `describe-images`/`get-pipeline-state` that the candidate AMI exists, is tagged `candidate=true`, and that the pipeline execution is waiting at the Approve stage (not auto-promoted).
- You MUST NOT approve the Approve stage yet — subsystem hardware proof (Step 5) comes first, and the human gate is Step 6.

### 5. Boot a disposable target from the candidate and run the subsystem workload
The perf-gate does not exercise these fixes; boot a throwaway target from the candidate AMI and run the workload named in each fix's hardware-verification plan.

**Constraints:**
- Launching/terminating instances is **mutating + billable**: You MUST confirm before `run-instances`/`terminate-instances`, and You MUST tag the target `Project=haiku-graviton` and a clear disposable name (e.g. `hw-proof-<finding>`).
- You MUST NOT boot-test on the currently-canonical production targets or reuse a box that holds state someone else needs; a candidate image is unproven. Use a fresh disposable instance.
- You MUST confirm boot via serial console (`aws ec2 get-console-output --instance-id <id> --latest` — remember `--latest`, a Haiku node emits no lifecycle event) and, once up, reach it with the DeBeOS boot-target user and key recorded in the gitignored `graviton/audit/fleet.local.md`. DeBeOS runs no SSM agent, so these are the only channels.
- You MUST run the workload matched to the subsystem: **BFS** → boot→write→forced-stop→start→verify durability + `checkfs`; **network-stack** → jumbo-frame (MTU 9001) iperf3/`nettput` + route/ARP churn (and, for a lock/race fix, a multi-core socket() storm with concurrent protocol registration); **ENA** → jumbo TX/RX + `ena_fault` injection; **bluetooth/AF_UNIX** → the matching socket smoke; **arm64** → clean boot to userland (metal for GIC/ECAM paths).
- A network throughput test may need a same-subnet peer and one SG ingress rule for the test port; adding an SG rule is **mutating** — You MUST confirm first and You MUST remove it in Step 7.
- You MUST capture objective evidence (numbers, console excerpts, exit codes) and classify each `hw-pass` / `hw-fail`. A `hw-fail` MUST NOT be approved for promotion.

### 6. Verdict and PROMOTION GATE (human approval at the pipeline)
Summarize results and stop for explicit approval before advancing the pipeline's Promote stage.

**Constraints:**
- You MUST present, per fix: the diff, the CrossBuild result, and the hardware evidence, with a clear recommendation (promote / hold / reject).
- You MUST NOT approve the pipeline's Approve stage (which triggers Promote → `haiku-canonical promote`), push to `graviton`, or open a CR unless the user gives an explicit affirmative approval for that specific candidate — `approve_promotion` defaulting to anything other than a live human "yes" MUST be treated as "no", because promotion changes what every future instance boots.
- On approval, You MUST advance the Approve stage (`aws codepipeline put-approval-result`) and let Promote run, then MUST verify the single-canonical invariant (`haiku-canonical check`), that the old canonical was un-canonicaled, and that the new canonical carries `auto-delete=off`/`auto-stop=no` so the reaper cannot take it.
- On approval to land the code, You MAY merge the topic branch to `graviton` via review per `crux-code-reviews`; You MUST NOT bypass review, because trunk changes land through code review, not an automated runbook.

### 7. Teardown and report
Return the fleet to rest and write the run report.

**Constraints:**
- You MUST stop/terminate the disposable test target(s) you started (confirm terminate) and remove any SG rule you added, because idle instances cost money and open rules are exposure — but You MUST NOT touch the canonical AMI, any pipeline/build infrastructure, or any instance you did not start.
- You MUST append a dated entry to `graviton/audit/AUDIT_LOG.md`: which fixes built (CrossBuild pass/fail), hw-pass vs hw-fail, what (if anything) was promoted, the pipeline execution + candidate AMI ids, and open items.
- You MUST write the detailed run report under `graviton/audit/review/` (gitignored) and MUST NOT commit fleet/instance/account IDs to the public repo.

## Examples

### Example 1: One BFS real-bug, promoted through the pipeline
**Input:** scoreboard_path with one BFS fix; repo_root set; approve_promotion `false`.
**Expected Behavior:** pre-flight green → topic branch + apply diff → push + point `haiku-graviton-bake`
at it → CrossBuild green → Register `candidate=true` (candidate AMI id recorded), pipeline parked at
Approve → launch `hw-proof-bfs` from the candidate → boot→write→forced-stop→verify + `checkfs`
= `hw-pass` → present at gate → on human "yes", approve the pipeline stage, Promote runs, invariant
verified → teardown, `AUDIT_LOG.md` updated.

### Example 2: Fix clears the checker but fails CrossBuild
**Input:** a real-bug whose diff breaks the arm64 build.
**Expected Behavior:** Step 3's CrossBuild stage fails, the fix is marked `build-failed`, its commit is
dropped from the branch, the CodeBuild log tail is reported, and it is never registered/booted/promoted.

### Example 3: No survivors
**Input:** scoreboard with zero real-bugs (all false-positive/refuted).
**Expected Behavior:** Step 1 reports "nothing to prove," makes no mutation, and stops.

## Troubleshooting

### `sts get-caller-identity` fails
The STS token expired. Ask the user to refresh creds; do not proceed — every step needs AWS.

### CrossBuild fails in the pipeline
Read the CodeBuild log tail for the failing stage. A build break is a rejection, not something to force
green — drop the offending commit and re-run the pipeline on the reduced branch.

### Pipeline auto-promoted / no Approve gate hit
Stop and investigate before touching canonical: confirm the pipeline definition still has the manual
Approve stage between Test and Promote. If a candidate reached canonical without a human "yes",
`haiku-canonical check` the invariant and report it immediately.

### Boot target has no console output
Ensure `get-console-output` uses `--latest`; a Haiku node generates no lifecycle event so a bare call
returns stale/empty output (see `graviton/docs/ec2-stop-start.md`).

### Network test shows no throughput
Confirm the peer is same-subnet, MTU 9001 on both ends, and the SG ingress rule for the test port was
actually added (and remember to remove it in teardown).
