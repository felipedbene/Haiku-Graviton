# DeBeOS Metal Build-Debug (fast jam iteration)

## Overview

Root-cause a **CrossBuild / `jam` build or package-solve failure on the c9g.metal builder with fast
local iteration**, instead of discovering it one ~4.5-minute bake at a time. This is the "verify it
builds" loop of the feature-cook flow (`graviton/docs/feature-cook-flow.md`): it sits between staging a
change on a topic branch and the gated `debeos-hardware-proof` bake. A package-solve error (e.g.
*"nothing provides lib:X"*, *"would need to uninstall package Y"*) surfaces in the first minutes of a
`jam @minimum-mmc`, so on the metal you can reproduce and fix it in seconds-to-minutes and only bake
once it solves clean.

It **starts the metal builder** (billable) but touches no canonical AMI and commits nothing from the
metal — it is **fail-closed** (stop the metal when done) and produces only a confirmed fix to apply on
a topic branch elsewhere.

## Parameters

- **failure** (required): the build/solve error to reproduce and fix (paste the CrossBuild/CodeBuild log line, e.g. `would need to uninstall package openssl3-3.5.7-1`).
- **branch** (required): the branch whose state to reproduce (`graviton`, or a topic branch).
- **metal_instance** (optional, default from the gitignored `graviton/audit/fleet.local.md`): the c9g.metal builder instance id.
- **hpkg_pool** (optional, default `/opt/haiku/hpkg-out/arm64`): the pool of natively-built hpkgs to stage.

**Constraints for parameter acquisition:**
- If all required parameters are already provided, You MUST proceed to the Steps
- If any required parameters are missing, You MUST ask for them before proceeding
- When asking for parameters, You MUST request all parameters in a single prompt
- When asking for parameters, You MUST use the exact parameter names as defined

## Steps

### 1. Start and reach the metal
**Constraints:**
- Starting the metal is a **mutating, billable** action: You MUST confirm with the user before `aws ec2 start-instances`, and You MUST record intent to stop it in Step 6.
- You MUST wait for `instance-running` and for SSM `PingStatus=Online` (`aws ssm describe-instance-information`), and reach it with **`graviton/scripts/ssm-run <id>`** — the script takes the command **on stdin** (heredoc) and needs `HAIKU_GRAVITON_BUCKET` set for full (untruncated) output; passing the script as an argument yields "empty script". SSH (key `haiku-graviton`, user `ubuntu`) is the fallback.
- You MUST NOT assume `/opt/haiku/haiku` is usable: it is the **upstream `master`** clone (the clone-trap), not `graviton`. Use a graviton tree — `/opt/haiku/haiku-gvt` (branch `graviton`, has a configured `generated.arm64`).

### 2. Reproduce the branch state without clobbering the tree
**Constraints:**
- You MUST `git fetch origin` and reproduce the target `branch`, but You MUST NOT `git reset --hard` / discard a **dirty working tree** — these shared build trees often carry someone's uncommitted WIP. Prefer `git checkout origin/<branch> -- <specific files>` to update only what the fix touches (a package solve depends on `build/jam/repositories/**` and image definitions, not on compiled source), or add a detached `git worktree`.
- You MUST reuse the tree's existing `generated.arm64` cross-tools (symlinked to `/opt/haiku/haiku`); a fresh `configure --build-cross-tools` is ~1 h and unnecessary for a solve failure.

### 3. Set HAIKU_REVISION (the clone-without-tags trap)
**Constraints:**
- You MUST pass `HAIKU_REVISION=hrevNNNNN` (or `-sHAIKU_REVISION=`) to `jam`. These trees have **no git tags**, so `determine_haiku_revision` fails *"you are using a Haiku clone without tags"* and the build aborts **before** reaching the solve you are trying to see. Use the revision the target image is built at.

### 4. Stage local packages properly (not raw cp)
**Constraints:**
- You MUST stage with **`graviton/scripts/haiku-stage-local-packages <generated-dir> [<hpkg-pool-dir>]`**, which derives the wanted files from `build/jam/repositories/HaikuPortsLocal/<arch>` and copies them from the pool into `<generated>/download`. A raw `cp` into `download/` is **not** recognised — jam reports `package <X> not available!` and the dependent build features stay off (this silently changes the failure you observe).
- You MUST ensure the pool (`/opt/haiku/hpkg-out/arm64` or the bake `hpkg-pool`) contains **every** package the `HaikuPortsLocal/<arch>` list names, at the **exact version/revision** — a mismatch is silently skipped. jam prints one line per missing package; read it rather than guessing.
- You MUST remember that `HaikuPortsLocal/<arch>` (local, never fetched) is the correct place for self-built packages — **not** `repositories/HaikuPorts/<arch>`, whose SHA256 selects a *published* remote index (a line there 404s the fetch).

### 5. Run jam and read the solve
**Constraints:**
- You MUST invoke jam as `sudo bash -c 'cd <generated-dir> && HAIKU_REVISION=... jam -q -sHAIKU_IMAGE_SIZE=<n> @minimum-mmc'`. The `cd` **inside** the sudo'd shell sets `PWD` correctly; a bare `sudo jam` strips `PWD`, collapsing `HAIKU_ABSOLUTE_OUTPUT_DIR` and failing on a bogus `.: init-vars` path. You MUST NOT run `jam` via bare `sudo`.
- You MUST capture jam output to a log and `grep` it for the solve conflict (`nothing provides`, `would need to uninstall`, `not available`, `problem [0-9]`); the package-solve fails early, so a modest `timeout` is enough to reach it.
- You SHOULD NOT wait for a full build — the solve verdict is what this SOP is after.

### 6. Iterate the fix, then hand it back; stop the metal
**Constraints:**
- You MUST iterate on the metal (edit `HaikuPortsLocal`/image definition / dependency closure → re-stage → re-jam) until the solve is clean, capturing each verdict. Chase the whole dependency closure: a local package pulls its `requires` (e.g. `openssl3` → `lib:libzstd` → `zstd`) into the local set, and each must be listed + staged.
- You MUST NOT `git commit`/`push` from the metal tree. The confirmed fix is applied on a **topic branch** in the normal checkout → PR → `debeos-hardware-proof` bake.
- You MUST stop the metal (`aws ec2 stop-instances`) when done — it is a large billable instance — and You MUST append the root cause + fix to `graviton/audit/AUDIT_LOG.md`.

## Examples

### Example 1: `nothing provides lib:libzstd` after enabling a build feature
Reproduce on the metal → jam reports the missing provider → add the provider (`zstd`) to
`HaikuPortsLocal/arm64` and stage its hpkg → re-jam → the libzstd error clears (surfacing the next
issue, if any). Fix applied on a topic branch, then baked.

### Example 2: build died before the solve
jam aborts on *"clone without tags"* → the real failure was masked by a missing `HAIKU_REVISION`.
Set it and re-run; the actual solve error appears.

## Troubleshooting

### `ssm-run: empty script`
The script is read from **stdin**; pass it via heredoc (`ssm-run <id> <<'EOF' … EOF`), not as an argument, and export `HAIKU_GRAVITON_BUCKET`.

### `package <X> not available!` for packages you "staged"
You copied into `download/` by hand. Use `haiku-stage-local-packages`, and check the version/revision matches the `HaikuPortsLocal` list exactly.

### jam fails with a `.: init-vars: not found`-style path error
You ran `jam` under bare `sudo`, stripping `PWD`. Use `sudo bash -c 'cd <gen> && jam …'`.

### The tree won't update
It has uncommitted WIP. Do not `reset --hard`; `git checkout origin/<branch> -- <files>` the specific files the solve needs, or use a worktree.
