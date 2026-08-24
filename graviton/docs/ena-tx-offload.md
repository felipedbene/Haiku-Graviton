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
   stack, payload summed by the device). Hardware-verified on **c7g.4xlarge**:
   **−78.0 µs/MiB of transmit CPU, −3.54 %, permutation *p* = 0.0079** over ten
   alternating boots — with a negative control that does not move (*p* = 0.60) and
   wire-level confirmation from the peer that the checksums the device produced
   are correct (§4.4).

   **An earlier −12.6 % figure from `c7g.large` is withdrawn.** It was measured on
   the wrong instrument, and the replication that retired it is §4.1. The
   correction is 3.6×, in the unflattering direction, and it is the most important
   thing in this document.
2. **TX doorbell coalescing is cancelled, and not on cost grounds.** The device
   grants a burst of **2 LLQ ring entries between doorbells** and **one jumbo
   frame consumes both**. Measured on the wire: **99.94 % of transmit frames
   leave the burst allowance at zero** (199 880 of 200 000). A deferred doorbell
   is therefore forced by the very next frame, so the achievable coalescing ratio
   at MTU 9001 is **1:1 — exactly zero saving**, whatever a doorbell costs. The
   batched transmit entry point the stack would need is a ~250-line change across
   five modules for a provably empty win.
3. **This and the `compute_checksum()` optimisation are near-complete substitutes,
   and the loop fix is the better change.** Measured on the same instance class with
   the same instrument: loop fix +2.37 %, offload +3.54 %, **combined ≈ 3.5 % not
   5.9 %**, and offload's marginal value once the loop fix lands is **≈ 1.2 %**. The
   two agree to 12 % on the size of the pass they both attack, which is the strongest
   coherence evidence here. Whichever merges second must quote the combined figure.
   See §6.

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

So the single largest *identifiable* per-byte term on transmit was a checksum the
hardware was willing to compute for free. Note what this does not say: it does not
say the checksum was a large share of transmit cost. Measured, it is **3.5 %** (§4.2)
— worth having, and an order of magnitude less than the framing in the brief implied.
Most of transmit's per-byte cost is still unattributed.

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

### 4.1 First, the result that was wrong, and why

The first version of this document reported **−301 to −361 µs/MiB (−12.6 % to
−15.1 %), *p* ≈ 0.006** on `c7g.large`. **That figure is withdrawn.** Re-measured
on `c7g.4xlarge` with a method built to resolve a small effect, the answer is
**−78.0 µs/MiB (−3.54 %)** — a 3.6× over-statement.

Three defects in the original, in descending order of how much they matter:

1. **The send buffer was not pinned.** Runs there autotuned to 474 933 and
   510 777 bytes, and `throughput-measurement.md`'s own sweep shows 512 KiB is
   *worse* than 256 KiB. The two arms were therefore not sampling the same socket
   configuration, and the slower arm had more opportunity to land in the worse
   part of that curve. This is a confound I introduced, not a platform property.
2. **`c7g.large` boot-to-boot transmit cost is bimodal at ~19 %** (visible in the
   raw runs: an `on` arm spanning 1991–2309 and an `off` arm spanning 2174–2581).
   An effect of a few percent cannot be resolved against that with five boots.
3. **The statistics treated runs as independent.** Runs inside one boot share a
   kernel load, a DHCP lease, a driver state and a bandwidth-allowance history. A
   Mann-Whitney over 20 runs claims a confidence the design does not support; the
   unit of analysis is the boot.

There is a further possibility I did **not** settle and which must not be used to
rescue the old number: `c7g.large` is 2 vCPU at ~61 % busy, so the sending thread
contends with the driver's reader and consumer threads, and relieving it may buy
more than the work removed. If that effect is real the true `c7g.large` figure
would sit above 3.5 % — but the three defects above are sufficient to explain the
gap on their own, and **an unvalidated mechanism is not a defence of an invalidated
measurement.** Recorded as open, not as mitigation.

### 4.2 The replication: `c7g.4xlarge`, ten alternating boots

Method, all of it chosen to make a small effect resolvable:

- **One arm per boot**, alternating, in **two sequences with opposite starting
  arms** — `on off on off on off` then `off on off on` — so sequence position
  cannot pose as treatment. 5 boots per arm.
- **Send buffer pinned** with `nettput -P 262144`, auto-sizing off. This is what
  collapses within-boot spread to ~1 %.
- **2 GiB per run**, 5 transmit and 3 receive runs per boot, first run after each
  boot discarded, ≥12 s between runs (§8).
- **Verified every boot, by artefact:** MTU is 9001; `listimage` shows **8**
  modules loaded from `non-packaged/add-ons/kernel`; and the driver's advertised
  capability word matches the arm (`0x1` for `on`, `0x0` for `off`). All ten boots
  passed all three. Same binary throughout — only a driver settings file differs.
- **Permutation test on the boot medians**, not a *t*-test on runs.

Transmit, µs/MiB, per boot:

| boot | arm | runs | median |
|---|---|---|---|
| S1:1 | **on** | 2090, 2114, 2122, 2173 | **2118** |
| S1:2 | off | 2199, 2174, 2198, 2201, 2204 | 2199 |
| S1:3 | **on** | 2132, 2128, 2302, 2115, 2295 | **2132** |
| S1:4 | off | 2213, 2214, 2204, 2200, 2215 | 2213 |
| S1:5 | **on** | 2135, 2130, 2133, 2123, 2129 | **2130** |
| S1:6 | off | 2217, 2576, 2215, 2208, 2206 | 2215 |
| S2:1 | off | 2193, 2204, 2200, 2201, 2201 | 2201 |
| S2:2 | **on** | 2090, 2116, 2090, 2132, 2116 | **2116** |
| S2:3 | off | 2206, 2182, 2181, 2167, 2182 | 2182 |
| S2:4 | **on** | 2121, 2132, 2113, 2124, 2129 | **2124** |

> **within-arm boot-median spread: on 0.75 %, off 1.50 %** — against ~19 % on
> `c7g.large`. This is the precondition, and it is what makes the platform a usable
> instrument. Establish it before trusting anything measured on a new instance size.

| | mean of boot medians |
|---|---|
| offload **on** | **2124.0 µs/MiB** |
| offload off | 2202.0 µs/MiB |
| **difference** | **−78.0 µs/MiB, −3.54 %** |

**Permutation test on boot medians: 2 of 252 label assignments reach
|diff| ≥ 78.0 → two-sided *p* = 0.0079**, which is the floor for 5 versus 5 — the
observed split is the most extreme of all 252 possible.

### 4.3 Sensitivity, because one number at one setting is not a result

| variant | difference | *p* |
|---|---|---|
| all 10 boots, boot medians (headline) | −78.0 µs/MiB (−3.54 %) | 0.0079 (floor 0.0079) |
| drop S1:1, which has 4 tx runs not 5 | −76.5 (−3.47 %) | 0.0079 (floor 0.0159) |
| boot **means** instead of medians, keeping the 2576 outlier | −78.1 (−3.53 %) | 0.0159 |
| sequence S1 alone (3 v 3) | −82.3 (−3.73 %) | 0.1000 (floor 0.1000) |
| sequence S2 alone (2 v 2) | −71.5 (−3.26 %) | 0.3333 (floor 0.3333) |

The two sequences were run with **opposite starting arms** and agree in sign and to
within 0.5 percentage points, each hitting its own floor. Neither is significant
alone — that is arithmetic, not weak evidence, since three-versus-three cannot go
below *p* = 0.1.

**Negative control — receive, which must not move** (receive checksums were already
offloaded before this change):

| | mean of boot medians |
|---|---|
| offload **on** | 2733.5 µs/MiB |
| offload off | 2717.6 µs/MiB |
| difference | **+15.9 µs/MiB, +0.59 %, *p* = 0.5952** |

**The control passes.** It also sets the noise floor at ~0.6 %, so a −3.54 %
transmit effect is roughly six times the floor. (An independent agent's control on
the same instance class came out at ~0.7 %.)

**Throttling audit.** Every transmit run but one sat at 4953–4957 Mbit/s and every
receive run at 4960. The single exception — 4318 Mbit/s, 2576 µs/MiB — fell in the
**off** arm, which is the direction that would flatter this change, and the
per-boot median excluded it. Recomputing with boot *means*, which keeps it, moves
the result by 0.1 µs/MiB. That is the per-boot median doing the job it was chosen
for.

### 4.4 The wire check, which no throughput test can substitute for

A checksum wrong by a bitwise NOT (§3.3) does not show up as a bad throughput
number if TCP retransmits around it, so goodput is not evidence. The peer was asked
directly, with `tcpdump -vv` on the receiving interface — which sees exactly the
bytes the ENA device emitted:

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
offload, so its *outbound* segments are captured before its NIC fills the field in —
and there `tcpdump` reports **12 of 12 `(incorrect -> 0x…)`**. The verdict is real,
not a default.

**The one `(incorrect)`, named rather than dismissed.** Its length was **26847 =
3 × 8949**, and `generic-receive-offload: on` on the peer's interface. GRO coalesced
three wire segments into one pseudo-segment before the capture tap, keeping the
first segment's checksum field, so `tcpdump` summed three segments against one
segment's checksum. Disabling GRO and re-capturing gave **200 of 200 correct** — that
is the confirmation, not the argument.

**Peer-side counters over one 2 GiB offloaded transmit** (`nstat` delta):

```
TcpInSegs:        234114
TcpInCsumErrors:       0
TcpInErrs:             0
TcpRetransSegs:        0
```

Zero checksum errors and **zero retransmissions** in 234 114 segments.

### 4.5 Coherence with an independent change, measured the same way

This replaces a comparison the first version of this document should not have made.
It compared a measured µs/MiB saving against a first-principles figure derived from
ns/byte, and **those two are not commensurable**: `nettput`'s summed
`cpu_info::active_time` is understood to *understate* CPU actually removed (by
roughly 3.7× in one calibration), so it is a **lower bound** whose sign and ranking
are trustworthy but whose absolute magnitude cannot be set against an isolated
microbenchmark. Two µs/MiB numbers remain comparable **to each other**, which is all
an A/B needs — and that is the comparison to make:

`feat/net-checksum-fast` makes `compute_checksum()` **4.15×** faster and measures
**+2.37 %** end to end on this same instance class with this same instrument. A
4.15× loop removes `1 − 1/4.15 = 75.9 %` of the pass, so:

| route | whole pass, end to end |
|---|---|
| loop fix: 2.37 % ÷ 0.759 | **3.12 %** |
| offload, which removes all of it: measured directly | **3.54 %** |

**Two changes, two agents, two mechanisms, agreeing to 12 % on the size of the same
pass.** That is worth more than either number alone, and it is the coherence check
the earlier version tried and failed to make.

### 4.6 Soak, health and fault injection

- One **8 GiB** transfer, offloaded: 4944.2 Mbit/s at 2042 µs/MiB, 1.06 M frames.
- **`txChecksumRejected` = 0 across every run in this document**, roughly **9 million
  transmitted frames**. That counter is the canary for §3.6.
- `csum offloaded` tracks frames to within about five per boot — ARP and DHCP, which
  are not TCP and correctly do not ask.
- No leak, stranded-descriptor or out-of-range messages; no resets other than the
  two deliberately injected.

Two watchdog resets were injected mid-transfer with `ena_fault 1`, both recovering
in **33 ms**. The reason this specific test matters is narrow.
`ena_com_create_io_queue()` memsets `io_sq`, which **zeroes `cached_tx_meta`**. Since
`ena_com_meta_desc_changed()` compares only the four geometry fields, a driver that
filled `ena_meta` with zeros would memcmp-match that fresh cache, emit no meta
descriptor at all, and ask the device to checksum a packet whose header offsets it
does not know — a silent failure visible only as corrupt packets. Because the
geometry is filled unconditionally, the first frame after a reset differs from the
zeroed cache and the descriptor is re-emitted. **Verified, not reasoned: post-reset
captures are 16/16 and 200/200 correct.**

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

## 6. Honest sizing: this and `compute_checksum()` overlap, and neither should be added to the other

Two changes are chasing the same bytes, and now that both are measured end to end on
the same instance class with the same instrument, the arithmetic is settled rather
than estimated.

| change | mechanism | measured, `c7g.4xlarge` |
|---|---|---|
| `compute_checksum()` at 4.15× (`feat/net-checksum-fast`) | makes the pass cheaper | **+2.37 %** (*p* = 0.0143) |
| TX checksum offload (this branch) | removes the pass | **+3.54 %** (*p* = 0.0079) |
| implied whole-pass cost, from the loop fix (2.37 ÷ 0.759) | | 3.12 % |
| **combined** | | **≈ 3.5 %, not 5.9 %** |
| **offload's marginal value once the loop fix has landed** | | **≈ 1.2 %** |

So:

- **They are near-complete substitutes on this path, not partial ones.** Once offload
  is on, TCP does not call `compute_checksum()` over the payload at all, so the loop
  fix contributes approximately nothing to *this* measurement — and vice versa, most
  of what offload removes is what the faster loop would also have removed.
- **Adding the two headlines gives ~5.9 % and is wrong by about 1.7×.** Whoever merges
  second must measure the delta on top of the other and quote the combined figure.
- **Offload's marginal value after the loop fix is ~1.2 %** — the residue a faster loop
  cannot reach: `checksum_data()`'s walk across five `net_buffer` nodes per jumbo
  frame, and one extra touch of a mebibyte of the sender's data.

**On that evidence, offload is the weaker of the two changes to ship first**, and it
should be said plainly: the loop fix is 2.37 % across *every* checksum in the system —
loopback, every device that does not offload, IP and ICMP headers — while this is
3.54 % on one device's transmit path and 1.2 % once the loop fix exists. If only one
could ship, it should be the loop fix.

Both still ship, for reasons the percentage does not carry:

1. **The correctness work is the durable part.** The `_NEEDED` contract, the
   capability negotiation, the `RTF_LOCAL` and fragmentation exclusions, and the
   `cached_tx_meta` trap in §3.4 are the mechanism *any* future offload needs —
   receive hashing for RSS, or checksum offload on another device — and none of it
   existed.
2. **It deletes a traversal rather than accelerating one**, which is why it still
   leads a 4.15× loop by 1.2 %.
3. It is the change that established the transmit cost model has an order of
   magnitude more per-byte than per-frame in it (§6.1), which is what cancelled the
   doorbell half of this project.

### 6.1 What this contributes to the transmit cost model

There is no per-frame/per-byte fit for transmit anywhere in the tree — the
2.34 µs/frame + 1.85 ns/byte model is a **receive** measurement and does not
transfer (§1). Two points of the transmit model are now measured, and they are
the two extremes:

| term | measured | share of transmit CPU at MTU 9001 |
|---|---|---|
| doorbell — **purely per-frame** | 238 ns each, 116.3 per MiB (`c7g.large`) | **1.3 %** |
| TCP checksum — **purely per-byte** | 78 µs/MiB (`c7g.4xlarge`) | **3.5 %** |

Both are small in absolute terms, and the point is the *ratio*: the per-byte term is
2.7× the per-frame one here, and the per-frame one has a hard ceiling of zero (§5).
Note the two rows come from different instance sizes and are not directly
subtractable; each is a within-instance A/B.

The per-byte term is an order of magnitude larger than the per-frame one, which
is the same shape receive has, arrived at independently on the transmit path.
That is the strongest available answer to the sequencing question in the brief,
and it is why the per-frame item was cancelled and the per-byte one shipped.


---

## 7. What was disproven or abandoned, kept on the record


### 7.0 My own −12.6 %, withdrawn

The first version of this document reported −12.6 % to −15.1 % on `c7g.large` at
*p* ≈ 0.006. The replication on `c7g.4xlarge` returns **−3.54 %**. The old figure is
withdrawn, the causes are in §4.1, and the largest of them was mine: **I did not pin
the send buffer**, so the two arms were not sampling the same socket configuration.

Worth stating for the next person, because the failure was not a lack of care — the
first matrix on that instance was already discarded for bandwidth-allowance
contamination, and the second had a negative control that passed:

> **A passing negative control does not license a noisy instrument.** Receive moved
> only 1.8 % while transmit moved 12.6 %, and I read that as the effect being real
> and large. It was evidence that the effect was real; it was not evidence that its
> *magnitude* was right, because the confound that inflated it — send-buffer
> autotuning — acts only on the transmit path and so could not show up in the
> control.

The general lesson is to choose the instrument before the experiment: `c7g.large` has
~19 % boot-to-boot transmit spread and cannot resolve a few percent, and no amount of
care within a run fixes that.

### 7.1 "Transmit checksum offload is not worth doing on this evidence"

`net-receive-profile.md` §7 says exactly that. **Measured wrong, by 3.54 % of
transmit CPU at *p* = 0.0079** — a smaller correction than the first version of this
document claimed (§7.0), but still a real one. The reasoning was sound and the premise was
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

### 8.3 Three more preconditions, each of which cost a matrix

**Pin the send buffer.** `nettput -P <bytes>`, auto-sizing off. Unpinned, the buffer
autotunes to a different size per run (474 933 and 510 777 were both observed), and
`throughput-measurement.md`'s sweep shows 512 KiB costs more per byte than 256 KiB —
so the arms are not sampling the same socket configuration, and the effect is
whatever the autotuner happened to do. Pinning collapses within-boot transmit spread
to about 1 %. **This is what invalidated the first version of §4** (§7.0).

**Choose the instrument before the experiment.** Within-arm boot-median spread is
~19 % on `c7g.large` and **0.75–1.50 % on `c7g.4xlarge`**. A few-percent effect is
not resolvable on the former at any sample size that is practical, and care inside a
run does not compensate. Measure the spread for *your own arms* on a new instance size
before believing anything, and report it — it is the number that says whether the rest
of the result means anything.

**Check the peer's inbound rules before blaming the code.** The builder's security
group permitted **only port 5301** inbound; any other peer port fails with a connect
error from every test node, which looks exactly like a broken build. `5302-5310` now
exists from the test SG (rule `sgr-04fd3da4f6c85873b`, added by this work) so that
concurrent agents can each hold their own port — the collision class this is meant to
end cannot otherwise be avoided. This is the second time an SG rule has produced a
false negative that looked like a driver bug; the first is in
`throughput-measurement.md`.

### 8.4 What µs/MiB is, and what it may not be compared with

`nettput` derives µs/MiB from `cpu_info::active_time` summed over every CPU. That
figure is understood to **understate** CPU actually removed — by roughly 3.7× in one
calibration — so treat it as a **lower bound** whose *sign* and *ranking* are sound
but whose absolute magnitude is not.

The consequence is a rule, not a caveat: **compare µs/MiB against µs/MiB, never
against a figure derived from an isolated microbenchmark.** Two A/B numbers taken with
this instrument are comparable to each other, which is all an A/B needs — §4.5 does
exactly that, and it agrees to 12 % with an independent change. The first version of
this document instead set a measured µs/MiB saving beside a ns/byte prediction and
called the match "coherence"; that comparison was meaningless and has been removed.
Independently, two other measurements on this project have found isolated
microbenchmarks over-predicting delivered savings by ~2×, so **an isolated cost is an
upper bound on a delivered one, not a prediction of it.**

### 8.5 What this does *not* excuse

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

# every run: pin the send buffer, and use a peer port in 5302-5310 (5301 is the
# shared default and will collide with other agents)
ssh baron@$NODE "nettput -c $PEER -p 5305 -n 2G -P 262144"

# verify by artefact every boot, not once
ssh baron@$NODE 'ifconfig /dev/net/ena/0 | grep "MTU: 9001"'
ssh baron@$NODE 'listimage | grep -c non-packaged/add-ons/kernel'   # must be 8
ssh baron@$NODE 'grep "transmit checksum offload 0x" /var/log/syslog | tail -1'

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

