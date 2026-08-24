# SMP work is placed badly and never repaired

Status: **investigation; instrumentation written, awaiting a bake. Design NOT
final** — one measurement decides its shape, and that measurement has not been
taken yet. Verified against the tree at `a195688e46`.

This is **upstream Haiku behaviour, not a Graviton regression.** Nothing in the
arm64 port changed the predicates involved; the absence of SMT merely removed a
divisor that was hiding one of them. It costs a large fraction of a many-core
non-SMT machine.

---

## 1. What is actually measured

Two independent measurement rounds on 16-vCPU `c7g.4xlarge` instances booted from
the canonical AMI `ami-0d61e3910062bb80a` (`hrev59996`), using
`src/bin/smpscale`, which gives every thread an identical fixed work unit.
Binary verified by artifact: matching `sha256` on build host, desktop and node,
and the hot loop confirmed in `objdump` to be a register-bound `madd` /
`eor …, lsr #29` dependency chain with zero memory operands.

### 1.1 The defect is far larger than "0.500 at N = 16"

The original framing — perfect scaling to N = 15, then exactly 0.500 at
N = 16 — is **wrong in both directions**, and the error was caused by the metric.

- **It starts at N = 8 on a 16-CPU machine, with 8 CPUs completely idle.**
  N = 8 failed 5 of 6 runs; N = 12 failed 4 of 6.
- **Severity exceeds 2x.** Observed max/min over busy CPUs reaches **3.0 and
  4.03** — three and four saturated threads stacked on one core.
- **Up to 9 of 16 CPUs idle**, i.e. more than half the machine wasted.
- **N = 16 is not deterministic.** It was perfect in 2 of 6 runs in one round and
  2 of 4 in another. The reported "8/8" does not reproduce. Any A/B therefore
  needs enough repeats to beat a coin toss.
- N = 20 concentrated **all five** surplus threads on one CPU (15019 ms against
  an ideal 6000 ms), and N = 32 put roughly **17 of 32** threads on cpu9
  (51249 ms vs an ideal 6000 ms, **8.5x**) with `cpus>10%` collapsing to 2 —
  fourteen CPUs idled for 48 seconds beside a 16-deep queue.

### 1.2 Why the ladder hid it: `eff` saturates

`smpscale`'s `eff` is `baseWall / wallMs`, driven by the **slowest** thread. One
doubled core pins it to 0.500 whether one core is doubled or five, and whether
one CPU is idle or nine. It is a floor indicator, not a severity measure, and it
understated a defect that can waste 56 % of the machine. Worse, in a
single-point run `baseWall` comes from the only row, so `eff` is trivially
1.000 and means nothing — several figures quoted early in this investigation were
that artifact.

The honest measures, now emitted by the tool, are **per-CPU busy sets** (how many
CPUs did a full unit of work, how many idled, which ones) and **max/min over the
busy CPUs**, which does not saturate. Also note `eff`'s optimum is
`1 / ceil(N / ncpus)`, so it *should* fall as N passes `ncpus`: at N = 17, 0.500
is already optimal and "improving" it would mean something is wrong.

### 1.3 The imbalance is created at placement and frozen — the causation is the
opposite of the original story

This is the finding that reshapes the project, and it is strong. Sampling
per-CPU state at 50 ms across 12 runs, ~37 intervals each:

- The set of slow threads is **identical in the first sample and in every
  later sample, in all 12 runs** — `rows_differing_from_first = 0` every time.
- In a 6 s N = 16 run, CPUs 5, 8 and 12 read 0 % in **all 23** intervals while
  cpu13 read 100 %.
- So there are **zero migrations, in either direction**, and the imbalance
  predates the first sample: it is established during the spawn burst.

Therefore `rebalance()` is **the mechanism that fails to repair, not the
mechanism that creates.** Everything in §3 about its arithmetic is still true and
still a defect — but fixing it alone would produce a *repair* of a bad placement,
not a good placement.

**The consequence that must be designed around:** fixing `rebalance()` alone
makes the ladder read 1.000 while every thread-pool startup still collides and is
now unwound by a migration storm, paying cache and TLB cost. Fixing placement
alone *also* makes the ladder read 1.000 while `rebalance()` stays dead. **The
efficiency ladder cannot distinguish these two**, which is precisely why the
acceptance criteria in §6 are stated in busy sets and migration counts.

### 1.4 The likely root cause, and it is cheap to test

**1 ms of spawn stagger eliminates the defect entirely.** 0 ms gives 11 and 15
busy CPUs with max/min 2.010 / 2.003; 1 ms gives 16 and 16 with 1.010 / 1.008.
The threshold is ~1 ms, which is **exactly `kLoadMeasureInterval`**
(`load_tracking.h:13`, 1000 µs).

Proposed mechanism, each part verified in the tree:

1. `choose_core()` (`low_latency.cpp:47`) tries the package idle-core list, then
   falls through to `gCoreLoadHeap.PeekMinimum()`, then
   `gCoreHighLoadHeap.PeekMinimum()`.
2. Heap keys are refreshed by `CoreEntry::_UpdateLoad()` only once per
   `kLoadMeasureInterval` unless forced (`scheduler_cpu.cpp:545`).
3. A new thread's load is **inherited from its parent**:
   `fNeededLoad = currentThreadData->fNeededLoad` (`scheduler_thread.cpp:113`) —
   near zero for a parent that is about to block. So placing a thread adds ~0 to
   the target core's load, `_UpdateLoad()` sees `oldKey == newKey` and returns
   early, and **the heap does not move.**

During a sub-millisecond spawn burst every core therefore reads the same stale
key and `PeekMinimum` hands out **the same core repeatedly**. That explains the
concentration (all five surplus threads on one CPU; 17 on cpu9) far better than
anything in `rebalance()` does.

**This is a hypothesis, not yet a finding.** §5 is the instrumentation that
settles it.

---

## 2. `fNeededLoad` is demand, not supply

Still correct, still load-bearing, and it is what makes the core-load signal
meaningful at all.

`ThreadData::_ComputeNeededLoad()` (`scheduler_thread.cpp:282`) computes
`fMeasureAvailableActiveTime / fMeasureAvailableTime * kMaxLoad`, where
(`scheduler_thread.h:490-493`, `:416-421`) available time is *ran + slept* and
**excludes time spent runnable in the run queue** — the preemption path
(`PutBack()`) never touches it. So a purely CPU-bound thread reports
`fNeededLoad == kMaxLoad` however little CPU it actually receives: this is
*demand*, deliberately.

`CoreEntry::fLoad` is the unclamped running sum of those demands, so a core with
two saturated threads genuinely holds 2000. **The oversubscription signal exists
and is correct.**

Two objections that would have broken this, both closed:

- *A thread that never sleeps never recomputes its load.* No — `Continues()`
  (`scheduler_thread.h:337`) calls `_ComputeNeededLoad()`, and `reschedule()`
  calls `Continues()` on every run/ready transition (`scheduler.cpp:360`, `:459`).
- *The EWMA never reaches `kMaxLoad`.* It reaches it **exactly**: `compute_load`'s
  `deltaTime` is elapsed *available* time and `UpdateActivity()` increments
  available and active by the same amount when the thread never sleeps, so the
  ratio is exactly 1.

---

## 3. The migration predicate is arithmetically unsatisfiable

Independently confirmed. `low_latency.cpp:118-131`, with `kMaxLoad = 1000` and
`kLoadDifference = 200` (`scheduler_common.h:43`):

```c++
int32 coreLoad  = core->GetLoad();          // clamped to kMaxLoad
int32 otherLoad = other->GetLoad();         // >= 0
int32 difference = coreLoad - otherLoad - kLoadDifference;   // therefore <= 800
int32 threadLoad = threadData->GetLoad() / core->CPUCount(); // 1000 on non-SMT
return difference >= threadLoad ? other : core;
```

`CoreEntry::GetLoad()` (`scheduler_cpu.h:404`) returns
`std::min(fLoad / fCPUCount, kMaxLoad)`. The clamp bounds `coreLoad` at 1000, so
`difference <= 800` **always**.

**The sharp statement, which is better than "saturated threads":** on a non-SMT
core, **any thread whose `fNeededLoad` exceeds 800 — 80 % duty — can never be
migrated, however idle the machine.** That includes an 85 %-duty *interactive*
thread, which no CPU-bound ladder would ever test.

`rebalance()` is live: one call site (`scheduler.cpp:115`), reached every quantum
via `enqueue(oldThread, false)` when a doubled-up core alternates threads
(`scheduler.cpp:358-366`, `:432-437`). So the predicate is consulted thousands of
times a second and declines every time — consistent with the observed zero
migrations.

### 3.1 Why it works on x86: the `/ CPUCount()` divisor must NOT be removed

With SMT, `CPUCount == 2` halves `threadLoad` to 500 against a maximum
`difference` of 800, and the predicate becomes satisfiable. **That is the only
reason x86 migration works today.** arm64 sets
`topology_id[CPU_TOPOLOGY_SMT] = 0` for every CPU
(`arch/arm64/arch_cpu.cpp:114-116`), correctly — Graviton has no SMT — so
`CPUCount == 1` and the divisor vanishes.

The divisor is also the *correct* normalisation: `GetLoad()` is itself
`fLoad / fCPUCount`, i.e. demand per logical CPU, and removing a thread of demand
`L` from a core with `C` logical CPUs reduces that quantity by `L / C`. Both
sides of the inequality are in the same units.

**Deleting it would set `threadLoad = 1000` on x86 and port the arm64 defect onto
every SMT machine — untestable from here.** Recorded as a rejected hypothesis.

---

## 4. Constraint: `CoreEntry::GetLoad()`'s contract must not change

An earlier draft of this design proposed simply removing the clamp from
`GetLoad()`. **That would have been a latent panic on x86**, and it is worth
recording exactly why, because it is the failure class that matters most here:
boots fine on the only hardware available for testing, panics on the platform
that cannot be tested.

`CPUEntry::_RequestPerformanceLevel()` (`scheduler_cpu.cpp:328-331`):

```c++
int32 load = std::max(threadData->GetLoad(), fCore->GetLoad());
ASSERT_PRINT(load >= 0 && load <= kMaxLoad, ...);
```

`ASSERT_PRINT` is `panic()` under `KDEBUG`, and **`KDEBUG_LEVEL` is 2 in the
checked-in build** (`build/config_headers/kernel_debug_config.h:8`, no override
anywhere in the tree; `KDEBUG` is `KDEBUG_LEVEL_2`, `:19`). So the assert is
compiled into nightly and release images — the earlier claim that release builds
compile it out was wrong.

It is dead on Graviton only by accident: `gTrackCPULoad` is
`increase_cpu_performance(0) == B_OK` (`scheduler.cpp:780`), the only cpufreq
modules are `intel_pstates`/`amd_pstates` under `src/add-ons/kernel/cpu/x86/`,
and `src/add-ons/kernel/cpu/arm64/` contains only a Jamfile. So on arm64
`gTrackCPULoad == false` and `_RequestPerformanceLevel` never runs. On x86 with a
pstates module it runs on **every context switch** and would fire for three or
more saturated threads on an SMT core — ordinary desktop oversubscription.

**Design constraint adopted: add a separate unclamped accessor used only by the
rebalance predicates, and leave `GetLoad()` alone.** Full consumer list to
respect, and the audit is provably complete because `scheduler_cpu.h` is included
from nowhere outside `src/system/kernel/scheduler/`:

| Site | Role | Must keep clamped? |
|---|---|---|
| `low_latency.cpp:120,121` | rebalance predicate | **no — wants unclamped** |
| `low_latency.cpp:175` | `rebalance_irqs` | wants unclamped |
| `power_saving.cpp:141,174,184` | rebalance predicate | **no — wants unclamped** |
| `power_saving.cpp:99` | `choose_core` pack test | wants unclamped |
| `power_saving.cpp:204,205,264` | IRQ packing | wants unclamped |
| `scheduler_cpu.cpp:328,331` | cpufreq level + live `panic()` | **yes** |
| `scheduler_cpu.cpp:554,563,565` | **heap key** + `kHighLoad`/`kMediumLoad` band | **think hardest — see below** |
| `scheduler_cpu.cpp:384,716` | debug dumps | cosmetic |

**The heap key is the delicate one.** It decides the ordering `choose_core()`
sees, which is the very path §1.4 implicates. Making it unclamped would let
oversubscribed cores sort correctly — plausibly part of the fix — but it changes
placement behaviour, so it must be decided *after* §5, not before. Note also
that `_UpdateLoad()` already keys on `fCurrentLoad` when an interval was skipped
(`scheduler_cpu.cpp:554`) — the **raw** unclamped sum, neither divided by
`fCPUCount` nor clamped — so unclamped keys already occur today, on every
architecture, and the two branches of that ternary are in different units. That
is a pre-existing inconsistency worth fixing on its own.

---

## 5. The instrumentation, and why it is not five lines of dprintf

The coordinator's suggested instrumentation was `dprintf` in `choose_core()`.
**That would erase the phenomenon.** `dprintf` reaches the console through
`arch_debug_serial_puts()`, which writes the UART **one character at a time,
synchronously** (`arch/arm64/arch_debug_console.cpp:70-82`, via
`debug.cpp:1553`). One line is milliseconds — and §1.4 establishes that the
defect *disappears* once spawns are separated by about a millisecond. Logging
inside the burst would stagger it and report health. A textbook Heisenbug.

Implemented instead (`scheduler_placement_trace.{h,cpp}`, `SCHEDULER_TRACE_PLACEMENT`,
explicitly **not for merge**):

- A lock-free ring buffer; recording is a few stores plus one `atomic_add`, with
  **all formatting and all I/O deferred** until after the burst.
- Per placement: which of `choose_core()`'s three paths returned the core (idle-core
  list / `gCoreLoadHeap` / `gCoreHighLoadHeap`), the core ID, its load, and the
  thread's load. This is the fact that decides the fix.
- Real migrations recorded; `rebalance()` declines only *counted* (they happen
  thousands of times a second), split into declines made while a less loaded core
  existed — which indicts the **predicate** — and declines made when every core
  looked identical, which indicts the **metric**.
- Dumped from ordinary thread context via a magic value on
  `_kern_set_scheduler_mode`, since formatting does blocking serial I/O and must
  never run near the scheduler. `smpscale -p` asks for it.

Predictions, so the experiment can fail: if §1.4 is right, a 16-thread burst
shows most placements taking the `idle-core-list` path early and then repeatedly
returning **the same core ID**, with the recorded core load stuck at a stale
value; and declines will be dominated by the "every core looked identical" class.
If instead placements are spread evenly across distinct cores, §1.4 is **refuted**
and the defect really is a rebalancing failure — in which case §3 is the whole
fix and this document is wrong about causation.

---

## 6. Acceptance criteria

Efficiency numbers are **not** acceptable evidence (§1.2). Required:

1. **Per-CPU busy sets and migration counts** at N ∈ {8, 12, 16, 17}, five
   repeats each — enough to beat the coin toss in §1.1. Now emitted directly by
   `smpscale` (busy/idle/partial counts, max/min over busy CPUs, idle CPUs named)
   and by `-g` (per-thread migration counts via `sched_getcpu()`).
2. **Migration counts, not just throughput.** Ping-pong shows up as a large count
   with a balanced ladder — otherwise exactly the outcome that gets declared a
   success. A per-thread counter for `rebalance()` returning `other != core` is
   in the kernel instrumentation; the userland `-g` counter cross-checks it.
3. **N = 17 must stay at eff 0.500**, which is already optimal.
4. **Mixed workload** (`-x`): sleepers must not start migrating.
5. **Real-time regression test.** 15 normal spinners plus one
   `B_REAL_TIME_DISPLAY_PRIORITY` spinner currently gives `rt/max = 0.998`, so RT
   is **not** starved today. But `rebalance()` is **priority-blind** — it compares
   loads and never consults `GetEffectivePriority()` — so any change that lets
   high-load threads migrate freely can bounce an RT thread every quantum. The
   network stack's reader thread runs at that priority, which is why this matters
   here specifically.
6. **Artifact verification** every time: `sha256` matched build-host to node, and
   `objdump` of the hot loop.

### What cannot be tested here

**An SMT topology.** Graviton has none and `c7g.metal` panics in GICv3, so no
machine in this project has `CPUCount() > 1`. The x86 SMT path is covered by
reasoning only, and that is a residual risk, not an eliminated one. It is the
main reason for the §4 constraint: a change confined to a new accessor used by
the rebalance predicates cannot alter the cpufreq assert or the heap key unless
we deliberately choose to, and each of those choices can be argued separately.

---

## 7. Rejected and refuted

Kept deliberately; dead hypotheses get written down rather than deleted.

- **"Perfect scaling to N = 15, exactly 0.500 at N = 16, 8/8."** Refuted. The
  defect starts at N = 8, severity reaches 4.03x, and N = 16 is a coin toss. The
  metric caused the error (§1.2).
- **"`rebalance()` creates the imbalance."** Refuted — zero migrations in either
  direction across 12 runs; the imbalance predates the first 50 ms sample (§1.3).
  `rebalance()` fails to *repair*.
- **"A persistently skipped CPU hosting the parked parent makes a collision
  certain at N = ncpus."** Refuted twice: idle CPU counts of 3, 5, 6 and 9 cannot
  come from one parked parent, and a gate that keeps the parent blocked on
  `snooze_until` with an absolute-deadline release **still collides**, in one run
  worse (max/min 4.029, 6 idle CPUs). Dropped.
- **"Just remove the clamp in `CoreEntry::GetLoad()`."** Rejected: live
  `panic()` on x86 (§4).
- **"The `/ core->CPUCount()` divisor is the bug."** Refuted — it is the correct
  normalisation and the only reason x86 works (§3.1).
- **"`kLoadDifference` is the wrong hysteresis."** No evidence for it; it only
  looked wrong because it was compared against a clamped value.
- **"Release builds compile the cpufreq assert out."** Wrong: `KDEBUG_LEVEL` is 2
  in the checked-in build (§4).
- **"Five lines of dprintf in `choose_core()` will settle it."** Rejected — the
  serial write is per-character and synchronous, and would stagger the burst it
  is measuring (§5).
- **"A prior shell-loop measurement proved SMP works."** `date` at 1 s resolution
  against a ~2 s run cannot distinguish 1x from 2x. Right sign, hid everything.

---

## 8. Separate defects found along the way, out of scope

- **`gTrackCPULoad == false` on arm64** means `CPUEntry::ComputeLoad()` never
  runs, so `CPUEntry::fLoad` is permanently 0 and the load-triggered
  `rebalance_irqs()` at `scheduler_cpu.cpp:211` **can never fire on Graviton**.
  Interrupt rebalancing is silently dead on this platform.
- **`MinMaxHeap::PeekMinimum(int32 index)` is not the index-th smallest.** It
  returns `fMinElements[index]`, a raw heap slot (`MinMaxHeap.h:193`), so only
  `index == 0` is the true minimum. The `PeekMinimum(index++)` loops in
  `choose_core()` and `rebalance()` therefore iterate candidates in **arbitrary
  order** — a real hazard for any fix that touches those loops, and a reason not
  to trust "the least loaded core" language in the existing comments.
- **`smpscale` was not wired into the build** — no `SubInclude` in
  `src/bin/Jamfile` and absent from `build/jam/packages/Haiku`, so every number
  measured before this branch came from a hand-compiled binary the committed tree
  could not reproduce. Fixed here.
- **`smpscale`'s release gate was a busy-wait**, fabricating N runnable CPU-bound
  threads during the very spawn burst under investigation. Fixed here (blocking
  release on a common absolute deadline).
