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
| E1 (tracked: #108) | ena | Adaptive interrupt moderation | Tie the ENA interrupt-moderation interval to the observed packet rate instead of a fixed interval. | Interrupt moderation is on but static; a fixed interval trades latency at low pps for overhead at high pps. | Under a sustained high-pps iperf3, IRQ/s drops materially vs the fixed interval with no loss of throughput and no added latency at low pps. |
| E2 (tracked: #57) | network-stack | ECN for the receive standing-queue | Add ECN marking/response on the receive path so congestion is signalled by marking, not by tail-drop + retransmit delay. | The receive-ceiling work showed a 16 MiB FIFO standing queue dominates transit; a CoDel-style drop fix hit a TCP loss-delay wall, and ECN is the mechanism that avoids the loss penalty. | Under the standing-queue workload, goodput rises and tail latency falls without the loss-triggered retransmit delay the drop-based fix incurred. |
| E3 (tracked: #56) | kernel/arm64 | Real IRQ CPU affinity | Make `arch_int_assign_to_cpu` actually distribute device interrupts across CPUs instead of leaving every IRQ on CPU 0. | All arm64 IRQs are pinned to CPU 0 today; the scheduler's IRQ rebalancer churns for nothing, and this is the blocker under ENA multi-queue. | On a multi-vCPU instance under network load, device IRQ counts are spread across multiple CPUs (per-CPU IRQ stats), not concentrated on CPU 0. |
| E4 (tracked: #56) | ena | Multi-queue RX/TX | Give the ENA driver multiple RX/TX queues with per-queue interrupts. | Single-queue caps throughput and pins all interrupt work to one CPU; multi-queue is the path to scaling receive across cores. | Throughput scales across parallel flows and per-queue IRQs land on distinct CPUs. **(Depends on E3; likely returns `needs-decomposition`.)** |
| E5 (tracked: #551) | app_server-remote | Viewer vs controller roles in `remote_broker` | Add a role to the broker's token model: accept per-session tokens that carry either `controller` or `viewer`, and drop client-to-server input events for viewer sessions at the broker, before they reach loopback `app_server`. Keep the existing single shared token working as a controller token so no deployment breaks. | The broker authenticates with one static shared token (`RemoteBroker.cpp:445-500`) and has no way to let someone watch without driving, so sharing a session for support, pairing or a demo means handing over full control. | A viewer-token client completes the handshake and keeps receiving display traffic, while every input event it sends is dropped at the broker and never reaches `app_server`; a controller-token client on the same broker is unaffected. Observable in the broker's own counters plus the viewer's display still updating. |
| E6 (tracked: #549 §2) | app_server-remote | Bound the flow queue in seconds of stream, not bytes | `RemoteFlowQueue` bounds the outbound queue by message count and bytes (`kMaxMessages`, `kMaxBytes`). Add a seconds-of-stream bound derived from the connection's observed drain rate, keeping the existing coalesce -> supersede -> degrade-to-`RP_RESYNC` ladder as the overflow action. | A fixed byte bound is the wrong unit for a link whose rate varies: the same 4 MiB is a blink on one client and many seconds of accumulated latency on another, and seconds-of-stream is what actually bounds how far behind a slow client can fall. (Idea only, from a Selkies study — MPL-2.0, nothing lifted.) | Under an artificially throttled client the queue latches `ResyncOwed()` once the backlog exceeds the configured seconds-of-stream, at a byte level that tracks the measured drain rate rather than a constant; a fast client on the same server never latches. |
| E7 (tracked: #549 §3) | app_server-remote | `RP_FRAME_ACK` held-time correction | Define the `RP_FRAME_ACK` payload (frame id, `decode_ms`, `queue_depth`, `held_ms`) and the server-side RTT estimator that **subtracts `held_ms`** and counts **frames-sent-since-last-ack** rather than frame-id distance. Wire format and estimator only — Tier P's tile encoder is not implemented and is explicitly out of scope here. | An ack RTT that includes the client's held time measures the client's scheduler, not the link: a backgrounded browser tab clamps timers to 1 s. And id-distance misreads an idle screen as a stalled client, because damage-gated capture sends nothing when nothing changes. `RP_FRAME_ACK` is currently a reserved opcode with no payload (`RemoteProtocol.h:277`). | Given a synthetic ack stream with known injected `held_ms`, the estimator's RTT equals the injected link delay within tolerance and is **independent of `held_ms`**; and a run in which zero frames were sent produces no backpressure signal regardless of elapsed time or frame-id gap. Both are testable without hardware. |

<!--
Add new candidates below this line. Template:
| E5 | <ena|network-stack|bfs|app_server-remote|kernel/arm64> | <title> | <what to build> | <measured why> | <observable acceptance> |
-->
