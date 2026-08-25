# A latency account for the arm64 network receive path

**Date:** 2026-08-25. **Hardware:** `c7g.16xlarge` (Graviton3, 64 vCPU),
`us-west-2`, Haiku `hrev59996` from AMI `ami-049b28b6e06791056`, MTU 9001, single
ENA queue, 8 concurrent receive flows.

Part I was written and committed to *before* anything was measured, so its
predictions can be read as predictions — including the two of mine that turned out
to be wrong. Part II is the results.

## Verdict

1. **The consumer thread is the bottleneck, it is 100% saturated, and it is 53%
   busy.** Its *wall-clock* service time is 10.167 us/frame, which predicts the
   measured throughput to **0.4%** (7.09 Gbit/s predicted, 7.06 measured). Every
   instrument in this tree until now measured **CPU**, and by that measure the
   thread is half idle and "nothing is saturated". Wall-clock occupancy and CPU
   utilisation are different quantities and the difference is the whole answer.
2. **47% of that ceiling is one lock.** The consumer blocks **4.9 us per frame**
   acquiring `TCPEndpoint::fLock`, which the receive path shares with the
   application's own `read()` and with TCP's timers. That is the entire
   non-executing half of the bottleneck thread's time.
3. **99.93% of a frame's transit time is spent in the reader → consumer FIFO** —
   a standing **13.7 ms**, 11.5 MiB of a 16 MiB cap, never empty. Every hand-off,
   every instrumented lock and the whole protocol stack sum to 10.3 us, 0.075%.
4. **That queue is how the ceiling is enforced on the sender.** It inflates the
   round trip 30-60x (0.35 ms idle → 15 ms loaded, measured externally by ICMP),
   which collapses TCP's congestion window until the senders deliver exactly the
   consumer's drain rate. Hence an equilibrium in which nothing looks saturated.
5. **Shrinking that queue is better on both axes at once: 16 MiB → 256 KiB gives
   +25.5% goodput and 29x less latency.** The buffer was sized in bytes; it needs
   to be sized in time. This also retires the "a bigger FIFO changes nothing"
   result — the cap had only ever been tested in the direction that cannot help.
6. **The hand-off-latency hypothesis is confirmed in type and refuted in
   mechanism.** There is a serialization worth half the ceiling, but it is not any
   hand-off between the driver, the FIFO and the consumer: those cost 0.04-0.22 us
   each. It is a lock two layers up, shared with the application.

Three hypotheses for the receive shortfall are already dead and are not re-run
here: packet loss / a 14.5 Gbit/s consumer ceiling (`ena-rx-cadence-falsified.md`
§"What this closes"), interrupt cadence and moderation
(`ena-rx-cadence-falsified.md`), and multi-queue headroom
(`ena-multiqueue-headroom.md`). What survives is *hand-off latency*, and the
explicit request from that work was **a latency account, not another rate
sweep**.

---

## 0. The capacity bound, computed before building anything

The project rule is to check a mechanism against a capacity bound *before*
pursuing it. Doing that here turns a vague hypothesis into an arithmetic one, and
it can be done entirely from numbers already measured.

From `ena-rx-cadence-falsified.md` §"Nothing is saturated", the shipped arm on
`c7g.16xlarge`, 8 receive flows, MTU 9001:

| quantity | measured value |
|---|---|
| receive frame rate | 97,273 frames/s |
| goodput | 6.98 Gbit/s |
| device-interface **consumer** thread | 50.5% of one core |
| driver **reader** thread | 40.3% of one core |
| **net timer** thread | 18.4% of one core |
| whole machine (64 vCPU) | 2.65% busy — **97.3% idle** |

Divide the per-thread CPU by the frame rate to get each stage's **service time**:

```
consumer  0.505 core / 97273 frames/s = 5.19 us/frame
reader    0.403 core / 97273 frames/s = 4.14 us/frame
net timer 0.184 core / 97273 frames/s = 1.89 us/frame
frame arrival period at 97273 frames/s = 10.28 us
```

Now the two bounds a chain of single-threaded stages can be against:

- **If the stages overlap** (true pipelining), the chain's ceiling is the *max*
  service time: `1 / 5.19 us` = 192,700 frames/s = **13.9 Gbit/s** at MTU 9001.
- **If the stages do not overlap** (mutual exclusion, or a strict
  signal/block ping-pong), the ceiling is the *sum*: `1 / 9.33 us` =
  107,200 frames/s = **7.7 Gbit/s**. Include `net timer` in the exclusion and it
  is `1 / 11.22 us` = 89,100 frames/s = **6.4 Gbit/s**.

**The observed 6.98 Gbit/s sits between the sum-of-two and sum-of-three
predictions and is 50% of the overlap prediction.** That is the whole reason to
believe a hand-off hypothesis rather than a cost hypothesis, and it is why the
earlier dismissal of `receive_lock` — *"a blocked thread accrues no CPU time; it
is a scalability limit, not a per-frame cost"* (`net-receive-profile.md` §6) — is
the hole in the previous work. That sentence is correct about **CPU** and it is
exactly the wrong thing to conclude about **rate**. A serialization limit is
invisible to every instrument used so far, all of which measure CPU.

### 0.1 What this bound licenses and what it forbids

It licenses pursuing serialization. It **forbids** two things:

- Blaming per-frame or per-byte *cost* for the 6.98 → 13.9 Gbit/s gap. Cost is
  already accounted for; the machine is 97.3% idle.
- Expecting serialization to explain the 13.9 → 29.8 Gbit/s gap to Linux. Even a
  perfectly pipelined chain is capped at 13.9 Gbit/s **at the measured per-frame
  service time**. So the shortfall has *two* causes and this document can only
  address the first. Any claim that fixing hand-off latency reaches Linux parity
  is refuted in advance by this arithmetic.

### 0.2 Little's Law is the form the account has to take

For a chain, `throughput = frames_in_flight / transit_latency`. A per-stage
latency account is only interpretable next to the concurrency it runs at:

- transit 30 us at 97,273 frames/s implies **2.9 frames in flight**;
- transit 9.3 us at the same rate implies **0.9 frames in flight** — i.e. the
  chain holds barely one frame and is latency-bound.

So the instrument must report **both** per-stage latency **and** the occupancy of
every queue between stages. Latency alone cannot distinguish "the stage is slow"
from "the stage is starved", and occupancy alone cannot price it.

---

## 1. The instrument, designed on paper

### 1.1 Stage boundaries

| # | stage | from | to |
|---|---|---|---|
| S1 | interrupt to reader | ISR entry timestamp | reader thread resumes from its wait |
| S2 | driver drain | reader begins this completion | `net_buffer` fully built |
| S3 | FIFO residency | enqueue to the device-interface FIFO | consumer dequeues it |
| S4 | protocol dispatch | consumer dequeue | TCP appends payload to the socket receive queue |
| S5 | socket residency | TCP append | the application's `read()` consumes those bytes |

Plus three *waiting* measurements, which is where a hand-off hypothesis actually
lives:

| # | quantity | why |
|---|---|---|
| W1 | duration of the reader's blocking wait | starved-by-device vs starved-by-hand-off |
| W2 | duration of the consumer's blocking wait | starved-by-reader |
| W3 | duration of every acquisition of the shared receive lock, and how often it blocked | the exclusion in §0 |

Plus two *occupancy* measurements, which cost no clock reads at all:

| # | quantity |
|---|---|
| Q1 | FIFO depth sampled at each enqueue and each dequeue |
| Q2 | number of stages simultaneously inside their own critical region (the direct test of §0's exclusion) |

### 1.2 What would falsify each number

Pre-registered, so that a result cannot be reinterpreted after the fact.

- **S1 is falsified as a bottleneck** if its distribution is dominated by values
  below ~1 us. A wake-up that costs 0.5 us cannot matter on a 10.3 us period.
- **S3 (FIFO residency) and Q1 together are the decisive pair.** If Q1 shows a
  FIFO that is *empty or near-empty at dequeue* in the great majority of
  samples, the consumer is **starved** and the consumer is not the constraint —
  which falsifies every "the consumer is too slow" reading of the data. If Q1
  shows a *deep* FIFO while the consumer is at 50% CPU, then the consumer is
  neither starved nor busy, which would mean it is **blocked**, and W3 must
  account for the difference. If Q1 is deep *and* W3 is negligible *and* the
  consumer is at 50%, **the instrument is wrong** — those three cannot be true
  together, and that combination is the instrument's self-check.
- **Q2 falsifies §0's exclusion hypothesis directly.** If the reader and the
  consumer are frequently inside their critical regions simultaneously, the
  stages do overlap, the sum-of-service-times bound does not apply, and §0's
  arithmetic fit is a coincidence. This is the single measurement most likely to
  kill the hypothesis, so it is the one to trust.
- **The whole account is falsified if `S1+S2+S3+S4+S5` is far below the frame
  period times the in-flight count implied by Little's Law.** A gap there is
  time spent somewhere the probes do not look, and the honest report of that is
  the gap, not a rounded-up stage.
- **W3 is falsified as a mechanism** if the lock is essentially never contended
  (blocked acquisitions ≈ 0), in which case §0's fit must be explained by a
  ping-pong rather than by exclusion, and the ping-pong shows up in W1/W2
  instead.

### 1.3 The clock, and why it has to be calibrated first

Every number above is a difference of two clock reads, so the clock's cost and
resolution bound what the instrument can say. On arm64 the kernel's
`system_time()` reduces to a read of the architected counter (`CNTVCT_EL0`) plus
a scale conversion. Two properties must be **measured, not assumed**:

- **Resolution.** `CNTFRQ_EL0` sets the tick, and a counter running at a few tens
  of MHz quantises to a few tens of nanoseconds. Stages of 1-5 us tolerate that;
  a 100 ns stage does not, and the histogram would show the quantisation as
  structure. Report the tick.
- **Cost.** `MRS` from the counter is a serialising read on many
  implementations. If a read costs 30 ns, six stamps cost 180 ns, which is 1.7%
  of a 10.28 us frame period — acceptable. If a read costs 300 ns, six stamps
  cost 11% and **the instrument is the experiment**. The task's own framing
  applies: a probe adding 1 us to a per-frame budget of a few microseconds is
  not a measurement, it is a different system.

The calibration is therefore: (a) time N back-to-back counter reads to get the
per-read cost, (b) histogram the deltas between consecutive reads to get the
effective resolution and to see whether the read is ever preempted, and (c)
report both **before** any latency number.

### 1.4 The overhead A/B, which is the calibration that actually matters

Per-read cost is necessary but not sufficient — it does not capture cache
pollution, an extra barrier, or a changed inlining decision. So the instrument
ships with a runtime switch and the load is measured three ways:

1. stock driver/modules,
2. instrumented build, probes **off**,
3. instrumented build, probes **on**.

**If (3) differs from (2) by more than the boot's noise floor, every latency
number is discarded.** Arms interleaved, never in ascending order, following the
apparatus rules in `ena-rx-cadence-falsified.md`. (1) versus (2) separates "the
probe costs something when it runs" from "adding the probe changed the build".

### 1.5 Aggregation, not tracing

The probes accumulate into per-stage `{count, sum, sum of squares, min, max}` and
a **base-2 histogram** of 32 buckets, read out by ioctl. Nothing is written to a
log on the datapath: on this platform `dprintf` writes the UART one character at
a time, synchronously — a barrier, not a probe
(`ena-rx-cadence-falsified.md` §Apparatus). Histograms are reported rather than
means because the hypothesis under test is about *tails*: a hand-off that is
usually 200 ns and occasionally 400 us is invisible in a mean and is the entire
story.

### 1.6 Carrying a timestamp with a frame

S3 and S5 need a per-`net_buffer` timestamp that survives the hand-off. Fields
are **appended to the end** of `net_buffer` so that every module compiled against
the old header still reads every existing field at its old offset; only the
allocating module needs to know the new size. Both the driver and the stack are
rebuilt from one tree and both **stamp a build version** that is read back from
syslog before any number is believed — the `rx-cadence-4-mod` lesson from the
falsification run, where a wrong load path silently measured the packaged driver.

---

---

# Part II — Results

**Hardware:** `c7g.16xlarge` (Graviton3, 64 vCPU), `us-west-2a`, Haiku
`hrev59996` from AMI `ami-049b28b6e06791056`, MTU 9001, single ENA queue.
**Load:** 8 concurrent receive flows, one process per flow against a forked
listener per connection, from a `c7g.16xlarge` Linux peer on the same subnet.
Both instances were launched for this work in a security group that permits all
traffic within itself, so no firewall rule is in the measurement path.

The offered load reproduces the known shortfall: **7.06 Gbit/s at 98,052
frames/s**, against 29.8 Gbit/s for Linux on one ENA queue on the same instance
type. The frame rate agrees with the earlier session's 97,273 frames/s to 0.8%.

---

## 2. Calibration — read this before any latency number below

### 2.1 The clock

Measured on the test node itself, twice, identical to three significant figures:

```
CNTFRQ_EL0            : 1050000000 Hz  (0.9524 ns per tick)
one mrs               :   6.68 ns
one isb + mrs         :  16.90 ns
probe pair (2 mrs + accumulate): 13.39 ns
back-to-back delta    : min 7, mean 7.00, max 8 ticks  (6.67 / 6.67 / 7.62 ns)
delta distribution    : [4..8):100.00%  [8..16):0.00%
```

Four things follow, and they are the licence for everything after this section:

- **Resolution is ~6.7 ns with 1 tick (0.95 ns) of jitter.** Every stage reported
  below is 30 ns or larger, so nothing is quantisation-limited. In 100,000
  back-to-back samples not one landed above 8 ticks, so the measuring thread was
  never interrupted mid-measurement and there is no tail in the noise floor to
  mistake for a tail in the signal.
- **`system_time()` would have been useless here.** It returns *microseconds*.
  Three of the stages below are under 0.3 us and one is 0.038 us; measured with
  `system_time()` they would all have read 0. This is the single most important
  design decision in the instrument and it was made from reading the source, then
  confirmed against the counter frequency on the metal.
- **The ISB was correctly omitted.** It costs 10.2 ns, i.e. it would have more
  than doubled the probe, to remove a reordering error bounded by the core's
  reorder window — nanoseconds against signals of microseconds and milliseconds.
- **Total probe cost per frame: ~60-90 ns** (9 counter reads plus 11 accumulates
  in the stack module, 3 more reads in the tcp module). Against a 10.2 us
  per-frame budget that is **0.6-0.9%**.

### 2.2 The overhead that matters: does running the probe change the system?

Per-read cost does not capture cache pollution, an extra indirect call, or a
changed inlining decision, so the load was measured with the same binary and the
probe switched off and on, strictly alternating, inside one boot, with all eight
flows alive throughout every arm.

**Build A, stack probes only** (all probes inlined, 8 arms interleaved):

```
off: 7.041  7.099  7.827  7.234     mean 7.300
on : 7.346  7.254  7.129  7.403     mean 7.283
```

Difference **−0.017 Gbit/s (−0.2%)** against a within-arm spread of ±0.35 Gbit/s
(±4.8%). Not distinguishable from zero.

**Build B, stack + tcp probes** (12 arms, strictly alternating off/on so the
comparison is paired):

| pair | off | on | difference |
|---|---|---|---|
| 1 | 7.299 | 7.067 | 0.232 |
| 2 | 7.539 | 7.117 | 0.422 |
| 3 | 7.887 | 6.880 | 1.007 |
| 4 | 7.252 | 7.198 | 0.054 |
| 5 | 6.996 | 6.640 | 0.356 |
| 6 | 7.168 | 7.034 | 0.134 |

**Mean difference 0.368 ± 0.139 Gbit/s (SE), paired *t* = 2.64 on 5 df,
*p* ≈ 0.046, 6 of 6 pairs positive — the probe costs 5.0% ± 1.9% of goodput.**

That is a real effect and it is reported rather than absorbed, because the
pre-registered rule in §1.4 said the numbers would be discarded if the probe
moved the outcome. Three things about it:

- **It is the tcp probes, not the instrument as a concept.** Build A, whose probes
  are all `static inline` in the same module as the counters, costs nothing
  measurable. Build B adds three calls through
  `net_stack_module_info::rxlat_add`, which is an *indirect cross-module call*
  that cannot be inlined, per TCP segment. That is the price of reaching
  accumulators in another add-on, and it is the thing to fix if this probe is ever
  wanted permanently.
- **The numbers are not discarded, because 5% of the budget cannot manufacture the
  findings.** Attributing the *entire* 5% to added consumer service time is
  0.5 us/frame. The `fLock` wait is 4.9 us — ten times that. The FIFO residency is
  13,685 us — twenty-seven thousand times that. And the two comparisons that carry
  the argument (16 MiB versus 256 KiB, probe-off versus probe-on) each have the
  probe present in *both* arms, so it cancels.
- **The one number it does bias, it biases against the conclusion.** Probe cost
  lands in CPU time, not in blocked time, so §6.1's blocked-time figure is if
  anything understated.

An uninstrumented consumer service time can be recovered from the same data:
the probe-off arms average 7.357 Gbit/s = 102,150 frames/s, i.e. **9.79 us/frame**
against 10.167 us/frame measured with the probe on.

### 2.3 The instrument announces itself

`rxlat info` reads back a build stamp bumped by hand on every rebuild, and
`listimage` was used to confirm both modules loaded from
`/boot/home/config/non-packaged/add-ons/kernel/network/` rather than from the
packaged copies, **before any number below was believed**. This is not
ceremony: an earlier session in this tree measured a packaged driver for a whole
run because a non-packaged copy was in the wrong directory, and reported the
result as "the change did nothing".

One hazard found the hard way and worth recording: **copying a kernel add-on
directly into a live add-on directory kills the connection doing the copying**
and leaves a zero-length file. Staging the module elsewhere and `mv`-ing it into
place writes only a directory entry and is safe. A `sync` afterwards is
load-bearing — BFS journals the inode but not the file's data, so without it the
module comes back the right size and full of zeros after the reboot.

---

## 3. The per-stage account

`rxlat read`, 16 MiB receive FIFO (the shipped value), 6.5 s window, 699,435
frames. Means; the distributions are in §3.2.

| stage | mean | share of transit |
|---|---|---|
| reader: `deframe` | 0.038 us | 0.0003% |
| reader: FIFO enqueue (incl. the mutex) | 0.220 us | 0.0016% |
| **FIFO residency (enqueue → dequeue)** | **13,685 us** | **99.93%** |
| consumer: FIFO dequeue call | 0.214 us | 0.0016% |
| consumer: `receive_lock` acquire | 0.071 us | 0.0005% |
| consumer: protocol dispatch (ethernet → IPv4 → TCP → socket → wake) | 9.784 us | 0.071% |
| **total software transit** | **13,695 us** | 100% |

**99.93% of a received frame's transit time is spent sitting in the reader →
consumer FIFO.** Every hand-off, every lock and the entire protocol stack
together sum to 10.3 us — 0.075% of the total.

Inside protocol dispatch, from the tcp module:

| sub-stage | mean |
|---|---|
| `TCPEndpoint::fLock` acquire (in `SegmentReceived`) | **4.912 us** |
| `SegmentReceived` total, lock included | 7.572 us |
| `_NotifyReader` — condition-variable wake of the reading thread + select notify | 0.718 us |

### 3.1 Occupancy, and the Little's Law cross-check

| quantity | value |
|---|---|
| FIFO depth at dequeue, mean | 11,766 KiB of a 16,384 KiB cap (72%) |
| FIFO depth at dequeue, 81% of samples | 8-16 MiB |
| FIFO peak | 16,769,079 bytes — the cap |
| ENOBUFS drops | 18 in the window; 0.015% of frames |

Two independent measurements have to agree, and they do:

```
frames in flight from latency  = 13.685 ms x 98,052 frames/s = 1,342 frames
frames in flight from occupancy = 11,766 KiB / 9001 B        = 1,338 frames
```

**Agreement to 0.3%.** The per-frame residency histogram and the byte-occupancy
histogram are computed from different quantities by different code paths, and
Little's Law reconciles them. That is the strongest internal evidence that the
instrument is measuring real wall-clock queueing.

### 3.2 Distributions — the tails, which is why histograms were collected

| stage | p50 | p90 | p99 | max |
|---|---|---|---|---|
| FIFO residency | [7.99, 16.0) ms | [16.0, 32.0) ms | [16.0, 32.0) ms | 22.1 ms |
| protocol dispatch | [3.90, 7.80) us | [15.6, 31.2) us | [15.6, 31.2) us | 2.63 ms |
| reader `receive_data` | [1.95, 3.90) us | [15.6, 31.2) us | [62.4, 125) us | 2.62 ms |
| FIFO enqueue | [0.061, 0.122) us | [0.122, 0.244) us | [0.244, 0.488) us | 16.1 us |
| dequeue call | [0.122, 0.244) us | [0.122, 0.244) us | [0.244, 0.488) us | 13.5 us |
| `receive_lock` | [0.061, 0.122) us | [0.061, 0.122) us | [0.122, 0.244) us | 5.3 us |

The FIFO residency has **no low mode at all**: its minimum over 699,435 frames is
372 us and 88% of frames wait more than 8 ms. This is a *standing* queue, not a
burst absorber — there is no operating point at which it is empty.

`receive_lock` and the FIFO mutex do have tails (5-16 us) but they are so rare
that they do not move the mean off 0.07-0.22 us. **The hand-off costs are not
where the time is**, and the pre-registered W3 test therefore fails to find its
mechanism — see §5.1.

---

## 4. The independent check: an ICMP probe, outside the kernel entirely

A kernel histogram can be wrong in ways that are invisible from inside. ICMP echo
traverses the reader, the FIFO and the consumer exactly as TCP does, but touches
no TCP state and no socket queue, and it is timed by a different machine. So
`ping` against the node under load measures the same queue with a completely
independent instrument.

Idle controls were taken before and after every sequence and never drifted:
**0.348, 0.363, 0.361, 0.378 ms.**

| offered load | ICMP RTT min/avg/max |
|---|---|
| idle | 0.327 / **0.348** / 0.408 ms |
| 1 receive flow | 0.343 / **0.409** / 2.360 ms |
| 2 receive flows | 18.6 / **20.8** / 22.1 ms |
| 4 receive flows | 19.5 / **21.4** / 25.2 ms |
| 8 receive flows (three separate arms) | 6.9-9.3 / **10.1, 13.5, 17.2** / 20.8 ms |
| 1 transmit flow | 0.370 / **0.438** / 1.682 ms |
| 4 transmit flows | 2.53 / **2.76** / 3.08 ms |
| 8 transmit flows | 7.41 / **7.74** / 10.4 ms |

Two things this establishes that the kernel probe cannot:

- **The delay is real wall-clock delay to an outside observer**, 30-60x the idle
  path, not an artefact of how the probe attributes time.
- **A single flow shows no inflation at all.** One flow is held to ~5 Gbit/s by
  the per-flow cap on this network, which is below the receive path's drain rate,
  so no queue forms. The queue is what happens when offered load exceeds the
  drain rate — which is the definition of a bottleneck being fed too hard.

The transmit rows are a by-product worth recording: **the transmit path has a
standing queue of its own**, 7.7 ms at 8 flows. It is not analysed here.

### 4.1 The sender's own view

`ss -tin` on the Linux sender, 8 flows, is unambiguous about which control loop
is binding:

```
rtt:15.095/0.213   minrtt:0.408     cwnd:190  ssthresh:28
snd_wnd:16760576   rwnd_limited:20ms(0.1%)   notsent:8079878
bytes_sent:2145266935  bytes_retrans:3490110   (0.16%)
```

- **`snd_wnd` is 16.7 MB and `rwnd_limited` is 0.1%.** The receiver's advertised
  window is wide open. Every "the application cannot drain the socket fast enough
  so the window closes" story is refuted by this line.
- **`notsent` is 8 MB.** The sender has data ready and is not sending it, so it is
  not application-limited either.
- **`rtt` is 15.1 ms against a `minrtt` of 0.408 ms.** The sender is
  congestion-window-limited at a round trip inflated 37x, and
  `cwnd x MSS / rtt = 190 x 8949 / 15.1 ms = 901 Mbit/s` per flow — which times 8
  flows is 7.2 Gbit/s, the observed aggregate. At `minrtt` the same `cwnd` would
  carry 33 Gbit/s per flow.

**The receive path throttles the sender through latency, not through the
window.** That is why no window-based or loss-based explanation ever fitted:
retransmission is 0.16%, which the Mathis bound already excluded as a cause, and
the window is never the constraint.

---

## 5. The intervention: the queue is the latency, proven by removing it

If the 13.7 ms is the FIFO, then changing the FIFO's size must change it
proportionally. The cap is settable at runtime through the probe's
`setsockopt`, so all arms run in **one boot, interleaved, non-ascending**, with
the shipped 16 MiB value re-measured four times as a bracket.

| FIFO cap | goodput | FIFO residency | ICMP RTT | consumer dispatch | drops |
|---|---|---|---|---|---|
| **16 MiB (shipped)** | 6.873, 7.391, 6.755, 7.271 → **7.07** Gbit/s | **14,250 us** | **15.1 ms** | 9.81 us | 0.015% |
| 4 MiB | 7.141 | 3,988 us | 4.44 ms | 9.68 us | 0.18% |
| 1 MiB | 7.702 | 855 us | 1.26 ms | 8.94 us | 1.5% |
| **256 KiB** | 9.165, 8.589 → **8.88** Gbit/s | **118 us** | **0.51 ms** | 7.38 us | 3.3% |
| 64 KiB | 5.079 | 20 us | 0.378 ms | 7.42 us | 4.5% |

Read the ICMP column against the residency column. Adding the 0.35 ms idle path
delay to each measured residency predicts the externally observed round trip:

| cap | residency + 0.35 ms | measured ICMP | error |
|---|---|---|---|
| 16 MiB | 14.60 ms | 15.1 ms | +3% |
| 4 MiB | 4.34 ms | 4.44 ms | +2% |
| 1 MiB | 1.21 ms | 1.26 ms | +4% |
| 256 KiB | 0.47 ms | 0.51 ms | +9% |
| 64 KiB | 0.37 ms | 0.378 ms | +2% |

**Five points spanning a factor of 256 in queue size and 40x in delay, and a
kernel histogram predicts an external ICMP measurement to within 9%.** The
queueing delay and the RTT inflation are the same quantity, measured at both
ends. This is the load-bearing evidence in the document.

**256 KiB is better than 16 MiB on both axes at once: +25.5% goodput and 29x less
latency.** That is the signature of a buffer sized past its useful knee — the
long queue was buying nothing and costing a factor of thirty in delay, and the
delay was costing throughput through TCP's control loop.

Two limits on that number, stated because it is the tempting one to quote:

- **256 KiB is a demonstration, not a shippable default.** It trades 3.3% frame
  loss for the improvement. A real fix sizes the queue in *time* rather than in
  bytes — a few hundred microseconds of the measured drain rate — or applies
  backpressure instead of dropping. Sizing in bytes is what produced a 19 ms
  buffer in the first place.
- **64 KiB is past the other knee** and shows what the falsification criterion in
  §1.2 looks like when it fires in the opposite direction: goodput collapses to
  5.08 Gbit/s, and the consumer's loop period (14.17 us) pulls away from its
  dispatch time (7.42 us) — **the consumer becomes starved**, exactly as the
  pre-registered Q1 criterion said a starved consumer would look. The instrument
  can tell the two failure modes apart.

**This also retires the "bigger FIFO changes nothing" result from
`ena-rx-cadence-falsified.md`.** That experiment moved the cap 16 MiB → 128 MiB
and correctly found no change in goodput. Both values are far past the knee; the
cap was only ever tested in the direction that could not help.

---

## 6. What limits the rate, in one line of arithmetic

| quantity | measured |
|---|---|
| consumer thread **wall-clock** loop period | 10.167 us/frame |
| consumer thread **CPU** per frame (netprof, 6.000 s window, residual +0.00%) | 5.436 us/frame |
| consumer thread wall-clock occupancy | 10.167 us x 98,052 frames/s = **99.7%** |
| consumer thread CPU utilisation | **53.3% of one core** |
| implied blocked time | 10.167 − 5.436 = **4.731 us/frame (47%)** |
| implied ceiling `1 / 10.167 us` | 98,400 frames/s = **7.09 Gbit/s** |
| **measured goodput** | **7.06 Gbit/s at 98,052 frames/s** |

**The consumer thread's wall-clock service time predicts the throughput to 0.4%.**
It is the bottleneck, it is fully saturated, and it is 53% busy.

That sentence is the resolution of the paradox this whole line of work has been
stuck on. `ena-rx-cadence-falsified.md` concluded "nothing is saturated, 97.3% of
the machine idle, the busiest thread at half a core". Both halves are true and
the conclusion does not follow: **wall-clock occupancy and CPU utilisation are
different quantities, and every instrument in this tree measured only the
second.** A thread can be 100% occupied and 50% busy, and then it is a hard
bottleneck that looks idle.

### 6.1 Where the other half of the consumer's time goes

```
wall 10.167 us  -  CPU 5.436 us  =  4.731 us/frame not executing
TCPEndpoint::fLock acquire       =  4.912 us/frame
```

**The consumer blocks for 4.9 us per frame acquiring the TCP endpoint mutex.**
The lock wait and the not-executing time agree to within 4%, which is inside the
uncertainty of a subtraction between two different instruments — so that single
lock accounts for *all* of the bottleneck thread's non-executing time, and
therefore for 47% of the ceiling.

The same subtraction on a second, independent window (the stack-only build, no
`fLock` probe, a different netprof run) gives 10.635 − 5.520 = **5.115 us/frame
blocked, 48%** — so the blocked fraction reproduces across builds and boots even
though only one of them could name the cause.

The number is not an artefact of the operating point. At a 256 KiB cap, where
everything else changes, it tracks:

| | 16 MiB | 256 KiB |
|---|---|---|
| consumer wall/frame | 10.167 us | 7.965 us |
| consumer CPU/frame | 5.436 us | 4.670 us |
| blocked/frame | 4.731 us (47%) | 3.295 us (41%) |
| `fLock` acquire | 4.912 us | 3.050 us |
| `fLock` as a share of blocked | **104%** | **93%** |

(104% is a subtraction between a wall-clock histogram and a separately measured
CPU total overrunning by 4%; it means "all of it", not "more than all of it".)

`fLock` is held on the receive side by `SegmentReceived`, on the application side
by `ReadData` while it copies to userland, and by TCP's timers. So this is a
hand-off latency after all — the receive path waiting for the application and the
timer thread — but it is **not** any of the hand-offs the hypothesis named, and it
is one layer below where the instrument was first placed.

Two candidate mechanisms were tested and one was killed:

- **`_NotifyReader` — the condition-variable wake of the reading thread — is
  0.718 us, not 4.9 us. Refuted.** A per-frame scheduler hand-off was the more
  elegant explanation and it is wrong.
- **The application's read chunk size does not move it.** 64 KiB against 1 MiB is
  a 16x change in the number of `read()` calls, hence in how often the
  application takes the lock:

  | chunk | blocked/frame | goodput |
  |---|---|---|
  | 64 K | 4.962, 5.263, 5.643 us | 7.405, 7.068, 6.792 Gbit/s |
  | 1 M | 4.813, 5.557 us | 7.421, 6.690 Gbit/s |
  | 4 K | 6.777 us | 5.986 Gbit/s |

  No difference between 64 K and 1 M. (The 4 K row is the rate-dependent cost
  inflation documented in `net-receive-profile.md` §3.2, not a lock effect.) So
  the wait tracks the *total* time the application holds the lock, which is set
  by how much data it copies, not by how many calls it makes. Reducing the call
  count cannot help; shortening or splitting the critical section might.

---

## 7. What this account explains, and what it does not

### Explained

1. **The ~7 Gbit/s ceiling.** `1 / (consumer wall-clock service time)` = 7.09
   Gbit/s against 7.06 measured. Not loss, not cadence, not queue count, not
   CPU capacity.
2. **Why every previous instrument said "nothing is saturated".** They measured
   CPU. The bottleneck is saturated in wall clock at 53% CPU.
3. **47% of that ceiling**, by name: the `TCPEndpoint::fLock` wait, 4.9 us/frame.
4. **The 30-60x RTT inflation**, and therefore the mechanism by which the ceiling
   is enforced on the sender: a standing 11.5 MiB / 13.7 ms queue in the device
   interface FIFO. Verified independently by ICMP at five queue sizes to within
   9%, and by Little's Law against occupancy to within 0.3%.
5. **Why the system sits in an equilibrium with nothing saturated.** The queue
   inflates the RTT until TCP's congestion control throttles the senders to
   exactly the consumer's drain rate. It is a closed feedback loop, and its
   set-point is the bottleneck's service time.

### Not explained — and where it must be hiding

1. **Who holds `fLock` for those 4.9 us, and why for so long.** The wait is
   measured; the holder is inferred from the code and from the chunk-size
   experiment. Directly attributing it needs the *hold* time instrumented on the
   application side (`ReadData`) and in TCP's timer callbacks, not just the
   acquire time on the receive side. That is the next measurement and it is a
   small change to the same module.
2. **The remaining 4.99 us/frame of genuine CPU in the consumer.** This account
   prices it but does not decompose it. The sampling profiler works on arm64 now
   (`830d8d0814`), and `profile -a -k` against thread 150 would break it down.
3. **The gap from 13.9 to 29.8 Gbit/s.** §0.1 forbade this document from claiming
   it, and it stands: even a perfectly unblocked consumer is capped near 14
   Gbit/s at the measured per-frame CPU cost. Reaching Linux parity needs a
   *cost* reduction as well, or more than one consumer thread.
4. **The transmit path's own 7.7 ms standing queue** (§4). Measured in passing,
   not analysed.

### A pre-registered falsifier that fired, and what it caught

§1.2 declared: *"If Q1 is deep and W3 is negligible and the consumer is at 50%,
the instrument is wrong — those three cannot be true together."* All three are
true. The queue is deep (72% full), the instrumented locks are negligible
(0.07-0.22 us), and the consumer is at 53% CPU.

The instrument is not wrong; **the trilemma was incomplete.** It assumed the only
ways for a thread to be neither starved nor CPU-busy were the locks that had been
instrumented. The fourth possibility — blocking on a lock *deeper in the call
chain than the probes reached* — is what actually happened, and finding it
required adding probes to a second module. Writing the falsifier down is what
forced that step instead of allowing the 51% to be waved at.

### And one hypothesis of my own that is refuted

§0 predicted that the reader and consumer threads were **mutually excluded**,
because the sum of their CPU service times (9.33 us) fitted the frame period
(10.28 us) suspiciously well. **That fit was a coincidence.** The FIFO mutex costs
0.22 us to acquire from the reader and 0.21 us from the consumer, the two threads
run on different CPUs simultaneously, and the consumer's wall-clock service time
alone accounts for the throughput. The reader is not in the critical path at all.

The prediction was wrong in its mechanism and right in its type: there *is* a
serialization, it is worth about half the ceiling, and it is a lock — just a
different lock, two layers up, shared with the application rather than with the
reader.

---

## 8. Reconciling with the fitted cost model

`net-receive-profile.md` fits receive cost as **2.34 us/frame + 1.85 ns/byte**,
with ~88% of the per-byte term unexplained. Two connections, one solid and one
suggestive. Both carry the caveat that the fit was taken on `c7g.large` over an
MTU sweep and this work is on `c7g.16xlarge` at a fixed MTU 9001, so these are
consistency checks, not a re-fit.

**The per-frame term: hand-offs are at most a fifth of it.** Every queue and lock
hand-off this instrument prices sums to **0.49 us/frame** (deframe 0.038 +
enqueue 0.220 + dequeue 0.214 + `receive_lock` 0.071 — excluding `fLock`, which
is a wait, not a cost, and consumes no CPU). So hand-off *mechanism* cannot be
more than 21% of the 2.34 us/frame, and is 4.8% of the consumer's service time.
Anyone planning to attack per-frame overhead should know that the hand-offs
themselves are already cheap.

**The per-byte term: part of the unexplained 88% is queue-induced cache misses,
and that is now a measurement rather than a hypothesis.** `net-receive-profile.md`
§4.1 proposed that most of the 1.85 ns/byte is *"demand cache misses on a
scattered pattern the prefetcher cannot follow"*, and labelled it explicitly
*"a hypothesis, not a result"*. Shortening the FIFO does not change a single byte
of the work done per frame — same frames, same MTU, same protocol path, same
binary — and yet:

| FIFO cap | standing queue | consumer dispatch | `SegmentReceived` |
|---|---|---|---|
| 16 MiB | 11,766 KiB ≈ 1,338 frames | 9.784 us | 7.572 us |
| 256 KiB | 124 KiB ≈ 14 frames | **7.256 us (−25.8%)** | **5.544 us (−26.8%)** |

The only variable is **how stale the frame's bytes are when the consumer finally
touches them**: 1,338 frames deep spans ~11.5 MiB and cannot survive in any cache,
14 frames deep is ~124 KiB and can. A 25.8% reduction in dispatch cost from
nothing but queue depth is direct evidence for §4.1's mechanism.

Sized against the fit: 2.53 us/frame at 9001 bytes is **0.28 ns/byte, 15% of the
1.85 ns/byte term** — *if* it is a per-byte effect. I did not sweep MTU here, so I
cannot formally separate per-byte from per-frame, and I am not claiming that
split. What is claimed is narrower and firmer: **2.53 us/frame of the consumer's
cost is recoverable by queue sizing alone, and cache residency is the only
explanation consistent with nothing else having changed.**

---

## 9. Reproducing this

Needs a Haiku node from the canonical AMI and a Linux peer on the same subnet;
neither the ENA driver nor the kernel is modified, so no image bake is involved.

```bash
# build the instrumented modules and the tool
jam -q stack tcp rxlat

# stage, then rename -- copying into a live add-on directory drops the connection
scp stack  node:/boot/home/stack.new
scp tcp    node:/boot/home/tcp.new
ssh node 'mv /boot/home/stack.new /boot/home/config/non-packaged/add-ons/kernel/network/stack
          mv /boot/home/tcp.new   /boot/home/config/non-packaged/add-ons/kernel/network/protocols/tcp
          sync; shutdown -r'
# then, before believing anything:
ssh node 'listimage | grep non-packaged; /boot/home/rxlat info'   # build stamp

rxlat calibrate            # always, and read it first
rxlat on                   # zeroes the counters, then samples
rxlat read                 # every stage, mean/min/max/percentiles/histogram
rxlat fifo 262144          # move the receive FIFO cap with no reboot
```

Method notes that cost something here:

- **The peer must fork per connection** (`nettput-peer.py --concurrent`) *and*
  each flow must be its own client process. A single-threaded peer inflated
  N-flow aggregates 2.7-4.6x in earlier work.
- **Bracket the interface counters around the measured window and time the
  window itself.** An `ifconfig` in its own remote shell puts a third of a second
  of untimed traffic inside the bracket.
- **Interleave arms, never ascend.** The within-boot spread here is ±4.8% on
  goodput, which is larger than several of the effects that matter.
- **Take an idle ICMP control before and after each sequence.** It is nearly free
  and it is what makes a 40x inflation a measurement rather than a claim.
- **A single flow cannot reproduce this at all** — the per-flow network cap holds
  it below the drain rate and no queue forms. Two flows are enough.
