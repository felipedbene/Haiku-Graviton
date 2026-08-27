# DeBeOS bug-audit log

Dated entries, one per audit round. Publication-clean (no account IDs / internal tool names).

---

## 2026-08-27 — Round 1: static sweep → triage → Phase-4 executor; BFS "corruption" falsified

**Scope reviewed:** whole-tree static sweep (cppcheck `--enable=all` + clang-tidy on the active
surface), Trac cross-reference (BFS + general components), prioritization of the active surface
(kernel/arm64, ENA, network stack, BFS, app_server-remote). Phase-4 fix executor built
(propose-only SOP + workflow). Graviton test fleet explored read-only.

**Numbers:** 23,366 unique findings; 168 active-surface bug-shaped candidates; top 28 in
`PRIORITY.md`. Instrument noise (unknownMacro/syntaxError/internalAstError, ~300) excluded.

**Confirmed still-open static candidates (not yet Phase-4 verified):**
- **network-stack** — `radix.c` `uninitvar m0` / null `rn`, `arp.cpp` null `entry`. Correctness
  defects on the packet hot path; nothing has explained them away. **Now the top deep-pass target.**
- **BFS** — `BPlusTree.cpp:1458` / `BlockAllocator.cpp:890` / `Journal.cpp:793` always-true
  conditions; `Volume::fDevice` uninit member. `BPlusTree.cpp` carries the only static⋂upstream
  matches (open Trac #11399, #14180; allocator #20230) — those remain real and independent.
- **ENA** — `ena_eth_com.c` int-multiply used as a pointer offset (datapath truncation risk).

**Fixed vs deferred:** the audit applied **zero** code fixes (propose-only; no finding has been run
through Phase 4 yet). Everything below the top 28 stays parked in `FINDINGS.md`.

**Headline correction — a lead this audit got wrong.** `PRIORITY.md` originally elevated BFS partly
on an *observed* "volume-wide corruption / page-writer defect under concurrent I/O" (the ROADMAP
Stage-1 open item). On hardware that premise was **falsified**: the acute failure — freshly written
files vanishing (`B_ENTRY_NOT_FOUND`) during a 200+-crate parallel Rust build — was **out of disk
space** on an 8.8 GiB image, not a filesystem/DMA bug. A concurrent-write instrument found **zero**
corruption across cached, pure-`O_DIRECT`/raw-DMA, and rename-churn patterns under memory pressure;
squeezing free space reproduced the exact errno. Fixed by a 20 GiB image bump (now canonical), baked
and validated (real parallel build finished with ~12.5 GiB to spare), promoted with the
single-canonical invariant verified. The NVMe `dsb oshst` barrier was hardware hygiene, not the fix.
Ref: `graviton/docs/arm64-native-build-out-of-space.md`. `PRIORITY.md` banner + BFS re-rank updated
to match.

**Meta-lesson (blog-post-worthy).** Static analysis *and two expert agent workflows* both
over-attributed to a kernel-level BFS/DMA defect what a single measurement pinned to disk space. The
decisive tool was the concurrent-write instrument on the raw-DMA path, not more code reading. This
is exactly the repo's "keep the disproven hypothesis on the record" tradition, and a clean cautionary
tale: an audit's prior is a hypothesis to test on hardware, not a verdict. The reusable guardrail is
already baked into the Phase-4 SOP — "static verification proves the checker is satisfied, NOT that
the code is correct; only a Graviton run does."

**Open items carried forward:**
- `darling` 0.24.1 `rustc` ICE — a narrow, deterministic upstream compiler bug; worth a minimal repro
  only if it ever blocks a real dependency.
- Perf-gate Test stage needs the metal builder running for future promotions to go through the
  pipeline rather than the manual `haiku-canonical` route.
- Phase-4 deep pass on network-stack (radix.c/arp.cpp) not yet run.

**Process note:** the bug-audit tooling now lives in `graviton/audit/` (`FINDINGS.md`, `PRIORITY.md`,
the SOP + workflow, this log). The private fleet map is `graviton/audit/fleet.local.md` (gitignored).

### Shakedown run (2026-08-27, propose-only, network-stack, limit 4)

First live run of the Phase-4 SOP+workflow. 6 agents, 0 errors, ~4 min. Processed the 4 `radix.c`
findings (uninitvar `m0` :641/:645; null `rn` :375/:384). **Result: 4/4 false-positive, 0 real bugs.**
Independently re-verified against the source — both dispositions are **correct** (m0 assigned
unconditionally at line 632; `last` guaranteed non-NULL by the radix treetop invariant). cppcheck
value-flow FPs on vendored 4.4BSD code.

- **What worked:** the SOP does genuine, line-accurate analysis; correct BSD provenance; pinpoints
  cppcheck's side-effect-in-if-condition limitation; propose-only clean; incidental observations
  logged without acting. No hallucinated bugs, no lazy dismissals.
- **What to improve (folds into the overnight build):**
  1. **Cluster near-duplicate findings** — 641/645 are one `m0` issue, 375/384 one `last` issue; the
     run spent 4 agents on 2 clusters. Collapsing them halves the work.
  2. **Down-rank cppcheck `uninitvar`/`nullPointer` on vendored/BSD-derived files** — high FP base
     rate; they crowded out the one distinct lead (`arp.cpp:896`), which never got processed.
  3. Adversarial-verify stage matters most for the findings that come back *real* (BFS BPlusTree,
     ENA) — 0 here, so it was untested this run.
- **Substantive:** `radix.c` cluster retired as FP. Live network-stack lead is now just `arp.cpp:896`.
  Combined with the BFS→ENOSPC correction, the audit's two highest-confidence leads have both
  deflated — a healthy reminder that static-analysis priors are hypotheses, and the flow's value is
  as much in *killing* false leads cheaply as in finding real ones.
