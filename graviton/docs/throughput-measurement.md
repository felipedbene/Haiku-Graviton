# Throughput on Graviton: the first real numbers

**Date:** 2026-08-23. **Hardware:** `c7g.large` (Graviton3 / Neoverse V1, 2 vCPU),
`us-west-2a`, AMI `ami-05e6c7d95daeb03f4`, Haiku `hrev59996`. **Peer:** the
`c7g.metal` builder `10.42.0.149` on the same subnet, MTU 9001, RTT 0.326 ms.

Until now the jumbo-frame work was proven *correct* and not proven *faster*.
This closes that gap, and in doing so found a second bottleneck that had nothing
to do with the driver.

## The tool

`src/bin/nettput` plus `graviton/scripts/nettput-peer.py`. A stock image has no
iperf, no netperf and no compiler, so there was no way to ask the question at
all. Two decisions in it matter for reading the numbers below:

- It reports **both** the rate and **CPU microseconds per mebibyte**, the latter
  from `cpu_info::active_time` summed over every CPU. Summing all CPUs rather
  than timing our own thread is the point: the driver and the stack spend most of
  their time in interrupt and kernel threads a per-thread measurement cannot see.
  A larger MTU can leave the rate untouched while halving the cost, and that
  still counts.
- Transmit runs are timed until the peer **acknowledges the last byte**, not
  until the last write returns. Otherwise the socket buffer absorbs the tail of
  the run and reports a rate that never touched the wire.

## Result 1: jumbo frames are dramatically faster, and far cheaper

Both MTUs measured on **one running node, one boot, one image** -- MTU changed
with `ifconfig` between rows. Same kernel, same driver, same instance, so a
difference is the MTU and not the weather. 512 MiB per run, 3 runs per cell,
median reported.

| direction | MTU 1500 | MTU 9001 | rate | cpu per byte |
|---|---|---|---|---|
| **receive** | 952 Mbit/s, 8944 µs/MiB | **4933 Mbit/s**, 2115 µs/MiB | **+418%** | **0.24×** |
| **transmit** | 1148 Mbit/s, 8159 µs/MiB | 1419 Mbit/s, 3852 µs/MiB | +23.5% | 0.47× |

Receive is **5.2× faster and costs a quarter of the CPU per byte**. 3.36 GB moved
each way with **0 errors, 0 dropped** across 2.09 M packets.

Transmit's weak +23.5% is not the driver. See below.

## Result 2: transmit was capped by the default socket buffer, not the NIC

Transmit measured 1611 Mbit/s. The default socket buffer is 65535 bytes and the
RTT is 0.326 ms:

```
65535 * 8 / 0.000326 = 1608 Mbit/s        measured: 1611 Mbit/s
```

The number was the *default*, to three digits. A single TCP stream cannot have
more than one send buffer in flight per round trip, and Haiku hard-codes both
socket buffers to 65535 in `net_socket.cpp` with **no autotuning anywhere** --
`TCPEndpoint` sizes both queues from `buffer_size` once, at construction.

Sweeping `SO_SNDBUF`/`SO_RCVBUF` (nettput's `-w`, set before `connect` so the
window scale is negotiated correctly), MTU 9001, 512 MiB per run:

| window | transmit Mbit/s | receive Mbit/s |
|---|---|---|
| 64 K | 1498 | 1554 |
| 128 K | 3851 | 2590 |
| 192 K | 4399 | -- |
| **256 K** | **4451** | 4948 |
| 288 K | 4671 | -- |
| 512 K | 3832 | 4949 |
| 1 M | 3603 | 4933 |
| 4 M | 3067 | 4948 |

Interleaved control, alternating so drift cannot fake it:

| run | rate | cpu/MiB |
|---|---|---|
| tx default (65535) | 1611.6 Mbit/s | 3900 µs |
| tx `-w 256K` | 4375.8 Mbit/s | 2228 µs |
| tx default (65535) | 1396.9 Mbit/s | 4099 µs |
| tx `-w 256K` | 4420.6 Mbit/s | 2221 µs |

**~3× the throughput for 45% less CPU per byte.** The plateau is roughly
192-288 KiB and the gain *reverses* above it, so
`send.buffer_size = 256 * 1024` now sits in the middle of the measured optimum
rather than being picked for looking round.

The receive default was deliberately **left at 65535**: measured the same way it
already reached 4942 Mbit/s and every larger value tested was equal or worse, so
there is no evidence for changing it -- and unlike the send buffer, a larger
receive buffer grows the window this host advertises to a peer that may then put
that much in flight.

## Result 3: verified in a shipped image, and it unmasked the real jumbo gain

`send.buffer_size = 256 * 1024` was then baked into `ami-08e3f96650b377978` and
re-measured on a fresh `c7g.large` (`hrev59996`, RTT 0.404 ms this time). The
image reports the new default without anyone asking for it:

```
socket buffers  : send 262144, receive 65535 bytes
```

Repeating the MTU A/B on that image, 512 MiB per run, 3 runs per cell, median:

| direction | MTU 1500 | MTU 9001 | rate | cpu per byte |
|---|---|---|---|---|
| **receive** | 981 Mbit/s, 8764 µs/MiB | **4936 Mbit/s**, 2144 µs/MiB | **+403%** | **0.24×** |
| **transmit** | 1356 Mbit/s, 6585 µs/MiB | **3852 Mbit/s**, 2613 µs/MiB | **+184%** | **0.40×** |

**Transmit's jumbo gain was +23.5% before this change and is +184% after it.** The
frames were always being chained correctly; the send buffer was hiding the win.
That is the useful lesson from this whole exercise: a measurement that stops at
"the driver looks fine" can be measuring the wrong ceiling entirely.

Combined against the original baseline (MTU 1500 with a 65535 send buffer),
transmit went 1148 → 3852 Mbit/s (**3.35×**) and receive 952 → 4936 Mbit/s
(**5.2×**), each at well under half the CPU per byte.

Health after 3.32 GB in and 3.34 GB out across 1.9 M packets: **0 errors,
0 dropped**, no ENA reset, leak or stranded-descriptor messages.

## Open finding: a throughput cliff between 65535 and 65536

Not explained, and recorded rather than guessed at.

| receive buffer | rate |
|---|---|
| 65535 (default, left alone) | 4942, 4928 Mbit/s |
| **65536** (explicitly set, one byte more) | **1474, 1116 Mbit/s** |
| 262144 (explicitly set) | 3953, 4949 Mbit/s |

Consistent across interleaved repeats. One byte of requested buffer size costs a
factor of three to four.

`65535` is exactly the boundary in `TCPEndpoint::_PrepareSequenceNumbers`:

```c
while (fReceiveWindowShift < TCP_MAX_WINDOW_SHIFT
        && (0xffffUL << fReceiveWindowShift) < socket->receive.buffer_size) {
    fReceiveWindowShift++;
}
if (fReceiveWindowShift < 8 && !IsLocal())
    fReceiveWindowShift = 8;
```

Tempting, but the second clause forces the shift to 8 for every non-local
connection either way, so the loop cannot be the whole story. The remaining
suspect is the `setsockopt` path itself: `tcp_setsockopt` calls
`SetReceiveBufferSize()` and *then* falls through to the generic handler which
assigns `socket->receive.buffer_size` again, while `TCPEndpoint` has already
captured `fReceiveWindow` from the old value at construction. That is a
hypothesis and has not been tested. **A dedicated hunt is warranted** -- if
`SO_RCVBUF` can make a socket 3× slower, applications that set it "to be helpful"
are being punished for it.

## Method notes worth keeping

- **`c7g.large`, not `t4g.medium`.** T instances throttle CPU to a baseline when
  credits run out, and CPU-cost-per-byte is half of what is being measured -- a
  throttled core would corrupt exactly the number that matters. c7g is also the
  real target.
- **The security group is a measurement hazard.** The shared SG has no
  intra-group rule at all, so nothing between two project instances is permitted
  by default. This previously produced a *false negative* on jumbo TX that looked
  exactly like a driver bug. Rules for `:22`, `:5301` and ICMP were added from
  the group to itself before measuring, and revoked after.
- **A/B on one boot beats two images.** `ifconfig <iface> mtu N` on a live
  interface removes kernel, driver, image and instance from the list of things
  that could explain a difference. It also exercises the MTU-down path where the
  `ethernet_set_mtu` underflow used to be.
- **Short runs lie.** A 32 MiB receive completes in 0.057 s, which is mostly
  ramp-up. 512 MiB per run is the minimum that gave repeatable numbers.
- **Always cross-check the peer's own view.** On transmit the peer's rate agreed
  with Haiku's to within 0.3% (1604 vs 1600 Mbit/s), which is what makes the
  acknowledgement-timed measurement trustworthy. A large disagreement would mean
  bytes counted but not delivered.

## Still open

- The 65535/65536 receive cliff above.
- **No send-buffer autotuning.** 256 KiB is a better fixed guess than 65535, but
  every fixed guess is wrong somewhere: it is a waste on a LAN and too small on a
  long fat path. Linux autotunes for good reason.
- Transmit **doorbell coalescing** still needs a batched transmit entry point in
  the stack (`ETHER_SEND_NET_BUFFER` is called once per `net_buffer` with no
  "more coming" signal).
- **Multi-queue / RSS** remains blocked in the stack, so all of the above is
  single-queue. 4.4 Gbit/s is from one queue on one core pair.
- PMU counters (`arch_pmu`) are still KDL-only, so cycles-per-byte is inferred
  from `active_time` rather than counted. A userland readout would sharpen this.
