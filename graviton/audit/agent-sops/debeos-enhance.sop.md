# DeBeOS Enhancement Implementation

## Overview

Takes a single **curated enhancement** from `ENHANCEMENTS.md`, decides whether it is worth doing and
feasible as one bounded change, and — if so — implements it as a **complete unified diff**, statically
self-checked, with a commit message and a Graviton hardware-verification plan.

This is the enhancement counterpart to `debeos-bugfix.sop.md`. The key differences: an enhancement is
a *chosen* improvement, not a static-analysis finding that might be noise — so there is no
"false-positive" classification and no adversarial FP hunt. The output is a **full diff** (not a
minimal one-line fix), because an enhancement adds capability. Everything else matches the bug-fix
SOP's discipline: **propose-only** — it never modifies the DeBeOS working tree, never commits, and
never builds (there is no `aarch64-haiku` cross-toolchain here). All work happens in a throwaway
`git worktree` deleted before the SOP returns; the only durable output is a report file.

## Parameters

- **enhancement** (required): One enhancement to implement, as an object or line with: `id` (backlog id, e.g. `E3`), `title`, `subsystem` (one of `ena`, `network-stack`, `bfs`, `app_server-remote`, `kernel/arm64`), `description` (what to build), `rationale` (why it's worth it), and `acceptance` (how you know it's done — the observable behaviour the hardware plan must show).
- **repo_root** (required): Absolute path to the DeBeOS git checkout (e.g. `/local/home/benfelip/Projects/Haiku-Graviton`).
- **output_file** (required): Absolute path where this run MUST write its JSON report (the orchestrator assigns a distinct path per enhancement).
- **scratch_root** (optional, default: `/tmp/debeos-enhance`): Absolute path under which the isolated worktree is created and removed.

**Constraints for parameter acquisition:**
- If all required parameters are already provided, You MUST proceed to the Steps
- If any required parameters are missing, You MUST ask for them before proceeding
- When asking for parameters, You MUST request all parameters in a single prompt
- When asking for parameters, You MUST use the exact parameter names as defined

## Steps

### 1. Parse the enhancement and gate on the active surface
Parse `enhancement` into `id`, `title`, `subsystem`, `description`, `rationale`, `acceptance`.

**Constraints:**
- You MUST reject and STOP (writing a report with `classification: "out-of-scope"`) if the enhancement's target files are not under one of the active-surface roots — `src/system/kernel/arch/arm64/`, `src/add-ons/kernel/drivers/network/ether/ena/`, `src/add-ons/kernel/network/`, `src/add-ons/kernel/file_systems/bfs/`, `src/servers/app/drawing/interface/remote/` — because DeBeOS's exercised surface is where an enhancement can be proven on hardware.
- You MUST process exactly one enhancement per run, because mixing enhancements into one diff violates the one-change-per-commit rule.

### 2. Create an isolated scratch worktree
Create a detached `git worktree` from `HEAD` so all edits and analysis run on a disposable copy.

**Constraints:**
- You MUST run `git -C <repo_root> worktree add --detach <scratch_root>/<unique-id> HEAD` using a unique id derived from the enhancement id + subsystem so concurrent runs never collide.
- You MUST NOT edit, stage, or commit anything under `repo_root` itself, and MUST use `--detach` so the `graviton` branch is never checked out.
- You MUST NOT run `git commit`, `git push`, `git checkout graviton`, or `git branch` anywhere — the human review gate decides what lands.
- If `git worktree add` fails, You MUST STOP and write a report with `classification: "error"` and the git error text.

### 3. Assess worth and feasibility
In the worktree, read the target code and its surroundings and decide whether this enhancement is worth doing AND implementable as one bounded, reviewable diff.

**Constraints:**
- You MUST read the actual code the enhancement touches (the functions, their callers, the data structures) before designing, because an enhancement that fights the existing architecture is worse than none.
- You MUST classify the enhancement as exactly one of: `implemented` (worth it and done as a full diff below), `needs-decomposition` (worth it but too large/architectural for one reviewable diff — you MUST propose a split into smaller enhancements instead of forcing a mega-diff), `reject` (not worth the complexity, redundant, or architecturally wrong — say why), `out-of-scope`, or `error`.
- You MUST record a one-paragraph assessment: what it changes, why it is worth the added complexity, and the main risk.
- For `needs-decomposition` or `reject`, You MUST skip Steps 4–5, write the report (with the proposed split or the rejection rationale), and go to Step 6. Forcing a diff you cannot make cohesive is worse than reporting that it needs breaking down.

### 4. Implement the enhancement as a full unified diff (`implemented` only)
Edit the files in the worktree to implement the enhancement completely against its acceptance criteria.

**Constraints:**
- You MUST implement the whole enhancement (not a partial stub) such that its `acceptance` criteria could be met, but You MUST NOT add capability beyond what the enhancement specifies — scope creep is the primary failure mode here, and the skeptic reviewer will attack it.
- You MUST match the surrounding code style (tabs, width 4; explain *why* not *what* in comments) per the DeBeOS coding guidelines, and keep any vendored layer (e.g. `ena-com/`) close to its upstream shape, putting DeBeOS-specific glue in the platform files.
- You MUST NOT change unrelated code or fix bugs you notice — log those in `incidental_observations` instead, because one enhancement per commit is a hard rule.
- You MUST capture the change as a unified diff via `git -C <scratch_root>/<unique-id> diff` and store the exact diff text in the report.

### 5. Statically self-check (`implemented` only)
Run the analyzers on the touched files and prove the change introduces no new defects the tools can see.

**Constraints:**
- You MUST run `cppcheck --enable=all -D__aarch64__ -D__HAIKU__ -I <repo_root>/headers -I <repo_root>/headers/private -I <repo_root>/headers/os -I <repo_root>/headers/posix <touched files>` and, for C++ kernel/arch files, `clang-tidy --checks='-*,clang-analyzer-*,bugprone-*' <file> -- <Haiku include set> -D__HAIKU__ -D_KERNEL_MODE -D__aarch64__ -x c++ -std=c++17`.
- You MUST confirm the change introduces no new findings of medium-or-higher severity on the touched files, recording the raw before/after tool output. If it does, You MUST revise or revert and re-classify `needs-decomposition` with the explanation.
- You MUST label this as STATIC-ONLY: it proves the analyzers are satisfied, NOT that the code builds, boots, or delivers the enhancement — only the Graviton hardware plan does.

### 6. Emit the report and destroy the worktree
Write the JSON report to `output_file`, then remove the scratch worktree.

**Constraints:**
- You MUST write a JSON object to `output_file` with: `enhancement` (echoed input), `classification`, `assessment`, `diff` (unified diff string, empty unless `implemented`), `static_check` (object with `before`, `after`, `new_findings` list), `commit_message` (empty unless `implemented`), `hardware_verification_plan`, `risks` (list of the main risks a skeptic would raise), `proposed_split` (list, only for `needs-decomposition`), `incidental_observations` (list), and `confidence` (`high`/`medium`/`low`).
- For an `implemented` enhancement, the `commit_message` MUST follow the house style — an imperative subject prefixed by subsystem (e.g. `ena: ...`, `net/stack: ...`, `arm64: ...`), a body stating what capability is added and how, and a line noting it is static-checked and still needs the hardware plan run.
- The `hardware_verification_plan` MUST name the concrete Graviton workload that demonstrates the `acceptance` criteria (e.g. ENA multiqueue → per-queue IRQ distribution under parallel iperf3 across N flows; network throughput → jumbo iperf3 + the specific metric that must improve; BFS → the durability/throughput workload that shows the gain), because an enhancement is proven by the capability it delivers, not by compiling.
- You MUST run `git -C <repo_root> worktree remove --force <scratch_root>/<unique-id>` after writing the report.
- You MUST return only a one-line summary (classification + id + subsystem + confidence); the orchestrator aggregates from the report file.

## Examples

### Example 1: Implemented (ENA adaptive interrupt moderation)
**Input:** enhancement `E1 | ena | adaptive interrupt moderation | tie the moderation interval to observed packet rate | rationale: cut IRQ overhead at high pps without adding latency at low pps | acceptance: IRQ/s drops under a sustained high-pps iperf3 with no throughput loss`.
**Expected Behavior:** worktree created; the datapath and existing static-moderation code read; a bounded diff adds the adaptive interval; cppcheck/clang-tidy on the touched files show no new findings; report carries the full diff, an `ena: ...` commit message, and a per-queue IRQ-rate-under-iperf3 hardware plan. Response: `implemented | E1 | ena | high`.

### Example 2: Needs decomposition (ENA multiqueue RX/TX)
**Input:** an enhancement to add full multi-queue RX/TX to the ENA driver.
**Expected Behavior:** Step 3 finds it spans IRQ affinity (arm64), per-queue datapath, and admin-queue changes — too large for one reviewable diff. Classified `needs-decomposition`, no diff; report proposes a split (e.g. arm64 IRQ-affinity fix first, then per-queue datapath, then queue-count negotiation) with the dependency order. Response: `needs-decomposition | E4 | ena | medium`.

### Example 3: Reject (over-engineering)
**Input:** an enhancement adding a configurable pluggable-policy framework where a two-line constant would do.
**Expected Behavior:** Step 3 classifies `reject` with the YAGNI rationale and the simpler alternative. Response: `reject | E7 | network-stack | high`.

## Troubleshooting

### The analyzers are not installed
Install `cppcheck clang clang-tools-extra` before dispatching, or Step 5 cannot run and the enhancement MUST classify `error` — an `implemented` diff MUST NOT be reported without a static check.

### The enhancement has no clear acceptance criterion
If `acceptance` is missing or untestable, You MUST NOT invent a success story. Classify `needs-decomposition` and note that the backlog entry needs a measurable acceptance criterion before it can be proven on hardware.

### The change keeps growing
If the diff sprawls across many files or subsystems, that is the signal to STOP and classify `needs-decomposition` with a proposed split — a large diff hides scope creep and cannot be reviewed or attributed.
