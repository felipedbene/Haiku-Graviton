# TCP segmentation offload: the device cannot do it

**Date:** 2026-08-24. **Verdict: TSO is not implementable on AWS ENA.** The device
does not advertise the capability, on Graviton3 or Graviton4, and the protocol has
no way to use it unadvertised. This document records the disproof, the evidence
that it is the *device* and not a driver limitation, the TCP-side design that was
worked out before the disproof landed (so that it need not be re-derived if a
future ENA generation changes the answer), and — because the question "what is the
biggest remaining transmit lever" still needs an answer — a measured ranking of
what is actually left.

Nothing here was implemented. The finding arrived before the first line of code,
which is the cheapest possible place for it to arrive.

---

## 0. Verdict

1. **ENA does not support hardware TSO.** Confirmed on three instances spanning
   two Graviton generations and two kernel/driver versions. Linux — which has a
   mature ENA driver and would use TSO if it were there — reports it permanently
   unavailable and falls back to software segmentation (GSO).
2. **It is the device, not the driver.** The Linux ENA driver gates `NETIF_F_TSO`
   directly on the device's advertised `ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_IPV4`
   bit. `ethtool` reporting `[fixed]` is precisely the report that the bit is
   clear. The gate is quoted in §2.
3. **The premise that TSO attacks both cost terms was wrong**, independently of
   whether the device supports it. TCP's segmentation is already zero-copy
   (`BufferQueue::Get()` uses `append_cloned`, not a copy), so a super-buffer
   saves no per-byte work. TSO attacks the per-frame term only. Even a perfect
   implementation on a device that supported it would have been a single-digit win
   at jumbo MTU. §4. Note also that the per-frame/per-byte split this project has
   been quoting is a *receive* measurement — transmit has never been decomposed,
   and an earlier draft of this document wrongly applied one to the other. §4.4.
4. **What ENA *does* offer on transmit is partial (pseudo-header) checksum
   offload** ~~, on both IPv4 and IPv6. That is bits 1 and 3 of the offload
   descriptor,~~ **— IPv4 ONLY.** Not the "full checksum" bits either — a
   distinction that changes how the concurrent checksum-offload work must be
   written. §5.

   > **CORRECTED 2026-08-24: the IPv6 half of this claim is wrong.** It was inferred
   > from Linux `ethtool` on a newer kernel. A **first-party admin-queue read** of
   > `GET_FEATURE(OFFLOAD)` on the instance under test says:
   >
   > ```
   > ena: offload: tx 0x3 (ipv4 l3 csum 1, ipv4 l4 csum part 1 full 0,
   >                       ipv6 l4 csum part 0 full 0, tso v4 0 v6 0),
   >      rx supported 0x7, rx enabled 0x0
   > ```
   >
   > **`tx 0x3` is bits 0 and 1 only: IPv4 L3 + IPv4 L4 *partial*.** The IPv6 L4 bit
   > is **clear**. So there is no IPv6 transmit checksum offload on this device, no
   > `_FULL` of any kind, and no TSO. Anywhere this document treats IPv6 offload as
   > available — or as "one bit away" — it is wrong. A device's own capability word
   > beats an inference from another OS's tooling.
5. **The biggest remaining transmit levers are per-byte, and none of them is
   TSO.** ~~Measured on Neoverse V1: Haiku's checksum loop runs at 0.229 ns/byte
   where a 64-bit unrolled version does 0.095 — 2.4× — worth ~7% of the per-byte
   term.~~ Ranking in §6.

   > **Superseded 2026-08-24.** Both numbers moved and the "~7%" should never have
   > been written. `compute_checksum()` is **merged** at **4.15× at 1988 bytes** —
   > and 1988 is the size that matters, because `checksum_data()` gets one
   > `data_node` at a time out of 2048-byte buffers. **The "~7% of the per-byte
   > term" is deleted, not corrected: that term is the *receive* fit, and this is a
   > transmit change.** See §4.4, and the correction box in §6.1.

---

## 1. What was measured, and the control

Three EC2 instances, all Amazon Linux 2023 arm64, all reading the same ENA device
through `ethtool -k`:

| instance | core | kernel | ena driver | `tx-tcp-segmentation` | `tx-tcp6-segmentation` |
|---|---|---|---|---|---|
| `c7g.metal` (existing builder) | Neoverse V1 | 6.x amzn2023 | (in-tree) | `off [fixed]` | `off [fixed]` |
| `c7g.large` (mine, terminated) | Neoverse V1 `0xd40` | 6.18.41 | 2.17.2g | `off [fixed]` | `off [fixed]` |
| `c8g.large` (mine, terminated) | Neoverse V2 `0xd4f` | 6.18.41 | 2.17.2g | `off [fixed]` | `off [fixed]` |

Also `off [fixed]` on all three: `tx-tcp-ecn-segmentation`,
`tx-tcp-mangleid-segmentation`, `tx-udp-segmentation`, `large-receive-offload`.
And `generic-segmentation-offload: on` — Linux's *software* segmentation, which is
the fallback that exists precisely because hardware TSO is absent.

**The negative control.** The obvious failure mode of this measurement is that
`ethtool -k` or the driver reports everything as unavailable, in which case the
result is meaningless. It does not: through the same command, the same driver and
the same gate, `tx-checksum-ipv4` reads **`on`** and is **changeable**, and on the
two newer kernels `tx-checksum-ipv6` reads `on` as well. `receive-hashing` reads
`on`. So the mechanism demonstrably can and does report capabilities as present.
The absence of TSO is a fact about the device, not an artefact of the instrument.

An attempt to force it — `ethtool -K <if> tso on` — left the state unchanged at
`off [fixed]`, which is what `[fixed]` means: the feature is absent from
`netdev->hw_features` and is not merely switched off.

---

## 2. It is the device, not the driver

`ethtool` marks a feature `[fixed]` when it is absent from `netdev->hw_features`.
For ENA, `hw_features` is built from the device's advertised offload bits and
nothing else. From `amzn-drivers` `kernel/linux/ena/ena_netdev.c`,
`ena_set_dev_offloads()`:

```c
	if (feat->offload.tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_IPV4_MASK)
		dev_features |= NETIF_F_TSO;

	if (feat->offload.tx & ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_IPV6_MASK)
		dev_features |= NETIF_F_TSO6;
	...
	netdev->features = dev_features | NETIF_F_SG | NETIF_F_RXHASH | NETIF_F_HIGHDMA;
	...
	netdev->hw_features |= netdev->features;
```

`feat->offload` is the response to `ENA_ADMIN_STATELESS_OFFLOAD_CONFIG`, fetched
unconditionally by `ena_com_get_dev_attr_feat()` — the same admin call our own
vendored ena-com makes at `ena-com/ena_com.c:2385-2391`. So:

> `tx-tcp-segmentation: off [fixed]` ⟺ the device left
> `ENA_ADMIN_FEATURE_OFFLOAD_DESC_TSO_IPV4_MASK` (bit 5) clear in its offload
> descriptor.

There is no path around this. `ena_com_prepare_tx()` will happily set
`ENA_ETH_IO_TX_DESC_TSO_EN` on an unadvertised device — it validates `header_len`
against the queue limit and nothing else — but the device is entitled to reject or
mis-segment the descriptor, and using an unadvertised capability is exactly the
kind of thing that produces the rare-corruption failure mode this project has been
told to avoid. The bit is not advertised; it is not available.

Worth recording for anyone who reads the vendored HAL and concludes otherwise: the
ena-com layer *does* carry full TSO support — `ena_com_tx_ctx::tso_enable`
(`ena-com/ena_eth_com.h:63`), `ena_com_tx_meta::{mss,l3_hdr_len,l3_hdr_offset,l4_hdr_len}`
(`ena-com/ena_com.h:108-113`), the TSO_IPV4/IPV6/ECN feature bits
(`ena-com/ena_defs/ena_admin_defs.h:1367-1372`). The presence of the plumbing in a
vendor HAL that is shared across many devices and generations says nothing about
this device. That is the trap; the offload descriptor is the answer.

---

## 3. What the device *would* have required

Established from the vendored code, in case this becomes relevant again. None of
it was exercised.

- **`meta_valid` is the master switch.** `ena_com_prepare_tx()` gates the entire
  offload block (`ena-com/ena_eth_com.c:529-547`) on it. With `meta_valid == 0`,
  `tso_enable`, `l3_proto`, `l4_proto` and every checksum bit are silently
  dropped. This is the first thing an implementation would get wrong.
- **`mss` is 14 bits**, split lo/hi across two descriptor words
  (`ena_eth_io_defs.h:348-349, 370-371`) — maximum 16383.
  `l3_hdr_len` and `l3_hdr_offset` are 8 bits each; **`l4_hdr_len` is 6 bits and
  is in *words*, not bytes** — a units bug waiting to happen.
  `ena_com_create_meta()` masks these silently; it does not validate.
- **The header-length ceiling is 224, not 96.** The boot log reports
  `max_tx_header 96` from `ena_admin_queue_ext_feature_fields::max_tx_header_size`,
  and then `ena_com_config_dev_mode()` **overwrites** `ena_dev->tx_max_header_size`
  with the LLQ-derived `desc_list_entry_size - descs_num_before_header * 16`
  = 256 − 32 = 224 (`ena-com/ena_com.c:3541-3542`). That 224 is what
  `ena_com_prepare_tx()` actually enforces. 224 accommodates IPv4 with maximal
  options (134) and IPv6 without extension headers (114); 96 would not have.
  This is worth knowing regardless of TSO — the number in the boot log is
  discarded, and reading it as a live constraint is a mistake.
- **A 64 KiB super-buffer would not have fitted the current driver.** The
  effective transmit descriptor cap is **8**, not the 17 the boot log reports.
  The chain is: the device reports `max_packet_tx_descs = 17`
  (`ena.cpp:706`); the driver reserves one for the meta descriptor
  `ena_com_prepare_tx()` may emit, giving 16 (`ena.cpp:721-722`); then
  `min_c(ENA_MAX_PACKET_DESCRIPTORS, 16)` clamps it to 8 (`ena.cpp:726`,
  `ena.h:131`). At `ENA_PACKET_BUFFER_SIZE = 1920` bytes per bounce slot
  (`ena.h:112`) that is **8 × 1920 = 15360 bytes maximum per frame**. A 64 KiB
  super-buffer needs 35 slots. Even lifting `ENA_MAX_PACKET_DESCRIPTORS` to the
  device's 16 only reaches 30720.
  Real scatter-gather is impossible today because `get_memory_map` is a `NULL`
  entry in `net_buffer_module_info` (`stack/net_buffer.cpp:2394`) — there is no
  interface that yields physical addresses for a `net_buffer`. Even with it, 16
  data descriptors × 4 KiB pages caps a page-aligned super-buffer at 64 KiB and a
  misaligned one at less.
- **The device does not report a maximum TSO size.** `grep -rn "max_tso"` over the
  whole driver tree returns nothing; `ena_freebsd_2.8.4`'s admin defs have no such
  field. Upstream FreeBSD hardcodes it. So the limit would have had to be chosen,
  not queried — and IPv4's `total_length` is a `uint16`, so 65535 including
  headers is the hard ceiling anyway (`ipv4.cpp:1579`).

---

## 4. The TCP-side design, and why it was smaller than it looked

Worked out before the disproof. Recorded because most of it turned out to be
*good news about Haiku's TCP* that a future reader should not have to rediscover,
and because two of the three things the brief expected to be hard are not.

### 4.1 Where segmentation happens

`TCPEndpoint::_SendQueued()`, `TCPEndpoint.cpp:2601-2656` — a
`do { … } while (length > 0)` loop that per MSS does:

```
segmentMaxSize = fSendMaxSegmentSize - tcp_options_length(segment)   :2602
gBufferModule->create(256)                                          :2623
fSendQueue.Get(buffer, fSendNext, segmentLength)                     :2629
_PrepareAndSend(...) -> add_tcp_header() -> send_routed_data()        :2637
```

Deferring it means taking `min(available, sendWindow, tsoMax)` in one pass and
emitting a single buffer with one header. That part is genuinely small.

### 4.2 Congestion control and retransmission are already correct — for free

This is the part the brief expected to be hard, and it is not, for one structural
reason: **Haiku's TCP is byte-accounted, not packet-accounted, and its send queue
is a byte stream rather than a list of segments.**

- **Congestion window.** `_SendQueued()` computes `sendWindow` from
  `min(fSendWindow, fCongestionWindow)` less `consumedWindow`, and caps `length`
  by it (`:2557-2583`). `flightSize` is `(fSendMax - fSendUnacknowledged)`. All
  byte quantities. A super-buffer that advances `fSendNext` by its whole length is
  accounted correctly by construction. Nothing to change.
- **Retransmission at segment granularity.** `_Retransmit()` sets
  `fSendNext = fSendUnacknowledged` and calls `_SendQueued()` (`:2830-2836`).
  `BufferQueue` is indexed by sequence number over a byte stream, so the
  retransmit re-segments from bytes with no memory of how the data was originally
  packetised. **There is nothing to re-segment**; the stack cannot help but do the
  right thing. The `retransmit` path also `break`s after one segment (`:2653`), so
  retransmits would naturally not use TSO.
- **Loss recovery disables it automatically.** When `fDuplicateAcknowledgeCount
  != 0`, `length` is clamped to one SMSS (`:2596-2599`), so a super-buffer cannot
  be built during fast recovery.

The one real accounting bug would have been `--fSendMaxSegments` at `:2428`,
decremented once per `_PrepareAndSend` — a super-buffer must decrement by its
segment count. It is only the initial-window burst limiter (set at `:1747`, then
`UINT32_MAX` after the first advancing ack at `:2763`) and the byte-wise
`sendWindow` cap already bounds the same thing, so it is a one-line fix to a
redundant guard.

### 4.3 Where a super-buffer actually dies today

Not in TCP. In three size checks below it:

| site | behaviour |
|---|---|
| `ipv4.cpp:1626-1631` | `size > mtu` → `EMSGSIZE`, because `_PrepareSendPath()` sets `IP_DONTFRAG` on every TCP endpoint (`TCPEndpoint.cpp:2703-2704`). Without `IP_DONTFRAG` it would IP-fragment instead, which is worse. |
| `ipv4.cpp:1579` | `size > 0xffff` → `EMSGSIZE`; `total_length` at `:1541` is a `uint16`. Absolute ceiling. |
| `ethernet.cpp:291` | `size > device->frame_size` → `B_BAD_VALUE`, checked *before* the driver sees it. |

Plus `ena.cpp:2317` / `:2325`. Each needs a "this is a TSO buffer" flag to bypass.
`net_buffer::buffer_flags` is a `uint16` with only two bits used
(`net_buffer.h:19-22, 44`), so 14 bits are free — no struct growth needed for the
flag. The MSS itself has nowhere to go: the 4-byte anonymous union at
`net_buffer.h:31-40` is already occupied by `sequence` on exactly the buffers in
question (TCP's send queue writes it, `BufferQueue.cpp:104`). That is a real
design problem, and the honest answer is a field on `net_buffer_private` plus an
accessor added to `net_buffer_module_info` — the unused `associate_data` slot
(`net_buffer.h:78`, `NULL` at `net_buffer.cpp:2375`) is the only free slot of the
right shape.

Note in passing: `split_buffer` does **not** go through `copy_metadata()`
(`net_buffer.cpp:1069-1085`), so split fragments do not inherit `buffer_flags`.
Any future offload flag must audit that path.

### 4.4 Why the win would have been small anyway

`BufferQueue::Get()` clones — `append_cloned`, `BufferQueue.cpp:324` — it does not
copy. So collapsing N segments into one buffer removes N−1 `net_buffer`
allocations, header constructions, route traversals, ioctls and doorbells, and
**zero bytes of copying**. TSO is a per-frame optimisation, full stop. The
per-byte checksum walk and the driver's bounce copy both survive it untouched.

**A caveat on the sizing that must be stated first, because it invalidates the
usual shortcut.** The 2.34 µs/frame + 1.85 ns/byte model comes from
`net-receive-profile.md`, which is a **receive** measurement. **There is no
per-frame/per-byte decomposition of the transmit path anywhere in this tree.**
Applying the receive split to transmit — which the brief for this project did, and
which an earlier draft of this document repeated — is not justified: transmit pays
a full checksum walk that receive skips when the device validates
(`tcp.cpp:718-719`), and receive pays a `read()`-side copy that the profile
measured at 46% of its cost. The two paths should not be assumed to share a shape.

Used only as an order-of-magnitude sanity check, and flagged as such:

| MTU | frames/MiB | per-frame share *if the receive split held* | ceiling if *all* per-frame cost vanished |
|---|---|---|---|
| 9001 | 117 | 12% | 2211 → 1938 µs/MiB, **−12%** |
| 1500 | 724 | 46% | 3635 → 1941 µs/MiB, **−47%** |

The receive model does happen to reproduce the measured transmit 2182 µs/MiB at
MTU 9001 to within 1.3% — which is interesting and is *not* evidence, since a
two-parameter fit landing on one point is nearly free. **The predicted win, stated
before measuring as the house standard requires, was 5–8% at MTU 9001** (TSO
removes the stack's share of the per-frame cost, not the driver's or the
interrupt's) **and 25–35% at MTU 1500.** Jumbo frames are already working and
hardware-verified here, so the project would have been optimising the
configuration we do not run.

The structural argument in the paragraph above — that `append_cloned` means TSO
removes zero bytes of copying, so it cannot touch the per-byte term at all — does
not depend on the disputed split, and is what actually decides the question.

---

## 5. What ENA does offer on transmit — and what that means for the concurrent work

The offload descriptor bits that read as *present*, from the same `ethtool`
evidence and the same driver gate:

| ethtool feature | driver gate | ENA offload bit |
|---|---|---|
| `tx-checksum-ipv4: on` | `NETIF_F_IP_CSUM` | bit 1, `TX_L4_IPV4_CSUM_**PART**` |
| `tx-checksum-ipv6: on` (newer kernels) | `NETIF_F_IPV6_CSUM` | bit 3, `TX_L4_IPV6_CSUM_**PART**` |
| `rx-checksumming: on` | `NETIF_F_RXCSUM` | `RX_L4_IPV4_CSUM` / `RX_L4_IPV6_CSUM` |
| `receive-hashing: on` | `NETIF_F_RXHASH` | RSS |

Three consequences that the checksum-offload work on `feat/ena-tx-offload` needs,
and which cost nothing to learn from here:

1. **It is the *partial* bit, not the full one.** The Linux driver maps
   `NETIF_F_IP_CSUM` from `..._CSUM_PART_MASK` (bit 1); the `..._CSUM_FULL_MASK`
   bits (2 and 4) are not what is being advertised. So the contract is: TCP writes
   the **pseudo-header partial** into the checksum field and the device completes
   it, selected by `ena_com_tx_ctx::l4_csum_partial = 1`. It is *not* "leave the
   field zero and the device does everything".
2. **`meta_valid` must be set to 1**, or `ena_com_prepare_tx()` drops
   `l4_csum_enable`/`l4_csum_partial` on the floor (`ena_eth_com.c:529-547`).
   `ena_send()` currently `memset`s the context to zero (`ena.cpp:2401-2403`) and
   never sets it.
3. **`NET_BUFFER_L4_CHECKSUM_VALID` cannot be reused for this.** Today
   `add_tcp_header()` sets it on transmit to mean "software already computed the
   full checksum" (`tcp.cpp:436-438`), while the receive path reads it as "do not
   verify" (`tcp.cpp:718-719`). A `_NEEDED`/partial flag must be a distinct bit
   with distinct semantics, which is what that branch is already doing — this
   confirms the approach and pins down which ENA mode it must target.

Also relevant to them: `ena_com_tx_ctx::df` is written **unconditionally**, outside
the `meta_valid` block (`ena_eth_com.c:520-522`), and `ena_send()` always leaves it
0 regardless of the IPv4 DF bit — which TCP always sets. Harmless while no offload
is active; not harmless once the device starts interpreting the descriptor.

---

## 6. What the remaining transmit levers actually are

Since the assigned lever does not exist, here is the measured replacement ranking.
All per-byte — not because the 88% figure applies to transmit (it does not; see
§4.4) but because each of these removes or shrinks a whole pass over every byte,
and because the only transmit cost actually measured here is per-byte.

### 6.1 ~~Haiku's checksum loop is 2.4× slower than it needs to be~~ — FIXED, 4.15×, merged

> **MERGED 2026-08-24** as `e63fe3f24a` "net: make the internet checksum 4x faster,
> and stop it overflowing" (via `63e1ca7b15`). The measured speed-up is **4.15× at
> 1988 bytes**, not 2.4×.
>
> **Why the size matters more than the ratio, and this is the transferable lesson:**
> `checksum_data()` is handed **one `data_node` at a time**, out of 2048-byte
> buffers — so **1988 bytes is the length to optimise**. Tuning for "one 8949-byte
> jumbo frame", which is the intuitive choice at MTU 9001, would have optimised a
> size the routine never sees.

`compute_checksum()` (`stack/utility.cpp:101-131`) is a 16-bit-at-a-time
accumulate loop that still carries its original comments:

```c
	// TODO: unfold loop for speed
	// TODO: write processor dependent version for speed
	while (length >= 2) { sum += *buffer++; length -= 2; }
```

It is on the transmit path unconditionally: `add_tcp_header()` →
`Checksum::PseudoHeader()` (`NetUtilities.h:85-95`) →
`gBufferModule->checksum(buffer, 0, buffer->size, false)` — a full payload walk per
segment, every segment, with no offload check anywhere.

Benchmarked against a 64-bit-accumulator, 4×-unrolled equivalent producing
bit-identical results, gcc 11.5 `-O2`, on **Neoverse V1 (`0xd40`, c7g.large)** and
on Neoverse V2 (`0xd4f`, c8g.large). Interleaved A/B, 5 rounds, 100 000 iterations
per round, best round reported:

| payload | current | 64-bit unrolled | speedup |
|---|---|---|---|
| 1448 B (MTU 1500 MSS) | 0.2256 ns/B | 0.0964 ns/B | 2.34× |
| 8949 B (MTU 9001 MSS) | 0.2291 ns/B | 0.0948 ns/B | 2.42× |
| 65495 B | 0.2319 ns/B | 0.0946 ns/B | 2.45× |

The per-byte rate is flat across a 45× size range in both variants, which is the
built-in control: the result is the loop, not a cache-residency artefact. Checksums
matched at every size.

**Saving: 0.134 ns/byte** ~~, or ~7% of the 1.85 ns/byte per-byte term~~ — ~~the same
order as the arm64 `memcpy` fix already proven here (6.4%)~~ — and the same root
cause: a generic C loop where the architecture wanted a real implementation.

> **Two corrections, 2026-08-24.**
>
> 1. **The percentage is deleted, not adjusted. `1.85 ns/byte` is the RECEIVE fit,
>    and this is a TRANSMIT change.** There is **no per-frame/per-byte decomposition
>    of the transmit path anywhere in this tree** — which §4.4 of *this document*
>    states explicitly and forbids. Expressing a transmit saving as a fraction of a
>    receive term is the exact error §4.4 was written to stop, committed here in §6.
>    Leaving it in place is how it spread to other documents.
> 2. **The comparison figure is stale twice over.** `compute_checksum()` is now
>    **merged** and the measured speed-up is **4.15× at 1988 bytes** (not 2.4×), and
>    **1988 is the size that matters** because `checksum_data()` receives one
>    `data_node` at a time from 2048-byte buffers — sizing the optimisation for "one
>    8949-byte frame" would have tuned the wrong length. The `memcpy` figure is
>    **8.4%**, not 6.4%.
Caveat, stated because it will move the number: this is a warm-cache userspace
measurement on contiguous memory. In the kernel the walk traverses a `data_node`
chain (`net_buffer.cpp:2207-2253`) with per-node overhead, and the data is
subsequently re-read by the driver's bounce copy, so it is cache-warm for that.
The kernel number will be worse for both variants; the ratio should survive.

### 6.2 Ranking

| lever | term | status |
|---|---|---|
| **Zero-copy transmit** — implement `get_memory_map` in the buffer module, real scatter-gather in `ena_send()`, retire the 1920-byte bounce slots | per-byte, removes a whole copy | **held** — largest single item, but it touches `ena.cpp` and the transmit path where two branches are live; sequenced behind them. Design notes in §6.4 |
| **TX checksum offload** — removes the 0.229 ns/B walk entirely for ENA | per-byte. ~~~12%~~ **−3.54%, p = 0.0079** | **MERGED** `c2753e0030` (2026-08-24). The ~12% traced to an earlier **−12.6%** figure that is **withdrawn as a confound** (unpinned send buffer) |
| **Optimise `compute_checksum()`** — ~~2.4× measured, ~7%~~ **4.15× at 1988 bytes** | per-byte | **MERGED** `63e1ca7b15` (2026-08-24), *not* unclaimed. Still correctly *not* redundant with offload: it also serves loopback, other drivers, and any path where offload is unavailable |
| **TX doorbell coalescing / batched transmit entry point** | per-frame | **DEAD** — `219d8ab858`. LLQ grants 2 burst entries, one jumbo frame consumes both, **99.94% of frames already leave the allowance at zero**; ratio 1:1, saving exactly zero at MTU 9001. The batched transmit entry point is not coming |
| ~~TSO~~ | per-frame, ~~≤12% at MTU 9001~~ (that bound came from the **receive** split and should never have been applied here — §4.4) | **IMPOSSIBLE — the device does not advertise it.** `tso v4 0 v6 0` |

Note that the first three are all per-byte and the two offload items are partly
redundant with each other: for ENA IPv4/IPv6 traffic, checksum offload makes §6.1
moot. They are not redundant in general.

### 6.3 Ideas considered and rejected

- **Software segmentation (a GSO equivalent): rejected.** Deferring segmentation
  to the device layer without hardware support still requires N headers, N
  `net_buffer`s and N checksums; late segmentation moves that work rather than
  removing it. The only part it would amortise is the ioctl and the doorbell,
  which the batched transmit entry point on `feat/ena-tx-offload` captures
  directly and more cheaply. Building a GSO layer to reach the same place would be
  strictly more code for strictly less.
- **Using `tso_enable` on an unadvertised device: rejected.** `prepare_tx` would
  accept it. The device is not obliged to, and the failure mode — rare corruption
  or a stalled connection — is the one this project has least ability to detect.
- **Waiting for a newer Graviton: no evidence for it.** Graviton4 / Neoverse V2
  with ENA driver 2.17.2g answers exactly as Graviton3 does.

### 6.4 Zero-copy transmit — design notes, held not started

~~Held deliberately: it touches `ena.cpp` and the transmit path where
`feat/ena-tx-offload` and `feat/ena-multiqueue` are both live, and it is not worth
a three-way conflict.~~

> **Reason for the hold is GONE, 2026-08-24 — this is unblocked.** `feat/ena-tx-offload`
> is **merged** (`c2753e0030`) and `feat/ena-multiqueue` is **closed/cancelled** (see
> `ena-multiqueue-headroom.md` §5). There is no three-way conflict left to avoid.
> Zero-copy transmit is genuinely still open — note the known obstacle recorded
> elsewhere in this tree: **`get_memory_map` returns NULL**, which is the first thing
> the design below has to solve.

Recorded here so the survey is not lost.

**The cost being removed.** Every transmitted byte is copied once, in
`ena_send()`:

```c
	if (sBufferModule->read(buffer, copied, slot->data, chunk) != B_OK)
```

— `ena.cpp:2385-2400`, looping over `segments` bounce slots of
`ENA_PACKET_BUFFER_SIZE = 1920` bytes each, then handing the device the *slots'*
physical addresses. That is a full payload `memcpy` per frame, on the per-byte
term, in addition to the checksum walk of §6.1 and the user→kernel copy in
`SendData()`. The driver already documents why (`ena.cpp:2378-2382`):

> …no way to get a physical address through any interface the buffer module offers
> — `get_memory_map` is a `NULL` entry in `net_buffer_module_info` — so the frame
> is copied.

**The blocker is one unimplemented slot in the stack, not the driver.**
`net_buffer_module_info::get_memory_map` is declared (`net_buffer.h`) and wired to
`NULL` (`net_buffer.cpp:2394`). Nothing in the tree implements it. So the work is
in two halves, and only the second half is contended:

1. *Stack side (uncontended).* Implement `get_memory_map` over the `data_node`
   chain: for each node, translate its virtual range to physical extents,
   splitting at page boundaries. The awkward part is that `net_buffer` payloads
   live in a slab-allocated data area, so a node's bytes are contiguous virtually
   but need `get_memory_map()`-style physical translation per page, and cloned
   nodes (`append_cloned`, which is how TCP's send path builds every buffer —
   `BufferQueue.cpp:324`) alias another buffer's pages. Aliasing is fine for DMA
   *reads*; it does mean the buffer must not be freed until the device is done,
   which the existing `entry->buffer` retention in `ena_send()` already provides.
2. *Driver side (contended).* Replace the bounce loop with the returned extent
   list. Two hard constraints fall out of the descriptor arithmetic in §3:
   - **The descriptor cap is 8**, so a zero-copy frame may use at most 8 physical
     extents. A 9001-byte jumbo frame spanning arbitrary page boundaries can
     easily need more than 8 extents once TCP has cloned it out of several queue
     buffers — so a *hybrid* is required, not a straight replacement: use the
     extent list when it fits in 8, fall back to bouncing when it does not.
     Raising `ENA_MAX_PACKET_DESCRIPTORS` to the device's 16 (`ena.cpp:721-726`)
     widens the fast path considerably and is probably a precondition.
   - **LLQ mode still needs a contiguous header** (`ena.cpp:2415-2435`,
     `context.push_header`), which today is taken from bounce slot 0. Zero-copy
     has to either keep a small header-only bounce slot or require the first
     extent to cover `tx_max_header_size` (224 — see §3) contiguously.

**Expected win, unmeasured.** One full payload copy. Using the memcpy figures
already established here — 0.056 ns/byte for a good arm64 `memcpy`, against the
0.229 ns/byte the checksum loop costs for a comparable single pass — the copy is
plausibly 0.05–0.15 ns/byte of the per-byte term, i.e. the same order as §6.1 but
probably smaller than checksum offload. That is a guess from the wrong end; the
honest position is that **the transmit per-byte term has never been decomposed**
(see the note in §4.4 about `net-receive-profile.md` being a *receive*
measurement), and decomposing it should come before choosing between these levers.
A transmit equivalent of `netprof`'s per-thread partition is the missing
instrument.

---

## 7. What would change this verdict

One thing only: an ENA device that sets bit 5 (`TSO_IPV4`) or bit 6 (`TSO_IPV6`)
of the `ENA_ADMIN_STATELESS_OFFLOAD_CONFIG` descriptor. The cheap way to keep
watching for it, on any Linux instance:

```
ethtool -k $(ip -o -4 route show default | awk '{print $5}') | grep tx-tcp-segmentation
```

`off [fixed]` means the bit is clear. Anything else means this document is stale
and §3 and §4 are the head start.

A second, less likely path: the driver could read `features.offload` itself and
log it, giving a first-party read of the bits rather than one inferred through
Linux. `ena.cpp` fetches the descriptor already — `ena_com_get_dev_attr_feat()`
fills `features.offload` — and then discards it, unread. Logging that word during
bringup would cost two lines and would let the checksum-offload work assert its
own preconditions instead of trusting this document. Recommended regardless of
TSO, but it belongs to whoever owns `ena.cpp`.
