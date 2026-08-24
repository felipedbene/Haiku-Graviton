# The internet checksum on arm64, and what receive offload is really doing

**Date:** 2026-08-24. **Hardware:** `c7g.large` (Neoverse V1, `0xd40`) and
`c8g.large` (Neoverse V2, `0xd4f`), `us-west-2`, canonical AMI
`ami-0d61e3910062bb80a`, DeBeOS `hrev59996`, MTU 9001, peer `c7g.metal`
`10.42.0.149`.

Three results, in decreasing order of how well established they are.

1. **`compute_checksum()` is 4.15× faster and no longer overflows**, worth a
   measured **+2.37% of transmit CPU per MiB** end to end (p = 0.014, clean
   negative control). Verified bit-identical against two oracles over 3,539,802
   cases on hardware. §1–§4, §7.2.
2. **The driver was telling the stack the device had verified the IPv4 header
   checksum when nothing had.** Fixed; hardware-verified. §5.
3. **Receive checksum offload was never disabled** — the reading that said it was
   is a field the device does not maintain. There is nothing to enable, and the
   receive path already computes no TCP checksum at all. §6.

Plus **two methodology findings that constrain every future transmit measurement
on this project**, not just this one. They are in §7, and getting result (1)'s
number at all depended on both:

- **M1. Transmit cost on `c7g.large` is bimodal, and the mode is chosen at boot.**
  Two identical baseline boots: 2661 vs 2232 µs/MiB, 19% apart, with ~1.5% spread
  *within* each boot. Since swapping a kernel module requires a reboot, **any
  transmit effect below ~19% is unresolvable on a 2-vCPU instance.** Measure
  transmit on `c7g.4xlarge` or larger, and interleave arms **across** boots rather
  than only within one.
- **M2. Pinning the send buffer is mandatory for any transmit-cost measurement.**
  With `nettput -w 256K` (a floor, auto-sizing still running) within-boot spread
  was 2217–2715. With `-P 256K` (pinned, auto-sizing off) it collapsed to 1.2%.
  The noise was the send-buffer auto-sizer, not the network.

And **a finding about the instrument, which is bigger than this change**: only 27%
of the routine's saving materialises end to end, and after eliminating cache
residency, the cross-compiler's codegen (by linking the shipped object into a native
harness) and the node-walk access pattern, a byte counter showed the transmit path
walks **100.016%** of the payload through the checksum — with a receive control that
moved the counters by exactly zero. So the path is innocent, TCP checksums every byte
it sends exactly once, and what is left is that **`nettput`'s summed
`cpu_info::active_time` reports only 27% of a known 191 µs/MiB of removed CPU work.**
If that holds up, every µs/MiB figure this project has published is a *lower bound*
rather than an estimate. It is currently an inference by elimination, not a
measurement of the instrument; §7.3 names the one-boot calibration that would settle
it.

And one technique worth copying, because it is why the worst bug here was caught
before it shipped rather than after: **make the tested code be the shipped code
structurally, not by transcription.** §3.

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

`src/tests/add-ons/kernel/network/checksum/`.

### 3.0 The technique: make the tested code *be* the shipped code

The harness `#include`s the shipping `checksum.h` directly. That is the single most
important structural decision here, and it is why §2's bug was caught.

The failure mode it defends against is specific and this project has already paid
for it once: a harness that tests a *transcription* of the routine tests a routine
that can differ from the shipped one in exactly the place that matters, and a
transcription is most likely to diverge precisely where the original is subtle —
which is where the bugs are. A sibling effort on this tree lost its entire
evidentiary basis that way.

The whole reason `compute_checksum()`'s body moved out of `utility.cpp` into a new
dependency-free header is to make this possible: `utility.cpp` is full of mutexes,
condition variables and `dprintf`, so it cannot be compiled in a userspace harness,
and before this change there was no way to test the algorithm except by copying it.
`checksum.h` needs only integer typedefs and an endianness macro, which the test
supplies through a `shim/` directory (types only — no algorithm, and the shim
derives `B_HOST_IS_LENDIAN` from the compiler's own byte-order macro rather than
assuming, so it cannot silently test the wrong convention). Under jam the real
headers are used instead. The file carries a comment saying not to "simplify" this
by pasting the loop in.

Two independent routes to the same principle are now in use on this tree: this one
(extract to a dependency-free header both the kernel and the harness compile), and
another agent's (`objcopy` the compiled kernel object into a native harness). Either
is fine. Transcribing the routine into the test is not.

Verification of the technique itself, not just of the routine: the built kernel
module was disassembled to confirm the shipped `checksum_data()` really does contain
the new inner loop — see §4.1.

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

## 5. The IPv4 header checksum claim, and why this bug is Haiku-specific

`NET_BUFFER_L3_CHECKSUM_VALID` tells `ipv4.cpp:1771` to skip verifying the IPv4
header. `ena_receive()` set it on:

```c
	if (context.l3_proto == ENA_ETH_IO_L3_PROTO_IPV4 && !context.l3_csum_err)
```

and its own comment said why that is not enough — `l3_csum_err` reads 0 on a frame
the device never examined — and then guarded against the wrong thing. `l3_proto`
says the device **parsed** the frame as IPv4, which it does for steering regardless
of whether it validated anything. So every received IPv4 frame arrived stamped as
verified by nobody, and the stack skipped the check on the strength of it. A
corrupted total length, protocol or address was parsed rather than dropped.
Ethernet's FCS catches most wire corruption, so what was lost is
defence-in-depth rather than daily correctness — but it was lost silently, and the
comment made it look handled.

### 5.1 Why this is Haiku's bug and not inherited from anyone

**Haiku's `NET_BUFFER_L3_CHECKSUM_VALID` is a stronger claim than anything Linux
makes.** That asymmetry is the whole story.

Linux's `ena_rx_checksum()` sets `skb->ip_summed = CHECKSUM_UNNECESSARY` **only**
from the L4 branch, and only behind an explicit `if (likely(ena_rx_ctx->l4_csum_checked))`.
It never sets it from L3. Its IPv4 handling is purely negative — on
`l3_proto == IPV4 && l3_csum_err` it records a bad checksum and drops to
`CHECKSUM_NONE`, meaning "unverified, stack must check". Linux's IP stack then
verifies the header itself, always. Linux therefore has **no concept** of "the
device verified the IP header, skip it", so it cannot have this bug.

Haiku does have that concept, and there is no device bit that can honestly support
it: the ENA receive descriptor carries `l3_csum_err` with no `l3_csum_checked`
companion to the L4 bit, so "verified and good" and "never looked" are
indistinguishable.

### 5.2 The fix, and the guard that was written and then removed

The fix is to **stop making the claim**: delete the L3 flag assignment. The stack
then always verifies the header, which is what Linux does and what correctness
requires.

An earlier version instead gated the flag on
`rx_enabled & ..._RX_L3_CSUM_IPV4_MASK`. That was written, built, and then removed,
because §6 measured `rx_enabled` reading `0x0` while the device is demonstrably
validating L4 checksums on 983 of the first 1000 frames. **`rx_enabled` does not
describe what the device does**, so gating on it would have disabled a path for a
false reason, and — worse — it would have looked like a principled capability check
while resting on a field nobody maintains. Recorded here rather than deleted
because the wrong fix is the more tempting one.

The L4 branch is untouched and is correct: it requires the explicit
`l4_csum_checked`, which the device does set.

**Cost: 20 bytes of checksum against a 9001-byte frame — about 0.2% of the bytes
the frame already costs, and ~0.05% with §4's faster loop.** For a header that is
now genuinely verified.

Hardware-verified on `c7g.large`: 270 MB received and 271 MB transmitted, **0
errors, 0 dropped**, no throughput change.

## 6. What receive offload is actually doing

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
   checksum at all — which the negative control in §7 independently confirms.
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

**Corroborated independently**, by a second agent reaching the same conclusion by a
different route, and there is direct precedent in this driver's own comments: a
missing `driver_supported_features` declaration once made the device **stop
advertising LLQ support entirely** (`ena.cpp:361-368`, on
`RSS_CONFIGURABLE_FUNCTION_KEY`). The device's advertisement is contingent on what
the driver declares, and one undeclared bit is enough. This is a one-bit
experiment, and it belongs to whoever owns `ena.cpp` — not attempted here to avoid
colliding with `feat/ena-tx-offload`.

## 7. The end-to-end measurement

Predicted before measuring, as the house standard requires: saving 0.173 ns/byte
against a transmit cost of 2182 µs/MiB (= 2.081 ns/byte) is **8.3% at the
microbenchmark's face value, realistically 5–8%** once the kernel's per-node loop
overhead, which the microbenchmark does not have, is paid.

**Measured: +2.37% on transmit (p = 0.014), and the prediction was too high by
about 3×.** §7.2. It could not be measured at all on `c7g.large`, for a reason that
outlives this result and is the more useful finding — that is M1, below.

### 7.0 M1: transmit cost is bimodal per boot on a 2-vCPU instance

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

**The negative control worked on this platform too.** Receive µs/MiB across all
four boots and both module versions: 2107–2332, with no separation between arms.
Exactly as predicted — the device validates L4, `l4_csum_checked` is set, the stack
skips the TCP checksum, so receive never calls this routine on payload and cannot
move. A treatment that moved receive would have meant the reasoning was wrong.

### 7.1 M2: pin the send buffer, always

Found on the way to M1 and independently binding on anyone measuring transmit.

The first attempt used `nettput -w 256K`, and produced a *within-boot* spread of
2217–2715 µs/MiB — 22%, which would have made even the 4xlarge measurement
useless. `-w` sets a **floor**, not a pin: `_UpdateSendBuffer()` treats
`socket->send.buffer_size` as the minimum and keeps auto-sizing above it. Switching
to `-P 256K`, which pins the buffer and disables auto-sizing outright, collapsed
the within-boot spread to **1.2%**.

So the noise was the send-buffer auto-sizer converging differently run to run, not
the network and not the NIC. **Any transmit-cost measurement on this project must
use `-P`.** A corollary worth noting separately: the auto-sizer's own convergence
is not repeatable at this resolution, which is a fact about the auto-sizer that
nobody has looked at directly.

### 7.2 The result, on `c7g.4xlarge`

Designed around M1 and M2: **arms interleaved across boots**, one arm per boot,
send buffer pinned, 5 repetitions per direction per boot, 512 MiB per run. Two
sequences run back to back and pooled, the second starting with the *opposite* arm
so a sequence-order effect cannot masquerade as a treatment effect: `A B A B A B`
then `B A B A`. Every boot's loaded module was verified by path from `listimage`
and its MTU re-read before any run. **Stock ENA driver throughout**, so §5's L3 fix
could not contaminate the receive control.

**M1 does not hold on 4xlarge, which is the precondition for trusting any of this.**
Per-boot medians within an arm span 1.4% (baseline) and 0.6% (treatment), against
19% on `c7g.large`. So the boot-to-boot bistability is a 2-vCPU artefact and this
platform can resolve a small effect.

| arm | stack module | n boots | mean µs/MiB | sd | range |
|---|---|---|---|---|---|
| A | packaged (baseline) | 4 | **2193.0** | 14.5 | 2176–2206 |
| B | mine | 4 | **2141.0** | 8.0 | 2132–2150 |

**Transmit: +2.37%, or 52 µs/MiB = 0.0496 ns/byte.**

- **Every baseline boot is worse than every treatment boot** — complete separation
  of the per-boot medians, with a 26 µs/MiB gap between the two ranges.
- Welch *t* = 6.28; exact permutation test on the 8 boot medians, one-tailed,
  **p = 0.0143** (1 of 70 arrangements is as extreme).
- Transmit rate was **4964–4965 Mbit/s in all eight boots** — pinned to three
  digits. The path is window-limited, so as expected the effect appears only in
  µs/MiB and not in rate.

**Negative control — receive: +0.74%, not significant.** A 2720.5 ± 18.4, B
2700.5 ± 19.2, ranges overlapping, permutation p = 0.10. Receive computes no TCP
checksum (the device validates L4, §6) and no IPv4 header checksum (stock driver,
§5), so it should not move, and it does not. Net of the control the transmit effect
is ~1.6%; the control also sets the noise floor at ~0.7%, giving the transmit
result a signal-to-noise of about 3:1.

Two boots of the second sequence produced no data at all — all 20 runs failed
together. Almost certainly a concurrent agent's `nettput-run`, which begins with
`pkill -f nettput-peer.py` and would have killed my peer mid-sequence. Failed runs
are *absent*, not wrong, so they reduce n rather than biasing it; they are why n is
4 per arm and not 5. **Anyone sharing the builder as a throughput peer should
expect this**, and a future harness should use a distinct peer port and not
blanket-`pkill`.

### 7.3 Only 27% of the saving appears, and it is not the routine

Predicted 0.173 ns/byte from the isolated routine. Measured 0.0496 ns/byte end to
end. Three candidate explanations were tested. **All three are wrong**, and what is
left is a sharper question than the one I started with.

**Hypothesis 1: the kernel's walk is memory-bound where the microbenchmark is
cache-hot.** Making arithmetic 4× faster buys nothing against a memory wall.

*Disproved.* Same two routines, 1988-byte chunks, Neoverse V1:

| condition | replaced | current | speedup | saving |
|---|---|---|---|---|
| L1-resident, re-read | 0.2295 | 0.0555 | 4.14× | 0.1740 ns/B |
| 512 MB streamed, each chunk touched once | 0.2290 | 0.0566 | 4.05× | 0.1725 ns/B |

Identical to within 2%. The prefetcher keeps a linear walk fed; the streaming-read
floor on this core is 0.0635 ns/B (15.8 GB/s), which the *new* routine essentially
reaches while the old one was nowhere near it. So the replaced routine was
ALU-bound, the new one sits at the memory limit, and neither fact depends on cache
residency.

**Hypothesis 2: the Haiku cross-gcc produced worse code than the host build.**
Plausible on the evidence: the kernel add-on compiles with `-fno-tree-vectorize`
and `-mcpu=neoverse-n1+crypto` — tuning for *Graviton2* on a Graviton3/4 host —
neither of which the host benchmark had.

*Disproved, by linking the actually-shipped object into a native harness.*
`compute_checksum` sits at `.text+0x38c` in the built `utility.o`, size 0x110 (68
instructions, the whole loop inlined), and — checked, because it is the
precondition for this technique — has **zero relocations** in that range: a
self-contained leaf. Extracted with `objcopy --dump-section`, `mmap`ed executable,
and called through a function pointer on the builder's Neoverse V1 core, alongside
host-compiled copies of the same source.

Gate first: the extracted blob agreed with both host-compiled routines on every
length 0..600 × every alignment 0..7, **0 mismatches**, so the timings below are of
the real thing.

| variant | hot ns/B | cold ns/B |
|---|---|---|
| replaced loop (host gcc) | 0.2303 | 0.2312 |
| new (host gcc, default flags) | 0.0497 | 0.0532 |
| **new (Haiku cross gcc, the shipped bytes)** | **0.0501** | **0.0546** |

**Within 1% hot and 3% cold.** Re-running the whole harness with `-O2
-fno-tree-vectorize -mcpu=neoverse-n1+crypto` changed nothing (0.0495/0.0531), so
those flags are not costing anything here either. The codegen is exonerated.

**Hypothesis 3: the node walk.** `checksum_data()` gets one `data_node` at a time —
about five per jumbo frame — pointer-chasing across freshly allocated, cache-cold
slab memory. That is a different access pattern from a linear 512 MB stream, and
the streaming control specifically showed the *linear* case is prefetcher-friendly.

*Disproved.* Simulating it — 5 nodes of 1900 bytes per frame, nodes spaced 2048
bytes apart as the slab allocates them, whole 512 MB working set, cold:

| variant | node-walk ns/B |
|---|---|
| replaced loop | 0.2383 |
| new (host gcc) | 0.0546 |
| new (shipped kernel bytes) | 0.0560 |

Saving **0.1823 ns/byte** — if anything *larger* than the linear case, not smaller.
The access pattern is not it either.

#### What that leaves: the transmit path is innocent, so the metric is not

The routine delivers **~0.18 ns/byte, with the exact bytes the kernel ships, on the
exact access pattern the kernel uses**. So the remaining binary question was whether
the transmit path puts only ~27% of the payload through it, or whether `nettput`'s
cost metric misses the removed work.

**Instrumented and answered: the path walks 100.016% of the payload.** A byte
counter in `checksum_data()` plus one in the `compute_checksum()` leaf, so no caller
could be missed, over a 512 MiB transfer on `c7g.4xlarge`:

| quantity | value | vs payload |
|---|---|---|
| application payload | 536,870,912 B | — |
| bytes into `checksum_data()` | 536,958,717 B in 131,559 calls | **100.016%** |
| …of which calls ≥1024 B (data segments) | 535,568,266 B in 65,138 calls | 99.76% |
| bytes into the `compute_checksum()` leaf | 536,949,736 B in 465,657 calls | 100.015% |

The 0.016% excess is TCP headers and options; 65,138 calls over 512 MiB is 8,222
bytes per segment, and 465,657/65,138 = **7.15 leaf calls per segment**, i.e. the
node walk, at about 1,150 bytes per node.

**Direction control, and it is exact:** an immediately following 512 MiB *receive*
transfer moved the counters by **zero bytes and zero calls** — identical strings
before and after. Receive computes no TCP checksum (the device validates L4, §6) and
on this image no IPv4 header checksum either (pre-§5 driver), so the transmit figure
above is transmit and nothing else. That also means there was never a direction
confound to worry about.

**So there is no correctness bug**, and I want to be unambiguous about that because
"TCP checksums only a quarter of what it sends" would have been serious: TCP
checksums every byte it sends, exactly once, as `add_tcp_header()` reads.

By elimination, then:

| | |
|---|---|
| bytes through the routine, per MiB of app data | 1,048,747 |
| measured saving of the routine on that access pattern | 0.1823 ns/byte |
| **CPU work removed, from first principles** | **191 µs/MiB** |
| **CPU time saved, as `nettput` measures it** | **52 µs/MiB** (§7.2) |
| fraction of known removed work the metric reports | **27.2%** |

For scale: at 0.2383 ns/byte the old checksum accounts for 128 ms of the 1.132 s of
CPU the run reports — 11.3% of transmit CPU — and removing 80% of it should have
shown up as ~9%. It showed up as 2.37%.

#### This is an inference about the instrument, not a measurement of it

Stated plainly because it is the weakest link in the chain: I have measured that the
bytes are all there and that the routine is fast on them, and I have measured the
end-to-end saving. **I have not directly measured the metric's error.** Concluding
"`cpu_info::active_time` under-attributes by ~3.7×" is what is left after the other
explanations died, not something observed.

Two mechanisms would produce it, and they are not equally comfortable:

- **The metric under-counts.** `active_time` is *built* by summing each thread's
  `kernel_time`/`user_time` deltas as it leaves the CPU
  (`scheduler_cpu.cpp:264-276`, established in `net-receive-profile.md`). Any work
  that is not attributed to a thread at a context switch — or is attributed at a
  coarser granularity than it happens — is invisible to it.
- **Freed CPU is reabsorbed.** The transfer is window-limited (rate pinned to three
  digits across eight boots), so removing sender work cannot speed it up; if the
  freed time is spent spinning or polling rather than idling, `active_time` does not
  fall. Against this: the machine is only **8.0% busy** (1.132 s of CPU over 0.884 s
  wall across 16 CPUs), so there is abundant idle to fall into rather than spin in.

**The experiment that discriminates them, and it is a clean one:** *inject* a known
quantity of CPU work into this path — run the old checksum loop twice, doubling a
cost already computed at 128 ms per 512 MiB — and see how much of that known
increment the metric reports. If +128 ms of real work shows up as +35 ms, the
instrument under-attributes by the same 3.7× and the finding is about
`cpu_info::active_time`. If it shows up as +128 ms, then the instrument is fine on
work that is *added* and something about *removing* work on a rate-limited path is
different, which points at reabsorption. This calibrates the instrument directly
rather than by difference, needs no A/B, and is one module build and one boot.

#### Why this matters beyond this change

**If the instrument is the answer, every µs/MiB figure this project has reported is
a lower bound on the real CPU saving, not an estimate of it** — including the jumbo
frame, send-buffer, multi-queue and offload numbers. It would not make any of them
wrong in sign or in ranking between changes of the *same* kind, but it would make
them incomparable to first-principles estimates, and it would mean this project has
been systematically understating its own wins. That is worth one boot to find out.

## 8. What shipped, and what it interacts with

| change | branch | evidence |
|---|---|---|
| `compute_checksum()` 4.15×, both accumulators widened, `checksum.h`, first unit test for it | `feat/net-checksum-fast` | 3.54M-case exhaustive verification; microbenchmark; kernel disassembly |
| Stop claiming the device verified the IPv4 header | `fix/ena-rx-csum-guard` | 270 MB each way, 0 errors, 0 dropped, no rate change |

Measured end to end at **+2.37% transmit CPU per MiB** (p = 0.014) across 8
interleaved boots on `c7g.4xlarge`, with receive as a clean negative control. §7.2.

### 8.1 What this change is *for*: breadth, not a share of the transmit number

Stated first, because it is the durable answer and the transmit percentage is not.

**On the TCP transmit path over ENA, this change and `feat/ena-tx-offload`'s
checksum offload are near-complete substitutes, not partial ones.** Once offload is
on, TCP does not call `compute_checksum()` over the payload at all — there is no
residual walk for a faster loop to speed up. That is a correction to my own earlier
"partly substitutes" framing, and it moves *against* this change: with the real
4.15× rather than the 2.4× that agent had assumed, the arithmetic is that the loop
costs ~239 µs/MiB today and ~58 µs/MiB after this change, so this removes ~181,
while offload removes a measured 301–361. Combined ≈12.6–15.1%, **not** the ~20%
that summing the two headlines would give.

So the value of this change is what offload cannot reach:

- **loopback**, which has no device to offload to
- **any non-ENA device**, and ENA before/without the offload path
- **IPv6**, for as long as offload bit 3 stays clear (§6) — which is to say, for as
  long as we do not declare the host_info bit
- **non-TCP/UDP protocols**, ICMP and ICMPv6
- **IP fragments**, and the per-fragment header checksum in `ipv4.cpp:647`
- **receive**, wherever the device did not validate — including every IPv4 header
  now that §5 stopped pretending it had been checked
- and the **two overflow fixes**, which are correctness and do not substitute for
  anything

That list, plus "it made a decades-old TODO false", is the case for it. The +2.37%
is a true number about one path that is likely to be superseded on that path.

### 8.2 A warning for anyone pricing an offload

§7.3 is the transferable result: the isolated routine predicted 0.173 ns/byte and
the transmit path delivered 0.0496, and the shortfall is **not** in the routine —
codegen, cache residency and node-walk access pattern were each tested and
eliminated. It is in the path or in the cost metric.

So a first-principles estimate and a µs/MiB measurement **disagree by ~3.7× on this
path**, and §7.3 has now placed the discrepancy in the instrument rather than in the
code: the bytes are all there and the routine is fast on them.

That changes what the warning is. It is **not** "offload will deliver less than you
think" — offload may well deliver its full first-principles value in CPU terms. It is
that **µs/MiB and first-principles estimates are not commensurable here**, in the
direction of µs/MiB reading low. So:

- Do not cite this document's 3.7× as a discount on anyone else's *measured* number.
  It is a discount on *estimates* relative to *this metric*, which is a statement
  about the metric.
- Two µs/MiB measurements of similar changes remain comparable to each other, which
  is why §7.2's A/B stands.
- Anyone measuring TX checksum offload should still use 4xlarge with a pinned send
  buffer and arms interleaved across boots (M1, M2) rather than `c7g.large`, where
  cost is bimodal by boot at 19% — that reason is independent of any of this.

The L3 fix moves in the opposite direction by a negligible amount: the stack now
verifies 20 bytes of IPv4 header per frame that it previously skipped, ~0.2% of a
9001-byte frame's bytes, and ~0.05% with the faster loop.

## 9. Disproved or abandoned along the way

- **"The driver enables no receive checksum offload, so enabling it is the big
  win."** The premise is false — the device does it anyway and the driver already
  consumes it. There is no setter to call. §6.
- **`rx_enabled` as a capability gate.** Written, then removed: it reads 0 while
  the device is demonstrably checking, so gating on it would have permanently
  disabled a path for the wrong reason. The fix does not gate; it stops making an
  unprovable claim. §5.
- **`sum += (uint32)a + (uint32)(a >> 32)`.** Correct on ordinary data, wrong on
  carry-heavy data. §2.
- **Measuring a module A/B on `c7g.large`.** Defeated by per-boot bimodality. §7.0.
  Redone successfully on `c7g.4xlarge`, where the bimodality is absent. §7.2.
- **Three explanations for why only 27% of the isolated saving appears, all wrong.**
  (a) *Memory-bound walk*: cold-streamed 512 MB gives the same 4.05× and the same
  0.1725 ns/byte saving as an L1-resident buffer, and the new routine sits at the
  core's streaming-read floor. (b) *Haiku cross-gcc codegen*, suspected because the
  kernel builds with `-fno-tree-vectorize` and `-mcpu=neoverse-n1` on a Graviton3/4
  host: the shipped bytes, extracted with `objcopy` and run natively, are within 1%
  hot and 3% cold of the host build, and adding those flags to the host build changes
  nothing. (c) *Node-walk pointer-chasing over cold slab memory*: simulated at 5
  nodes of 1900 bytes per frame, 2048-byte stride, and the saving is 0.1823 ns/byte —
  *larger* than the linear case. The shortfall is in the path or the metric. §7.3.
- **"The transmit path must be skipping bytes."** The surviving explanation after the
  other three died, and it is also wrong: a byte counter says 100.016% of the payload
  goes through the checksum, in 65,138 data-segment calls averaging 8,222 bytes, over
  7.15 nodes each. TCP checksums every byte it sends, exactly once. §7.3.
- **Pricing an offload by the isolated cost of the work it removes.** The routine
  said 0.1823 ns/byte over a verified 100% of the payload — 191 µs/MiB — and the
  metric reported 52. Any estimate built by pricing removed work, including for TX
  checksum offload, is not commensurable with a µs/MiB measurement until the
  instrument is calibrated. §7.3, §8.2.
