# The internet checksum on arm64, and what receive offload is really doing

**Date:** 2026-08-24. **Hardware:** `c7g.large` (Neoverse V1, `0xd40`) and
`c8g.large` (Neoverse V2, `0xd4f`), `us-west-2`, canonical AMI
`ami-0d61e3910062bb80a`, DeBeOS `hrev59996`, MTU 9001, peer `c7g.metal`
`10.42.0.149`.

Three results, in decreasing order of how well established they are.

1. **`compute_checksum()` is 4.15× faster and no longer overflows.** Verified
   bit-identical against two oracles over 3,539,802 cases on hardware. §1–§3.
2. **The driver was telling the stack the device had verified the IPv4 header
   checksum when nothing had.** Fixed; hardware-verified. §4.
3. **Receive checksum offload was never disabled** — the reading that said it was
   is a field the device does not maintain. There is nothing to enable, and the
   receive path already computes no TCP checksum at all. §5.

And one honest failure: **the end-to-end µs/MiB effect of (1) could not be
resolved on `c7g.large`**, because transmit cost is bimodal *per boot* with a
±19% spread and a module A/B needs a reboot. §6. That measurement is owed on
`c7g.4xlarge`.

---

## 1. What was wrong with the routine

`src/add-ons/kernel/network/stack/utility.cpp`, unchanged since 2006, complete
with its own unfinished intentions:

```c
	// TODO: unfold loop for speed
	// TODO: write processor dependent version for speed
	while (length >= 2) {
		sum += *buffer++;
		length -= 2;
	}
```

One 16-bit load and one add per two bytes, into a `uint32`. It is on the transmit
path unconditionally — `add_tcp_header()` → `Checksum::PseudoHeader()` →
`checksum_data()` → here — so every transmitted byte passes through it, and on
arm64, which also had no optimised `memcpy` until recently, that is a measurable
share of the per-byte cost.

It also had a latent overflow. Adding 16-bit words into a `uint32` wraps after
131076 bytes. No caller can reach that — `data_node::used` is a `uint16`, so the
per-call ceiling is 65535 and the typical call is ~2000 bytes, carved from
`BUFFER_SIZE = 2048`. But `checksum_data()`'s own accumulator was luckier still by
a smaller margin: it adds one folded 16-bit value per node into a `uint32`, so
65537 single-byte nodes overflow it, and the only thing preventing that is a
`buffer->size > 0xffff` check in `ipv4.cpp:1579` — a cap enforced one layer away,
which any future aggregation (LRO, a software segmentation path) would lift
without knowing this loop depended on it. Both accumulators are 64-bit now.

## 2. The replacement, and the bug the tests caught

Sum the 32-bit halves of 64-bit loads into a 64-bit accumulator; fold once at the
end. Equivalent because one's complement addition is associative and the fold only
ever adds 16-bit lanes together — so lane order and accumulator width are both
free, which is also why the same expression is correct on either endianness.

The implementation moved to a new `checksum.h`, dependency-free, for two reasons:
the inner loop now inlines into `checksum_data()` instead of being a call per
node, and a userspace test can compile **the same source that ships**.
`utility.cpp` keeps `compute_checksum()` and `checksum()` as forwarders, since
`checksum()` is exported through `net_stack_module_info` (`net_stack.h:144`) and
`ipv4.cpp:647` calls it on a raw header.

**An earlier draft was wrong, and only the carry-heavy test patterns found it.**
It read:

```c
	sum += (uint32)a + (uint32)(a >> 32);		/* WRONG */
```

Both operands are `uint32`, so C evaluates that addition in 32-bit arithmetic and
discards the carry out of it *before* widening to the accumulator. It agreed with
both oracles on counting patterns, on random data, and on zeros. It failed only on
`0xff` fill — where every word is `0xffff` and every addition carries — losing
exactly one per 8 bytes. Two adds into the `uint64` cannot do this:

```c
	sum += (uint32)a;
	sum += (uint32)(a >> 32);
```

This is the bug an internet checksum is most likely to have, it is invisible on
ordinary data, and it would have shown up in production as rare corruption on
whichever packets happened to carry carry-heavy payloads. Writing it is why the
test came before the measurement.

## 3. Correctness evidence

`src/tests/add-ons/kernel/network/checksum/`. It `#include`s the shipping
`checksum.h` directly. That is deliberate: a sibling effort on this tree lost its
entire evidentiary basis to a harness that benchmarked a *copy* differing from the
shipping code in exactly the interesting place, and the file says so in a comment
so nobody "simplifies" it back.

**3,539,802 checks, 0 failures** on Neoverse V1:

| test | what it covers |
|---|---|
| exhaustive | every length 0..1024 × every alignment 0..31 × 6 content patterns |
| unroll boundaries | lengths ±3 around every multiple of 8 out to 70000 bytes, 9 alignments |
| node composition | the `checksum_data()` contract: per-node results summed with `__swap_int16` on odd-offset nodes, checked at **every** 2-node split point and a grid of 3-node splits, against the whole-buffer answer |
| RFC 1071 | the worked example from §3 of the RFC as a fixed known answer, so a self-consistent change of convention cannot pass |
| past overflow | lengths 131000..131200 and 200000, where the old routine is the one that is wrong |

Two independent oracles: a byte-at-a-time reference written from the contract (no
casts, no wide loads, no unrolling — structurally shares nothing with the routine
under test), and a verbatim copy of the implementation being replaced.

The content patterns are `zero`, `ones`, `alternating`, `highbit`, `counting`,
`random`. `ones` and `highbit` exist solely to force carry propagation, and they
are the ones that earned their keep.

**The composition test is the one that protects the contract.** `checksum_data()`
depends on three representation details — host byte order, the odd trailing byte
being the *first* byte of a word, and the result being folded but **not**
complemented — and a change to any of them passes every other test in the table
while breaking every TCP, UDP and ICMPv6 checksum in both directions at once.
Those three are now written down at the top of `checksum.h`.

### 3.1 Caller audit

Every user of the routine was established before it was touched.

- **`compute_checksum()` has three callers**, all in the stack: `net_buffer.cpp:2231`
  and `:2233`, and `simple_net_buffer.cpp:614` — the last of which is commented out
  of the build (`Jamfile:23`) but must stay semantically identical to
  `net_buffer.cpp` or a future re-enable breaks silently. The forwarder keeps it
  working untouched.
- **`checksum()` has one caller**: `ipv4.cpp:647`, per-fragment header checksum on
  a raw `ipv4_header*`, ≤60 bytes.
- **Two call sites depend on the non-complemented form** (`finalize == false`):
  `NetUtilities.h:61` and `ipv6.cpp:1336`. Both complement themselves.
- **Incremental / RFC 1624 checksum update: none anywhere.** There is no NAT or
  packet-rewriting layer in this tree, so no caller depends on adjusting an
  existing checksum. `ipv4.cpp:648` leaves a TODO wishing for it.
- **Nothing depends on an unfolded sum crossing an API boundary.** `Checksum::fSum`
  is private; `checksum_data()` folds in both branches.

Left alone deliberately, and worth knowing about:

- `ipv6_utils.h:21-33` `compute_wordsum()` is a **second, independent copy** of the
  word loop, returning a raw unfolded `uint32` with no odd-byte handling. It is
  only ever called on 16- and 2-byte inputs from `ipv6.cpp:1338`, so optimising it
  would be measuring nothing.
- `Checksum::operator<<(uint8)` (`NetUtilities.h:36-44`) is **parity-blind**: it
  hardcodes "this byte is at an even stream position", so two consecutive
  single-byte pushes both land in the low half. Latent — its only user,
  `UnixAddress.cpp:280-291`, never compares the result against a wire checksum.
  Not touched; do not assume `Checksum` is parity-aware.
- `icmp6.cpp:291-292` computes over `buffer` and stores into `reply`. Pre-existing,
  unrelated, flagged so it is not misattributed to this change.
- `checksum_data()` returns `int32` with `B_BAD_VALUE`/`B_ERROR` error paths, and
  `NetUtilities.h:61` adds the result to `fSum` without checking — a bad
  offset/size silently poisons a pseudo-header sum. Pre-existing.
- Five further independent internet-checksum implementations exist outside the
  stack (bootloader `IP.cpp:211`, `ping`, `traceroute`, an ICMP test, and a
  FreeBSD-compat shim whose `in_pseudo()` is a `panic()`). None is affected.

## 4. Speed

Same binary as the correctness tests, so the routine measured is the routine
verified. Interleaved A/B, replaced vs current, 5 rounds, best round reported. The
buffer is mutated every iteration because both routines are pure and the arguments
loop-invariant — without that a compiler may compute either once and the benchmark
measures an empty loop. That mutation is charged to both arms and inflates both
per-byte numbers, so it understates the ratio.

| payload | replaced | current | speedup |
|---|---|---|---|
| 64 B (bare ACK) | 0.2495 | 0.1799 | 1.39× |
| 512 B | 0.2339 | 0.0648 | 3.61× |
| 1448 B (MTU 1500 MSS) | 0.2334 | 0.0562 | 4.15× |
| **1988 B (a real `data_node`)** | **0.2278** | **0.0548** | **4.15×** |
| 8949 B (MTU 9001 MSS) | 0.2307 | 0.0511 | 4.51× |
| 65495 B | 0.2322 | 0.0501 | 4.63× |

ns/byte, alignment 0; alignment 1 differs by under 3% throughout, which is why the
loads go through `memcpy()` — standards-clean *and* free.

**1988 bytes is the number that matters**, not 8949: `checksum_data()` calls this
once per `data_node`, and payload nodes are carved from 2048-byte buffers. So the
operative figure is **0.228 → 0.055 ns/byte, saving 0.173 ns/byte**. The 64-byte
row is dominated by the harness's own per-iteration store; bare ACKs are
negligible in aggregate anyway.

Sanity check that the fast number is physically possible: 0.055 ns/byte on a
2.6 GHz core is ~8 bytes/cycle, i.e. one 64-bit load and two adds per cycle. A
4-wide core with two load ports can retire that. It is not "the compiler deleted
the work".

### 4.1 Verified by artifact, in the kernel

Not just in the test binary. Disassembling `checksum_data` in the cross-built
`stack` module (`net_buffer.o`, 189 instructions):

- `ldp` (paired 64-bit loads) present; 49 `add`; 4 `ldrh` remaining, which is the
  2-byte tail loop.
- **no `bl`** — the inner loop is fully inlined into `checksum_data()`, which is
  the second reason `checksum.h` exists.

And on the running node, `listimage` showed the loaded add-on as
`/boot/home/config/non-packaged/add-ons/kernel/network/stack` with md5
`bbd521b489b1c436fcda0d28d6a16587`, matching the builder. The packaged module was
not loaded.

## 5. What receive offload is actually doing

The starting position was that the device supports all three receive checksum
offloads and the driver enables none: `rx_supported 0x7`, `rx_enabled 0x0`. Both
readings are correct. The conclusion drawn from them is not.

**First-party read**, logged from DeBeOS's own admin queue (the descriptor was
being fetched at bring-up and discarded; it is now kept and logged):

```
KERN: ena: offloads: tx 0x3, rx supported 0x7, rx enabled 0x0
```

**Per-frame descriptor counters**, added to the receive path outside any debug
`ifdef`, over a 256 MiB transfer:

```
KERN: ena: rx offload observed after 1000 frames: l4_csum_checked 983,
      l4_csum_err 0, l3_proto==ipv4 997, l3_csum_err 0
      (rx_supported 0x7, rx_enabled 0x0)
```

**The device is validating L4 checksums on 98% of frames while reporting
`rx_enabled 0x0`.** So:

1. **`rx_enabled` does not describe what the device is doing.** It must not gate
   anything. Reproduced across two boots (983/1000 and 982/1000).
2. **Receive L4 checksum offload is already working**, and the driver already
   consumes it correctly via `l4_csum_checked`. This is why receive computes no TCP
   checksum at all — which the negative control in §6 independently confirms.
3. **There is nothing to enable.** ena-com has `ena_com_get_offload_settings()`
   and **no setter** — no `ENA_ADMIN_STATELESS_OFFLOAD_CONFIG` set command exists
   in the shared HAL, so no driver on any OS enables this. `rx_enabled` appears
   nowhere in the Linux driver. And there is no RX-checksum bit in
   `driver_supported_features`: the available bits are `rx_offset`,
   `interrupt_moderation`, `rx_buf_mirroring`, `rss_configurable_function_key`,
   `rx_page_reuse`, `tx_ipv6_csum_offload`, `phc`. The host_info hazard is real but
   does not apply here.

So the receive per-byte cost this was expected to remove **was already removed**,
before this project started. There is no receive win of that shape available.

One genuine find for the transmit-offload work, from the same log line: **`tx 0x3`
is bits 0 and 1 only** — IPv4 header checksum and IPv4 L4 *partial*. Bit 3
(IPv6 L4 partial) is **clear**, and the reason is almost certainly the host_info
hazard: we do not declare `ENA_ADMIN_HOST_INFO_TX_IPV6_CSUM_OFFLOAD_MASK`, and
Linux does. On a newer Linux kernel that declares it, `ethtool` reports
`tx-checksum-ipv6: on`; on an older one it reports `off [fixed]`. Declaring that
bit is likely all that stands between us and IPv6 transmit checksum offload. Bits
5 and 6 are clear, independently confirming from our own admin queue that the
device does not support TSO.

## 6. The end-to-end measurement, which failed

Predicted before measuring, as the house standard requires: saving 0.173 ns/byte
against a transmit cost of 2182 µs/MiB (= 2.081 ns/byte) is **8.3% at the
microbenchmark's face value, realistically 5–8%** once the kernel's per-node loop
overhead, which the microbenchmark does not have, is paid.

That could not be resolved, for a reason worth recording.

**Transmit cost on `c7g.large` is bimodal, and the mode is chosen at boot.** Every
row below is 8 repetitions on one boot, MTU confirmed at 9001, send buffer pinned
with `-P 256K` so auto-sizing is off:

| boot | stack module | tx µs/MiB (median) | tx Mbit/s | within-boot spread |
|---|---|---|---|---|
| X | packaged (baseline) | **2661** | 3790 | 1.2% |
| Z | mine | 2424, then 2015 mid-run | 4235 → 4960 | shifted |
| Y | packaged (baseline) | **2232** | 4400 | 1.5% |

Two baseline boots, identical configuration, differ by **19%** — more than twice
the effect being looked for. Within a boot the spread is ~1.5%, so this is not
run-to-run noise; it is a state selected at boot and then stable, most likely
thread and interrupt placement on two vCPUs (the receive profile already found
four threads accounting for 99.9% of network CPU).

Since swapping a kernel module requires a reboot, **a module A/B on this instance
cannot resolve 7%.** Every treatment observation (2015–2453) happens to be better
than the worse baseline (2661) and the best is better than the better baseline
(2232), but that is an ordering over four boots, not a measurement, and reporting
a percentage from it would be exactly the kind of claim this project has had to
retract four times. It is not reported.

What is owed: the same A/B on **`c7g.4xlarge`**, where 16 vCPUs should remove the
placement bistability, and where receive is already known to run at 99.9% of
Linux's rate so the effect will appear as µs/MiB rather than as a rate.

**The negative control worked, and is the one solid end-to-end result.** Receive
µs/MiB across all four boots and both module versions: 2107–2332, with no
separation between arms. That is exactly as predicted — the device validates L4,
`l4_csum_checked` is set, the stack skips the TCP checksum, so receive never calls
this routine on payload and cannot move. A treatment that moved receive would have
meant the reasoning was wrong.

Also incidentally established: **the earlier `-w 256K` bimodality was the send
buffer auto-sizer.** With `-w` the spread within a single boot was 2217–2715; with
`-P` pinning the buffer it collapsed to 1.2%. Anything measuring transmit cost
should pin the send buffer.

## 7. What shipped, and what it interacts with

| change | branch | evidence |
|---|---|---|
| `compute_checksum()` 4.15×, both accumulators widened, `checksum.h`, first unit test for it | `feat/net-checksum-fast` | 3.54M-case exhaustive verification; microbenchmark; kernel disassembly |
| Stop claiming the device verified the IPv4 header | `fix/ena-rx-csum-guard` | 270 MB each way, 0 errors, 0 dropped, no rate change |

**Interaction with `feat/ena-tx-offload` transmit checksum offload, stated
plainly: these two are partly substitutes, not additive.** The device supports
IPv4 L4 partial checksum on transmit. Where offload applies, it removes the whole
0.228 ns/byte, and this change's 0.173 saving is subsumed. This change is still
worth having for everything offload cannot cover — IPv6 while bit 3 stays clear,
non-TCP/UDP protocols, fragments, loopback, any future non-ENA driver — and for
the overflow fixes, which are correctness rather than speed. But if transmit
checksum offload lands and works, the marginal value of the faster loop on the
ENA transmit path drops to roughly nothing, and whoever sequences this should
expect that rather than adding the two numbers.

The L3 fix moves in the opposite direction by a negligible amount: the stack now
verifies 20 bytes of IPv4 header per frame that it previously skipped, ~0.2% of a
9001-byte frame's bytes, and ~0.05% with the faster loop.

## 8. Disproved or abandoned along the way

- **"The driver enables no receive checksum offload, so enabling it is the big
  win."** The premise is false — the device does it anyway and the driver already
  consumes it. There is no setter to call. §5.
- **`rx_enabled` as a capability gate.** Written, then removed: it reads 0 while
  the device is demonstrably checking, so gating on it would have permanently
  disabled a path for the wrong reason. The fix does not gate; it stops making an
  unprovable claim. §4.
- **`sum += (uint32)a + (uint32)(a >> 32)`.** Correct on ordinary data, wrong on
  carry-heavy data. §2.
- **Measuring a module A/B on `c7g.large`.** Defeated by per-boot bimodality. §6.
