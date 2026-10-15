# A latency account for the arm64 network transmit path

> **STATUS: Parts I and II.** Part I (§1-§7) is the pre-run account for #582 —
> buffers enumerated from trunk, the mechanisms those buffers can and cannot
> produce, and falsifiers registered *before* any run. It was written with no
> hardware access and contains no measurement; every line in it is a source
> citation against `origin/graviton`, arithmetic over one, or a labelled
> prediction. **It is left exactly as it was written**, including the predictions
> the run went on to refute, because that is what makes Part II a test rather
> than a story.
>
> Part II (§8 onward) is the hardware run: 2026-10-15, `c7g.8xlarge` Graviton3,
> MTU 9001, canonical AMI `ami-0eee11d43faa67d8f` (hrev59996). Its verdict in one
> line: **#582's premise is wrong. There is no standing queue on the transmit
> path.** The standing delay is real and reproduces, the *cause* is on the
> transmit side, and the *queue* is the receive FIFO — §2's composition argument
> is right in outline and wrong in detail, because the coupling is the consumer
> thread's own CPU occupancy and not the submission ring gating it.

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

---

# Part II — the hardware run

Run 2026-10-15. **Target**: one `c7g.8xlarge` (Graviton3, 32 vCPU, baseline ==
peak == 15 Gbps, so no network credit window), canonical AMI
`ami-0eee11d43faa67d8f`, `hrev59996` of 2026-10-13, MTU 9001 read back per arm.
**Peer**: one `c7g.8xlarge` Linux (AL2023) in the same subnet, running
`nettput-peer.py --concurrent`, which forks per connection. Neither instance was
a spot instance. No reboots: the canonical image's SSM agent never re-registers
after one, so every arm in this run is from a single boot of a single instance,
which also means every arm shares one set of confounds rather than one each.

**Method per arm**: an external ICMP probe from the peer (`ping -c 200 -i 0.05`)
inside the load window, bracketed by an idle control (`ping -c 60 -i 0.05`)
*before and after*; the target's own `ena_fault stats 20` and `ifconfig` byte and
packet deltas over the same 20 s window, cross-bracketed against each other;
`txprobe` (below) on the target inside the same window; and the peer's `ss -tin`
view of all eight connections. Arms were interleaved, never run in ascending
flow-count order, with >=20 s idle between them, and the harness sweeps and
`kill -9`s any load process left by a previous arm before starting (one early
0-flow arm was contaminated exactly that way and is excluded).

**Peer-is-not-the-bottleneck control.** This is the confounder that has produced
false numbers in this tree before, so it is checked per arm and not assumed. In
the 8-flow arms `ss -tin` on the peer shows eight distinct `ESTAB` sockets with
`Recv-Q 0` on every one of them, `app_limited` set, and per-connection
`bytes_received` of 1.702-1.721 GB over the window — a 1.1 % spread. Eight
sockets each taking an equal eighth with nothing backed up in any receive queue
is concurrency, not serial service.

## 8. Two instruments this run needed, and why `nettput` could not be one

`nettput` is TCP-only, and TCP is the one protocol whose transmit work this stack
performs *on the receive consumer thread* (§2, fact 1). Any question of the form
"is this the transmit path or the thread that happens to run it?" is unaskable
with a TCP-only load generator. Two small userland tools close that gap, both
added under `graviton/scripts/`:

- **`udpblast.c`** — an open-loop UDP sender (`udpblast <host> <port>
  <payload-bytes> <seconds> [pps-cap]`). UDP egress runs on the *caller's* thread
  with no congestion control, and when it is aimed at a blackholed port there is
  no return traffic at all, so the receive consumer stays idle. It therefore
  offers transmit load through the same `txLock`, the same bounce slots and the
  same submission queue as TCP, with the consumer thread taken out of the circuit.
  Measured in these arms: 10 receive frames/s against 56 000 transmit frames/s,
  i.e. the consumer really is idle.
- **`txprobe.c`** — times `send()` on a UDP socket from an ordinary userland
  thread and reports min/p50/p90/p99/max/mean plus a `>=1 ms` count, timed with
  `system_time()` (`CNTVCT_EL0`, accurate here; the arm64 measurement bugs #140
  and #103 are in counter *configuration*, not clock rate). One warm-up send
  settles route and ARP. This is the direct measurement of the egress chain —
  stack entry to doorbell — *from a thread that is not the consumer*, and it is
  the instrument that decides #582.

Both are built with the arm64 cross-tools and copied to the target; neither needs
a kernel change, which is what made them runnable against the canonical AMI. They
are not in any Jamfile on purpose — they are measurement scaffolding, not part of
the image. The target has no compiler, so they are cross-built on the host:

```sh
G=generated.arm64
$G/cross-tools-arm64/bin/aarch64-unknown-haiku-gcc -O2 -o txprobe \
  graviton/scripts/txprobe.c \
  -I headers -I headers/posix -I headers/os -I headers/os/kernel \
  -I headers/os/support -I headers/os/storage -I headers/config \
  -I $G/cross-tools-arm64/lib/gcc/aarch64-unknown-haiku/13.3.0/include \
  -nostartfiles \
  $G/objects/haiku/arm64/release/system/glue/arch/arm64/crti.o \
  $G/objects/haiku/arm64/release/system/glue/start_dyn.o \
  $G/objects/haiku/arm64/release/system/glue/init_term_dyn.o \
  -L $G/objects/haiku/arm64/release/system/libroot -lroot \
  -L $G/objects/haiku/arm64/release/system/libnetwork \
  $G/objects/haiku/arm64/release/system/glue/arch/arm64/crtn.o
```

(`udpblast` is the same line without the `os/kernel` and `os/support` includes,
which only `txprobe` needs for `system_time()`.)

## 9. The phenomenon reproduces; the number does not

TCP transmit, `nettput -c <peer> -p 10900 -n 64G -P 262144` per flow. ICMP from
the peer; throughput and frame rate from the target's own counters. Repeated arms
are listed in the order run.

| flows | idle control | ICMP under load min / **avg** / max | aggregate | tx frames/s |
|---|---|---|---|---|
| 0 | 0.194/**0.219**/0.363 | 0.195 / **0.210** / 0.293 ms | — | 30 |
| 1 | 0.214, 0.223 | 0.190 / **0.231** / 1.357 ms; **0.238** | 5.00, 5.00 Gb/s | 69 384, 69 381 |
| 2 | 0.202, 0.212 | 0.265 / **0.545** / 2.527 ms; **0.576** | 7.08, 6.83 Gb/s | 98 193, 94 753 |
| 4 | 0.210, 0.215 | 0.908 / **1.220** / 3.764 ms; **1.210** | 6.51, 6.42 Gb/s | 90 341, 89 021 |
| 8 | 0.205-0.238 | 1.970-2.126 / **2.415, 2.473, 2.492, 2.499, 2.514, 2.518, 2.562** / 2.93-5.31 ms | 6.35-6.64 Gb/s | 88 039-89 664 |

Read the `min` column first. At 8 flows it is 1.97-2.13 ms against a 0.21 ms idle
control, and the average is 2.5 ms: **every single packet pays ~2 ms**. That is a
standing queue, not a tail — which is the one thing #582's title gets right.

Then read it against throughput: the aggregate is *flat* from 2 flows (7.08 Gb/s)
to 8 flows (6.45 Gb/s) while the standing delay grows 4.6x. The queue buys
nothing. That is the bufferbloat signature.

**P2 — confirmed, and it is the number this run existed to report.** The 8-flow
transmit aggregate is **6.35-6.64 Gbit/s at 88-90k frames/s**, inside the
registered 5-10 Gbit/s band and seven times the "under ~1 Gbit/s" that would have
made #582 hardware-bounded. The ring's own occupancy cannot explain 2.5 ms.

**P1 — falsified as stated.** The registered prediction was 4-12 ms at 8 flows.
Measured: 2.42-2.56 ms, seven arms, spread 6 %. The *phenomenon* reproduces
unmistakably — 10-12x idle, monotone in flow count, standing rather than tail —
but the 7.74 ms figure does not, at this instance size on this tree. The quoted
row was taken on `c7g.16xlarge` on the pre-CoDel, pre-ReceiveRing tree with a
retired instrument, and this run cannot say which of those three differences
accounts for the 3x. #582's headline number should be treated as superseded by
the table above, not as a figure still to be chased.

## 10. The queue is not on the transmit path — two independent controls

### 10.1 Frame rate per se is harmless (P5 — confirmed)

UDP transmit via `udpblast`, 8900-byte payloads, a distinct destination port per
flow (the VPC shapes per 5-tuple at ~5 Gbit/s, so eight flows down one tuple
would have measured the shaper), all ports blackholed at the peer.

| flows | idle control | ICMP under load avg | aggregate | tx frames/s | rx frames/s |
|---|---|---|---|---|---|
| 1 | 0.218 | **0.242** ms | 5.01 Gb/s | 70 020 | — |
| 2 | 0.211 | **0.237** ms | 5.57 Gb/s | 77 883 | — |
| 4 | 0.201 | **0.255** ms | 4.56 Gb/s | 63 689 | — |
| 8 | 0.209, 0.220, 0.221 | **0.291, 0.291, 0.302** ms | 3.85-4.02 Gb/s | 53 856-56 273 | 10 |
| 16 | 0.222 | **0.366** ms | 4.06 Gb/s | 56 732 | — |

The sharpest single comparison is one UDP flow against eight TCP flows: **70 020
transmit frames/s produces 0.242 ms and 89 497 transmit frames/s produces
2.499 ms.** A 28 % difference in offered frame rate against a 10x difference in
standing delay. Sixteen UDP flows at 56 732 frames/s reach 0.366 ms — one seventh
of the TCP figure at two thirds the frame rate.

So the standing delay is not a function of how many frames the transmit path
handles. It is a function of *which thread* handles them.

### 10.2 The egress chain itself has no standing queue

`txprobe` from an ordinary userland thread, inside the same windows as the rows
above:

| arm | p50 | p99 | max | ICMP avg in the same window |
|---|---|---|---|---|
| idle, 0 flows | **2 us** | 5 us | 10 us | 0.210 ms |
| 2 TCP transmit flows | **21 us** | 116 us | 143 us | 0.576 ms |
| 4 TCP transmit flows | **24 us** | 132 us | 170 us | 1.210 ms |
| 8 TCP transmit flows | **23, 23, 25 us** | 136-151 us | 262, 786, 2158 us | 2.49-2.52 ms |
| 8 TCP *receive* flows | **4 us** | 331 us | 484 us | 1.443 ms |
| 8 UDP transmit flows | **157-164 us** | 169-185 us | 174-8091 us | 0.291-0.302 ms |

Two readings matter.

First, under the 8-flow TCP transmit load that produces a 2.5 ms standing ICMP
delay, a userland thread's own `send()` — through the same `txLock`, the same
bounce slots, the same submission queue, the same doorbell — completes in **23 us
at p50 and 150 us at p99**. The transmit path is two orders of magnitude faster
than the delay attributed to it. There is nothing 2.5 ms deep in it.

Second, the dissociation runs the other way too: under 8 UDP transmit flows,
`txprobe` is *seven times worse* (p50 ~160 us, nine competing userland senders on
one `txLock`) while the ICMP standing delay is at its *smallest* (0.29 ms). The
instrument that watches the transmit path and the instrument that watches the
round trip move in opposite directions. They are not measuring the same queue.

**A caveat stated rather than buried.** `txprobe` sends 400-byte datagrams, which
need one bounce slot and admission for two descriptors; a 9001-byte TCP segment
needs five slots and admission for six (`ena.cpp:3264`, `ena.cpp:3282-3284`). A
ring sitting permanently near-full could therefore admit `txprobe` while gating
TCP, so §10.2 alone does not kill §2's gate. §11 is what kills it.

## 11. The causal experiment: price the consumer thread's work

The counters §4 landed would have settled §2 directly, but they are not in the
canonical AMI (they land with #677, merged after `hrev59996`), and testing a
changed driver needs a fresh AMI bake — out of scope for this run, and a reboot
is not a substitute because kernel modules bind at boot. **P3 is therefore
unrun.** §4 had already written down what this run would need if the gate turned
out to be refuted — "pure CPU occupancy of the consumer ... *this instrument
cannot see that*" — so the substitute instrument is a *positive control*: add a
known, measurable amount of per-frame transmit work and see what moves.

The lever already exists. `ena_fault doorbells <n>` makes `ena_send()` write the
submission-queue tail register `n` extra times per frame under `txLock`
(`ena.cpp:3524`). Writing the same tail again tells the device nothing, consumes
no descriptor and changes no DMA — it costs exactly `n` MMIO writes of
consumer-thread time. Arms run in sequence on one boot, with the knob returned to
zero as a mutation arm.

| arm | knob | ICMP min / **avg** / max | aggregate | tx frames/s | txprobe p50 |
|---|---|---|---|---|---|
| 8 TCP transmit, baseline | 0 | 2.126 / **2.499** / 3.515 ms | 6.45 Gb/s | 89 497 | 25 us |
| 8 TCP transmit, injected | **32** | 2.789 / **3.138** / 3.638 ms | 5.14 Gb/s | 71 239 | 24 us |
| 8 TCP transmit, reverted | 0 | 2.089 / **2.514** / 5.035 ms | 6.46 Gb/s | 89 664 | 23 us |
| 8 UDP transmit, injected | **32** | 0.253 / **0.300** / 0.515 ms | 4.04 Gb/s | 56 532 | 158 us |
| 8 UDP transmit, baseline | 0 | 0.263 / **0.291** / 0.465 ms | 4.01 Gb/s | 56 133 | 157 us |

Idle controls bracketing these five arms: 0.225, 0.196, 0.221, 0.203, 0.221 ms.

Four things follow, and the fourth is the answer to #582.

**1. The consumer thread is the throughput bottleneck, not the device.** Adding
software work per frame *reduced* aggregate throughput 20 % (89 497 -> 71 239
frames/s). If the submission ring or the device were the limiter, the software
would have had slack to absorb 32 register writes and the frame rate would not
have moved. It moved, so there was no slack: at 8 flows this stack is
software-bound on one thread. The same reading explains the flat 2-to-8-flow
throughput in §9.

**2. It prices the thread.** 89 497 frames/s is 11.17 us of consumer time per
transmitted frame; 71 239 frames/s is 14.04 us. The 2.87 us difference buys 32
MMIO writes, i.e. **~90 ns per doorbell write** on this hardware — a plausible
device-BAR write cost, and a sanity check that the knob does what it claims.

**3. The standing delay is proportional to that per-frame service time.** Service
time rose 25.7 % (11.17 -> 14.04 us) and the standing delay rose 25.6 %
(2.499 -> 3.138 ms). Nothing about the ring, the device, the wire or the peer
changed between those two arms. That proportionality was not fitted: it is what a
closed queueing system whose population is fixed by TCP's send windows predicts,
and it is the identification this run rests on. Reverting the knob returns the
delay to 2.514 ms and the rate to 89 664 frames/s — so the effect is a property
of the injected work, not of arm order or of the harness.

**4. The same injected work costs nothing when it is not on the consumer
thread.** Under 8 UDP transmit flows the identical 32 writes/frame move the
standing delay by **0.009 ms** (0.291 -> 0.300) and the aggregate by 0.7 %,
because there the transmit work is spread over eight userland threads each with a
~142 us per-frame period, and 2.9 us vanishes into it.

The 2x2 is the whole argument:

| per-frame transmit work | on the receive consumer thread (TCP) | on userland threads (UDP) |
|---|---|---|
| baseline | 2.499 ms | 0.291 ms |
| +32 MMIO writes/frame | 3.138 ms (**+0.639**) | 0.300 ms (**+0.009**) |

Transmit work matters to the standing delay only, and exactly, to the extent that
the receive consumer thread is the thread doing it.

## 12. Where the ~7.7 ms actually sits

**`interface->receive_queue` — `src/add-ons/kernel/network/stack/device_interfaces.cpp:282`.**

The chain, every link already cited in Part I and all of it confirmed by §11:

1. `device_consumer_thread()` (`device_interfaces.cpp:223`) is the **only** drain
   of that FIFO, and `consumer_loop()` (`device_interfaces.cpp:154`) holds
   `interface->receive_lock` across the handler walk on the single-queue path.
2. Every arriving ACK reaches `TCPEndpoint::SegmentReceived()` on that thread,
   which takes `fLock` (`TCPEndpoint.cpp:3033`) and calls `_SendQueued()` while
   still holding it (`TCPEndpoint.cpp:3087-3088`), releasing it only at
   `TCPEndpoint.cpp:3200`.
3. `_SendQueued()` runs the whole synchronous egress chain down to `ena_send()`
   (`ena.cpp:3234`) — including five `sBufferModule->read()` calls copying a
   9001-byte frame into five 1920-byte bounce slots (`ena.h:125`), five
   submission descriptors, checksum-offload context, and one doorbell.
4. The same thread also builds and sends ICMP echo replies inline
   (`icmp.cpp:535`), which is why an ICMP round trip measures this thread's
   service backlog and not the transmit path. That is the sense in which #582's
   "transmit path" reading of the 7.74 ms was an artefact of the instrument.
5. So inbound packets — the ACKs that clock the flows, and the probe — wait in
   `receive_queue` behind ~11.2 us of transmit work per transmitted frame.

**How deep.** 30 499 receive frames/s and a 2.499 ms standing delay put
**~76 packets** in the FIFO. At ~66 bytes for a pure ACK that is **~5 KB** —
against a FIFO limit of 16 MiB. *The buffer's size is irrelevant to this bug.*
The queue is not bounded by capacity; it is set by service rate, and 5 KB is what
~76 ACKs weigh. Anyone attacking #582 by resizing `receive_queue` would be
attacking a number that never binds.

**Why 8 flows specifically.** Nothing special happens at eight. The flow count is
the clue precisely because of how smooth it is: 1 flow 0.23 ms, 2 flows 0.55, 4
flows 1.22, 8 flows 2.50 — a near-perfect doubling, because each added flow adds
an independent ACK stream whose transmit work lands on the same single thread,
while that thread's throughput ceiling is already reached at two flows. Eight
flows is simply where the product of "one thread" and "work per flow" crosses
2 ms. There is no threshold to find.

**§3 holds, and the arithmetic is now measured rather than assumed.** A ~5 KB
standing queue is far below `NET_FIFO_CODEL_MIN_BYTES` = 65536
(`utility.h:168`), so `codel_update()` returns false and resets
`first_above_time` on every dequeue (`utility.cpp:493-497`): CoDel is blind here
*by construction*, and would remain blind at nearly an order of magnitude more
delay. Lowering `min_bytes` would only let it see a queue of pure ACKs, which are
Not-ECT per RFC 3168 (`TCPEndpoint.cpp:3327-3328`), so the only action available
would be dropping ACKs — relieving latency by breaking ACK clocking. Confirming
the CoDel counters directly (`codel dropped`, `max sojourn`) was **P4**, and
**P4 is unrun**: `dump_receive_queue_diagnostics()` has no caller anywhere in the
tree, so those fields are KDL-only; the EC2 serial console is reachable on this
instance (an `ssh` to the serial-console endpoint connects) but the
keystroke-feeding harness needed to drive KDL non-interactively was not got
working inside this session's tooling constraints, and KDL was never entered.
P4 is therefore an open, cheap, worthwhile follow-up — not a result.

### Verdicts on the registered falsifiers

| | prediction | verdict |
|---|---|---|
| P1 | 7.7 ms reproduces at 4-12 ms | **falsified as stated** — 2.42-2.56 ms, 7 arms; the phenomenon reproduces, the magnitude does not |
| P2 | 8-flow aggregate 5-10 Gbit/s | **confirmed** — 6.35-6.64 Gbit/s, so not hardware-bounded |
| P3 | `tx ring-full stalls` >1 % at 8 flows | **unrun** — counters land post-`hrev59996`; no bake, no reboot. Substituted by §11 |
| P4 | receive FIFO shows ms sojourn, tens of KB peak, ~0 CoDel drops | **unrun** — KDL-only, serial console reachable but not driven |
| P5 | 8 UDP transmit flows inflate under ~1.5 ms | **confirmed** — 0.29-0.30 ms at 54-56k frames/s |
| P6 | 8 flows throttled to the 2-flow aggregate still inflate more than 2 flows | **unrun** — superseded by §11, which varies service time directly instead of offered load |
| P7 | `txDoorbells == txFrames`, `txBurstExhausted/txFrames > 0.98` | **partial** — `+0 forced per frame` and `burst left min 0` confirmed; exhaustion measured **0.932** (49 492 811 of 53 100 000) over a mixed lifetime that includes small `txprobe` frames and the injected arms, so the doorbell ratio for the run as a whole is not a trunk property |

And on the mechanisms Part I proposed: **§2's gate is refuted as the operative
mechanism** — §2 was right that the coupling runs through the single consumer
thread, and wrong that the submission ring is what couples it. The ring is not
full; the thread is busy. **§2.1 (`txCompleted` banks stale credit) is
untested** — it needs #677's stall counters, i.e. the same bake P3 needs. It
remains a live candidate for a *different* question (what the stall path costs
when the ring does fill), not for this one.

## 13. What to change — the decomposition, and why no fix ships here

**No behaviour change ships with this document, deliberately.** Everything below
is a kernel or driver change, every such change binds at boot, and verifying one
needs a fresh AMI baked from kernel + stack + driver together. This session could
measure the canonical image and could not bake. A plausible patch with no
hardware A/B is exactly the artefact this project does not ship, so the analysis
lands and the patch does not.

Three levers, cheapest first. All three are measured against the same target:
**per-frame consumer-thread service time**, now a directly measurable quantity
(§11) rather than an inference.

- **D1 — cut the driver's per-frame cost (smallest, fully bounded).** A
  9001-byte frame costs **five** `sBufferModule->read()` calls into five
  1920-byte bounce slots and five submission descriptors, because
  `ENA_PACKET_BUFFER_SIZE` is 1920 (`ena.h:125`) and is shared by both
  directions. A *transmit*-side slot of one maximum frame makes that one copy and
  one descriptor, and drops the LLQ ring entries per frame from 2 to 1 — which
  also un-exhausts the device's 2-entry burst allowance (measured 93.2 %
  exhausted today, `burst left min 0`) and makes doorbell coalescing possible for
  the first time, which `ena.cpp:3478-3502` records as currently worthless at a
  1:1 achievable ratio. Cost: the transmit bounce area grows from 256 x 1920
  (~0.5 MB) to 256 x 9216 (~2.4 MB). The receive slots should stay at 1920, since
  there the device chooses the split. This does not fix #582 — it moves the whole
  curve down by whatever fraction of 11.17 us the copies and descriptors are,
  which §11's method can measure exactly.
- **D2 — get `_SendQueued()` off the consumer thread (the real fix, and the
  expensive one).** `SegmentReceived()` should queue the endpoint as having send
  work and let a separate worker drain it, instead of running the egress chain
  inline under `fLock`. This is the transmit half of **#414**'s lever ("shrink
  what runs under `fLock` in the consumer path ... move the ring push /
  reassembly / ACK bookkeeping out of the lock hold"), and #582 is the first
  measurement of what the *transmit* half of that hold costs: ~11.2 us per
  transmitted frame of the single busiest thread in the system. It changes lock
  ordering and ACK timing on the hottest path in the stack and must not be
  attempted without the §11 instrument on both arms.
- **D3 — not queue count.** The obvious "give the driver more than one queue
  pair" is **already settled in the negative**: #56 was closed won't-do after
  three independent hardware campaigns (c8gn 2026-08-30; c7g 2026-09-19; c8gn on
  the SPSC base) in which 8 queue pairs *lost* 30-33 % of multi-flow aggregate,
  with a Linux control reaching 29.8 Gbit/s on one queue over the same path.
  Queue count is not this stack's limiter and adding it to #582's fix list would
  re-run a closed experiment.

AQM has no role here and should not be reached for. An AQM needs a queue you own,
the transmit path has none (§1), and the receive FIFO's discipline is blind to a
5 KB backlog of Not-ECT packets for the two structural reasons in §3 — both of
which this run confirms by arithmetic rather than overturning.

**The cheapest next measurement is P4**, which needs no code: drive KDL from the
EC2 serial console under an 8-flow transmit load and read `peak`, `max sojourn`
and `codel dropped` out of `net_device_interface`. It would convert §12's derived
~76 packets / ~5 KB into a read number. Wiring
`dump_receive_queue_diagnostics()` (`device_interfaces.cpp:1235`, defined and
called from nowhere) to an ioctl and adding the four CoDel fields would make it
readable from userland for every future run — still no behaviour change, and the
one piece of scaffolding this whole line of work keeps paying for the absence of.

## 14. Incidental, measured but not run down

- **Transmit throughput falls with flow count on one queue pair.** UDP transmit
  aggregate: 70 020 frames/s at 1 flow, 77 883 at 2, 63 689 at 4, 53 856-56 273
  at 8, 56 732 at 16 — a 23 % decline from the 1-flow figure, with eight or
  sixteen userland senders serialized on one `txLock` and one queue pair
  (`ena.h:86-87`). TCP transmit shows the same shape (7.08 Gb/s at 2 flows,
  6.45 at 8). This is a *throughput* observation and plays no part in the #582
  latency analysis; the `txprobe` p50 of ~160 us under 8 UDP flows versus 2 us
  idle is consistent with it. It is recorded here rather than filed because the
  attribution to `txLock` is by elimination, not by a lock-occupancy instrument,
  and because its obvious remedy is the queue count #56 closed.
- **The 8-flow ACK ratio is 0.34, not the 0.40 Part I assumed** (30 499 receive
  frames/s against 89 497 transmit frames/s). It makes no difference to any
  conclusion; §3's arithmetic is unchanged in kind.
- **`ena_fault -h` and `ena_fault help` both print "ioctl failed: Not a tty"** on
  this image, while `ena_fault stats <n>` and `ena_fault doorbells <n>` work
  normally. A usage path is being routed through the device ioctl.
- Still true from §7 and worth repeating because this run needed both: **
  `ENA_IOCTL_GET_ENI_STATS` has no reader** anywhere in the tree (its
  `hwTxDrops` would have been a useful cross-check on every arm above), and
  **`dump_receive_queue_diagnostics()` has no caller**, which is what made P4
  KDL-only and therefore unrun.
