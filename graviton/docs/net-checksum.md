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

And **one open puzzle, with four explanations killed and the instrument cleared**:
only 27% of the routine's saving materialises end to end. Eliminated in turn — cache
residency; the Haiku cross-compiler's codegen, by linking the shipped object into a
native harness; the node-walk access pattern; and the transmit path skipping bytes,
by a counter showing it walks **100.016%** of the payload with a receive control that
moved by exactly zero. Then the metric itself was calibrated by *injecting* a known
quantity of work, which it reported at **105–118%** — so **`cpu_info::active_time` is
sound and every µs/MiB figure this project has published stands as measured.** The
shortfall is real, specific to this change, in the path rather than the tool, and
unexplained. §7.3 records the one hypothesis left (memory-bandwidth contention,
untested) and the in-situ timing that would settle it.

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

#### The instrument is sound. Measured, and it disproves my own inference.

The previous version of this section inferred, by elimination, that
`cpu_info::active_time` under-attributes by ~3.7×, and flagged that as an inference
rather than a measurement. **It has now been measured, and the inference was wrong.**

The calibration injects a *known* quantity of CPU work instead of removing one: extra
passes of the old loop over the same bytes, result discarded, so the increment is an
exact multiple of a cost established independently (0.2295 ns/byte hot ×
1,048,747 bytes per MiB = **240.7 µs/MiB per extra pass**). The multiplier is chosen
by **destination port**, not by a rebuild, so baseline, 2× and 4× all happen in one
boot with one module — removing boot-to-boot variation, the thing most likely to fake
this. `c7g.4xlarge`, MTU 9001, pinned send buffer, 512 MiB per run, three magnitudes
interleaved across three repetitions.

| passes | median µs/MiB | extra passes | increment | per pass | vs predicted | rate Mbit/s | cpu busy |
|---|---|---|---|---|---|---|---|
| 1 (baseline) | 2231 | 0 | — | — | — | 4805 | 1.142 s |
| 2 | 2484 | 1 | +253 | **253.0** | **1.05×** | 4210 | 1.272 s |
| 4 | 3075 | 3 | +844 | **281.3** | **1.17×** | 3288 | 1.574 s |

Least squares over all three points: slope **283.4 µs/MiB per pass**, intercept
**2219** — within 12 µs/MiB of the measured baseline, so the relationship is a clean
proportionality with no offset. That is what two magnitudes buy: a constant factor,
not a fixed error.

**The metric reports 105% of injected work at the 2× point and 118% by the fitted
slope. It does not under-report; if anything it slightly over-reports.** The 2× point
is the trustworthy one: throughput falls from 4805 to 3288 Mbit/s across the sweep, so
the sender thread is closer to single-core CPU-limited than the 8%-of-machine figure
suggests, and at 4× it saturates and picks up scheduling cost that inflates the slope.

Counter cross-check, so the injection is known to have done what it claims: over nine
runs the real path counted 4,832,635,952 bytes against 4,831,838,208 sent (100.02%),
the injection counted 6,393,376,522 bytes against the 6,442,450,944 implied by
(1×3 + 3×3) × 512 MiB (99.2%, the shortfall being the last report line lagging), and
the leaf saw 11,226,003,493 — the sum of both, as it must.

#### So the shortfall is real, specific to this change, and unexplained

This is the fourth hypothesis to die and I am not going to guess a fifth into the
record as though it were established. What is now known:

- Adding one pass of the old loop to this exact path costs **253 µs/MiB**, and the
  metric sees it.
- Therefore the old loop genuinely costs ~253 µs/MiB in situ, consistent with its
  benchmarked 0.2295–0.2383 ns/byte.
- Replacing it with a routine benchmarked at 0.0546–0.0566 ns/byte should therefore
  have saved ~190 µs/MiB.
- §7.2 measured **52 µs/MiB**, solidly (p = 0.014, complete separation across boots).

**Every explanation that would have been comfortable is now excluded**: the bytes are
all there (100.016%), the shipped codegen is as fast as the host build, the access
pattern does not matter, and the instrument attributes work correctly.

The hypothesis I would test next, **untested and recorded as such**: the asymmetry
between added and removed work is exactly what memory-bandwidth contention would
produce. An injected extra pass re-reads data already in cache, so it costs its full
ALU price and is fully visible. The *first* pass pulls the payload from DRAM, and the
new routine's 0.0566 ns/byte is 17.7 GB/s — at the single-core streaming limit with no
headroom — while in situ the memory system is simultaneously serving the NIC's DMA
read of those same bytes and the `write()` copy that produced them. If the first pass
is memory-bound in situ, making its arithmetic 4× faster cannot recover 4× of its
time, and speeding up a *second, cache-warm* pass would.

**The measurement that would settle it, rather than another inference:** time the walk
*in situ* — read the cycle counter around `csum_walk()` and accumulate — and get the
actual in-situ ns/byte for the old and new modules directly. That converts the whole
question from a difference between a benchmark and a throughput number into one
number measured in the place it matters. It is one more counter on
`diag/checksum-byte-count`.

#### What this means for the project's other numbers: nothing changes

The previous version of this section warned that every µs/MiB figure this project has
published might be a lower bound. **That warning is withdrawn.** The instrument
attributes CPU work at 105–118% of a first-principles prediction, so the jumbo-frame,
send-buffer, memcpy, multi-queue and offload figures stand as measurements of what
they claim to measure, and nothing decided on them needs revisiting.

What remains true, and is the transferable lesson, is narrower and points at the code
rather than the tool: **on this transmit path a first-principles estimate of removed
work over-predicted the measured saving by 3.7×, and the cause is in the path, not in
the metric.** Estimates of the form "this operation costs X ns/byte, so removing it
saves X" should be treated as upper bounds here until someone measures in situ.

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

§7.3 is the transferable result, and it survived a full round of elimination:

- The isolated routine predicted 0.1823 ns/byte over a **verified 100.016%** of the
  payload — 191 µs/MiB of CPU work removed.
- The measured saving was **52 µs/MiB** (§7.2), solidly.
- The routine is not at fault: shipped codegen matches the host build, cache residency
  does not matter, and the node-walk access pattern if anything favours it.
- **The metric is not at fault either**: injecting a known 240.7 µs/MiB per pass is
  reported at 105–118%.

So a first-principles estimate over-predicted a measured saving by **3.7× on this
path, with the cause in the path and not in the tool.**

What the warning is, precisely:

- **"This operation costs X ns/byte in a benchmark, so removing it saves X" is an
  upper bound here, not an estimate.** That held for the checksum by a factor of 3.7,
  for reasons still unknown, and there is no reason to assume the transmit path
  treats another removed operation differently.
- **Do not cite this document's 3.7× as a discount on anyone's *measured* number.** It
  is the gap between an estimate and a measurement on one specific change. A µs/MiB
  measurement of TX checksum offload measures offload, and this factor says nothing
  about it.
- **µs/MiB measurements are sound and comparable to each other** — the calibration
  establishes that — which is why §7.2's A/B stands and why the project's other
  figures need no revisiting.
- Anyone measuring TX checksum offload should still use 4xlarge with a pinned send
  buffer and arms interleaved across boots (M1, M2) rather than `c7g.large`, where
  cost is bimodal by boot at 19% — that reason is independent of any of this.

There is also a live possibility that cuts *for* offload rather than against it: if the
checksum's in-situ cost is limited by memory bandwidth rather than arithmetic (§7.3's
untested hypothesis), then **offload, which removes the CPU's read of those bytes
entirely rather than merely speeding it up, would not be subject to the same
shortfall.** That would make offload's measured −12.6% credible on its own terms and
this change's 2.37% the anomalous one. Worth keeping in mind before treating my
factor as a prior for anything.

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
- **"`cpu_info::active_time` under-attributes by 3.7×."** My own inference by
  elimination, published as an inference and then **disproved by measuring it**:
  injecting a known 240.7 µs/MiB per extra checksum pass is reported at 105% at the
  2× point and 118% by the fitted slope over three magnitudes, with an intercept
  within 12 µs/MiB of baseline. The instrument is sound and the project's µs/MiB
  figures stand. §7.3.
- **Pricing an offload by the isolated cost of the work it removes.** This one
  survives, but relocated: the estimate over-predicted the measured saving by 3.7×
  and the cause is in the transmit path, not the metric. Treat "costs X ns/byte, so
  removing it saves X" as an upper bound here until measured in situ. §7.3, §8.2.
