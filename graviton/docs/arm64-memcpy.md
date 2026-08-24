# The arm64 `memcpy()`: design, verification, and what it is worth

Status: **verification in progress.** This document was written before the
verification was run, so that the plan can be judged on more than its
conclusion. Sections marked *RESULT* are filled in afterwards; sections marked
*DISPROVED* record hypotheses that died, and are kept rather than deleted.

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

The new routine: byte loop below 16 bytes; then align the **destination** only
(stores are the side that benefits); then 32 bytes per iteration with four
independent unaligned 64-bit loads and four aligned 64-bit stores; then an
8-byte loop; then a byte tail.

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

*To be filled in.*
