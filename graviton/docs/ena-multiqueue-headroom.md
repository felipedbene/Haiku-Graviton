# Multi-queue receive: closed, with a measured reason, and a different project in its place

**Date:** 2026-08-24. **Status: the multi-queue receive + RSS project is CLOSED.**
Not parked, not deferred — closed, because the deciding experiment shows that more
receive queues cannot buy anything on this hardware even in principle. The same
measurements found a **real 2.9× receive deficit** that this project's design
would not have touched, and that is written down here too.

**Hardware.** Haiku receivers: `c7g.4xlarge` (16 vCPU) and `c7g.16xlarge`
(64 vCPU), `us-west-2a`, canonical AMI `ami-0d61e3910062bb80a`, `hrev59996`,
MTU 9001, **unmodified single-queue driver**. Reference receivers and senders:
Ubuntu 24.04 `c7g.4xlarge` and `c7g.16xlarge`, ENA driver, MTU 9001, and the
`c7g.metal` builder. All in one subnet and AZ. Every instance launched for this
work was tagged `ephemeral=true` and terminated afterwards.

**Instruments.** `src/bin/nettput` and `src/bin/netprof` on the Haiku side,
`iperf3 3.16` and `graviton/scripts/nettput-peer.py --concurrent` on the Linux
side, plus `ntclient.py` (a throwaway Linux client for the nettput protocol, so
the *same sender program* could be pointed at a Linux receiver).

---

## 0. Verdict, in the order the evidence arrived

1. **Haiku single-flow receive is at parity with Linux.** 4963 vs 4964 Mbit/s.
   A single TCP flow between instances in this VPC is shaped at 5 Gbps by AWS,
   and both stacks sit within 0.1% of it. There was never any single-flow
   headroom to win. (§2)
2. **Haiku aggregate receive does have a ceiling, and it is real.** On
   `c7g.16xlarge` — 64 vCPU, **30 Gbps sustained** — Haiku plateaus at
   **9.0–10.2 Gbit/s** across 2 to 16 concurrent flows — one flow is 4963, held
   there by §1.1's shaper, not by Haiku — and across one or two distinct sender
   instances, while Linux on the identical instance pair reaches **29.8 Gbit/s**.
   **A 2.9× deficit.** (§3, §4)
3. **But the ceiling is not made of queues, and this is the finding that closes
   the project.** Forcing the *Linux* receiver down to a **single** ENA receive
   queue with `ethtool -L ens5 combined 1` costs it **nothing**: 29826 Mbit/s on
   one queue against 29823 on eight, interleaved A/B, four runs. One receive
   queue on this device can carry 30 Gbps. **So the hardware is not the
   constraint, a per-queue rate limit does not exist, and adding queues to Haiku
   cannot be the fix for a ceiling that one queue clears three times over.** (§5)
4. **Nothing in Haiku is saturated at its own ceiling.** At 10.15 Gbit/s the
   reader thread is at 61% of one core, the consumer at 54%, and **the whole
   64-vCPU machine is at 3.8%**. The plan's premise — that the receive path is
   limited by being single-threaded — is refuted by its own subject: the single
   thread is 39% idle at the ceiling. (§4.1)
5. **The deficit is not a parallelism problem.** ~~It is a cadence problem~~ —
   **corrected 2026-08-25.** The cadence hypothesis (§6) was falsified and the
   account is now in `ena-receive-latency-account.md`: the limiter is **bufferbloat in
   the device-interface receive FIFO** — a standing ~13.7 ms, ~16 MiB queue that is
   ~99.93% of a frame's transit time — **plus `TCPEndpoint::fLock`, ~47% of the
   ceiling** (the consumer is 100% wall-clock saturated at only ~53% CPU-busy). §6's
   "single FIFO's plain mutex" candidate was essentially right. The
   `XXX STRUCTURAL FIX STILL OWED` at `ena.cpp:219-228` remains a worthwhile
   **correctness** fix, but it is not the throughput lever.

**What was built:** nothing in the kernel, nothing in the stack, nothing in the
driver. Two small independently-useful fixes came out of the measurement work and
are on `feat/ena-multiqueue`: a concurrency mode for the throughput peer, and a
POSIX conformance fix in TCP `connect()` — the latter hardware-verified with two
negative controls (§7).

---

## 1. The two AWS shaping limits, measured rather than assumed

No receive number on this instance family can be read without these, and neither
was in any earlier document in this tree.

`aws ec2 describe-instance-types`:

| type | vCPU | baseline Gbps | peak Gbps | `DefaultEnaQueueCountPerInterface` |
|---|---|---|---|---|
| c7g.large | 2 | **0.937** | 12.5 | **2** |
| c7g.4xlarge | 16 | 7.5 | 15 | 8 |
| c7g.8xlarge | 32 | 15 | 15 | 8 |
| c7g.16xlarge | 64 | **30** | 30 | 8 |
| c7g.metal | 64 | 30 | 30 | 8 |

- **The device's queue grant is `DefaultEnaQueueCountPerInterface`, and it is 8
  for the whole family above `large`.** The `c7g.large` boot log's `tx_sq 2 ...
  rx_sq 2` and the `c7g.4xlarge`'s and `c7g.16xlarge`'s `tx_sq 8 ... rx_sq 8` are
  exactly that field. Eight is the ceiling however many vCPUs are present, so
  `ena-multiqueue-plan.md` §2.5's "up to 31 IO queue pairs" was never going to be
  asked for.
- **Every `c7g.large` figure in this tree was taken inside the network I/O credit
  window.** That instance's *baseline* is 937 Mbit/s. The 4.9 Gbit/s in
  `throughput-measurement.md` and `net-receive-profile.md` is 5.3× the sustained
  allowance. See §8 — this needs a correction to an existing document, and it is
  not this document's to make.

### 1.1 The per-flow limit is 5 Gbps and it is not ours

| sender → receiver | tool | flows | Mbit/s |
|---|---|---|---|
| metal → linux 4xl | iperf3 | 1 | 4964 |
| linux 4xl → metal | iperf3 | 1 | 4964 |
| linux 4xl → linux 4xl | iperf3 | 1 | 4964 |
| linux 4xl → linux 4xl | nettput-peer (python) | 1 | 4963 |
| **linux 16xl → linux 16xl** | iperf3 | 1 | **4964** |
| **linux 16xl → linux 16xl** | nettput-peer (python) | 1 | **4963** |
| metal → **Haiku** 4xl | nettput | 1 | **4960** |
| **linux 16xl → Haiku 16xl** | nettput | 1 | **4963** ×5 runs |

4964 Mbit/s of TCP payload is 5000 Mbit/s of wire at MTU 9001 — the 0.71%
difference is exactly the Ethernet + IP + TCP framing on a 9015-byte frame. Two
tools, two operating systems, five instance pairings, four significant figures.

### 1.2 The per-instance limit is `PeakBandwidthInGbps`, to four figures

| receiver | flows | Mbit/s | `PeakBandwidthInGbps` |
|---|---|---|---|
| linux 4xl | 4, 8, 16 | 14894 | 15 |
| linux 4xl | 4 (python sender) | 14847 | 15 |
| linux 16xl | 4 | 17942 | 30 |
| linux 16xl | 8, 16 | **29824, 29823** | 30 |
| linux 16xl | 8 (python sender) | 24239 | 30 |

29824 Mbit/s of payload is 30 Gbps of wire. **The addressable space on a
`c7g.16xlarge` is therefore 4.96 Gbit/s for one flow and 29.8 Gbit/s in
aggregate.** Those are the goalposts and everything below is measured against
them.

The python sender's 24239 at n=8 is a *wall-clock* aggregate over staggered
stream completions, so it understates the instantaneous peak. It is quoted only
to establish that the sender used against Haiku can source **at least
24 Gbit/s** — 2.4× Haiku's ceiling — so nothing below is a sender limitation.

---

## 2. Single-flow: parity, and no headroom that ever existed

Same peer, same instance class, same subnet, MTU 9001:

| receiver | tool | Mbit/s | % of the 5 Gbps flow cap |
|---|---|---|---|
| Linux `c7g.16xlarge` | iperf3 | 4964 | 99.3% |
| **Haiku `c7g.16xlarge`** | nettput | **4963** | 99.3% |
| Linux `c7g.4xlarge` | iperf3 | 4965 | 99.3% |
| **Haiku `c7g.4xlarge`** | nettput | **4960** | 99.2% |

Cross-checked against the peer's own clock, which is the control that matters
because it does not use Haiku's timebase at all: on one run the peer reported
4974.7 Mbit/s against Haiku's 4959.5, a 0.3% disagreement; on another,
9529.5 against 9522.7, 0.07%.

The Haiku single-flow controls in the `c7g.16xlarge` sweep were **4963 Mbit/s on
every one of five interleaved runs**, with the reader thread at 41–42% of a core
each time. That is the negative control for everything in §3: a quantity that
should not move, and did not.

Cost is the one place the stacks are not equivalent — Haiku spends
**~2750 µs of CPU per MiB** received. That is `net-receive-profile.md`'s subject,
not this one's.

---

## 3. Aggregate: Haiku plateaus at ~10 Gbit/s while Linux climbs to 29.8

`c7g.16xlarge` Haiku receiver, sender(s) `c7g.16xlarge` Linux running
`nettput-peer.py --concurrent`. Aggregate is bytes from the interface's own
counters bracketed around an 8-second `netprof` window inside the transfer, so
the rate and the CPU come from the same bracket. Single-flow rows use `nettput`'s
own figure, since the bracket for a 14-second run is contaminated by its end.

| N flows | senders | aggregate Mbit/s | reader | consumer | net timer | machine (64 vCPU) |
|---|---|---|---|---|---|---|
| 1 (control ×5) | 1 | **4963** | 41–43% | 41–45% | 15–17% | 2.5% |
| 2 | 1 | 10012 | 62.7% | 69.2% | — | 4.1% |
| 4 | 1 | 10154 | 60.8% | 54.2% | — | 3.8% |
| 8 | 1 | 10216 | 61.1% | 53.6% | — | 3.8% |
| 8 | 1 | 9546 | 57.2% | 51.8% | 21.7% | 3.7% |
| **8** | **2** | **8962** | 52.1% | 53.0% | 20.8% | 3.5% |
| 16 | 1 | 10116 | 60.5% | 54.7% | — | 3.9% |
| **16** | **2** | **9667** | 58.1% | 55.8% | 18.6% | 3.8% |
| — Linux, same pair | 1 | 29824 | — | — | — | — |

`reader`/`consumer` are `/dev/net/ena/0 reader` and `... consumer` as a percentage
of **one** core. `netprof` reconciliation residuals on every multi-stream row were
+0.00% to +0.13%, so nothing significant is happening outside the threads listed.

**The plateau is 9.0–10.2 Gbit/s and it does not care about N.** Eight flows do
not beat four; sixteen do not beat eight. Per-flow rate falls as N rises
(4963 → 2490 → 1250 → 630) so that the sum stays put: a single shared bottleneck,
not a per-flow one.

Same shape on `c7g.4xlarge`, where the instance cap is 15 Gbps rather than 30:
Haiku aggregate 10016 (N=2), 11018 (N=4), 10973 (N=8) against Linux's 14894.
The 4xl numbers are the weaker evidence — the instance cap is only 1.4× the
plateau there — which is exactly why the 16xl run was worth an instance-hour.

### 3.1 The control that rules out the network

The obvious alternative is that ~10 Gbit/s is a limit of the *pair* of instances
rather than of Haiku. It is not: **splitting the flows across two distinct sender
instances does not raise the ceiling.** 8 flows from one sender gave 9546–10216;
4+4 from two senders gave 8962. 16 from one gave 10116; 8+8 from two gave 9667.
If the ceiling were a per-pair path allowance, two source instances would have
cleared it.

This control was worth building because an unshaped path had already produced one
false result in this work — see §9.

---

## 4. Nothing is saturated at Haiku's ceiling

This is the observation that redirects everything.

At 10154 Mbit/s aggregate on 64 vCPU:

```
/dev/net/ena/0 reader      60.8% of one core
/dev/net/ena/0 consumer    54.2% of one core
net timer                  ~19%  of one core
whole machine              3.80% of 64 vCPU   (~2.4 cores of 64)
```

- **Not aggregate CPU.** 3.8% of the machine. 61 cores idle.
- **Not the reader thread.** 61% of one core, so it is blocked 39% of the time.
- **Not the consumer thread, and therefore not `receive_lock`.** 54%. If holding
  `receive_lock` across the whole protocol dispatch (`device_interfaces.cpp:128`)
  were the wall, the consumer would be pinned at 100% and it is not. Note this
  agrees with `net-receive-profile.md`'s reasoning that a thread blocked on that
  lock accrues no CPU — but here the consumer is not blocked *or* busy; it is
  simply not being given work fast enough.
- **Not interrupt affinity.** One MSI-X vector on CPU 0 (`io irq 1022`,
  collection 0) is delivering the whole ceiling while the machine idles.

**Every one of the two "blockers" this project was scoped to remove was in full
force, and neither of them is what stops the throughput.** The plan's §6.1
arithmetic — "two threads at 62% occupancy, so the ceiling is around 8 Gbit/s of
pure CPU" — was sound arithmetic about a 2-vCPU machine and was then read as a
property of the code. On 64 vCPU the same code goes past it and stops somewhere
else entirely.

---

## 5. The deciding experiment: Linux with one queue loses nothing

If Haiku's ceiling were a per-queue limit of the device, multi-queue would be
exactly the fix and the 2.9× would be the prize. So: take the *Linux* receiver,
which reaches 29.8 Gbit/s with eight queues, and give it one.

`ethtool -L ens5 combined N` on the `c7g.16xlarge` Linux receiver, sender
unchanged, `iperf3 -P 8`, 12-second runs, **interleaved A/B**:

| queues | Mbit/s | busy RX interrupt lines |
|---|---|---|
| 8 | 29823 | 8 |
| **1** | **29826** | **1** |
| 8 | 29824 | 8 |
| **1** | **29824** | **1** |

**Going from eight receive queues to one costs 0.01%.** The interrupt spread
confirms the change took effect — eight busy IRQ lines become one.

Three conclusions, and the first two are what close this project:

1. **There is no per-queue rate limit near 10 Gbit/s.** A single ENA receive
   queue on this device carries the full 30 Gbps.
2. **Multi-queue receive cannot be the fix for Haiku's ceiling**, because one
   queue on this hardware already clears that ceiling three times over. Whatever
   is limiting Haiku to 10 Gbit/s is above the queue, and giving it eight of them
   would leave the limit exactly where it is. RSS spread, per-queue structs,
   `receive_data_queue`, MSI-X affinity, `GITS_CMD_MOVI` — roughly 1300–2000
   lines rated high risk in `ena-multiqueue-plan.md` steps 3–5 — are aimed at a
   constraint that is measurably not binding.
3. **A single-queue receive path is not, in itself, a throughput limitation on
   this hardware.** That is worth stating plainly, because it is the opposite of
   the assumption the whole plan rested on.

---

## 6. Where the 2.9× actually is: not parallelism ~~— interrupt cadence~~

> **Resolved 2026-08-25 (`ena-receive-latency-account.md`).** The interrupt-cadence
> hypothesis stated below was falsified. The 2.9× is **bufferbloat in the
> device-interface receive FIFO** — a standing ~13.7 ms, ~16 MiB queue that is ~99.93%
> of a frame's transit time — **plus `TCPEndpoint::fLock`, ~47% of the ceiling**. The
> "single FIFO's plain mutex" candidate listed at the end of this section was
> essentially right. The cadence arithmetic below is retained for the record only.

Stated as the leading hypothesis with its arithmetic, **not** as a result.

From the same interleaved 12-second Linux runs, the receive IRQ counter on the
single-queue configuration advanced by 249,738 while 41.7 GBytes arrived:

```
41.7 GB / 9001 B      = 4.63 M frames
4.63 M / 249,738      = ~18.6 frames drained per interrupt
249,738 / 12 s        = ~20,800 interrupts/s
```

Linux takes about **21 thousand interrupts a second and drains ~19 frames on
each** to carry 29.8 Gbit/s on one queue.

Haiku's side of the same arithmetic:

```
ENA_RX_IRQ_INTERVAL = 20 us (ena.h:150)   =>  interrupts bounded at 50,000/s
measured at the ceiling: 1,129,733 frames in 8 s  =  141,217 frames/s
141,217 / 50,000      = 2.82 frames per interrupt, if every interrupt is used
reader CPU per frame  = 4.86 s / 1,129,733  = 4.30 us
reader wall per frame = 8 s / 1,129,733     = 7.08 us   (39% blocked)
```

So Haiku is delivering 141 k frames/s through a path whose interrupt rate is
capped at 50 k/s, with the draining thread idle 39% of the time. **The reader is
waiting, not working.** The obvious mechanism is the one already written down in
the driver:

> `XXX STRUCTURAL FIX STILL OWED: this unmask belongs *after* the ring has been
> drained, not here.` — `ena.cpp:219-228`

Today `ena_io_interrupt()` re-arms the vector *before* anything is consumed, so
interrupt moderation is the only thing bounding the interrupt rate, and the
reader is repeatedly woken for a small batch instead of once for a large one.
That is `ena-multiqueue-plan.md` step 2: **~40–60 lines, driver-only, no kernel
change, no stack change** — and because `ena` is a kernel *module* it can be
hot-swapped into `/boot/home/config/non-packaged/add-ons/kernel/` on a running
node, so it costs a 4-minute loop rather than a 25-minute image bake.

> **The hot-swap claim above stands (checked 2026-08-24), but check it with a stamp
> rather than trusting this paragraph.** An earlier revision of this note asserted the
> opposite — that `ena` could not be dropped in because a support-score tie goes to the
> packaged copy — and that was wrong. `_FindBestDriver()` keeps a candidate only on
> `support > bestSupport`, strictly greater (`device_manager.cpp:1803`), so a tie is
> decided by enumeration order; and the iterator pushes `kModulePaths` **forward**
> (`module.cpp:2015`) onto a stack and pops **LIFO** (`:831`), so
> `B_USER_NONPACKAGED_ADDONS_DIRECTORY` is searched **first**. First-scanned wins, and
> non-packaged is first.
>
> **The real carve-out is the boot path, and it is a filesystem problem rather than a
> scoring one:** the boot *storage* driver cannot be overridden because its override
> directory lives under `/boot/home`, on the volume that driver must mount. `ena` is
> not in the boot path.
>
> **Both of the above are reasoning-from-code, and this question has now been answered
> wrongly twice from the same source.** So: **compile a distinctive version string into
> anything you hot-swap and look for it.** Without that, "the number did not move" and
> "the code did not load" are indistinguishable — which is exactly how the `nvme_disk`
> case was caught.

**Honest limits of this hypothesis.** 2.82 frames per interrupt is a *lower*
bound on Haiku's batch size: if the real interrupt rate is below 50 k/s the batch
is larger. The counter that would settle it is `ioInterrupts` (`ena.h:274`),
which is incremented on every interrupt and currently only reaches `dprintf` for
the *first* one — so the first piece of work for whoever picks this up is to make
that counter readable, exactly as the house rule says: if you cannot observe it,
build the observability first.

Two other candidates for the same 10 Gbit/s wall, neither excluded:

- **One `net_buffer` per `receive_data()` call** (`net_device.h:54`) — one ioctl
  per frame, 141 k/s at the ceiling. ~~A batched receive entry point is the
  symmetric twin of the batched *transmit* entry point that the doorbell and TSO
  work already needs, which makes it a more attractive project than it looks in
  isolation.~~

  > **Withdrawn 2026-08-24: there is no twin.** Doorbell coalescing is **DEAD**
  > (`219d8ab858`) and TSO is **impossible on this device** (`tso v4 0 v6 0`). "It
  > comes free with two other projects" was the whole of the case for batched
  > receive, and both of those projects are gone. It may still be worth doing — but
  > argue it on its own merits.
- **The single FIFO's plain mutex** (`stack/utility.cpp:211,232`), taken once per
  frame by the reader and once by the consumer.

None of these is a queue.

---

## 7. What shipped

Two commits on `feat/ena-multiqueue`, both fallout from the measurement rather
than from the plan.

### 7.1 `nettput-peer.py --concurrent`

The peer served connections strictly one after another, which cannot measure
aggregate throughput at all. With four clients it produces per-stream rates in
the ratio 1 : 1/2 : 1/3 : 1/4 — each stream's clock starts when it connects but
it only transfers in its own slot — and the first sweep in this work was ruined
by exactly that, in a way that looked like congestion rather than a broken
instrument. `--concurrent` forks a child per connection. Forking rather than
threading is deliberate: each stream gets its own interpreter on its own core, so
the peer cannot quietly become the bottleneck the measurement is hunting for.
`SIGCHLD` is `SIG_IGN` rather than reaped, because a parent blocked in `wait()`
while N clients connect at once staggers their starts and the aggregate then
measures the stagger. Backlog raised from 4 to 64 for the same reason.

One port also means one firewall rule. Opening a port *range* between two
security groups is a change to shared infrastructure that every later experiment
has to trust, and forking removes the need for it.

### 7.2 TCP `connect()` returned EWOULDBLOCK for a connection timeout

`TCPEndpoint.cpp`'s `posix_error()` maps `B_TIMED_OUT` to `B_WOULD_BLOCK`. That
is correct for `ReadData()` and `SendData()`, where a timed-out wait means
`SO_RCVTIMEO`/`SO_SNDTIMEO` expired or a non-blocking call could not proceed. It
is wrong for `Connect()`: POSIX gives `connect()` **ETIMEDOUT** for "the attempt
to connect timed out before a connection was made", and the non-blocking case
returns `EINPROGRESS` before the wait is ever entered
(`TCPEndpoint.cpp:696-700`).

The consequence is a misdirection, not a cosmetic difference: a dropped SYN — a
full listen backlog, a firewall rule, a peer that never answered — is reported as
*"Operation would block"*, which reads as a non-blocking-socket mistake in the
caller's own code. It cost time twice during this work.

Fixed with a `connect_error()` alongside `posix_error()`, used only by the two
`_WaitForEstablished()` call sites in `Connect()`. **Hardware-verified** — see
§7.3 for the measurement, which also gives the three cases side by side.

### 7.3 Verifying it, and the three connect outcomes

`nettput` was pointed at three destinations from a Haiku `c7g.16xlarge`:

| destination | what the network does | expected |
|---|---|---|
| `10.42.0.149:5555` | security group drops the SYN, no RST | ETIMEDOUT after 75 s |
| `10.42.0.8:5555` | host answers with RST | ECONNREFUSED, immediately |
| `10.42.0.8:5399` | peer listening | success |

`TCP_CONNECTION_TIMEOUT` is 75 s (`tcp.h:183`) and
`min_c(socket->send.timeout, TCP_CONNECTION_TIMEOUT)` leaves it at 75 s for a
socket with no `SO_SNDTIMEO`, so the blackhole case takes 75 seconds to report.
Result recorded in §7.4.

### 7.4 Measured before and after, on one `c7g.16xlarge`

Before — the shipped canonical AMI:

```
Mon Aug 24 03:14:32 UTC 2026
nettput: cannot connect to 10.42.0.149:5555: Operation would block
real    1m15.003s
Mon Aug 24 03:15:47 UTC 2026
```

After — the `tcp` module cross-built with this change, dropped into
`/boot/home/config/non-packaged/add-ons/kernel/network/protocols/tcp` and the node
rebooted, on the same instance:

```
Mon Aug 24 03:22:43 UTC 2026
nettput: cannot connect to 10.42.0.149:5555: Operation timed out
real    1m15.003s
Mon Aug 24 03:23:58 UTC 2026
```

| case | before | after |
|---|---|---|
| SYN dropped by a security group | `Operation would block` @ 75.003 s | **`Operation timed out`** @ 75.003 s |
| host answers with RST | `Connection refused`, immediate | `Connection refused`, immediate |
| peer listening | 4782.7 Mbit/s | 4944.1 Mbit/s, 2767 µs/MiB |

**75.003 seconds is `TCP_CONNECTION_TIMEOUT` to three decimal places**
(`tcp.h:183` = 75000000 µs) in both runs, so the *same* code path is being taken
and only the error mapping changed. The two negative controls did not move, which
is what the change's scope predicts: neither the RST path nor a successful connect
reaches `connect_error()`.

**Verified by artifact, not by exit code.** `listimage` on the rebooted node
reports the tcp module loaded from
`/boot/home/config/non-packaged/add-ons/kernel/network/protocols/tcp` at
`0xffff0000dcfdc000` — the non-packaged copy, not the one in the image — so the
"after" row is definitely running the patched module. `sync` before the reboot was
necessary: file *data* is not flushed by the page writer on this port, so a module
written and immediately rebooted comes back zero length.

---

## 8. A methodology warning that lands on another document

`c7g.large`'s network baseline is **937 Mbit/s** (§1). Every figure in
`graviton/docs/throughput-measurement.md` above that — including the headline
receive 4936 Mbit/s and the "+403% jumbo" claim — sits inside the AWS network I/O
credit window, which is **history-dependent**. Back-to-back runs on that instance
are therefore not independent trials, which is the same class of confound as the
"65535/65536 cliff" that this project already had to retract.

It matters specifically for the jumbo claim, because the MTU-1500 baselines there
(952 and 981 Mbit/s) are within 2–5% of the 937 Mbit/s baseline allowance. Some
part of "MTU 1500 is slow" may be "MTU 1500 ran out of credit". The win is
certainly real — jumbo frames reduce per-frame work and that is measured
independently in `net-receive-profile.md` — but the *magnitude* is not safe as
stated.

Settling it needs an interleaved Linux `c7g.large` control at both MTUs plus a
soak long enough to exhaust credit, on an instance type where the baseline is
above the rates being compared. **Not done here**, and flagged rather than
silently left.

---

## 9. Hypotheses that died, and the one measurement that was thrown away

Kept because `graviton/docs/` keeps disproven hypotheses next to surviving ones.

| hypothesis | verdict | what killed it |
|---|---|---|
| "the receive path's single-threaded design caps it near 8 Gbit/s" | **false** | reader thread at 61% of one core at the ceiling; machine at 3.8% of 64 vCPU (§4) |
| "multi-queue receive is the next lever above 15 Gbps" | **false** | Linux with **one** queue does 29826 Mbit/s (§5) |
| "the 4.9 Gbit/s plateau is a per-flow AWS cap" | **true, and it is 5 Gbps** — but it explains only single-flow, never the aggregate plateau (§1.1, §3) |
| "the aggregate plateau is a per-pair path allowance" | **false** | two distinct sender instances do not raise it (§3.1) |
| "the aggregate plateau is a per-queue device limit" | **false** | §5 |
| "`receive_lock` across protocol dispatch is the aggregate wall" | **not at this rate** | consumer thread at 54%, not 100% (§4) |
| "interrupt affinity is a prerequisite for any of this" | **not for throughput** | one vector on CPU 0 delivers the whole ceiling with 63 cores idle (§4) |
| "8 queue pairs because 16 vCPU" | **no** — it is `DefaultEnaQueueCountPerInterface`, 8 for the whole family (§1) |

### 9.1 The discarded measurement, and why it is recorded

The first aggregate sweep ran against a Linux peer at `10.42.0.170` and produced
a single-flow rate of **9525 Mbit/s** — 1.92× the per-flow cap, confirmed on the
sender's own clock at 9529.5. Two controls were needed to work out what it meant:

- *Not the tool.* The same Python sender, on the same host, reached exactly
  4962.7 Mbit/s to a *Linux* receiver. iperf3 on that pair gave 4964.
- *Not Haiku's TCP.* Haiku hit the 4960 cap cleanly on the metal path.

What is left is the **path**: `10.42.0.170 → 10.42.0.10` was not subject to the
5 Gbps shaper while `10.42.0.170 → 10.42.0.198` and both directions to the metal
builder were. Co-residency on one Nitro host is the likely reason and remains a
guess; the consequence is not. **Those numbers cannot be compared with anything
and all of them were discarded.**

The lesson is worth more than the numbers: **on EC2, the path is part of the
experimental apparatus.** Two instances of the same type in the same subnet can
sit behind different shaping, and a rate that beats a documented limit is a signal
that the apparatus changed, not that the software got faster. Every A/B in this
document therefore holds the *pair* fixed, and the Linux reference for each Haiku
number was taken between instances of the same type on a pair verified to show
the 5 Gbps per-flow cap.

One thing that pair did establish, and it is the only reason it appears here at
all: with a single flow at 9525 Mbit/s the reader thread was at 46% of one core.
That was the first sign that §4's conclusion was coming.

---

## 10. What a successor should do, in order

1. **Make `ioInterrupts` (`ena.h:274`) readable from userland**, and count frames
   per interrupt at the ceiling. This is the observability that §6's hypothesis
   needs and it is smaller than anything else on this list. If it cannot be
   observed, nothing below is worth starting.
2. **Move the interrupt unmask after the ring drain** — **`ena.cpp:221`** (the line
   reference `219-228` has drifted), the `XXX STRUCTURAL FIX STILL OWED` that is
   already there, ~40–60 lines, driver-only, hot-swappable without a bake — **but
   verify the swap took effect with a compiled-in version stamp; see the note in §6.**
   Then re-measure the §3 sweep. This is the whole of §6's hypothesis and it is still the cheapest thing
   on the list. **Being worked as of 2026-08-24 — check before starting.**
3. **A batched receive entry point** (`net_device.h:54`), if step 2 does not
   close the gap. ~~It is the twin of the batched *transmit* entry point that
   doorbell coalescing and TSO already require, so the two projects share a
   design and should share a decision.~~

   > **The stated justification has evaporated (2026-08-24), so this step must be
   > argued on its own merits or dropped.** There is no batched *transmit* entry
   > point coming: **doorbell coalescing is DEAD** (`219d8ab858` — LLQ grants 2 burst
   > entries, one jumbo frame consumes both, 99.94% of frames already leave the
   > allowance at zero, ratio 1:1, saving exactly zero at MTU 9001) and **TSO is
   > impossible on this device** (`tso v4 0 v6 0`; tx offload `0x3` is IPv4 L3 +
   > IPv4 L4 partial only). There is no twin and no shared design. Batched *receive*
   > may still be worth doing — but "it comes free with two other projects" was the
   > whole of the case for it, and both of those projects are gone.
4. **Do not build multi-queue receive.** §5 is not a "probably not"; it is an
   interleaved A/B on the same hardware showing one queue and eight queues within
   0.01% of each other.

And the transmit deficit — ~~Haiku ~5.1 Gbit/s against Linux 9.5 on an unshaped
path~~ — is not this document's, but note it is the *same* shape of problem as §6:
per-frame and protocol (one doorbell per `net_buffer`, ~~no TSO, no TX checksum
offload~~), at 8% of the machine, so N transmit queues fed from N CPUs cannot help
it either.

> **Three corrections to the paragraph above, 2026-08-24.**
>
> 1. **The 5.1 / 9.5 Gbit/s pair is retracted data.** Those are the unshaped-path
>    numbers that **§9.1 of this same document discards**: *"Those numbers cannot be
>    compared with anything and all of them were discarded."* The closing paragraph
>    reasoned from them anyway, with no marker and **no instance class or date**.
>    Do not quote them.
> 2. **"No TX checksum offload" is stale.** It shipped — `c2753e0030`, measured
>    **−3.54%, p = 0.0079** (an earlier −12.6% is withdrawn as a confound).
> 3. **"No TSO" understates it.** TSO is not missing, it is **impossible**: the
>    device advertises `tso v4 0 v6 0`, and tx offload `0x3` is IPv4 L3 + IPv4 L4
>    *partial* only. Listing it as a gap invites someone to try.
>
> **The conclusion survives all three** — N transmit queues fed from N CPUs still
> cannot help a per-frame/protocol problem. Only the supporting figures were bad.
