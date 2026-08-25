# Where the CPU goes in the arm64 network receive path

**Date:** 2026-08-24. **Hardware:** `c7g.large` (Graviton3 / Neoverse V1, 2 vCPU),
`us-west-2`, AMI `ami-0b1569c656c3b6765`, Haiku `hrev59996`, single queue,
MTU 9001, peer `c7g.metal` `10.42.0.149` on the same subnet.

> ## READ THIS FIRST — the date above is misleading (banner added 2026-08-24)
>
> **The date says today; the body is pre-merge.** The *profiling* in this document
> is sound and is still the reference for where receive CPU goes. But its §0 verdict,
> §4, §7 and §8 describe work as pending that has since **landed**, and its
> recommendations have been superseded by measurements taken after it was written.
> Corrected in place below; the per-item summary:
>
> | This document says | Actual state, 2026-08-24 |
> |---|---|
> | "arm64 has no optimised `memcpy`" (§0.3, §4, §7) | **MERGED** — `f76217c69b`. See `arm64-memcpy.md`. |
> | memcpy worth **6.4% ± 1.8%** of receive CPU | Superseded. That was an *in-situ floor*; the end-to-end figure is **8.4%** (itself self-revised **down** from a single-boot 12.6%). |
> | the memcpy design: "align the destination, then 32 bytes per iteration" (§7) | **Not what shipped, and was measured wrong.** What shipped: no loop below 129 B, align to **16**, 64 B/iteration. |
> | "run the string tests before trusting it" (§7) | **There is no string test suite in this tree** — `arm64-memcpy.md` §3.1. A correctness test was written as part of that work (`c4fbf637c5`). |
> | profiler reports `100.00% unknown`, "fixing it is the bake I want" (§0.4, §8) | **MERGED** — `830d8d0814` implements `arch_debug_get_stack_trace()`. |
> | "transmit checksum offload … not where the measured cost is" (§"Not worth doing") | **MERGED and it did pay** — **−3.54%, p = 0.0079**. |
> | "multi-queue receive confirmed worthless on this instance" | **Correct, and now closed fleet-wide**: Linux on ONE ENA queue does 29826 Mbit/s vs 29823 on eight. |
>
> **Resolved 2026-08-25 — what limits receive is now accounted for
> (`ena-receive-latency-account.md`).** The receive fit is **2.34 µs/frame +
> 1.85 ns/byte** and ~88% of the per-byte term is still not itemised as *cost* (the §0
> measurement stands). But that per-byte cost is **not** what caps throughput, and it
> was the wrong thing to call the "live thread": the machine is 97.3% idle at the
> ceiling, so cost is not the constraint. The ceiling is **bufferbloat in the
> device-interface receive FIFO** — a standing ~13.7 ms, ~16 MiB queue that is ~99.93%
> of a frame's transit time — **plus `TCPEndpoint::fLock`, ~47% of the ceiling**. The
> consumer thread is 100% wall-clock saturated while only ~53% CPU-busy, which is
> precisely why every CPU-based instrument in this document missed it. A time-based
> **CoDel** queue discipline was built and hardware-measured but **NOT merged** (it
> cannot hit ≤0.2% loss and ≤1 ms latency together — TCP loss–delay coupling, Mathis);
> the loss-free fix is **ECN**. Only the "unexplained / live thread" framing is
> retired; the per-byte measurement itself is untouched.
>
> **One caveat this document is the origin of, so it is corrected here.** The
> per-frame/per-byte split is a **receive** fit. **There is no transmit fit** — none
> exists anywhere in this tree. §3's line "Transmit is the same shape, more extreme"
> is an assumption, not a measurement, and it has since been propagated into other
> documents as if it were one. Do not apply `1.85 ns/byte` to transmit.

`throughput-measurement.md` established that receive costs ~2100 µs of CPU per
mebibyte at 4.9 Gbit/s — about **19 µs of CPU per 9 KB frame**, roughly ten times
what the work justifies. It could not say where that goes. This does, and the
answer overturns two things previously believed about it.

Everything below is measured on hardware. Where a number is a fit, the fit and
its residuals are given; where an experiment failed, it is reported as failed.

---

## 0. Verdict

1. **The receive cost is not per-frame.** It is **88% per-byte**
   (1.85 ± 0.12 ns/byte) and only **12% per-frame** (2.34 ± 0.65 µs/frame), from a
   15-point MTU sweep. Every plan written on the premise that per-frame cost is
   the lever — including `ena-multiqueue-plan.md` §6 — is aimed at the smaller
   half of the problem.
2. **Four threads account for 99.9% of it**, and the largest is not the driver
   and not the stack: it is the *application's own `read()` syscall*, at 46%,
   98% of it in kernel time.
3. ~~**arm64 has no optimised `memcpy`.**~~ **FIXED and merged 2026-08-24
   (`f76217c69b`); worth 8.4%, not 6.4%.** As written: it used the generic C one, which copies
   **one byte at a time** whenever source and destination differ in alignment
   mod 8 — the normal case on receive. Measured: **0.388 ns/byte mismatched
   against 0.074 aligned**, and a destination-aligning replacement does
   **0.056 at every alignment**. Removing the penalty is worth a measured
   **6.4% ± 1.8%** of receive CPU (paired, *p* ≈ 0.006), and the replacement is
   ~30% faster in the aligned case too. **Superseded:** 6.4% was an in-situ floor;
   the end-to-end figure on the baked image is **8.4%** across three boots — itself
   revised **down** from 12.6% measured on a single boot. Quote 8.4%.
4. **I account for 100% of the 19 µs to a named thread and 22% to a named
   operation.** The remaining 78% is inside four known threads and needs a
   sampling profiler to break down further. That profiler exists, runs today, and
   ~~reports `100.00% unknown` because one arm64 hook is a stub. Fixing it is the
   bake I want.~~ **The hook is FIXED and merged (`830d8d0814`,
   `arch_debug_get_stack_trace()` plus keeping the frame pointer). The profiler
   attributes properly; the bake happened.** The 78% is still not broken down —
   that work is available now, not blocked.

---

## 1. What was measured with, and why that route

Four instruments, cheapest first. Three of the four need no image bake, which is
why there are 60-odd hardware measurements here rather than three.

### 1.1 `src/bin/netprof` — an *exact* per-thread partition

`nettput` reports the total from `cpu_info::active_time`. `netprof` decomposes
that same total across every thread on the system over a window.

The partition is exact rather than statistical, and deliberately so.
`CPUEntry::TrackActivity()` (`scheduler/scheduler_cpu.cpp:264-276`) *builds*
`active_time` by summing the `kernel_time` and `user_time` deltas of each
non-idle thread as it leaves the CPU. So the per-thread deltas do not estimate
`nettput`'s number — they are its addends. The tool prints both sides and their
residual, so an error cannot hide:

```
reconciliation
  sum over threads      :    8004155 us
  sum over cpus (active):    8009894 us
  residual              :       5739 us (+0.07%)
```

**Residuals across all clean runs: 0.01% to 0.10%.** That is the strongest
evidence in this document, because it means nothing significant is happening
outside the threads listed — no hidden interrupt cost, no unaccounted kernel
work.

Two things it cannot do, both stated in its header comment:

- **Interrupt handler time is not separated.** The kernel charges it to whichever
  thread it interrupted; it keeps a separate total only in `cpu_ent::irq_time`
  (`interrupts.cpp:367-369`), which no syscall exposes. Interrupt time taken on
  an *idle* CPU escapes `active_time` entirely and would show as a negative
  residual. None was seen, and the CPUs were 60-67% busy, so this is bounded
  small.
- **A thread that exits mid-window** is missing from the table but present in
  `active_time`, so it appears as a positive residual. That is how the
  `nettput` figures for four of the MTU rows were recovered, and it is why the
  first version of the tool had to stop counting idle threads: they accrue an
  entire wall-clock window each and broke the reconciliation by 100%, in the
  direction that hides everything else.

### 1.2 A 15-point MTU sweep with exact frame counts

MTU varies frames-per-byte by ~6x. Rather than infer frame counts from a rate,
each window is bracketed by the interface's own counters **inside a single remote
shell**, so no ssh round trip lands between the brackets:

```
ifconfig /dev/net/ena/0 | grep Receive: ; netprof -n 6 6 ; ifconfig ... | grep Receive:
```

That yields exact frames, exact bytes and exact per-thread CPU for the same
bracket, which is what makes the regression in §3 possible.

### 1.3 `src/tests/system/benchmarks/memcpybench.c`

Times `memcpy()` at the alignments and sizes the receive path actually uses,
against a candidate replacement, **from one binary on one machine** — so the
comparison cannot be explained by the clock, the compiler or the memory system.
It verifies the replacement against `memcpy()` at all 64 alignment pairs over 105
sizes first, because a copy that is wrong is usually a fast one.

### 1.4 `nettput -A` / `-W` — the alignment penalty *in situ*

§1.3 measures the routine. Whether the receive path actually *pays* the penalty is
a different question, and `-A` (shift the data buffer) answers it without touching
the kernel. Getting it to answer anything took two tries; see §5.

### 1.5 What was checked first and rejected

`profile` (`src/bin/debug/profile`, in the minimum image at
`build/jam/images/definitions/minimum:79`) is a real sampling profiler over the
kernel's `system_profiler`, and pointing it at the kernel would have been far
cheaper than any of the above. **It does not work on arm64.** Verified on the
canonical AMI during a live 4.9 Gbit/s transfer:

```
profiling results for summary "all" (1):
  tick interval:  1000 us
  total ticks:    10040 (10040000 us)
  expected ticks: 11143 (missed 1103)
  unknown ticks:  10040 (10040000 us, 100.00%)
  no functions were hit
```

It samples at the right rate, enumerates every thread, and attributes 100% of it
to nothing, because `arch_debug_get_stack_trace()` in
`src/system/kernel/arch/arm64/arch_debug.cpp` was `return 0;`. That is fixed on
this branch (§7).

---

## 2. The partition: four threads, 99.9%

MTU 9001, 4.9 Gbit/s, 8 s window, 2 vCPU at 61% busy. Shares are the mean of two
independent runs; per-frame figures use the §3 fit.

| thread | share | µs/frame | what it is |
|---|---|---|---|
| `nettput:nettput` | **46%** | 8.80 | the application's `read()` — **98% kernel time** |
| `kernel_team:/dev/net/ena/0 reader` | **26%** | 5.08 | driver ISR wake-up → `ena_receive()` → deframe → FIFO |
| `kernel_team:/dev/net/ena/0 consumer` | **19%** | 3.52 | FIFO → IPv4 → TCP → socket wake-up |
| `kernel_team:net timer` | **8%** | 1.56 | TCP timers |
| all other threads (≈25 of them) | **0.1%** | 0.02 | |

Raw, from one run (8 s window, 2 CPUs, 9,754,274 µs of `active_time`):

```
      cpu us    user    kernel   %total  thread
     4337682  109340   4228342   44.47%  nettput:nettput
     2589124       0   2589124   26.54%  kernel_team:/dev/net/ena/0 reader
     2029348       0   2029348   20.80%  kernel_team:/dev/net/ena/0 consumer
      792327       0    792327    8.12%  kernel_team:net timer
        1211       0      1211    0.01%  kernel_team:page scrubber
   ... 17 further threads totalling 714 us ...
  residual: 1048 us (+0.01%)
```

**The single largest consumer of receive CPU is the receiving application's own
syscall, and it is essentially all kernel time.** That is not where any previous
analysis in this tree looked. ~~Transmit is the same shape, more extreme:~~ the
sending thread is 73% of the cost at 88.8% of one whole CPU.

> **Corrected 2026-08-24 — "the same shape" was an assumption and it propagated.**
> The 73%/88.8% thread attribution *is* measured. What is **not** measured is that
> transmit shares receive's per-frame/per-byte *decomposition*: **there is no
> transmit fit anywhere in this tree.** This sentence was read as licence to apply
> receive's `2.34 µs/frame + 1.85 ns/byte` to transmit, which then appeared in
> `tcp-tso.md` §6 as a saving expressed as a fraction of "the 1.85 ns/byte per-byte
> term" — for a transmit change. `tcp-tso.md` §4.4 retracts that explicitly. Two
> thread-level cost profiles being lopsided in the same direction does not make the
> underlying models transferable.

One asymmetry worth recording: `net timer` is 8% on receive and **0.001%** on
transmit (94 µs versus 792,327 µs over comparable windows). Whatever it is doing,
it is receive-specific.

---

## 3. Per-frame or per-byte? Per-byte, 88% of it

Fifteen points, MTU 1500 to 9001, exact frame and byte counts, least squares of
`cpu = a·frames + c·bytes`. Twelve points fitted (MTU ≥ 2500 — see §3.2).

| component | µs per frame | ns per byte | R² | @9001: frame | byte | total |
|---|---|---|---|---|---|---|
| **total** | **2.34 ± 0.65** | **1.847 ± 0.117** | 0.940 | 2.34 | 16.63 | **18.96** |
| `nettput` read | 0.43 ± 0.59 | 0.930 ± 0.106 | 0.803 | 0.43 | 8.37 | 8.80 |
| ena reader | 0.43 ± 0.27 | 0.517 ± 0.048 | 0.846 | 0.43 | 4.65 | 5.08 |
| ena consumer | 1.48 ± 0.27 | 0.227 ± 0.049 | 0.843 | 1.48 | 2.04 | 3.52 |
| net timer | −0.01 ± 0.12 | 0.173 ± 0.021 | 0.798 | 0.00 | 1.56 | 1.56 |

The model reproduces every fitted row to within 12%:

```
  mtu  9001  measured 17.55 us/frame   model 18.59   +5.9%
  mtu  7000  measured 16.34            model 15.09   -7.6%
  mtu  5000  measured 12.31            model 11.60   -5.8%
  mtu  3500  measured  8.01            model  8.98   +12.1%
  mtu  2500  measured  7.01            model  7.23   +3.1%
```

**1.85 ns/byte is 4.8 cycles per byte at 2.6 GHz.** That is the number to
explain, and per-frame overhead is not it.

### 3.1 Node count is *not* an independent driver — and that explains the 1% win

`net_buffer` node count was the obvious third candidate: the driver posts
1920-byte receive buffers (confirmed live — the driver logs `chain capacity
15090` = 8 × 1920 − 270), so a frame arrives as `ceil(len/1920)` nodes and every
layer touches each one. Adding `nodes` as a third regressor moves R² from 0.940
to only 0.956 and its coefficient is **1.8σ** (1.01 ± 0.56 µs/node). Nodes and
bytes are collinear across an MTU sweep and cannot be cleanly separated by one,
but the fit gives no reason to prefer nodes.

Independent confirmation from the change that already shipped: **2048 → 1920
roughly halved nodes per jumbo frame (~9 → 5) and was worth 1%.** A large
per-node cost is incompatible with that. Both lines of evidence agree, so the
node-count hypothesis is closed rather than merely unproven.

### 3.2 Correction: MTU 1500 is *not* four times dearer per byte because it is 1500

`throughput-measurement.md` records receive at 8764 µs/MiB at MTU 1500 against
2144 at 9001 and concludes "jumbo costs 0.24× per byte". The frames+bytes model
misses MTU 1500 by **−54%**, and the three low-MTU points share a signature:

| MTU | rate | ns/byte |
|---|---|---|
| 1500 | 117 MiB/s | 7.92 |
| 1800 | 175 MiB/s | 5.84 |
| 2000 | 221 MiB/s | 3.61 |
| 2500 – 9001 | 157 – 597 MiB/s | 2.13 – 2.81 |

The per-byte cost tracks the **connection rate**, not the frame size: at MTU 1500
the stream runs at a fifth of the speed and every thread's cost per byte
inflates, `net timer` worst (4× at MTU 1500 versus MTU 2500). So the MTU 1500 /
9001 comparison conflates frame size with connection rate, and 0.24× overstates
what frame size alone buys. Jumbo frames are still a large win; the mechanism was
misread.

### 3.3 Read chunk size is irrelevant, so it is not the syscall

nettput's own end-to-end numbers, MTU 9001:

| read chunk | rate | cost |
|---|---|---|
| 4 K | 924 Mbit/s | 8884 µs/MiB |
| 16 K | 4959 Mbit/s | 2172 µs/MiB |
| 64 K | 4481 Mbit/s | 2102 µs/MiB |
| 1 M | 4478 Mbit/s | 2198 µs/MiB |

**16 K to 1 M is 64× fewer syscalls for the same cost within noise.** Per-call
overhead in the read path is therefore negligible, and `nettput`'s 46% is work
proportional to the data, not to the number of times it asks for it. (The 4 K row
is not a syscall effect either — at 924 Mbit/s it is the same rate-dependent
inflation as §3.2.)

---

## 4. ~~arm64 has no optimised memcpy~~ — FIXED and merged 2026-08-24 (`f76217c69b`)

> The measurement in this section (0.388 ns/byte mismatched vs 0.074 aligned) is
> the diagnosis that motivated the fix and it stands. The *state* it describes does
> not: arm64 now has a hand-written `memcpy`. See `arm64-memcpy.md` for what
> shipped, and note the shipped design differs from the one proposed in §7 here.

`src/system/libroot/posix/string/arch/arm64/Jamfile` and
`src/system/kernel/lib/arch/arm64/Jamfile` both build
`string/arch/generic/generic_memcpy.c`. x86_64 has its own `memcpy.cpp`; arm64
does not. The generic routine reaches its word-at-a-time loop **only when source
and destination are misaligned by the same amount**, and copies one byte at a time
otherwise. That condition exists for architectures on which an unaligned access
traps. arm64 does them in hardware.

A received TCP payload starts 54 bytes into the frame (14 + 20 + 20), so copying
it out to a buffer from `malloc()` is a source at 6 mod 8 against an aligned
destination — mismatched. `read_data()` (`net_buffer.cpp:1509-1535`) does one
`user_memcpy()` per node, and on arm64 `user_memcpy` is
`arch/generic/user_memory.h:79` → plain `memcpy` inside a `FaultHandlerGuard`.

Measured on the test node, one binary, cold 64 MiB buffers
(`memcpybench`; the replacement verified identical to `memcpy()` at all 64
alignment pairs over 105 sizes first):

| case | size | src off | dst off | libroot ns/B | cyc/B | replacement ns/B | ratio |
|---|---|---|---|---|---|---|---|
| aligned | 8961 | 0 | 0 | 0.085 | 0.22 | 0.056 | 1.50× |
| **source off 6 (TCP payload)** | 8961 | 6 | 0 | **0.388** | **1.01** | **0.056** | **6.94×** |
| both off 6 (same mod 8) | 8961 | 6 | 6 | 0.074 | 0.19 | 0.065 | 1.15× |
| dest off 6 | 8961 | 0 | 6 | 0.388 | 1.01 | 0.064 | 6.06× |
| driver → net_buffer | 1920 | 0 | 0 | 0.125 | 0.33 | 0.105 | 1.19× |
| net_buffer → user | 1920 | 6 | 0 | 0.415 | 1.08 | 0.113 | 3.69× |
| a socket read | 65535 | 6 | 0 | 0.387 | 1.01 | 0.042 | 9.23× |

Any mismatch costs the same 0.388 ns/byte — off by 1, 2, 4 or 6 are identical, so
this is the byte loop and nothing subtler. **The penalty is 5.2×, and the
replacement is faster than the aligned generic path as well.**

### 4.1 But the copies are not the whole per-byte cost — an important limit

Two copies at their *aligned* cost are 2 × 0.086 = 0.17 ns/byte; at the
*mismatched* cost, 0.78. The path's measured per-byte total is **1.85**. So even
in the worst case the copies are under half of it, and if they were aligned they
would be 9%.

**Most of the 1.85 ns/byte is therefore not copying.** A plain scalar copy of
cold 64 MiB buffers runs at 0.085 ns/byte in the same binary, so it is not raw
memory bandwidth either. The likely remainder is demand cache misses on a
*scattered* pattern the prefetcher cannot follow — 1920-byte chunks spread over a
2 MB DMA ring, slab objects, then a cross-core re-read by the reading thread —
plus per-node bookkeeping. **That is a hypothesis, not a result.** It is exactly
what §1.5's profiler would settle, and it is why the profiler bake matters more
than any single fix named here.

---

## 5. Does the receive path actually pay the penalty? Yes, ~6%

§4 measures the routine in a benchmark. Whether the kernel's copies are really
mismatched is separate, and the first attempt to find out **failed**, which is
worth recording because the failure was in the experiment:

> Sweeping `-A 0..7` at MTU 9000 came out flat. `recv()` on a stream socket
> returns whatever is queued, so each call starts at an arbitrary offset and the
> destination phase is randomised no matter where the buffer begins. Shifting the
> buffer moved the first read and nothing after it. The sweep measured nothing
> and looked like a clean negative.

Three conditions have to hold together: MSG_WAITALL so every read moves exactly
the chunk (`-W`), a chunk that is a whole number of segments (8960 × 7 = 62720,
under the default 65535 receive buffer), and an MSS that is a multiple of 8 —
MTU **9000**, not 9001. With all three, and offsets interleaved in pairs so drift
cannot fake a difference, 10 pairs of 4 GiB each:

| pair | `-A 6` | `-A 3` | Δ |
|---|---|---|---|
| 1 | 1880 | 1987 | +107 |
| 2 | 1864 | 2161 | +297 |
| 3 | 1861 | 2175 | +314 |
| 4 | 2042 | 1985 | −57 |
| 5 | 1944 | 2019 | +75 |
| 6 | 2041 | 2130 | +89 |
| 7 | 2023 | 2183 | +160 |
| 8 | 1936 | 2163 | +227 |
| 9 | 1951 | 1972 | +21 |
| 10 | 2036 | 2149 | +113 |

**Mean difference 135 ± 37 µs/MiB (SE), paired *t* = 3.62 on 9 df, *p* ≈ 0.006,
9 of 10 pairs positive. 6.4% of the cost, or 1.15 µs per frame.**

Read this as a **floor, not a ceiling**:

- `-A 3` is not necessarily the worst offset, nor `-A 6` the best; they were
  picked from a noisy 8-point scan.
- The full byte-loop penalty over an 8960-byte payload would be 2.81 µs/frame
  (329 µs/MiB). The measured 135 is 41% of that, so only part of the copying
  changed path between the two offsets — the per-node phase model in
  `memcpy.c`'s comment is evidently too simple, and some nodes are mismatched at
  every offset. A routine that does not care about alignment collects all of it.
- The replacement is also ~30% faster than the generic routine in the aligned
  case (0.056 versus 0.074-0.085 ns/byte).
- **At MTU 9001 the MSS is 8961, so the destination phase advances one byte per
  segment and drifts through every value regardless of buffer address.** No
  application can align its way out of this. It has to be fixed in `memcpy`.

---

## 6. The accounting, stated honestly

At MTU 9001 and 4.9 Gbit/s, **19.0 µs of CPU per frame**:

| item | µs/frame | share | basis |
|---|---|---|---|
| `nettput` read thread | 8.80 | 46% | **measured** (per-thread, residual 0.01%) |
| ena reader thread | 5.08 | 27% | **measured** |
| ena consumer thread | 3.52 | 19% | **measured** |
| net timer thread | 1.56 | 8% | **measured** |
| everything else | 0.02 | 0.1% | **measured** |
| — of which per-byte | 16.63 | 88% | fit, R² 0.940 |
| — of which per-frame | 2.34 | 12% | fit, R² 0.940 |
| **named:** memcpy alignment penalty | 1.15 | **6%** | **measured in situ**, paired, *p* ≈ 0.006 |
| **named:** the two copies at aligned cost | ~1.55 | **8%** | benchmark × path structure |
| **named:** TCP/IP dispatch per frame | 1.48 | **8%** | the consumer's per-frame coefficient |
| **unnamed, but localised to the four threads above** | ~14.8 | **78%** | — |

**I account for 100% of the 19 µs to a named thread and 22% to a named
operation.** The honest headline is the first row, not the last: half the cost of
receiving is in the receiving application's syscall, and no previous analysis of
this path looked there.

### Ruled out, with numbers

| suspect | verdict | evidence |
|---|---|---|
| per-syscall / per-`read()` overhead | **not it** | 16 K → 1 M chunk: 64× fewer calls, same cost (§3.3) |
| `net_buffer` node count | **not a major driver** | 1.8σ in the fit; and halving nodes bought 1% (§3.1) |
| receive checksum in software | **already offloaded** | ENA sets `NET_BUFFER_L4_CHECKSUM_VALID` (`ena.cpp:2718`), TCP honours it (`tcp.cpp:702`) |
| interrupt / doorbell overhead | **bounded ≤0.1%** | residual 0.01-0.10%; no thread outside the four |
| `receive_lock` across protocol dispatch | **not a cost** | a blocked thread accrues no CPU time; it is a scalability limit, not a per-frame cost |
| raw memory bandwidth | **not it** | the same binary copies cold 64 MiB at 0.085 ns/byte |

### Facts settled in passing

These close `ena-multiqueue-plan.md` §7 step 0:

- **SMP works: 2 CPUs online** on `c7g.large` (`netprof` reports both, and both
  carry load).
- **The device grants 2 IO queue pairs**: `queue counts (ext v1): tx_sq 2 tx_cq 2
  rx_sq 2 rx_cq 2`.
- **The device allows 12 receive descriptors per packet**; the driver caps at
  `ENA_MAX_PACKET_DESCRIPTORS` = 8. Headroom exists there if it is ever wanted.

---

## 7. What to do, sized

### ~~Highest value: give arm64 a real `memcpy`. Worth ≥6%, small, system-wide.~~ DONE

> **MERGED 2026-08-24 (`f76217c69b`), and both the figure and the design below are
> wrong. Do not implement from this section — read `arm64-memcpy.md`.**
>
> - **Value:** 8.4% end-to-end across three boots, not ≥6.4%. (A single boot said
>   12.6%; that was revised **down**.)
> - **Design:** "align the destination, then 32 bytes per iteration" is **not what
>   shipped and was measured to be wrong.** What shipped: **no loop at all below
>   129 bytes**, align to **16** not 8, **64 bytes per iteration**. Getting the
>   small-copy and overlap-ordering cases right took four follow-up commits
>   (`4a132e2208`, `4728c15c96`, `f112082559`, `fee5dba7eb`, `0a0805c6ea`) — the
>   "one new 100-line file" estimate below was the most optimistic thing here.

`src/system/libroot/posix/string/arch/arm64/memcpy.c` on this branch: align the
destination, then 32 bytes per iteration with unaligned loads. Same shape as the
x86_64 file that already exists for the same reason.

- **Size:** one new 100-line file, two Jamfile lines.
- **Measured value:** ≥6.4% of receive CPU, a floor (§5). It applies to every
  copy in the system — file I/O, `app_server`, package extraction — not just to
  networking.
- **Risk:** *medium, and not because it is hard.* `memcpy` is the most
  load-bearing function in the tree. The routine is verified against `memcpy()`
  at all 64 alignment pairs over 105 sizes and both `libroot.so` and
  `kernel_lib_posix_arch_arm64.o` build clean with it, but it **has not executed
  inside a kernel or a libroot**. Run the string tests and boot before trusting
  it.
- One latent bug fixed alongside: the kernel's copy is now built `-fno-builtin`,
  as libroot's already was. Without it the compiler may recognise the byte loops
  as `memcpy` and call the function from inside itself; that the existing kernel
  build does not is luck.

### Then: make the profiler work, and stop guessing about the other 78%

`arch_debug_get_stack_trace()` is implemented on this branch (commit
`830d8d0814`), together with `-fno-omit-frame-pointer` for the arm64 kernel and
`arch_debug_get_interrupt_pc()`. With it, `profile -a -k` gives function-level
attribution of exactly the threads §2 names, and the two open questions —
what the other 78% is, and how much of the per-byte cost is really cache misses
(§4.1) — become one 6-second command instead of a research project.

- **Size:** ~150 lines in one arch file, 2 lines of build config. Already written.
- **Risk:** low and contained. It runs in timer-interrupt context, so the walker
  validates alignment and stack ownership and has a hard iteration cap rather
  than faulting or spinning on a corrupt frame chain.
- **Value:** it is the instrument, not a fix. Everything else on this list is
  currently sized by inference.

### Not worth doing on this evidence

- **Multi-queue receive.** Confirmed worthless on this instance: 2 vCPUs, 2 queue
  pairs, and the cost is per-byte anyway. Parallelism cannot reduce cost per byte.
- **Further `net_buffer` node reduction** (§3.1).
- ~~**Transmit checksum offload.** The receive side is already offloaded; transmit
  is not where the measured cost is.~~ **WRONG, and it was filed under "not worth
  doing". Merged 2026-08-24 and it paid: −3.54% of transmit CPU, p = 0.0079.** Two
  further corrections in the same area: the receive side is **not** "already
  offloaded" in the way implied — a false `NET_BUFFER_L3_CHECKSUM_VALID` claim had
  to be *removed* (`710f546d51`), and `rx_enabled` reads `0x0` while the device
  demonstrably validates L4 on ~98% of frames, so it is not a usable guard. Beware
  also an earlier **−12.6%** figure for this change: withdrawn as a confound (the
  send buffer was not pinned).

### Worth investigating next, in order

1. **The `nettput` read thread's other 5.3 µs/frame** — 28% of everything, in
   `BufferQueue::Get`'s `append_cloned` per node, five `user_memcpy` guard
   set-ups, and the clone teardown. Profiler first.
2. **`net timer` at 8% of receive and 0.001% of transmit.** A timer thread whose
   cost is proportional to throughput is odd on its face.
3. **The ena reader's other 4.1 µs/frame** — 22%, in `net_buffer` creation and
   descriptor handling.

---

## 8. What I want baked

Both are on `fix/arm64-net-profiling`:

| commit | what | why |
|---|---|---|
| `830d8d0814` | `arch_debug_get_stack_trace()` + frame pointers + `arch_debug_get_interrupt_pc()` | makes `profile -a -k` work; names the other 78% |
| `577dbc9895` | arm64 `memcpy.c` + two Jamfiles | the fix, ≥6% measured, ~~**needs the string tests and a boot**~~ — **there is no string test suite in this tree** (`arm64-memcpy.md` §3.1); one was written for this work as `c4fbf637c5`. Merged via `f76217c69b`; measured **8.4%** |

Also on the branch, needing no bake (they cross-build and `scp` onto a running
node): `src/bin/netprof`, `src/tests/system/benchmarks/memcpybench.c`, and
`nettput`'s `-A` / `-W`.

~~**If only one thing is baked, bake the profiler.**~~ **BOTH ARE BAKED AND MERGED
(2026-08-24): `830d8d0814` and `f76217c69b`.** This section is a spent request, not
a to-do list.

The reasoning was sound and its prediction resolved in an interesting direction:
the profiler *was* the higher-value bake, and it did settle the memcpy question —
**8.4%**, i.e. neither the ≥6% floor nor "considerably more". The 78% is still not
broken down, but nothing is blocking that now.

---

## 9. Reproducing this

From the metal builder, against a `c7g.large` booted
from the canonical AMI:

```bash
# per-thread partition during a transfer
scp netprof baron@$NODE:/boot/home/
ssh baron@$NODE "( nettput -c $PEER -p 5301 -n 8G -r > /boot/home/r.txt 2>&1 & )"
sleep 3
ssh baron@$NODE "ifconfig /dev/net/ena/0 | grep Receive: ; \
                 /boot/home/netprof -n 12 8 ; \
                 ifconfig /dev/net/ena/0 | grep Receive:"

# the memcpy question, both halves
ssh baron@$NODE "/boot/home/memcpybench"
ssh baron@$NODE "ifconfig /dev/net/ena/0 mtu 9000"
ssh baron@$NODE "nettput -c $PEER -p 5301 -n 4G -r -W -b 62720 -A 6"   # interleave
ssh baron@$NODE "nettput -c $PEER -p 5301 -n 4G -r -W -b 62720 -A 3"   # ten times
```

Method notes that cost something to learn:

- **Bracket the counters inside one remote shell.** An `ifconfig` in its own ssh
  call puts a third of a second of unmeasured traffic outside the window.
- **The netprof window must sit strictly inside the transfer.** Four MTU rows
  here have the receiving thread missing from the table because the transfer
  ended mid-window; the reconciliation residual recovers the number, but only
  because there was exactly one such thread.
- **Never let two runs overlap on one port.** A first attempt at the chunk sweep
  had each run still going when the next started; every row was wrong and none
  of them looked wrong.
- **Interleave A/B pairs and use a paired test.** Run-to-run cost is bimodal at
  roughly ±10% (~1940 and ~2140 µs/MiB), which swamps a 6% effect in an
  unpaired 8-point scan and produced a false negative in §5 before the pairing
  was added.
- **`c7g.large`, never a t-family type.** Cost per byte is the measurement;
  a throttled core corrupts precisely it.
