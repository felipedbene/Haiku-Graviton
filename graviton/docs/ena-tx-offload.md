# Transmit-side offload for ENA: what shipped, what did not, and the number that decided it

**Date:** 2026-08-24. **Hardware:** `c7g.large` (Graviton3 / Neoverse V1, 2 vCPU),
`us-west-2`, canonical AMI `ami-0d61e3910062bb80a`, Haiku `hrev59996`, single
queue, MTU 9001. **Peer:** the `c7g.metal` builder `10.42.0.149` on the same
subnet. Modules cross-built on that builder and hot-swapped into
`/boot/home/config/non-packaged/add-ons/kernel/` on the node; no image bake.

Two changes were briefed together. **One shipped and one is cancelled, and the
cancellation is the more useful result** because it is settled by a property of
the device rather than by a marginal measurement.

---

## 0. Verdict

1. **TX checksum offload ships.** IPv4 TCP only, negotiated at run time from the
   device's own advertisement, `l4_csum_partial = 1` (pseudo-header seeded by the
   stack, payload summed by the device). **−301 µs/MiB of transmit CPU (−12.6 %) by the mean, −361 (−15.1 %) by the
   median, *p* ≈ 0.006, across five alternating boots** — with a negative control
   that does not move (§4) and wire-level confirmation from the peer that the
   checksums the device produced are correct (§4.3).
2. **TX doorbell coalescing is cancelled, and not on cost grounds.** The device
   grants a burst of **2 LLQ ring entries between doorbells** and **one jumbo
   frame consumes both**. Measured on the wire: **99.94 % of transmit frames
   leave the burst allowance at zero** (199 880 of 200 000). A deferred doorbell
   is therefore forced by the very next frame, so the achievable coalescing ratio
   at MTU 9001 is **1:1 — exactly zero saving**, whatever a doorbell costs. The
   batched transmit entry point the stack would need is a ~250-line change across
   five modules for a provably empty win.
3. **This change and the pending `compute_checksum()` optimisation are near-complete
   substitutes on this path, not partial ones.** Offload dominates (301–361 µs/MiB
   against the loop fix's 181), but their combined effect on ENA transmit is
   ≈ 12.6–15.1 %, *not* the sum of the two headlines. Whichever merges second must
   quote the combined figure. See §6.

---

## 1. Why transmit, and why the checksum

`net-receive-profile.md` fitted the **receive** cost as 2.34 µs/frame +
1.85 ns/byte — 12 % per-frame, 88 % per-byte. That decomposition is a receive
measurement and **must not be reused for transmit**: all four threads in its
partition are receive threads and the 15-point sweep was `nettput -r`. There is
no equivalent fit for transmit anywhere in the tree.

But transmit differs from receive in exactly the term that matters, and it
differs in the direction that helps this project:

- **Receive checksums are already offloaded.** `ena.cpp` sets
  `NET_BUFFER_L4_CHECKSUM_VALID` from the completion descriptor and
  `tcp.cpp`'s `tcp_receive_data()` honours it, so receive never walks the payload
  to check a sum.
- **Transmit was not.** `add_tcp_header()` called
  `Checksum::PseudoHeader(...)`, which folds
  `Checksum::BufferHelper` into the sum — a full pass over every byte of every
  segment, in the sending thread, on the caller's own stack.

So transmit's per-byte share is *higher* than receive's 88 %, and the single
largest identifiable per-byte term on transmit was a checksum that the hardware
was willing to compute for free.

`net-receive-profile.md` §7 lists "Transmit checksum offload" under **"not worth
doing on this evidence"**. That judgement was reached from a receive measurement,
against a path that is already offloaded on receive. **It is superseded by §4
below**, and this paragraph exists so that the contradiction is on the record
rather than silent.

---

## 2. What the device actually offers

Read out of the device's own `GET_FEATURE(OFFLOAD)` on the instance under test,
by a driver-only hot-swap that logs the word (`ena_report_offload_capabilities()`):

```
ena: offload: tx 0x3 (ipv4 l3 csum 1, ipv4 l4 csum part 1 full 0,
                      ipv6 l4 csum part 0 full 0, tso v4 0 v6 0),
     rx supported 0x7, rx enabled 0x0
```

Four facts follow, and three of them constrain the design:

| fact | consequence |
|---|---|
| `ipv4 l4 csum **part** 1`, `full 0` | The **partial** contract is the only one available. The stack must seed the folded pseudo-header sum and the driver must set `l4_csum_partial = 1`. "Write zero and let the device do everything" is not on offer. |
| `ipv6 l4 csum part 0 full 0` | No IPv6 L4 offload **on this device**. |
| `tso v4 0 v6 0` | No TSO. The vendored `ena-com` carries full TSO support; that is a shared HAL and says nothing about this device. |
| `l3 csum ipv4 1` | Available and deliberately unused — see §3.4. |

**Capability is negotiated at run time, not compiled in.** That matters because
the answer is not the same on every instance: an independent probe using
Amazon's production Linux driver found `tx-checksum-ipv6` reported differently on
`c7g.metal` than on `c7g.large`. Whatever the cause of that discrepancy — the
device bit above is read straight from the admin queue on the instance under
test — a build that hard-codes "IPv4 and IPv6" would emit corrupt IPv6 packets
somewhere. The driver advertises the IPv6 bit **only if the device does**, so
both cases are correct without a second build.

Negotiation cost nothing to add: `ena_device_init()` already calls
`ena_com_get_dev_attr_feat()`, and the driver was reading `features.dev_attr.*`
out of the result and discarding `features.offload` entirely.

---

## 3. The design

### 3.1 Two new flags, and why the existing one could not be reused

`net_buffer.h` gains a `_NEEDED` pair beside the existing `_VALID` pair:

```c
	NET_BUFFER_L3_CHECKSUM_VALID	= (1 << 0),
	NET_BUFFER_L4_CHECKSUM_VALID	= (1 << 1),
	NET_BUFFER_L3_CHECKSUM_NEEDED	= (1 << 2),
	NET_BUFFER_L4_CHECKSUM_NEEDED	= (1 << 3),
```

`_VALID` is set on **receive** by the driver to mean "hardware checked this" and
on **transmit** by TCP to mean "I computed this". Those look like two meanings
but they are one: *the checksum bytes in this packet are correct*. Both readers
want exactly that — `tcp_receive_data()` skips verification, and the
loopback re-injection path at `datalink.cpp`'s `RTF_LOCAL` branch relies on it
for a buffer that goes out of the transmit path and straight into the receive
queue. Overloading that bit for "please compute this" would have broken the
second reader, which is why `_NEEDED` is a distinct bit rather than a third
meaning for the first.

### 3.2 Per-device capability negotiation

A flag alone buys nothing: the win only exists if the stack *stops* computing.
So the capability is plumbed, not assumed.

- `net_device` gains `uint32 tx_checksum_offload`, carrying
  `NET_DEVICE_TX_CHECKSUM_IPV4_L4` / `_IPV6_L4`. Zero — "offload nothing" — is
  what every device that has never heard of the field gets.
- `ether_driver.h` gains `ETHER_GET_TX_CHECKSUM_OFFLOAD`, appended at the end of
  the opcode enum because those values are positional and shared with
  out-of-tree drivers. A driver that does not implement it fails the call, which
  is the safe answer.
- `ethernet_up()` asks once, and only of a driver that does `net_buffer` I/O:
  the `read()`/`write()` fallback hands the driver a flat copy with nowhere to
  carry a per-buffer request. `ethernet_down()` clears it again.
- `TCPEndpoint::_CanOffloadChecksum()` reads
  `fRoute->interface_address->interface->device->tx_checksum_offload` and masks
  by the route's address family.

Two exclusions in `_CanOffloadChecksum()` are load-bearing rather than
defensive:

- **`RTF_LOCAL`.** `datalink_send_routed_data()` pushes such a buffer straight
  back into the *receive* queue. No device ever sees it, so nothing would finish
  the sum and this machine's own receive path would drop it as corrupt.
- **address family.** Per-family, because hardware support is per-family (§2).

It is recomputed per segment rather than cached at `_PrepareSendPath()`. That is
four loads and a mask against a pass over the whole segment, and an
`InterfaceAddress` can be replaced under a long-lived endpoint — a cached "yes"
would silently corrupt every segment sent afterwards.

### 3.3 The partial-checksum contract, and the trap in Haiku's helper

`l4_csum_partial = 1` means the device does **not** compute the pseudo-header and
uses the L4 checksum field as its seed. The value it wants is the **folded
one's-complement sum of the pseudo-header, *not* complemented** — the same
convention as Linux's `CHECKSUM_PARTIAL`.

Haiku's `Checksum` class could not express that, and the failure mode is
invisible on a LAN:

- `Checksum::PseudoHeader()` sums source, destination, protocol and length **and
  then the entire payload**.
- `Checksum::operator uint16()` always folds *and* complements
  (`result ^= 0xFFFF`), with no way to obtain the uncomplemented fold.

"Reuse `PseudoHeader()` and skip the payload" therefore gives a value wrong by a
bitwise NOT. So the class gained a `Sum()` accessor that folds without
complementing, `operator uint16()` was rewritten in terms of it (same value, no
behaviour change for existing callers), and a new
`Checksum::PartialPseudoHeader()` returns pseudo-header-only, uncomplemented.
Note that the length folded in is `buffer->size`, i.e. the **per-segment** TCP
length — correct for non-TSO partial offload; the exclusion of the length applies
only when TSO is enabled, and this device has no TSO.

### 3.4 What the driver has to know, and where it gets it

A flag pair is not sufficient. The device needs `l3_hdr_offset`, `l3_hdr_len`,
`l4_hdr_len` and `l4_proto` to find the field, and `net_buffer` has no
header-offset members. Two options: grow `net_buffer`, or have the driver parse.

**The driver parses**, because `ena_send()` already copies every byte of the
frame into a bounce slot, so the headers are contiguous and to hand at the moment
they are needed — one pointer dereference instead of a walk across a node chain,
and no new `net_buffer` field for every protocol to maintain.

`ena_prepare_tx_checksum()` reads ethertype, IP version and IHL, the fragment
field and the protocol, then the TCP data offset, and fills:

```c
	context->meta_valid = 1;
	context->l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;
	context->l4_proto = <TCP|UDP>;
	context->l4_csum_enable = 1;
	context->l4_csum_partial = 1;
	context->df = <IPv4 DF bit>;
	context->ena_meta.mss = 0;
	context->ena_meta.l3_hdr_offset = 14;
	context->ena_meta.l3_hdr_len = <IHL * 4>;
	context->ena_meta.l4_hdr_len = <data offset, in 32-bit words>;
```

Four subtleties, each of which is a way for this change to look like it works and
do nothing:

- **`meta_valid` must be 1** or `ena_com_prepare_tx()` writes none of the
  checksum bits: they live in the data descriptor behind
  `if (ena_tx_ctx->meta_valid)`.
- **`meta_valid = 1` with a zeroed `ena_meta` is worse than useless.** The
  *meta* descriptor is only emitted when `ena_com_meta_desc_changed()` sees a
  difference from `io_sq->cached_tx_meta`, which compares **only** the four
  geometry fields — never the checksum bits. A fresh queue's cache is all zeros,
  so a zeroed `ena_meta` memcmp-matches, no meta descriptor is ever sent, and the
  device is asked to checksum a packet whose header offsets it does not know.
  Filling the geometry unconditionally, as the reference does in its non-TSO
  path, is what makes it work. It also costs nothing in steady state: the cache
  means one meta descriptor for the whole stream, not one per frame.
- **`df` is written outside the `meta_valid` block** and so has always been sent
  as 0, because the context is `memset`. The contract says it must be 1 unless the
  packet is IPv4 with DF clear, so it is now set from the header.
- **Do not copy the reference's "anything that is not TCP is UDP" fallback.**
  Linux is safe only because it never marks ICMP/GRE/ESP `CHECKSUM_PARTIAL`. Here
  the protocol is checked explicitly and anything else is refused, because a UDP
  checksum written at offset 6 of an ICMP or ESP header is silent corruption.

**L3 (IPv4 header) checksum offload is deliberately not enabled**, although the
device offers it. The reference only sets `l3_csum_enable` under TSO, and the
reward is 20 bytes of header against a workload that is overwhelmingly per-byte:
risk with no measurable win. `NET_BUFFER_L3_CHECKSUM_NEEDED` is defined for
symmetry with the `_VALID` pair and nothing sets it.

### 3.5 The two places an unfinished checksum must not escape to

**Fragmentation.** `split_buffer()` copies `buffer_flags` into every fragment, so
an unfinished checksum would be inherited by fragments that cannot carry one:
only the first holds the L4 header, and no NIC offloads across IP fragments. So
`send_fragments()` in `ipv4.cpp` now finishes the sum in software *before*
splitting and swaps `_NEEDED` for `_VALID`. It needs one pass and no
seed arithmetic: the field already holds the pseudo-header sum and lies inside
the summed range, so `checksum(buffer, headerLength, ...)` picks it up along with
the payload.

For TCP this is unreachable — a segment is sized from the same MTU being
fragmented against — but "should not" is not "cannot" (a route's MTU can shrink
under an established endpoint) and the failure mode without it is a peer silently
discarding every fragment.

**Loopback re-injection.** Handled at the source, by excluding `RTF_LOCAL` routes
in `_CanOffloadChecksum()` (§3.2). The IPv4 multicast-loopback `duplicate()` is
not reachable from TCP.

### 3.6 The driver's last line, and what happens if it ever fires

`ena_send()` validates before offloading, and the validation is deliberately
**stricter than the negotiation**: every rejection is a bug above, which is
exactly why it is checked rather than assumed. On rejection the frame is
**dropped** with a rate-limited `ERROR` and a counter, rather than put on the
wire with a half-computed checksum.

Dropping is not free of consequence and that is recorded here rather than
glossed: `TCPEndpoint::_PrepareAndSend()` frees the buffer and returns the error
**without advancing `fSendNext`**, so the same segment is retransmitted by the
retransmit timer, rejected again, and the connection eventually resets. That is
loud and bounded — not a spin — and it is strictly better than a corrupt packet
the peer discards silently, which produces the same stall with no diagnostic.

A software fallback in the driver was considered and rejected: the only
*plausible* rejection cause is an IP fragment, and for a fragment neither the
driver nor anything else can compute a correct sum from one fragment's bytes. An
unreachable fallback that would be wrong in the one case it might be reached is
worse than no fallback.

The rejection path returns **before** `ena_com_prepare_tx()`, so it strands no
descriptors and needs no doorbell on the error path.

**Measured: 0 rejections in 2.4 million transmitted frames** across every run in
§4.

---

## 4. Measured


Every number below is 2 GiB per run at MTU 9001 on `c7g.large`, driven from the
metal peer. The condition is selected by a **driver settings file**
(`tx_checksum_offload false`), so **both conditions run the same binary** — the
only difference is whether the driver advertises the capability and therefore
whether the stack computes the sum.

### 4.1 The A/B, alternating boots

Offload cannot be toggled inside a boot: `ethernet_up()` reads the capability
once. So the conditions alternate at boot level, four runs per boot after a
discarded warm-up. Transmit CPU, µs/MiB:

| boot | offload | runs | median | mean |
|---|---|---|---|---|
| 1 | **on** | 1991, 2035, 2035, 2022 | 2028 | 2020.8 |
| 1 | off | 2564, 2210, 2227, 2581 | 2395 | 2395.5 |
| 2 | **on** | 2031, 2272, 2003, 2291 | 2151 | 2149.3 |
| 2 | off | 2579, 2574, 2190, 2191 | 2382 | 2383.5 |
| 3 | **on** | 2000, 2309, 2034, 2034 | 2034 | 2094.3 |
| 3 | off | — the node failed to boot; see §7.3 | | |

| | n | mean µs/MiB | median | mean Mbit/s |
|---|---|---|---|---|
| offload **on** | 12 | **2088.1** | **2034** | **4847.9** |
| offload off | 8 | 2389.5 | 2395.5 | 4208.7 |
| **difference** | | **−301.4 (−12.6 %)** | **−361.5 (−15.1 %)** | **+15.2 %** |

Mann-Whitney U = 12 of a possible 96, *z* = −2.74, ***p* ≈ 0.006**. The
distributions barely overlap: only two of eight `off` runs fall below three of
twelve `on` runs.

Paired within a boot pair, on the means: boot 1 −374.7 µs/MiB (−15.6 %), boot 2
−234.2 (−9.8 %).

**Read µs/MiB, not Mbit/s.** The rate is window-limited, and it moved anyway
(+15.2 %) because a cheaper sender keeps the window fuller — but the CPU cost is
the number the change acts on.

### 4.2 The negative control: receive must not move

Receive checksums were already offloaded before this change, so transmit checksum
offload must leave receive alone. Same runs, same boots, interleaved with the
transmit runs above:

| | n | mean µs/MiB |
|---|---|---|
| offload **on** | 6 | 2210.8 |
| offload off | 4 | 2250.8 |
| difference | | −40.0 (−1.8 %) |

Mann-Whitney U = 8 of 24, *z* = −0.75, ***p* ≈ 0.45 — not significant**, with
heavily overlapping ranges (on 2114–2278, off 2181–2277). **The control passes:**
the thing that should not move does not, while the thing that should moves by
seven times as much at a hundredth of the *p*-value.

### 4.3 The wire check, which a throughput test cannot substitute for

A checksum that is wrong by a bitwise NOT (§3.3) does not show up as a bad
throughput number if TCP retransmits around it, so goodput is not evidence. The
peer was asked directly, with `tcpdump -vv` on the receiving interface — which
sees exactly the bytes the ENA device emitted:

```
10.42.0.79.40056 > 10.42.0.149.5301: Flags [.], cksum 0x3f3c (correct),
    seq 5341483:5350432, ..., length 8949
```

| capture | verdicts |
|---|---|
| offloaded transmit, 12 jumbo segments | **12 × `(correct)`** |
| offloaded transmit after one injected device reset | 15 × `(correct)`, 1 × `(incorrect)` — explained below |
| offloaded transmit after a second injected reset | **16 × `(correct)`** |
| offloaded transmit, **GRO disabled on the peer**, 200 segments | **200 × `(correct)`** |

**Positive control for the instrument.** "`correct`" is only worth something if
`tcpdump` would say otherwise. The peer is Linux with its own transmit checksum
offload, so its *outbound* segments are captured before its NIC fills the field
in — and there `tcpdump` reports **12 of 12 `(incorrect -> 0x…)`**. The verdict is
real, not a default.

**The one `(incorrect)`, named rather than dismissed.** Its length was **26847 =
3 × 8949**, and `generic-receive-offload: on` on the peer's interface. GRO
coalesced three wire segments into one pseudo-segment before the capture tap,
keeping the first segment's checksum field, so `tcpdump` summed three segments
against one segment's checksum. Disabling GRO and re-capturing gave **200 of 200
correct** — that is the confirmation, not the argument.

**Peer-side counters over one 2 GiB offloaded transmit** (`nstat` delta):

```
TcpInSegs:        234114
TcpInCsumErrors:       0
TcpInErrs:             0
TcpRetransSegs:        0
```

Zero checksum errors and **zero retransmissions** in 234 114 segments. Nothing was
silently dropped and re-sent.

### 4.4 Coherence with an independently measured quantity

`compute_checksum()` was independently measured at **0.229 ns/byte**. One pass
over a mebibyte is therefore `1048576 × 0.229 ns` = **240 µs/MiB** predicted,
against **301 measured** (mean) or **361** (median).

Predicting the right *size* from a completely different instrument is worth more
than either number alone. The 25–50 % excess is where it should be:
`checksum_data()` does not run that loop over a flat buffer, it walks a node
chain — five nodes per jumbo frame at 1920 bytes each — and it touches a mebibyte
of data an extra time on a machine where the receive profile already showed
per-byte cost is dominated by things other than the arithmetic.

### 4.5 Soak and health

- One **8 GiB** transfer, offloaded: 4944.2 Mbit/s at 2042 µs/MiB, 1.06 M frames.
- **`txChecksumRejected == 0` across every run in this document**, roughly
  **7.6 million transmitted frames**. That counter is the canary for §3.6: it
  going non-zero means something above set `_NEEDED` on a frame this device
  cannot finish.
- `csum offloaded` tracks frames to within about five per boot — the difference is
  ARP and DHCP, which are not TCP and correctly do not ask.
- No leak, stranded-descriptor, out-of-range or reset messages other than the two
  deliberately injected ones.

### 4.6 Fault injection: reset, and the trap it was aimed at

Two watchdog resets were injected mid-transfer with `ena_fault 1` (against a
driver built with `-DENA_DEBUG_FAULT_INJECTION`), both recovering in **33 ms**.

The reason this specific test matters is narrow and worth stating.
`ena_com_create_io_queue()` memsets `io_sq`, which **zeroes
`cached_tx_meta`**. Since `ena_com_meta_desc_changed()` compares only the four
geometry fields, a driver that filled `ena_meta` with zeros would memcmp-match
that fresh cache, emit no meta descriptor at all, and ask the device to checksum
a packet whose header offsets it does not know — a silent failure that only shows
up as corrupt packets. Because the geometry is filled unconditionally, the first
frame after a reset differs from the zeroed cache and the descriptor is
re-emitted. **Verified, not reasoned: post-reset captures are 16/16 and 200/200
correct.**


---

## 5. Doorbell coalescing: cancelled, and why that is a hardware fact


### 5.1 The device will not let it work

In LLQ placement the device grants a **burst allowance** of
`llq_info.max_entries_in_tx_burst` ring entries, and **a doorbell is what refills
it**. On this hardware the boot log says:

```
ena: LLQ configured: 256 byte entries, 16 descriptors per entry,
     max 2 per burst, transmit header limit 224
```

**Two entries per burst.** And a jumbo frame needs two of them:
`ena_com_is_doorbell_needed()` computes
`1 + ceil((num_bufs − descs_num_before_header) / descs_per_entry)`, which for a
9015-byte frame in five 1920-byte bounce slots is `1 + ceil((5 − 2) / 16)` = 2.

So the prediction is that one jumbo frame consumes the whole allowance. It was
measured rather than left as arithmetic — the driver now reads
`entries_in_tx_burst_left` immediately *before* ringing the doorbell:

```
ena: tx: 200000 frames, 200000 doorbells, burst left min 0, exhausted 199880
```

**99.94 % of transmit frames leave the burst allowance at zero**, and
`burst left min` never rose above 0 in any run. With checksum offload enabled the
figure is 98.7–99.4 % rather than 99.94 %, because `meta_valid` makes
`ena_com_is_doorbell_needed()` count a meta descriptor on the rare frame where the
cached geometry changes; the conclusion is the same either way. A deferred
doorbell is therefore *forced* by the very next frame:
`num_entries_needed (2) > entries_in_tx_burst_left (0)`. The achievable
coalescing ratio at MTU 9001 is **1:1. The saving is exactly zero, whatever a
doorbell costs.**

At MTU 1500 a frame needs one entry (`num_bufs` = 1 ≤ `descs_num_before_header`),
so two frames fit one burst and the best case saves half the doorbells.

### 5.2 What a doorbell costs, so the ceiling is a number and not an argument

Priced without building anything, using a runtime knob
(`ena_fault doorbells <n>`, `ENA_IOCTL_TX_EXTRA_DOORBELLS`) that rings the
doorbell *n* extra times per frame. The extra writes carry a tail the device has
already been told about, so they change nothing except how much MMIO the transmit
path pays.

Interleaved **inside one boot** — which matters, because run-to-run transmit cost
here is bimodal at roughly ±10 % (visible in the `extra=0` column) and that
swamps the effect across a reboot. 1 GiB per run, MTU 9001:

| pair | 0 extra | 8 extra | Δ µs/MiB |
|---|---|---|---|
| 1 | 2324 | 2405 | +81 |
| 2 | 2021 | 2404 | +383 |
| 3 | 2021 | 2411 | +390 |
| 4 | 2016 | 2178 | +162 |
| 5 | 2323 | 2413 | +90 |

Mean Δ **221.2 µs/MiB** for 8 extra doorbells per frame; SE 68.9, paired
*t* = 3.21 on 4 df, ***p* ≈ 0.032**, 5 of 5 pairs positive. Verified by artifact
rather than by the knob's exit code: the driver counter read
**6 730 480 doorbells for 1 700 000 frames**.

At 116.3 frames per MiB (9015 bytes on the wire), 8 extra is 930 extra doorbells
per MiB, so:

> **one transmit doorbell ≈ 238 ns**, and the *entire* doorbell cost at MTU 9001
> is 116.3 × 0.238 = **27.7 µs/MiB, or 1.3 %** of the 2141 µs/MiB baseline.

### 5.3 The ceiling, and the price of reaching it

| MTU | best achievable ratio | doorbells saved per MiB | µs/MiB saved | share of that MTU's transmit cost |
|---|---|---|---|---|
| **9001** | **1:1** (measured, §5.1) | **0** | **0** | **0 %** |
| 1500 | 2:1 | 346 | 82 | ≈ 1.25 % (against ~6585 µs/MiB) |

Against that, the change needed is a batched transmit entry point: a new
`net_device_module_info` slot with a scalar fallback for all five device modules
that implement `send_data` (ethernet, loopback, tunnel, dialup, ppp), plumbing
through `datalink.cpp`, and a driver rework. Call it 250 lines across the whole
networking subsystem to buy zero at the operating point.

**Cancelled.** Not "deferred" — the ceiling is a property of the device's burst
allowance, so it does not become worth doing later unless the hardware changes.

### 5.4 Two hazards found while deciding, recorded because they outlive this decision

Anyone who revisits this needs both of these, and neither is obvious from the
code:

1. **Deferring the doorbell deadlocks the transmit path, it does not merely delay
   a frame.** `ena_send()` blocks on `acquire_sem(device->txCompleted)` with **no
   timeout** when the submission queue is full. Completions only ever arrive for
   descriptors the device has been *told about*. A batch that writes *k* frames
   with the doorbell deferred and then finds the ring full is waiting on
   completions for its own un-rung descriptors — forever, and nothing recovers it:
   the watchdog is keep-alive-only and an idle-but-healthy device keeps sending
   keep-alives. This is the common case at line rate, not an edge case: the wait
   loop exists precisely *because* the ring saturates. Any implementation must
   ring **before** any blocking wait. Upstream never meets this because it stops
   the queue and returns instead of blocking.
2. **Six-plus early returns in `ena_send()` would strand un-rung descriptors**,
   and the guard the reference uses for exactly this (`ena_com_used_q_entries`)
   **does not exist** in the vendored `ena_freebsd_2.8.4` HAL — which must stay
   byte-identical, so it cannot be added. Nor does `ena_com_io_sq` record what was
   last written to `db_addr`, so an implementation has to track its own
   `lastDoorbellTail` and flush when `io_sq->tail` differs.

A third, smaller thing: the comment at the doorbell site describing upstream's
`ENA_DB_THRESHOLD` of 64 is **stale** — upstream has no such constant now and uses
a per-call "more coming" boolean. The comment has been corrected to describe the
burst-allowance finding instead.


---

## 6. Honest sizing: this and `compute_checksum()` overlap, and offload dominates


This has to be said plainly, because two changes are chasing the same bytes, and
the honest answer is less flattering to this change than the headline in §4.

`compute_checksum()` in `stack/utility.cpp` is a 2-bytes-per-iteration scalar loop
with its `TODO: unfold loop for speed` still in place. On `feat/net-checksum-fast`
it is independently measured at **0.228 → 0.055 ns/byte, a 4.15× speed-up**. That
is faster than the 2.4× this section originally assumed, and the better the loop
gets the less offload is left to remove.

Per mebibyte, one pass over the payload:

| | ns/byte | µs/MiB | µs per 9001-byte segment |
|---|---|---|---|
| the loop as it is today | 0.228 | 239 | 2.05 |
| the loop at 4.15× | 0.055 | 58 | 0.50 |
| **what the loop fix removes** | | **181** | **1.55** |
| **what offload removes (measured, §4)** | | **301–361** | **2.6–3.1** |

So the accounting is:

- **Offload strictly dominates on this path.** It removes 301–361 µs/MiB; the loop
  fix removes a *subset* of that, 181 µs/MiB. The 120–180 µs/MiB difference is the
  part that is not arithmetic at all — `checksum_data()`'s walk across five
  `net_buffer` nodes per jumbo frame, and touching a mebibyte of the sender's data
  an extra time on a machine whose per-byte cost is dominated by memory behaviour
  rather than by instructions.
- **Once offload is on, the loop fix contributes approximately nothing to *this*
  measurement**, because TCP no longer calls `compute_checksum()` over the payload
  at all. These two are not "partial substitutes" on the ENA transmit path — they
  are near-complete substitutes, with offload the larger of the two.
- **After the loop fix lands, offload's remaining marginal value is 120–180 µs/MiB,
  or roughly 5–8 %** of a post-loop-fix transmit baseline of ~2208 µs/MiB — down
  from the 12.6–15.1 % in §4.
- **The combined effect of both changes on ENA transmit is therefore ≈ offload's
  effect alone, 12.6–15.1 %, not the sum of the two headlines.** Adding them gives
  roughly 20 %, which would be wrong by about a factor of 1.5.

**Consequence for whoever merges second: measure the delta on top of the other, and
quote the combined figure rather than your own.** Either change measured against
the unmodified baseline will legitimately claim most of the same microseconds.

The loop fix is still clearly worth having — it is just worth having for reasons
this measurement cannot see: loopback, every device that does not offload, IP and
ICMP header checksums, and any future protocol that has to compute in software.
Its value is breadth. This change's value is that it deletes a traversal rather
than accelerating one, which is why it still leads by 120–180 µs/MiB even against
a 4.15× loop.

If the arithmetic had come out the other way — if the loop fix had covered the
whole 301 µs/MiB — the right recommendation would have been to ship the loop fix
alone and drop this change, and that would be written here instead. It did not:
the measured saving exceeds the *entire* cost of the loop it replaces (301–361
against 239), which is only possible because most of what offload removes was never
the loop.

### 6.1 What this contributes to the transmit cost model

There is no per-frame/per-byte fit for transmit anywhere in the tree — the
2.34 µs/frame + 1.85 ns/byte model is a **receive** measurement and does not
transfer (§1). Two points of the transmit model are now measured, and they are
the two extremes:

| term | measured | share of transmit CPU at MTU 9001 |
|---|---|---|
| doorbell — **purely per-frame** | 238 ns each, 116.3 per MiB | **1.3 %** |
| TCP checksum — **purely per-byte** | 301–361 µs/MiB | **12.6–15.1 %** |

(The checksum row is the cost of the *whole pass*, of which the scalar loop itself
is 239 µs/MiB and the node walk and extra data touch are the remaining 62–122 —
see §6.)

The per-byte term is an order of magnitude larger than the per-frame one, which
is the same shape receive has, arrived at independently on the transmit path.
That is the strongest available answer to the sequencing question in the brief,
and it is why the per-frame item was cancelled and the per-byte one shipped.


---

## 7. What was disproven or abandoned, kept on the record


### 7.1 "Transmit checksum offload is not worth doing on this evidence"

`net-receive-profile.md` §7 says exactly that. **Measured wrong, by 12.6–15.1 %
of transmit CPU at *p* ≈ 0.006.** The reasoning was sound and the premise was
not: the judgement came from a receive measurement, and receive checksums were
already offloaded, so the receive path contained none of the cost the transmit
path was paying. That entry is superseded rather than deleted, and this is the
correction.

### 7.2 "The full-offload form would be simpler"

It would — no pseudo-header seed, no `Checksum` change, nothing for the stack to
compute. **The device does not offer it:** `ipv4 l4 csum full 0` (§2). Only the
partial contract exists, so the pseudo-header work is not a design choice.

### 7.3 One boot in six lost the NIC, and it was not this change

Boot 3/off never came up. The serial console shows **no panic and no KDL**; it
shows PCI enumerating only `0:0:0` and `0:1:0`, no `ena: found an ENA device`, and
a boot that proceeds normally to packagefs verification and then has no network.
The device was absent from the PCI bus on that boot, which is below anything this
change touches (it does not go near attach or enumeration). Recorded as an
observed reboot flake — 1 in 6 warm reboots on `c7g.large` — rather than
attributed. The node was replaced and the remaining work repeated on a fresh one.

The full serial capture of the failing boot — and of the healthy boot immediately
before it, in the same 64 KiB window, which is what makes the comparison possible —
is kept at
`s3://haiku-graviton-668984504585-us-west-2/evidence/warm-reboot-nic-loss-i-02bab0e2575cde9a7-console.txt`.
The decisive part is that the second boot's PCI section enumerates only `0:0:0`
and `0:1:0` and contains no `found an ENA device`, while the first boot's contains
the full attach sequence.

**This will surface as a spurious perf-gate failure**, not as a visible NIC
problem: the gate's symptom is "sshd never answered", which is exactly what a
booted machine with no network looks like.

### 7.4 A software fallback in the driver: considered, rejected

The obvious safety net for §3.6 is for the driver to compute the sum itself over
its bounce copies. It was rejected because the only *plausible* rejection cause is
an IP fragment, and for a fragment the driver cannot produce a correct sum from
one fragment's bytes however it computes it. An unreachable fallback that would be
wrong in the one case it might be reached is worse than no fallback, so the frame
is dropped loudly instead.

### 7.5 An IPv6 discrepancy left open

This device reports `ipv6 l4 csum part 0 full 0` from its own
`GET_FEATURE(OFFLOAD)` on the instance under test, so no IPv6 offload is claimed
here. An independent probe using Amazon's production Linux driver reported
`tx-checksum-ipv6: on` for `c7g.large` and `off [fixed]` for `c7g.metal`. The two
readings disagree and the disagreement is **not resolved**; it may be Linux
reporting its own generic checksum handling rather than the device bit.

It does not need resolving for this change to be correct, and that is the point of
doing it by negotiation: the driver claims the IPv6 bit **only if the device
advertises it**, so both readings produce correct behaviour from one build. A
version that had hard-coded "IPv4 and IPv6" from a `c7g.large` validation would
emit corrupt IPv6 packets elsewhere.

### 7.6 Measured at MTU 1500: attempted, not obtained

The doorbell price was to be measured at MTU 1500 as well, where the per-frame
term is largest. `nettput` returned no result for any of the ten runs at that MTU
and it was not retried, because the MTU-1500 ceiling follows arithmetically from
the doorbell cost and burst geometry that *were* measured (§5.3). Recorded as
not-measured rather than quietly omitted.

### 7.7 Not done, deliberately

- **UDP.** `UdpEndpoint::SendRoutedData()` has the route in hand and would be
  easier to wire than TCP was, but there is no bulk UDP in anything measured here,
  and it opens a question nothing in `ena-com` answers: under partial offload the
  RFC 768 "zero means no checksum, send 0xffff instead" fixup moves into hardware,
  and whether this device emits 0xffff rather than 0x0000 is neither documented
  nor verified. The driver accepts `IPPROTO_UDP` so that the day someone wires it
  the driver side already works, but nothing sets `_NEEDED` for UDP.
- **L3 (IPv4 header) checksum offload** — §3.4.
- **TSO.** The device advertises none (`tso v4 0 v6 0`), on any Graviton
  generation tested. The vendored `ena-com` carries full TSO support and that is a
  trap: it is a HAL shared across many devices.


---

## 8. Measuring transmit on a burstable instance: a recipe, not a footnote

**This is the third independent time this class of corruption has been hit on this
project, so it is written here as a procedure to copy rather than as a note about
one run.** Anyone measuring transmit CPU on `c7g.large` needs it.

### 8.1 The failure

`c7g.large` does not have a flat network allowance: it sustains a **baseline** rate
and can exceed it only against a credit balance. Back-to-back multi-gigabyte
transfers drain that balance, and when it runs out the connection rate collapses.

The reason that is a *correctness* problem and not just noise:
`net-receive-profile.md` §3.2 established that **per-byte CPU cost tracks the
connection rate**, not the frame size — at a fifth of the rate every thread's cost
per byte inflates. So a throttled run does not add scatter around the true value,
it **biases µs/MiB upward**. And because throttling arrives as a function of
*cumulative bytes moved*, it lands preferentially on whichever condition the
script runs later — which manufactures an effect, with the right sign, in whatever
direction the loop happens to be ordered.

**Observed, in the first attempt at the A/B in §4:** two runs at **237 and
385 Mbit/s** against a 4400–5000 Mbit/s cohort, in the second half of the matrix.
The whole matrix was discarded. Had those two runs landed in the `off` half they
would have *supported* the hypothesis, at a plausible-looking magnitude.

### 8.2 The recipe

| knob | value that worked | why |
|---|---|---|
| bytes per run | **≤ 2 GiB** | 2 GiB is ~4 s at line rate; enough to average over (512 MiB was the previously established floor for repeatability) without being a sustained load |
| idle between runs | **≥ 20 s** | brings the duty cycle to ~4 s in 24 s ≈ 17 %, i.e. a long-run average near 0.6 Gbit/s — under this size's baseline |
| after each boot | **discard the first run** | consistently unrepresentative, in both conditions |
| every run | **record the rate alongside µs/MiB** | the rate is the throttling detector; a cost figure with no rate beside it cannot be audited |
| any run far below the cohort rate | **discard the whole matrix, not the run** | if the allowance was exhausted, the runs *around* it are suspect too |

Roughly fifty runs at those settings showed no collapse. The generalisation, for a
different instance size or a longer run: **keep the long-run average under the
instance's baseline bandwidth, not its burst bandwidth.** The exact baseline for a
size matters less than the rule and the detector — the rate column tells you when
you have got it wrong, and it is the only thing that does.

### 8.3 What this does *not* excuse

A duty cycle low enough to avoid throttling does not remove the need for
interleaving. Run-to-run transmit cost here is still bimodal at roughly ±10 %
(plainly visible in the `extra=0` column of §5.2, which alternates 2016–2021 and
2323–2324 within a single boot). Throttling and bimodality are two different
problems: the recipe above handles the first, and only alternating conditions
handles the second.

---

## 9. Reproducing this


From the metal builder against a `c7g.large` booted from the canonical AMI.

**All of `stack`, `ethernet`, `loopback`, `tunnel`, `ipv4`, `tcp` and `udp` must be
swapped in the same step.** `net_device` grew a field, so a `stack` from one build
and a `loopback` from another disagree about the size of a struct that device
modules allocate and the stack writes through. **A partial swap is a silent ABI
mismatch, not a load failure** — nothing refuses to load and nothing logs a
complaint; the stack simply reads or writes past the end of a smaller allocation.
This constrains the module-hotswap shortcut as much as it constrains the bake: the
fast iteration loop is still available, but only at the granularity of the whole
set.

```bash
# on the builder
cd /opt/haiku/txoff/haiku/generated.arm64
jam -q -j32 ena stack ethernet tcp ipv4 udp loopback tunnel ena_fault

# onto the node -- scp does not work against Haiku
NP=/boot/home/config/non-packaged/add-ons/kernel
base64 -w 200 <file> | ssh baron@$NODE "base64 -d > $NP/<path>"
ssh baron@$NODE 'sync'          # file data is otherwise never flushed
ssh baron@$NODE 'sync; (sleep 1; shutdown -r -q) &'

# verify by artifact before believing any number
ssh baron@$NODE 'grep -E "ena: offload:|checksum offload" /var/log/syslog'
ssh baron@$NODE 'ifconfig /dev/net/ena/0 | grep MTU'

# the A/B: same binary, different advertisement
ssh baron@$NODE 'echo "tx_checksum_offload false" > \
    /boot/home/config/settings/kernel/drivers/ena; sync'   # then reboot

# the wire check -- on the PEER, with GRO off or GRO will lie
sudo ethtool -K $IF gro off
sudo tcpdump -i $IF -vv -n -c 200 "tcp and src host $NODE and dst port 5301 and greater 1000"

# the doorbell price, interleaved inside one boot
ssh baron@$NODE '/boot/home/ena_fault doorbells 8'   # and 0, alternating
```

Method notes that cost something to learn:

- **`c7g.large` has a burst bandwidth allowance, and exhausting it corrupts the
  metric.** Sustained back-to-back multi-gigabyte runs eventually collapse the
  rate, and per-byte CPU cost inflates when the rate drops (already established
  in `net-receive-profile.md` §3.2) — so a throttled run does not merely add
  noise, it biases µs/MiB upward. A 20 s gap between runs and ≤2 GiB per run kept
  the duty cycle under the baseline. The first attempt at this A/B had two runs at
  237 and 385 Mbit/s and was discarded entirely.
- **Discard the first run after a boot.** It is consistently unrepresentative.
- **Interleave inside a boot wherever the knob allows it.** The doorbell knob is
  an ioctl precisely so that its A/B can be; the offload A/B cannot, because the
  capability is read once at `ethernet_up()`, and it is visibly the noisier of the
  two measurements as a result.
- **A goodput A/B cannot validate a checksum.** Ask the peer, and give the
  instrument a positive control, or `(correct)` means nothing.
- **GRO on the capturing host will report a false `(incorrect)`.** The tell is a
  length that is an exact multiple of the MSS.
- **`SubDirC++Flags` must precede the `KernelAddon` declaration** or it is
  silently ignored — the first `-DENA_DEBUG_FAULT_INJECTION` build appended it to
  the end of the Jamfile, produced a different binary (the embedded build
  timestamp), and had none of the fault ioctls. Verify the *capability*, not the
  checksum of the file.

