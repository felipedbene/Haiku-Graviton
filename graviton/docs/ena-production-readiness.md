# ENA driver: production-readiness backlog (2026-08-22)

Derived from a file-level comparison of our driver against the reference host driver.
Some claims below were re-verified by hand; those are marked **[verified]**. The rest
come from the comparison pass and are marked **[reported]** — trust them enough to plan
with, not enough to skip reading the code before acting.

> ## Re-checked against `graviton` on 2026-08-24 — the P3 section is largely wrong now
>
> **A `[verified]` tag records that a claim was checked *once, on 2026-08-22*. It is not
> a guarantee of currency, and at least one `[verified]` claim below has since become
> false — which is worse than an untagged one, because the tag suppresses re-checking.**
>
> | Item | State on 2026-08-24 |
> |---|---|
> | Fault injector inert, no `-D` in the Jamfile | **FIXED** — `2ef6ffe3e5` makes it a build switch (`HAIKU_ENA_FAULT_INJECTION`), plus keep-alive / hold-reset / extra-doorbell ioctls |
> | P3: "multi-queue, RSS and jumbo frames are blocked in the stack" | **All three wrong now** — see the box in §P3 |
> | P3: doorbell / ack / RX-refill batching is "available today" | **DEAD** — the device forbids it |
> | P3: TX checksum offload as future work | **MERGED**, −3.54%, p = 0.0079 |
> | P3: bounce copies are "the prerequisite for jumbo" | **False** — jumbo shipped without touching them |
> | Missing `docs/watchdog-design.md`, `FINDINGS.md`, `HANDOFF.md` | **STILL TRUE** — and the count is **eight** citations, not six |
> | `hwRxDrops`/`hwTxDrops` unreadable, no ioctl | **STILL TRUE** |
> | P1 watchdog gaps (`DEVICE_REQUEST_RESET`, `NOTIFICATION` unhandled) | **STILL TRUE** — only `LINK_CHANGE` and `KEEP_ALIVE` are registered |
> | `XXX STRUCTURAL FIX STILL OWED` (unmask before drain) | **STILL OPEN** — `ena.cpp:221`. This is now the project's main network bottleneck, and a pair is on it |
>
> **The P1 list is still the right list.** It is P3 that has been overtaken.

## What the reference actually is

- **`~/Projects/ENA` is *not* a host driver.** It is `ssh://git.amazon.com/pkg/ENA`,
  Annapurna Labs **device-side firmware** (admin-queue handlers, live migration, Alpine
  SoC configs). **[verified]** — remote and HEAD `63f02f88` confirmed. Its value to us is
  as *wire-protocol ground truth* (`ena_defs/`), not as copyable code.
- The host-driver reference is **`github.com/amzn/amzn-drivers`**, FreeBSD side at tag
  `ena_freebsd_2.8.4`. That is our lineage; the Linux copy under
  `kernel/linux/common/ena_com/` is a different (GPL) flavour and is **not**.

## The foundation is sound — the re-vendor idea is a dead end

Our vendored `ena-com/` is **byte-identical** to `ena_freebsd_2.8.4` across `ena_com.c`,
`ena_com.h`, `ena_eth_com.{c,h}` and all five `ena_defs/` headers; only `ena_plat.h`
differs, by design. **[reported, via md5]** There is no drift to reclaim and no latent HAL
bug hiding in a hand-edited fork. The re-vendoring discipline in `ena-com/README.md` has
been followed. **All the gaps are in the driver half, `ena.cpp`.**

## Fixed 2026-08-22

Three P0 bugs, all **[verified]** in the code before and after:

1. **Interrupt moderation was actively disabled** — all three unmask sites passed
   `(0, 0, true, true)`, i.e. *interrupt on every completion*. Now `20`/`50` µs with
   `no_moderation_update = false` (`ENA_RX_IRQ_INTERVAL` / `ENA_TX_IRQ_INTERVAL`,
   `ena.h`). Aggravating factor specific to us: our ISR unmasks **immediately**, whereas
   the reference unmasks only after its cleanup task drains the ring — so we had neither
   mitigation. A `XXX STRUCTURAL FIX STILL OWED` comment records that the unmask belongs
   after the drain; the vector is shared by both directions, so both sides must agree on
   who re-arms.
2. **Unbounded `net_buffer` leak on every reset** — `ena_release_buffers()` freed the
   arrays but never the in-flight `txBuffers[i].buffer`. Every packet in flight at reset
   was leaked; on a repeatedly-wedging NIC that turns a recoverable fault into OOM.
   Deliberately takes no lock: the reset path already holds `txLock`+`rxLock` across the
   call, and the teardown path calls it *after* `mutex_destroy(&txLock)`.
3. **`ena_open()` raced the reset path into a null dereference** — no `resetLock`, no
   `resetting`/`deviceDead` test, then touched queues the reset had freed. Now takes
   `resetLock` (order `resetLock → rxLock`, matching the reset path's
   `resetLock → txLock → rxLock`) and refills under `rxLock`.

## Not yet done, in priority order

### P1 — the rest of the watchdog. We implement 1 of the reference's 5 checks.

These are ~400 lines of already-debugged logic in the reference's `ena.c`, depending only
on a periodic timer and the shared HAL — both of which we have. This is the cheapest
production-risk reduction available. **[reported]**

| # | Gap | Why it matters |
|---|---|---|
| 1 | **Wedged admin queue undetected** (`check_for_admin_com_state`) | The HAL sets `running_state = false` on a command timeout and *nothing notices*; every later admin command fails while the interface looks "up" forever |
| 2 | **`DEVICE_REQUEST_RESET` AENQ not subscribed** | The device's own "reset me" is ignored |
| 3 | **`NOTIFICATION` subscribed with no handler** | Every hint is logged as an unhandled error; also delivers `ENA_HW_HINTS_NO_TIMEOUT` |
| 4 | **Missing-TX-completion watchdog absent** | One lost completion wedges TX permanently. Needs a timestamp in `struct ena_tx_buffer`. Take the 2.8.4 shape — it contains a timestamp-race fix we would otherwise rediscover |
| 5 | **RX-stall / missed-RX-interrupt detection absent** | No recovery from a refill deadlock |
| 6 | **Only 2 reset reasons** vs the reference's 17 | A malformed-descriptor storm becomes an infinite silent error loop |
| 7 | **No post-reset parameter validation** | MAC/max_mtu silently overwritten after reset |
| 8 | **No queue-creation size backoff** | One attempt, then fail |
| 9 | **`ena_uninit_device()` teardown order** frees IO queues *before* removing the interrupt handler — the inverse of both the reference and our own reset path. Latent use-after-free | |

### P2 — observability. Do this early: it is what makes everything else measurable.

- **ENI/customer metrics** (`bw_in_allowance_exceeded`, `pps_allowance_exceeded`,
  `conntrack_allowance_exceeded`, …). The HAL already provides
  `ena_com_get_eni_stats`, `ena_com_get_customer_metrics`, `ena_com_get_cap`. **These are
  how you tell "our driver is slow" from "EC2 is throttling us"** — without them every
  throughput investigation is guesswork. The *transport* is Haiku-specific: there is no
  sysctl tree, so the reference's `ena_sysctl.c` is non-portable in form, reusable only in
  content. Needs an ioctl + a small userland tool.
- We store `hwRxDrops`/`hwTxDrops` and **no ioctl reads them** — write-only counters. No
  packet/byte counters at all. **[reported]**
- ~~**The fault injector is dead in every shipped build**: four `#ifdef
  ENA_DEBUG_FAULT_INJECTION` sites and **no `-D` in the Jamfile**, so `ena_fault` is inert.
  **[verified]** One-line fix to a debug profile~~ — **FIXED and merged 2026-08-24**,
  `2ef6ffe3e5` "ena: make fault injection a build switch instead of a hand edit". The
  Jamfile now carries `if $(HAIKU_ENA_FAULT_INJECTION) { SubDirC++Flags
  -DENA_DEBUG_FAULT_INJECTION ; }`, and the suppress-keep-alive / hold-reset /
  extra-doorbell ioctls exist. Still open: hooks for the remaining failure modes
  P1 adds detection for.
- **`docs/watchdog-design.md`, `FINDINGS.md`, `HANDOFF.md` are cited from ~~six~~
  **eight** places in the code and do not exist.** **[verified 2026-08-22; RE-VERIFIED
  and still true 2026-08-24 — the count was low]** The citations are at `ena.h:173,207,352`
  and `ena.cpp:1246,1635,1685,1722,1887`; the driver directory contains only `Jamfile`,
  `ena-com/`, `ena.cpp`, `ena.h`, `ena_plat.cpp`. Every "verified, see section 8" claim in
  the driver is unauditable.

### P3 — throughput, and the part that is *not* driver work

> ### This whole subsection is superseded (2026-08-24). Do not plan from it.
>
> Every item below has been settled, and three were settled in a direction this text
> does not allow for. **This is the clearest example in the corpus of a `[verified]`
> tag outliving the fact it certified.**
>
> - ~~**Jumbo frames blocked in the stack**~~ — **SHIPPED and hardware-verified both
>   directions at MTU 9001.** `ethernet.cpp` now clamps at
>   `ETHER_MAX_JUMBO_FRAME_SIZE` when the device supports `net_buffer`s, and the
>   driver posts 1920-byte receive buffers so segments fit a `data_node`
>   (`017f72cecd`). It needed a stack clamp fix **and** driver multi-descriptor
>   RX/TX.
> - ~~**Multi-queue + RSS blocked in the stack**~~ — **CANCELLED on evidence, which is
>   a different verdict from "blocked".** "Blocked" invites someone to unblock it.
>   The deciding control: **Linux forced to ONE ENA queue does 29826 Mbit/s against
>   29823 on eight** (`c7g.16xlarge`, interleaved A/B). One queue already carries
>   ~30 Gbps. Also, the IO-queue grant is a **fixed 8 for the whole C7g family**, not
>   a function of vCPU count. See `ena-multiqueue-headroom.md` §5.
> - ~~**Doorbell / completion-ack / RX-refill batching is available today, ~40
>   lines**~~ — **DEAD.** `219d8ab858` measured it: LLQ grants **2 ring entries per
>   burst**, one jumbo frame consumes both, and **99.94% of transmit frames already
>   leave the allowance at zero**. Ratio 1:1; **the saving is exactly zero** at
>   MTU 9001. The 40-line estimate was fine; the premise was not.
> - ~~**TX checksum offload** as future work~~ — **MERGED** (`c2753e0030`),
>   **−3.54%, p = 0.0079**. Beware an earlier **−12.6%** figure: withdrawn as a
>   confound (the send buffer was not pinned). Related: a false
>   `NET_BUFFER_L3_CHECKSUM_VALID` receive claim had to be *removed*
>   (`710f546d51`), and `rx_enabled` reads `0x0` while the device demonstrably
>   validates L4 on ~98% of frames — so it is **not** a usable guard.
> - ~~**Bounce copies are the prerequisite for jumbo**~~ — **false.** Jumbo shipped
>   without eliminating them. Zero-copy transmit is still genuinely open and worth
>   doing on its own terms; note the obstacle: **`get_memory_map` returns NULL.**
>
> **What actually belongs in P3 now:** the interrupt-cadence structural fix at
> `ena.cpp:221`. DeBeOS plateaus at **9.0–10.2 Gbit/s** on `c7g.16xlarge` against
> Linux's **29.8** — that ~3× is the throughput story, and it is a single-queue
> problem. A pair is working it; check before picking it up.

~~See the corrections at items 5 and 6 of `graviton-optimization-plan.md`: **multi-queue,
RSS and jumbo frames are blocked in Haiku's network stack**, not in this driver
(**[verified]**: single-buffer `receive_data` with no queue index, one reader thread per
interface, `ETHER_MAX_FRAME_SIZE` 1514 clamped by the ethernet module). Reclassify them as
stack projects.~~

~~What *is* available in the driver today: **doorbell / completion-ack / RX-refill
batching** (~40 lines, ported from `ena_datapath.c`), then TX checksum offload (logic
ports; the header parsing must be rewritten against `net_buffer`), then eliminating the
TX/RX bounce copies — which is also the prerequisite for jumbo.~~ No reference help for
the last one: `ena_tx_map_mbuf`/`ena_rx_mbuf` are pure `bus_dma`/mbuf.

## Where the reference cannot help at all

`ena_netmap.c` (netmap), `ena_sysctl.c` (sysctl tree), the deferred-TX taskqueue +
`buf_ring` machinery, and the whole SGL/mbuf datapath — `mbuf` appears ~198 times and
`bus_dma*` ~86 times across `ena.c` + `ena_datapath.c`. Haiku has `net_buffer`, `area_id`,
`create_area_etc(B_CONTIGUOUS)` and a blocking read model; none of that maps.

## Things ours does *better* — do not regress these

Stranded-RX-descriptor reclaim on error paths (the reference just resets); bounds-checking
untrusted `len`/`pkt_offset` before the copy (no reference analogue); TX
double-completion / out-of-range `req_id` validation; the `msixConfigured` two-state
teardown; `ena_com_set_admin_auto_polling_mode(true)`, which the reference never sets; and
the barrier reasoning in `ena_plat.h` (everything maps to `memory_full_barrier()` because
arm64 inner-shareable barriers do not order against a PCIe master) — better argued than
the reference's. Note the corollary: we have no `bus_dmamap_sync` fallback, so DMA
correctness rests on Nitro PCIe being I/O-coherent. Right call here, worth remembering.
