# A latency account for the arm64 network transmit path — Part I

> **STATUS: Part I only. Nothing in this document is a measurement of the
> transmit path.** It is the pre-run account for #582: the buffers enumerated
> from trunk, the mechanism those buffers can and cannot produce, and the
> falsifiers registered *before* a run, so that whatever the run reports it
> cannot be rationalised after the fact. Part II, the results, is not written.
>
> The session that wrote this had no usable AWS credentials and **took no
> hardware measurement.** Every line below is either a source citation against
> `origin/graviton`, arithmetic over such a citation, or a labelled prediction.

**What #582 reports**, from `ena-receive-latency-account.md` §4 (measured
2026-08-25, `c7g.16xlarge`, Graviton3, MTU 9001, single ENA queue, on the
**pre-CoDel, pre-ReceiveRing** tree, with an instrument — `rxlat` — that no
longer exists):

| offered load | ICMP RTT min/avg/max |
|---|---|
| idle | 0.327 / **0.348** / 0.408 ms |
| 1 transmit flow | 0.370 / **0.438** / 1.682 ms |
| 4 transmit flows | 2.53 / **2.76** / 3.08 ms |
| 8 transmit flows | 7.41 / **7.74** / 10.4 ms |

Those rows are quoted, not reproduced. The author of that document labelled them
"a by-product worth recording", which is the correct weight: **an ICMP round trip
traverses the receive path inbound and the transmit path outbound**, and on this
stack the reply is built and sent on the receive side's own thread
(`icmp.cpp:535`). So 7.74 ms establishes that *something* costs ~7.4 ms under
transmit load and says nothing about which path it is on. The title of #582 —
"the transmit path has its own standing queue" — is the thing to be tested, not
a premise.

---

## 1. The transmit path's buffers, enumerated on trunk

Verified against `origin/graviton` at `82e35f5df3`. The important negative first.

**There is no software transmit queue of any kind.** The only `net_fifo` on the
ENA datapath is `interface->receive_queue` (`device_interfaces.cpp:282`, 16 MiB);
the per-queue FIFOs beside it are multiqueue machinery the driver never
activates. Nothing resembling a qdisc exists. The egress chain

```
TCPEndpoint::_SendQueued() -> send_routed_data -> ipv4 -> datalink_send_data
  -> ethernet_frame_send_data -> ethernet_send_data -> ioctl(ETHER_SEND_NET_BUFFER)
  -> ena_send()
```

is **synchronous on the caller's thread from end to end**
(`ethernet.cpp:326` is the ioctl; `ena_send()` is `ena.cpp:3234`). There is no
transmit thread in the driver either: completion reclaim runs only from
`ena_send()` itself and from the watchdog.

That leaves exactly two buffers.

- **B1 — `fSendQueue`, per socket.** `socket->send.buffer_size`, 256 KiB by
  default, auto-grown toward 2xBDP. Eight flows are eight *independent* queues,
  and **none of them is in an ICMP probe's path**, so B1 cannot be the figure
  above. It is the right buffer for a different question (application
  `write()`-to-wire latency) and the wrong one for this issue.
- **B2 — the ENA transmit bounce slots and submission queue.** The *only* buffer
  any two flows share. `ENA_DEFAULT_TX_RING_SIZE` is 512 (`ena.h:95`), but in LLQ
  (device placement) mode the depth is taken from `max_llq_depth` and **halved
  again for 256-byte descriptor entries when the device reports no separate wide
  limit** (`ena.cpp:1074-1106`) — and the attach log on this hardware reports 256
  byte entries, so the effective depth may be 256. A 9001-byte frame costs
  `ceil(9001 / ENA_PACKET_BUFFER_SIZE)` = `ceil(9001/1920)` = **5** bounce slots
  and as many submission-queue descriptors, and admission demands `segments + 1`
  free (`ena.cpp:3282-3284`). So the ring holds on the order of **42-102 jumbo
  frames, i.e. 0.4-0.9 MB, for the whole machine** — the spread is dominated by
  the 256-vs-512 question, which the attach log answers and which Part II must
  record before anything else.

**One queue pair, one lock.** `ENA_IO_QUEUE_PAIRS` is 1 with a fixed
`ENA_TX_QUEUE_ID 0` (`ena.h:86-87`) and a single `txLock`. The stack is already
multiqueue-capable; the driver is what pins it to one. That is DeBeOS as shipped,
so "single ENA queue" is not a harness artefact — it is the only configuration
trunk can produce.

---

## 2. The mechanism this enumeration actually implies

The natural reading of B2 — treat the ring as a queue the probe waits in, divide
occupancy by drain rate — gives 0.4-0.9 MB at 5-10 Gbit/s = **0.37-1.5 ms**, a
fifth of the inflation at best. On that reading B2 is ruled out and #582 has no
candidate left on the transmit side at all.

**That reading is the wrong model.** The ring is not a buffer the ICMP packet
waits in. It is a **gate that suspends the only thread that drains the buffer the
ICMP packet does wait in.**

Three facts, each a citation:

1. **TCP does its transmit work on the receive side's thread, under the receive
   side's lock.** `SegmentReceived()` takes `fLock` and calls `_SendQueued()`
   while still holding it (`TCPEndpoint.cpp:2987-2988`). #414 moved the
   *acknowledgement* emission out from under that lock; `SEND_QUEUED` stayed
   inside. The thread that calls `SegmentReceived()` is
   `device_consumer_thread` (`device_interfaces.cpp:223`), which is the **single**
   drain of `interface->receive_queue`. The same thread builds and sends ICMP echo
   replies inline (`icmp.cpp:535`).
2. **When the ring is full, `ena_send()` parks its caller indefinitely.**
   `acquire_sem(device->txCompleted)` with no timeout (`ena.cpp:3309`), inside a
   loop that re-tests ring occupancy. There is no queue to fall back on and no
   drop: the thread waits.
3. **Therefore a full transmit ring stops the receive queue from draining**, for
   as long as the wait lasts, and the receive queue is 16 MiB with no effective
   discipline over the traffic in question (§3).

Composed, the ring's *small* depth makes things worse rather than bounding them:
a shallower ring gates the consumer more often. And the ICMP probe is behind the
stall twice — once queued in the FIFO the stalled consumer is not draining, and
once at the same gate when its own reply is sent.

This is the precise sense in which #582's title may be both right and misleading:
the *cause* can be entirely on the transmit side while the *queue* is entirely on
the receive side. No AQM can be applied to it, because an AQM needs a queue you
own and the transmit side has none.

### 2.1 A second mechanism, not previously named: `txCompleted` banks stale credit

`ena_io_interrupt()` releases one unit on `txCompleted` on **every** io
interrupt, whether or not any sender is waiting and whether or not the interrupt
had anything to do with transmit (`ena.cpp:494-495`). Nothing ever drains the
accumulated count. The receive side of the same handler is explicitly *not* like
this: `ena_receive()` reads `get_sem_count()` and swallows the backlog in one
`acquire_sem_etc()` before going round again (`ena.cpp:3691-3694`).

The asymmetry cuts both ways, and the two directions have opposite signatures:

- **Before the banked credit runs out**, `acquire_sem()` returns immediately, so
  the wait loop degenerates into a **poll**: unlock `txLock`, take a free
  semaphore unit, retake `txLock`, reclaim, re-test. On any receive-carrying run
  the bank is large. With eight senders plus the consumer that is a thundering
  herd on one mutex, once per turn of the loop rather than once per frame — which
  is a contention mechanism of the right *magnitude* for a superlinear curve,
  where the straightforward "8 threads x ~19 us of transmit work ~ 150 us" bound
  is two orders too small.
- **After it runs out**, one interrupt releases exactly one unit, so N parked
  senders need N interrupts to all wake even when a single completion batch freed
  enough descriptors for all of them.

Both are hypotheses. Both are now directly measurable (§4).

---

## 3. Why the receive-path fix does not reach this, and why "fix `min_bytes`" is not the answer either

The receive-FIFO CoDel discipline (`4a341a7a96`) is on trunk and is the obvious
candidate to have already caught a standing queue in `receive_queue`. It cannot,
for two independent structural reasons.

**Reason one: the minimum-queue gate.** `codel_update()` returns false — *and
resets `first_above_time`*, so the interval timer never completes — whenever
`queueBytes < codel->min_bytes` (`utility.cpp:493-497`). `min_bytes` is a fixed
`NET_FIFO_CODEL_MIN_BYTES` = **65536** (`utility.h:168`). A queue of pure ACKs is
~66 bytes a packet, so the discipline cannot act until roughly **a thousand**
queued ACKs. If the transmit load is ACK-clocked at the measured ~0.40
ACKs/frame, 7.4 ms of standing queue is ~19 KB — invisible to CoDel *by
construction*. (Note separately that CoDel's own specification puts this guard at
one MTU; 65536 is 7x one jumbo frame and 44x one standard frame even for
full-size traffic. That is worth revisiting on its own merits and is **not** a
#582 fix, for the next reason.)

**Reason two: the shed is structurally inert on an ACK queue.** This stack's
CoDel does not free a shed buffer; it flags `NET_BUFFER_ECN_CE_MARK` and lets the
L3 translate point promote an ECT packet to CE, or drop a non-ECN one
(`utility.cpp` `fifo_dequeue_buffer_codel()`). But DeBeOS TCP marks **only data
segments** ECT(0): "retransmits, pure ACKs and SYN/SYN-ACK stay Not-ECT"
(`TCPEndpoint.cpp:3327-3328`), which is correct per RFC 3168. So a standing queue
made of pure ACKs is entirely Not-ECT, the gentle half of the mechanism is
unavailable, and the only remedy left is **dropping ACKs** — which relieves the
latency by breaking ACK clocking, i.e. by throttling the sender. That is a trade,
not a fix, and #582 has no data with which to justify it.

Together: lowering `min_bytes` would make CoDel *see* the queue H1 predicts and
still give it nothing useful to do about it. **The candidate fixes for #582 are
all serialization fixes, not sizing fixes** — get `_SendQueued()` off the single
receive-FIFO consumer, or out from under `fLock`; release `txCompleted` with a
count and drain its stale bank; or give the driver more than one queue pair. Each
is a non-trivial change to lock discipline on the busiest thread in the system,
and none should be attempted against a two-month-old round-trip number from a
retired instrument on a superseded tree.

---

## 4. The instrument this lands, and how to read it

The mechanism in §2 was unmeasurable: nothing in the tree recorded whether
`ena_send()` ever parked, let alone for how long. It now does. Four counters,
written only under `txLock` and touched **only when the ring is actually full**,
so the unobstructed path pays nothing:

| `ena_fault stats` line | meaning |
|---|---|
| `tx ring-full stalls` | frames that found the submission queue full, and the share of all transmit frames that is |
| `tx stalled total` | wall-clock time senders spent parked, and the mean per stall |
| `tx waits/stall` | `acquire_sem()` calls per stall |
| `tx worst stall` | the largest single stall, a since-boot high-water mark |

Times come from `system_time()`, i.e. `CNTVCT_EL0`/`CNTFRQ_EL0`, which is
accurate on this platform — the arm64 measurement bugs (#140, #103) are in
counter *configuration*, not in the clock rate. **Do not judge any arm of this
run by CloudWatch `CPUUtilization` or by `top`**: both under-report by ~55x on
Haiku arm64 (#140), and conflating CPU utilisation with wall-clock occupancy is
exactly what let the receive path look 97% idle while sitting at its hard ceiling
for months.

The three readings are mutually exclusive, which is the point of landing the
instrument before the hypothesis:

- **Gate confirmed.** `tx worst stall` in milliseconds and `tx stalled total`
  a substantial share of the run's wall clock ⇒ §2 is the mechanism; the standing
  queue is in `receive_queue`, caused from the transmit side, and the fix is to
  stop the receive consumer from being the thread that waits.
- **Gate refuted.** `tx worst stall` in tens of microseconds and
  `tx stalled total` negligible ⇒ the ring never gates anything, and what remains
  is pure CPU occupancy of the consumer (~19 us of transmit work per frame
  against a ~25.6 us ACK inter-arrival). **This instrument cannot see that**, and
  the one that could — `rxlat`, wall-clock thread occupancy — was retired and
  never forward-ported. Saying so now is what stops a null result being read as
  "nothing is wrong".
- **Poll confirmed.** `tx waits/stall` well above 1 with a short mean ⇒ §2.1: the
  stalls are being spent spinning on banked credit and retaking `txLock`, not
  sleeping. This is the one reading that makes the lock, rather than the ring, the
  thing to change.

---

## 5. Falsifiers, registered before the run

- **P1.** The 7.7 ms substantially reproduces on trunk at 8 TCP transmit flows:
  4-12 ms, not under 2 ms. No trunk change touches the transmit path, and the
  receive CoDel that would otherwise catch it is gated off by §3.
- **P2.** The 8-flow transmit aggregate is 5-10 Gbit/s. **If it is under
  ~1 Gbit/s, the ring's own occupancy explains the whole delay on the naive model
  of §2, the composition argument is unnecessary, and #582 is hardware-bounded.**
  This aggregate has never been measured anywhere in this tree; it is the single
  number the run exists to report.
- **P3.** `tx ring-full stalls` is a non-trivial share of transmit frames at 8
  flows (>1%) and ~0% at 1 flow. **If it is ~0% at 8 flows, §2 is dead** and no
  amount of the rest of this document rescues it.
- **P4.** Under 8-flow transmit load the receive FIFO shows `max_sojourn` in
  milliseconds with `peak_bytes` of only tens of KB and `codel dropped` at or near
  zero — readable with no new code by entering KDL after the run and running
  `net_device_interface`, since those are monotonic high-water marks that survive
  it (`device_interfaces.cpp:375-399`; serial console on c7g is available, #635).
  **If `codel dropped` is large, §3's `min_bytes` argument is wrong.**
- **P5.** 8 UDP transmit flows at a comparable frame rate show inflation under
  ~1.5 ms. UDP egress runs on the application's own thread, with no `fLock`, no
  congestion control and an idle receive consumer. **If UDP reproduces the TCP
  figure, the whole "it is really the receive side" argument is dead** and the
  queue is in the driver after all. Needs a ~30-line userland UDP sender
  (`nettput` is TCP-only) — userland only, no kernel change.
- **P6.** 8 flows throttled to the 2-flow aggregate still show more inflation
  than 2 flows. If not, the delay is occupancy, not contention, and §2.1 is dead.
- **P7 (apparatus).** `txDoorbells == txFrames` and
  `txBurstExhausted / txFrames > 0.98`. If this fails, the build or the hardware
  is not what the 2026-08 work measured and nothing in the run is comparable to
  the table at the top.

## 6. Run conditions, if Part II is taken forward

Not a suggestion list — these are conditions under which earlier figures in this
tree were discarded.

- **Instance `c7g.8xlarge`**: baseline == peak == 15 Gbps, so there is no network
  credit window to deplete. `c7g.large` has a 937 Mbit/s baseline and every
  `c7g.large` figure in this tree was taken inside that window; an earlier matrix
  was discarded wholesale over it. `c7g.16xlarge` is not needed — that size
  existed for a Linux-parity question #582 does not ask. Check the pricing API
  before booking; there is no $/hr table in this tree.
- **Peer**: one Linux arm64 instance, same subnet, security group permitting
  intra-group traffic (its absence once produced a false negative on jumbo
  transmit that "looked exactly like a driver bug"). **The peer must fork per
  connection** — a sequential peer once measured 13.5-23.2 Gbit/s against a true
  5.0. Drive both over `aws ssm`, not SSH.
- **Fixed conditions**: MTU 9001 *read back* per row; `nettput -P 262144`, never
  `-w` (`-w 256K` gave 22% spread, `-P 256K` collapsed it to 1.2%); <=2 GiB per
  run; >=20 s idle between runs; discard the first run after each boot; arms
  **interleaved, never ascending**.
- **Per arm, recorded**: idle ICMP control before *and* after every sequence (this
  is what makes an inflation a measurement rather than a claim); the four
  `ena_fault stats` stall lines; transmit frame *and* byte rate cross-bracketed
  against the interface's own counters over the same window, because one counter
  cannot detect its own miscount; the peer's throughput view and `ss -tin`; and
  the KDL `net_device_interface` dump for P4.

## 7. Incidental, found while writing this and not acted on

- **`ENA_IOCTL_GET_ENI_STATS` has no reader anywhere in the tree.** The driver
  fills a 23-field `ena_eni_stats` snapshot that nothing in `src/` ever asks for,
  including the hardware's own `hwTxDrops` — which would be worth having in P2's
  arm. The new counters here deliberately went into `ena_irq_stats` instead,
  because that is the snapshot `ena_fault` actually samples, with reset
  invalidation already built in.
- **`dump_receive_queue_diagnostics()` (`device_interfaces.cpp:1235`) is defined
  and called from nowhere**, and does not print the CoDel block in any case, so
  P4 still needs KDL. Wiring it to an ioctl *and* adding the four CoDel fields
  would make the direct test of §2 readable from userland. One change, both
  halves — not attempted here, to keep this change inside the driver.
- **`ENA_TX_IRQ_INTERVAL = 50` (`ena.h:187`) is inert** on this hardware: the
  device reports `interrupt delay resolution 0` and does not advertise
  moderation. It must not be blamed for anything in Part II.
