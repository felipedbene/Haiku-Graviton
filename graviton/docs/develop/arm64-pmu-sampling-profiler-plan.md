# arm64 PMU sampling profiler — staged design plan

**Status:** design only, NOT implemented. Hardware-gated (GICv3 PPI + interrupt
behaviour on real Graviton). This file records the decomposition and the one
non-obvious conflict so the future implementation does not re-derive them.

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
3. **E-PMU-2 — system_profiler consumer.** Feed the interrupted PC (and a short
   backtrace) from the handler into `src/system/kernel/debug/system_profiler.cpp`
   as a cycle-attributed sample source (today it is timer-only, compiled for
   every arch — this is the cross-cutting piece; mirror the x86 perf/timer
   sampling hook). Depends on 1b.

## Hardware-verification gates (all on real Graviton, before any merge)
- No interrupt storm: bounded IRQ rate under a CPU-bound load; system responsive.
- Inert when `arm64_pmu` off: overflow IRQ not enabled, counters at 0, per-thread
  cycles unaffected.
- (C) still correct with the dedicated-counter design: per-thread cycle deltas
  match wall-time × core clock with the profiler active.
- A known hot loop appears as the top function in the profile.
- Boot clean on c7g and c9g.
