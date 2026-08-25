# arm64 `Query()` valid-bit defect — hardware verification

Verification record for the fix in `36d365a594` / `34093ec4bf`
(`src/system/kernel/arch/arm64/VMSAv8TranslationMap.cpp`) and its reproducer
`src/bin/mprotect_probe`. The defect itself is written up in the commit messages and
in `package-chain-status.md`; this file exists because the fix had been **merged and
promoted to canonical without its own reproducer ever being run**, and a kernel fix
whose reproducer was never fired against it is an untested kernel fix.

## What was verified — **2026-08-25, ~00:05–00:15Z**

**An unprivileged `mprotect()` could panic the arm64 kernel. It no longer can.** Measured
as a two-arm A/B on real Graviton hardware, with the *same* probe binary on both arms.

| arm | AMI | kernel | probe result |
|---|---|---|---|
| **test** | `ami-0ccbe274a66e8d439` (`haiku-querypp`), branch `fix/arm64-query-page-present` @ `39f2e965e6` | announces `kernel build query-page-present-1, compiled Aug 24 2026 19:16:08` | **PASS**, all 4 probes returned, `rc=0`, node still answered afterwards |
| **control** | `ami-03158b45971dc3106` (`haiku-uartbatch`), branch `fix/uart-thre` @ `8c4b1ed4e7` | no such banner line — the stamp does not exist in that kernel | **PANIC** on the first probe; node wedged in KDL |

Both `c7g.large`, same subnet, same security group, launched 11 minutes apart in bake
time. Ancestry checked rather than assumed: `git merge-base --is-ancestor 36d365a594
8c4b1ed4e7` is **false** (control genuinely lacks the fix) and true for `39f2e965e6`
(test genuinely has it).

### The control panic, in full

```
PANIC: area 0xffff0000de314b58 looking up page failed for pa 0x0
Welcome to Kernel Debugging Land...
revision: hrev59996
Thread 325 "mprotect_probe" running on CPU 1
 3 ... <kernel_arm64> panic + 0x8c
 4 ... <kernel_arm64> _user_set_memory_protection + 0x9e0
 5 ... <kernel_arm64> syscall_dispatcher + 0x750
 6 ... <kernel_arm64> do_sync_handler + 0x450
 7 ... <kernel_arm64> handle_el0_sync + 0xa0
ESR=00000000560000d8 FAR =000000f8a9dde880
 8 ... </boot/system/lib/libroot.so> _kern_set_memory_protection (nearest) + 0x04
10 ... </boot/system/cache/tmp/mprotect> main + 0x64
```

Frame 10 at `main + 0x64` is the **first** of the four probes
(`head-mapped-tail-untouched`), which is what was pre-registered as the expected failure
point.

### WITHDRAWN: the matching `ESR` is not evidence of anything

**An earlier version of this file claimed `ESR=0x560000d8` was "byte-for-byte the ESR from
the original `miniruby` panic, the strongest available evidence that the probe reproduces
that defect and not a lookalike." That claim is wrong and is withdrawn.** `ESR_EL1` decodes
as:

| field | value | meaning |
|---|---|---|
| `EC` (31:26) | `0x15` | `EXCP_SVC64` — "SVC instruction execution in AArch64" (`headers/private/kernel/arch/arm64/arm_registers.h:134`) |
| `IL` (25) | 1 | 32-bit instruction |
| `ISS` (24:0) | `0xd8` = 216 | the SVC immediate — **the syscall number** |

On this platform the syscall index *is* the SVC immediate (`syscalls.inc:5-11` encodes
`svc #(code)`; `arch_int.cpp:397` reads `uint32 syscall = (frame->esr & 0xffff)`), and index
216 is `_kern_set_memory_protection` — the 217th declaration inside the
`#pragma syscalls begin/end` region of `headers/private/system/syscalls.h`, counted
independently.

So `ESR = (0x15 << 26) | (1 << 25) | 216` is **fully determined by "we were inside the
`mprotect` syscall"**. It is a redundant restatement of backtrace frame 8, not independent
corroboration: *any* panic under `mprotect()` — a lock bug, an unrelated NULL deref, a
deliberate `panic()` — prints the identical value. Matching ESRs here rule out nothing
except a different syscall.

**`FAR` is worse than uninformative — it is stale.** `arch_asm.S:64,70` snapshots `FAR_EL1`
on *every* exception entry, but the architecture does not write `FAR_EL1` on an `SVC`, so the
`FAR=` beside a syscall-path panic is a leftover from whatever last took a data abort. The
tell is in the data itself: `FAR=000000f8a9dde880` is **not page-aligned**. Neither `FAR`
value is the address passed to `mprotect()` and the difference between them means nothing.

### What the evidence actually is

The attribution rests on a conjunction, all of it kernel-side:

1. The panic format string carrying the `area %p` prefix exists at **exactly one** site in
   the tree — `src/system/kernel/vm/vm.cpp:6232`, inside `_user_set_memory_protection()`,
   which is the function named by frame 4. (The other occurrence, `vm.cpp:1843`, has no
   `area` prefix and sits in `vm_create_anonymous_area`'s `B_ALREADY_WIRED` arm.)
2. That site is reachable only with `PAGE_PRESENT` set on a physical address that
   `vm_lookup_page()` cannot resolve — and it reported **`pa 0x0`**.
3. Pre-fix `Query()` (`git show 36d365a594^`) set `PAGE_PRESENT` with **no** validity test,
   and by elimination it is the only thing on this platform that can produce `PAGE_PRESENT`
   together with `pa == 0`. A *valid* PTE with a zero address field cannot be manufactured:
   `Map()` is the only writer of `kPteTypeL3Page`, and pre-fix `Protect()` preserved the
   address and type bits, so writing attributes into a zero entry left it invalid.
4. arm64 was the outlier — riscv64 returns early per-PTE and x86 PAE gates on
   `X86_PAE_PDE_PRESENT`.
5. The reproducer is a faithful construction of the mechanism, and it panics an unpatched
   kernel and returns cleanly on a patched one.

**Lesson worth keeping: two panics agreeing on a register that merely encodes the syscall
number look like corroboration and are not.** The check that caught it was decoding the
field instead of comparing the hex.

### Predictions registered before the control arm ran

Written down first, so the run could falsify them:

- the probe prints `start` and the first `mprotect(...) ...` line, then **never** prints
  `returned` — ✅ correct
- the node becomes unreachable — ✅ correct
- the console shows `looking up page failed for pa 0x0` under
  `_user_set_memory_protection` — ✅ correct
- **falsifier, stated in advance:** if the control had printed `PASS` too, the probe would
  not reproduce the defect and the test arm's `PASS` would have been vacuous. It did not.

## Two instrument traps this run hit

**1. `get-console-output` without `--latest` returns the cached boot snapshot, not the live
console.** The control node was wedged and its console showed a *clean boot ending at
`register_domain(9, unix)`* with no panic anywhere in 37,907 bytes. Re-fetching the exact
same instance with `--latest` returned 40,272 bytes containing the whole panic and stack
trace. **A panic can be entirely absent from the default console read.** Had that gone
unnoticed the honest report would have been "node unreachable, no panic evidence" — which
reads as an inconclusive run, or worse invites the guess that it panicked. Cf. the standing
rule that a zero-row filter is not evidence of absence: here the *corpus* was wrong, not the
filter.

**2. "Unreachable" is not "panicked."** Between losing sshd and finding the console text,
the only positive evidence was `describe-instance-status`: `InstanceStatus: impaired` with
`SystemStatus: ok`, i.e. the guest was wedged while the host was fine. That is worth
capturing as a cheap intermediate discriminator, but it is *not* a panic, and it does not
name a thread or a stack.

## Why the same binary on both arms mattered

The probe ships only in the patched image (it was added on the same branch as the fix), so
the control had no copy. It was pulled from the test node and pushed to the control node
through the builder, **sha256 identical at all three hops**
(`079f1af90c805a96e7cbaefa0c745958d3becb7bd79752ea8355c8360b65a9bc`, 139,283 B). So the
only difference between the two arms is the kernel. Without that, "the patched image has a
probe that passes" and "the older image panics on something" would be two unrelated
observations.

## What this unblocks

`package-chain-status.md` cut ruby out of `vim` to route around this panic, and recorded
that the cut was acceptable **only because the defect was separately owned and being
fixed** — with the explicit follow-up that *when the kernel fix lands, the honest move is to
try ruby again*. That condition is now met: the fix is merged, baked, canonical, and
verified against its own reproducer on hardware. `ruby-3.2.9-arm64-mcontext.patch` is
already in the tree for that retry.

Also relevant to the browser: netsurf is built but has never been *run*, and a JIT is
exactly the kind of consumer this defect ate. Removing it is a precondition for trusting any
future "it runs" claim, not merely a tidy-up.

## Follow-up defect found by auditing this fix — the root-table guard

`36d365a594` added *two* things to `Query()`: the valid-bit test, and an early return when
`fPageTable == 0`. Its own commit message explains the second one — `ProcessRange()` catches
an absent root table "only with an `ASSERT`, which is compiled out unless `KDEBUG` is on, and
would otherwise walk whatever lies at the start of the physical map."

**That reasoning applies verbatim to five other methods, and the guard was applied to one.**
Coverage as merged:

| method | valid-bit test | `fPageTable == 0` guard |
|---|---|---|
| `Map()` | n/a (never consumes the old entry's address) | allocates the root table |
| `Unmap()` | n/a | **yes** |
| `Query()` / `QueryInterrupt()` | yes | **yes** |
| `UnmapPage()` | yes | **NO** |
| `UnmapPages()` | yes | **NO** |
| `Protect()` | yes | **NO** |
| `ClearFlags()` | yes | **NO** |
| `ClearAccessedAndModified()` | yes | **NO** |

The valid-bit half is complete — all eight `ProcessRange()` callbacks and all four
non-`ProcessRange()` PTE readers test validity. The root-table half was not.

Why it is a live hazard rather than a style nit, each part checked in source:

- **The state exists.** `arch_vm_translation_map.cpp:36-42` constructs every *user* map with
  `pt = 0`; it stays zero until the first `Map()`.
- **The only backstop is compiled out.** `ProcessRange()`'s `ASSERT(ptPa != 0)` expands to
  `do { } while(0)` when `KDEBUG` is 0 (`debug.h:32`), which is the default and therefore what
  every baked AMI runs.
- **The consequence is worse than the bug just fixed.** `TableFromPa(0)` returns
  `KERNEL_PMAP_BASE + 0`, so physical page zero is read as the root table; wherever the
  garbage there has bits[1:0] == `0b11` the walk descends, and the callbacks then
  `atomic_get_and_set64(ptePtr, 0)` or CAS attribute bits into **arbitrary physical memory**.
  Silent corruption, not a clean panic.
- **Reachability, graded honestly: reachable state, unproven trigger.**
  `_user_set_memory_protection()` can no longer get there, because its `Query()`/`PAGE_PRESENT`
  gate now reports "absent" for an empty map. But `vm_set_area_protection()` calls
  `map->ProtectArea()` at `vm.cpp:3298` with **no `Query()` gate at all**, and `UnmapArea()`'s
  first branch calls `UnmapPages()` unconditionally for `CACHE_TYPE_DEVICE` or
  `wiring != B_NO_LOCK`. Both walk without asking whether a root table exists. Reaching them
  needs an address space holding an area in which no page was ever mapped. **No deterministic
  unprivileged trigger was exhibited**, so this is not being claimed as exploitable — but the
  case for "unreachable" would be a reachability audit nobody has done, whereas the case for
  "reachable" is one field that source shows is zero.

**Fixed by centralising rather than repeating:** `ProcessRange()` now returns early on
`ptPa == 0` instead of asserting it. That covers all six walkers at once and cannot rot as
callers are added, which is how riscv64 already does it
(`RISCV64VMTranslationMap::LookupPte`). Recursive calls cannot reach it with zero — a zero
`GetOrMakeTable()` result is already filtered by a `continue` — so it only fires for a
top-level call on an empty map.

## Still open, deliberately not fixed here

- **`vm.cpp:6232` stays reachable for device areas.** `_user_set_memory_protection()` has no
  `cache_type` filter, so a valid mapping to physical memory outside the `vm_page` array — an
  MMIO / `CACHE_TYPE_DEVICE` area in a *user* address space — reaches `vm_lookup_page()` and
  panics with a **non-zero** `pa`. That is architecture-independent and the arm64 valid-bit
  fix cannot address it; it also cannot be the panic observed here, which reported `pa 0x0`.
  `VMTranslationMap::PageUnmapped()` already special-cases `CACHE_TYPE_DEVICE`, which is
  evidence such areas do reach translation maps. Reachability on this platform is *inferred*
  — no device with a user `mmap` hook was identified.
- **A guard/loop TOCTOU in `Protect()` and `ClearFlags()`**, argued benign: both valid-test a
  PTE read before the CAS loop, then re-read inside it without re-testing. Safe only because
  nothing flips a PTE valid→invalid concurrently — software writers hold `fLock`, and
  `fixup_entry()`, the one lock-free writer, only sets AF or clears the read-only bit. That is
  an argument, not a measurement, and it breaks if a lock-free invalidator ever appears.
- **`Query()` still has a false-*absent*** for an address mapped by a level-1/2 *block* entry:
  `GetOrMakeTable()` returns 0 with a null reservation, the callback never runs, and the answer
  is "not present". Pre-existing upstream, and harmless because nothing creates blocks after
  boot. Recorded so it is clear these commits fixed the invalid-entry axis and did not touch
  the block axis.
- **`GetOrMakeTable()`'s `ASSERT(type != kPteTypeL12Block)`** is the third load-bearing
  `ASSERT` in this file that vanishes in a release kernel; with a non-null reservation it would
  CAS a fresh empty table over a block entry and destroy 512 mappings. Pre-existing.

## Not verified here

- **The other accessors are still not exercised by a test.** `mprotect_probe` reaches
  `Query()` via `set_memory_protection` only. The valid-bit test in `UnmapPage()`,
  `UnmapPages()`, `Protect()`, `ClearFlags()` and `ClearAccessedAndModified()` is confirmed
  **present by source audit**, not by execution — and the new root-table guard likewise has no
  test. A probe that drives `ProtectArea()` against an area in a never-mapped address space
  would close that gap.
- **Nothing about x86.** Every other architecture already gated `PAGE_PRESENT` on a
  valid/present bit; arm64 was the sole violator. That was read from source, not tested.
- **`ruby` itself.** The retry is unblocked, not done.
