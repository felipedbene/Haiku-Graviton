# arm64 IRQ affinity → ENA multiqueue — staged design

**Status:** design (2026-08-29). Investigation across four layers (SPI/GIC, ITS/MSI,
ENA driver, net-stack/scheduler). Propose-only; every stage is gated on hardware proof
before merge. This is DeBeOS's own implementation.

## Problem

On arm64 every interrupt is serviced on CPU 0, which caps NIC (ENA) throughput to a
single queue on a single core (measured ~4.95 Gbit/s RX / ~4.4 TX single-queue). Two
distinct routing mechanisms are involved, and the ENA-relevant one is **not** the one
the "IRQ affinity" name suggests:

- **SPIs** are routed by `GICD_IROUTER[n]` (per-INTID affinity). `arch_int_assign_to_cpu()`
  (`arch/arm64/arch_int.cpp`) is a stub returning `0`. Its callers
  (`interrupts.cpp` `install_io_interrupt_handler` / `assign_io_interrupt_to_cpu`) use the
  return as an **authoritative CPU id** (`gCPU[return]`), so the stub actively rewrites the
  bookkeeping to "CPU 0" and makes failure inexpressible.
- **MSIs/LPIs** (ENA queue interrupts) are routed by the **GICv3 ITS collection →
  redistributor** map, *not* by `GICD_IROUTER`. The ITS pins one collection to the boot
  CPU's redistributor (`gicv3_its.cpp`), so all MSIs land on CPU 0. **This is the path that
  matters for ENA.**
- The scheduler's IRQ rebalancer calls the stub every tick on a busy CPU, gets `0`, and
  re-parents the IRQ back to CPU 0 — pure `irqs_lock` churn on the hottest CPU, zero effect.

The scheduler thread-migration bug (`rebalance()` unsatisfiable predicate) is **already
fixed + merged** (`220bdd6f9f`, unclamped-load) — not part of this work.

## The load-bearing constraint (sets expectations)

The net stack delivers RX through **one `device_reader_thread` → one 16 MiB
`receive_queue` FIFO → one `device_consumer_thread`** per interface
(`device_interfaces.{h,cpp}`), and the driver contract `receive_data(net_device*,
net_buffer**)` is per-device, one buffer per call. NIC multiqueue **cannot even express
itself into the stack** today. Therefore:

- Spreading interrupts moves per-byte deframe/drain work off CPU 0 (real CPU-headroom win)
  but end-to-end **RX goodput does not scale** until the receive path becomes per-queue.
- The consumer is wall-saturated (~8.88 Gbit/s) and `TCPEndpoint::fLock` is ~47% of the
  ceiling. `fLock` is per-endpoint, so **only multi-flow workloads benefit**; single-flow RX
  stays consumer-bound regardless of queues (that is the ECN / receive-ceiling thread).

Frame the win honestly: **multiqueue = multi-flow throughput + per-core CPU headroom, not
single-flow goodput.**

## Staged plan

### Stage 0 — cheap, standalone, no dependencies (do first)
- **Kill the rebalancer churn.** In `assign_io_interrupt_to_cpu` capture the arch return;
  if it differs from the requested CPU, treat affinity as unsupported (re-add to the old
  CPU, set a one-time `sIRQAffinitySupported = false`), and early-return the rebalancer in
  both `low_latency.cpp` / `power_saving.cpp` when unsupported (mirrors the `gSingleCore`
  fast path). Removes the hot-CPU lock traffic today; hardware-testable via IRQ-load counters.
- **ITS attribute-readback correctness.** `_InitTables` reads back `GITS_BASER` and warns on
  a cacheability/shareability downgrade, but the same check is **omitted** for `GITS_CBASER`,
  `GICR_PROPBASER`, `GICR_PENDBASER`. On real metal a silent PROPBASER downgrade makes an
  LPI **never enable** — invisible, and newly acute once we arm LPIs on every redistributor.
  Add the readback+warn (macros already exist in `gicv3_regs.h`). Lands regardless of the rest.

### Stage 1 — affinity plumbing + honest contract (SPI)
- Add `InterruptController::AssignToCpu(int32 irq, int32 cpu)` virtual to `soc.h` (default
  `return cpu;`). GICv3 impl: for an SPI (`GIC_SPI_BASE ≤ irq < fIrqCount`) write
  `GICD_IROUTER + irq*8 = gic_routing_affinity(gCPU[cpu].arch.mpidr)` then `_WaitForRwp()`;
  return the CPU actually targeted. Wire `arch_int_assign_to_cpu` to `InterruptController::Get()`.
- **Contract:** the int32 return is "the CPU the IRQ now targets" (success = requested;
  can't-route = current/boot). Never negative/OOB — callers do `gCPU[return]`. Matches x86.
- `AssignToCpu` is the single dispatch point: **SPI → IROUTER; LPI(vector ≥ ITS base) → ITS**.

### Stage 2 — ITS per-CPU collections (the ENA-relevant routing)
- Capture every redistributor target in `_InitLpis` (not just the boot CPU) → per-CPU
  collection array; `MAPC` one collection per CPU at init; grow the collection table to
  `max(nCPUs,16)` and decode `GITS_TYPER` HCC/CIDbits to cap.
- Route MSIs: minimum = round-robin `MAPTI` event→collection (`i % nCollections`) — a small
  change that lifts the CPU-0 cap. Full = explicit `SetVectorAffinity(vector, cpu)` re-issuing
  MAPTI+INVALL+SYNC, exposed up through the PCI MSI path so a driver can pin per-queue.
- SYNC discipline: every MAPTI to a new target needs a SYNC against that target.

### Stage 3 — ENA driver multiqueue
- Read io-queue count from device caps (`max_{tx,rx}_{sq,cq}_num`), clamp to
  `min(caps, ENA_MAX_NUM_IO_QUEUES, nCPUs, msix-1)`; replace the hardcoded
  `ENA_IO_QUEUE_PAIRS 1` + scalar per-queue state with arrays; request `1 + N` MSI-X vectors;
  per-queue handlers; bind vector i → CPU i via the Stage 1/2 affinity call.
- RSS is already configured but **inert** — every indirection entry points at the one RX
  queue; change the fill to `i % nRxQueues`. HAL already supports 128 queues.
- Watchdog/reset must iterate all queues; keep the per-pair RX-rearms-liveness invariant.

### Stage 4 — per-queue receive in the stack (the actual RX-scaling unlock; biggest/riskiest)
- Extend the driver→stack contract (`net_device.h`): a queue-indexed receive entry / declared
  queue count.
- Make `net_device_interface` per-queue: arrays of reader thread, consumer thread, and FIFO;
  spawn one reader+consumer pair per queue, each **pinned to the CPU its MSI targets** (flow
  affinity, avoids cross-CPU cache traffic).
- Shrink the per-queue FIFO from 16 MiB (the 256 KiB standing-queue win already measured:
  +25.5% goodput / 29× latency).
- Caveat: multi-flow scales; single-flow to one endpoint still serializes on `fLock`.

## Status log
- **Stage 0 Change 2 (ITS readback): MERGED** to graviton `38918b7c4a` (2026-08-29) —
  hardware-proven on 2-PE (c7g) and 4-PE (c8g) Graviton, warn-only, no false positives.
- **Stage 0 Change 1 (churn-kill, `f796d229a8`): held, folding into Stage 1.** Boot-clean on
  c7g+c8g, no regression, NIC IRQ observed stably pinned (no churn/oscillation under a
  19M-frame RX flood). Its specific latch (fire-once when affinity declined) was NOT
  positively triggered — an RX-only flood doesn't create the core-load imbalance the
  rebalancer needs, and the CPU-bound trigger was env/classifier-blocked. Safe by
  construction (only skips work; x86 unchanged). Stage 1 makes affinity *supported*, which
  exercises the real distribution path — so Change 1 rides in with Stage 1's proof cycle.

## Ordering & dependencies
Stage 0 (independent, immediate) → Stage 1 (independent) → Stage 2 (needs Stage 1 dispatch) →
Stage 3 (needs Stage 2 + affinity API) → Stage 4 (needs Stage 3 + is the RX-goodput gate).
Stages 0–3 deliver CPU headroom + multi-flow spread; Stage 4 is where multi-flow RX goodput
actually rises. Each stage bakes + proves on real Graviton (multi-flow throughput, per-CPU
IRQ counts, checkfs/boot sanity) before a gated merge.

## Verification per stage
- Stage 0: IRQ-rebalance lock traffic gone (KDL/counters); no regression.
- Stage 1: an SPI-backed IRQ observed on a non-boot CPU (KDL `ints`); boot clean on c7g/c8g.
- Stage 2: ENA MSIs observed on ≥2 CPUs (per-queue `ioInterrupts` diagnostics); LPIs enable
  on every targeted redistributor (the Stage-0 readback guards this).
- Stage 3: N RX/TX queues up, RSS spreads flows, per-queue interrupt counts non-zero on N CPUs.
- Stage 4: multi-flow RX aggregate goodput rises above the single-consumer ceiling; single-flow
  unchanged (expected); latency per queue improves with the smaller FIFO.
