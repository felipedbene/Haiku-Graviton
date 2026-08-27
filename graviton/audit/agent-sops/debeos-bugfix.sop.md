# DeBeOS Static-Analysis Bug-Fix

## Overview

Confirms a single static-analysis finding from `PRIORITY.md`, root-causes it, and — if it is a
real bug — produces a **proposed** minimal fix as a unified diff, statically self-verified, with a
commit message and a Graviton hardware-verification plan.

This SOP is **propose-only**. It never modifies the DeBeOS working tree, never commits, and never
runs a build (there is no `aarch64-haiku` cross-toolchain on the analysis host). All editing and
verification happen inside a throwaway `git worktree` that is deleted before the SOP returns; the
only durable output is a report file. Use it as the per-finding unit of work behind the Phase-4
bug-fix workflow, or standalone on one finding.

## Parameters

- **finding** (required): One finding to process, as a single line or object with: `file` (repo-relative path), `line` (integer), `checker` (cppcheck/clang-tidy id, e.g. `uninitvar` or `bugprone-assignment-in-if-condition`), `subsystem` (one of `ena`, `network-stack`, `bfs`, `app_server-remote`, `kernel/arm64`), and `description` (the finding message). Example: `bfs/Journal.cpp:793 | knownConditionTrueFalse | bfs | LogEntryLength() > FreeLogBlocks() is always true`.
- **repo_root** (required): Absolute path to the DeBeOS git checkout (e.g. `/local/home/benfelip/Haiku-Graviton`).
- **output_file** (required): Absolute path where this run MUST write its JSON report (the orchestrator assigns a distinct path per finding).
- **scratch_root** (optional, default: `/tmp/debeos-bugfix`): Absolute path under which the isolated worktree is created and removed.

**Constraints for parameter acquisition:**
- If all required parameters are already provided, You MUST proceed to the Steps
- If any required parameters are missing, You MUST ask for them before proceeding
- When asking for parameters, You MUST request all parameters in a single prompt
- When asking for parameters, You MUST use the exact parameter names as defined

## Steps

### 1. Parse the finding and gate on the active surface
Parse `finding` into `file`, `line`, `checker`, `subsystem`, `description`.

**Constraints:**
- You MUST reject and STOP (writing a report with `classification: "out-of-scope"`) if `file` is not under one of the active-surface roots — `src/system/kernel/arch/arm64/`, `src/add-ons/kernel/drivers/network/ether/ena/`, `src/add-ons/kernel/network/`, `src/add-ons/kernel/file_systems/bfs/`, `src/servers/app/drawing/interface/remote/` — because touching subsystems outside DeBeOS's exercised surface is explicitly out of scope for this audit.
- You MUST process exactly one finding per run because mixing findings into one diff violates the one-finding-cluster-per-commit rule.
- You MUST confirm `file` exists under `repo_root` before proceeding.

### 2. Create an isolated scratch worktree
Create a detached `git worktree` from `HEAD` so all edits and analysis run on a disposable copy.

**Constraints:**
- You MUST run `git -C <repo_root> worktree add --detach <scratch_root>/<unique-id> HEAD` to create the worktree, using a unique id (derive it from the file path + line so concurrent runs never collide).
- You MUST NOT edit, stage, or commit anything under `repo_root` itself because this SOP is propose-only and MUST leave the real working tree untouched.
- You MUST use `--detach` because a detached worktree has no branch, guaranteeing the `graviton` branch is never checked out or modified.
- You MUST NOT run `git commit`, `git push`, `git checkout graviton`, or `git branch` anywhere because the human review gate — not this SOP — decides what lands.
- If `git worktree add` fails, You MUST STOP and write a report with `classification: "error"` and the git error text.

### 3. Confirm the finding and root-cause it
In the worktree, read `file` around `line` (with enough surrounding context — the whole function and its callers as needed) and determine whether the finding is a real defect.

**Constraints:**
- You MUST read the actual code, not rely on the finding's `description` alone, because static-analysis messages are frequently false positives (this repo's own notes treat unconfirmed tool output as noise until proven).
- You MUST classify the finding as exactly one of: `real-bug` (a definite defect), `false-positive` (the code is correct; the checker is wrong), or `needs-hardware` (plausibly real but only a Graviton run can decide).
- You MUST record a one-paragraph root-cause explanation citing the specific lines and control flow.
- You SHOULD check the git blame / surrounding commits for context when the code looks deliberately unusual, because much of this tree is vendored or BSD-derived and may be intentional.
- If the classification is `false-positive` or `needs-hardware`, You MUST skip Steps 4–5, write the report, and go to Step 6.

### 4. Produce a minimal fix (real-bug only)
Edit the file in the worktree to fix the root cause with the smallest change that is correct.

**Constraints:**
- You MUST make the minimal change that addresses the root cause, not a stylistic rewrite, because a small diff is reviewable and a large one hides scope creep.
- You MUST match the surrounding code style (tabs, width 4; explain *why* not *what* in comments) per the DeBeOS coding guidelines.
- You MUST NOT change unrelated lines, reformat the file, or fix other findings you notice — log those separately in the report's `incidental_observations` field instead, because one finding per commit is a hard rule.
- You MUST capture the fix as a unified diff via `git -C <scratch_root>/<unique-id> diff -- <file>` and store the exact diff text in the report.

### 5. Statically self-verify (real-bug only)
Re-run the analyzers on the single file, before and after the fix, and prove the finding clears without introducing new ones.

**Constraints:**
- You MUST run the same checker that produced the finding: for a cppcheck id, `cppcheck --enable=all -D__aarch64__ -D__HAIKU__ -I <repo_root>/headers -I <repo_root>/headers/private -I <repo_root>/headers/os -I <repo_root>/headers/posix <file>`; for a clang-tidy id, `clang-tidy --checks='-*,<checker>,clang-analyzer-*,bugprone-*' <file> -- <Haiku include set> -D__HAIKU__ -D_KERNEL_MODE -D__aarch64__ -x c++ -std=c++17`.
- You MUST confirm the specific finding at (or near) `line` is present BEFORE the fix and absent AFTER the fix, and record both raw tool outputs in the report.
- You MUST confirm the fix introduces no new findings of equal or higher severity on that file; if it does, You MUST revert the edit and re-classify as `needs-rework` with an explanation.
- You MUST label this verification as STATIC-ONLY in the report because it proves the checker is satisfied, NOT that the code builds, boots, or behaves correctly — only a Graviton run does that.

### 6. Emit the report and destroy the worktree
Write the JSON report to `output_file`, then remove the scratch worktree.

**Constraints:**
- You MUST write a JSON object to `output_file` with these fields: `finding` (echoed input), `classification`, `root_cause`, `diff` (unified diff string, empty unless `real-bug`), `static_verification` (object with `before`, `after`, `finding_cleared` boolean, `new_findings` list), `commit_message` (empty unless `real-bug`), `hardware_verification_plan`, `incidental_observations` (list), and `confidence` (`high`/`medium`/`low`).
- For a `real-bug`, the `commit_message` MUST follow the house style — an imperative subject prefixed by subsystem (e.g. `bfs: ...`, `ena: ...`, `arm64: ...`), a body stating the bug and the fix, and a line stating it was static-verified with the checker and still needs the hardware plan run.
- The `hardware_verification_plan` MUST name the concrete Graviton workload that would confirm the fix (e.g. BFS → boot→write→forced-stop→verify durability harness + `checkfs`; network → jumbo-frame iperf3 + route/ARP churn; ENA → jumbo TX/RX + `ena_fault` injection) because a static pass is not proof.
- You MUST run `git -C <repo_root> worktree remove --force <scratch_root>/<unique-id>` after writing the report, because leaving worktrees behind pollutes the repo's worktree list.
- You MUST NOT print the full diff or tool output to the response; You MUST return only a one-line summary (classification + file:line + confidence) because the orchestrator aggregates from the report file, not the response.

## Examples

### Example 1: Real bug (BFS always-true condition)
**Input:**
- finding: `bfs/Journal.cpp:793 | knownConditionTrueFalse | bfs | LogEntryLength() > FreeLogBlocks() is always true`
- repo_root: `/local/home/benfelip/Haiku-Graviton`
- output_file: `/tmp/debeos-bugfix/reports/bfs-Journal-793.json`

**Expected Behavior:**
Worktree created; code confirms the comparison can never be false; minimal fix corrects the
operands/types; cppcheck on the file shows `knownConditionTrueFalse` at ~793 present before, absent
after, no new findings; report carries the diff, a `bfs: ...` commit message, and a
boot→write→forced-stop durability plan. Response: `real-bug | bfs/Journal.cpp:793 | high`.

### Example 2: False positive (vendored ENA)
**Input:**
- finding: `ena/ena-com/ena_com.c:917 | bugprone-assignment-in-if-condition | ena | assignment within an 'if' condition`
- repo_root: `/local/home/benfelip/Haiku-Graviton`
- output_file: `/tmp/debeos-bugfix/reports/ena-ena_com-917.json`

**Expected Behavior:**
Code review shows the assignment-in-if is an intentional, upstream-shaped idiom in the vendored
`ena-com/` layer; classified `false-positive`; no diff; report explains why and notes the
keep-close-to-upstream constraint. Response: `false-positive | ena/ena-com/ena_com.c:917 | high`.

### Example 3: Out of scope
**Input:**
- finding: `src/apps/deskbar/BarView.cpp:200 | uninitvar | other | ...`

**Expected Behavior:**
Step 1 rejects it (`classification: "out-of-scope"`) because the path is outside the active
surface; no worktree is created. Response: `out-of-scope | src/apps/deskbar/BarView.cpp:200 | n/a`.

## Troubleshooting

### The analyzers are not installed
If `cppcheck` or `clang-tidy` is missing, Step 5 cannot run. Install them
(`sudo dnf install -y cppcheck clang clang-tools-extra`) before dispatching, or the run MUST
classify as `error` — a fix without static verification MUST NOT be reported as `real-bug`.

### clang-tidy reports "file not found" on a header
Kernel/arch files need Haiku's full include set. Add every subdir of `headers/os` and
`headers/private` as `-I`, plus `-D_KERNEL_MODE -D__aarch64__`. If a kernel-only header still
cannot be resolved, note the degraded parse in `static_verification` and lower `confidence`; do NOT
report a `real-bug` whose fix could not be verified.

### git worktree add fails with "already exists"
The unique id collided. Derive it from `<subsystem>-<basename>-<line>` plus a per-run suffix, and
ensure Step 6 removed prior worktrees (`git -C <repo_root> worktree prune`).

### The finding is real but the fix is non-obvious or risky
Classify `needs-hardware` (or `needs-rework` if an attempted fix regressed the analyzers), explain
the risk, and let the human decide — this SOP MUST NOT guess at fixes for code it cannot build.
