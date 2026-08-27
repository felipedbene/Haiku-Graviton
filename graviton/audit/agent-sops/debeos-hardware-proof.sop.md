# DeBeOS Hardware-Proof & Promotion (morning runbook)

## Overview

Takes the **surviving real-bugs** from the overnight propose-only run (`overnight-scoreboard.md`) and
drives each proposed diff to **real-Graviton proof** and, only behind an explicit human gate, into the
**canonical AMI**. This is the complement to `debeos-bugfix.sop.md`: that one proves the *checker* is
satisfied and never builds; this one **builds, bakes, boots, and exercises** the fix on hardware —
the step the analysis host cannot do.

Unlike the static SOP, this runbook **mutates real infrastructure** (starts instances, bakes AMIs,
launches targets, and can promote canonical). It is therefore **gated, sequential, and fail-closed**:
every mutating action is confirmed, canonical is never touched until a human approves, and any build
failure or failed hardware workload **rejects** the fix rather than shipping it. Use it in the
morning after the overnight run completes and after credentials are refreshed.

## Parameters

- **scoreboard_path** (required): Absolute path to the overnight scoreboard, normally `<repo>/graviton/audit/review/overnight-scoreboard.md`. Its "surviving real-bugs" list (plus the diffs in `overnight-review.md`) is the work queue.
- **repo_root** (required): Absolute path to the DeBeOS checkout (e.g. `/local/home/benfelip/Haiku-Graviton`).
- **aws_region** (optional, default: `us-west-2`): Region of the test fleet.
- **builder_instance** (optional, default: `haiku-builder3`): the Ubuntu cross-compile host. Its instance ID lives in the gitignored `graviton/audit/fleet.local.md`, not here (public repo).
- **approve_promotion** (optional, default: `false`): MUST stay `false` until a human explicitly approves promotion at the Step-7 gate. No value other than an explicit human "yes" at runtime authorizes `haiku-canonical promote`.

**Constraints for parameter acquisition:**
- If all required parameters are already provided, You MUST proceed to the Steps
- If any required parameters are missing, You MUST ask for them before proceeding
- When asking for parameters, You MUST request all parameters in a single prompt
- When asking for parameters, You MUST use the exact parameter names as defined

## Steps

### 1. Pre-flight (credentials, tools, clean tree, work queue)
Confirm the environment is ready and read the surviving-real-bug queue.

**Constraints:**
- You MUST run `aws sts get-caller-identity` and confirm it returns an ARN; if it fails, You MUST STOP and ask the user to refresh credentials (`source aws-vars.sh` / re-auth) because every later step needs live AWS access and the overnight STS token has likely expired.
- You MUST confirm the account is the non-production test account and MUST keep to `describe`/`list` until a mutating step is explicitly reached and confirmed, per the production-safety rules.
- You MUST `git -C <repo_root> fetch origin` and confirm the local `graviton` is synced with `origin/graviton`; a stale checkout silently regresses the pipeline (per `graviton/pipeline/README.md`). You MUST NOT proceed on a diverged tree without flagging it.
- You MUST parse `scoreboard_path` into the list of **surviving real-bugs** (those that passed adversarial verify). If the list is empty, You MUST report "nothing to prove" and STOP.
- You MUST present the queue (file:line, subsystem, one-line) and the plan before any mutation.

### 2. Assemble the candidate branch (no writes to graviton)
Create one topic branch off synced `graviton` and apply the approved diffs locally.

**Constraints:**
- You MUST create a fresh topic branch (e.g. `audit/hw-proof-<date>`) from `origin/graviton`; You MUST NOT commit to, or push, the `graviton` branch, because it is DeBeOS's trunk and work lands only via topic branches.
- You MUST apply the diffs from `overnight-review.md` for the approved real-bugs, one local commit per finding-cluster, each commit message stating the bug + that it is pending hardware proof.
- You MUST NOT `git push` because nothing is published until a human approves at Step 7.
- You SHOULD batch all approved fixes onto the one branch (one bake covers all) but keep them as separate commits so a failed workload can be attributed and reverted individually.
- If a diff no longer applies cleanly (tree moved overnight), You MUST re-run that finding through `debeos-bugfix.sop.md` rather than force-applying.

### 3. Build on the cross-compile host (first real compile — fail-closed)
Start the builder and cross-compile the affected target(s). A fix that does not compile is rejected here.

**Constraints:**
- Starting `builder_instance` is a **mutating, billable** action: You MUST confirm with the user before `aws ec2 start-instances`, and You MUST record intent to stop it in Step 8.
- You MUST get the topic branch onto the builder (push to a scratch remote / rsync over SSH) and run the Jam build for the affected target(s) (e.g. `jam -q kernel_arm64`, the ENA driver, or `@nightly-anyboot`), capturing output to a log and inspecting the tail — builds are large; do not stream.
- If the build fails, You MUST mark that fix `build-failed`, drop its commit from the branch, and MUST NOT carry it further — static verification passing does not mean it compiles. Report the failure with the log tail.
- You MUST NOT disable any build check or `-Werror` to force a green build, because that hides the very defect class this audit exists to catch.

### 4. Bake a candidate AMI (canonical untouched)
Produce a candidate image from the built branch; never promote here.

**Constraints:**
- You MUST bake via the established path (`graviton/pipeline` CDK bake, or `graviton/builder/make-gpt-image.sh` → register), and You MUST tag the result `candidate=true` (per `pipeline/README.md`), NOT `canonical=true`.
- You MUST NOT run `haiku-canonical promote` in this step, and MUST NOT alter the current canonical AMI or its `auto-delete=off`/`auto-stop=no` protection tags, because canonical is the golden image and only the Step-7 gate may move it.
- You MUST record the candidate AMI id and bake log location in the run report.

### 5. Boot a test target from the candidate
Launch (or re-image) a disposable DeBeOS target and confirm it reaches userland.

**Constraints:**
- Launching/terminating instances is **mutating + billable**: You MUST confirm before `run-instances`/`terminate-instances`, and You MUST tag the target `Project=haiku-graviton` and a clear disposable name (e.g. `hw-proof-<finding>`).
- You MUST NOT boot-test on the currently-canonical production targets or reuse a box that holds state someone else needs, because a candidate image is unproven and could corrupt a known-good box or clobber another operator's work; use a fresh disposable instance.
- You MUST confirm boot via serial console (`aws ec2 get-console-output --instance-id <id> --latest` — remember `--latest`, a Haiku node emits no lifecycle event) and, once up, SSH in with the DeBeOS boot-target user and key recorded in the gitignored `graviton/audit/fleet.local.md`. DeBeOS runs no SSM agent, so these are the only channels.
- If it does not reach userland, You MUST capture the console log, mark the batch `boot-failed`, and STOP before any promotion.

### 6. Run the hardware workload per finding
Exercise each fix with the workload named in its hardware-verification plan; capture pass/fail evidence.

**Constraints:**
- You MUST run the workload matched to the subsystem: **BFS** → boot→write→forced-stop→start→verify durability + `checkfs`; **network-stack** → jumbo-frame (MTU 9001) iperf3/`nettput` against a same-subnet peer + route/ARP churn; **ENA** → jumbo TX/RX + `ena_fault` injection; **arm64** → clean boot to userland (metal for GIC/ECAM paths).
- A network throughput test may need a same-subnet peer and one SG ingress rule for the test port; adding an SG rule is **mutating** — You MUST confirm first and You MUST remove it in Step 8.
- You MUST capture objective evidence (numbers, console excerpts, exit codes) for each fix, and classify each `hw-pass` / `hw-fail`. A `hw-fail` MUST NOT proceed to promotion.
- You SHOULD keep SSH sessions read-only except for the specific workload commands the test requires, on the disposable target only.

### 7. Verdict and PROMOTION GATE (human approval required)
Summarize results and stop for explicit approval before touching canonical.

**Constraints:**
- You MUST present, per fix: the diff, the build result, and the hardware evidence, and a clear recommendation (promote / hold / reject).
- You MUST NOT run `haiku-canonical promote`, push the branch, or open a CR unless the user gives an explicit affirmative approval at this gate for that specific candidate — `approve_promotion` defaulting to anything other than a live human "yes" MUST be treated as "no", because promotion changes what every future instance boots.
- On approval, You MUST `haiku-canonical promote <candidate-ami>` and then MUST verify the single-canonical invariant (`haiku-canonical check`) and that the old canonical was un-canonicaled; You MUST confirm the new canonical carries `auto-delete=off`/`auto-stop=no` so the reaper cannot take it.
- On approval to land the code, You MAY push the topic branch and open a CR per `crux-code-reviews`; You MUST NOT merge to `graviton` directly, because trunk changes land through code review, not an automated runbook.

### 8. Teardown and report
Return the fleet to rest and write the run report.

**Constraints:**
- You MUST stop the builder and stop/terminate the disposable test target(s) you started (confirm terminate), and remove any SG rule you added, because idle instances cost money and open rules are exposure — but You MUST NOT touch the canonical AMI or any instance you did not start.
- You MUST append a dated entry to `graviton/audit/AUDIT_LOG.md`: which fixes were built/tested, hw-pass vs hw-fail vs build-failed, what (if anything) was promoted, and open items.
- You MUST write the detailed run report under `graviton/audit/review/` (gitignored) and MUST NOT commit fleet/instance IDs to the public repo.

## Examples

### Example 1: One surviving BFS real-bug, promoted
**Input:** scoreboard_path with one `hw`-eligible BFS fix; repo_root set; approve_promotion `false`.
**Expected Behavior:** pre-flight green → topic branch + apply diff → build `@nightly-anyboot` on
`haiku-builder3` (confirmed) → bake candidate AMI (`candidate=true`) → launch `hw-proof-bfs` →
boot→write→forced-stop→verify + `checkfs` = `hw-pass` → present at gate → on human "yes",
`haiku-canonical promote`, invariant verified → teardown, `AUDIT_LOG.md` updated.

### Example 2: Fix compiles-clean statically but fails to build
**Input:** a real-bug whose diff breaks the arm64 build.
**Expected Behavior:** Step 3 catches the build failure, marks it `build-failed`, drops it from the
branch, reports the log tail, and never bakes/boots/promotes it.

### Example 3: No survivors
**Input:** scoreboard with zero surviving real-bugs (all false-positive/refuted).
**Expected Behavior:** Step 1 reports "nothing to prove," makes no mutation, and stops.

## Troubleshooting

### `sts get-caller-identity` fails
Overnight STS token expired. Ask the user to refresh (`source aws-vars.sh` or re-auth); do not proceed
— every step needs AWS.

### Diff no longer applies
The tree moved overnight. Re-run that finding through `debeos-bugfix.sop.md` to regenerate the diff
against current HEAD rather than force-applying.

### Boot target has no console output
Ensure `get-console-output` uses `--latest`; a Haiku node generates no lifecycle event so a bare call
returns stale/empty output (see `graviton/docs/ec2-stop-start.md`).

### Network test shows no throughput
Confirm the peer is same-subnet, MTU 9001 on both ends, and the SG ingress rule for the test port was
actually added (and remember to remove it in teardown).
