# The arm64 `memcpy()`: design, verification, and what it is worth

Status: **verified in userland on real hardware, against the exact object code
that ships; not yet booted.** Sections 1-5 were written *before* any of it was
run, so the plan can be judged on more than its conclusion, and they are left as
written even where the results went on to contradict them -- §3.3 and §3.4 in
particular were partly wrong, and §6 says how. Section 6 is the result.

Recommendation: **do not merge until it has booted.** Everything short of a boot
now passes, including five defects that this work found and fixed, three of them
after the change had already been reviewed. See §6.7 for exactly what a bake
would settle.

## 1. What the change is

`src/system/libroot/posix/string/arch/arm64/memcpy.c` replaces
`arch/generic/generic_memcpy.c` for arm64, in **both** libroot and the kernel
(`src/system/kernel/lib/arch/arm64/Jamfile`).

The generic routine reaches its word-at-a-time loop only when source and
destination are congruent mod 8:

```c
if (MISALIGNMENT(d, size_t) == MISALIGNMENT(s, size_t)) { ...word loop... }
while (count > 0) { *d++ = *s++; }      /* otherwise: the whole copy, bytewise */
```

That guard is correct for architectures where an unaligned access traps. arm64
performs unaligned accesses to Normal memory in hardware, so on arm64 the guard
bought nothing and imposed a byte loop on the commonest case. x86_64 already
carries its own `memcpy` (`arch/x86_64/memcpy.cpp`) for the same reason.

The case matters because a received TCP payload starts 54 bytes into the frame
(14 Ethernet + 20 IPv4 + 20 TCP), so copying it into a `malloc()`ed buffer is a
source at 6 mod 8 against an 8-or-better aligned destination: mismatched, every
frame, ~68,500 times a second at MTU 9001.

The routine as originally proposed: byte loop below 16 bytes; then align the
destination to 8 ("stores are the side that benefits"); then 32 bytes per
iteration; then an 8-byte loop; then a byte tail. Three of those four decisions
turned out to be wrong, and §6.3 says how.

The routine as it now stands: no loop at all below 129 bytes, but a ladder of
fixed-width overlapping accesses -- at most four, all loads issued before all
stores; above 128 bytes, align the destination to **16** (the width the compiler
actually emits) and then 64 bytes an iteration, with the same fixed-width ladder
for the tail. `dest == source` returns without writing. Nothing is ever read or
written outside `[dest, dest + count)`, and the access ordering keeps the routine
correct for `dest < source` overlaps, which is the direction real callers
accidentally depend on.

## 2. Why this is the most dangerous change in the tree

`memcpy` is called by everything, and its failure mode is not a crash but
silent data corruption discovered later. It is also now in the kernel, where it
runs in early boot, in exception paths, and before caches and page tables reach
their final state. The verification therefore has to be adversarial rather than
confirmatory.

The author's own commit message (`577dbc9895`) sets the bar: *"the routine is
verified against memcpy() at all 64 alignment pairs over 105 sizes ... but it
has not yet run inside a kernel or a libroot. Do not ship it without the string
test suite and a boot."*

## 3. Findings from static review, before any test ran

These are recorded here because they change what the verification has to do.

### 3.1 There is no string test suite. The instruction to run one cannot be followed.

`src/tests/system/libroot/posix/string/` contains exactly one file,
`compare_test.cpp`, 496 bytes. It tests `strcmp`/`memcmp`/`strncmp`/
`strcasecmp`/`strncasecmp` on a single 1-byte input, and it **prints rather than
asserts**, so it has no pass/fail and no meaningful exit code. It is a 2008
regression probe for signed-char handling.

Tree-wide, there is **no test of `memcpy`, `memmove`, `memset`, `bcopy`,
`strcpy` or `strlcpy` anywhere**. The CppUnit suite `librootposixtest.so`
contains only `CryptTest`. No glibc `test-memcpy.c` or gnulib string test was
ever imported; the vendored gnulib/glibc tests in the tree are all
wide-character conversion.

So "run Haiku's own string test suite" resolves to "there is nothing to run."
The coverage is not thin, it is absent, and a passing `compare_test` would be
worth nothing. **The correctness test has to be written as part of this work**,
and that is the largest single item below.

### 3.2 `memcpybench.c`'s existing verification becomes vacuous the moment the routine ships

This is the most important finding of the static review.

`verify()` in `src/tests/system/benchmarks/memcpybench.c` compares
`reference_memcpy()` — a copy of the new algorithm living inside the benchmark —
against **`memcpy()` itself** as the oracle. That is a valid independent check
*today*, because today's libroot `memcpy` on arm64 is still the generic byte
routine.

The instant `arm64/memcpy.c` is built into libroot, the oracle **becomes the
thing under test**. `verify()` then compares the new algorithm against the new
algorithm, passes unconditionally, and proves nothing — while still printing
`identical (0 mismatches over 64 alignment pairs x 105 sizes)`. A green result
that means nothing is worse than a red one.

The claim "verified at all 64 alignment pairs over 105 sizes" is therefore true
of the *pre-change* tree only, and it silently expires on merge. Any test used
to gate this change must carry its **own** oracle, written locally, that cannot
be replaced by the routine under test.

### 3.3 `-fno-builtin` is not sufficient to prevent recursion, and its blast radius is wider than documented

> **Partly disproved by §6.1, and kept as written.** The reasoning below is
> sound and the conclusion was wrong: GCC 13.3 emitted no recursive call for
> `memcpy` under any of ten flag combinations, including with
> `-ftree-loop-distribute-patterns` forced on. The pass *does* fire on
> `generic_memset.c`, which is the same hazard in the same merge object, so the
> instinct was right and the example was wrong. `-fno-tree-loop-distribute-patterns`
> proved unnecessary and was not added.

The commit adds `-fno-builtin` to the kernel Jamfile to stop the compiler
recognising the byte loops inside `memcpy` as `memcpy` and calling into the
function from itself. The commit message is right that the current build's
behaviour is "luck, not design", but `-fno-builtin` is the wrong instrument:

- `-fno-builtin` stops GCC treating *calls* to `memcpy` as builtins. It does
  **not** disable `-ftree-loop-distribute-patterns`, which is the pass that
  converts an explicit byte loop into a library call to `memcpy`/`memset`. That
  pass is on at `-O2`, and `OPTIM ?= -O2` (`build/jam/BuildSetup:703`) is what
  both the kernel and libroot build at. glibc and the Linux kernel both pass
  `-fno-tree-loop-distribute-patterns` explicitly for exactly this reason.
- The same exposure applies to `generic_memset.c`, which is in the same
  `KernelMergeObject` and has the same byte-loop-in-its-own-definition shape.

Also: `KernelMergeObject` passes its extra-CFLAGS argument to `SetupKernel` for
**all** listed sources (`build/jam/KernelRules:160-176`), so `-fno-builtin` now
also applies to `siglongjmp.S`, `sigsetjmp.S`, `kernel_longjmp_return.c`,
`kernel_setjmp_save_sigs.c` and `generic_memset.c`. Harmless-to-good in each
case, but wider than the commit message states.

This must be settled on the **generated object code**, not by argument.

### 3.4 Strict aliasing is not a problem here, but only because of a global flag

The routine casts `uint8_t*` to `uint64_t*` and dereferences, which is a strict
aliasing violation. It gets away with it because
`build/jam/ArchitectureRules:21` adds `-fno-strict-aliasing` to
`ccBaseFlags` for every non-legacy-GCC architecture, kernel included. musl's
`memmove` in this same tree does it properly with
`__attribute__((__may_alias__))`. Relying on a global build flag for
correctness in the one routine whose miscompilation is undetectable is a poor
trade; the typedef should carry `__may_alias__` regardless.

`__attribute__((aligned(1)))` on a scalar typedef to *decrease* alignment is
also not clearly supported by GCC's documented contract (the documentation
discusses decreasing alignment only in terms of `packed`). On arm64 the
generated `ldr` is unaligned-safe either way, so this is a documentation-intent
issue rather than a live bug — but it should be confirmed in the disassembly
rather than assumed.

> **Resolved.** `may_alias` was added, so the punning no longer depends on
> `-fno-strict-aliasing`. The `aligned(1)` question was confirmed harmless in the
> disassembly, and then made moot: the wide accesses are now `__uint128_t`, whose
> `ldp`/`stp` lowering is unaligned-safe on Normal memory regardless of what the
> compiler believes about alignment.

### 3.5 `memmove` and `bcopy` inherit the new routine, correctly

`memmove` comes from musl (`src/system/libroot/posix/musl/string/memmove.c`,
and the kernel's own `src/system/kernel/lib/Jamfile:127`). It delegates to
`memcpy` **only** when the ranges provably do not overlap:

```c
if ((uintptr_t)s-(uintptr_t)d-n <= -2*n) return memcpy(d, s, n);
```

Its overlapping paths are self-contained and unchanged. `bcopy` is a one-line
wrapper over `memmove`. So `memmove`/`bcopy` semantics are preserved provided
the new `memcpy` is correct for non-overlapping ranges — but they are now
*reached through* the new code, so they must be tested, and `memcpybench.c`
tests neither.

### 3.6 Overlap behaviour changes for `d > s`, and that is a real risk to callers

`memcpy` on overlapping ranges is undefined, but the tree may contain callers
that are wrong about it, and their observable behaviour changes:

- **`d < s`** (destination below source): both old and new are *correct*, and
  the new one remains correct even for overlaps closer than 32 bytes, because
  all four loads of an iteration are issued before any of its four stores and
  the compiler may not reorder a store ahead of a possibly-aliasing load.
- **`d > s`**: both old and new are wrong, but *differently*. The old byte loop
  replicates a repeating pattern with period `d - s`; the new one replicates in
  32-byte and 8-byte blocks. A latent bug elsewhere in the tree therefore
  changes its symptom on merge.

This has to be characterised empirically, not just reasoned about.

## 4. The verification plan

Ordered by what would kill the change soonest.

### V1 — Object code (no hardware needed; kills the change fastest)

For **both** the libroot and the kernel builds, at the real optimisation level
and with the real flags:

1. `objdump -d` the compiled `memcpy.o` and assert it contains **no `bl`/`b`
   branch to `memcpy` or to any other symbol** — i.e. it is entirely
   self-contained and leaf. A recursive call here is an unbounded recursion in
   every process and in the kernel.
2. Do the same for `generic_memset.o`, which shares the hazard.
3. Confirm the unaligned loads are plain `ldr`/`ldp` and that no
   alignment-sensitive instruction (`ldxr`/`stxr`, `ldar`/`stlr`, `casp`,
   `dc zva`) appears.
4. Repeat 1–3 **with `-fno-builtin` removed** to establish whether the current
   safety is real or accidental, and **with `-O3`** and with
   `-ftree-loop-distribute-patterns` forced on, to find out how close to the
   edge the build is sitting. If the loops turn into calls under any of these,
   add `-fno-tree-loop-distribute-patterns` to both Jamfiles.
5. Also disassemble the **shipped artifact** — `libroot.so` from the built image
   and `kernel_arm64` — not just the intermediate `.o`, to confirm what actually
   ships is what was compiled.

### V2 — A real correctness test, with its own oracle

Write `src/tests/system/libroot/posix/string/memcpy_test.c` as a `SimpleTest`
that **asserts and exits non-zero**, carrying its own byte-at-a-time oracle
written so the compiler cannot turn it into a `memcpy` call (`volatile`
pointers). It must cover:

- **Sizes**: 0 and 1; every size 0..256 exhaustively; every internal branch
  boundary ±1 (15/16/17, 31/32/33, 39/40/41, 7/8/9); page-size boundaries
  (4095/4096/4097, 8191/8192/8193); 65535/65536; and one multi-MiB copy.
- **Alignment**: all 64 pairs mod 8 (as now), **extended to all 4096 pairs mod
  64** for a subset of sizes, so an implicit 16-, 32- or cache-line-alignment
  assumption cannot hide.
- **Return value**: `memcpy` must return `dest`. Untested today.
- **Guard bands**: 64 poisoned bytes either side of the destination, verified
  byte-for-byte untouched after every single copy. Not one byte outside
  `[dst, dst+n)`.
- **Page-boundary faulting (the decisive case)**: `mmap` two adjacent pages,
  `mprotect(PROT_NONE)` the second, and place the **source** so the copy ends
  exactly at the last byte of the first page. A routine that reads ahead of its
  length faults here and nowhere else. Then the mirror image for the
  **destination**, and then both with the guard page *before* the buffer to
  catch reading or writing backwards. Repeat across every alignment and across
  every size 1..192 so that each of the head, 32-byte body, 8-byte and tail
  paths is the one that ends at the boundary.
- **`memmove`/`bcopy`**: full overlap sweep — every displacement in
  -192..192 over sizes 0..192, against a memmove oracle. These must be
  *correct*, not merely unchanged.
- **Overlap characterisation for `memcpy`**: a *reporting* (not failing) mode
  that diffs new-vs-old-generic for overlapping ranges, so §3.6 is documented
  as measured fact.

### V3 — The exact kernel object, exercised in userland

The kernel compiles the same source with different flags, so the libroot test
does not cover the kernel's object code. Rather than rely on a boot to exercise
it, extract the kernel-compiled `memcpy.o`, rename its symbol
(`objcopy --redefine-sym memcpy=kernel_memcpy`), link it into the V2 test, and
run the **entire V2 battery against the actual kernel binary code** in userland,
where a fault is a clean SIGSEGV instead of a KDL prompt. This is the cheapest
way to get the kernel's code under the page-boundary and guard-band tests, and
it needs no bake.

### V4 — Device memory / MMIO

On arm64 an unaligned access to Device-nGnRnE memory faults, unlike Normal
memory. If anything in the kernel copies to or from an MMIO mapping, the new
routine faults where the byte loop did not. Determine, from the arm64
page-table attribute code and from the drivers (ENA, GICv3/ITS, PCI, ACPI,
framebuffer), whether any `memcpy`/`memmove`/struct-copy target is Device-type
memory as opposed to ordinary or uncached-Normal RAM. *This is the failure mode
most likely to be missed, because nothing in a normal test finds it.*

### V5 — Boot and workload

Only after V1–V4. Bake an image (operator's job), then:
- boot, and check the serial console for early-boot faults;
- network path hard (`nettput`, both directions, MTU 9001), watching for
  checksum failures rather than only for throughput;
- a filesystem workload with verification (write a large tree, read it back and
  compare hashes) — corruption in a copy shows up as bad files, or as nothing
  at all until much later;
- the `ena_fault` injection path, to exercise reset and error unwind;
- `profile -a -k`, now that `arch_debug_get_stack_trace()` works (`830d8d0814`,
  which the author says to bake first).

### V6 — Measurement, with the house discipline

Baseline, real hardware, MTU 9001: receive **4950.8 Mbit/s at 2082 µs/MiB**,
transmit **4484.6 at 2182**. Rates are window-limited, so **µs/MiB is the
number that must move.** The receive cost model from a 15-point MTU sweep is
**2.34 µs/frame + 1.85 ns/byte** (R² 0.94, `graviton/docs/net-receive-profile.md`);
the per-byte term is 88% of cost at MTU 9001, and the two bounce copies account
for at most 0.78 of that 1.85 ns/B. So **most of the per-byte cost is not the
copies**, and this change cannot close more than ~42% of the per-byte term even
if it makes the copies free.

Rules, because four earlier claims in this project were retracted for failing
the same way — measured once, in one condition:

- **Interleaved A/B.** The routine is selected at build time, so this means two
  images and interleaving the *runs*, not two runs back to back.
- **Negative control**: report something not expected to move. `memset`
  throughput (`generic_memset` is unchanged) and `memspeedTest`'s raw
  load/store bandwidth are the controls; if either moves, the measurement is
  confounded and the receive numbers are not trustworthy either.
- **Verify by artifact**: `objdump -d` the `memcpy` inside the *running*
  `libroot.so` and `kernel_arm64` on the node under test, and confirm MTU is
  still 9001, before believing any rate.
- Report what fraction of the 1.85 ns/B gap actually closed, and state plainly
  what remains unexplained.

## 5. Explicit non-goals and known gaps

- Not covered: `memcpy` under concurrent modification of the source, and
  copies straddling a `mprotect` change made by another thread. Out of scope.
- Not covered: any architecture other than arm64. The generic routine is
  untouched for everyone else.
- The `-mcpu=neoverse-n1+crypto` baseline (`ArchitectureRules:52`) means the
  scheduling was tuned on a core that is not necessarily the one measured;
  Graviton3/4 are supersets, so this affects performance figures, not
  correctness.

## 6. RESULT

Every number below was taken on the c7g.metal builder (Neoverse-V1, the same
core family the target runs), pinned with `taskset`, and reproduced on two
different cores. The routine measured and tested is the **actual object code
the Haiku cross-compiler emits for the kernel and for libroot** -- extracted
with `objcopy --redefine-sym memcpy=...` and linked into a Linux harness --
not a transcription of the algorithm. That is possible because the object has
zero relocations and zero undefined symbols, and it is what makes it possible
to put the kernel's own machine code under a `PROT_NONE` guard-page test
without baking an image.

### 6.1 V1 -- object code

| | kernel build | libroot build |
|---|---|---|
| relocations | 0 | 0 |
| undefined symbols | none | none |
| stack spills (`stp/ldp` vs `sp`) | 0 | 0 |
| SIMD/`q` register accesses | 0 | 0 |
| `ldxr/stxr/ldar/stlr/casp/dc zva` | 0 | 0 |
| disassembly md5 | *identical* | *identical* |

Findings:

- **No recursion, and `-fno-builtin` is not what prevents it.** memcpy was
  compiled ten ways -- with and without `-fno-builtin`, at `-O2`, `-O3` and
  `-Os`, and with `-ftree-loop-distribute-patterns` forced on -- and emitted no
  outbound call in any of them. So the hazard the commit message flags is real
  in principle but does not fire here, and the flag it adds to the kernel
  Jamfile is defensive rather than load-bearing. Keep it anyway; it costs
  nothing and the next compiler may differ.
- **The hazard does fire, on `memset`.** The same battery was then run over
  `generic_memset.c`, `generic_memcpy.c` and the new `memcpy.c` across all three
  flag sets this tree uses (libroot, kernel, boot loader) at `-O2`, `-O3` and
  `-Os`, with and without each guard. Result:

  | file | flags | guard | outcome |
  |---|---|---|---|
  | `generic_memset.c` | libroot | none, `-O2` / `-O3` | **`R_AARCH64_CALL26 memset`** |
  | `generic_memset.c` | libroot | `-fno-builtin` *or* `-fno-tree-loop-distribute-patterns` | clean |
  | `generic_memset.c` | libroot | none, `-Os` | clean (incidentally) |
  | `generic_memset.c` | kernel | none, any `-O` | clean -- `-fno-tree-vectorize` |
  | `generic_memset.c` | boot | none, any `-O` | clean -- `-Os` and `-fno-tree-vectorize` |
  | new `memcpy.c` | all three | all combinations (18) | clean |
  | `generic_memcpy.c` | boot | all combinations | clean |

  That relocation is memset calling itself: unbounded recursion in every process
  that clears a buffer. **Either guard alone suppresses it, which identifies
  `-ftree-loop-distribute-patterns` as the pass responsible** -- and that pass is
  not disabled by `-fno-builtin`, which is why glibc and the Linux kernel pass
  `-fno-tree-loop-distribute-patterns` explicitly. So the commit's instinct was
  right and its example was wrong.

  Both flags are now set on both the kernel and libroot builds of this
  directory, gated on GCC. Confirmed afterwards on the built objects: all four
  of `{kernel,libroot} x {memcpy.o, generic_memset.o}` carry zero call
  relocations and zero undefined symbols, and `libroot.so` and `kernel_arm64`
  each contain a `memcpy` and a `memset` with no `bl` in either.

  **Not fixed, and named so it is not forgotten:** no other architecture's
  `src/system/kernel/lib/arch/*/Jamfile` has this guard -- arm64's came from
  `577dbc9895`. They are safe today purely because kernel builds pass
  `-fno-tree-vectorize`, which nobody would think to preserve for the sake of
  `memset`. Every *libroot* arch Jamfile does carry `-fno-builtin` already.
- **Kernel and libroot now emit byte-identical code, and that is a deliberate
  property rather than a coincidence.** They did not before: libroot
  auto-vectorised the body to 128-bit `ldr q`/`str q` while the kernel, built
  `-fno-tree-vectorize`, used `ldp`/`stp` — so the same C produced two different
  routines and testing one said nothing about the other. Spelling the wide access
  as `__uint128_t`, rather than writing 8-byte accesses and hoping the vectoriser
  widens them, makes the width part of the source instead of part of the
  optimiser's mood. Three things follow, and they are worth protecting:

  * one disassembly to review instead of two, and a test of either one is a test
    of both;
  * **zero `q`/SIMD registers and zero stack spills in either build**, which
    removes the kernel-NEON question entirely — no question of FP state, of
    `CPACR` trapping, or of whether an exception path may touch vector
    registers;
  * the code no longer changes shape when someone adds or removes
    `-fno-tree-vectorize` from a Jamfile.

  A future SIMD rewrite (§6.7) **reopens all three**. That is not an argument
  against it — glibc's 2x is real — but it means the kernel-SIMD policy question
  has to be answered explicitly at that point rather than inherited.
- Verified on the **shipped artifacts** (`libroot.so`, `kernel_arm64`), not
  only on the intermediate `.o`.

### 6.2 V2/V3 -- correctness

509,882 checks per routine. Four routines were run through the identical
battery, and the two controls are what make the result mean anything:

| routine | result |
|---|---|
| `generic_memcpy` (the incumbent), as a control | **PASS**, 0 failures |
| glibc's aarch64 memcpy, as a control | FAIL, 11 -- all of them `memcpy(p, p, n)` on a read-only page |
| the **libroot** build's object code | **PASS**, 0 failures |
| the **kernel** build's object code | **PASS**, 0 failures |

The incumbent passing is the control that says the battery is not simply
accepting anything. glibc failing exactly one check, and only that one, is the
calibration: glibc does not short-circuit `dest == source`, so **that check is a
compatibility requirement of this tree rather than a standards requirement** --
it is asserted because `generic_memcpy` had it, because callers may rely on it,
and because on arm64 this routine is also `user_memcpy()` and the kernel's
memcpy, where the consequence is a KDL panic rather than a signal.

Covered: sizes 0..256 exhaustively plus 35 boundary sizes to 65535, at all 64
alignment pairs mod 8; all 4096 alignment pairs mod 64 over 40 sizes; the return
value; 64 bytes of poisoned guard band either side of every destination, checked
after every copy; 4 MiB copies; `memmove`/`bcopy` over every overlap in
-256..256; and 12,288 copies arranged to end at, or begin at, the boundary of a
page whose neighbour is `PROT_NONE`.

The page-boundary subtest carries its own negative control -- a copy deliberately
aimed at the guard page, which must fault. Without it, a run in which `mprotect`
had quietly not taken effect would have passed while proving nothing.

### 6.3 Five defects found, four of them not by review

Two were found by the adversarial reviewer, three by this test. All are fixed.

1. **The verification tested a different function.** `memcpybench.c`'s
   `verify()` called a static `reference_memcpy()` inside the benchmark, which
   had drifted from the shipped routine in exactly the two places the shipped
   routine was non-obvious: the `count < 16` early-out (absent from the
   reference) and the alignment-loop guard. Those two are interlocked -- the
   early-out is the only justification for dropping the guard -- so the one
   piece of reasoning that could be wrong was precisely what nothing checked,
   while the commit message reported it as verified at 64 alignment pairs.
2. **Short copies regressed up to 50%.** Below 16 bytes the routine byte-copied
   unconditionally, but `generic_memcpy` took its *word* path whenever the two
   misalignments matched -- which is what a short copy between two aligned
   pointers is. 8 bytes went 0.482 -> 0.723 ns/B.
3. **The stated design rationale was false.** "Only the destination is aligned,
   because stores are the side that benefits" -- but the compiler widened the
   body to 16-byte accesses, so aligning to 8 left half the stores crossing a
   boundary. At 8961 bytes the case where the prologue *incidentally* aligned
   the source beat the aligned-destination case it was designed for.
4. **`memcpy(p, p, n)` faulted on read-only memory**, because the
   `dest == source` short-circuit had been dropped.
5. **`dest < source` overlap tolerance was lost** -- found by this test, and
   *introduced by the first version of the fix for 2*. The overlapping-access
   construction that makes short copies fast breaks the one overlap direction
   that both the incumbent and glibc happen to get right, and that real callers
   accidentally rely on (`memcpy(p, p + k, n)`, in-place header removal). The
   first attempt at a rule for this was itself wrong and the test found the
   counterexample at size 33, displacement 1. See §6.6.

### 6.4 V4 -- device memory

On arm64 MMIO really is Device-nGnRnE, and unaligned access to it really does
fault: `vm_map_physical_memory()` (`src/system/kernel/vm/vm.cpp:1986-1997`)
silently defaults to `B_UNCACHED_MEMORY` for any caller that names no memory
type, and `GetMemoryAttr()`
(`src/system/kernel/arch/arm64/VMSAv8TranslationMap.cpp:558-580`) maps that to
`MAIR_DEVICE_nGnRnE`. `arch_vm_set_memory_type()` on arm64 is a no-op, so
nothing weakens the request.

**No Graviton-live caller is at risk.** ENA keeps MMIO behind sized `volatile`
accessors and converts its LLQ window to `B_WRITE_COMBINING_MEMORY` (Normal-NC)
before touching it, with a hand-rolled 64-bit store loop whose comment says "the
device requires 64-bit wide stores, so this cannot go through memcpy"; GICv3/ITS
tables are `create_area(B_CONTIGUOUS)` RAM; the arm64 linear physmap is
Normal-WB and covers only RAM, which clears `vm_memcpy_from/to_physical()`;
`norflash.cpp` and `53c8xx.c`, the two textbook offenders, are not built for
arm64. ACPI is already explicitly fixed for this exact hazard --
`ACPICAHaiku.cpp:493-507` forces write-back under
`#if __HAIKU_ARCH_ARM || __HAIKU_ARCH_ARM64` with a comment saying ARM uncached
memory does not support unaligned access. Two in-tree acknowledgements that the
hazard class is known here.

**Three real exposures, none reachable on Graviton, listed here as a target list
for the follow-up.** None of them is caused by this change. All three are made
*reachable* by it, because the routine it replaces byte-copied whenever the two
misalignments differed and a byte access to Device memory never faults.

1. `src/system/kernel/debug/frame_buffer_console.cpp:489-493` maps the
   framebuffer with **no memory type**, so Device-nGnRnE, and only repairs it to
   write-combining later in `init_post_modules`. In that window `console_blit`
   does one `memmove` per scanline (`:330-337`), and musl's `memmove`
   (`src/system/libroot/posix/musl/string/memmove.c:16`) forwards to `memcpy`
   for non-overlapping ranges. The only caller is `blue_screen.cpp:100` with
   `srcx == destx == 0`, so source and destination differ by exactly
   `bytes_per_row` and are congruent mod 8 unless the stride is odd or the depth
   is 24bpp. Reachable only in a KDL before `init_post_modules`, on an
   odd-stride or 24bpp mode, on an arm64 machine that has a framebuffer.
   Graviton has no video device and the arm64 EFI loader uses UEFI `ConOut`, so
   not this target — but a genuine one for arm64 in general, including the
   QEMU/virtio-gpu guests this project boots.
2. `src/add-ons/kernel/drivers/graphics/framebuffer/framebuffer.cpp:62` has the
   same shape: maps without naming a memory type.
3. **`src/add-ons/kernel/drivers/audio/hda/hda_controller.cpp:603-605`,
   `:910-912`, `:950-952` call
   `vm_set_area_memory_type(..., B_UNCACHED_MEMORY)` on `create_area` RAM DMA
   buffers when `!dma_snooping`** — which on arm64 turns *ordinary RAM* into
   strongly-ordered Device memory, where every unaligned or wide access faults.
   **`hda` is built for arm64.** This is the one that matters most, because it
   is not a display path and not confined to a KDL: it is a driver turning
   general-purpose memory into Device memory on a live system. **It is a latent
   bug independent of this change and it deserves its own follow-up**, not a
   line in this document.

The right fix for all three is the same and it is at the *source*: name the
memory type at map time (`B_WRITE_COMBINING_MEMORY` for a framebuffer, and for
hda, do not ask for `B_UNCACHED_MEMORY` on arm64 at all — Normal-Non-Cacheable
is what the driver actually wants and it does not fault on unaligned access).
Repairing the attribute afterwards, as `frame_buffer_console` does, leaves a
window. That work is deliberately **not** done here: it touches three drivers
and a memory-type policy question, and bundling it with a memcpy change would
make both harder to review.

**The boot loader is unaffected.** `src/system/boot/Jamfile:318` names
`generic_memcpy.c` into `boot_libroot_efi.o`, and
`src/system/boot/arch/arm64/Jamfile:20` sets `kernelLibArchSources = ;` empty.
The `SEARCH` there pointing at `string/arch/arm64` is vestigial. So early boot
before the kernel keeps the old routine, which removes a whole class of
early-boot risk from this change.

### 6.5 V6 -- measurement

Interleaved within one binary: the incumbent, the new libroot object, the new
kernel object and glibc, each measured in turn per row, best of repeats, pinned,
reproduced on two cores. Every row also re-measures the incumbent a second time
as a **harness control** -- if that differs from the first measurement, the row
is noise. The control was under 1% on almost every row; it exceeded 3% only at
sizes below 16 bytes and on three cold rows, and those rows are called out as
unresolvable rather than reported as results.

ns/byte, and the ratio the change is worth:

| case | size | incumbent | new | vs incumbent | glibc | glibc vs new |
|---|---|---|---|---|---|---|
| **the receive path, cold** | | | | | | |
| driver -> net_buffer | 1920 | 0.0862 | 0.0735 | **1.17x** | 0.0690 | 1.07x faster |
| net_buffer -> user (src+6) | 1920 | 0.3905 | 0.0784 | **4.98x** | 0.0667 | 1.18x |
| MTU 1500 payload (src+6) | 1448 | 0.4262 | 0.1199 | **3.55x** | 0.1027 | 1.17x |
| MTU 9001 payload (src+6) | 8961 | 0.3911 | 0.0586 | **6.67x** | 0.0461 | 1.27x |
| MTU 9001, aligned | 8961 | 0.0663 | 0.0512 | **1.30x** | 0.0462 | 1.11x |
| a socket read (src+6) | 65535 | 0.3874 | 0.0440 | **8.81x** | 0.0392 | 1.12x |
| **jumbo, warm (instruction cost only)** | | | | | | |
| aligned | 8961 | 0.0497 | 0.0283 | **1.76x** | 0.0194 | 1.46x |
| src+6 | 8961 | 0.3871 | 0.0397 | **9.75x** | 0.0197 | 2.01x |
| **short, matching alignment** | | | | | | |
| | 1 | 4.407 | 4.715 | 0.93x | 4.475 | 1.05x |
| | 8 | 0.5060 | 0.5202 | 0.97x | 0.5362 | *0.97x -- new is faster* |
| | 16 | 0.2587 | 0.2594 | 1.00x | 0.2683 | *0.97x -- new is faster* |
| | 32 | 0.1371 | 0.1241 | **1.11x** | 0.1215 | 1.02x |
| | 33 | 0.1415 | 0.1403 | 1.01x | 0.1267 | 1.11x |
| | 48 | 0.1107 | 0.0904 | **1.23x** | 0.0843 | 1.07x |
| | 64 | 0.0919 | 0.0672 | **1.37x** | 0.0400 | 1.68x |
| | 128 | 0.0742 | 0.0505 | **1.47x** | 0.0319 | 1.58x |
| | 256 | 0.0622 | 0.0487 | **1.28x** | 0.0275 | 1.77x |
| **short, mismatched (src+6)** | | | | | | |
| | 8 | 0.6801 | 0.5208 | **1.31x** | 0.5342 | *0.97x -- new is faster* |
| | 32 | 0.4726 | 0.1452 | **3.26x** | 0.1259 | 1.15x |
| | 256 | 0.3942 | 0.0573 | **6.87x** | 0.0300 | 1.91x |

Reproduced on core 20: the same rows land within a few percent, and the ratios
that matter are unchanged (net_buffer -> user 4.22x, MTU 9001 payload 6.78x,
socket read 8.86x, 8961 warm src+6 9.80x).

So: **1.1x to 9.8x faster than the routine it replaces at every size from 12
bytes upward, and at every mismatched alignment including the shortest** -- and
the mismatched case, which is the one the network receive path actually performs,
is between 3.3x and 9.8x.

What remains, stated plainly:

- **One byte costs 7% more than it did** (4.41 -> 4.72 ns for the call), and
  8, 16 and 24 bytes at matching alignment are at parity, 0.97x-1.00x, which is
  within a percent or two of the harness's own repeatability at those sizes. That
  is the price of dispatching on size at all, and no general-purpose routine
  avoids it: glibc is *slower than the incumbent* at 8 and 16 bytes for the same
  reason. `generic_memcpy`'s path for a short matched-alignment copy is one
  alignment test and one word copy, and there is nothing left to shave. It is
  worth being clear that the earlier versions of this routine were 50% slower at
  8 bytes and 15% slower at 32 -- those were real regressions, they were found by
  measurement rather than review, and they are gone.
- **glibc is still faster than this routine above 32 bytes -- up to 2.0x.** That
  is not noise, and it will not close by tuning. glibc's aarch64 memcpy loads up
  to 128 bytes into `q` registers before storing any of it, which needs only
  eight registers; `__uint128_t` lowers to *pairs* of general-purpose registers,
  so the same structure would spill, which is why the group size here is 64
  bytes. Closing the gap means SIMD intrinsics. See §6.7. Below 32 bytes this
  routine is already at or slightly ahead of glibc, because it dispatches in
  fewer branches.

### 6.6 The overlap rule, including the version of it that was wrong

`memcpy` is undefined on overlap, so none of this is a promise. It is asserted
anyway because both the incumbent and glibc happen to tolerate
`dest < source`, and a caller that works today should not start failing for a
reason no bug report could ever explain.

Writing destination byte *p* destroys the source byte living at that address,
which is source index *p - delta* for *delta = source - dest > 0*. A group of
accesses reading source *[x, y)* is therefore safe exactly when no earlier write
touched *[x + delta, y + delta)*.

The plausible rule -- "ascending non-overlapping writes, plus one overlapping
access at the end" -- **is wrong**, and it is written here because someone will
reach for it again. A trailing overlapping access is safe only if the previous
writes stopped at or before its start *plus delta*, and with delta as small as 1
that means: only if they stopped exactly at its start. The first version of the
33..128 case wrote *[0, 32)* and then an overlapping tail at
*[count - 32, count)*; at count 33 the tail re-read bytes the first access had
already replaced. The test found it at size 33, displacement 1, on the first
run.

The rule that holds: every group writes a range beginning exactly where the
previous one ended, and every group issues all of its loads before any of its
stores, which absorbs the overlap *within* a group where it is harmless. That is
why 33..128 is one load-everything-then-store-everything group and not a ladder
of four, and why the alignment prologue copies exactly the bytes it consumes
rather than a full 16 with a short advance.

Measured: the incumbent and glibc agree with `memmove` truth on all 65,280
`dest < source` cases; the new routine now does too. `dest > source` differs
from the incumbent in 14,111 of 65,280 overlapping cases -- both are wrong,
differently, as any forward copy must be, and as glibc also is (17,247). A
latent caller that is wrong about *that* direction does not become newly broken,
but it changes how it is broken.

### 6.7 What is not done, and what it would take

- **Not booted, and not run on Haiku at all.** Everything above is the shipping
  machine code exercised in userland on the right core, under Linux. Nothing here
  has run inside a Haiku kernel or libroot, which is the one thing the original
  commit message asked for and the one thing that still needs an image. Wanted:
  boot, serial console clean through early boot, `nettput` both directions at
  MTU 9001 watching for checksum failures rather than only for rate, a filesystem
  workload verified by hash, `ena_fault`, and `profile -a -k`.

  **Correction, recorded because the first version of this paragraph was wrong.**
  It said the canonical AMI accepted none of the private keys on this host. It
  does. `~/.ssh/haiku-graviton-ed25519` here and
  `/home/ubuntu/.ssh/haiku-ed25519` on the metal builder are the same key --
  `SHA256:WNS7PS4zeMUERF5gCL6MLA6f7ShpUqpKgV6htzu/nyM`, verified with
  `ssh-keygen -lf` on both. The failure was the **username**: the Haiku account
  is `baron`, and only `user` and `root` were tried. `haiku-uaf-ed25519` is the
  EC2 *key-pair name* passed to `run-instances`, which is a different thing
  entirely and is irrelevant here -- Haiku has no cloud-init, so the AMI's baked
  `authorized_keys` is what decides, and the key-pair name at launch has no
  effect at all.

  Two ways in, both working:

  ```sh
  # from this host: SSM port-forward through the metal, then SSH locally
  aws ssm start-session --region us-west-2 --target <metal-instance-id> \
      --document-name AWS-StartPortForwardingSessionToRemoteHost \
      --parameters '{"host":["<haiku-node-ip>"],"portNumber":["22"],"localPortNumber":["50022"]}' &
  ssh -p 50022 -i ~/.ssh/haiku-graviton-ed25519 baron@127.0.0.1

  # or from the metal builder over ssm-run, as the perf gate does
  SO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 \
      -o LogLevel=ERROR -i /home/ubuntu/.ssh/haiku-ed25519"
  sudo -u ubuntu ssh -n $SO baron@<haiku-node-ip> 'uname -a'
  ```

  `scp` to a Haiku node fails; copy with
  `base64 -w 200 <file> | ssh ... 'base64 -d > /path'`, and run `sync` on the
  Haiku side before rebooting or the file lands zero-length.

  That gap does not put a bake at risk of proving nothing, because the two
  subtests whose validity depends on the host OS each carry a negative control:
  the guard-page test deliberately faults against its own `PROT_NONE` page and
  fails loudly if that does *not* fault, and the read-only self-copy test
  deliberately writes to the read-only page for the same reason. If Haiku's
  `mprotect` or `sigsetjmp` behaved differently, the test would say so rather
  than passing quietly.
- **The end-to-end network number is not measured.** The microbenchmark says the
  copies got 3.4-9.8x cheaper in the receive path's configuration. It does not
  say what that is worth in µs/MiB, because that needs a baked image. From the
  cost model (2.34 µs/frame + 1.85 ns/B, R² 0.94) the two bounce copies account
  for at most 0.78 of the 1.85 ns/B per-byte term, so **even making the copies
  free could not close more than ~42% of it** -- and this change makes them
  about 6.75x cheaper at MTU 9001, not free. Expect the per-byte term to fall by
  roughly 0.5-0.65 ns/B, i.e. a third of it, which at MTU 9001 is on the order of
  25-30% of receive cost. **The majority of the per-byte cost will still be
  unexplained afterwards**, and saying otherwise would be the same mistake this
  project has had to retract four times. One known candidate is being handled
  separately: `compute_checksum()` measures 0.229 ns/B against 0.095 for a
  64-bit unrolled equivalent.
- **glibc is still 1.1-2.0x faster.** Closing it means `<arm_neon.h>` and
  `q`-register load-everything-first groups up to 128 bytes. That is a larger
  and more interesting change than this one: it is the difference between a
  routine that is much better than what we had and one that is competitive with
  the best available. It also raises a kernel question this version deliberately
  avoids -- whether SIMD registers may be used in kernel memcpy -- which is
  answerable (exception entry eagerly saves all 32 `q` registers) but is a
  policy decision, not a detail.
- **Not tested:** concurrent modification of the source during a copy, and
  copies straddling an `mprotect` performed by another thread. Out of scope.
- **`generic_memcpy.c` is untouched**, so no other architecture is affected, and
  the arm64 boot loader continues to use it.

## 7. Reusing this: putting kernel object code under a userland test

This is the part of the work most likely to be useful to something other than
`memcpy`, so it is written out in full rather than left implicit.

### The problem it solves

A leaf routine in the kernel is the worst thing in the tree to test. There is no
unit-test harness, `mmap`/`mprotect` are not available to reason about faults,
a fault is a KDL prompt rather than a signal you can catch and continue from,
and every iteration costs an image bake of roughly twenty-five minutes. So
kernel leaf routines get tested by booting and hoping, which for a copy routine
means hoping that silent data corruption would have shown up as something.

Worse, testing the *libroot* build of the same source does not test the kernel
build. Before this change the two were compiled with different flags and GCC
emitted genuinely different instructions from the same C -- 128-bit NEON
`ldr q`/`str q` in libroot, `ldp`/`stp` in the kernel. A test of one said
nothing about the other, and nothing in the tree would have told you that.

### The observation

A self-contained leaf routine compiles to an object file with **no relocations
and no undefined symbols**. Check it:

```sh
$XT/readelf -r memcpy.o     # -> "There are no relocations in this file."
$XT/nm -u memcpy.o          # -> nothing
```

An object like that is pure position-independent machine code with no
dependency on its runtime, its libc, or its OS. Since Haiku and Linux on arm64
share the AAPCS64 calling convention and the ELF format, such an object can be
linked into a Linux binary and called directly. It is the same instruction bytes
the kernel will execute, running on the same core.

That is what makes it possible to point a full userland test battery -- guard
bands, `PROT_NONE` pages either side, `sigsetjmp` fault recovery, millions of
cases -- at the kernel's own object code, in seconds, with no bake.

### The recipe

```sh
XT=<...>/cross-tools-arm64/bin/aarch64-unknown-haiku-

# 1. Build the object through the real build, so the flags are the real flags.
#    Do not hand-write a compile line; extract it if you need to see it:
#      jam -n -a kernel_lib_posix_arch_arm64.o | grep -F 'memcpy.c'
jam -q kernel_lib_posix_arch_arm64.o

# 2. Confirm the object is self-contained. If this fails, stop -- the technique
#    does not apply and the failure itself is worth knowing about.
${XT}readelf -r <obj>/memcpy.o
${XT}nm -u <obj>/memcpy.o

# 3. Rename the symbol so it does not collide with the host libc's, which lets
#    the test link both and compare them.
${XT}objcopy --redefine-sym memcpy=kernel_memcpy <obj>/memcpy.o k.o

# 4. Neutralise the ELF OS/ABI byte so the host linker does not object.
#    (Byte 7 of e_ident. Harmless: nothing in a leaf object depends on it.)
printf '\x00' | dd of=k.o bs=1 seek=7 count=1 conv=notrunc status=none

# 5. Build the test against it with the *host* compiler, selecting the routine
#    under test by macro so one source covers every variant.
gcc -O2 -fno-builtin -mcpu=neoverse-n1+crypto \
    -DCOPY_UNDER_TEST=kernel_memcpy -o mt_kernel memcpy_test.c k.o

taskset -c 4 ./mt_kernel
```

`-fno-builtin` on the test itself matters, and so does reaching the routine
through a `volatile` function pointer: otherwise the compiler inlines or folds
the call and you measure GCC's idea of a copy instead of the object's, which
leaves no trace in the output.

### What it does and does not establish

It establishes everything about the **machine code**: correctness at every size
and alignment, that it does not touch memory outside its arguments, that it does
not fault where a correct routine would not, and its cost in ns/byte on the
right core. Four variants were run through one battery here -- the incumbent,
glibc, the libroot object and the kernel object -- and having the incumbent and
glibc in the same run is what turned "the tests pass" into "the tests pass, they
would have failed, and here is what a *different* correct implementation scores
on them".

It establishes nothing about **integration**: that libroot exports the symbol,
that the kernel links it, that it survives early boot before caches and page
tables are in their final state, or that Haiku's own `mmap`/`mprotect`/signal
semantics match the host's. Those still need a boot. Where a subtest depends on
host-OS behaviour, give it a negative control -- the guard-page test here
deliberately faults against its own `PROT_NONE` page and fails loudly if that
does *not* fault, so the same binary run on Haiku cannot pass vacuously.

### Where else it applies

Any self-contained arch leaf: `generic_memset.c`, `memcmp`, `strlen`, the
`byteorder.S` helpers, `generic_atomic.cpp`, checksum routines. The
`compute_checksum()` cost noted in §6.7 is the obvious next candidate -- it is a
leaf, it is measurable, and it is currently 2.4x off a straightforward 64-bit
unrolled version.
