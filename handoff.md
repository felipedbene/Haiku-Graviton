# Handoff — issue drive-down to zero (paused 2026-10-20)

Goal was: drive `felipedbene/Haiku-Graviton` open issues to zero, with PRs for your review.

**Started at 9 open. Now 3 open, each with an open PR that closes it on merge.**
Nothing is merged. Nothing was deployed. No production object was written or deleted.

---

## 1. Where things stand

### Closed, with evidence in a comment on each (6)

These were already fixed in trunk; the work was verifying that honestly and closing them.

| issue | fixed by | what I verified before closing |
|---|---|---|
| #837 overlay misses hyphenated port dirs | PR #840 | Pulled the **deployed** `BuildFn` code and ran its own `_overlay_stage` output under `sh` against a fake tree. All three affected ports (`x16_emulator`, `pkcs11_helper`, `dfu_util`) stage into the hyphenated dir and log `overlay: staging …`. No post-deploy wave had carried these ports, so this stood in for the wave. |
| #838 no spot ICE fallback | PR #841 | Drove the **deployed** `launch_builder.py` with a stubbed EC2 raising `InsufficientInstanceCapacity`: it walks all 6 pools (3 types × 2 subnets), then raises the retryable `BuilderCapacityUnavailable`; buys on-demand only when explicitly enabled. Confirmed the SFN retry is live in `describe-state-machine`. |
| #839 leases stranded `building` | PR #842 | `ReleaseFn` deployed; `ReleaseLeasesOnFailure → ReapOnFailure` live in the deployed graph; **observed running** in the post-deploy waves (8 executions, `released 0 … kept [...]`, which is the conditional correctly declining to touch rows `record.py` had already recorded). |
| #846 nothing ties agent patch to baked hpkg | PR #854 | `haiku-hpkg-provenance-selftest` **PASSes** on trunk; its live arms report `amazon_ssm_agent source unchanged since 1f6d2c4bb10e`. |
| #847 ~65 s Run Command / session | PR #855 | Hardware A/B in the PR: 65.2 s → **1.6 s**, zero `timedout` lines. Also confirmed all four bake pools now hold `-3`. **Caveat stated in the close comment:** canonical `ami-058776e58bd902fba` was baked *before* `-3` landed, so canonical still carries `-2`. |
| #850 blocked fault signal spins forever | PR #853 | `send_fault_signal` present in `origin/graviton:src/system/kernel/signal.cpp` (verified by content — squash-merge makes `--is-ancestor` lie). Hardware A/B complete: all four bug modes SPIN → SIGNAL 21, controls unchanged. |

### Open, each blocked only on your review (3)

| issue | PR | branch | Linux suite |
|---|---|---|---|
| #849 empty claim still launches a builder | **#859** | `fix/849-no-builder-for-empty-claim` | **119/119 pass** |
| #845 stale-driver refusal charged to the port | **#860** | `fix/845-stale-driver-not-a-port-failure` | **121/121 pass** |
| #852 blue publish blocked by un-baselined debt | **#861** | `fix/852-publish-the-wave-delta` | **110/110 pass** |

Each PR body has problem → fix → verification, and each declares `Closes #N` exactly once
(checked with `gh pr view <n> --json closingIssuesReferences` — no accidental prose closes).

---

## 2. The three PRs in brief

### PR #859 — `ops: do not launch a builder for a chain that claimed nothing`
- **State machine:** a `Choice` after `ClaimBatch`. Non-empty `claimed_items` → `LaunchBuilder`;
  empty → `MarkNothingClaimed` (a `Pass` stamping `nothing_claimed`) → `NothingClaimed` (`Succeed`).
  Deliberately *not* the failure path — nothing was claimed or launched.
- **Driver:** `--plan-file` is a snapshot, so `filter_to_queued()` reconciles it against live state
  before planning. `--no-live-filter` opts out.
- **2nd commit addresses a `/code-review` pass on the PR** (5 findings, all real):
  keeps `building` rows whose lease expired (claim.py claims those — dropping them made a
  stranded backlog undrivable), keeps a row with no `build_state`, bounds the
  `UnprocessedKeys` retry with growing backoff, uses `ConsistentRead`, de-duplicates the
  key list, and makes a no-op chain countable instead of reading as `ok`.

### PR #860 — `ops: a stale-driver refusal is not a port failure`
- `run_ssm` emits `VERDICT=STALE` (was `FAIL`); `poll_ssm` → `stale-driver`; `record.py`
  re-queues with **no attempt counted** via a new `_NOT_THE_PORT` map (same shape as #769's
  `capacity`). Unlisted classes are still port failures.
- Adds a driver **pre-flight**: one read of the deployed Lambda's expected
  `NATIVEBUILD_VERSION` vs the published driver, refusing to drive on a mismatch, instead of
  96 refusals on 11 booted builders. Finds the build task by walking the graph, not by name.
  Best-effort on the read, strict on the comparison. An **absent** driver is a mismatch
  (2nd commit — I caught that myself; the first cut waved the worst case through).
- Operator role gains `states:DescribeStateMachine` + `lambda:GetFunctionConfiguration`,
  resource-scoped, describe-only.
- SOP §13.1b: a deploy touching the driver and a `haiku-publish-driver` are one operation.

### PR #861 — `ops: baseline the debt that blocked every blue publish`
- **Baseline only — no change to the publish path or the gate's logic.** The baseline was cut
  against green; blue carries its own bootstrap-era debt, so every blue publish refused.
- Five keys added, all read back from the real packages. `icu67_tools` is called out as a
  **recipe defect**, not a missing base: I read `icu67-67.1-1-arm64.hpkg` and it provides
  `icu67` / `lib:libicu*`, never `icu`, so no publish of any icu can satisfy that pin.
- **Per your call, the two harvest orphans (`ffmpeg7_devel`, `icu78_devel`) are baselined and
  published as accepted debt**, so the other 140 harvested packages are not blocked behind
  them. I did *not* scope the incoming snapshot to the run's delta (the issue's other
  suggestion) because those 142 are legitimate pending output and per-run scoping would
  strand them.
- Verified against the **real union** (blue's 3,189 + 140 incoming, read-only sync):
  trunk's baseline → exit 1 with exactly the issue's 5 `INCOHERENT` lines (so those 5 were the
  complete blocking set); this branch → **exit 0, 8 known-broken, 0 new, 0 stale**. Posted as
  a comment on the PR.

---

## 3. What I did NOT do

- **Nothing merged, nothing deployed.** All three PRs need a
  `cdk deploy DebeosOpsStack` after merge to take effect — and per your memory note that
  needs **both** `-c onPremPublicIp=<live CGW ip, read from describe-customer-gateways>` and
  `-c onPremSgCidr=10.0.0.0/8`. #861 also needs a deploy because the baseline is a
  content-hashed CDK asset.
- **`/code-review` ran on #859 only.** #860 and #861 have not had an adversarial review pass.
  That is the most useful next step if you want one before reading them.
- **No bake, no AMI promote, no S3 writes, no DynamoDB writes.**

## 4. Follow-ups worth tracking (not filed as issues — say the word and I will)

1. **#847 rollout:** canonical still carries SSM agent `-2`. One bake → validate on a fresh
   `c7g.large` → `haiku-canonical promote` ships the 65 s → 1.6 s fix. Pools are already staged
   with `-3`, and #854's `check-pool` will verify provenance.
2. **#845 aftermath:** the 63 ports wave `wd-10191705` pushed to `needs_human` /
   `repeated-failure` still carry an attempt they did not earn. PR #860 stops it recurring but
   does **not** repair those rows — that is a live state-table repair and your call.
3. **#852 debt:** blue's 3 orphans stay in the pool (you chose baseline over deleting; deleting
   is a destructive write to the live repo and yours to run). Clearing any of the 5 means
   building the pinned base, or fixing the `icu67` recipe, then deleting the baseline line —
   `--strict-baseline` will report it STALE once it stops reproducing.
4. **#838 residual:** the builder AMI has still never booted on `c7g.2xlarge` / `m8g.2xlarge`.
   The first real fallback launch is that check — watch `instance_type` and SSM Online.
5. **Incidental, unverified** (in the PR bodies, deliberately not issues): blue's `repo` index
   object is dated 2026-09-24 while `packages/` holds 3,189 hpkgs; the harvest's `stage1/`,
   `superseded-no-simd/`, `quarantine-wrong-arch/` prefixes vs the snapshot's `--include`.

## 5. Environment notes before you resume

- **This machine:** on branch `fix/849-no-builder-for-empty-claim`. The 34 uncommitted files
  (`README.md`, the Screenshot `.catkeys`, `CollatorTest.cpp`) were **already dirty when I
  started** — I never touched or staged them. Each of my commits staged only its own files.
- **Your dev desktop** (`benfelip-dskarm`) is on a **detached HEAD at `452f8230e9`** because I
  ran the test suite there (the 9 `haiku-repo-add` jest cases cannot run on macOS — they need
  the Linux `package`/`package_repo` host tools). `git stash list` is empty and `git status` is
  clean, so nothing was lost, but you'll want `git checkout graviton` there.
- **Scratch to delete:** `/tmp/union852` is **~12 GB** (the blue pool sync). Also
  `/tmp/coh852`, `/tmp/lamcheck`, `/tmp/dsk-memory-incoming`, `/tmp/baseline852.txt`.
- **`graviton/pipeline/node_modules`** was installed on this machine with node 22
  (`~/.nvm/versions/node/v22.18.0/bin`). The repo's deps need node ≥20; the system node here is
  18, so prefix `PATH` with that nvm bin to run `npx jest` / `npx tsc`.
- **Memory:** I copied the 10 memory files from your desktop into
  `~/.claude/projects/-Users-benfelip-Projects-Haiku-Graviton/memory/` as asked. The
  branch-off-trunk, closing-keyword and ops-stack-deploy ones shaped the work above.
- On macOS the suite shows **9 failures on every branch including `origin/graviton`** — all
  `haiku-repo-add` cases, environmental. The Linux numbers in the table are the real ones.

## 6. Fastest way to resume

```bash
gh pr list --state open                      # 859, 860, 861
gh issue list --state open                   # 845, 849, 852 — close on merge
/code-review 860 high                        # not yet reviewed
/code-review 861 high                        # not yet reviewed
```
