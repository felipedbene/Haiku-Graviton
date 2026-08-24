# TCP send-buffer autotuning

**Date:** 2026-08-24. **Hardware:** `c7g.large` (Graviton3, 2 vCPU), `us-west-2a`,
AMI `ami-0cff6f999ca5003ff` (`hrev59996`), MTU 9001. **Peer:** a `c7g.large`
running `graviton/scripts/nettput-peer.py`, same subnet. **Tool:**
`src/bin/nettput`, 512 MiB per run unless stated, comparisons interleaved.

Status: **MERGED — `85f9d73594` "tcp: autotune the send buffer towards the
bandwidth-delay product"** (added 2026-08-24).

Haiku had no send-buffer autotuning at all. The fork's answer so far was a fixed
`send.buffer_size = 256 * 1024`, which was three times better than the 65535 it
replaced ([throughput-measurement.md](throughput-measurement.md)) and still just a
guess. This replaces the guess with a measurement taken at run time, and the
headline is that the guess was **16x wrong** one order of magnitude of round-trip
time away from where it was chosen.

> **Read the 16× with its measurement condition (noted 2026-08-24).** It comes from
> the **10 ms *emulated*, one-direction** path, not from a physical link — the only
> physical RTT measured here is **0.16 ms**, and the emulation is disclosed further
> down. The 16× is real and it is the right headline for *why a fixed value cannot
> work*; it is not a throughput result anyone will see on a same-subnet EC2 pair.
>
> **And be careful with the LAN comparison in the headline table.** This document's
> own "Honest limits" section reports one 5-pair session where autotuning and pinned
> 256 KiB were **a wash** (median ratio 1.03, 2 wins of 5), and records run-to-run
> spread of **4293–5296 Mbit/s** for the same configuration. The defensible LAN claim
> is the one that section states: **equal or better, never systematically worse** —
> not a percentage. Note the headline's "it chose" column reads 256 KiB, i.e. the two
> arms converge on the same buffer size, so a large LAN delta between them would need
> explaining rather than quoting.

## The result first

Three round-trip times on the same instance, same boot, same peer, 512 MiB per
run, interleaved. Each column is a *fixed* send buffer, pinned so auto-sizing
cannot rescue it (`nettput -P`); the last column is the shipped default with no
arguments at all.

| RTT | pinned 256 KiB | pinned 1 MiB | pinned 8 MiB | **autotuned** | it chose |
|---|---|---|---|---|---|
| **0.16 ms** (LAN) | **4318** *(best fixed)* | 3598-4264 | 2915 | **5280-5296** | 256 KiB |
| **1.24 ms** | 1859 | **4307** *(best fixed)* | 2639 | **4340** | 1.5-2.9 MiB |
| **10.17 ms** | 206 | 774 | **3165** *(best fixed)* | **3702** | 8 MiB |

Mbit/s, median of 3-5 interleaved runs per cell.

Two things to read off it:

- **The best fixed value is a different value at every RTT**, and being wrong
  costs 1.5x at best and **16x** at worst (206 vs 3165 Mbit/s at 10 ms). A fixed
  256 KiB is not a conservative choice; it is the *worst* of the three on a 10 ms
  path.
- **Autotuning matches or beats the best fixed value at all three**, without
  being told the RTT, and the size it converges on tracks the path.

Cross-checked against the peer's own count of the bytes, which agreed to within
**0.15%** on every run (e.g. Haiku 5159.7 vs peer 5166.8 Mbit/s), so these are
delivered bytes and not buffered ones. 0 errors, 0 dropped on the interface
across 3.9 GB transmitted.

## Design

`TCPEndpoint::_UpdateSendBuffer()`, called from `_Acknowledged()`, mirroring
`_UpdateReceiveBuffer()`. The target is

```
target = 2 * delivery rate * minimum round-trip time
```

capped at **8 MiB**, MSS-rounded, gated by `FLAG_AUTO_SEND_BUFFER_SIZE`.

The formula is unremarkable. **Which measurements go into it is the whole
problem**, and the first version of this got it wrong in a way worth recording.

### The estimator that failed, and why

Version one sized against the **observed flight size** — the bytes outstanding
when an acknowledgement arrived — on the reasoning that it is a direct
measurement of the pipe and that the loop is self-terminating: while the queue is
the bottleneck the pipe fills to the queue, so the target is twice the queue and
it doubles; when something else becomes the bottleneck the pipe stops growing and
so does the queue.

It is self-terminating. It terminates in the wrong place. Measured on the LAN:

| | rate | cpu/MiB | it chose |
|---|---|---|---|
| autotuned, v1 (flight-based) | 3670-4210 | 2369-2773 | **1.0-3.5 MiB** |
| pinned 256 KiB | 4317-5279 | 1906-2372 | -- |

**25% slower than the fixed default it was meant to improve on.** The reason is a
feedback loop: extra queued data raises the delay, higher delay means more bytes
in flight, more bytes in flight raises the target, and the target enlarges the
queue. Flight size is not the bandwidth-delay product; it is the
bandwidth-delay product *plus whatever we ourselves have queued in the network*.
Sizing against it means chasing your own tail, and on a path where overdriving
costs no loss it chases until it hits the cap.

The fix is to use the one component of the delay our own queueing cannot inflate:
the **minimum** round trip, not the smoothed estimate. Delivery rate is
acknowledged bytes over the interval, which is goodput, not what was pushed at
the interface. That product converges on the pipe instead of following it, and
the same code then chose 256 KiB on the LAN and 8 MiB at 10 ms.

### It has to be microseconds

`tcp_now()` ticks in milliseconds and `fSmoothedRoundTripTime` is in those ticks.
A 0.16 ms round trip smooths to **zero**, so a bandwidth-delay product computed
from it is zero on exactly the path this was written for — and a 1 ms floor
would put the LAN target at 625 KiB, in the region measured to be *harmful*.

So `_SampleMinRoundTripTime()` keeps a separate microsecond probe: one
outstanding sample at a time, `system_time()` at transmit against `system_time()`
at the acknowledgement, minimum tracked over the connection. It is timed locally
at both ends, so it does not depend on the peer echoing a timestamp, and it never
samples a retransmit (an acknowledgement may have been provoked by the original,
which would report a delay shorter than the path has — fatal for a minimum).

### Why "twice", and why growth is bounded without a clamp

Twice the bandwidth-delay product is one round trip of data in flight plus one
already queued behind it, so an acknowledgement is never what the writer waits
for. Same factor and same reason as Linux's `tcp_sndbuf_expand()`.

Growth is at most a doubling per interval and needs no explicit clamp: while the
queue is the binding constraint the rate cannot exceed `size / minRTT`, so the
target cannot exceed twice the size. The interval is **one minimum round trip**,
which makes it also the ramp rate — five decisions to get from 256 KiB to 8 MiB.
At two round trips the ramp cost a measurable 8% of a 512 MiB transfer on the
10 ms path (autotuned 3212 median against 3494 for a buffer that was the right
size from the first byte, losing 3 of 4 interleaved passes). At one round trip
the autotuned case won **5 of 5** passes and 17% on the median. That is the only
tuning constant in the change that came from a measurement of two alternatives.

### The cap: 8 MiB

Half of what the receive side may reach (`UINT16_MAX << 8` = 16 MB), deliberately:
receive queue occupancy needs a peer that chooses to send that much, whereas a
send queue is filled on demand by a local writer, so the same number is not the
same exposure. It is also where the measurements stop improving — at 10 ms:

| explicit request | rate | cpu/MiB |
|---|---|---|
| 8 MiB | 3165-3730 | 2343-3207 |
| 16 MiB | **1577** | 5432 |

16 MiB is *half the speed and twice the CPU* of 8 MiB on a path where 8 MiB is
right. That is not a memory-policy number, it is a measured knee, and it happens
to coincide with the memory-policy argument.

## setsockopt semantics: a shrink is obeyed, a growth is a floor

Same rule as the receive side, and for the same reason — an explicit
`SO_SNDBUF` must never cost throughput, because the default never travels through
`setsockopt` and so pinning on every explicit call punishes the application that
tried to help:

```c
if (length < fSendQueue.Size())
    fFlags &= ~FLAG_AUTO_SEND_BUFFER_SIZE;
fSendQueue.SetMaxBytes(length);
```

Verified on hardware, at 10.17 ms RTT where the difference is most visible:

| request | reported before -> after | rate | verdict |
|---|---|---|---|
| `SO_SNDBUF` 65535 (a shrink) | 65535 -> 65535 | **51.7** Mbit/s | obeyed exactly; predicted `65535*8/10.17 ms` = 51.6 |
| `SO_SNDBUF` 1 MiB (a growth) | 1048576 -> **8388608** | 3294 | floor, not a pin -- it grew past the request |
| `SO_SNDBUF` 16 MiB (above the cap) | 16777216 -> 16777216 | 1577 | honoured; auto-sizing will not go there itself |
| nothing | 262144 -> **8388608** | 3702 | -- |

The first row is the one that matters: the application asked for 64 KiB on a path
where growing would have been **64x faster**, and the kernel obeyed it. A request
to bound memory is still a request to bound memory.

The observability in that table is new and is part of the change:
`getsockopt(SO_SNDBUF)` on TCP now reports the size the queue actually has rather
than the size last requested. Both queues auto-size, so the stored value is only
ever a starting point or a floor, and answering with it hid the entire mechanism
from anyone trying to observe it -- including from the measurement trying to
establish whether it works. `nettput` now prints it before and after every run,
which is where the "it chose" column above comes from.

## What the static default became: still 256 KiB, and that was measured

The obvious move once autotuning exists is to put the default back to 65535 and
let growth do the work. Measured, that is wrong. The `stack` add-on was rebuilt
with `send.buffer_size = 65535` and run against the same peer with the same
autotuning module, LAN, 3 reps:

| default | 4 MiB flow | 32 MiB flow | 512 MiB flow | converged size |
|---|---|---|---|---|
| 65535 | 4196 | 3704 | 4529 | 90-161 KiB |
| **256 KiB** | 4039 | **5132** | **5280-5296** | 256 KiB |

The autotuned target on that path is about 175 KiB (2 x 5.2 Gbit/s x 0.14 ms) and
a socket started at 65535 converges just below it, at 90-161 KiB — correctly, by
its own definition, and slower than 256 KiB. **The measured LAN optimum
(256-288 KiB) is above twice the bandwidth-delay product.** So on a short path
the floor is doing work the estimator cannot do, and the estimator is right not
to try: what is left over is a plateau the formula has no way to know about.

Keeping 256 KiB therefore costs nothing (it is a cap on queued data, not an
allocation, and auto-sizing simply never fires below it) and buys up to 28% on
mid-sized flows. It is not raised further because above roughly 1 MiB the gain
reverses hard — 8 MiB measured 2915 Mbit/s on that same path against 5280 for
256 KiB.

## Memory

`send.buffer_size` is a cap on queued data rather than an allocation, so raising
the ceiling costs nothing until a socket uses it. Autotuning many sockets upward
at once is a different risk profile from one fixed value, though, and it is
addressed on three levels:

1. **Growth is earned, not granted.** A socket only grows if it has *delivered*
   enough to justify the size — the target is measured goodput times measured
   minimum delay. An idle socket, an interactive socket, a socket on a slow
   client: all stay at 256 KiB forever. Nothing grows on connect.
2. **Bounded per socket at 8 MiB**, half the receive side's ceiling, for the
   asymmetry described above.
3. **It is given back under pressure.** Every sizing interval checks
   `low_resource_state(B_KERNEL_RESOURCE_MEMORY)`, and if memory is tight the
   queue is returned to `socket->send.buffer_size` — the application's own
   request, or the system default if it never asked, which is exactly the right
   floor because it is the only size anyone actually asked for. Auto-sizing stays
   on, so it regrows when the pressure clears. The receive side still only has a
   `TODO` here; this is strictly better than the path it mirrors, and it is the
   one part of the change **not** exercised on hardware, because provoking
   `B_LOW_RESOURCE_*` on a 4 GB instance while measuring throughput would have
   measured the pressure and not the buffer.

## How this was verified without baking an image

Worth recording, because it turned a 22-minute bake per iteration into about four
minutes and made four iterations affordable.

The TCP protocol is a **kernel add-on**, and `kModulePaths` in
`src/system/kernel/module.cpp` searches `B_USER_NONPACKAGED_ADDONS_DIRECTORY`
*first*. So a freshly cross-built `tcp` dropped into
`/boot/home/config/non-packaged/add-ons/kernel/network/protocols/` on a running
node overrides the one inside the read-only package, and a `shutdown -r` (53 s on
a `c7g.large`) picks it up. The same trick on `.../network/stack` is how the
65535-default experiment above was run without touching the image either.

Two smaller notes for the next person:

- `scp` to a Haiku node fails (no sftp subsystem) and piping a binary straight
  into `ssh ... 'cat >'` closed the connection. `base64 -w 200 | ssh ... 'base64
  -d >'` works, and Haiku has coreutils' `base64`.
- **The second RTT was emulated, and only in one direction.** `tc qdisc ...
  netem delay 10ms` on the *peer's* egress delays only the acknowledgement path,
  which is precisely the delay that decides how long a byte occupies the send
  queue, so it is the right emulation for send-buffer sizing — but it is not a
  physically longer path, and the data direction was never delayed. A real second
  RTT would have wanted a peer in another availability zone; the project VPC has
  a single subnet in `us-west-2a`, and adding one is a change to shared
  infrastructure rather than to an ephemeral instance. **So: the 1 ms and 10 ms
  columns are emulated-delay results, and only the 0.16 ms column is a physical
  path.**

## Honest limits

- **Run-to-run variance on a shared virtual NIC is large** — the same
  configuration measured between 4293 and 5296 Mbit/s across sessions. Every
  comparison here is interleaved and every number is a median of 3-5, and the
  paired win/loss counts are quoted where they are not unanimous. On the LAN in
  particular, one 5-pair session had autotuning and pinned 256 KiB as a wash
  (median ratio 1.03, 2 wins of 5) while a later strictly-alternating 3-pair
  session had autotuning ahead by 22-35%. The defensible claim on the LAN is
  **equal or better, never systematically worse**; the large wins are at the
  other two RTTs.
- **Minimum RTT is tracked for the life of the connection**, never re-armed. A
  path whose delay genuinely rises (a route change) keeps a stale, smaller
  minimum and therefore a smaller buffer than it wants. That is the safe
  direction, but it is a limitation; BBR re-arms its minimum on a ~10 s window.
- **The 8 MiB knee is a property of this stack, not of TCP.** The congestion
  window here is never validated (`_Acknowledged()` grows it on every
  acknowledgement with no `is_cwnd_limited` check), so the send queue is in
  practice the only thing stopping this stack from overdriving a short path.
  That is why a large fixed buffer is actively harmful, and it means the
  measured optima above would move if congestion-window validation were ever
  added. Fixing that is the real work behind these numbers and is not attempted
  here.
- **Single stream, single queue.** ~~Multi-queue/RSS is still blocked in the stack,~~
  **Multi-queue/RSS is CANCELLED on evidence, not blocked** (corrected 2026-08-24):
  Linux forced to **one** ENA queue does **29826 Mbit/s** against 29823 on eight
  (`c7g.16xlarge`), so queue count is not the limiter. "Blocked" invites someone to
  unblock it. The single-stream caveat itself stands, and
  nothing here says anything about many concurrent sockets — including the
  memory question, which is argued rather than measured.

## What is not fixed, deliberately

`socket_spawn_pending()` still copies `parent->send` *after* `create_socket()` has
constructed the `TCPEndpoint`, so a listening socket's `SO_SNDBUF` never reaches
an accepted socket's queue (defect 1 in
[tcp-rcvbuf-cliff.md](tcp-rcvbuf-cliff.md)). Send auto-sizing makes this much
less harmful — the child now grows on its own instead of being stuck at the
default forever — but the explicit request is still dropped, and an application
that sets a *small* `SO_SNDBUF` on a listener still does not get it honoured on
the children. Left alone because it is a separate change in a separate file with
its own risk.
