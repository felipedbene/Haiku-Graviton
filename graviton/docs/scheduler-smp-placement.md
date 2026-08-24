# SMP work is placed badly and never repaired

Status: **implemented** on `fix/scheduler-saturated-migration`, three commits,
awaiting hardware A/B. Design was reviewed and approved before implementation. The causation question has
been settled by measurement on hardware (§1.7) without needing a bake. One
remaining question — the mechanism of defect B — needs the kernel instrumentation
in §5 and therefore one image bake, but it does not change the shape of the fix.
Verified against the tree at `a195688e46`.

**Summary for review.** The reported "0.500 at N = 16" is three separate defects.
Below `ncpus` it is a *placement* failure with a confirmed mechanism (a core stays
in the idle-core list until its CPU reschedules, so a sub-5-µs burst is handed the
same core repeatedly). At exactly `ncpus` it is a *repair* failure — a core becomes
idle after the placement decision and nothing ever moves work onto it. Above
`ncpus` it is both. The proposed fix is therefore two independent changes, each
tied to a measured signature, and **neither touches `CoreEntry::GetLoad()`'s
contract**.

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
  N = 8 failed 5 of 6 runs; N = 12 failed 4 of 6. **Independently confirmed
  first-hand**: a `-t 3000 1 8 12 16 17` ladder, 5 repeats, gave `max/min` of
  **2.00–4.01 at every one of N = 8, 12, 16, 17 in all five repeats**, while N = 1
  was always clean. Worst single point: N = 16 with cpu15 at **15022 ms** — five
  threads stacked — and nine CPUs at 0 ms.
- **Severity exceeds 2x.** Observed max/min over busy CPUs reaches **3.0, 4.03
  and 5.02** — up to five saturated threads stacked on one core.
- **`cpu_busy_ms` is always ≈ N × unit**, so no CPU time is lost and no work is
  duplicated. The defect is *purely* placement. Corroborating signature: the walls
  are **exact integer multiples** of the work unit (2.0x, 3.0x, 4.0x, 5.0x, never
  1.4x or 2.6x), which is threads stacked K-deep and run to completion rather than
  threads sharing CPUs broadly.
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
- **Confirmed independently and by a different method.** `smpscale -g` has each
  thread sample `sched_getcpu()` at ~1 kHz and count its own transitions. Over
  three repeats at N = 8 and 16: **`migr_max` = 1 in every single run** — no
  thread moved more than once in 3–12 s of running — with totals of 2–10
  migrations across 24 000–48 000 samples (0.08–0.21 per thousand samples). Once
  a thread is placed it never moves.
- The mixed workload (`-x`) is the sharpest version: the **sleepers** land on
  13–14 distinct CPUs, so the wake path is demonstrably alive and placing threads
  widely, yet the **CPU-bound threads remain stacked 3.0x–4.0x** with only 13–17
  migrations in 25 512 samples. The repair path is not merely slow, it is absent
  for exactly the threads that need it — which is precisely what §3's
  ">80 % duty can never migrate" predicts.

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

### 1.5 A second, sharper placement mechanism — and the stagger threshold is not
yet measured

There is a competing explanation that fits the data better, and it is
distinguishable by a cheap experiment.

A core leaves the package idle-core list only when one of its CPUs **actually
reschedules onto a real thread**: `CPUEntry::UpdatePriority()` calls
`fCore->CPUWakesUp()` when the old priority was `B_IDLE_PRIORITY`
(`scheduler_cpu.cpp:190`), and `UpdatePriority()` is called from `reschedule()`.
But `enqueue()` only *requests* that the target CPU reschedule — it sends an
inter-processor interrupt (`scheduler.cpp:130-137`). **Until that CPU gets round
to rescheduling, its core is still in the idle-core list.**

So during a spawn burst on the parent's CPU: thread 1 is placed on idle core A
and an IPI is sent; before A wakes, thread 2 is placed and `GetIdleCore(0)`
**still returns A**; and so on. The window is IPI-plus-reschedule latency, not a
load-measurement interval.

**This matters because the two mechanisms predict different thresholds**, and the
reported threshold is not actually measured:

- The claim "the threshold is ~1 ms, which is exactly `kLoadMeasureInterval`" rests
  on **two points**, 0 µs and 1000 µs. Two points cannot locate a threshold; they
  only bracket it. The true knee could be anywhere in between.
- If the knee is at **tens of microseconds**, the mechanism is the idle-list lag
  above and `kLoadMeasureInterval` is a coincidence.
- If the knee is at **~1000 µs**, the mechanism is the stale heap key of §1.4.

**A stagger sweep settles this and needs no kernel change and no image bake** — it
runs on the stock canonical AMI, because the stagger is entirely a property of the
test program. `smpscale -s <µs>` exists for exactly this. Sweeping 0, 10, 25, 50,
100, 250, 500, 1000, 2000 µs with repeats locates the knee, and the knee names the
mechanism. This is the cheapest decisive experiment available and it is being run
before any bake is requested.

### 1.6 The sweep was run, and it identifies the mechanism — via my own bug

A 100-run sweep (staggers 0, 5, 10, 25, 50, 100, 250, 500, 1000, 2000 µs; N = 8
and 16; 5 repeats) came back **completely flat**: 0 of 5 clean at every stagger
for N = 16, and 1 of 100 overall, which is indistinguishable from the coin toss.
No knee anywhere.

That looked like a null result. It is not — it is the mechanism, and it was
exposed by a flaw I introduced myself.

Replacing the busy-wait gate with a blocking `snooze_until()` barrier (§8) was
correct on its own terms: the busy-wait fabricated N runnable CPU-bound threads
during the very burst under investigation. But it **silently destroyed the only
mechanism by which a spawn stagger could matter**. With the barrier, every worker
blocks the instant it is spawned, so:

- its CPU never reschedules onto it, so `CPUEntry::UpdatePriority()` never sees a
  transition out of `B_IDLE_PRIORITY`, so **`CoreEntry::CPUWakesUp()` never
  runs**, so the core is **never removed from the package idle-core list**;
- `choose_core()`'s first path therefore keeps returning **the same idle core**,
  however far apart the spawns are.

Contrast the two builds, which together form a natural experiment:

| gate | staggered spawn | do threads run between placements? | core leaves idle list? | placement |
|---|---|---|---|---|
| busy-wait (old) | yes | **yes**, they spin | **yes** | **spreads** — the reported "1 ms fixes it" |
| blocking barrier (new) | yes | no, they block | no | **piles up at every stagger** |

**This selects §1.5 over §1.4.** The variable that decides placement is whether
the core has left the idle-core list, not whether a load-heap key has been
refreshed. It also reinterprets the original datapoint: what 1 ms of stagger
bought in the old build was time for the **asynchronous wake-up IPI**
(`smp_send_ici(..., SMP_MSG_FLAG_ASYNC)`, `scheduler.cpp:136`) to land and the
target CPU to reschedule. The resemblance to `kLoadMeasureInterval` was a
**coincidence**, and the claim that the threshold "is exactly
`kLoadMeasureInterval`" should be discarded.

One further detail closes the loop on why the barrier run still misplaces
everything. `has_cache_expired()` compares against the **core's active time**, not
wall time (`low_latency.cpp:36-44`). During an idle 200 ms barrier the cores
accumulate almost no active time, so the cache does **not** expire; on wake each
thread takes the `Rebalance()` path, which declines (all loads ~0), and so the
**spawn-time layout is preserved verbatim**. Placement decides everything and
nothing ever repairs it.

`smpscale -i` now drops the barrier so each worker starts real work the instant it
is resumed — the realistic case, and the only condition under which `-s` can move
placement. The sweep must be re-run with `-i -s`, and that is the outstanding
experiment.

### 1.7 The corrected sweep: the knee is at or below 5 microseconds, and the
defect splits into three

90 runs of `smpscale -i -t 1000 -s <µs> <N>` on the same node, staggers
0/5/10/20/50/100/200/500/1000 µs, N = 8 and 16, 5 repeats each. Plus a 10-run
control without `-i`.

**At N = 8 the knee is at or below 5 µs:**

| stagger (µs) | 0 | 5 | 10 | 20 | 50 | 100 | 200 | 500 | 1000 |
|---|---|---|---|---|---|---|---|---|---|
| clean of 5 | **3** | 5 | 5 | 5 | 5 | 5 | 5 | 5 | 5 |

**40 of 40 runs perfect for every stagger from 5 µs upward** (max/min
1.000–1.004); at 0 µs, 2 of 5 collapse to 8 threads on 7 cores. 5 µs was the
smallest non-zero value tested, so the true threshold is **≤ 5 µs and still
unresolved below that**.

**This is three orders of magnitude away from `kLoadMeasureInterval`.** It
confirms §1.5 (the asynchronous wake IPI leaves a core in the idle-core list for
IPI-plus-reschedule latency) and **refutes §1.4**: a stale load-heap key could
only be cleared by a stagger on the order of the 1000 µs load-tracking interval,
and 5 µs is nowhere near it.

**The control confirms `-i` is what makes `-s` live.** Without `-i`, staggers 0
and 1000 µs at N = 16 are statistically indistinguishable — walls 3025–4034 ms
against a 1000 ms unit, only 9–13 CPUs ever above 10 % — exactly as §1.6
predicted.

**But N = 16 has no knee at all**, and that is the second finding. No stagger
achieves 5/5; success is sporadic and **non-monotonic** in stagger (3 of 45 runs,
s = 200 giving 2/5, s = 1000 giving 1/5, every other value 0/5), while a run with
*no* stagger succeeds 1 of 3. And the failure signature is strikingly precise:
**exactly one core doubled, exactly one core left idle, and the other fourteen
running one thread each** — decoded from `unit_ms ≈ 2000, busy = 1, part = 14,
idle = 1`, whose active-time total is 2000 + 14×1000 = 16000 ms = 16 × 1 unit.

So there are **three distinct defects**, not one:

| | signature | when | mechanism | fixed by |
|---|---|---|---|---|
| **A** | several cores doubled, many idle, varies run to run | N < ncpus, sub-5 µs bursts | idle-core-list lag; `GetIdleCore(0)` returns `fIdleCores.Last()` until the target CPU reschedules — **confirmed** | placement |
| **B** | **exactly one** core doubled, **exactly one** idle | N == ncpus, stagger-independent | not yet established (§1.8) | **rebalance** |
| **C** | one core absorbing 5, or 17 of 32 threads | N > ncpus | heap path: keys clamped equal and a new thread adds ~0 load, so `PeekMinimum` repeats a core | placement + rebalance |

Defect A's mechanism is now **confirmed from the code as well as behaviourally**:
`PackageEntry::CoreGoesIdle()` does `fIdleCores.Add(core)` (appends) and
`GetIdleCore(0)` returns `fIdleCores.Last()` (`scheduler_cpu.h:545,481`), so a
burst is handed the *same* most-recently-idled core until its CPU reschedules.

### 1.8 Defect B is a repair problem, and that settles the scope question

Defect B is the original reported symptom — 0.500 at N = 16 — now isolated from
defect A and shown to be **stagger-independent**, i.e. not a burst artifact at all.

The candidate mechanism is the parent thread: while it is spawning, it occupies one
CPU, so only `ncpus - 1` cores are genuinely free; thread number `ncpus` finds no
idle core, falls to the heap path and doubles someone; the parent then blocks in
`wait_for_thread()` and **its core goes idle and stays idle forever**. That
produces exactly one doubled core and exactly one idle core.

**This is stated as a candidate, not a finding.** It does not obviously survive
the stagger-independence: at staggers of 500–1000 µs the parent `snooze()`s between
spawns and its core should re-enter the idle list, which ought to change the
outcome, and it does not. The kernel placement trace of §5 is what will settle it,
and this is the strongest remaining reason to bake.

**But the fix does not depend on which core it is.** Whatever the mechanism,
defect B is a case where a core becomes idle *after* the placement decision was
taken. **No placement fix can ever address it**, because the information did not
exist when the choice was made. Only a working repair path can — and the repair
path is arithmetically dead (§3).

That is the empirical answer to "decide explicitly whether placement is in scope":

- **Placement fix is necessary** — it is the only thing that fixes defect A, which
  is the most common case (N below ncpus) and is confirmed.
- **Rebalance fix is necessary** — it is the only thing that can fix defect B, and
  defect B is precisely the originally reported symptom.
- **Neither is sufficient.** They address disjoint, separately measured
  signatures. Doing only one and reporting a clean ladder would be the failure
  mode warned about.

`-i` alone, with no stagger, already gives **8/8 and 12/12 CPUs busy with max/min
≤ 1.001, 3 of 3 repeats** — so defect A really is the whole story below `ncpus`,
and N = 17 comes out at max/min 2.00 with all 16 CPUs busy, which is the
arithmetic optimum and must not be "improved".

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

### On this platform, `dprintf` is not a probe — it is a barrier

Worth stating as a general rule, because it will catch other work in this project.

`dprintf()` reaches the console through `arch_debug_serial_puts()`, which loops
over the string calling `arch_debug_serial_putchar()` — one UART register write per
character, synchronously, with no buffering
(`arch/arm64/arch_debug_console.cpp:61-82`, reached from `debug.cpp:1553`). An
80-character line is on the order of **milliseconds**.

So any timing-sensitive kernel path instrumented with `dprintf` is not being
observed, it is being **serialised**. The effect is not a perturbation to be
corrected for afterwards; it is larger than most of the phenomena worth measuring.
Here the defect vanishes once thread spawns are separated by **5 µs** (§1.7), so a
single `dprintf` inside `choose_core()` would have staggered the burst by roughly
a thousand times the threshold and reported a healthy scheduler. The instrument
would have created the very condition that hides the bug.

The rule: **to observe anything in the kernel faster than a millisecond, record to
memory and format later.** Recording must be a few plain stores plus at most one
atomic; every `printf`-family call, every lock, and every allocation belongs in the
deferred dump, which must itself run in ordinary thread context. This is also why
the dump here is triggered by a syscall rather than from the scheduler.

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

Build state: `jam -q kernel_arm64` and `jam -q smpscale` both succeed on
`a195688e46` plus this branch, with **no diagnostics** naming
`scheduler_placement_trace`, `low_latency`, `scheduler.cpp` or `smpscale.cpp`.
`smpscale` now links from the committed tree (previously it was hand-compiled and
not reproducible), pulling `libgnu.so` for `sched_getcpu()` and resolving
`_kern_set_scheduler_mode` against `libroot.so`; its hot loop is still a
five-instruction `madd` / `eor …, lsr #29` chain with zero memory operands.

Predictions, so the experiment can fail: if §1.4 is right, a 16-thread burst
shows most placements taking the `idle-core-list` path early and then repeatedly
returning **the same core ID**, with the recorded core load stuck at a stale
value; and declines will be dominated by the "every core looked identical" class.
If instead placements are spread evenly across distinct cores, §1.4 is **refuted**
and the defect really is a rebalancing failure — in which case §3 is the whole
fix and this document is wrong about causation.

---

## 5.5 The leading fix candidate, and why it satisfies both hypotheses at once

Both §1.4 and §1.5 are failures of the *same kind*: `choose_core()` decides using
a quantity that has **not yet caught up with the threads already placed**. The
idle-core list lags until the target CPU reschedules; the heap key lags because a
new thread inherits ~0 demand from its parent so the key does not move.

There is already a counter in the tree with **neither lag**:

```c++
CoreEntry::ThreadCount() = fThreadCount + fCPUCount - fIdleCPUCount
```
(`scheduler_cpu.h:351`) — queued threads plus running threads. `fThreadCount` is
incremented by `atomic_add` inside `CoreEntry::PushBack()`/`PushFront()`
(`scheduler_cpu.cpp:435,445`), which runs during `Enqueue()`, i.e. **at the moment
of placement**. Checked against the burst case:

| state | fThreadCount | fIdleCPUCount | `ThreadCount()` |
|---|---|---|---|
| idle core, nothing placed | 0 | 1 | **0** |
| one thread placed, CPU not yet woken | 1 | 1 | **1** |
| that thread now running | 0 | 0 | **1** |
| two threads placed on the same idle core | 2 | 1 | **2** |

The row that matters is the fourth: the second placement is visible
**immediately**, which is exactly what the idle-core list and the load heap both
fail to show. There is also precedent for treating this as the oversubscription
measure — `ComputeQuantum()` already uses `fCore->ThreadCount()` divided by
`CPUCount()` (`scheduler_thread.cpp:207-209`).

**Fix 1 (placement, cures defects A and C): prefer an idle core that has not
already been claimed.** `choose_core()`'s idle-core path currently takes
`fIdleCores.Last()` and keeps taking it until that core's CPU reschedules. Walk
the idle list once and prefer the candidate with the smallest `ThreadCount()`,
falling back to the least-claimed one rather than to the load heap. The walk
already exists — `GetIdleCore(index)` is called in a loop for the CPU-mask check —
so the added cost is one `ThreadCount()` read per candidate, and the common case
exits on the first entry because it is genuinely free. (Better still, add a
purpose-built `PackageEntry` method that walks `fIdleCores` once: `GetIdleCore(i)`
is O(i), so looping it is O(n²) in the idle-list length.)

**Use `ThreadCount()` as a tie-breaker, never as a replacement for load.** The
distinction is what keeps the change safe:

- `ThreadCount()` is a *bad* general load metric — a core with five sleepy
  threads has `ThreadCount() == 5` but almost no load, and a core with one
  saturated thread has `ThreadCount() == 1`. Preferring the latter would be
  actively wrong.
- So load stays the primary criterion and thread count breaks ties. Behaviour
  then changes **only when the load metric cannot discriminate**, which is
  precisely the burst pathology and nothing else.

This also has the property the §4 constraint demands: it is a pure placement
change and **does not touch `CoreEntry::GetLoad()`'s contract at all**, so it
cannot reach the live `panic()` in `_RequestPerformanceLevel` or alter the heap
key.

**Fix 2 (repair, cures defect B): make the migration predicate satisfiable, via a
new accessor rather than by changing `GetLoad()`.** Add an unclamped
`CoreEntry::GetLoadUnclamped()` (or similar) returning `fLoad / fCPUCount`, and use
it **only** in the rebalance predicates — `low_latency.cpp:120,121` and
`power_saving.cpp:141,174,184`. `GetLoad()` keeps its clamp and its contract, so
the cpufreq assert, the heap key, and the `kHighLoad`/`kMediumLoad` band decision
are all untouched and provably unaffected (§4).

Worked through for the defect-B signature — one core with two saturated threads,
one core idle: `coreLoad = 2000`, `otherLoad = 0`, `difference = 1800`,
`threadLoad = 1000` → migrate; afterwards `otherLoad + 200 >= coreLoad` is
`1200 >= 1000` → decline. One migration, then stable. And at N = 17 (all cores at
1000 except one at 2000, none idle) `difference = 800 < 1000` → decline, which is
correct: moving a thread would only move the collision. `kLoadDifference` needs no
change.

**Fix 3 (`power_saving`, same change): the mode fails by exactly one unit.**
`power_saving.cpp:142-155` gates on `threadLoad >= coreLoad / 2`, and for exactly
two saturated threads on a non-SMT core that is `1000 >= 1000` — true, so it
declines. It therefore fails for **exactly two** threads, the commonest case, and
no low-latency ladder would ever reveal it. Using the unclamped accessor makes
`coreLoad = 2000` and the test `1000 >= 1000` still true, so this one needs its own
touch: the guard must compare against the *unclamped* half, or be bypassed when a
core is genuinely oversubscribed. **Stated explicitly rather than left silent**:
without this, `power_saving` stays broken for the two-thread case.

### 5.6 The two fixes are separable, and the migration count separates them

The concern that "the efficiency ladder cannot distinguish a placement fix from a
rebalance fix" is real, but the **migration count can**, which is why it is a
required criterion rather than a nice-to-have:

| | busy sets at N = 16 | migrations |
|---|---|---|
| today | broken ~50 % of runs | **0** |
| placement fix only | correct | **~0** |
| rebalance fix only | correct | **burst at startup**, then 0 |
| both | correct | ~0 |

So the layering is deliberate, and both are wanted for different reasons:

- **Placement (primary)** — stop creating the collision. Cheap, and the only one
  that avoids paying cache and TLB cost to unwind a bad start.
- **Rebalance (secondary)** — repair collisions that placement cannot foresee,
  because a thread's demand can *rise after* it is placed. No placement fix
  addresses the >80 %-duty thread of §3 whose load grows later; only a working
  repair path does.

---

## 5.9 Pre-registered predictions for the A/B

Written down **before** the fixed kernel boots, so that the result cannot be
rationalised afterwards. Each is falsifiable and each names which fix it tests.

1. **Fix 1 reproduces the stagger result without the stagger.** `-i -s 0` at
   N = 8 and N = 12 should match what `-i -s 5` gave on the stock kernel:
   `busy == N`, `max/min <= 1.01`, **5 of 5 repeats**. On the stock kernel `-s 0`
   gave 3 of 5. If N = 8 still fails at `-s 0`, Fix 1 does not do what §1.7 says
   it does.
2. **Fix 2 fixes N = 16, which no stagger value could.** N = 16 should reach
   `busy = 16, idle = 0, max/min < 1.1` in **5 of 5** repeats. On the stock kernel
   the best any stagger achieved was 2 of 5, and 0 of 5 at most values. This is
   the sharpest single discriminator in the whole exercise, because it is the one
   result that Fix 1 *cannot* produce.
3. **Migration counts stay small and non-zero.** `-g` at N = 16 should show
   `migr_tot` on the order of **1–20** per run — a handful of corrective moves —
   not the 0 of the stock kernel and not thousands. Specifically `migr_max` should
   be small (single digits). **If `migr/1ks` rises above ~5, that is thrashing and
   the fix is wrong even if the busy sets look perfect.**
4. **N = 17 does not change.** It should stay at `max/min ≈ 2.00` with all 16 CPUs
   busy, because 17 threads on 16 cores must double exactly one. An "improvement"
   here means something is migrating pointlessly.
5. **N = 32 degrades gracefully.** `max/min ≈ 2.0` rather than the ~8.5 measured
   on the stock kernel, with all 16 CPUs busy and no CPU absorbing 17 threads.
6. **The mixed workload does not regress.** Sleepers must still spread (13–14
   distinct CPUs) *and* the CPU-bound half must now come out level — on the stock
   kernel the sleepers spread while the CPU-bound threads stayed stacked 3–4x.
   Sleeper migration counts must not explode.
7. **The real-time thread is not disturbed.** 15 normal spinners plus one
   `B_REAL_TIME_DISPLAY_PRIORITY` spinner currently gives `rt/max = 0.998`. It must
   stay there. `rebalance()` is **priority-blind** — it compares loads and never
   consults `GetEffectivePriority()` — so making high-load threads migratable is
   exactly the change that could start bouncing an RT thread every quantum. This
   is the prediction most likely to fail, and the network stack's reader thread
   runs at that priority.

A/B commands, run interleaved against the canonical AMI on the same instance type:

```
smpscale -i -t 1000 -s 0 8 ; smpscale -i -t 1000 -s 0 12      # prediction 1
smpscale -t 3000 1 8 12 16 17                                  # predictions 2, 4
smpscale -t 3000 -g 8 16                                       # prediction 3
smpscale -t 3000 32                                            # prediction 5
smpscale -t 3000 -x 16                                         # prediction 6
```

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
