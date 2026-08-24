# The scheduler can never migrate a saturated thread onto an idle core

Status: **design, pending approval to implement.** Code reading complete and
verified against the tree at `a195688e46`. Hardware reproduction in progress.

This is a **generic Haiku defect**, not an arm64 one. It costs one whole core on
every non-SMT multi-core machine. arm64/Graviton merely removes the accident
that was hiding it on x86 desktops.

---

## 1. The symptom

Reproduced first-hand on instance `i-059bc5d23bc47a09e`, a 16-vCPU
`c7g.4xlarge` booted from the canonical AMI `ami-0d61e3910062bb80a`
(`hrev59996`), with `src/bin/smpscale`, which gives every thread an *identical,
fixed* work unit. Perfect scaling therefore holds wall time constant as N rises;
perfect serialisation makes it proportional to N. Binary verified by artifact:
identical `sha256` on the build host, this desktop and the node
(`138c39ce…2eec2`), and the hot loop confirmed in `objdump` to be a
register-bound `madd` / `eor …, lsr #29` dependency chain with **zero memory
operands**.

Four identical repeats of the full ladder, `-t 3000`:

```
threads:    1     2     4     6     8    10    12    13    14    15    16    17    20
run 1:  1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 0.500 0.200
run 2:  1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 0.500 0.500 0.333
run 3:  1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 0.500 0.500 0.333
run 4:  1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 1.000 0.333 0.250
```

### 1.1 Two corrections to the original framing

Recorded because both matter for what the fix has to achieve, and because
getting them wrong would have meant claiming a result that is not there.

- **N = 16 is *not* deterministic, and the reported "exactly 0.500, 8/8" does
  not reproduce.** It was perfect (1.000) in runs 1 and 4 and broken (0.500) in
  runs 2 and 3 — **2 of 4**. Placement is a coin toss, so the claim in the
  original brief that mechanism 4 makes "a collision certain at N = ncpus" is
  **refuted**: it makes one *likely*, not certain. Any A/B of the fix therefore
  needs enough repeats to separate a real change from this coin toss, which a
  single before/after pair could not do.
- **The overflow does not spread one-per-core; it concentrates on a single
  core**, and this is far worse than reported. At N = 20 the four extra threads
  should land on four different CPUs (ideal max 6000 ms). Instead run 1 put
  **all five on cpu1** — 15019 ms — and run 4 put four on cpu6 (12014 ms). The
  scheduler picks a victim and keeps feeding it. See §2.6, which turned out to
  be a *third* mechanism, not a restatement of mechanism 4.

### 1.2 What did reproduce, exactly as reported

- **The imbalance scales with the work unit**, confirming there is no
  rebalancing at all rather than slow rebalancing. A 2000 ms unit gives a
  **4002.0 ms** max with cpu15 at **1 ms**; an 8000 ms unit gives a **16005.4 ms**
  max with cpu8 at **2 ms**. Eight full seconds of a completely idle CPU beside a
  doubled-up one, and no migration at any duration.
- **N = 32 is catastrophic**: 51279.8 ms against an ideal 6000 ms — **8.5x** —
  with `cpus>10%` collapsing to **2**. cpu9 absorbed roughly **17 of the 32
  threads** (51249 ms) while cpu3 got 24 ms. Fourteen CPUs each ran one thread
  and then idled for 48 seconds rather than take any of cpu9's 16-deep queue.
- **The victim CPU varies** (cpu1, cpu4, cpu6, cpu7, cpu8, cpu9, cpu13, cpu15
  across runs), so this is not one masked or broken CPU.
- **Both witnesses agree.** Summed `cpu_info::active_time` equals
  `N x 3000 ms` to within 0.1 % in every row, and per-CPU `active_time` tracks
  wall-clock `max_ms` to within a millisecond. No CPU time goes missing: the work
  is all executed, just placed catastrophically. `min_ms` is always ~3000, so the
  *fast* threads are perfectly served — only the victims stack up.

### 1.3 A metric artifact to be careful of

In the single-point runs (`-t 2000 16`, `-t 8000 16`, `-t 3000 32`) `smpscale`
prints `eff 1.000`, which is meaningless: `baseWall` is taken from the first
ladder row, so with one row `eff` is trivially 1. Those runs must be read from
`max_ms` and the per-CPU table, never from `eff`. Anything quoting `eff` from a
single-point run is quoting an artifact.

---

## 2. The mechanism

Four things combine. All four were verified by reading the tree, not inferred.

### 2.1 `fNeededLoad` is *demand*, and it is unbounded per core — this is the good news

This is the load-bearing fact for the whole fix, and it was the one thing that
could have invalidated it.

`ThreadData::_ComputeNeededLoad()` (`scheduler_thread.cpp:281`) computes

```
fNeededLoad = fMeasureAvailableActiveTime / fMeasureAvailableTime * kMaxLoad
```

where (`scheduler_thread.h:490-493`, `:416-421`)

- `fMeasureAvailableActiveTime` += time the thread actually **ran**;
- `fMeasureAvailableTime`       += time it **ran**, plus time it **slept**.

Time spent *runnable but waiting in the run queue* is added to **neither**. The
preemption path is `ThreadData::PutBack()` / `enqueue()`, and neither touches
`fMeasureAvailableTime`; only `Enqueue()` after an actual `GoesAway()` adds
`timeSlept`.

So for a purely CPU-bound thread, `sleepTime == 0` and **`fNeededLoad ==
kMaxLoad` no matter how little CPU it actually gets.** That is deliberate:
`fNeededLoad` measures how much CPU a thread *wants*, given the opportunities it
had — demand, not supply.

Two objections that would have broken this, both checked and closed:

- *"A thread that never sleeps never recomputes its load, so `fNeededLoad` sits
  at its initial 0."* No. `ThreadData::Continues()` (`scheduler_thread.h:337`)
  calls `_ComputeNeededLoad()`, and `reschedule()` calls `Continues()` on every
  `B_THREAD_RUNNING`/`B_THREAD_READY` transition (`scheduler.cpp:360`) and again
  on the incoming thread (`:459`). A saturated thread recomputes roughly every
  millisecond.
- *"The EWMA in `compute_load()` will not actually reach `kMaxLoad`."* It
  reaches it exactly. `compute_load` is invoked as
  `compute_load(fLastMeasureAvailableTime, fMeasureAvailableActiveTime,
  fNeededLoad, fMeasureAvailableTime)`, so its `deltaTime` is elapsed
  *available* time and its `measureActiveTime` is active time in the same
  window. For a thread that never sleeps `UpdateActivity()` increments both by
  the identical amount, so `deltaTime == measureActiveTime` and
  `newLoad = kMaxLoad` exactly — not asymptotically. The EWMA then holds at
  `kMaxLoad`.

Consequently `CoreEntry::fLoad`, which is just the running sum of the
`fNeededLoad` of the threads assigned to the core (`AddLoad`/`RemoveLoad`/
`ChangeLoad`, `scheduler_cpu.h:409-460`), is an **unbounded sum of demands**. A
core with two saturated threads genuinely holds `fLoad == 2000`.

**The oversubscription signal exists and is correct.** It is destroyed one
layer up.

### 2.2 `CoreEntry::GetLoad()` clamps the signal away

`src/system/kernel/scheduler/scheduler_cpu.h:404`

```c++
return std::min(fLoad / fCPUCount, kMaxLoad);
```

`fLoad / fCPUCount` is "demand per logical CPU on this core" — a meaningful,
unbounded quantity. The `std::min` truncates it at 1.0. A core running two
saturated threads reports **exactly the same load** as a core running one
(`2000/1 → 1000`, vs `1000/1 → 1000`). Oversubscription becomes invisible to
every consumer: both core load heaps, `choose_core()`, and `rebalance()`.

### 2.3 The migration test is therefore arithmetically unsatisfiable

`src/system/kernel/scheduler/low_latency.cpp:118-131`

```c++
int32 coreLoad = core->GetLoad();
int32 otherLoad = other->GetLoad();
if (other == core || otherLoad + kLoadDifference >= coreLoad)
        return core;
int32 difference = coreLoad - otherLoad - kLoadDifference;
int32 threadLoad = threadData->GetLoad() / core->CPUCount();
return difference >= threadLoad ? other : core;
```

With `kMaxLoad = 1000` and `kLoadDifference = kMaxLoad * 20 / 100 = 200`
(`scheduler_common.h:43`):

- `coreLoad <= kMaxLoad` because of the clamp;
- `otherLoad >= 0`;
- so `difference <= 1000 - 0 - 200 = 800`, **always**;
- a fully CPU-bound thread has `threadLoad = 1000 / CPUCount()`.

On a non-SMT core `CPUCount() == 1`, so the test is `800 >= 1000` — **false for
every value of kMaxLoad**, since `0.8 * kMaxLoad >= kMaxLoad` can never hold. A
saturated thread can never be migrated, even onto a completely idle core.

### 2.4 The test is consulted constantly, and fails every time

Worth confirming, because if saturated threads never reached `rebalance()` at
all then fixing its arithmetic would change nothing.

They do. In `reschedule()` (`scheduler.cpp:358-366, 432-437`): when a thread's
quantum ends, `putOldThreadAtBack = true`, and if a *different* thread is chosen
next it goes through `enqueue(oldThread, false)`. `enqueue()` with
`newOne == false` calls `threadData->Rebalance()` (`scheduler.cpp:115`).

On the doubled-up core the two threads alternate, so `nextThread != oldThread`
every quantum and **`rebalance()` runs thousands of times per second and
declines every single time.** On a singly-occupied core `nextThread ==
oldThread`, so `rebalance()` is not called — correctly, as nothing needs to
move. This matches the observation exactly: sixteen seconds, no migration.

### 2.5 Why this hid on x86 desktops

`threadLoad = threadData->GetLoad() / core->CPUCount()`. With SMT,
`CPUCount == 2`, so `threadLoad` halves to 500 and `800 >= 500` succeeds. arm64
sets `topology_id[CPU_TOPOLOGY_SMT] = 0` for every CPU
(`arch/arm64/arch_cpu.cpp:114-116`) — which is **correct**, Graviton has no SMT
— so `CPUCount == 1`, the divisor vanishes, and the latent bug becomes visible.

---

## 3. Disproved: the `CPUCount()` divisor is not the bug

It was proposed that dividing a thread's load by its core's CPU count is
meaningless on a non-SMT machine and is the actual defect. **It is not, and
removing it would be a regression.** The divisor is the correct normalisation:

`CoreEntry::GetLoad()` is itself `fLoad / fCPUCount`, i.e. demand *per logical
CPU*. Removing a thread of demand `L` from a core with `C` logical CPUs reduces
that quantity by exactly `L / C`. So `threadLoad = GetLoad() / CPUCount()` puts
both sides of the inequality in the same units. The two divisions are
consistent by design.

On a non-SMT core `C == 1`, so the divisor is the identity — it is not doing any
harm there either. The only thing wrong with the expression is that the
`coreLoad` it is compared against has been clamped, which destroys the
monotonicity of the comparison above 1.0. **Fix the clamp, keep the divisor.**

---

## 4. The proposed fix

### 4.1 Core change — stop discarding the oversubscription signal

`CoreEntry::GetLoad()` returns the true per-logical-CPU demand:

```c++
return fLoad / fCPUCount;
```

Why this is the right change rather than a wider redesign:

- The quantity is already correct and already unbounded (§2.1). We are removing
  a truncation, not inventing a metric.
- It restores the migration test *without touching the test*. Worked through:
  - **2 saturated threads on one core, one core idle** (the defect):
    `coreLoad = 2000`, `otherLoad = 0`, `difference = 1800`, `threadLoad = 1000`
    → `1800 >= 1000` → **migrate**. Result 1000/1000.
  - **After that migration:** `otherLoad + 200 >= coreLoad` is `1200 >= 1000` →
    return `core`. **No thrash.** `kLoadDifference` works correctly as
    hysteresis once the loads are comparable, so it does not need changing.
  - **N = 20 on 16 CPUs** (4 cores at 2000, 12 at 1000): `difference = 2000 -
    1000 - 200 = 800`, `threadLoad = 1000` → declines. **Correct** — migrating
    would only move the collision. Graceful degradation, not serialisation.
  - **N = 32** (all cores at 2000): `otherLoad + 200 >= coreLoad` → declines.
    Correct.
  - **3 threads on one core, one core idle:** 2800 >= 1000 → migrate; then
    2000 vs 1000 → declines. Settles at the optimum for 17 threads on 16 CPUs.
- **The blast radius is provably tiny.** `std::min(x, kMaxLoad)` differs from
  `x` only when `x > kMaxLoad`. Every core whose summed per-CPU demand is at or
  below 100% behaves **bit-identically** to today. The change can only affect
  genuinely oversubscribed cores — which are exactly the broken case.
- It also improves *placement*, because `_UpdateLoad()` uses `GetLoad()` as the
  core-heap key (`scheduler_cpu.cpp:554,563`). Today an oversubscribed core and
  a busy-but-fine core have the same key, so `PeekMinimum()` cannot tell them
  apart; afterwards oversubscribed cores correctly sort last.
- **The core heaps already tolerate keys above `kMaxLoad` today.** In
  `_UpdateLoad()` (`scheduler_cpu.cpp:554`) the key is
  `intervalSkipped ? fCurrentLoad : GetLoad()` — and `fCurrentLoad` is the
  **raw** sum of demands, neither divided by `fCPUCount` nor clamped. So
  whenever a load-measurement interval is skipped on an oversubscribed core, the
  existing code already inserts an unclamped key into the heap, on every
  architecture. That is empirical evidence that unclamped keys are safe here, and
  it is also a pre-existing inconsistency: the two branches of that ternary are
  in different units.
- **Core-heap *membership* is unchanged.** The low/high split is at `kHighLoad =
  700`, below `kMaxLoad = 1000`, so any core that the clamp could have affected
  was already in `gCoreHighLoadHeap` at its clamped value of 1000 and stays
  there unclamped. Only the *ordering within* the high-load heap changes, and it
  changes to be correct. This also means `choose_small_task_core()`
  (`power_saving.cpp:52`, which reads `gCoreLoadHeap.PeekMaximum()`) sees
  exactly the same candidate set as before.

### 4.2 Required second change — a latent panic that makes "one line" wrong

`CPUEntry::_RequestPerformanceLevel()`, `scheduler_cpu.cpp:328-331`:

```c++
int32 load = std::max(threadData->GetLoad(), fCore->GetLoad());
ASSERT_PRINT(load >= 0 && load <= kMaxLoad, ...);
```

Unclamping §4.1 makes this assertion **false on any oversubscribed core**, i.e.
a kernel panic in any `DEBUG=1`/`KDEBUG` build. (`ASSERT_PRINT` compiles to
nothing when `KDEBUG == 0`, `debug.h:69`, which is why release images would not
have caught it — precisely the kind of thing that makes a one-line scheduler
change dangerous.)

The fix is to clamp *at the consumer*, where clamping is genuinely correct: the
cpufreq interface takes a 0..`kMaxLoad` demand, and an oversubscribed core
should simply request maximum performance.

```c++
int32 load = std::min(std::max(threadData->GetLoad(), fCore->GetLoad()), kMaxLoad);
```

This is the general principle of the change: **the clamp belongs at the
consumers that need a bounded ratio, not in the shared accessor that other
consumers need to be unbounded.**

### 4.3 Every other caller of `CoreEntry::GetLoad()`, checked

Required before changing the meaning of a shared accessor. The audit is
**provably complete**: `scheduler_cpu.h` is included from nowhere outside
`src/system/kernel/scheduler/`, so no consumer can exist beyond that directory
and a grep over it enumerates every one.

| Site | Effect of unclamping | Verdict |
|---|---|---|
| `scheduler_cpu.cpp:328` `_RequestPerformanceLevel` | assertion fires; cpufreq wants a bounded ratio | **must fix**, §4.2 |
| `scheduler_cpu.cpp:384` `CPUPriorityHeap::Dump` | `entry` is a **`CPUEntry`**, whose `GetLoad()` is `fLoad` and is genuinely bounded by `compute_load()` | unaffected |
| `scheduler_cpu.cpp:554,563` `_UpdateLoad` heap key | keys may exceed `kMaxLoad`; heaps are `int32`-keyed with no bound; `newKey > kHighLoad` still routes to the high-load heap | **desirable**, §4.1 |
| `low_latency.cpp:120,121,130` `rebalance` | the fix | intended |
| `low_latency.cpp:175` `rebalance_irqs` | won't move IRQs onto an oversubscribed core | improvement |
| `power_saving.cpp:99` `choose_core` | `GetLoad() + threadLoad >= kHighLoad` more readily rejects an oversubscribed pack target | improvement |
| `power_saving.cpp:204,205` `pack_irqs` | won't pack IRQs onto an oversubscribed core | improvement |
| `power_saving.cpp:264` `rebalance_irqs` | as above | improvement |
| `power_saving.cpp:141-184` `rebalance` | see §4.4 | **unchanged, documented** |

Overflow is not a concern: `fLoad <= nThreads * 1000`, far inside `int32`.

### 4.4 `power_saving.cpp` — deliberately *not* fixed, and why

`power_saving`'s `rebalance()` has different arithmetic (`:141-152`):

```c++
if (coreLoad > kHighLoad) {
        ...
        if (threadLoad >= coreLoad / 2)
                return core;
```

For two saturated threads this is `1000 >= 2000/2` → true → declines, even
unclamped. And past that guard it picks `gCoreLoadHeap.PeekMaximum()` — the
*most* loaded acceptable core — and requires `coreNewLoad - otherNewLoad >=
kLoadDifference / 2`, which an idle target also fails.

That is not an accident: this mode is *trying* to pack threads onto few cores to
let the rest idle, trading throughput for power. Making it spread saturated
threads would contradict its purpose. Since `SCHEDULER_MODE_LOW_LATENCY` is the
default (`scheduler.cpp:756`), the defect that costs a core on Graviton is fully
addressed by §4.1/§4.2.

**Recommendation: leave `power_saving`'s policy alone in this change and record
the gap here.** A bounded follow-up would be to let it fall through to spreading
only when a core's per-CPU demand exceeds `kMaxLoad` (genuine oversubscription,
never merely "busy"), but that is a power-policy decision that deserves its own
change and its own measurement, and mixing it in would make the blast-radius
argument in §4.1 untrue.

### 4.5 Mechanism 4 — the persistently skipped CPU: documented, not fixed

A separate, smaller defect explains why the collision at `N = ncpus` is
*certain* rather than merely likely. Threads are placed by `choose_core()` at
creation, which prefers a core from the package's idle-core list. The CPU where
the **parent** thread is running — it is executing `spawn_thread()` at that very
moment — is not idle, so it is skipped. Placement is therefore effectively
round-robin over `ncpus - 1`, and by the time N reaches `ncpus` some core must
have two.

**This should not be fixed by making placement clairvoyant.** Placement is a
heuristic that cannot know the future; rebalancing is the safety net, and the
safety net being *arithmetically dead* is the actual bug. With §4.1 the initial
collision still occurs but is corrected within about one load-measurement
interval (`kLoadMeasureInterval = 1000 us`) instead of never. §4.1 also makes
the core heaps rank oversubscribed cores correctly, which partially mitigates
the placement side for free.

The prediction to test: at N = 16 the run should show a brief initial imbalance
and then even per-CPU active_time, with a **small, bounded** migration count —
not zero, and not thousands.

---

## 4.6 What "fixed" means: efficiency is the wrong yardstick above N = ncpus

`smpscale`'s `eff` is `speedup / N` with `speedup = N * baseWall / wall`. With a
fixed per-thread work unit and perfect packing, `wall = ceil(N / ncpus)` units,
so the **arithmetic optimum** is

```
eff_optimal = 1 / ceil(N / ncpus)
```

which on 16 CPUs is 1.000 for N <= 16, 0.500 for 17..32, 0.333 for 33..48. So
`eff` falling as N passes `ncpus` is *correct behaviour*, and reading the raw
figure as a defect would manufacture a result. Against the measured ladder:

| N | measured eff | optimal eff | verdict |
|---|---|---|---|
| 1..15 | 1.000 | 1.000 | fine |
| **16** | **0.500** | **1.000** | **defect — a whole core lost** |
| 17 | 0.500 | 0.500 | **already optimal, no defect** |
| **20** | **0.333** | **0.500** | **defect — a core ran 3 threads** |
| **32** | ~0.059 | 0.500 | **defect — one core ran 17 threads** |

The N = 17 column matters: it is the one point on the ladder where the observed
0.500 is *not* a bug, and a fix that "improves" it would be doing something
wrong. Claiming credit for it would be exactly the kind of once-measured,
propagated-then-retracted result this project has been burned by.

The metric to use above `ncpus` is therefore the **imbalance ratio**

```
imbalance = observed max thread time / (ceil(N / ncpus) * unit)
```

where 1.0 is optimal regardless of N. Today that is **2.0 at N = 16**, 1.5 at
N = 20 and ~8.5 at N = 32. The fix must drive all of them to ~1.0, and must
leave N = 17 exactly where it is.

---

## 5. How the fix will be proved

Primary evidence is the ladder; everything else is corroboration.

1. **The ladder, interleaved A/B**, fixed vs. canonical kernel, several repeats,
   variance shown. Target: efficiency at N = 16 goes from 0.500 to ~1.000, and
   **N = 17 stays at 0.500** because that is already optimal (§4.6).
2. **Duration scaling as a negative control on the *fix*:** the N = 16 max time
   must now stay flat at ~1x the unit for 2000 ms and 8000 ms units, where today
   it is 2x both times.
3. **Graceful degradation, not serialisation**, judged by imbalance ratio
   (§4.6), which is 1.0 at the optimum for every N: N = 20 should go from 1.5 to
   ~1.0 (eff 0.333 → ~0.500) and N = 32 from ~8.5 to ~1.0 (eff ~0.059 → ~0.500)
   — *not* one CPU running 17 threads for 51 s.
4. **Mixed workload negative control:** some threads sleeping, some CPU-bound.
   Must not regress, and must not start migrating the sleepers.
5. **Migration thrashing, counted, not inferred from throughput.** `arm64` has a
   working `sched_getcpu()` — on non-x86 it is `_kern_get_cpu()`
   (`src/libs/gnu/sched_getcpu.cpp:85`), reachable from userland via
   `libgnu.so`. Each worker can sample its own CPU and count transitions, giving
   a **direct migration count with no kernel change and no image bake**. This
   run will be kept *separate* from the timing ladder so the syscall cost cannot
   contaminate the primary evidence.
6. **Artifact verification:** `objdump` the `smpscale` hot loop to confirm it is
   the intended register-bound dependency chain, and match `sha256sum` between
   the build host and the node before trusting any number.

### What cannot be tested here

**An SMT topology.** Graviton has none, and `c7g.metal` panics in GICv3, so
there is no arm64 machine in this project with `CPUCount() > 1`. The SMT case is
therefore covered by reasoning only, stated explicitly:

- For any core at or below 100% per-CPU demand the clamp was inactive, so
  behaviour is **bit-identical** to today — this covers essentially all current
  x86 SMT behaviour, because with `CPUCount == 2` a core needs demand above 2000
  to exceed the clamp.
- Where it does differ (an SMT core with more than 2 saturated threads), the new
  behaviour is the same correction being made on arm64, with `threadLoad` still
  correctly halved by the divisor (§3).
- The `_RequestPerformanceLevel` clamp (§4.2) is architecture-independent.

This is a residual risk and is recorded as such. It is mitigated by the
blast-radius argument, not eliminated.

---

## 6. Hypotheses considered and rejected

Kept deliberately, per project rule that dead hypotheses get written down.

- **"The `CPUCount()` divisor is the bug."** No — it is the correct
  normalisation and removing it would break SMT. §3.
- **"`kLoadDifference` is the wrong hysteresis and needs raising or lowering."**
  No — at 20% of `kMaxLoad` it correctly declines the post-migration state
  (1200 >= 1000) and correctly declines N = 20 and N = 32. It was never the
  problem; it only looked like one because it was being compared against a
  clamped value. §4.1.
- **"It is transient placement noise / slow rebalancing."** Refuted by the
  duration-scaling property: the imbalance lasts the whole run, 16 s at an
  8000 ms unit. §1.
- **"Saturated threads never reach `rebalance()`, so its arithmetic is
  irrelevant."** Checked and refuted: quantum expiry on a doubled-up core routes
  through `enqueue(oldThread, false)` → `Rebalance()` every quantum. §2.4.
- **"`fNeededLoad` of two threads sharing a CPU is 500 each, so `fLoad` is 1000
  and there is no signal to recover."** This would have killed the fix. Refuted
  by reading the accounting: run-queue wait time is excluded from
  `fMeasureAvailableTime`, so each reports 1000 and `fLoad` is genuinely 2000.
  §2.1.
- **"A prior shell-loop measurement proved SMP works, so this cannot be real."**
  That measurement used `date` at 1-second resolution against a ~2-second run.
  At efficiency 0.500 the parallel run takes 2x the serial one, and 1 s
  quantisation over 2 s cannot distinguish 1x from 2x. It got the right sign and
  hid this entire defect.
