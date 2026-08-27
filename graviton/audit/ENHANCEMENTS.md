# ENHANCEMENTS.md — curated DeBeOS enhancement backlog

The enhancement analogue of `PRIORITY.md`. Unlike the bug audit (which a static analyzer feeds), an
enhancement is a *chosen* improvement — so this list is **hand-curated**, and each entry earns its
place by being (a) on the exercised active surface, and (b) carrying a **measurable acceptance
criterion** that a Graviton hardware run can show. The `debeos-enhance` workflow reads this file,
implements each as a full diff in a throwaway worktree, and runs a pragmatic champion-vs-skeptic
review. Nothing here is committed or shipped by the workflow — it produces a review batch.

> **These are candidates, not commitments.** An entry being listed does not mean it is worth doing;
> the workflow's `reject` / `needs-decomposition` verdicts exist precisely to say so. Every claim
> below is grounded in this project's own measurements (publishable); add new entries the same way —
> measured motivation + an observable acceptance criterion, no internal detail.

## Format

Each row: **id · subsystem · title · description (what to build) · rationale (why) · acceptance (the
observable behaviour that proves it)**. Keep one enhancement per row; if a row needs several
independent diffs, expect the workflow to return `needs-decomposition` with a proposed split.

## Backlog

| id | subsystem | title | description | rationale | acceptance |
|----|-----------|-------|-------------|-----------|------------|
| E1 | ena | Adaptive interrupt moderation | Tie the ENA interrupt-moderation interval to the observed packet rate instead of a fixed interval. | Interrupt moderation is on but static; a fixed interval trades latency at low pps for overhead at high pps. | Under a sustained high-pps iperf3, IRQ/s drops materially vs the fixed interval with no loss of throughput and no added latency at low pps. |
| E2 | network-stack | ECN for the receive standing-queue | Add ECN marking/response on the receive path so congestion is signalled by marking, not by tail-drop + retransmit delay. | The receive-ceiling work showed a 16 MiB FIFO standing queue dominates transit; a CoDel-style drop fix hit a TCP loss-delay wall, and ECN is the mechanism that avoids the loss penalty. | Under the standing-queue workload, goodput rises and tail latency falls without the loss-triggered retransmit delay the drop-based fix incurred. |
| E3 | kernel/arm64 | Real IRQ CPU affinity | Make `arch_int_assign_to_cpu` actually distribute device interrupts across CPUs instead of leaving every IRQ on CPU 0. | All arm64 IRQs are pinned to CPU 0 today; the scheduler's IRQ rebalancer churns for nothing, and this is the blocker under ENA multi-queue. | On a multi-vCPU instance under network load, device IRQ counts are spread across multiple CPUs (per-CPU IRQ stats), not concentrated on CPU 0. |
| E4 | ena | Multi-queue RX/TX | Give the ENA driver multiple RX/TX queues with per-queue interrupts. | Single-queue caps throughput and pins all interrupt work to one CPU; multi-queue is the path to scaling receive across cores. | Throughput scales across parallel flows and per-queue IRQs land on distinct CPUs. **(Depends on E3; likely returns `needs-decomposition`.)** |

<!--
Add new candidates below this line. Template:
| E5 | <ena|network-stack|bfs|app_server-remote|kernel/arm64> | <title> | <what to build> | <measured why> | <observable acceptance> |
-->
