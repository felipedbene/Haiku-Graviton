# arm64 PMU sampling profiler — staged design plan

**Status:** E-PMU-1a/1b/2 implemented, but BLOCKED on hardware. On real Graviton
(Neoverse-V1/c7g, verified on 16xlarge and large) the E-PMU-1b counter-overflow
interrupt does **not fire**: the counters read correctly and the handler installs
cleanly on PPI INTID 23, yet no overflow is ever delivered, so E-PMU-2 collects
no PMU-driven samples and profiling falls back to the software timer. The
open blocker is making the overflow interrupt fire (see E-PMU-1b below). This
file records the decomposition and the one non-obvious conflict so the
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
   **BLOCKED (hardware):** on Graviton the overflow interrupt never fires. With
   all cores pegged the per-CPU overflow count never advances and E-PMU-2's
   `profile` shows zero PMU samples (only software-timer ticks). The counters
   themselves work (core clock is measured off `PMCCNTR_EL0`) and the handler
   installs on INTID 23 without error, so the fault is in interrupt delivery, not
   counting. The hardcoded INTID 23 (PPI 7) is the most likely culprit: the
   authoritative PMU interrupt number is the **MADT GICC Performance Interrupt
   GSIV**, which nothing in-tree parses — the comment in `arch_pmu.cpp` assumes
   Graviton follows the architected PPI 7, unverified against the actual MADT.
   Next step: parse the GICC Performance Interrupt GSIV from the MADT and install
   on that INTID instead of hardcoding; cross-check that Linux `perf record`
   (which uses exactly this GSIV) samples on the same instance.
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
   overflow, not merely on the handler being installed, so that on hardware
   where 1b's interrupt never fires the software timer keeps profiling rather
   than being suppressed into collecting nothing. Because 1b's interrupt does
   not fire on Graviton today, this path is code-complete but cannot be
   hardware-proven: `profile` there runs on the software timer and the PMU
   sample count is zero. It will light up once 1b delivers overflows.

## Hardware-verification gates (all on real Graviton, before any merge)
- No interrupt storm: bounded IRQ rate under a CPU-bound load; system responsive.
- Inert when `arm64_pmu` off: overflow IRQ not enabled, counters at 0, per-thread
  cycles unaffected.
- (C) still correct with the dedicated-counter design: per-thread cycle deltas
  match wall-time × core clock with the profiler active.
- A known hot loop appears as the top function in the profile.
- Boot clean on c7g and c9g.
