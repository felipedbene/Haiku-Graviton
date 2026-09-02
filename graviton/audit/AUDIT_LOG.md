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
- Perf-gate Test stage: for promotions to go through the pipeline rather than the manual
  `haiku-canonical` route it needs a throughput peer; it now self-provisions its own ephemeral
  Ubuntu peer per run (no persistent/metal builder required).
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

---

## 2026-08-27 — Round 2: overnight sweep reconciled; tooling v2; disputes adjudicated

**Two runs to reconcile.** The v1 *interactive* workflow (shakedown above: 4 findings, 4/4 FP, **no
adversarial verify at all**) and the v1 *overnight* workflow (138 findings; **2 real bugs**, **4
disputed-FPs**, 131 plain FP → **94.9% FP rate**, 0 refuted). The overnight design was clearly the
stronger one — its prosecutor found every candidate missed bug — and the interactive one had silently
drifted without the verify stage that produced all the signal.

**Tooling v2 (this round).** The two workflows are merged into one `debeos-bugfix.workflow.js`:
- **Verify is always on** in both modes (fixes the interactive-path gap).
- **Prosecutor is weighted** — auto-raised to 2–3 on high-surface (`ena`/`network-stack`) and
  FP-prone/vendored findings, because that is exactly where the overnight missed bugs hid. Refuters
  stay at the baseline (they killed 0 real-bugs — defense was not the weak point).
- A new **Adjudicate** stage: for every disputed-FP a fresh judge reads the *code* plus both
  arguments and rules `real-bug` / `false-positive` / `needs-hardware` — so disputes come back
  **resolved**, not dumped on the human. The v1 overnight workflow file is removed (merged).

**Disputes adjudicated against the source (D1–D4 from the overnight scoreboard):**
- **D1 — `stack.cpp` `family::next` uninit → REAL, root cause corrected.** The prosecutor was right
  that it bites, but mis-located it on the uninitialised member. The real defect: `get_domain_protocols`
  calls `::family::Lookup(socket->family)` **outside** the `sChainLock` block (the `MutexLocker`
  scope closes before it), while family inserts run **under** that lock — an unlocked read of the
  family hash table that races a concurrent insert on ARM64's weak memory model. Uninitialised `next`
  is only the *amplifier* (a stale-but-published node → garbage-pointer deref instead of a benign
  missed lookup). **Fix:** hold `sChainLock` across that `Lookup` (substantive) **and** initialise
  `family::next`/`chain::next` in their constructors (cheap hardening). Needs Graviton HW proof
  (multi-core socket()-storm + concurrent protocol registration).
- **D2 — `pppoe.cpp` deref-before-guard → real but LOW.** `device->SessionID()` is dereferenced at
  the `TRACE` on line 237 *before* the `if (device)` guard on 240, and `sLock` is commented out
  (line 203). Only bites in DEBUG builds (TRACE compiled out in production); PPPoE is not on the
  Graviton exercised surface. Reorder the guard + restore locking; low priority, no HW run.
- **D3 — `RemoteDrawingEngine.cpp` incorrect-rounding → real but COSMETIC.** `(int32)(x + 0.5)` at
  1122/1129 mis-rounds negative source coordinates by 1px (should be `lround`). Remote app_server
  path only; correctness, not a crash. Confirm negative `sourceRect` is reachable past clipping, then
  switch to `lround`.
- **D4 — `TCPEndpoint.cpp:717` knownConditionTrueFalse → NO ACTION.** The prosecutor argued the guard
  is dead code (async loopback delivery), contradicting the deliberate author comment at 715 ("we may
  be in ESTABLISHED already"). Either it is a live loopback fast-path (cppcheck FP, correct as-is) or
  benign dead code — no defect either way. Dismissal upheld; leave the guard.

**Real bugs confirmed in code (both 1-line, static-verified in the overnight run):**
- **`bluetooth_address.cpp:221`** — `bluetooth_print_address()` has an **inverted NULL check** on
  line 214 (`if (addr != NULL)` returns "<invalid>"); the correct sibling
  `bluetooth_print_address_buffer` uses `if (addr == NULL)`. A valid address prints "<invalid>", and a
  NULL `addr` falls through to dereference `addr->b[5]` → KDL. Invert the check. (The original
  "guaranteed KDL" reading was too strong — the path is unreachable on arm64/Graviton; see below.)
- **`UnixDatagramEndpoint.cpp:24`** — the `fShutdownRead:1` bitfield is used at lines 259/297/347 but
  never initialised in the ctor (only `fShutdownWrite` is). A fresh AF_UNIX SOCK_DGRAM socket can
  spuriously return EOF on recv / fail sends before any `shutdown(SHUT_RD)`. Add `fShutdownRead(false)`.

**Merged and hardware-proven (2026-09-02).** All three fixes (bluetooth:221 → #109,
UnixDatagram:24 → #110, and the D1 `stack.cpp` lock → #111) are **merged to `graviton`** — folded
into squash-root `951ceebc28` and byte-identical on tip `987f33ab32` — and therefore already ship in
the canonical AMI (`ami-0dfc6adaacebfb2cf`, `hrev59996`). Each was then verified on real Graviton
(c7g.xlarge, us-west-2) by hot-swapping the affected kernel network add-on via
`/boot/home/config/non-packaged/add-ons/kernel/network/protocols/`, which `kModulePaths` searches
ahead of the packaged tree — so an A/B needs no second AMI bake, only a reboot (and on this platform
an EC2 stop/start: `shutdown -r` halts without re-booting). Proof strength differs per bug and the
original write-ups above overstate two of them:

- **#110 (`UnixDatagramEndpoint.cpp`) — solid A/B, and the miss was real.** Disassembly pins the
  defect to one instruction in the ctor: with the initializer the bitfield byte is masked
  `and w1, w1, #0xfffffffc` (clears both bits); without it, `and w1, w1, #0xfffffffe` (clears
  `fShutdownWrite` only, so `fShutdownRead` is inherited from the allocator). The bug is nonetheless
  **latent on any DeBeOS kernel built at `KDEBUG_LEVEL 2`** (this tree's default), because
  `PARANOID_KERNEL_MALLOC` pre-fills every kernel allocation with `0xcccccccc` and bit 1 of `0xcc` is
  clear: instrumented builds measured `byte_or=0xcc` and `inherited_set=0` over 18,432 endpoint
  constructions, and unpatched repro attempts (2,000 iterations plus 5,120 deliberately heap-poisoned
  probes) were clean. Standing a recycled block in for the paranoid fill (allocate + `memset 0xff`,
  applied identically to both arms) reproduces it outright: buggy ctor **2,000/2,000 sends fail
  `EPIPE`** on a peer that was never shut down (`UnixDatagramEndpoint.cpp:260`), fixed ctor
  **2,000/2,000 clean**, plus 5,120/5,120 poisoned probes clean. So the fix is load-bearing for any
  build with `KDEBUG_LEVEL < 2`, and harmless-but-correct at the current level.
- **#109 (`bluetooth_address.cpp`) — fix-by-inspection; "guaranteed KDL" is wrong on this platform.**
  The bad `print_address` has exactly one live caller, `dump_domains()` in
  `stack/domains.cpp` (a KDL command); every other `AddressString()` site sits inside a `TRACE()`
  macro that release builds compile out (`routes.cpp` ships `//#define TRACE_ROUTES`). Reaching it
  needs a bluetooth route, hence a bluetooth interface, hence bluetooth hardware, which Graviton has
  not. Independently, the canonical AMI ships **no `bluetooth` add-on at all**
  (`/boot/system/add-ons/kernel/network/protocols/` = icmp, icmp6, ipv4, ipv6, tcp, udp, unix), and
  even after force-loading one so `register_domain(10, bluetooth)` succeeds, all 16
  `socket(AF_BLUETOOTH, {STREAM,DGRAM,RAW,SEQPACKET}, {0..3})` combinations return
  `EAFNOSUPPORT` — the add-on registers the domain with a NULL protocol module and no L2CAP/RFCOMM
  chain exists. The defect is real and correctly fixed, but it is unreachable here; it matters for a
  future port with bluetooth hardware, not for the arm64 flagship.
- **#111 (`stack.cpp` unlocked family lookup) — inspection plus a no-regression stress.** A race
  needs no A/B: the lookup is now inside `MutexLocker _(sChainLock)`. Exercised with 16 threads
  churning `socket()`/`close()` across 7 family/type combinations for a 300 s wall-clock window —
  **18,406,190 protocol-chain lookups, 0 rejected, no deadlock, no panic** — plus a shorter 3,674,035-
  lookup run on a separate boot.

A clean boot of the shipped (packaged) add-ons with no overrides was re-verified afterwards on the
same instance: ENA up at MTU 9001, ICMP 3/3 to 1.1.1.1, AF_UNIX datagram 2,000/2,000 clean. Both
test instances were reaped.

One **unrelated one-off KDL** was observed and is *not* attributable to these three bugs:
`PANIC: vm_page_fault ... at 0x20, ip socket_close+0x0c` during `app_server` team teardown
(`thread_exit` → `team_delete_team` → `Team::~Team()`). `FAR=0x20` is the `close` slot of
`net_stack_interface_module_info`, i.e. **`sStackInterface` was NULL**: `socket.cpp`'s
`put_stack_interface_module()` nulls it under a write lock on the `#if KDEBUG` unload path, while
every `fd_ops` entry point (`socket_close`, `socket_read`, `socket_write`, `socket_ioctl`, …) reads
it with no lock and no reference of its own. It did not recur across five subsequent boots on two
instances. Worth its own issue.

**Meta-lesson.** The reconciliation itself found a defect neither automated side got fully right: the
fix-agent dismissed D1, the prosecutor re-opened it but pinned the wrong root cause, and only reading
the lock scopes by hand located the actual unlocked-read race. The adjudicator stage exists to make
that step routine rather than heroic.

### Slice run (2026-08-27, v2 validation, network-stack/bfs/ena, 4 findings)

First run of the unified v2 workflow, on the four highest-value un-mined leads: `arp.cpp:896`
(null `entry`), `BPlusTree.cpp:1458` and `BlockAllocator.cpp:890` (the only static∩upstream
matches), and ENA `ena_eth_com.c:46` (int-mult-as-pointer). **13 agents, 0 errors, ~4.5 min.
Result: 4/4 false-positive, 0 real, 0 disputed.** Independently re-read; all four dismissals
are correct:
- `arp.cpp:896` — the `ARP_GET_ENTRIES` ioctl handler, **not** the L2 send hot path the finding
  claimed; `entry` is provably non-NULL (the `i <= cookie` unsigned loop runs ≥1 pass, each pass
  returns or assigns a non-NULL `iterator.Next()`). The finding also mislocated the code.
- `BPlusTree.cpp:1458` — a legitimate bounds guard rejecting the one remaining OOB value
  (`NumKeys()+1`); `NumKeys()` is an on-disk `uint16` the analyzer can't see, so not always-true.
  It is the *correct* defense against the corruption class behind #11399/#14180, not a bug.
- `BlockAllocator.cpp:890` — cppcheck is factually right it's always-true (complement of the
  line-885 `continue`), but it's a benign redundant guard (2008 upstream), no defect; the #20230
  negative-block tie is unsupported.
- `ena_eth_com.c:46` — u16×u8 index math bounded by queue depth (~1.7e7 ≪ INT_MAX); no overflow.

**Consequence:** the audit's remaining BFS/arp/ENA static leads are **retired**. Combined with the
earlier radix.c retirement, the whole audit's confirmed real bugs are exactly three:
`bluetooth_address.cpp:221`, `UnixDatagramEndpoint.cpp:24`, and the D1 `stack.cpp` unlocked-lookup
race — all three now merged to `graviton` (squash-root `951ceebc28`) and hardware-proven on Graviton;
see "Merged and hardware-proven" above for the per-bug evidence and its varying strength.

**Tooling notes:** v2 ran clean end-to-end and the weighted prosecutor fired (3 prosecutors on the
ENA/arp findings). But because no dismissal was disputed, the new **Adjudicate stage was still not
exercised in practice** — its first real test is a future run that produces a genuine dispute. The
`debeos-hardware-proof.sop.md` runbook was retargeted this round from the hand-driven metal builder
to the `haiku-graviton-bake` CodePipeline (CrossBuild in CodeBuild → Register `candidate=true` →
perf-gate → manual Approve → Promote); native/KVM builds now run on native Graviton Haiku EC2
instances over SSM (`haiku-nativebuild`), not a shared metal builder.
