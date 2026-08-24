# Graviton / ARM64 optimization plan

Status as of **2026-08-24**: **partly delivered, partly closed on evidence, and
no longer a pure plan.**

> ~~Status: **planning** — every item below is verified against the tree at the
> state of branch `graviton`; nothing here has been implemented.~~
>
> **That line was true when written and is now false.** It survived several
> rounds of landed work and told at least one reader that finished items were
> open. Current state, per item, verified against `graviton` on 2026-08-24:
>
> | Item | State on 2026-08-24 | Evidence |
> |---|---|---|
> | 1 LSE atomics | **MERGED, hardware-verified** (via item 2) | `20bf8f2711` |
> | 2 `-mcpu`/`-march` | **MERGED** — `-mcpu=neoverse-n1+crypto`, `ArchitectureRules:52` | `20bf8f2711` |
> | 3 crc/crypto baseline | baseline **MERGED** with item 2; **intrinsic** crc32c/AES/SHA/PMULL still open | `20bf8f2711` |
> | 4 ENA LLQ | **ANSWERED on hardware** — the device *does* offer LLQ; see item 4 | `ena-tx-offload.md` |
> | 5 Multi-queue + RSS | **CANCELLED on evidence** — see item 5 | `ena-multiqueue-headroom.md` §5 |
> | 5a Doorbell/ack batching | **DEAD on evidence** — see item 5a | `219d8ab858` |
> | 6 Jumbo frames | **MERGED, hardware-verified** both directions | `21348b03b9` |
> | 7 Barriers & alignment | open (audit said "mostly correct") | — |
> | 8 Spinlock WFE/SEV | open — *not* closed by 8a | — |
> | 8a `arch_cpu_pause()` → `isb` | **MERGED** | `af7e48b94c` |
> | 9 Cacheline constants | **CLOSED — audit only, already correct** | — |
> | 10 16K/64K granules | open (research) | — |
> | 11 Interrupt moderation | **partly MERGED**; the structural fix is still owed — see item 11 | `aa8cbd0c8e` |
> | 12 Runtime feature detection | open | — |
> | 13 PMUv3 counters | **facility MERGED**; *nothing measured with it yet* | `af7e48b94c` |
> | 14 SMMU / IOMMU on metal | open (investigate) | — |
> | 15 Default socket send buffer | **MERGED, measured** | `ed4ea6413a` |
>
> ### Two reading rules for this document, and for `graviton/docs/` generally
>
> **1. "Merged" is not "measured."** Items **8a** and **13** merged in a single commit
> (`af7e48b94c`) whose own message ends *"Nothing has been measured; no numbers are
> claimed anywhere."* Item 2 was proven at the instruction level with **no performance
> number attached**. A ticked box here means the code is in the tree — it does **not**
> mean anyone has read a number off it. This distinction is the one these documents
> most often lost.
>
> **2. Distrust the forward-looking sections, not the measurements.** Every stale
> claim found in the 2026-08-24 sweep of `graviton/docs/` was in a section that looked
> *forward* — `## Open`, `## Next steps`, `## What I want baked`, `## Recommendation`,
> "Still open". The measurements, diagnoses and dead-hypothesis records held up almost
> without exception. When you pick up one of these files, trust what it says it
> *observed* and re-check everything it says it *intends*.
>
> Items below still carry their original "current state" prose because the
> *diagnosis* is the valuable part. Where an item has landed or been closed, a
> dated banner says so at the top of that item. **Trust the banners over the
> surrounding prose**, and trust neither over `git log graviton`.

Each claim cites `file:line` evidence against the tree as it was when the item
was written; line numbers drift. Where the source alone cannot settle a question
it is marked **unverified**.

## What and why

The `graviton` branch boots Haiku on AWS Graviton2/3/4 (arm64) EC2. The build
targets a **single, portable AMI** that must run across Graviton generations, so
"optimization" here means: pick the best baseline the whole fleet shares
(Neoverse-N1, ARMv8.2), ~~lean on runtime feature detection where a generation
differs~~, and close the driver gaps that leave throughput on the floor. The two
hot spots are the arm64 kernel (`src/system/kernel/arch/arm64/`) and the ENA
network driver (`src/add-ons/kernel/drivers/network/ether/ena/`), plus the
userland build flags that gate both.

> **Correction (2026-08-22).** The struck-out clause was wrong. Haiku has **no
> runtime CPU-feature-detection mechanism on arm64 at all** — `grep -rn AT_HWCAP
> headers src` and `grep -rn getauxval .` both return zero hits, and nothing reads
> `ID_AA64ISAR0_EL1` (only the mask macros exist, `arm_registers.h:215-216`). So
> "lean on runtime detection" is not an available strategy; every generation
> difference must be resolved at **compile time** by the baseline in item 2. See
> the new item 12.

The headline finding, ~~as of writing~~ **FIXED and merged 2026-08-24
(`20bf8f2711`)**: **arm64 atomics were LL/SC, not LSE**, purely because the
compiler baseline was `-march=armv8-a+crc` (ARMv8.0). Fixing that one line (item 2)
turned every atomic in the kernel and libroot — including the socket refcount CAS
and the locks we had been hardening — into single LSE instructions. It was the
highest-leverage, lowest-risk change on the list, and it is done: the baseline is
now `-mcpu=neoverse-n1+crypto` at `ArchitectureRules:52`.

> **The part of this that was more interesting than the fix**, and the reason to
> read item 2 rather than just noting it closed: the real defect was worse than
> "no LSE". GCC 13.3 defaults to `-moutline-atomics`, and the kernel's
> `__aarch64_have_lse_atomics` flag is a `.bss` symbol with no constructor — hence
> permanently zero. So every atomic paid an **outline call *and* took the LL/SC
> path**. Verified at the instruction level: kernel outline calls 1173 → 4, inline
> LSE 10 → 1179, `ldapr` 0 → 168. **No performance number was ever attached** — see
> item 13.

## Priority table

| # | Item | Category | Current state (evidence) | Recommended change | Effort | Risk | Benefit |
|---|------|----------|--------------------------|--------------------|--------|------|---------|
| 1 | LSE atomics | compiler | **MERGED** `20bf8f2711` — LL/SC. Atomics are `__atomic`/`std::atomic` builtins (`SupportDefs.h:319`, `generic_atomic.cpp:12`); no LSE because baseline is ARMv8.0 | Gated by item 2 — arch baseline enables LSE | — | — | High (every lock/refcount) |
| 2 | `-mcpu`/`-march` | build config | **MERGED** `20bf8f2711` — `-march=armv8-a+crc` (`ArchitectureRules:41`); no `-mcpu`, no LSE, no crypto | `-mcpu=neoverse-n1` (or `armv8.2-a` + `-moutline-atomics`) | S | Low | High |
| 3 | CRC32 / AES / SHA / PMULL | build config | baseline **MERGED** `20bf8f2711`; intrinsics still open — `+crc` present (`ArchitectureRules:41`); no `+crypto`. crc32c is a software table (`shared/crc32.cpp`) | Fold into item 2 baseline; intrinsic crc32c is a separate follow-up | S–M | Low | Low–Med (disk fs only) |
| 4 | ENA LLQ | driver | **ANSWERED on hardware, device offers LLQ** — Full negotiation exists (`ena.cpp:504-562`); "not support" branch is device-driven (`ena.cpp:510`). BAR2 is mapped + WC (`ena.cpp:1649-1660`) | Confirm whether device advertises LLQ per instance type; likely instance/feature gap, not driver gap | S (investigate) | Low | Med (tx latency) |
| 5 | Multi-queue + RSS | driver | **CANCELLED on evidence 2026-08-24, do not start** — One pair, `ENA_MSIX_VECTOR_COUNT=2` (`ena.cpp:53`); RSS table built but "buys us nothing" with one queue (`ena.cpp:682`); single-pair struct (`ena.h:210-236`); `TODO` at `ena.cpp:844` | Per-vCPU IO queues, one MSI-X vector + ring each, RSS spread | L | Med | **High** (throughput) |
| 6 | Jumbo frames | driver | **MERGED, hardware-verified** `21348b03b9` — MTU pinned to 1500 (`ena.h:89`, `ena.cpp:1553`); device limit read but unused (`ena.cpp:1457`); needs multi-descriptor RX | Multi-descriptor RX, then raise device MTU | M | Med | Med (in-VPC) |
| 7 | Barriers & alignment | driver | Already conservative: `wmb/rmb/mb = dsb sy` (`ena_plat.h:374-386`); ena-com structs `aligned(64)` (`ena_plat.h:123`) | Mostly correct; optional: relax over-strong barriers, align per-queue structs when item 5 lands | S | Med | Low |
| 8 | Spinlock WFE/SEV | kernel | Busy-`yield`: `cpu_wait`→`arch_cpu_pause`→`arm64_yield` (`arch_cpu.h:140-143`, `cpu.cpp:355-360`). `arm64_wfe`/`arm64_sev` defined but unused (`arch_cpu.h:23-24`) | WFE/SEV monitored wait in the spin loop | M | Med | Med (contention/power) |
| 9 | Cacheline constants | kernel | **CLOSED, audit only** — `CACHE_LINE_SIZE 64` for arm64 (`arch_cpu.h:10`); runtime CTR_EL0 read is correct (`arch_cpu.cpp:87-91`) | No change — verified correct for Graviton | — | — | None (audit) |
| 10 | Larger granules (16K/64K) | kernel/MMU | Map is parameterized by `fPageBits` but instantiated at **4K** (`arch_vm_translation_map.cpp:42`, `pageBits=12`); `B_PAGE_SIZE`/`max-page-size=0x1000` everywhere | RESEARCH only — global `B_PAGE_SIZE` change; **new 10a**: contiguous-bit/block mappings at 4K instead | L | High | Med (TLB) |
| 11 | ENA interrupt moderation | driver | **partly MERGED** `aa8cbd0c8e`, structural fix still owed — `ena_com_init_interrupt_moderation()` is called (`ena.cpp:1501`) but **no interval is ever set and adaptive moderation is never enabled** — no other call site in `ena.cpp` | Set a non-adaptive RX/TX interval, or enable adaptive; AWS warns Graviton's faster packet processing *raises* the interrupt rate | S | Low | Med (irq load) |
| 12 | Runtime feature detection | kernel/libroot | **Absent entirely.** No `AT_HWCAP`, no `getauxval`, no `ID_AA64ISAR0_EL1` reader, no MRS trap emulation (`grep` over `headers/`+`src/`) | Kernel-published HWCAP word + libroot accessor; and/or force `-mno-outline-atomics` into recipe CFLAGS | M | Low | Med (unblocks userland LSE + crypto) |
| 13 | PMU (PMUv3) counters | kernel | **facility MERGED** `af7e48b94c`, nothing measured with it yet — **No PMU code at all** for arm64 (`grep -i pmcr_el0\|pmevcntr src headers` → zero) | Read-only counter facility so AWS's runbook ratios (`ipc`, `stall_*_pkc`, `*-mpki`, `data-tlb-tw-pki`) can be measured on Haiku | M | Low | **High** (unblocks every measurement below) |
| 14 | SMMU / IOMMU on metal | kernel/platform | Unaudited. c7g.metal exposes an SMMU; virtualized instances do not | Determine whether the SMMU is on and translating for ENA DMA; AWS reports turning it off "speed[s] up IO handling" on metal | S (investigate) | Med | Med (metal IO only) |

## Sequencing

> **Status (2026-08-22, re-checked 2026-08-24): step 1 is DONE and hardware-verified.**
> Items 2, 1 and 3 landed as `-mcpu=neoverse-n1+crypto` — merged as **`20bf8f2711`
> "arm64: build with -mcpu=neoverse-n1+crypto"**, now `ArchitectureRules:52` — and item 9
> was audited as already correct.
>
> > **Pointer corrected 2026-08-24.** This used to read "(`graviton-mcpu-neoverse.patch`,
> > uncommitted)". That file is **not tracked on `graviton`**; it is a leftover in a
> > checkout parked behind the branch, so a reader following the pointer finds nothing
> > and may conclude the work was lost. **Cite the merged commit, never a root-level
> > `graviton-*.patch`.** The same correction applies anywhere else in `graviton/docs/`.
>
> The premise below was wrong in an important way — GCC
> 13.3 defaults to `-moutline-atomics` and the kernel's `__aarch64_have_lse_atomics` flag
> is a `.bss` symbol with no constructor, hence permanently zero, so atomics paid a call
> **and** took LL/SC. Measured: kernel outline calls 1173 → 4, inline LSE 10 → 1179,
> `ldapr` 0 → 168, binaries slightly smaller, clean boot on real Neoverse cores. Still
> open: an actual performance measurement. Items 4-8 and 10 remain, gated on having a
> userland that can generate real load. Project-wide ordering now lives in
> [sequencing.md](sequencing.md).

1. **Item 2 (`-mcpu=neoverse-n1`) first.** One line in `ArchitectureRules`. It
   subsumes item 1 (LSE atomics) and item 3 (crc/crypto baseline), and it
   benefits the entire kernel and libroot — including the socket-refcount CAS
   and the spinlock/refcount paths we have been hardening. Rebuild + boot-test
   on Graviton; it is a portable Neoverse-N1 baseline, so a single AMI still
   runs across Graviton2/3/4. This is the cheapest, broadest win.
2. ~~**Item 5 (ENA multi-queue + RSS)** next — the real throughput lever, but a
   large driver refactor (per-queue structs, N MSI-X vectors, RSS spread).~~
   **CANCELLED on evidence, 2026-08-24. Do not start this.** It was never the
   lever this line calls it; see item 5 below for the deciding control.
3. ~~**Item 4 (LLQ investigation)** alongside item 5: confirm on hardware whether
   the device advertises LLQ; it changes the tx-placement path multi-queue uses.~~
   **ANSWERED on hardware, 2026-08-24.** The device does offer LLQ. See item 4.
4. ~~Then items 6 (jumbo), 8 (WFE/SEV), 7 (barrier tuning) as independent
   follow-ups.~~ **Item 6 (jumbo) is MERGED and hardware-verified** (`21348b03b9`).
   Items 8 (WFE/SEV) and 7 (barrier tuning) remain open — note **8a landed
   (`af7e48b94c`) and does *not* close 8**; `isb` is backoff, it does not stop the
   contended line being snooped. Item 10 is a research spike, not a scheduled
   change. Item 9 is already correct (audit only).
5. **Added 2026-08-22, from the AWS cross-check below.** Two of the new items are
   cheaper than anything remaining on the original list and are not gated on a
   userland workload: **item 8a** (`arch_cpu_pause()` → `isb` instead of `yield`)
   is a one-token edit that AWS recommends outright, and **item 11** (ENA
   interrupt moderation) is a few lines. **Item 13** (PMU counters) should be
   pulled *forward* — it is the only thing that converts the rest of this
   document from inference into measurement, and it is what makes item 10's
   research spike answerable rather than open-ended. **Item 12** belongs with
   Phase 2/4 in [sequencing.md](sequencing.md), not here, because it gates the
   ~150-package userland rather than the kernel.

   > **Outcome (2026-08-24).** All three of those calls were acted on. **Item 8a
   > merged** (`af7e48b94c`) and **item 11 partly merged** (`aa8cbd0c8e`). **Item 13
   > merged as a facility in the same commit as 8a** (`af7e48b94c`,
   > `src/system/kernel/arch/arm64/arch_pmu.cpp`, off unless the `arm64_pmu` boot
   > setting or KDL `pmu on` asks for it).
   >
   > **But the argument for pulling item 13 forward has not yet paid off, and it is
   > important not to read "merged" as "measured".** The commit message says so in
   > terms: *"Nothing has been measured; no numbers are claimed anywhere."* Every
   > "measure" line in this document is therefore still owed a number — the
   > instrument now exists, the readings do not. A reader who sees item 13 ticked
   > off and assumes the ratios in its table have been collected will be wrong.

6. **What is actually next, as of 2026-08-24.** The open kernel/driver items on
   this list are 3 (intrinsics, not the baseline), 7, 8 (WFE/SEV proper), 10, 12
   and 14, plus **using** item 13's facility. The live network bottleneck is *not*
   on this list under its own number: it is **ENA interrupt cadence**, the
   `XXX STRUCTURAL FIX STILL OWED` at `ena.cpp:221`, tracked under item 11 below.

---

## Cross-check against AWS's own Graviton guidance (2026-08-22)

Reviewed AWS's public `aws-graviton-getting-started` repo against this plan.
This section records what the guide says, what it validates, what it changes, and
— importantly — what it does **not** transfer to an OS kernel port. Everything
below is AWS's claim unless labelled as our finding; **no measurement in this
section is ours**.

**Sources** (all `https://github.com/aws/aws-graviton-getting-started`, `main`):

| Cite | Page |
|---|---|
| `c-c++.md` | [c-c++.md](https://github.com/aws/aws-graviton-getting-started/blob/main/c-c%2B%2B.md) — the `-mcpu` table, LSE |
| `optimizing.md` | [optimizing.md](https://github.com/aws/aws-graviton-getting-started/blob/main/optimizing.md) — memory model, LSE, profiling, interrupt rate |
| `README.md` | [README.md](https://github.com/aws/aws-graviton-getting-started/blob/main/README.md) — the generation → core → ISA → SVE table |
| `os.md` | [os.md](https://github.com/aws/aws-graviton-getting-started/blob/main/os.md) — per-distro kernel page size, LSE support |
| `linux_kernel.md` | [linux_kernel.md](https://github.com/aws/aws-graviton-getting-started/blob/main/linux_kernel.md) — SMMU on metal, preemption |
| `runtime-feature-detection.md` | [runtime-feature-detection.md](https://github.com/aws/aws-graviton-getting-started/blob/main/runtime-feature-detection.md) — HWCAP, SVE needs kernel support |
| `arm64-assembly-optimization.md` | [arm64-assembly-optimization.md](https://github.com/aws/aws-graviton-getting-started/blob/main/arm64-assembly-optimization.md) |
| `SIMD_and_vectorization.md` | [SIMD_and_vectorization.md](https://github.com/aws/aws-graviton-getting-started/blob/main/SIMD_and_vectorization.md) |
| `dpdk_spdk.md` | [dpdk_spdk.md](https://github.com/aws/aws-graviton-getting-started/blob/main/dpdk_spdk.md) |
| `perfrunbook/*` | [perfrunbook/](https://github.com/aws/aws-graviton-getting-started/tree/main/perfrunbook) — `README.md`, `intro_to_benchmarking.md`, `configuring_your_sut.md`, `debug_system_perf.md`, `debug_code_perf.md`, `debug_hw_perf.md`, `optimization_recommendation.md` |

Also consulted, for flag semantics rather than AWS guidance: the
[GCC 13.3.0 AArch64 options manual](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/AArch64-Options.html).

### The `-mcpu` question, settled: keep `-mcpu=neoverse-n1+crypto`

AWS's table (`c-c++.md`) gives a "performance" and a "balanced" flag per
generation:

| CPU | Flag (performance) | Flag (balanced) | GCC |
|---|---|---|---|
| Graviton2 | `-mcpu=neoverse-n1` | `-march=armv8.2-a` | GCC-9 |
| Graviton3(E) | `-mcpu=neoverse-v1` | `-mcpu=neoverse-512tvb` | GCC 11 |
| Graviton4 | `-mcpu=neoverse-v2` | `-mcpu=neoverse-512tvb` | GCC 13 |
| Graviton5 | `-mcpu=neoverse-v3` | `-mcpu=neoverse-512tvb` | GCC 15 |

The decisive rule is the one attached to that table: when targeting several
generations, use **the balanced flag of the *oldest* deployed generation**,
"since code built for a newer generation may not run on an older generation."
Our fleet may include Graviton2, so the oldest generation is Graviton2 and the
prescribed flag is `-march=armv8.2-a`. AWS's TLDR says the same thing outright:
"To target all current generation Graviton instances (Graviton2, Graviton3,
Graviton4, and Graviton5), use `-march=armv8.2-a`", and notes that flag "enables
all instructions supported by Graviton2, including LSE".

`-mcpu=neoverse-512tvb` is therefore **not** the balanced choice for us — it is
the balanced choice for a fleet whose *oldest* member is Graviton3.

**What `512tvb` actually means.** Per the GCC 13.3 manual it "does not refer to a
specific core" and covers "all Neoverse cores that (a) implement SVE and (b) have
a total vector bandwidth of 512 bits a cycle". Two halves:

- *ISA half:* "Unless overridden by `-march`, `-mcpu=neoverse-512tvb` generates
  code that can run on a Neoverse V1 core" — i.e. it raises the floor to
  Neoverse-V1, **including SVE**. AWS's own generation table (`README.md`) lists
  Graviton2's SVE features as none; SVE first appears on Graviton3.
- *Tuning half:* `-mtune=neoverse-512tvb` targets cores that can issue "4 128-bit
  Advanced SIMD arithmetic instructions a cycle" and the SVE equivalent ("2 for
  256-bit SVE, 4 for 128-bit SVE"). It is **vector-bandwidth** tuning.

**Why that disqualifies it here, twice over.**

1. *Fleet portability.* The kernel is built once and must boot on every instance
   type we deploy to. A bare `-mcpu=neoverse-512tvb` build may emit SVE and
   ARMv8.4 instructions and would fault on Graviton2 (Neoverse-N1). Our
   single-AMI requirement forbids it.
2. *We have no SVE support to fault into.* **Our finding, verified in-tree:** the
   arm64 port touches `CPACR_EL1` only for FP/SIMD (`CPACR_FPEN_TRAP_NONE`,
   `src/system/boot/platform/efi/arch/arm64/arch_start.cpp:61`); there is no
   `ZEN` bit, no `ZCR_EL1`, and no SVE register save/restore anywhere in
   `src/system/kernel/arch/arm64/`. SVE is therefore trapped and its state is not
   context-switched. AWS makes exactly this point from the other direction in
   `runtime-feature-detection.md`: reading ID registers directly "obscures the
   fact that the kernel must also be configured for SVE support", without which a
   context switch "could result in corruption of the content of SVE registers",
   and "SVE instructions are trapped by default until the kernel disables the
   trap." **We are that kernel, and we have not disabled the trap.** Any flag
   implying SVE — `neoverse-512tvb`, `neoverse-v1`, `neoverse-v2`, `neoverse-v3`
   — is unsafe for this port today regardless of which instance it runs on.

Separately, the tuning half buys a kernel next to nothing: the Haiku kernel's hot
paths are scalar (locks, refcounts, page-table walks, descriptor rings), so
retuning for 512-bit-per-cycle vector issue optimizes code we do not emit, while
de-tuning scalar scheduling for the Neoverse-N1 members of the fleet.

**Does GCC 13.3 support `neoverse-512tvb`?** Yes — it is listed among the valid
`-mcpu`/`-mtune` values in the GCC 13.3.0 manual, alongside `neoverse-n1`,
`neoverse-v1`, `neoverse-v2`. Support is not the blocker; portability is.

**Recommendation: no change. `-mcpu=neoverse-n1+crypto`
(`ArchitectureRules:52`) stands, and AWS's own documents confirm it twice:**

- It *is* AWS's Graviton2 "performance" flag, and AWS notes that on arm64
  "`-mcpu=` acts as both specifying the appropriate architecture and tuning".
- It is a strict superset of AWS's prescribed fleet-wide baseline
  `-march=armv8.2-a`: same ISA level, plus the features `README.md` lists as
  actually present on Graviton2 (fp16, rcpc, dotprod) and N1 scheduling.
- `perfrunbook/configuring_your_sut.md` recommends, verbatim, "`-march=armv8.2-a
  -mcpu=neoverse-n1` for Graviton2-and-later" — our line modulo the redundant
  `-march` (redundant because `-mcpu=neoverse-n1` already selects ARMv8.2-a).

**`+crypto` is validated as both necessary and safe.** The GCC 13.3 manual lists
only `fp` and `simd` as on by default across `-march`/`-mcpu` values; `crypto`
never appears in the defaults, so it must be requested explicitly — which is why
`-mcpu=neoverse-n1` alone would *not* have given us AES/SHA/PMULL. AWS's
`README.md` lists crypto as present on Graviton2 through Graviton5. The comment
already in `ArchitectureRules:49-51` says exactly this and is correct.

**If the fleet ever drops Graviton2**, the right move becomes
`-mcpu=neoverse-512tvb+crypto` — but *only after* SVE context-switch support
exists in the kernel (see reason 2 above). Until then it is blocked on kernel
work, not on a build-config decision. An intermediate that is safe today but of
unproven value: `-mcpu=neoverse-n1+crypto -mtune=neoverse-512tvb`, keeping the N1
ISA floor while tuning schedules for V1/V2. Expected effect on a scalar kernel is
~nil; **unverified, and not recommended without measurement.**

### What AWS validates about our LSE / outline-atomics fix

Everything, and the GCC manual explains the mechanism we found:

- AWS's magnitude claim, for calibration only: LSE "can improve system throughput
  for CPU-to-CPU communication, locks, and mutexes", and "The improvement can be
  up to an order of magnitude when using LSE instead of load/store exclusives"
  (`c-c++.md`); `optimizing.md` repeats it as "an order of magnitude faster for
  highly contended locks with high core counts". **This is AWS's number about
  AWS's workloads. It is not a Haiku measurement and must not be quoted as one.**
  It does tell us the *shape* of the expected result — the delta should grow with
  core count — which is directly usable as an experiment design (see below).
- AWS's prescribed verification method is exactly what we already did:
  `objdump -d` the binary and count LSE mnemonics (`cas`, `casp`, `swp`, `ldadd`,
  `stadd`, `ldclr`, `ldeor`, `ldset`, `ldsmax`/`ldsmin`, `ldumax`/`ldumin`)
  against exclusives (`objdump -d app | grep -i 'ldxr\|ldaxr\|stxr\|stlxr' | wc
  -l`). Our instruction-level verification is therefore complete by AWS's own
  standard; only the throughput number is missing.
- **The GCC manual explains our bug precisely.** On `-moutline-atomics`: "This
  option is only applicable when compiling for the base ARMv8.0 instruction set,"
  and with a newer revision or "when using `-mcpu=` when the selected cpu
  supports the `lse` feature ... the ARMv8.1-Atomics instructions will be used
  directly. This option is on by default." Our old baseline was
  `-march=armv8-a+crc` = base ARMv8.0, so outline atomics were both *applicable*
  and *on by default* — hence 1173 helper calls. The new baseline supports `lse`,
  so the helpers went inert. **Consequence: `-mno-outline-atomics` is redundant
  in `ArchitectureRules` and we should not add it there** (adding it would imply
  the flag was the fix, when the arch baseline was).
- **What does NOT transfer, and why AWS could never have told us:** AWS never
  mentions `__aarch64_have_lse_atomics` at all, and its only remedy for a
  non-LSE runtime is a *distro package* — "Ubuntu 18.04 (needs `apt install
  libc6-lse`)" — plus an `os.md` per-distro "LSE Support" column. That entire
  framing presumes a glibc that ships the outline helpers *and* an initialiser
  which sets the flag from `AT_HWCAP`. We are freestanding: the kernel has no
  libc, and Haiku has no HWCAP mechanism at all (item 12). A permanently-zero
  `__aarch64_have_lse_atomics` is a defect class that exists only outside
  glibc-land. **Our finding is more specific than AWS's guidance, not contradicted
  by it.**

### Methodology we can adopt for the open measurement gap

> **Still current as of 2026-08-24, with three pointers updated.** The methodology
> here has not been superseded; what changed is that the PMU *facility* now exists
> (item 13, `af7e48b94c`) while the *readings* still do not, so "the open
> measurement gap" is now a gap in effort rather than in capability. Two references
> below point at **item 5, which was cancelled** — read those as pointing at
> **item 11** (interrupt cadence), which is where the network question now lives.

Our one acknowledged gap is that the atomics change is proven at the instruction
level with no performance number. AWS's `perfrunbook` prescribes a methodology;
here is the honest split.

**Transfers, needs no Linux tooling:**

- *Discipline.* "Always define a specific question to answer with your benchmark"
  and "Control your variables and unknowns within the benchmark environment"
  (`intro_to_benchmarking.md`), with an enumerated variable list (OS version,
  kernel, dependency versions, instance size, placement group, background
  daemons, traffic profile, load generator). Our A/B is unusually clean: **the
  same instance, the same image, built two ways** — one variable.
- *Environment control from `configuring_your_sut.md`:* cluster placement group;
  "A difference of +/-50us is acceptable, differences of >+/-100us can adversely
  affect testing results" for RTT to a load generator; start on **dedicated
  tenancy** to characterise variance first; over-provision disk and network so
  "experiments ... test only the capability of the CPU and DRAM".
- *The core-count sweep.* AWS scales an instance down to emulate a smaller one
  (`configure_vcpus.sh <# vcpus> cores`, `configure_mem_size.sh`). Our equivalent
  is booting the same image with the CPU count capped, then showing the LSE delta
  **grow** with core count — which is the shape AWS's "high core counts" claim
  predicts. That makes the result falsifiable rather than a single number, and
  needs nothing but a boot argument.
- *`vCPU` counting.* "Graviton processors do not implement SMT (Hyper-Threading),
  so vCPUs map 1:1 to physical cores" (`configuring_your_sut.md`); dpdk_spdk.md:
  "in Graviton, every vCPU is a full CPU". No halving anywhere
  (~~relevant to item 5's queue count~~ — item 5 is cancelled; this survives only as a
  general rule: never halve a worker count on Graviton out of x86 habit).
- *CloudWatch network allowances,* which are read from **outside** the guest and
  so work fine against Haiku: `bw_in_allowance_exceeded`,
  `bw_out_allowance_exceeded`, `conntrack_allowance_exceeded`,
  `linklocal_allowance_exceeded`, `pps_allowance_exceeded`
  (`debug_system_perf.md`). These distinguish "our driver is slow" from "we hit
  an instance allowance" — genuinely useful, and now aimed at **item 11** rather than
  the cancelled item 5. Note the related hazard recorded elsewhere in
  `graviton/docs/`: burst credits and per-flow caps make EC2 throughput numbers
  drift between boots, so a single run is not a measurement.
- *The PMU itself.* See item 13. PMUv3 is architectural and readable at EL1;
  `debug_hw_perf.md`'s full ratio set and thresholds are reproducible on Haiku
  ~~once counters exist~~ — **the counters now exist** (`af7e48b94c`, gated behind the
  `arm64_pmu` boot setting or KDL `pmu on`). AWS lists `*7g` as having full PMU
  support at 16xlarge/metal — **`c7g.metal` qualifies, and as of `f5367b3602` it also
  boots to userland**, so this is executable now. **No ratio in that table has been
  collected yet.**

**Does NOT transfer (Linux-only, absent on Haiku):** APerf; `perf` in every form
(`perf stat`, `perf record`, `perf report`, `perf script`, `perf c2c`); the
FlameGraph scripts (`stackcollapse-perf.pl`, `difffolded.pl`); SPE (`arm_spe_0`,
needs a driver plus `linux-modules-extra`); the CMN uncore PMU (`arm_cmn_0`,
`hnf_mc_reqs`, `rnid_rxdat_flits`/`rnid_txdat_flits`); `sysstat`/`sar -n DEV`;
`htop`; `ethtool -c/-C`; `irqbalance`; `/proc/interrupts` and
`/proc/irq/*/smp_affinity_list`; `sysctl kernel.perf_event_paranoid` and
`kernel.kptr_restrict`; `CONFIG_ARM64_PSEUDO_NMI` + `irqchip.gicv3_pseudo_nmi=1`;
every script under `perfrunbook/utilities` (bash + Python on AL2023/Ubuntu); the
THP sysfs tree and `vm.nr_hugepages`; `libc6-lse` and the per-distro matrices;
and all the JVM material (JFR/JMC, `libperf-jvmti.so`, `-XX:` flags).

**Two AWS caveats that are traps for us specifically:**

1. "No synthetic benchmark is a substitute for your actual production code"
   (`perfrunbook/README.md`). We have no production workload — which is precisely
   the gate [sequencing.md](sequencing.md) puts on Phase 7, arrived at
   independently. For the *atomics* change a microbenchmark is still defensible,
   because the change is a pure instruction substitution on a primitive we know
   is hot; but it must be labelled a microbenchmark, not a throughput result.
2. Cycle-based sampling "will under-count idle time" because "Graviton idles in a
   clock-gated sleep state, so counters stop ticking" (`debug_code_perf.md`).
   **This is a direct measurement hazard for item 8:** if a WFE backoff parks
   cores, cycle counters stop, and any cycles-based comparison will flatter the
   change. Item 8 must be measured in wall-clock throughput, not cycles.
3. Also from `debug_code_perf.md`, a design note for item 13: profiles taken
   inside interrupt-masked kernel code get misattributed to whatever routine
   *unmasks* interrupts (their example is `arch_local_irq_restore`), which is why
   AWS reaches for pseudo-NMI. Any Haiku sampling profiler must overflow into an
   unmaskable context, not a timer we have masked.

### Where AWS says nothing, so our plan stands unaided

Called out so nobody mistakes silence for agreement:

- **ENA LLQ (item 4), MTU/jumbo frames (item 6), ENA queue counts.** Nothing in
  the repo — not in `optimizing.md`, `optimization_recommendation.md`, or
  `dpdk_spdk.md`, which mentions neither the ENA PMD version nor RSS nor
  hugepage sizing. Items 4 and 6 get no help; investigate on hardware as planned.
- **WFE/SEV (item 8).** Not mentioned anywhere in the repo, including
  `arm64-assembly-optimization.md`, which covers no synchronization primitives at
  all (no `ldxr`/`stxr`, no `dmb`/`dsb`/`isb` scopes, no `wfe`/`sev`, no
  `ldapr`/`ldar`/`stlr`). Our WFE/SEV design is neither validated nor
  contradicted.
- **Barrier scope tuning (item 7).** `optimizing.md` establishes only the model —
  "Arm is weakly ordered", Armv8 is "weakly ordered multi-copy-atomic", "Code
  that relies on TSO may lack barriers to properly order memory references" — and
  warns that bespoke lockless code "will have to use the proper intrinsics and
  barriers to correctly order memory transactions." It offers **no** `dsb sy` vs
  `dmb ish` guidance. **Do not weaken ena-com's `dsb sy` on the strength of
  AWS's generic advice; there is none to weaken it on.**
- **False sharing / cache-line layout.** No guidance in `optimizing.md` or
  `optimization_recommendation.md`. Their only *tooling* answers are SPE ("false
  sharing of atomic variables") and `perf c2c` ("cache coherence issues and false
  sharing in multi-core systems"), both Linux-and-metal-only — so item 7's
  per-queue alignment work cannot be validated with AWS's tools on Haiku.

---

## 1. LSE atomics (ARMv8.1) — MERGED 2026-08-24 (`20bf8f2711`), via item 2

> **Done.** The "Current state" below describes the tree *before* `20bf8f2711`; the
> baseline is now `-mcpu=neoverse-n1+crypto`, so these builtins compile to single LSE
> instructions. Verified at the instruction level, **not** by a performance
> measurement (see item 13). Retained for the diagnosis.

**Current state.** arm64 has no hand-written atomic assembly. The primitives are
compiler builtins:

- Kernel + inline callers use the GCC `__atomic_*` builtins in
  `headers/os/support/SupportDefs.h` — e.g. `atomic_test_and_set64` →
  `__atomic_compare_exchange_n(..., __ATOMIC_SEQ_CST, ...)`
  (`SupportDefs.h:326-332`), `atomic_get_and_set64` → `__atomic_exchange_n`
  (`SupportDefs.h:319-323`), `atomic_add` → `__atomic_fetch_add`
  (`SupportDefs.h`, 32-bit block just above).
- libroot userland uses C++ `std::atomic` in
  `src/system/libroot/os/arch/generic/generic_atomic.cpp` —
  `compare_exchange_strong` (`:29`), `exchange` (`:21`), `fetch_add` (`:37`).
  The arm64 libroot Jamfile pulls exactly this file in
  (`src/system/libroot/os/arch/arm64/Jamfile:13,23`).
- `headers/private/kernel/arch/arm64/arch_atomic.h` supplies **only barriers**
  (`dmb ishld` / `dsb ishst` / `dsb sy`, `:9-27`), no atomic ops. The kernel
  arm64 Jamfile has no atomic source file, confirming the builtins are the whole
  story (`src/system/kernel/arch/arm64/Jamfile`).

Both builtin families lower to **LL/SC retry loops** (`ldaxr`/`stlxr`) rather
than **LSE** single instructions (`casal`/`ldaddal`/`swpal`) unless the compiler
is told the target has LSE. Today it is not (see item 2), so they are LL/SC.

**Why it matters.** Every kernel lock acquire/release, every refcount, the
socket-refcount CAS, `try_acquire_spinlock`'s `atomic_get_and_set`
(`smp.cpp:267,310`), and the PTE CAS loops in
`VMSAv8TranslationMap.cpp:381,435,862,900,913,952` are these builtins. Under
contention an LL/SC loop can livelock-retry; LSE `cas`/`ldadd` is a single
bounded instruction and scales far better on many-core Graviton.

**Recommendation.** No code change here — LSE is unlocked entirely by the arch
baseline in item 2. After changing the baseline, spot-check the emitted code
(see "Verify" below) to confirm `cas`/`ldadd`/`swp` replace `ldxr`/`stxr`.

**Verify.** Disassemble a rebuilt object and grep the opcodes:

```bash
aarch64-unknown-haiku-objdump -d \
  generated.arm64/objects/haiku/arm64/release/system/libroot/.../generic_atomic.o \
  | grep -E 'casal|ldadd|swp|ldaxr|stlxr'
```

LL/SC today = `ldaxr`/`stlxr`; success = `casal`/`ldaddal`/`swpal`.

**Open questions.** With `-moutline-atomics` (item 2 alternative) the ops become
calls to `__aarch64_cas*` helpers that pick LSE-vs-LL/SC at runtime; that keeps
one portable binary but adds a branch per atomic. Decide LSE-hard
(`-mcpu=neoverse-n1`) vs outline-atomics in item 2.

**AWS guide (2026-08-22).** Resolved, in our favour and worse than the "adds a
branch per atomic" framing above: the helpers never selected LSE at all, because
nothing initialised `__aarch64_have_lse_atomics` (see Phase 1 in
[sequencing.md](sequencing.md)). The GCC 13.3 manual confirms the mechanism —
`-moutline-atomics` "is only applicable when compiling for the base ARMv8.0
instruction set" and "is on by default", which is exactly the configuration the
old `-march=armv8-a+crc` baseline created. AWS's verification recipe for this is
byte-for-byte the `objdump`-and-count method already used here. AWS's own
expectation of the payoff — "up to an order of magnitude" on contended locks at
high core counts — is **AWS's number, not ours**; see the cross-check section for
how to turn it into a falsifiable experiment.

## 2. `-mcpu` / `-march` targeting — MERGED 2026-08-24 (`20bf8f2711`)

> **Done: `case arm64 : archFlags += -mcpu=neoverse-n1+crypto ;`, now
> `ArchitectureRules:52`.** Clean boot on real Neoverse cores. The "Current state"
> below describes the pre-fix `-march=armv8-a+crc`. See also the settled `-mcpu`
> discussion above, which is the reasoning behind `neoverse-n1` rather than a newer
> core: a single portable AMI has to run across Graviton generations.

**Current state.** The single place arm64 arch flags are set:

```
build/jam/ArchitectureRules:41
    case arm64 : archFlags += -march=armv8-a+crc ;
```

`$(archFlags)` flows into `HAIKU_CCFLAGS`, `HAIKU_C++FLAGS`, `HAIKU_LINKFLAGS`
and `HAIKU_ASFLAGS` for the architecture (`ArchitectureRules:51,62-65`), so this
one case governs the whole arm64 build. There is **no** `-mcpu`, no `-march`
newer than `armv8-a` (= ARMv8.0), no `-moutline-atomics`, and no `+lse`/`+crypto`
anywhere in `build/jam/` or `configure` (grep returned only line 41). The arm64
CPU default is wired through `configure` (`HAIKU_CPU_$targetArch`,
`configure:363`; `aarch64-* → arm64`, `configure:288`).

Consequence: baseline ARMv8.0 → LSE atomics (ARMv8.1) are **off** (item 1),
Neoverse scheduling is off, only CRC32 HW is on.

**Recommendation.** Change line 41 to a Neoverse-N1 baseline:

```jam
case arm64 : archFlags += -mcpu=neoverse-n1 ;
```

- `neoverse-n1` is the Graviton2 core and the **greatest-common-denominator**
  across Graviton2/3(N1-superset V1/N2)/4(V2) — a binary built for it runs on
  all three, so the single-AMI requirement holds. It implies ARMv8.2-a and
  enables **LSE**, CRC, the FP/SIMD baseline, and N1 instruction scheduling.
- `+crc` is already implied by the ARMv8.2 baseline, so it need not be spelled
  out; keeping `+crc` explicit is harmless if preferred.

**Alternative (maximum portability, small runtime cost).**

```jam
case arm64 : archFlags += -march=armv8.2-a -moutline-atomics ;
```

`-moutline-atomics` emits both LSE and LL/SC paths behind a runtime
`__aarch64_have_lse_atomics` check, so the binary is safe even on a
hypothetical ARMv8.0 core, at the cost of a call/branch per atomic. Given the
fleet is uniformly ≥ Neoverse-N1, `-mcpu=neoverse-n1` is the better default;
reserve outline-atomics if the AMI must ever target pre-Graviton2 arm64.

**Exact change.** `build/jam/ArchitectureRules:41` — one line. Re-run
`configure` is **not** required (arch flags are read by `jam`, not baked into
`BuildConfig`); a plain rebuild picks it up.

**Verify / measure.** (a) Confirm LSE in disassembly per item 1. (b) Boot the
rebuilt image on a Graviton instance (see AGENTS.md → AWS Graviton test target).
(c) Microbench a contended refcount/lock or a socket accept/close loop before
and after; expect the largest deltas on many-vCPU instances.

**Open questions.** Confirm the cross-GCC in `buildtools` recognizes
`-mcpu=neoverse-n1` (GCC ≥ 9). If it is older, fall back to
`-march=armv8.2-a+lse` (unverified — depends on the pinned toolchain version).

**AWS guide (2026-08-22) — item closed, no change.** The toolchain question is
settled (GCC 13.3, well past the GCC-9 floor AWS gives for `neoverse-n1`), and
the flag choice is now confirmed against AWS's own table rather than reasoned from
first principles. The full argument, including why `-mcpu=neoverse-512tvb` is the
wrong answer for a single kernel image and why our port cannot safely emit SVE at
all, is in **"The `-mcpu` question, settled"** above. Summary: `-mcpu=neoverse-n1`
is AWS's Graviton2 "performance" flag and a strict superset of `-march=armv8.2-a`,
which is AWS's prescribed flag for "all current generation Graviton instances";
`512tvb` raises the ISA floor to Neoverse-V1 **including SVE**, which Graviton2
does not have and which this kernel neither enables nor context-switches.
`+crypto` is confirmed necessary (GCC enables only `fp`/`simd` by default, never
`crypto`) and safe (present on Graviton2-5). The comment block at
`ArchitectureRules:41-51` needs no correction.

## 3. HW extensions: CRC32 / AES / SHA / PMULL — baseline MERGED; intrinsics STILL OPEN

> **Half done, and the half that is done is the easy half.** `+crypto` is in the
> baseline as of `20bf8f2711`, so *our* code may use AES/SHA/PMULL and the compiler
> may auto-generate them. **What has not happened:** nothing has been rewritten to
> use them. `crc32c` is still the software table in
> `src/add-ons/kernel/file_systems/shared/crc32.cpp` — no intrinsic version exists.
> **Do not read the closed baseline as a closed item.**
>
> Note also the negative result recorded at the end of this item: `+crypto` does
> **not** unlock third-party crypto dispatch, because Haiku arm64 has no runtime
> feature-detection mechanism for a library to query (item 12).

**Current state.**

- **CRC32 instructions** are enabled (`+crc`, `ArchitectureRules:41`), but the
  only in-tree crc32c consumer is the **filesystem** checksum path
  (ext2/btrfs/xfs via `calculate_crc32c`), implemented as a **software byte
  table** (Gary S. Brown's table, `src/add-ons/kernel/file_systems/shared/crc32.cpp`;
  callers e.g. `ext2/Journal.cpp:767,835,849`). A table lookup does **not**
  auto-vectorize to the `crc32c` instruction; it needs an intrinsic rewrite.
- **Network checksums** on Haiku are IP/TCP ones-complement, not CRC; the
  ethernet FCS is computed by the ENA hardware. So there is no network CRC path
  to accelerate.
- **AES / SHA / PMULL** — no `+crypto` in the build (grep found none). TLS on
  Haiku is userland (OpenSSL-family), which does its own runtime CPU-feature
  dispatch via `getauxval`/HWCAP and is largely independent of the kernel arch
  baseline. **Unverified** whether Haiku's libroot exposes `AT_HWCAP` so
  userland crypto can detect Neoverse crypto extensions — worth checking before
  claiming any TLS win.

**Recommendation.** Fold `+crc`/crypto into the item-2 Neoverse baseline (which
already implies crypto extensions on real Graviton). Treat "make `calculate_crc32c`
use the `crc32c` instruction" as a small, separate, low-priority follow-up
(disk-bound, benefits ext2/btrfs mounts only). Do **not** block item 2 on it.

**Verify.** After item 2, `objdump -d` the crc32 object and confirm whether the
compiler emitted `crc32c*`; if still a table, an intrinsic version is required.

**Open questions.** ~~Does libroot surface `AT_HWCAP` for userland crypto
dispatch?~~ **Answered — no.** See below.

**AWS guide (2026-08-22).** `runtime-feature-detection.md` is unambiguous that
`getauxval(AT_HWCAP)`/`AT_HWCAP2` from `<sys/auxv.h>` is *the* recommended
mechanism (the bitmaps being "filtered by the kernel to include only features
which the kernel also supports"), with `AT_HWCAP` masks in `asm/hwcap.h`, and it
lists `HWCAP_AES`, `HWCAP_PMULL`, `HWCAP_SHA1`, `HWCAP_SHA2`, `HWCAP_CRC32` as
present on **all** Graviton generations and `HWCAP_ATOMICS` from Graviton2 on. It
explicitly discourages reading ID registers directly.

**Our finding: none of that exists on Haiku.** `grep -rn AT_HWCAP headers src`,
`grep -rn getauxval .` and `grep -rn ID_AA64ISAR0 src/` all return zero hits;
`arm_registers.h:215-216` defines the `ID_AA64ISAR0_EL1` masks but nothing reads
them, and there is no MRS trap emulation (`grep -i 'TID3\|TIDCP'` over
`src/system/kernel/arch/arm64` → nothing), so an EL0 `mrs` of an ID register would
trap rather than be emulated as it is on Linux. **So the claim that `+crypto`
buys userland TLS anything is now positively falsified, not merely unverified:**
an OpenSSL-family library on Haiku arm64 has no way to discover the crypto
extensions, so it will fall back to generic C paths regardless of what
`ArchitectureRules` says. `+crypto` remains correct — it lets *our* code use
those instructions — but it does not unlock third-party crypto dispatch. That is
now tracked as item 12.

## 4. ENA LLQ (Low-Latency Queues)

> **ANSWERED on hardware 2026-08-24 — the device *does* offer LLQ, and the driver
> is using it.** The "Recommendation: investigate before coding" and "Open
> questions" below are **spent**; do not re-run them. Captured from a boot log on
> the target instance (`ena-tx-offload.md` §5.1):
>
> ```
> ena: LLQ configured: 256 byte entries, 16 descriptors per entry,
>      max 2 per burst, transmit header limit 224
> ```
>
> So question 1 below ("does `supported_features` carry `BIT(ENA_ADMIN_LLQ)`") is
> **yes**, and the earlier "device does not support LLQ; using host placement"
> console line came from a different boot, not from a driver gap — exactly as the
> analysis below predicted.
>
> **The consequence nobody expected, and the reason this item still matters:** the
> LLQ burst allowance is **2 ring entries**, and at MTU 9001 a single jumbo frame
> consumes both. That is what **killed item 5a** (see below). LLQ being *present*
> is what makes doorbell coalescing impossible, not what would have made it
> possible.

**Current state.** The driver is **not** missing LLQ support — it implements the
full negotiation:

- `ena_configure_placement_policy()` sets inline-header, multiple-descs-per-entry,
  128/256-byte ring entries and calls `ena_com_config_dev_mode()`
  (`ena.cpp:504-562`), mirroring Amazon's `ena_set_llq_configurations()`.
- BAR2 (the LLQ push window) **is** mapped and set to write-combining
  (`ena.cpp:1649-1660`), and `comDev.mem_bar` is populated (`ena.cpp:1652`).
- The console line "device does not support LLQ; using host placement"
  (`ena.cpp:511`) is emitted **only** when the device does not advertise the LLQ
  feature bit: `(comDev->supported_features & BIT(ENA_ADMIN_LLQ)) == 0`
  (`ena.cpp:510`). That is a **device/instance-reported capability**, not a
  driver code gap.
- Ring-sizing already honours the LLQ depth caps when placement is DEV mode
  (`ena.cpp:590-620`), so the driver is ready to use LLQ if offered.

So on the boot where the message appeared, the device did not report the LLQ
feature. Whether that is the instance type, the negotiation ordering, or the
`mem_bar` state at the time is what needs confirming.

**Recommendation.** Investigate before coding. Confirm on hardware:

1. Whether `supported_features` carries `BIT(ENA_ADMIN_LLQ)` on the target
   instance type — LLQ availability varies by Nitro generation/instance family.
2. That `mem_bar != NULL` at the point `ena_configure_placement_policy()` runs
   (the early `return` at `ena.cpp:514-515` also falls back silently).
3. Cross-check `ena_config_host_info()` (`ena.cpp:314`, called at
   `ena.cpp:430`) — host-info feature flags can gate what the device offers back.

**Verify.** The driver already logs the full LLQ negotiation
(`ena.cpp:541-546`: `max_llq_num`, `max_llq_depth`, `max_wide_llq_depth`,
`entry_size_recommended`, supported flags). Capture that line from the boot log
on the target instance (via `aws ssm`) to see exactly what the device reports.

**Open questions.** Is this a m6g/c6g-vs-later difference? **Unverified** from
source; needs a per-instance-type boot-log capture.

**AWS guide (2026-08-22).** Nothing. LLQ is not mentioned anywhere in the repo —
not in `optimizing.md`, `optimization_recommendation.md`, or `dpdk_spdk.md` (which
mentions no ENA driver or PMD requirements at all). The plan above is unchanged
and unassisted; the hardware boot-log capture remains the only way to answer it.

## 5. Multi-queue + RSS — CANCELLED on evidence 2026-08-24

> **CANCELLED 2026-08-24. Do not start this, and do not re-derive it.** The
> 2026-08-22 correction below reclassified this from driver work to network-stack
> work. A control run afterwards closed it outright:
>
> **Linux on the same instance class, forced down to ONE ENA queue, does
> 29826 Mbit/s — against 29823 on eight** (interleaved A/B, four runs,
> `c7g.16xlarge`; `ena-multiqueue-headroom.md` §5). One queue already carries
> ~30 Gbps. DeBeOS's plateau on that class is **9.0–10.2 Gbit/s**. More queues
> cannot lift a ceiling that a single queue clears three times over, so queue count
> is not the limiter and cannot be.
>
> Two supporting facts, both worth keeping so the design below is not resurrected
> on a hunch:
>
> - The device's IO-queue grant is a **fixed 8 for the whole C7g family** — it is
>   *not* a function of vCPU count. So step 1's `min(num_io_queues_from_device,
>   ncpus) + 1` would have produced 9 vectors on a 2-vCPU instance and 9 on a
>   64-vCPU one, which is not the scaling story the step implies.
> - **The real limiter is interrupt cadence, item 11** — the
>   `XXX STRUCTURAL FIX STILL OWED` at `ena.cpp:221`. That is where the ~3× gap
>   lives, and it is a *single-queue* problem. Spending the large refactor below
>   would not have touched it.
>
> Everything from "CORRECTION, 2026-08-22" to the end of this item is retained as
> the record of how a plausible lever was ruled out. **It is not a worklist.**

> **CORRECTION, 2026-08-22 — this is not driver work, and it is not the lever this
> document has been calling it.** Haiku's network stack cannot consume multiple
> receive queues, so the driver refactor described below buys nothing on its own.
> Verified directly in-tree, not inferred:
>
> - `net_device_module_info::receive_data(net_device*, net_buffer** _buffer)`
>   (`headers/private/net/net_device.h:54`) hands up **one buffer at a time with no
>   queue index**. There is no multi-queue concept anywhere in the device API.
> - Exactly **one** `device_reader_thread` is spawned per interface
>   (`src/add-ons/kernel/network/stack/device_interfaces.cpp:538`), draining into a
>   single receive queue.
> - TX is one `ETHER_SEND_NET_BUFFER` ioctl per packet
>   (`src/add-ons/kernel/network/devices/ethernet/ethernet.cpp:281`), so there is no
>   batch for a driver to exploit and no "no more packets coming" signal to defer a
>   doorbell on.
>
> N hardware RX queues with N interrupt handlers could still win *interrupt-side* CPU
> parallelism while fanning in to the single `receive_data` call, but the payoff is
> capped by that serialized boundary, and per-queue MSI-X additionally needs distinct
> CPU affinity, which `install_io_interrupt_handler` has no argument for (on top of the
> open GICv3/ITS affinity question). **Reclassify: multi-queue + RSS is a Haiku
> network-stack project, sequenced after the driver-side robustness work.** `ena_rss.c`
> in the reference (338 lines) is genuinely portable and stays worth copying — but only
> once the stack can use it.
>
> What actually pays now, in the driver, and is ungated: interrupt moderation (item 11),
> and the RX-refill / completion-ack batching in item 5a below.

### 5a. Doorbell and ack batching — DEAD on evidence 2026-08-24

> **DEAD. Measured, not argued — and the measurement says the best case saves
> exactly zero.** Merged as `219d8ab858` ("ena: account for transmit doorbells, and
> settle whether they can be coalesced"), which added the accounting rather than the
> coalescing, because the accounting settled it.
>
> The device grants a burst of **2 LLQ ring entries between doorbells**, and a
> doorbell is what refills the allowance. At MTU 9001 one jumbo frame needs both
> entries, so a deferred doorbell is *forced* by the very next frame. Instrumented
> immediately before the doorbell:
>
> ```
> ena: tx: 200000 frames, 200000 doorbells, burst left min 0, exhausted 199880
> ```
>
> **99.94 % of transmit frames already leave the burst allowance at zero**
> (98.7–99.4 % with checksum offload on, for a `meta_valid` accounting reason that
> does not change the conclusion), and `burst left min` never rose above 0 in any
> run. Achievable coalescing ratio at MTU 9001: **1:1.**
>
> Scope of the claim, so it is not over-read: this is **MTU 9001**, which is what we
> run. At MTU 1500 a frame needs one entry, so two fit a burst and the ceiling on
> the saving is half the doorbells — still bounded by what a doorbell costs, which
> `ena-tx-offload.md` §5.2 puts a number on. Full working: `ena-tx-offload.md` §5.
>
> The **~40 lines** and "the win is available today" below were both wrong. Retained
> because the shape of the error is instructive: the reference driver amortises these
> three thresholds, and copying a reference optimisation without checking the
> capability the local device actually grants is how this item got written.

**Current state.** Everything is per-packet: the TX doorbell is rung for every frame
(`ena.cpp` TX path), the completion is acked per packet, and the RX ring is refilled one
descriptor at a time. The reference amortises all three —
`ena_com_is_doorbell_needed()`, an `ENA_TX_COMMIT`-sized ack batch, and a refill
threshold of roughly `ring_size / 8` (`ena_datapath.c`).

**Recommendation.** Port the three thresholds. ~40 lines, no dependency on the stack,
and unlike item 5 proper the win is available today. **Unverified** — no measurement yet
(see item 13).

**Current state — the main throughput lever, and a real refactor.**

- Exactly two MSI-X vectors are requested: `ENA_MSIX_VECTOR_COUNT 2`
  (`ena.cpp:53`) = one management (`ENA_MGMNT_VECTOR_IDX 0`) + one IO
  (`ENA_IO_VECTOR_IDX 1`) (`ena.cpp:51-52`); `ena_enable_msix()` bails if the
  device offers fewer than two (`ena.cpp:266-268`).
- One TX/RX pair only. `ena_create_queue_pair()` creates a single RX and single
  TX queue and carries an explicit `TODO`: *"one pair only. Scaling out means a
  vector and a ring per pair, plus RSS configuration to spread receive across
  them."* (`ena.cpp:844-845`). `ena_setup_io_queues()` calls it once
  (`ena.cpp:921-932`).
- The device struct is hard-wired single-pair: one each of
  `txSubmissionQueue`/`txCompletionQueue`/`rxSubmissionQueue`/`rxCompletionQueue`,
  one `txLock`/`rxLock`, one `ioVector` (`ena.h:210-236,197`). There is no
  per-queue object or array.
- RSS is **initialized** — `ena_prepare_rss()` builds a 128-entry indirection
  table (`ENA_RSS_TABLE_LOG_SIZE 7`, `ena.h:150`) and hash key
  (`ena.cpp:686-712`), and `ena_flush_rss()` pushes it after the queues exist
  (`ena.cpp:728+`) — but with one queue the table points every bucket at the
  single RX queue: *"We have one queue pair, so RSS buys us nothing directly"*
  (`ena.cpp:682`, and `:697` "everything lands on our single receive queue").

**Recommendation.** Scale IO queues to vCPU count with RSS spread:

1. Request `min(num_io_queues_from_device, ncpus) + 1` MSI-X vectors instead of
   the fixed 2 (`ena.cpp:53,266-274`); the device's max is read into
   `features->max_queues` / `max_queue_ext` (`ena.cpp:452,585`).
2. Promote the single-queue fields in `ena_haiku_device` (`ena.h:210-236`) into
   a per-queue struct (submission/completion queues, lock, buffer pool, free-id
   stack, MSI-X vector), one per pair; **cacheline-align** each (item 7) to
   avoid false sharing across CPUs.
3. Create N pairs in a loop in `ena_setup_io_queues()` (`ena.cpp:921`), each with
   its own vector.
4. Point the RSS indirection table across the N RX queues in
   `ena_prepare_rss()` (`ena.cpp:696-703`) instead of all-to-one.

**Verify / measure.** Boot on a multi-vCPU Graviton instance; confirm N IRQs are
delivered (the driver already counts IO interrupts, `ena.h:199`) and that
`netstat`/an iperf-style throughput test scales with queue count. Exercise the
reset/teardown path per queue with `ena_fault` (AGENTS.md → testing) since
multi-queue multiplies the descriptor-reclaim and reset unwind paths.

**Open questions.** How many IO queues the target instance grants (device-
reported); interrupt-affinity/steering support on the GICv3+ITS path (MSI-X
delivery is noted as "new" on this port, `ena.h:193`).

**AWS guide (2026-08-22) — validates the design and adds three requirements.**
`optimization_recommendation.md` §"Network-heavy workloads" is the only place AWS
addresses NIC scaling, and all of it lands on this item:

1. **Per-CPU IRQ affinity is not optional, it is the point.** AWS's recipe stops
   `irqbalance`, collects the ENA IRQs (`grep "eth0-Tx-Rx" /proc/interrupts`) and
   writes incrementing CPU numbers into `/proc/irq/$i/smp_affinity_list`,
   "assigning eth0 ENA interrupts to the first N-1 cores." So the plan's step 3
   ("each with its own vector") must be strengthened: each vector also needs a
   **distinct CPU affinity**. That promotes the open question above about
   interrupt-affinity support on the GICv3+ITS path from a footnote to a
   **prerequisite** — N vectors all delivered to CPU 0 would buy nothing.
2. **Do the steering in hardware, never in software.** AWS says to turn Receive
   Packet Steering *off* — `cat /sys/class/net/ethN/queues/rx-N/rps_cpus` should
   read `0` — "to avoid contention and extra IPIs", because "RPS is not needed on
   Graviton2 and newer." Directly confirms the RSS-indirection-table approach in
   step 4 and tells us **not** to build a software RX-steering fallback.
3. **Scale to `ncpus` with no halving.** "Graviton processors do not implement SMT
   (Hyper-Threading), so vCPUs map 1:1 to physical cores"
   (`configuring_your_sut.md`); `dpdk_spdk.md` adds "in Graviton, every vCPU is a
   full CPU" and flags the x86 habit of capping worker counts at half the vCPU
   count as an anti-pattern (`let physical_cores=$(nproc) / 2`). The plan's
   `min(num_io_queues_from_device, ncpus) + 1` is right as written.

**Measurement.** The CloudWatch allowance metrics (`bw_out_allowance_exceeded`,
`pps_allowance_exceeded`, `conntrack_allowance_exceeded`, …,
`debug_system_perf.md`) are read from outside the guest and therefore work against
Haiku — use them to prove a throughput plateau is the driver and not an instance
allowance before investing further. Note also `sar -n DEV`-style per-device rates
are unavailable to us; the driver's own counters are the substitute.

**Related new item.** Interrupt *moderation* is a separate gap, split out as item
11 — AWS warns Graviton's faster packet processing raises the interrupt rate, and
our driver never configures a coalescing interval.

## 6. Jumbo frames -- DONE, HARDWARE-VERIFIED 2026-08-23, THROUGHPUT MEASURED 2026-08-23

**Throughput (added 2026-08-23, `c7g.large`, one boot, MTU changed with
`ifconfig` between rows):** receive **952 → 4933 Mbit/s (+418%)** at **0.24× the
CPU per byte**; transmit 1148 → 1419 Mbit/s (+23.5%) at 0.47× the CPU per byte.
Transmit's small gain was later shown to be the socket send buffer, not the
driver — see item 15. Details in `throughput-measurement.md`.


**Result.** MTU **9001** on `/dev/net/ena/0` on a real Graviton instance
(`t4g.medium` from `ami-0d3f218d86ec93745`). Guest log:
`ena: set device MTU 9001 after queue creation (matching what we report to the
stack): ok`.

- **RX chaining proven**: metal -> Haiku with **DF set** (`ping -M do -s 8973`, so
  fragmentation cannot occur) -- 3/3 received, 8981 bytes back, 0.275 ms.
- **TX chaining proven**: Haiku -> metal `-s 8973` -- 3/3, 0.316 ms avg.
- **Boundary sweep** across the 2048-byte bounce slot: 1472, 2034, 2035, 2036,
  4083, 8972, 8973 -- all replied.
- **0 errors, 0 dropped** over 199 rx / 227 tx packets; no reset, leak or
  stranded-descriptor messages.

It took **both** halves: the ethernet layer's unconditional 1514 clamp (the real
binding constraint) had to become capability-gated, and the driver needed
multi-descriptor RX/TX. Note `ETHER_MAX_FRAME_SIZE` was deliberately *not*
bumped -- `tunnel.cpp` sizes queues with it and uses it directly as an MTU.

**Still open:** this proves jumbo *correct*, not *faster*. No throughput number
exists yet; that needs item 13's PMU work plus a load generator. Transmit
doorbell coalescing and TX checksum offload remain blocked in the stack (see
item 5a and the ENA notes).

<details>
<summary>Original analysis, kept for the record</summary>

> **CORRECTION, 2026-08-22 — our 1500 cap is not the binding constraint; the stack is.**
> Raising `ENA_FRAME_SIZE` and implementing multi-descriptor RX is necessary but **not
> sufficient**. Verified in-tree:
>
> - `ETHER_MAX_FRAME_SIZE` is **1514** (`headers/private/net/ethernet.h:17`).
> - Haiku's ethernet module clamps to it and pins MTU to it:
>   `device->mtu = ETHER_MAX_FRAME_SIZE - ETHER_HEADER_LENGTH`
>   (`src/add-ons/kernel/network/devices/ethernet/ethernet.cpp:155`),
>   `frame_size` clamped at `:199` and `:206-207`, and `ethernet_set_mtu` (`:384`)
>   rejects anything larger.
>
> So jumbo needs **two** changes, and the driver half is the smaller one: (a) Haiku's
> ethernet module and net stack must accept a larger frame, and (b) our driver needs
> multi-descriptor RX (`max_bufs = 1` today). The reference driver helps with (b) only.
> Sequence this with item 5's stack work, not ahead of it.

**Current state.** MTU is pinned at 1500:

- `ENA_FRAME_SIZE 1500` with a comment that jumbo needs multi-descriptor RX and
  is deferred (`ena.h:86-89`).
- The device's real limit **is** read — `device->maxSupportedMtu =
  features.dev_attr.max_mtu` (`ena.cpp:1457`, logged at `:1460`) — but not used;
  the value handed to the device is `device->frameSize` (= 1500)
  (`ena.cpp:1553-1554`).
- The RX path posts one 2048-byte buffer per frame with `max_bufs = 1`
  (`ena.cpp:1542-1543,2081-2085`), so a frame larger than one buffer cannot be
  reassembled — the reason MTU is set to match, not to the device max
  (`ena.cpp:1540-1551`).
- SET_FEATURE(MTU) is deliberately issued **after** queue creation
  (`ena.cpp:1532-1551`) — load-bearing ordering; keep it.

**Recommendation.** Two-part change, in order (as the code comment states,
`ena.cpp:1550-1551`): (1) implement multi-descriptor RX (chain buffers,
`max_bufs > 1`); (2) then raise the device MTU from `maxSupportedMtu`
(`ena.cpp:1457`) — e.g. 9001 for in-VPC jumbo — and advertise the same value to
the stack so the two never disagree (`ena.cpp:1544-1548`).

**Verify.** In-VPC ping/iperf at 9000-byte payloads between two instances;
confirm no fragmentation and that the RX reassembly handles multi-descriptor
completions. Fault-inject mid-jumbo-receive with `ena_fault`.

**Open questions.** Interaction with LLQ tx header limits (item 4) and with
per-queue buffer pool sizing once item 5 lands.

**AWS guide (2026-08-22).** Nothing. Jumbo frames, MTU 9001 and in-VPC MTU are
not discussed anywhere in the repo. No change to the plan above.

</details>

## 7. Barriers & cacheline alignment — STILL OPEN (audit only, 2026-08-24)

> **Open.** No code change has landed for this item. The audit's conclusion —
> "mostly correct" — still stands and is the reason it has stayed low priority.
> One dependency below is now void: the "align per-queue structs when item 5
> lands" clause, because **item 5 was cancelled**. There will be no per-queue
> structs.

**Current state — largely already correct/hardened.** AArch64 is weakly
ordered, and the driver's platform layer treats device-visible ordering
carefully:

- `wmb()`, `rmb()`, `mb()`, `dma_rmb()`, `mmiowb()` all map to
  `memory_full_barrier()` = `dsb sy` (`ena_plat.h:374-386`), deliberately
  stronger than the inner-shareable `dsb ishst`/`dmb ishld` that the arm64
  `arch_atomic.h` barriers default to (`arch_atomic.h:16-27`). The header
  comment explains why: an inner-shareable barrier is not guaranteed to order a
  DRAM descriptor write against the doorbell MMIO write seen by the PCIe master
  (`ena_plat.h:22-26,369-384`).
- Doorbell/MMIO register writes go through `ENA_REG_WRITE32`, which does a full
  barrier before the store (`ena_plat.h:458-462`); the LLQ push window uses
  explicit 64-bit stores followed by a full barrier
  (`ena_plat.h:465-490`, `ENA_MEMCPY_TO_DEVICE_64`). Descriptor publish uses
  `ENA_DB_SYNC_WRITE` → full barrier (`ena_plat.h:405-411`).
- ena-com ring/context structs are 64-byte aligned via
  `____cacheline_aligned = __attribute__((aligned(64)))` (`ena_plat.h:123`;
  used at `ena_com.h:140,159,204`).

**Recommendation.** Two low-priority, careful items:

1. The `dsb sy` everywhere is correct but heavier than strictly necessary; some
   sites (e.g. `dma_rmb` on a coherent Nitro link) could use a `dmb` variant.
   This is a micro-optimization with real correctness risk — do it only with
   `ena_fault` race coverage and measurement, and only after item 5.
2. When item 5 introduces per-queue structs in `ena_haiku_device`
   (`ena.h:176-236`, currently unaligned — `txLock`/`rxLock` share cachelines
   with neighbours), cacheline-align each per-queue object to prevent false
   sharing across the CPUs servicing different queues. No false-sharing hot path
   exists today because there is only one queue.

**Verify.** `ena_fault` reset/race path (AGENTS.md) must stay green across any
barrier change; measure per-packet cost before/after.

**Open questions.** Is Nitro PCIe guaranteed I/O-coherent on all target
instances (the code assumes so, `ena_plat.h:401-404`)? Treated as given here.

**AWS guide (2026-08-22) — no support for relaxing the barriers; keep them.**
`optimizing.md` establishes only the memory model: "Arm is weakly ordered, similar
to POWER and other modern architectures" versus x86's TSO; Armv8 systems
"including all Gravitons" are "weakly ordered multi-copy-atomic"; and "Code that
relies on TSO may lack barriers to properly order memory references." Its only
advice for code with "a bespoke implementation of lockless data structures" is
that it "will have to use the proper intrinsics and barriers to correctly order
memory transactions" — i.e. AWS's concern is *too few* barriers, never too many.
There is **no** `dsb sy` vs `dmb ish` scope guidance anywhere in the repo, and
`arm64-assembly-optimization.md` covers no ordering primitives at all.
**Recommendation 1 above (relaxing over-strong barriers) therefore gains nothing
from this review and stays where it is: lowest priority, measurement-gated, after
item 5.** The conservative `dsb sy` in `ena_plat.h:374-386` is not something to
trade away on generic advice.

For recommendation 2 (per-queue cacheline alignment), AWS's only relevant
*tooling* is SPE — called out as useful for "false sharing of atomic variables" —
and `perf c2c`, which "can be used to analyze cache coherence issues and false
sharing in multi-core systems" and "requires SPE on metal" (`debug_hw_perf.md`).
Both are Linux-only, so **the alignment work cannot be validated with AWS's tools
on Haiku**; the fallback is item 13's PMU counters plus wall-clock throughput.
There is also a new, unrelated metal-only IO consideration — the SMMU — split out
as item 14.

## 8. Spinlock backoff with WFE/SEV — STILL OPEN (item 8a did NOT close it)

> **Open as of 2026-08-24, and easy to mistake for closed.** `af7e48b94c` changed
> `arch_cpu_pause()` from `yield` to `isb` — that is **item 8a**, a backoff hint.
> This item is the monitored-wait redesign (`arm64_wfe`/`arm64_sev`), which stops
> the contended line being snooped at all. `isb` does not do that. The commit that
> landed 8a says so explicitly. Nothing here has landed.

**Current state — busy-`yield`, not WFE/SEV.**

- The spin loop in `acquire_spinlock()` reads `lock->lock` and, while held,
  calls `process_all_pending_ici()` then `cpu_wait(&lock->lock, 0)`
  (`smp.cpp:299-309`); the rw-spinlock variants do the same
  (`smp.cpp:400,421,445,503,524`).
- `cpu_wait()` (when no cpuidle module is registered) is just
  `arch_cpu_pause()` (`cpu.cpp:355-360`), and on arm64 `arch_cpu_pause()` is
  `arm64_yield()` = the `yield` hint (`arch_cpu.h:140-143`).
- `arm64_wfe()` and `arm64_sev()` are defined (`arch_cpu.h:23-24`) but **unused**
  anywhere in the spin path — grep finds only the macro definitions.

So contended spinlocks spin-poll with `yield`, burning the core and offering no
power benefit, versus a WFE monitored wait that parks the core until the lock's
cacheline is written and a `sev`/event wakes it.

**Recommendation.** Implement a WFE/SEV backoff for arm64. Sketch: in the
inner wait loop, arm the exclusive monitor on `&lock->lock` (a load-exclusive),
then `wfe` — the core sleeps until the monitored line is written or an event
arrives; `release_spinlock()` (`smp.cpp:332-361`) issues `sev` (or relies on
the natural event from the store-release clearing the monitored line). This is
best done behind the existing `arch_cpu_pause`/`cpu_wait` seam so generic
`smp.cpp` stays arch-neutral, or via an arm64 `cpuidle`-style wait hook.

**Effort/risk.** Medium — it touches the single most correctness-sensitive
primitive in the kernel; a missed wakeup is a hang. Needs careful review and
heavy SMP stress on real Graviton.

**Verify / measure.** Boot + SMP stress on a many-vCPU instance; measure lock
throughput under contention and idle power. Confirm no missed-wakeup hangs.

**Open questions.** Whether to route through `cpu_wait`/`arch_cpu_pause`
(`cpu.cpp:355`, `arch_cpu.h:140`) or add a dedicated arm64 monitored-wait; and
whether an explicit `sev` in `release_spinlock` is needed or the store-release
event suffices.

### 8a. `arch_cpu_pause()` → `isb` instead of `yield` — MERGED 2026-08-24

> **Merged** as `af7e48b94c`. `yield` is a no-effect hint on Neoverse cores, so every
> spin loop in the kernel had no backoff at all; on arm64 `cpu_wait()` always reaches
> `arch_cpu_pause()` (the cpuidle modules are x86-only), so this is live on every
> path. All 21 call sites were audited.
>
> **Two caveats recorded with the change, worth carrying:** `spin()` and the KDL
> CPU-halt timeout poll the clock rather than counting iterations, so an extra
> pipeline flush per iteration coarsens their granularity slightly; and
> `SPINLOCK_DEADLOCK_COUNT` counts iterations rather than duration, so a real deadlock
> now takes proportionally longer in wall-clock to panic. Neither is a correctness
> change.
>
> **This does NOT close item 8.** `isb` is backoff; it does not stop the contended
> line being snooped, which is what the WFE/SEV redesign in item 8 is for. Item 8
> remains open. No performance number was taken for 8a either.

**AWS guide (2026-08-22).** `optimization_recommendation.md` §"Locks and
synchronization" gives two recommendations, neither of which this plan was
tracking, and **neither** is WFE/SEV (which the repo never mentions at all):

1. Replace a back-off loop built on x86 `PAUSE` (or the `rep; nop` equivalent)
   with **a single `ISB` instruction** on Graviton2 — described as "a drop in
   replacement", citing the WiredTiger storage-layer commit as precedent.
2. Where a fast-path lock acquisition precedes an OS-level sleep, **retry the fast
   path several more times** before falling through to the slow path, citing
   Finagle's `NonReentrantReadWriteLock.scala` as an example of spinning longer on
   Graviton2.

**Why this matters here.** Recommendation 1 maps onto exactly the code item 8
describes, but as a *one-token edit rather than a redesign*: `arch_cpu_pause()` is
`arm64_yield()` (`arch_cpu.h:140-143`), and **`arm64_isb()` already exists two
dozen lines above** (`arch_cpu.h:26`). On Neoverse cores `yield` is an
architectural hint with no required effect, so today's spin loop re-issues its
load as fast as the core can retire it; `isb` forces a pipeline resynchronisation
and so delivers a real, bounded, cheap back-off delay — which is precisely the
role x86's `pause` plays and which `yield` does not fill.

**Recommendation.** Land 8a **before** the WFE/SEV work in item 8, as an
independent change. It is a single line in one arch header, it changes no locking
protocol, it cannot introduce a missed wakeup (the defining risk of item 8), and
it is AWS's stated advice for this exact pattern. It also improves the baseline
that item 8 must later beat. Recommendation 2 (extra fast-path retries) applies to
Haiku's mutex/rw-lock slow paths rather than `acquire_spinlock` (which never
sleeps) and is a separate, lower-priority idea.

**Effort/risk.** Small / low, versus Medium / Medium for item 8 proper.

**Verify / measure.** Boot plus SMP stress as for item 8. **The benefit is
unverified — we have no measurement, and AWS gives no number for the `ISB`
substitution either**, only that it is a drop-in replacement. Note the trap from
`debug_code_perf.md`: because Graviton "idles in a clock-gated sleep state, so
counters stop ticking", cycle-based measurement flatters anything that parks a
core. Measure 8a and item 8 in wall-clock throughput under contention, not cycles.

## 9. Cacheline size constants

**Current state — verified correct, no change needed.**

- `CACHE_LINE_SIZE` is `64` for arm64 (`arch_cpu.h:10`), which matches Graviton
  (64-byte lines). `CACHE_LINE_ALIGN` uses it (`arch/cpu.h:46`), and
  `cpu_ent` is so aligned (`cpu.h:52`).
- The runtime cache-line size is read correctly from `CTR_EL0` in
  `arch_cpu_sync_icache()`: `4 << (ctr_el0 & 0xF)` for I-cache and
  `4 << ((ctr_el0 >> 16) & 0xF)` for D-cache (`arch_cpu.cpp:87-91`) — the
  earlier shift UB is gone.
- ena-com's `____cacheline_aligned` is also 64 (`ena_plat.h:123`).

No hardcoded cacheline constant is wrong for Graviton. This item is an audit
pass; nothing to change.

**AWS guide (2026-08-22) — independently corroborated.** `dpdk_spdk.md` is the
only place in the repo that names a cache-line size, and it specifies
`RTE_CACHE_LINE_SIZE=64` for Graviton (with no 128-byte alternative offered
anywhere), which matches `CACHE_LINE_SIZE 64` (`arch_cpu.h:10`) and ena-com's
`____cacheline_aligned` (`ena_plat.h:123`). The repo gives no cache-line or
false-sharing *guidance* beyond that. Item stays closed.

## 10. Larger translation granules / huge pages (16K/64K) — STILL OPEN, RESEARCH (2026-08-24)

> **Open.** No code, and still correctly classified as a research spike rather than
> a scheduled change. **The gate named below is now buildable rather than
> hypothetical:** this item was waiting on `data-tlb-tw-pki`, and item 13's PMUv3
> facility merged in `af7e48b94c` — so the counter can now be programmed. It has
> not been read yet. That is the next step here, and it is cheap.

**Current state (scoped, not a quick win).** The translation map is *written* to
be granule-parameterized but is *instantiated* at 4K:

- `VMSAv8TranslationMap` takes `pageBits` and derives table geometry from it
  (`fPageBits`, `CalcStartLevel`, table walks all use `fPageBits`:
  `VMSAv8TranslationMap.cpp:117-131,317-321,453-461`).
- But the kernel map is constructed with `pageBits = 12` (4K), `vaBits = 48`:
  `new VMSAv8TranslationMap(kernel, pt, 12, 48, 1)`
  (`arch_vm_translation_map.cpp:42`).
- The rest of the system assumes 4K: `B_PAGE_SIZE` is used pervasively
  (`VMSAv8TranslationMap.cpp:278,284,605,627,664,676`), and the kernel link uses
  `-z max-page-size=0x1000` (`ArchitectureRules:397+` arm64 case).

**Why it's not a quick win.** Moving to a 16K or 64K granule (fewer TLB entries
per unit memory → fewer walks/misses, a real Graviton win for large working
sets) means changing `B_PAGE_SIZE` and every assumption keyed on it across the
VM, the loaders, ELF alignment, and `max-page-size` — not a driver-local or
config-local change. The arm64 map's `fPageBits` parameterization is a helpful
starting point, but the blast radius is system-wide.

**Recommendation.** Keep as a research spike: prototype a 16K-granule kernel in
a branch, measure TLB-miss deltas on a large-working-set workload, and only then
decide. Not scheduled alongside items 1–8.

**Open questions.** Whether Haiku's VM and ELF loader tolerate a non-4K
`B_PAGE_SIZE` at all on any arch (**unverified**); interaction with the boot
loader and the `max-page-size` link flag.

**AWS guide (2026-08-22) — reframes this item and supplies the gating metric.**
Three things, in order of usefulness:

1. **AWS's answer to TLB pressure is huge pages, not a larger base page.**
   `optimization_recommendation.md` §"high TLB miss rates" recommends Transparent
   Huge Pages (`echo always`/`madvise > /sys/kernel/mm/transparent_hugepage/enabled`),
   and notes that "On kernels 6.9 and newer, THP folios add 16kB and 64kB huge
   pages alongside 2MB"
   (`/sys/kernel/mm/transparent_hugepage/hugepages-16kB/enabled`, `…-64kB/…`,
   `…-2048kB/…`), plus pinned pages via `sysctl -w vm.nr_hugepages=X` or
   `hugepagesz=2M hugepages=512` for mmap-heavy applications. The sysfs knobs
   themselves do not transfer, but the **strategy** does, and it is the one this
   item was missing: on arm64, 16K/64K THP folios are implemented with the
   **contiguous bit** in the PTE (16 adjacent 4K PTEs coalesced into one TLB
   entry), and 2M/1G with **block mappings** at L2/L1 — *all available at a 4K
   granule, with no `B_PAGE_SIZE` change and none of the system-wide blast
   radius*. AWS also gives the caveat: exclusive use of huge pages "may lead to
   performance degradation", so test the whole workload.

   **Our finding:** `VMSAv8TranslationMap.cpp` uses neither. `grep -niE
   'contiguous|CONT'` over it returns nothing, and the only block-mapping mention
   is a comment about "splitting block mappings" (`:375`). So this is genuinely
   unimplemented, not merely unexploited.

   **Split this item.** *10a — contiguous-bit and block mappings inside
   `VMSAv8TranslationMap`.* Localized to one file, no `B_PAGE_SIZE` change, no ELF
   or loader impact; medium effort, medium risk, and it captures most of the TLB
   win AWS is chasing. *10b — the global `B_PAGE_SIZE` granule change*, i.e. the
   original item as written above: still a research spike, still high risk. **10a
   should be the spike, and 10b only if 10a proves insufficient.**

2. **A 64K kernel page size is proven in production on Graviton.** `os.md`'s
   per-OS "Kernel page size" column lists RHEL 8.2+, AlmaLinux 8.4+, Rocky Linux
   and CentOS Stream 9 as **64KB**, against 4KB for Amazon Linux 2/2023, all
   Ubuntu, Debian, FreeBSD and Flatcar. That removes "does a non-4K granule even
   work on Graviton hardware" as a risk for 10b. It does **not** settle anything
   else: AWS never recommends one over the other, gives no measurement, and lists
   no downside — and the open question above (whether *Haiku's* VM and ELF loader
   tolerate a non-4K `B_PAGE_SIZE`) is untouched by it and **remains unverified**.

3. **The gating metric now has a name.** `debug_hw_perf.md` defines
   `data-tlb-mpki` and `data-tlb-tw-pki` (and `inst-tlb-mpki`/`inst-tlb-tw-pki`),
   with the threshold "> 0" for each: `data-tlb-mpki` means "translation stalls
   before load/store issue", and `data-tlb-tw-pki` means the core must walk the OS
   page table, requiring "**extra memory references**" ahead of the application's
   own access. **This is the number that makes 10a/10b answerable instead of
   open-ended, and we cannot read it today** — hence item 13. Sequence 13 before
   10.

## 11. ENA interrupt moderation / coalescing — partly MERGED; the structural fix is STILL OPEN

> **State on 2026-08-24: this is the live network bottleneck, and someone is on it.**
> Do not pick it up without checking who; do not mark it done.
>
> **What landed** (`aa8cbd0c8e` "ena: enable interrupt moderation, stop leaking
> buffers, guard open"): moderation is now actually configured. `ena_io_interrupt()`
> re-arms the vector with `ena_com_update_intr_reg(..., ENA_RX_IRQ_INTERVAL,
> ENA_TX_IRQ_INTERVAL, true, false)` — the final `false` is
> `no_moderation_update`, so the intervals take effect. Previously it passed zeroes
> with `no_moderation_update = true`, which explicitly asks for *no moderation at
> all*. So the "never configured" diagnosis below is fixed.
>
> **What is still owed, and it is the bigger half.** `ena.cpp:221` carries a live
> `XXX STRUCTURAL FIX STILL OWED`: the unmask belongs *after* the ring has been
> drained, not in the interrupt handler. We unmask while every completion is still
> unconsumed, so **moderation is the only backstop we have** rather than a tuning
> knob on top of correct structure. Moving the unmask into the reader threads
> (`ena_receive()` / `ena_reclaim_transmitted()`, after the drain loop) is
> Haiku-specific work because the vector is shared by both directions, so both sides
> must agree on who re-arms.
>
> **The gap this is responsible for:** DeBeOS plateaus at **9.0–10.2 Gbit/s** on
> `c7g.16xlarge` where Linux does **29.8 Gbit/s** — and Linux does that on *one*
> queue, which is why item 5 was cancelled and this was promoted. Roughly 3×.
>
> **One figure to quote carefully.** The "**2.82 frames per interrupt**" number in
> `ena-multiqueue-headroom.md` is a **lower bound, not a measurement** — it is
> `141,217 / 50,000` and assumes *every* interrupt is used, which is the most
> favourable assumption available. Do not cite it as the observed cadence.

**Current state (2026-08-22, superseded above) — initialised but never configured.** `ena_setup_io_irqs()` calls
`ena_com_init_interrupt_moderation(&device->comDev)` and only logs on failure
(`ena.cpp:1501-1502`), with a comment explaining the ordering ("The reference
drivers initialise interrupt moderation here, while still polling"). That is the
**only** moderation call site in the driver — `grep -n 'moderation\|adaptive'
ena.cpp` finds nothing else. So:

- `ena_com_enable_adaptive_moderation()` / `..._disable_...`
  (`ena-com/ena_com.h:1169,1174`) are never called; `adaptive_coalescing`
  (`ena_com.h:419`) keeps its initialised value.
- `ena_com_update_nonadaptive_moderation_interval_tx/rx()`
  (`ena_com.h:1094,1104`) are never called, so `intr_moder_tx_interval` /
  `intr_moder_rx_interval` (`ena_com.h:425-426`) are never set.

The device therefore runs on whatever its default is, and the driver has no policy.

**AWS guide.** `optimizing.md` flags this as a Graviton-specific effect: Graviton
packet processing may be "faster and lower-latency than other platforms", which
"reduces the natural 'coalescing' capability of Linux kernel and increases the
interrupt rate", with the suggested mitigation being to enable adaptive RX
interrupts (`ethtool -C <interface> adaptive-rx on`).
`optimization_recommendation.md` gives the inverse for latency-sensitive services:
`ethtool -C ethN adaptive-rx off`. The `ethtool` plumbing does not transfer — we
have no `ethtool` and no ethtool-equivalent ioctl surface — but the underlying
knob is ena-com's, and we already link the code that drives it.

**Recommendation.** Small change, two parts. (1) Decide and *state* a policy
rather than inheriting a default: set a non-adaptive RX and TX interval explicitly,
or call `ena_com_enable_adaptive_moderation()` if the device reports support. (2)
Because AWS's advice points in opposite directions for throughput versus latency,
make it a driver setting rather than a constant. Sensible to land alongside or just
after item 5, since interrupt rate is what multi-queue multiplies.

**Verify / measure.** The driver already counts IO interrupts (`ena.h:199`) — that
counter is the measurement, and it is available without any Linux tooling: compare
interrupts-per-packet before and after at a fixed offered load.

**Open questions.** Whether the target device advertises adaptive moderation at
all (`ena_com_init_interrupt_moderation()` is allowed to fail and we continue);
what interval the reference drivers actually pick. **Unverified.**

## 12. Runtime CPU feature detection — absent entirely; STILL OPEN 2026-08-24

> **Open, re-verified 2026-08-24.** No `AT_HWCAP`/`getauxval`/`ID_AA64ISAR0_EL1`
> reader has landed. One correction to the reasoning below: it argues from "the
> kernel already reads ID registers at EL1" — that is now more true than when
> written, because `arch_pmu.cpp` (`af7e48b94c`) reads `ID_AA64DFR0_EL1` and
> `PMCEID0/1` and does exactly this kind of capability decoding. It is a working
> in-tree pattern to copy, not just an assertion.

**Current state — the mechanism does not exist.** Verified by grep over the tree:

- No `AT_HWCAP` or `AT_HWCAP2` anywhere in `headers/` or `src/`; no `getauxval`
  anywhere in the repository.
- No reader of `ID_AA64ISAR0_EL1` — `arm_registers.h:215-216` defines the masks,
  nothing uses them. (`ID_AA64MMFR*` and `ID_AA64PFR0` *are* read, for HAFDBS/CnP
  and the GIC CPU interface, so the pattern exists; the ISA-feature register is
  simply unread.)
- No MRS trap emulation for EL0 (`grep -i 'TID3\|TIDCP'` over
  `src/system/kernel/arch/arm64` → nothing), so unlike Linux, a userland `mrs` of
  an ID register traps rather than being emulated.

**Why it matters — two consequences, one per side of the syscall boundary.**

1. *Userland crypto gets nothing from `+crypto`.* This closes item 3's open
   question negatively: OpenSSL-family code on Haiku arm64 has no supported way to
   discover AES/SHA/PMULL, so it uses generic C paths. AWS's whole
   `runtime-feature-detection.md` recommendation — `getauxval(AT_HWCAP)` /
   `AT_HWCAP2`, masks from `asm/hwcap.h`, dispatch via ifunc or a function
   pointer — is unavailable to us. **This is the clearest example in this document
   of AWS guidance that does not transfer.**
2. *A latent repeat of the atomics bug across the userland build.* GCC 13.3 turns
   `-moutline-atomics` on by default whenever the target is base ARMv8.0 (GCC 13.3
   manual). Our `ArchitectureRules:52` baseline makes the flag inert for anything
   *jam* builds — but haikuports recipes and third-party build systems that set
   their own `CFLAGS` may compile at base ARMv8.0, emit calls to the
   `__aarch64_*` helpers, and hit the same permanently-zero
   `__aarch64_have_lse_atomics` we just fixed in the kernel: a call **plus** an
   LL/SC retry loop, for every atomic. This bears directly on Phase 2/4 in
   [sequencing.md](sequencing.md) (the ~150-package userland). **Unverified** —
   nobody has disassembled a haikuports-built arm64 binary to confirm it; that
   check is cheap and should be done early in Phase 2.

**Recommendation.** Two independent tracks:

- *Short term, for the atomics risk:* ensure `-mno-outline-atomics` (or the
  ARMv8.2 baseline) reaches recipe builds through whatever CFLAGS haikuporter
  hands them. Note that adding `-mno-outline-atomics` to `ArchitectureRules` would
  be **redundant** and misleading (see item 2) — the fix belongs in the recipe
  build environment, not the arch baseline.
- *Longer term:* publish a HWCAP-equivalent. The kernel already reads ID registers
  at EL1, so the missing piece is a word derived from `ID_AA64ISAR0_EL1` et al.,
  exposed to userland (via the runtime loader's arguments, a `system_info`-style
  call, or a real auxv), plus a libroot accessor. Without it, no third-party
  library can ever do feature dispatch on this port.

**Effort/risk.** Medium / low. **Benefit:** unblocks userland crypto and removes a
whole-userland atomics regression risk — neither of which is a kernel throughput
win, which is why it belongs in the Phase 2/4 sequence rather than Phase 7.

## 13. PMU (PMUv3) counters — facility MERGED 2026-08-24; NOTHING MEASURED WITH IT YET

> **The facility exists. The readings do not. Keep those two apart.**
>
> **Merged** as part of `af7e48b94c` ("arm64: back off spin loops with isb, and add a
> PMUv3 counter facility") — `src/system/kernel/arch/arm64/arch_pmu.cpp`,
> `headers/private/kernel/arch/arm64/arch_pmu.h`. It is the read-only facility this
> item asked for: `PMUVer` is read from `ID_AA64DFR0_EL1` with both "absent" and
> "implementation-defined" rejected; every programmed event is checked against
> `PMCEID0/1` and flagged if unimplemented (an unimplemented event reads zero, which
> is indistinguishable from an event that never happened); counters are 32-bit on
> Neoverse-N1/-V1 so they are software-extended, with `PMOVSCLR_EL0` read and cleared
> each sample to flag a wrap; `PMINTENCLR_EL1` is forced clear and `PMUSERENR_EL0`
> zeroed.
>
> **It is off unless asked for** — the `arm64_pmu` boot setting, or `pmu on` in KDL —
> because a hypervisor at EL2 may set `MDCR_EL2.TPM` and trap our accesses.
>
> **So the "Recommendation" below is done and the "Why it should move up the order"
> argument has NOT yet been cashed in.** The commit message is explicit: *"Nothing
> has been measured; no numbers are claimed anywhere."* Every "measure" line
> elsewhere in this document is still owed a number. The open work here is **using**
> this, not building it — starting with the two things this item was justified by: a
> real performance number for the LSE change (item 2), and `data-tlb-tw-pki` as
> item 10's gate.

**Current state (2026-08-22, superseded above) — nothing.** `grep -riE 'pmcr_el0|pmevcntr|pmuserenr|pmccntr'`
over `src/` and `headers/` returns zero hits. The arm64 port has no access to the
performance monitors at all, which is why every item in this document ends in
"measure" and none of them can.

**AWS guide.** The entire `perfrunbook` hardware chapter (`debug_hw_perf.md`) is
built on PMU counters, and it defines a specific, reproducible ratio set with
thresholds:

| Ratio | AWS's threshold / meaning |
|---|---|
| `ipc` | compare across instance types; lower on Graviton shows *that* a problem exists |
| `stall_frontend_pkc` vs `stall_backend_pkc` | decides instruction supply vs execution/memory as the limiter |
| `branch-mpki` | "> 10" → branch prediction is the bottleneck |
| `inst-l1-mpki` | "> 20" → code working set spills L1I |
| `data-l1-mpki`, `l2-mpki`, `l3-mpki` | "> 20", "> 10", "> 10" → data working-set size; `l3-mpki` doubles as a DRAM-bandwidth proxy |
| `data-tlb-mpki`, `data-tlb-tw-pki` | "> 0" → translation stalls / page-table walks (**this is item 10's gate**) |
| `inst-tlb-mpki`, `inst-tlb-tw-pki` | "> 0" → instruction-side translation stalls |
| `code_sparsity` | "> 0.5" → sparse code layout; Graviton 16xlarge or metal only |

AWS's instance-size table lists `*7g` as having full PMU support at 16xlarge and
metal, so **c7g.metal qualifies**. Their *collection* path does not transfer
(`aperf record`, `perf stat`, `sysctl kernel.perf_event_paranoid=-1`, and the
`measure_aggregated_pmu_stats.py` / `measure_and_plot_basic_pmu_counters.py`
helpers are all Linux) — but **PMUv3 is architectural**, and the counters are
readable at EL1 through `PMCR_EL0`, `PMCNTENSET_EL0`, `PMEVTYPER<n>_EL0`,
`PMEVCNTR<n>_EL0` and `PMCCNTR_EL0`. There is no Linux dependency in the
measurement itself, only in AWS's wrapper.

**Recommendation.** Implement a **read-only** counter facility for arm64: program
a small fixed set of events, expose them through a KDL command and/or a `/dev`
node, and compute AWS's ratios offline. Deliberately *not* a full profiler — no
sampling, no interrupts, no perf-event abstraction. That is enough to answer every
"measure" line in this document.

**Why it should move up the order.** It is the only item that converts the rest of
Phase 7 from inference into evidence: it closes the open Phase 1 gap (a real number
for the LSE change), it is the gate for item 10 (`data-tlb-tw-pki`), it substitutes
for the SPE/`perf c2c` tooling we cannot have in item 7, and it gives item 8/8a a
way to be judged.

**Effort/risk.** Medium / low — it is additive, reads registers the kernel already
runs at the right EL to read, and touches no existing path.

**Verify.** Cross-check `ipc` against a known-shape workload (a tight scalar loop
should approach the core's issue width); sanity-check `PMCCNTR_EL0` against the
generic timer, which DeBeOS already uses.

**Open questions.** Whether `PMUSERENR_EL0` should ever be opened to userland
(probably not, initially); whether counters survive our idle path — AWS warns
Graviton "idles in a clock-gated sleep state, so counters stop ticking"
(`debug_code_perf.md`), which is a correctness caveat for any cycle-derived ratio.
A design note for any *future* sampling profiler, also from `debug_code_perf.md`:
profiles taken inside interrupt-masked kernel code get misattributed to whatever
routine unmasks interrupts, which is why AWS uses pseudo-NMI
(`CONFIG_ARM64_PSEUDO_NMI`, `irqchip.gicv3_pseudo_nmi=1`) — sampling must arrive in
a context we have not masked. **All unverified; no PMU code exists yet.**

## 14. SMMU / IOMMU on metal — STILL OPEN, INVESTIGATE (2026-08-24)

> **Open; no audit done.** One premise below has since been settled and makes this
> item *reachable* rather than blocked: `c7g.metal` now boots to userland and
> enumerates its PCI devices (the ECAM multi-region fix, `f5367b3602`, merged via
> `e270548f33`), so the IORT can actually be read on the target. Previously this
> could not be checked on metal at all.

**AWS guide.** `linux_kernel.md` §"Metal IO tuning" states that on Graviton2 and
newer **metal** instances, turning the System MMU off will "speed up IO handling"
(`sudo ./configure_graviton_metal_iommu.sh off`, then reboot), notes that off "is
the default on x86", advises "Leave the SMMU on if you require the additional
security protections it offers", and — the part that matters most for us —
"Virtualized instances do not expose an SMMU to instances."

**Why it matters here.** Our test target is **c7g.metal**, so an SMMU is present
and in the ENA DMA path; the virtualized instance types in the fleet have none.
Two consequences:

1. There may be a measurable metal-only IO cost that has nothing to do with the
   ENA driver, which would confound any item 5/6/7 measurement taken on metal and
   extrapolated to virtualized instances. Worth knowing before we attribute a
   throughput number to a driver change.
2. It is unaudited on our side. **Unverified:** whether the Haiku arm64 port
   programs an SMMU at all (no audit done), or whether it inherits whatever state
   UEFI leaves — presumably permissive, since ENA DMA works today. If we are
   running under a firmware-configured bypass, the AWS knob may already be
   effectively "off" for us; if we are running translated, there is headroom.

**Recommendation.** Investigation only, no code. Determine from the ACPI tables
(IORT) whether an SMMUv3 is described on c7g.metal and what DeBeOS does with it,
then decide whether anything is worth changing. Low priority relative to items 5
and 13, but cheap, and it belongs on the record before ENA numbers are quoted.

**Not transferring.** `configure_graviton_metal_iommu.sh` is a Linux script
(`perfrunbook/utilities`); the equivalent lever for us would be firmware/EFI
configuration or simply leaving the SMMU untouched. Also non-transferring, noted
for completeness: `linux_kernel.md`'s `PREEMPT_LAZY`/`CONFIG_PREEMPT_NONE`
discussion is about Linux guests preempting userspace spinlock holders — Haiku's
`acquire_spinlock()` disables interrupts, so the kernel side does not apply, though
the general hazard (preempting a lock holder starves every waiter) is worth
remembering for Haiku *userland* spin loops.

---

## 15. Default socket send buffer — DONE, MEASURED 2026-08-23

**The single largest throughput win found so far, and it is not in the driver.**

`net_socket.cpp` hard-coded both socket buffers to 65535 with no autotuning. A
TCP stream cannot hold more than one send buffer in flight per round trip, so on
a `c7g.large` at 0.326 ms RTT that is a hard ceiling of
`65535 * 8 / 0.000326 = 1608 Mbit/s`. Transmit measured **1611 Mbit/s**. The
number being measured was the default, to three digits.

Raising `send.buffer_size` to **256 KiB**: transmit **1611/1397 → 4376/4421
Mbit/s (~3×)**, CPU per mebibyte **3900 → 2221 µs (−45%)**. A 64 KiB–4 MiB sweep
puts the plateau at 192–288 KiB with the gain reversing above it, so 256 KiB is
the measured optimum rather than a round number.

The receive default was left at 65535 on evidence: it already reached 4942 Mbit/s
and every larger value tested was equal or worse.

Full method, tables and the interleaved controls: `throughput-measurement.md`.

**Still open from the same measurements:**

- ~~A throughput cliff between a receive buffer of 65535 and 65536 — one byte~~
  **RESOLVED, and the framing above was wrong. There is no one-byte boundary:**
  65535, 65536 and 65537 measure identically when all three are set explicitly
  (3747/3751/3803 Mbit/s). The original comparison was confounded — the "65535"
  datapoint was the *default*, never set, and the "65536" datapoint was explicitly
  set, so the difference came from **the act of setting**, not from the value.
  Cause: `TCPEndpoint::SetReceiveBufferSize()` cleared
  `FLAG_AUTO_RECEIVE_BUFFER_SIZE`, the sole gate on receive-window growth, so *any*
  explicit `SO_RCVBUF` pinned the window forever. Fixed: pin only when the request
  is **smaller** than the queue already has. Verified — explicit 65536 went
  ~3650 → 4950 Mbit/s while a shrink to 16 K is still honoured.
  Note the magnitude is **RTT-dependent** (the same pin costs 3.4× at 0.326 ms and
  1.3× at 0.18 ms), so quoting a ratio for this class without the RTT is
  meaningless. See `tcp-rcvbuf-cliff.md`.
- ~~**No autotuning.** 256 KiB beats 65535 but every fixed value is wrong
  somewhere — a waste on a LAN, too small on a long fat path.~~
  **FIXED and merged 2026-08-24 (`85f9d73594` "tcp: autotune the send buffer
  towards the bandwidth-delay product").** The diagnosis was right, and the
  measurement is worth quoting because it shows how wrong a fixed value gets: on
  `c7g.large`, 512 MiB per run, interleaved — at **10.17 ms** RTT a pinned 256 KiB
  gives **206 Mbit/s** against **3165** for 8 MiB, while at **0.16 ms** the same
  8 MiB gives **2915** against **4318** for 256 KiB. The best fixed value is a
  different value at every round-trip time. `_UpdateSendBuffer()` now mirrors
  `_UpdateReceiveBuffer()`, targeting twice the bandwidth-delay product, capped at
  8 MiB.

  **Two implementation traps recorded with that change, because a first attempt hit
  both.** Size against the **minimum** round trip and **acknowledged** bytes, never
  the smoothed estimate or the observed flight size — anything our own queueing
  inflates is a feedback loop (larger queue → higher delay → higher target → larger
  queue), and the version that used flight size grew to 1–3.5 MB on a 0.16 ms path
  and measured **25% slower than the fixed default it replaced**. And the probe must
  be in **microseconds**: `tcp_now()` ticks in milliseconds, so a data-centre round
  trip smooths to zero — exactly the path this feature exists for. Full method:
  `tcp-send-autotune.md`.

---

## Evidence index (files inspected)

- `build/jam/ArchitectureRules:41` (arm64 arch flags), `:397+` (kernel link
  max-page-size)
- `headers/os/support/SupportDefs.h:290-360` (atomic builtins)
- `src/system/libroot/os/arch/generic/generic_atomic.cpp` +
  `src/system/libroot/os/arch/arm64/Jamfile`
- `headers/private/kernel/arch/arm64/arch_atomic.h` (barriers only),
  `arch_cpu.h:10,23-24,140-143` (WFE/SEV/yield, CACHE_LINE_SIZE)
- `src/system/kernel/smp.cpp:257-361` (spinlock), `src/system/kernel/cpu.cpp:355`
  (cpu_wait), `src/system/kernel/arch/arm64/arch_cpu.cpp:84-108` (CTR_EL0)
- `src/system/kernel/arch/arm64/VMSAv8TranslationMap.cpp`,
  `arch_vm_translation_map.cpp:42` (granule = 4K)
- ENA: `ena.cpp:53,266-297,430,504-562,682-712,836-933,1457,1532-1562,1625-1665`;
  `ena.h:86-89,150,176-236`; `ena-com/ena_plat.h:22-26,123,365-490`;
  `file_systems/shared/crc32.cpp` (software crc32c)

Added by the 2026-08-22 AWS cross-check:

- `build/jam/ArchitectureRules:41-52` (the arm64 case moved from `:41` to `:52`
  when the Neoverse comment block landed; `grep -rn outline build/jam configure`
  → no hits, i.e. no `-mno-outline-atomics` anywhere)
- `headers/private/kernel/arch/arm64/arm_registers.h:54-60` (`CPACR_EL1` FPEN
  only — no `ZEN`), `:215-216` (`ID_AA64ISAR0_EL1` masks, unread)
- `src/system/boot/platform/efi/arch/arm64/arch_start.cpp:61`
  (`CPACR_FPEN_TRAP_NONE` — FP/SIMD enabled, SVE not); no `ZCR_EL1` or SVE
  save/restore anywhere in `src/system/kernel/arch/arm64/`
- `headers/private/kernel/arch/arm64/arch_cpu.h:26` (`arm64_isb()` already
  defined), `:140-143` (`arch_cpu_pause()` = `arm64_yield()`) — item 8a
- `src/add-ons/kernel/drivers/network/ether/ena/ena.cpp:1498-1502` (moderation
  initialised, never configured); `ena-com/ena_com.h:419,425-428,1089-1104,
  1164-1174` (the unused moderation API) — item 11
- `src/system/kernel/arch/arm64/VMSAv8TranslationMap.cpp:375` (only mention of
  block mappings is about *splitting* them; no contiguous-bit use) — item 10a
- Negative greps, each returning zero hits: `AT_HWCAP` / `getauxval` /
  `ID_AA64ISAR0` readers / `TID3`|`TIDCP` (items 3, 12); `PMCR_EL0` /
  `PMEVCNTR` / `PMUSERENR` / `PMCCNTR` (item 13)
- External, cited in "Cross-check against AWS's own Graviton guidance": the AWS
  `aws-graviton-getting-started` pages listed in that section's source table, and
  the GCC 13.3.0 AArch64 options manual (for `neoverse-512tvb`,
  `-moutline-atomics`, and the `+crypto`/`+lse`/`+crc` defaults)
