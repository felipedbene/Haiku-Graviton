# arm64 PMU sampling profiler — staged design plan

**Status:** E-PMU-1a/1b/2 implemented; the overflow INTID is taken from the
MADT GICC Performance Interrupt GSIV. The earlier "overflow never fires" claim
is **DISPROVEN by measurement — the overflow interrupt DOES fire on real
Graviton** (Neoverse-V1/c7g, hrev59996, PMUVer 5, 32-bit event counters). With
`arm64_pmu` on and a CPU-bound load the per-CPU overflow count advances steadily
and is bounded (e.g. CPU0 1717→3439, CPU1 2628→2925 over ~54 s from `pmu dump`
over serial; no storm). The MADT reports the performance GSIV as **23** — the
architected PPI 7 — which is correct, so the earlier wrong-INTID hypothesis is
also dead. Two real defects remain, and neither is the interrupt number:

- **Defect (a) — serviced rate ~40× too low.** 10^7 cycles at ~2.6 GHz implies
  ~260 overflows/s/core; only ~6–32/s is observed, and the per-core spread
  (CPU0 ~32/s vs CPU1 ~5.5/s) tracks each core's load. The reload/period/re-arm
  code is correct (32-bit reload = `0xFF676980` = −10^7, PMOVSCLR cleared and the
  counter reloaded each overflow, PMCNTENSET/PMINTENSET kept set, counting
  CPU_CYCLES via the dedicated event counter). The deficit is a property of the
  virtualized platform: the guest counts CPU_CYCLES only while its vCPU is
  scheduled, and the overflow interrupt is delivered when the hypervisor injects
  it rather than at the instant the counter wraps — so a tight guest loop that
  rarely exits collapses many overflows into few serviced interrupts. Direct
  counter reads (core-clock measurement) are unaffected because they need no
  injection. **Needs bare-metal re-verification** (c7g.metal): with no
  hypervisor in the delivery path the same reload code should yield ~260/s. This
  is not fixable from guest code.

- **Defect (b) — the software timer never stood down (FIXED).** `profile` sample
  counts scaled exactly 4:1 with `-i` (1000→93671, 4000→23434; ratio 3.997) even
  after overflows were observed — the pure software-timer signature. Root cause:
  the stand-down was decided once, in `SystemProfiler::_InitTimers`, at session
  start. `profile` starts the profiler *before* it runs its workload, so on an
  idle instance no overflow has been serviced yet, `arm64_pmu_sampling_active()`
  is still false, the software timer is scheduled — and it was never revisited,
  so it ran the whole session while the PMU's samples were negligible against
  1000/s. Fix: re-check `arm64_pmu_sampling_active()` on every software-timer
  tick in `_ProfilingEvent`; the timer now retires itself for that CPU on the
  first tick after the workload drives a real overflow, handing over to the PMU.
  x86-neutral (guarded by `__HAIKU_ARCH_ARM64`).

The MADT-GSIV parsing (boot loader → `intc_info::pmu_gsiv` → `arm64_pmu_init`)
remains a correct, merge-safe hygiene fix (no hardcode; degrades gracefully to
the software timer when firmware states no GSIV; c7g.large boots clean and
profiles normally).

Net: on the fleet (virtualized) instances the mechanism now works — the PMU
supplants the software timer once it delivers — but the sample rate is capped by
hypervisor injection (defect a), so the profile is low-resolution there until (a)
is confirmed/addressed on metal. On bare metal, (b) fixed plus (a)'s expected
~260/s should give a usable cycle-attributed profiler.

This file records the decomposition and the one non-obvious conflict so the
implementation did not re-derive them.

## Goal
Turn the read-only PMUv3 facility (`arch_pmu.cpp`) into a cycle-attributed
sampling profiler so `profile`/DebugAnalyzer can root-cause the project's
cycle-level perf questions (net-receive per-byte cost, TCP/FIFO receive ceiling,
ENA per-byte vs per-frame, scheduler migration) on real Graviton. Graviton
exposes the PMU to guests (Graviton4 launch: 21 PMU events to guests), so this
works on the fleet, not only metal.

## What already shipped / is staged
- **A (shipped, canonical):** `arm64_pmu true` default-on — sysinfo reads the
  core clock (2598 MHz on c7g), and the on-demand `pmu` KDL counter facility is
  available. Commit `d56ca1e930`.
- **C (staged, `stage/arm64-pmu-per-thread-cycles`, `7aa5165848`):** per-thread
  CPU-cycle accounting reading `PMCCNTR_EL0` free-running on context switch.

## The conflict that forces the design (found by decomposition, not assumed)
The obvious sampling implementation — enable the **cycle-counter** overflow and
reload `PMCCNTR_EL0` to `-period` each overflow — is INCOMPATIBLE with (C) and
with `arm64_pmu_read()`/frequency measurement, which all treat `PMCCNTR_EL0` as
a free-running monotonic counter. Reloading it injects a `-period` discontinuity
every sample interval; because `arm64_pmu` is default-on, per-thread accounting
would be grossly wrong on every boot, not only while profiling.

## Decomposition (ordered, each independently hardware-provable)
1. **E-PMU-1a — make the counter safe to sample.** Architectural decision:
   dedicate a *separate* PMUv3 **event counter** (programmed to CPU_CYCLES) for
   the sampling overflow and leave `PMCCNTR_EL0` free-running for the clock and
   per-thread cycles. (Alternative considered: a non-reloading overflow scheme
   on PMCCNTR — rejected, it still perturbs the shared counter.)
2. **E-PMU-1b — overflow PPI + bounded handler.** Enable the overflow IRQ for
   the dedicated counter (PMINTENSET/PMOVSCLR), register a handler on the PMU
   overflow **PPI** on GICv3 (verify the exact INTID against the arm64 GICv3
   wiring and the arch-timer PPI registration — do NOT hardcode), reload the
   dedicated counter to `-period` each overflow (bounded, no storm), and bump a
   per-CPU overflow count reachable from the `pmu` KDL command. Inert when
   `arm64_pmu` is off. No profiler coupling yet.
   **Hardware-verified: the overflow interrupt fires and is bounded.** With the
   facility on and a CPU-bound load the per-CPU overflow count advances steadily
   (no storm), the handler installs on INTID 23 (the MADT GSIV), and the counters
   work (core clock is measured off `PMCCNTR_EL0`). The one open item is the
   serviced *rate*, which runs ~40× below the programmed period on a virtualized
   guest — see defect (a) in the Status section. The reload/re-arm code is
   correct; the cap is hypervisor interrupt injection, confirmable only on metal.
3. **E-PMU-2 — system_profiler consumer (implemented).** The overflow handler
   calls `system_profiler_hardware_sample()`
   (`src/system/kernel/debug/system_profiler.cpp`), which walks the interrupted
   thread's stack through the *same* `_DoSample()` path the software profiling
   timer already uses on every arch — so no new sampling or attribution code, it
   is a second trigger for the existing one. While a `profile`/system_profiler
   session runs on arm64 the software timer stands down
   (`arm64_pmu_sampling_active()` in `_InitTimers`), leaving the PMU cycle
   counter the single sample source, so the userland sample count reflects the
   bounded overflow rate rather than a mix of the two. The call is inert unless a
   sampling session is active, and falls back to the software timer wherever the
   PMU sampling facility is unavailable (reduced-PMU sizes, facility off), so
   other arches and reduced-PMU Graviton are untouched. Depends on 1b.
   `arm64_pmu_sampling_active()` gates the stand-down on an *actually serviced*
   overflow, not merely on the handler being installed, so reduced-PMU sizes and
   the facility-off case keep the software timer. The stand-down is re-checked on
   every software-timer tick in `_ProfilingEvent` (defect (b) fix): a session
   started before its workload — the common `profile` case, where no overflow has
   fired yet — starts on the software timer and hands over to the PMU the moment
   the workload drives a real overflow. Before this fix the decision was made
   once at session start and never revisited, so the timer ran the whole session
   and the PMU never became the effective source.

## Hardware-verification gates (all on real Graviton, before any merge)
- No interrupt storm: bounded IRQ rate under a CPU-bound load; system responsive.
- Inert when `arm64_pmu` off: overflow IRQ not enabled, counters at 0, per-thread
  cycles unaffected.
- (C) still correct with the dedicated-counter design: per-thread cycle deltas
  match wall-time × core clock with the profiler active.
- A known hot loop appears as the top function in the profile.
- Boot clean on c7g and c9g.
