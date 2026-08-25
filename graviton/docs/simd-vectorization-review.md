# ARM SIMD and vectorization on Haiku arm64: what is worth doing

Status as of **2026-08-25**: **review only. Nothing in this document has been
implemented, and the central capacity question — where graphics and image time
actually goes — has not been measured on this port.** Everything below is
labelled by provenance. Read §7 before acting on any ranking.

Scope note: the fleet baseline for this review is **Graviton 3 (Neoverse V1) and
newer**. Graviton 1 and 2 support is explicitly dropped, which is a change from
[graviton-optimization-plan.md](graviton-optimization-plan.md) §2, and that
change is what makes this review worth writing. It turns out to unlock much less
than it looks like it should, for a reason that has nothing to do with the ISA.

---

## 0. The four findings that matter

If you read nothing else:

1. **The single highest-value action is not SIMD at all.** The jam-built base
   system is compiled `-mcpu=neoverse-n1+crypto`, but **every haikuports
   userland package — all ~150 of them, including every image codec, mesa and
   WebKit — is compiled at the compiler's default `-march=armv8-a`, i.e.
   ARMv8.0-A.** At that baseline GCC turns on `-moutline-atomics`, and on Haiku
   the flag it dispatches through is never initialised. So **every atomic in all
   of userland pays an out-of-line call *and* then takes the LL/SC slow path** —
   the exact defect fixed in the kernel at `20bf8f2711`, still live everywhere
   else. §3, §6.1.
2. **SVE is a NO-GO, and dropping Graviton 2 does not change that.** The arm64
   kernel leaves `CPACR_EL1.ZEN = 0`, so SVE instructions trap and userland gets
   a clean `SIGILL`. Adding SVE state support is a 300–500 line kernel project
   with a public-ABI blocker. And per AWS's own counter ceilings, **SVE is at
   rough FLOP parity with NEON on both V1 and V2** — on Graviton 4 SVE is
   *narrower* than on Graviton 3. So the payoff after that work is small. §2, §4.
3. **Runtime SIMD dispatch is impossible on Haiku arm64 today by any mechanism**,
   and ifunc is not merely missing but an active hazard: `R_AARCH64_IRELATIVE`
   returns `B_BAD_DATA` and the image fails to load with the diagnostic compiled
   out. Everything must be decided at compile time. NEON is safe to assume
   unconditionally, because it is architecturally mandatory on ARMv8-A and the
   kernel preserves it correctly. §2, §4.
4. **Painter/AGG rasterization is close to irrelevant on a real EC2 instance**,
   because `app_server` there builds a `RemoteHWInterface` and the pixels are
   rasterized in the *client*, not on the Graviton. Image *decode* matters in
   every configuration including headless; 2D rasterization matters only in the
   QEMU/`ramfb` verification rig. §5.

---

## 1. What the AWS guide actually recommends

Distilled from AWS's public `aws-graviton-getting-started` repo (`main`), read
2026-08-25. **Every number in this section is AWS's, about AWS's workloads. None
of it is a Haiku measurement.**

A framing point first, because it saves the next reader time:
**`SIMD_and_vectorization.md` is the stalest page in the repo.** It is written
for the Graviton1/2 + Android era, never mentions Graviton 3/4/5, and its entire
SVE content is one `HWCAP_SVE` line. The actionable material lives in
`c-c++.md`, `README.md`, `runtime-feature-detection.md` and
`perfrunbook/debug_hw_perf.md`.

### 1.1 Flags per generation (documented)

| CPU | Core | ISA | "performance" flag | "balanced" flag | GCC | Clang |
|---|---|---|---|---|---|---|
| Graviton2 | Neoverse-N1 | ARMv8.2-a | `-mcpu=neoverse-n1` | `-march=armv8.2-a` | 9 | 10+ |
| Graviton3(E) | Neoverse-V1 | ARMv8.4-a | `-mcpu=neoverse-v1` | `-mcpu=neoverse-512tvb` | 11 | 14+ |
| Graviton4 | Neoverse-V2 | **Armv9.0-a** | `-mcpu=neoverse-v2` | `-mcpu=neoverse-512tvb` | 13 | 16+ |
| Graviton5 | Neoverse-V3 | **Armv9.2-a** | `-mcpu=neoverse-v3` | `-mcpu=neoverse-512tvb` | 15 | 19+ |

The governing rule, quoted: when targeting several generations use the balanced
flag **"for the oldest generation planned for deployment, since code built for a
newer generation may not run on an older generation."**

The repo's most concrete guidance is not prose but real build code, in
`video-encoding/ffmpeg-build/scripts/setup-compiler.sh`:

```
graviton3)  CFLAGS="-march=armv8.4-a+crypto+fp16+rcpc+dotprod+sve -mtune=neoverse-v1"
graviton4)  CFLAGS="-march=armv9-a+crypto+fp16+rcpc+dotprod+sve2 -mtune=neoverse-v2"
```

with the rationale that **"`-march` sets the target ISA and is
correctness-critical... Tuning only affects scheduling, never correctness."**
That split — a conservative `-march` plus an aggressive `-mtune` — is the single
most useful idea in the repo for our situation, and §6.1 uses it directly.

**The `+sve` in that graviton3 line is load-bearing evidence for us**: it proves
`-march=armv8.4-a` does **not** by itself imply SVE. `-march=armv9-a` *does*
(ARMv9 mandates SVE2), which is why the graviton4 line is unusable here. See
§6.1.

### 1.2 SVE availability and width (documented) — and the counterintuitive part

- Availability: *"Graviton3, Graviton4, and Graviton5 support SVE, earlier
  Gravitons do not."* SVE2 is Graviton 4 and 5 only.
- **Width, quoted from `perfrunbook/debug_hw_perf.md`:** *"SVE vectorization will
  use **256-bit vectors on Graviton3 (Neoverse V1)** processors and **128-bit
  vectors on Graviton4 (Neoverse V2, SVE2)**, but the scalable nature of SVE
  makes both the code and binary vector-length agnostic. NEON vectorization is
  always a 128-bit vector size."*

So **SVE narrows from 256-bit on Graviton 3 to 128-bit on Graviton 4.** Any code
or intrinsic that assumes a fixed 256-bit vector is wrong on Graviton 4. This is
an easy and serious mistake and it is exactly why SVE must be written
vector-length-agnostic; it is also the reason VLA-SVE plus runtime detection is
the right *forward-compatible* posture, since it requires knowing nothing about
generations that have not shipped.

`README.md`'s throughput row: Graviton3 = *"4x Neon 128bit vectors / 2x SVE
256bit"*; Graviton4 and 5 = *"4x Neon/SVE 128bit vectors"*.

### 1.3 NEON vs SVE performance: AWS never claims SVE is faster

A repo-wide search for any claim pairing SVE with a speedup returns **zero
hits**. The strongest statement is architectural, not performance: SVE is
vector-length agnostic and predicated, NEON is not.

**Inference (ours, from AWS's own numbers, clearly labelled as inference):**
`perfrunbook/debug_hw_perf.md` gives, for an L1-resident FP32 loop, a ceiling of
*"`flop-sve-pkc` of 16,000 with SVE, or `flop-nonsve-pkc` of 16,000 with NEON
SIMD, or `flop-nonsve-pkc` of 4,000 with scalar"*. **Identical SVE and NEON
ceilings, 4x over scalar for either.** That is consistent with the width table —
on Graviton 3, SVE is 2x wider but has half the pipes; on Graviton 4 both are
128-bit across 4 pipes. So a **parity expectation for SVE vs NEON is
well-founded from AWS's own document**, while the 4x for *either over scalar* is
the number actually worth chasing.

This is the fact that decides §4: the expensive thing (SVE kernel support) buys
parity with the cheap thing (NEON), which already works.

### 1.4 Autovectorization, intrinsics, libraries (documented)

- **`-fopt-info-vec-missed`** is the one concrete tool the guide names, to see
  which loops did not vectorize. No Clang equivalent given.
- Intrinsics via `arm_neon.h`. (The guide's own feature-guard example has a real
  bug — `defined(__GCC__)` is not a GCC predefine, and its parens are unbalanced.
  Do not copy it.)
- **Named libraries are both x86→NEON translators, not portable-SIMD wrappers:**
  **SIMDe** and **sse2neon**. AWS's appraisal of sse2neon: *"While SSE2NEON won't
  produce optimal code, it generally gets close enough to reduce the performance
  penalty of not using the vector intrinsics."* Strategy given: translate first,
  profile, then *"the hot paths can be rewritten directly with NEON intrinsics."*
- **Google Highway and xsimd are not mentioned at all.** If you expected the
  guide to bless a portable-SIMD wrapper, it does not.
- **zlib: the recommendation changed.** `README.md` now says *"use zlib-ng 2.3.3
  or later, which now outperforms our previous recommendation of
  zlib-cloudflare"*, and that *"The original zlib shipped by most distributions
  has no Arm optimizations."* Any older note recommending zlib-cloudflare is
  stale.
- Other named wins: PCRE2 10.34+ (*"Added NEON vectorization to PCRE's JIT"*),
  FFmpeg 6.0+, ISA-L (already Arm64-optimized), ArmPL for BLAS/LAPACK/FFT.
  **libjpeg-turbo and openssl get no Graviton-specific guidance.**
- Portability trap worth knowing for any C port: *"On x86 char is signed by
  default while on Arm it is unsigned by default"* — or compile `-fsigned-char`.

### 1.5 Runtime dispatch (documented) — and the warning written for us

`runtime-feature-detection.md` prescribes `getauxval(AT_HWCAP)`/`AT_HWCAP2`,
*"filtered by the kernel to include only features which the kernel also
supports"*, with dispatch by *"C++ classes, ifuncs, function pointers, or a
simpler control flow approach"*. Per-function compilation of a higher tier:

```c
#pragma GCC target("+sve2")
#pragma clang attribute push(__attribute__((target("sve2"))), apply_to = function)
```

`target_clones` is never mentioned. `/proc/cpuinfo` is never recommended for
dispatch.

**Reading ID registers directly is explicitly discouraged, and the reason is our
situation verbatim** — quoted in full because it is the citation behind §4:

> *"checking for SVE support by this method obscures the fact that the kernel
> must also be configured for SVE support since the width of SVE registers can
> be different from NEON and so context switching code must accommodate the
> different width. Without this kernel support, a context switch could result in
> corruption of the content of SVE registers. Because of this, the processor is
> configured to trap executions of SVE instructions by default and this trap must
> be disabled, a job done by the kernel if it is configured to support SVE."*

**We are that kernel, and we have not disabled that trap.** §4.

### 1.6 LSE / outline-atomics (documented)

*"All Graviton processors after Graviton1 have support for the Large-System
Extensions (LSE)... The improvement can be up to an order of magnitude when
using LSE instead of load/store exclusives."* The either/or, from
`perfrunbook/configuring_your_sut.md` — **"one of the following flags"**:

1. `-moutline-atomics` for code that must run on all Graviton platforms
2. `-march=armv8.2-a -mcpu=neoverse-n1` for Graviton2 or later

and from `optimization_recommendation.md`: if not targeting Graviton1, use
`-march=armv8.2-a` **instead of** `-moutline-atomics` *"to reduce overhead"*.

**AWS's verification recipe, which we should adopt as the acceptance test for
§6.1** — count LSE mnemonics against exclusives in a built binary:

```
objdump -d app | grep -ci 'cas\|casp\|swp\|ldadd\|stadd\|ldclr\|ldeor\|ldset\|ldsmax\|ldsmin\|ldumax\|ldumin'
objdump -d app | grep -ci 'ldxr\|ldaxr\|stxr\|stlxr'
```

AWS's remedy for a non-LSE runtime is *a distro package* — *"Ubuntu 18.04 (needs
`apt install libc6-lse`)"*. That framing presumes a glibc that ships the outline
helpers **and an initialiser that sets the flag from `AT_HWCAP`**. §3 is what
happens when neither exists.

---

## 2. Portability matrix, and whether dispatch is even possible

### 2.1 Can Haiku arm64 do runtime feature dispatch? No. By any mechanism.

All verified by reading `refs/heads/graviton`.

| Mechanism | State | Evidence |
|---|---|---|
| `getauxval` / `AT_HWCAP` / `AT_HWCAP2` | **Absent mechanism**, not just unimplemented | Zero hits in `headers/` or `src/`. Haiku's userland entry passes a `user_space_program_args` struct via `src/system/glue/start_dyn.c`, not an ELF auxv — there is no place an `AT_*` tag could come from |
| Kernel ID-register feature probe | **Absent** | Nothing reads `ID_AA64ISAR0_EL1` (so nothing reads `.Atomic`), `ID_AA64PFR0_EL1` (`.SVE`), `ID_AA64ISAR1_EL1`, or `ID_AA64ZFR0_EL1`. Decode macros exist with **zero readers** — `headers/private/kernel/arch/arm64/arm_registers.h:243-247` (Atomic), `:497-501` (SVE) |
| Generic per-CPU feature word | **x86-only by construction** | `x86_check_feature()` at `headers/private/kernel/arch/x86/arch_cpu.h:718` over `uint32 feature[FEATURE_NUM]` (`:583`). Generic `cpu_ent` in `headers/private/kernel/cpu.h` has **no** feature field. arm64's `arch_cpu_info` is two fields (`arch/arm64/arch_cpu.h:128-131`): `mpidr`, and `last_vfp_user` which **is never read or written anywhere** |
| Userland query API | **Does not exist** | `cpu_info` (`headers/os/kernel/OS.h:427-431`) and `cpu_topology_node_info` (`:528-538`) have no feature field. `arch_system_info.cpp` for arm64 is 45 lines and hardcodes `model = 0`, `vendor = B_CPU_VENDOR_UNKNOWN`, `frequency = 0`. `headers/private/kernel/arch/arm64/arch_system_info.h` is an **empty header** |
| arm64 commpage | Two entries only | `headers/private/system/arch/arm64/arch_commpage_defs.h:13-14` — `THREAD_EXIT`, `SIGNAL_HANDLER`. Not even the cheap route is taken |
| **IFUNC** | **Broken, and an active hazard** | `STT_GNU_IFUNC` is not defined in any Haiku ELF header (`headers/os/kernel/elf.h:416-417` stops at `STT_HIPROC`). `R_AARCH64_IRELATIVE` *is* defined (`headers/private/system/arch/arm64/arch_elf.h:27`) but has **no case** in `src/system/runtime_loader/arch/arm64/arch_relocate.cpp:54-75` → `default:` → `return B_BAD_DATA` → **the image fails to load**, and `TRACE` is compiled out (`:16`) so it is **silent**. The kernel loader rejects it explicitly at `src/system/kernel/arch/arm64/arch_elf.cpp:114-122`. x86_64 has the same gap — this is Haiku-wide |

**Consequence: GCC `target_clones`, `__attribute__((ifunc))`, and glibc-style
ifunc SIMD dispatch will not link-and-run on Haiku arm64.** Any haikuports recipe
that enables ifunc dispatch produces a shared object that fails to load with no
message. **This is worth an audit of its own** — grep recipes for
`--enable-ifunc` / `HAVE_IFUNC` / `target_clones`.

The only sound runtime inference available to userland is *"`__aarch64__` is
defined, therefore NEON exists"* — which is true, architecturally guaranteed on
ARMv8-A, and sufficient. Anything finer-grained (LSE, crypto, dotprod, i8mm,
SVE) is **unknowable at runtime on this port**.

### 2.2 The matrix

Given a Graviton-3-and-newer fleet:

| Tier | Contents | Verdict |
|---|---|---|
| **Safe everywhere, no dispatch needed** | **NEON / FPSIMD** (mandatory on ARMv8-A; kernel preserves it correctly, §4.1). ISA floor up to **ARMv8.4-A**: `+crypto`, `+fp16`, `+rcpc`, `+dotprod`. All `-mtune=` values — tuning never affects correctness. | **Use unconditionally.** This is the whole practical answer. |
| **Would need runtime dispatch → therefore off-limits today** | SVE2 and anything above the compile-time floor; per-function `#pragma GCC target("+sve2")` kernels | **Off-limits**, not because of the ISA but because §2.1 leaves nothing to dispatch *on*. Would require the §6.5 HWCAP work first. |
| **Off-limits regardless of dispatch** | **SVE and SVE2**, at any width; `-mcpu=neoverse-v1` / `-v2` / `-v3` / `-512tvb`; **`-march=armv9-a` and higher** (ARMv9 mandates SVE2) | **Blocked on kernel work**, §4. Emitting any SVE instruction today is an immediate `SIGILL`. |
| **Not available in our toolchain** | `-mcpu=neoverse-v3` (Graviton 5 tuning) needs GCC 15; we have **GCC 13.3** | Defer. `-mtune=neoverse-v2` is the newest we can name. |

**Toolchain check (documented):** the tree is on **GCC 13.3.0**
(`gcc-13.3.0_2026_03_29_bootstrap-1` in
`build/jam/repositories/HaikuPorts/arm64:25`). Per AWS's table that is new enough
for `-mcpu=neoverse-v1` (GCC 11) and `-mcpu=neoverse-v2` (GCC 13), and **not**
new enough for `-mcpu=neoverse-v3` (GCC 15). So the compiler can deliver
everything §6.1 recommends. *Unverified:* I did not run `gcc -mtune=neoverse-v2`
through the actual cross-compiler — see §7.

---

## 3. The finding that outranks every SIMD item: userland is still ARMv8.0

**Verified, by reading the build configuration and upstream libgcc source.**

Two separate compilers build this system, and only one of them was fixed.

**The base system** — kernel, libroot, `app_server`, the translators — is built
by jam and gets `-mcpu=neoverse-n1+crypto` from
`build/jam/ArchitectureRules:52`. Correct, and hardware-verified at `20bf8f2711`.

**Everything else** — all ~150 haikuports packages, including `libjpeg_turbo`,
`libpng16`, `zlib`, `freetype`, `mesa`, `icu74` and WebKit — is built by
haikuporter with the *native* Haiku arm64 GCC, and gets **no `-mcpu` or `-march`
at all**:

- `build/scripts/build_cross_tools_gcc4` has cases for `arm-*`
  (`--with-cpu=cortex-a8`), `riscv*-*` (`--with-arch=rv64gc`), `m68k-*`,
  `powerpc-*` — and **no `aarch64-*` case whatsoever**. No `--with-arch`, no
  `--with-cpu`.
- The haikuports `gcc-13.3.0_2023_08_10.recipe` likewise passes no
  `--with-arch`/`--with-cpu`; its only arch-conditional logic is a
  `libquadmath` exclusion for arm64 and `--disable-multilib` for x86_64.
- haikuporter injects nothing: its `runConfigure` only *validates* `CFLAGS` if a
  recipe sets them, and adds no arch flags.
- Recipes that set `CFLAGS` **replace** it entirely — e.g. `zlib-1.3.2.recipe`
  does `export CFLAGS="-O2 -g -DNDEBUG"` — so a flag injected downstream of the
  compiler default would be dropped there anyway.

With no `--with-arch` or `--with-cpu`, GCC's aarch64 default applies. That
default is **ARMv8.0-A**, and it is worth pinning down from GCC's own source
rather than from prose, because the whole finding rests on it —
`gcc/config/aarch64/aarch64.h:727-730`:

```c
/* If there is no CPU defined at configure, use generic as default.  */
#ifndef TARGET_CPU_DEFAULT
# define TARGET_CPU_DEFAULT TARGET_CPU_generic
#endif
```

and `gcc/config/aarch64/aarch64.cc:2757` resolves `generic` to
**`AARCH64_ARCH_V8A`**:

```c
  {"generic", generic, cortexa53, AARCH64_ARCH_V8A, ...
```

So: **`-march=armv8-a`, ARMv8.0-A, tuned for Cortex-A53.** Not Neoverse anything.

### 3.1 Why that is worse than merely "untuned"

At an ARMv8.0 baseline, **GCC 10+ turns `-moutline-atomics` on by default.**
Every atomic becomes a call to a libgcc helper that tests
`__aarch64_have_lse_atomics` and branches. Here is that flag's definition, read
from upstream `libgcc/config/aarch64/lse-init.c` (GCC 13 branch):

```c
_Bool __aarch64_have_lse_atomics
  __attribute__((visibility("hidden"), nocommon));

/* Gate availability of __getauxval on glibc. ... */
#ifdef __gnu_linux__
static void __attribute__((constructor (90)))
init_have_lse_atomics (void)
{
  unsigned long hwcap = __getauxval (AT_HWCAP);
  __aarch64_have_lse_atomics = (hwcap & HWCAP_ATOMICS) != 0;
}
#endif /* __gnu_linux__  */
```

**The only initialiser is inside `#ifdef __gnu_linux__`. Haiku is not
`__gnu_linux__`.** So on Haiku the symbol is a `.bss` `_Bool` that **nothing
ever writes**, and is therefore permanently `false`.

Therefore, in every haikuports arm64 binary, every atomic operation:

1. makes an out-of-line call to `__aarch64_ldadd4_acq_rel` (or similar),
2. tests a flag that is permanently zero,
3. and takes the **LL/SC** path anyway.

**It pays the dispatch and loses the dispatch.** This is precisely the defect
diagnosed and fixed for the kernel at `20bf8f2711` — and it is still live across
all of userland, in exactly the code that is most atomic-heavy: C++ refcounting
in WebKit, mesa/llvmpipe, ICU, and every `shared_ptr` in libstdc++.

The tree already half-anticipated this. `graviton-optimization-plan.md` item 12
lists *"force `-mno-outline-atomics` into recipe CFLAGS"* as an alternative — but
that is the wrong fix twice over: it would leave the ISA at ARMv8.0 (so
still LL/SC, just inline), and it treats the flag as the bug when the baseline
is. §6.1 is the right fix.

### 3.2 What NEON does *not* lose from this

Worth stating plainly so the finding is not overclaimed: **NEON is unaffected.**
`asimd` is mandatory in ARMv8.0-A, so `__ARM_NEON` is defined and every
upstream NEON code path still compiles and runs at the default baseline. What
userland loses is LSE, crypto (AES/SHA/PMULL), `fp16`, `dotprod`, `rcpc`, and
Neoverse scheduling. **So this finding does not make the codec audit moot — it
sits alongside it.**

---

## 3.3 Verdict on `graviton-mcpu-neoverse.patch`: already merged. Delete it.

There is an untracked `graviton-mcpu-neoverse.patch` in the repository root of at
least one working copy. **It is a stale artifact, not pending work.**

- **What it does:** replaces `case arm64 : archFlags += -march=armv8-a+crc ;`
  with `case arm64 : archFlags += -mcpu=neoverse-n1+crypto ;` in
  `build/jam/ArchitectureRules`, plus an 11-line explanatory comment.
- **Is it correct?** Yes, and it is **already in the tree** — merged as
  **`20bf8f2711` "arm64: build with -mcpu=neoverse-n1+crypto"**, now
  `build/jam/ArchitectureRules:41-52`, comment and all. It was
  hardware-verified at the instruction level (kernel outline calls 1173 → 4,
  inline LSE 10 → 1179, `ldapr` 0 → 168).
- **Should it be finished or committed? No — there is nothing left to do, and
  applying it would be a no-op at best.** The file appears "uncommitted" only
  because the checkout containing it is parked ~314 commits behind `graviton`.
- **Action: delete the file.** And note that
  [graviton-optimization-plan.md](graviton-optimization-plan.md) already had to
  issue a correction for exactly this trap — a pointer that read
  "(`graviton-mcpu-neoverse.patch`, uncommitted)" led a reader to conclude the
  work had been lost. **Cite the merged commit, never a root-level
  `graviton-*.patch`.** The other `graviton-*.patch` files in that root were
  scanned; none contains SIMD or codegen work. (`graviton-toolchain-fixes.patch`
  touches build profiles and driver lists; the rest are the arm64 reset, RTC,
  UART-stride, packagefs-unmount and headless-netserver changes.)

## 3.4 The base system's own flag, under the new Graviton-3 baseline

Distinct from §3 (which is about userland) and worth doing at the same time, for
consistency between the two compilers.

`ArchitectureRules:52` currently says `-mcpu=neoverse-n1+crypto`. That was the
right call *when a single AMI had to boot on Graviton 2* — and `20bf8f2711`
explicitly declined `neoverse-v1`/`-512tvb` because they raise the ISA floor to
include SVE. **Both halves of that reasoning have now changed by exactly one
half:** Graviton 2 is dropped, so the N1 ISA floor is no longer required; but §4
shows the SVE objection **still stands**, unchanged.

So the correct move is the same shape as §6.1 — raise the ISA floor and retune,
without admitting SVE:

```
case arm64 : archFlags += -march=armv8.4-a+crypto+fp16+rcpc+dotprod -mtune=neoverse-v1 ;
```

This is a small change with a small expected effect on a mostly-scalar kernel
(the optimization plan's own assessment of V1 retuning for the kernel was
"~nil"), and its real value is **consistency**: after §6.1 both compilers would
target the same ISA, so a reader can stop tracking which half of the system got
which flag. **Lower priority than §6.1, and it should not be bundled with it** —
the base system rebuild and the userland rebuild are separately verifiable, and
`20bf8f2711`'s `objdump` method applies to each independently.

**Do not "simplify" this to `-mcpu=neoverse-v1`.** That is the trap
`20bf8f2711` already avoided once: it implies SVE, and §4.2 makes that an
immediate `SIGILL`.

## 4. SVE: NO-GO, and why dropping Graviton 2 does not change the answer

### 4.1 NEON state: preserved correctly. Verified.

`headers/private/kernel/arch/arm64/arch_thread_types.h:15-20`:

```c
struct aarch64_fpu_state
{
	uint64 regs[32 * 2];
	uint64 fpsr;
	uint64 fpcr;
};
```

512 bytes of register file (32 x 128-bit) plus control — **fixed size, no
vector-length dependence.** It is the last member of `struct iframe` (`:24-40`),
and save/restore is **eager and unconditional on every exception**:
`src/system/kernel/arch/arm64/arch_asm.S:50-51` (`bl _fp_save` in
`EXCEPTION_ENTRY`) and `:79-80` (`bl _fp_restore` in `EXCEPTION_RETURN`),
instantiated for all eight live vectors (`:180-188`). `_fp_save` at `:197-226`
is `stp q0, q1, [x0], #32` ... `stp q30, q31`, then `mrs`/`str` of FPSR and
FPCR, then zeroing them so userland state cannot leak into the kernel.

The cooperative switch (`:259-295`) saves only `d8`–`d15`, which is **correct**:
AAPCS64 makes only the low 64 bits of `v8`–`v15` callee-saved, and userland's
full state is already in the iframe from the EL0 entry. Signals carry it too —
`struct vregs` with `__uint128_t fp_q[32]` at
`headers/posix/arch/arm64/signal.h:15-25`, memcpy'd at
`src/system/kernel/arch/arm64/arch_thread.cpp:184-187` and back at `:230-233`;
`arch_store_fork_frame` memcpys the whole iframe.

**Verdict: compile-time NEON in userland is fully supported and safe today.**

### 4.2 SVE state: `NOT PRESERVED, FAULTS ON USE`

**Deciding evidence:**

- `CPACR_EL1` is written **exactly twice in the whole tree, both in the EFI boot
  loader, never in the kernel** (`git grep CPACR -- src/system/kernel` → zero):
  - `src/system/boot/platform/efi/arch/arm64/arch_start.cpp:67` —
    `WRITE_SPECIALREG(CPACR_EL1, CPACR_FPEN_TRAP_NONE);`, and note this is
    inside the `el == 2 && FEAT_VHE` branch only.
  - `src/system/boot/platform/efi/arch/arm64/arch_smp.cpp:160-161`, secondaries:
    `mov x1, #0x300000` / `msr CPACR_EL1, x1`. **`0x300000` = bits 21:20, i.e.
    `FPEN = 0b11` (no trap). Bits 17:16 — `ZEN` — are `0b00`.**
  - `arm_registers.h:54-60` defines `CPACR_FPEN_*` and `CPACR_TTA`. **There is no
    `CPACR_ZEN` definition at all.**
- `ZCR_EL1`: **0 hits.** `ID_AA64ZFR0`: **0 hits.** No `str z`/`ldr z`,
  no predicate registers, no `FFR` anywhere. Total `SVE` hits across
  `headers/`+`src/`: **five**, all dead mask macros at
  `arm_registers.h:497-501`, zero readers.

**Because `ZEN = 0b00`, an SVE instruction at EL0 traps to EL1 (EC = 0x19).**
`EXCP_SVE` is not among the `EXCP_*` constants (`arm_registers.h:130-148`), and
`do_sync_handler()` (`src/system/kernel/arch/arm64/arch_int.cpp:266`, switch at
`:287`) does not decode it, so it falls to the initialised defaults at `:281-284`
— `B_INVALID_OPCODE_EXCEPTION` / `SIGILL` / `ILL_ILLOPC`.

> **`SVE CONTEXT STATE: NOT PRESERVED, FAULTS ON USE`** — decided by
> `src/system/boot/platform/efi/arch/arm64/arch_smp.cpp:161` (ZEN left zero) with
> `headers/private/kernel/arch/arm64/arch_thread_types.h:15-20` (FPSIMD-only
> save area).

**This is the *safe* failure mode**, and it corrects the natural prior. Userland
SVE today is a deterministic `SIGILL` on the first SVE instruction, not silent
corruption. An accidentally SVE-enabled binary dies loudly and immediately.

**But there is one latent corruption hazard, and it is a trap for a future
change.** The V registers alias the low 128 bits of Z0–Z31. If anyone enables
`CPACR_EL1.ZEN` **without** doing the rest of the work, `_fp_save`/`_fp_restore`
would faithfully preserve bits 0–127 and **silently drop bits 128..VL-1 of every
Z register, plus all of P0–P15 and FFR, on every interrupt including the timer
tick.** Enabling ZEN is the one change that must never be made in isolation.

### 4.3 Sizing the SVE kernel project — and why it is still not worth starting

~300–500 lines. Files: `arm_registers.h` (`CPACR_ZEN`, `ZCR_EL1`,
`ID_AA64ZFR0`, `EXCP_SVE` defines, ~30 lines); `arch_cpu.cpp` (probe
`ID_AA64PFR0_EL1.SVE`, probe max VL, set ZEN — `arch_cpu_init_percpu()` is the
only all-CPU hook and already writes TCR_EL1, ~60 lines); `arch_asm.S`
(`_sve_save`/`_sve_restore` with `MUL VL` addressing, predicates, `rdffr`/`wrffr`,
~80 lines); `arch_int.cpp` (decode EC=0x19 as first-touch, ~40 lines);
`arch_thread_types.h` + `asm_offsets.cpp`. Two hard design calls:

1. **iframe sizing forces a lazy scheme.** The iframe is stack-allocated by
   `sub x19, x19, #(IFRAME_sizeof)` (`arch_asm.S:30`) with the size from
   `asm_offsets.cpp:44`. Sizing for max architectural VL (2048-bit → ~8.7 KiB
   per frame, per nesting level, on the kernel stack) is not viable, so vector
   state must move out of the iframe into a lazily-allocated per-thread buffer.
   The never-used `last_vfp_user` field is presumably a vestige of exactly this.
2. **The signal ABI is a blocker.** `struct vregs`
   (`headers/posix/arch/arm64/signal.h:15-25`) is public POSIX ABI embedded *by
   value* in `signal_frame_data`, so it cannot grow without breaking every
   existing arm64 binary. Linux solved this with a variable-length sigcontext
   extension chain, which Haiku has no equivalent of.

**And after all that, §1.3 says the payoff is parity with NEON**, on both cores
we can test. SVE's real advantages — predication (no scalar tail loop) and
vector-length agnosticism — are genuine but are code-elegance and
forward-portability wins, not the 2–4x that would justify a kernel project with
a public-ABI blocker.

**Recommendation: do not start SVE work as a performance measure.** Do it if and
when a specific workload is shown to be predication-bound, or as a
forward-compatibility investment with its own justification. **If it is ever
done, the vector-length-agnostic discipline is mandatory** — write `MUL VL`
addressing and `whilelo` loops, never a fixed 256-bit assumption, because the
same binary must run on Graviton 3 (256-bit) and Graviton 4 (128-bit). We cannot
and should not design against unannounced generations; VLA-SVE plus runtime
feature detection is the correct forward-compatible posture precisely *because*
it does not require knowing their specifics.

---

## 5. Where graphics performance can even matter: the capacity bound first

This is the honest framing question, and it eliminates a whole category.

**A real Graviton EC2 instance has no display device.** Load-bearing proof
(measured, ours): Haiku's EFI loader reports `GOP protocol not found` on a
booting c7g, so `frame_buffer.enabled = false`. See
[framebuffer-guest-capture.md](framebuffer-guest-capture.md).

Consequently our baked images set `TARGET_SCREEN` and `app_server` builds a
**`RemoteHWInterface`**: it forwards a *display list* over loopback to a remote
client, which rasterizes. `RemoteHWInterface::FrontBuffer()` returns `NULL` —
there is no local surface, by design.

| Configuration | Does Painter/AGG rasterize on the Graviton? | Does image *decode* happen on the Graviton? |
|---|---|---|
| Real EC2, default bake (`RemoteHWInterface`) | **No.** Pixels only ever exist in the client | **Yes** |
| Real EC2, headless (build hosts, CI) | No — no `app_server` painting at all | **Yes** |
| QEMU guest on the metal, `-device ramfb` (`AccelerantHWInterface`) | **Yes** — real local rasterization | **Yes** |
| Streamed desktop | Only if the design streams *pixels*; not if it forwards the display list | **Yes** |

**Capacity bounds this establishes:**

- **`Painter`/AGG 2D rasterization: bound is ~0% of any real-EC2 workload**, and
  matters only in the QEMU/`ramfb` verification rig — a developer-iteration
  tool, not a production workload. Vectorizing it cannot help the fleet. This is
  the category that turns out **not to matter**, and saying so is the point.
- **mesa llvmpipe / software GL: same bound as Painter** — it needs a local
  framebuffer, so QEMU-rig only, plus any future pixel-streaming design.
- **Image codec decode: matters in every configuration, including headless.**
  This is the only graphics-adjacent category with a nonzero bound on the actual
  fleet.

That ordering — **codecs above everything graphical** — falls straight out of
the bound, before any measurement.

### 5.1 The x86-only fast path, found and characterised

The predicted finding exists, and it is narrower than predicted in one way and
worse in another.

`src/servers/app/drawing/Painter/bitmap_painter/DrawBitmapBilinear.h`:

- `:409-479` — `struct BilinearSimd` guarded by **`#ifdef __i386__`**, calling
  `bilinear_scale_xloop_mmxsse()` (`:436`).
- `:595-604` — runtime selection gated on
  `gSIMDFlags & (APPSERVER_SIMD_MMX | APPSERVER_SIMD_SSE)`.
- `:630-638` — the `kUseSIMDVersion` dispatch arm, again `#ifdef __i386__`.

The assembly is `painter_bilinear_scale.nasm`, and
`src/servers/app/drawing/Painter/Jamfile:18-20` gates it:

```
if $(TARGET_ARCH) = x86 {
	PAINTER_ARCH_SOURCES = painter_bilinear_scale.nasm ;
}
```

**So this is 32-bit x86 only — not even x86_64.** arm64 and x86_64 both take the
scalar `DrawBitmapBilinearDefault` path. That reframes it: not "x86 fast, arm64
slow" but "legacy 32-bit x86 fast, every modern architecture slow."

`app_server` does have its own feature-flag hook —
`uint32 gSIMDFlags = detect_simd()` at
`src/servers/app/drawing/Painter/Painter.cpp:111`, flags defined at
`Painter.h:47-48`. But `detect_simd()` (`Painter.cpp:113-175`) is
`#if __i386__ ... #else return 0; #endif`. **On arm64 `gSIMDFlags` is a
hardcoded zero.** That is *correct* today (there is no NEON code to dispatch
to), not a broken detector — but it is the hook a NEON path would land in, and
it is the one piece of working compile-time dispatch in the graphics stack.

**Capacity bound:** this path covers *bilinear-filtered scaled bitmap drawing
only*. It is not the general blit path and not the general fill path. Combined
with §5's bound of ~0% on real EC2, **this is a real gap with a small bound.**
Interesting as a proof-of-concept for NEON in `app_server`; not a fleet win.

### 5.2 NEON in the tree: there is none, and three structural facts constrain adding it

`git grep -l arm_neon.h` over `refs/heads/graviton` returns **one file, and it is
a document** (`graviton/docs/arm64-memcpy.md`). **There are zero NEON intrinsics
in the entire Haiku source tree** — no `__ARM_NEON`, `vld1*`, `vst1*`,
`uint8x16_t`, `float32x4_t`, `vaddq_*`, `vmull_*` anywhere in `src/` or
`headers/`. For comparison, x86 SIMD intrinsic headers appear in four files:
`src/system/libroot/posix/string/arch/x86_64/memcpy.cpp`, `.../x86_64/memset.cpp`,
`src/add-ons/kernel/drivers/disk/nvme/libnvme/nvme_common.h`, and
`src/tests/system/kernel/hello_avx.c`.

Hand-written assembly, whole tree: **283 files, 181 x86/x86_64 (64%) vs 12 arm64
(4.2%)** — and all 12 arm64 files are boot/kernel/ABI plumbing (`crt0`,
`exceptions`, `sigsetjmp`, `byteorder`, …). **None does pixel or string work.**

Three structural constraints on any future arm64 vector work:

1. **`.nasm` is unbuildable on arm64 by construction.**
   `build/jam/MainBuildRules:151-167` hardcodes `NASMFLAGS` to `-f elf32` and is
   not arch-parameterised; there is no `nasm` entry in
   `build/jam/repositories/HaikuPorts/arm64`. `.S` *is* fully parameterised (rule
   `As`, `build/jam/OverriddenJamRules:177-219`). **So arm64 vector work must be
   intrinsics or `.S`, never `.nasm`** — the existing x86 SIMD path cannot be
   "ported" in place, it must be rewritten.
2. **The kernel gets no autovectorization on any architecture.**
   `build/jam/ArchitectureRules:366-369` sets `-fno-tree-vectorize` kernel-wide:
   *"Since GCC 13, autovectorization generates code which causes problems in
   various virtual machines (bare metal is apparently unaffected.) ... disable for
   the kernel. (See #18593.)"* Note we run **both** cases — `c7g.metal` is bare
   metal, every other instance is a VM — so we are affected. **Consequence: any
   kernel-side SIMD must be explicit intrinsics or assembly; and
   `-fopt-info-vec-missed`-driven autovectorization work (§1.4) applies to
   userland only.**
3. **arm64 ships no accelerant with 2D hooks.**
   `SYSTEM_ADD_ONS_ACCELERANTS` is filtered by `FFilterByBuildFeatures` in
   `build/jam/images/definitions/minimum:265-272` and `regular:177-192`, whose
   arms name only `x86,x86_64`, `x86` and `riscv64`. An arm64 image gets exactly
   `framebuffer.accelerant` and `virtio_gpu.accelerant`, neither publishing
   `B_SCREEN_TO_SCREEN_BLIT` or `B_FILL_RECTANGLE`. So in the QEMU rig **all 2D
   work is in `app_server`**, with no hardware path to fall back to — which is
   the one fact that argues *for* §6.4 rather than against it.

**And a symmetry worth recording so it is not mistaken for a gap:** the vendored
AGG (`src/libs/agg`, `headers/libs/agg`) contains no SIMD for any architecture.
Its only assembly is MSVC-inline and dead even on x86 — `agg_basics.h:128-145` is
behind `#if defined(AGG_FISTP)` and **`AGG_FISTP` is never defined anywhere in the
tree**; `agg_math.h:263-285` requires `_MSC_VER`. AGG is scalar C++ everywhere,
so this is not an arm64 disadvantage.

### 5.3 The libroot string routines: one measured NEON gap, one untouched routine

This is the best-specified SIMD opportunity in the tree, and it is not graphics.

| | `memcpy` | `memset` | `memmove` |
|---|---|---|---|
| **arm64** | `arch/arm64/memcpy.c` — 300 lines, purpose-written, **scalar** (`__uint128_t`), merged `f76217c69b` | **`arch/generic/generic_memset.c`** — 8-byte word loop | generic musl C |
| **x86_64** | `arch/x86_64/memcpy.cpp` — **SSE2** + `rep movsb` | `arch/x86_64/memset.cpp` — **SSE2** (`_mm_set1_epi8`/`_mm_store_si128`) | generic musl C |
| **arm (32)** | `arch/arm/memcpy.S` — **assembly** | generic | generic |

**(a) `memcpy`: a measured 1.1–2.0x gap, with the fix already named.**
[arm64-memcpy.md](arm64-memcpy.md) §6 records, as this project's own
measurement, that **glibc's `memcpy` is still 1.1–2.0x faster than ours**, and
states the remedy in terms: *"Closing it means `<arm_neon.h>` and `q`-register
load-everything-first groups up to 128 bytes."* That is a **quantified gap with a
specified fix** — the only one in this document — and it was deliberately deferred,
not overlooked.

**The policy question it deferred is now answered.** The same passage flags *"a
kernel question this version deliberately avoids — whether SIMD registers may be
used in kernel memcpy — which is answerable (exception entry eagerly saves all 32
`q` registers) but is a policy decision, not a detail."* §4.1 confirms that
reading independently: `_fp_save` at `arch_asm.S:197-226` is eager and
unconditional on all eight exception vectors, so by the time kernel code runs,
userland's V0–V31 are already in the iframe and kernel code may clobber them
freely. `memcpy` does not block, so the cooperative-switch path (which saves only
`d8`–`d15`, correctly per AAPCS64) is not involved. **So the answer is yes, and
what remains is a policy decision to record, not a technical unknown.**

**(b) `memset`: never written for arm64, and locked out of autovectorization.**
The arm64 `Jamfile:38-41` merges `memcpy.c` + `generic_memset.c`.
`arch/generic/generic_memset.c` is a scalar loop writing **8 bytes per
iteration**, and `Jamfile:23-26` must compile it `-fno-builtin
-fno-tree-loop-distribute-patterns` to stop GCC recognising the byte loop and
replacing it with a call to `memset` **from inside `memset`** — a verified
infinite recursion via an `R_AARCH64_CALL26` against `memset` itself at both `-O2`
and `-O3`, not a theoretical worry. **Those guards also prevent the compiler from
vectorizing it**, and `-fno-tree-vectorize` (§5.2) removes the possibility in the
kernel copy regardless. So arm64 `memset` is locked to 8 bytes per iteration by
construction, on both sides. A NEON `memset` (`stp q0, q1` → 32–64
bytes/iteration), or `dc zva` for the zero-fill case, is a straightforward win on
large fills.

**Calibration, and the counter-precedent that is the most important
methodological point in this document.** The arm64 `memcpy` change was measured at
**8.4% of receive CPU** — real, but sub-10%, and a useful guard against
over-optimism. Meanwhile the largest per-byte win this project has achieved was
`e63fe3f24a`, the internet checksum: **0.228 → 0.055 ns/byte, 4.15x, measured on
Neoverse V1** — and it used **no SIMD at all.** It replaced a 16-bit-at-a-time
loop with 64-bit loads into a 64-bit accumulator. **Widening scalar loads beat the
SIMD that was never written.** Before reaching for NEON anywhere in this
document, check whether the loop in question is still scalar-narrow — that is the
cheaper fix and it has the better track record here.

---

## 6. Ranked opportunities

Ranked by (breadth of code affected) x (confidence the mechanism is real) /
(cost), **not** by measured share of runtime — because no such profile exists
for this port (§7). Each row states the capacity bound that justifies it.

### 6.1 — Give haikuports userland a real ISA baseline. **Rank 1.**

- **Magnitude:** unquantified here, but the bound is the whole of userland.
  AWS documents *"up to an order of magnitude"* for contended locks (their
  number, their workloads, **not ours**). The current state is strictly worse
  than not dispatching: call **plus** LL/SC (§3.1).
- **Confidence: HIGH.** Verified from the build configuration and from upstream
  libgcc source. Falsifiable by one `objdump` (§6.6).
- **Effort: S.** A configuration change, then a userland rebuild.
- **Capacity bound:** 100% of userland code is compiled this way. Atomics are a
  material share of refcount-heavy C++ — WebKit, mesa, ICU, libstdc++. And this
  is the *precondition* for several rows below: crypto intrinsics and `dotprod`
  are not even available to a codec today.
- **The change.** In the haikuports GCC recipe, set the compiler's *default*
  (which survives recipes that overwrite `CFLAGS`, §3):

  ```
  --with-arch=armv8.4-a+crypto+fp16+rcpc+dotprod  --with-tune=neoverse-v1
  ```

  This is **AWS's own graviton3 string from `setup-compiler.sh` with `+sve`
  removed** — conservative `-march`, aggressive `-mtune`, exactly the split AWS
  prescribes (§1.1). `+sve` is removed because §4 says the kernel cannot preserve
  the state. **`-march=armv8.4-a` does not imply SVE** — proven by AWS having to
  spell `+sve` explicitly in that same line.
- **Three things that must not be done instead.** `-mcpu=neoverse-v1` (implies
  SVE → `SIGILL`). `-march=armv9-a` or higher (ARMv9 mandates SVE2 → `SIGILL`).
  `-mno-outline-atomics` (leaves the ISA at ARMv8.0, so still LL/SC, just
  inline — and misattributes the bug to the flag).
- **What would refute it:** an `objdump` of a shipped arm64 hpkg library showing
  inline `cas`/`ldadd` and no `bl __aarch64_*` calls. That would mean the native
  compiler is not at the default baseline and §3 is wrong.

### 6.2 — Verify, then fix, codec SIMD in the built packages. **Rank 2.**

- **Capacity bound:** image decode is the **only** graphics-adjacent stage with a
  nonzero bound on real EC2 (§5). It runs in every configuration including
  headless.
- **Status of each, from recipe reading (documented, symbol-level verification
  deferred — §7):**

| Package | Version pinned for arm64 | SIMD state | Confidence |
|---|---|---|---|
| `libjpeg_turbo` | `3.1.4.1-1` | `-DWITH_SIMD` **not passed** → upstream CMake default, which is **ON**. AArch64 NEON in libjpeg-turbo 3.x is intrinsics, needing no `nasm`. **So NEON is probably already enabled** | Medium — needs symbol check |
| `libpng16` | `1.6.53-1` | Recipe passes **no** `--enable-arm-neon` → `pngpriv.h:138-142` default applies: `PNG_ARM_NEON_OPT 2` when `__ARM_NEON && PNG_ALIGNED_MEMORY_SUPPORTED`. `__ARM_NEON` is always defined on aarch64. **So NEON is probably already enabled** | Medium — needs symbol check |
| `zlib` | **`1.2.13_bootstrap-1`** | **Stock zlib has no Arm optimizations at all** (AWS states this outright). `zlib-ng` **does not exist in haikuports** | HIGH |
| `freetype` | **`2.6.3_bootstrap-1`** | 2016-era, bootstrap build | HIGH that it is stale; unassessed what it costs |

- **The honest result is a partial negative**, and it is worth stating clearly:
  the hoped-for "free win" of a disabled-upstream-NEON-path **does not appear to
  exist for libjpeg-turbo or libpng** — their defaults are ON and our recipes do
  not override them. Both still deserve the symbol check in §6.6 before the
  matter is closed, because a default can be silently overridden by a failed
  compiler feature-probe.
- **What is real here:** `zlib` and `freetype` are pinned to **bootstrap-stage
  packages** that were never rebuilt with the final toolchain (`zlib-1.2.13`,
  `freetype-2.6.3`). Twenty of the 66 arm64 package pins carry `_bootstrap`.
  **Effort S, confidence HIGH, magnitude unknown** — and the fix is a rebuild,
  not new code. Whether zlib is hot enough to matter is unmeasured; note that
  `libpng` decode is zlib-bound, which gives it a path to mattering.
- **`zlib-ng` is a genuine opportunity with a cost:** AWS recommends it
  explicitly (≥2.3.3, superseding zlib-cloudflare) and it has real NEON, but it
  is **not in haikuports**, so this is "write and maintain a new recipe", not a
  flag flip. Rank it below the rebuild.

### 6.3 — NEON in the arm64 libroot/kernel string routines. **Rank 3.**

Two sub-items, sharing one test harness. This is the **only genuinely NEON-shaped
opportunity in the document with a number already attached**, and it is not
graphics — which is itself part of the answer.

**(a) NEON `memcpy` — closes a measured 1.1–2.0x gap.**
- **Magnitude: 1.1–2.0x on `memcpy` itself. MEASURED by this project** against
  glibc ([arm64-memcpy.md](arm64-memcpy.md) §6) — not estimated, not AWS's.
- **Confidence: HIGH.** The gap is measured and the fix is already specified
  (`<arm_neon.h>`, `q`-register load-everything-first groups to 128 bytes).
- **Effort: M.** The deferred kernel-SIMD policy question is answered (§5.3a):
  exception entry eagerly saves all 32 `q` registers, so kernel use is safe.
  What is owed is a recorded policy decision, not an investigation.
- **Capacity bound:** libroot *and* kernel, so fleet-wide, not graphics-only.
  **But bound the expectation honestly:** the previous, scalar `memcpy` rewrite —
  a far larger relative improvement over what preceded it — was worth **8.4% of
  receive CPU**. A further 1.1–2.0x *on the routine* is therefore worth
  single-digit percent *on a workload*, at best. Do not expect more.

**(b) NEON `memset` — a routine that was never written for arm64.**
- **Magnitude: estimated 4–8x on large fills** (8 bytes/iteration → 32–64), or
  better with `dc zva` for zero-fill. **Estimated from instruction width alone —
  not measured.**
- **Confidence: HIGH that the gap exists** (§5.3b, verified); **LOW that it is
  worth a lot**, because its share of any workload is unmeasured.
- **Effort: S–M.** One file plus a Jamfile line.

**For both:** the correctness harness from the `memcpy` work is directly
reusable — it ran 509,882 checks and **found six defects, four after the change
had already been reviewed once, and one that the fix introduced into itself.**
**Reuse it; do not skip it.** Mind the self-recursion trap that already forced
`-fno-builtin -fno-tree-loop-distribute-patterns` on this directory, and note
that a NEON intrinsic version sidesteps it by construction (the compiler cannot
pattern-match a `q`-register loop back into a `memset` call).

**Refuted by:** a profile showing `memcpy`+`memset` below a few percent of any
workload we care about. Which is exactly the profile that does not exist (§7).

### 6.4 — NEON bilinear bitmap scaling in `app_server`. **Rank 4 — do not start yet.**

- **Confidence the gap exists: HIGH** (§5.1, verified with file:line).
- **Capacity bound: ~0% on real EC2** (§5 — `RemoteHWInterface` rasterizes in
  the client), and within the QEMU rig it covers only bilinear-filtered scaled
  bitmap draws, not the general blit or fill path.
- **Verdict: correctly shaped, wrongly prioritised.** It is the most
  *satisfying* item on this list and one of the least valuable. Its real merit is
  as a **proof-of-concept**: `gSIMDFlags`/`detect_simd()` is the one working
  compile-time dispatch hook in the graphics stack, and `#ifdef __ARM_NEON`
  alongside `#ifdef __i386__` is the natural pattern. Worth doing *when* a
  measured GUI profile says bitmap scaling is hot, and not before.
- **Two constraints if it is ever done.** It must be intrinsics or `.S`, never
  `.nasm` (§5.2.1) — so it is a rewrite, not a port. And the better seam may not
  be the bilinear scaler at all: `PixelFormat`'s blend function-pointer table
  (`src/servers/app/drawing/Painter/drawing_modes/PixelFormat.cpp:158`+, setting
  `fBlendPixel`/`fBlendHLine`/`fBlendSolidHSpan`/…) is the one generalisable
  dispatch point in the drawing stack. It is currently keyed on `drawing_mode`
  and `source_alpha` rather than on CPU features, and the **41 drawing-mode span
  blenders** behind it (`DrawingMode.h` macros `BLEND` `:28`, `BLEND_COMPOSITE`
  `:103` — the latter with a **per-pixel integer divide** at `:123-126`) are
  where the per-pixel work actually is. A profile should decide between the two
  seams; do not assume the bilinear scaler is the hot one just because it is the
  one someone already vectorized on x86 twenty years ago.

### 6.5 — Publish a HWCAP-equivalent word. **Rank 5 — enabling work, not a win.**

- Not a performance change; it makes future dispatch *possible*. The kernel reads
  `ID_AA64ISAR0_EL1`/`ID_AA64PFR0_EL1` in `arch_cpu_init()`, stores the result,
  and exposes it via a **third arm64 commpage entry**
  (`arch_commpage_defs.h` currently has two) plus a libroot accessor.
- **Effort: M. Self-contained**, and unlike ifunc it needs no loader work and
  does not touch the signal ABI.
- **It would not make SVE safe** — that is §4.3, separate and larger. It would
  only make LSE/crypto/dotprod dispatch honest, which §6.1 makes unnecessary by
  deciding at compile time instead. **So this is only worth doing if a concrete
  need for runtime dispatch appears.** Cheaper alternative for most cases: decide
  at compile time.
- **Adjacent, and worth doing on its own:** audit haikuports recipes for ifunc
  (`--enable-ifunc`, `HAVE_IFUNC`, `target_clones`). §2.1 shows such a package
  produces a shared object that **fails to load silently**. This is a
  correctness bug hunt, not an optimization.
- **One latent arch-guard bug found in passing**, unrelated to performance but
  cheap to fix while nearby:
  `src/apps/icon-o-matic/generic/support/support.h:47` unconditionally does
  `#define constrain_int32_0_255 constrain_int32_0_255_asm`, missing the
  `#ifdef __i386__` guard that its twin at
  `src/add-ons/translators/wonderbrush/support/support.h:24-52` has. It compiles
  clean on arm64 today only because nothing in icon-o-matic calls it — i.e. it is
  a trap armed for whoever next does. (`src/add-ons/accelerants/nvidia/engine/
  nv_acc_dma.c:1428` has unguarded x86 inline asm for the same reason, harmless
  only because arm64 ships no nvidia accelerant — §5.2.3.)

### 6.6 — Not ranked: mesa/llvmpipe

Deferred, not dismissed. llvmpipe JITs at runtime, so the `-mcpu` question for it
is *what LLVM detects on the host at run time*, which is a different mechanism
from §6.1 and needs its own investigation. Its capacity bound is the same as
Painter's (§5): QEMU-rig only. **Not investigated here** — see §7.

---

## 7. The single highest-value action, and the cheapest experiment

**Action: §6.1 — set `--with-arch=armv8.4-a+crypto+fp16+rcpc+dotprod
--with-tune=neoverse-v1` as the native arm64 GCC default, and rebuild
userland.**

It affects 100% of userland rather than one loop; it is a configuration change
rather than new code; it is the *precondition* for two other rows; and the defect
it fixes is one this project has already diagnosed, fixed, and
hardware-verified once — in the kernel — so the mechanism is not in doubt.

**The cheapest experiment that would prove or kill it** costs one command and no
build. It is AWS's own verification recipe (§1.6) applied to a library we have
already shipped:

> On the builder (`i-0f7f6f3e8922acffd`) or any Haiku arm64 guest, pick a
> shipped haikuports library — `libjpeg.so`, or better something atomic-heavy
> like `libicuuc.so` — and count:
>
> ```
> objdump -d libicuuc.so | grep -ci 'bl.*__aarch64_\(cas\|swp\|ldadd\|ldclr\|ldeor\|ldset\)'
> objdump -d libicuuc.so | grep -ci '\b\(cas\|casp\|swp\|ldadd\|stadd\|ldclr\|ldeor\|ldset\)'
> objdump -d libicuuc.so | grep -ci '\b\(ldxr\|ldaxr\|stxr\|stlxr\)'
> ```
>
> **Confirms §6.1** if the first count is nonzero and the second is ~zero: the
> library dispatches through outline helpers and contains no inline LSE.
> Combined with the libgcc source in §3.1 (the initialiser is `__gnu_linux__`-only),
> that is the dead dispatch, proven on shipped object code.
>
> **Refutes §6.1** if inline `cas`/`ldadd` are present and `bl __aarch64_*` is
> absent — the native compiler is not at the default baseline and §3 is wrong.
>
> Also run it on a jam-built binary (e.g. `/boot/system/servers/app_server`) as a
> **positive control**: that one *should* show inline LSE, because
> `ArchitectureRules:52` applies there. If the control fails, the instrument is
> wrong, not the hypothesis.

This is a read-only `objdump`, cheap enough to run while the metal is busy, and
it needs no reboot, no bake, and no A/B.

**The follow-up measurement, deferred.** If confirmed, the *value* of fixing it
still needs a number. Design: build the same tree twice, differing only in that
flag; boot both on the **same instance, same boot session where possible**;
interleave runs. Use a lock-heavy userland workload, not a synthetic microbench
(AWS: *"No synthetic benchmark is a substitute for your actual production
code"*). Because AWS's claim is that the LSE delta **grows with core count**,
sweep the vCPU count and show the delta *growing* — that makes the result
falsifiable rather than a single number. Known confounds in this project that
must be controlled: burst credits, per-flow caps, and documented boot-to-boot
drift (see [throughput-measurement.md](throughput-measurement.md)); a single run
is not a measurement.

---

## 8. Honest accounting

### Sources

Everything cited here is **public**: AWS's `aws-graviton-getting-started` repo
(`main`, read 2026-08-25), the GCC 13.3 AArch64 options manual, upstream
`libgcc/config/aarch64/lse-init.c` (GCC 13 branch), upstream libpng's
`pngpriv.h`, the public haikuports recipes, and this tree at
`refs/heads/graviton`. Plus prior measurements taken by this project on
instances we rent. **No non-public source was needed for any conclusion**, which
is worth recording because it means every claim below is independently
checkable by a reader outside the project.

### Verified here (read in `refs/heads/graviton`, or in named upstream source)

- No `aarch64` case in `build/scripts/build_cross_tools_gcc4`; no
  `--with-arch`/`--with-cpu` in the haikuports GCC recipe; haikuporter injects no
  arch flags. And GCC 13's own source fixes what that default *is* —
  `aarch64.h:727-730` (`TARGET_CPU_generic`) and `aarch64.cc:2757`
  (`generic` → `AARCH64_ARCH_V8A`, tuned `cortexa53`). → userland is at
  **ARMv8.0-A, Cortex-A53-tuned**. (§3)
- `libgcc/config/aarch64/lse-init.c` gates its only initialiser on
  `#ifdef __gnu_linux__`, so `__aarch64_have_lse_atomics` is permanently false on
  Haiku. (§3.1)
- `CPACR_EL1` written twice, both in the boot loader, `FPEN=0b11` and `ZEN=0b00`;
  no `ZCR_EL1`, no SVE save/restore, five dead SVE macros. → SVE traps to
  `SIGILL`. (§4.2)
- FPSIMD state is eagerly saved on every exception and carried through signals
  and fork; 512-byte fixed save area. → NEON is safe. (§4.1)
- No `getauxval`/HWCAP/auxv; no arm64 ID-register feature probe; no userland
  feature API; `R_AARCH64_IRELATIVE` → `B_BAD_DATA`, silently. → no runtime
  dispatch by any mechanism. (§2.1)
- The `__i386__`-only bilinear SIMD path, its NASM file, its Jamfile gate, and
  `detect_simd()` returning 0 off x86 — and that the gate is `TARGET_ARCH = x86`,
  so **x86_64 takes the scalar path too**. (§5.1)
- Zero `arm_neon.h` includes and zero NEON intrinsic tokens in the tree; four
  files with x86 SIMD intrinsics; 181 x86 vs 12 arm64 hand-written asm files,
  none of the 12 doing pixel or string work. (§5.2)
- `.nasm` is unbuildable on arm64 (`NASMFLAGS -f elf32`, no arm64 `nasm`
  package); `-fno-tree-vectorize` is set kernel-wide on every arch
  (`ArchitectureRules:366-369`, Haiku #18593); `SYSTEM_ADD_ONS_ACCELERANTS` is
  empty on arm64 so no accelerant publishes a 2D hook. (§5.2)
- AGG contains no SIMD for any architecture, and its only asm is dead even on
  x86 (`AGG_FISTP` never defined) — symmetric, not an arm64 disadvantage. (§5.2)
- arm64 has no `memset.c`; it uses `generic_memset.c` at 8 bytes/iteration, with
  vectorization suppressed by the recursion guard **and** by `-fno-tree-vectorize`
  in the kernel copy. (§5.3b)
- The deferred "may the kernel use SIMD registers in `memcpy`" question is
  answered yes by the eager `_fp_save` on all eight exception vectors — which
  corroborates what [arm64-memcpy.md](arm64-memcpy.md) §6 already suspected.
  (§5.3a)
- The missing `__i386__` guard at
  `src/apps/icon-o-matic/generic/support/support.h:47`. (§6.5)
- `RemoteHWInterface::FrontBuffer()` returns `NULL` on real EC2 → no local
  rasterization. (§5)
- Recipe-level SIMD flags for `libjpeg_turbo` (no `-DWITH_SIMD`) and `libpng16`
  (no `--enable-arm-neon`); `pngpriv.h`'s aarch64 default; the 20 `_bootstrap`
  pins in the arm64 repository. (§6.2)

### Documented (AWS's or GCC's claims, not our measurements)

Everything in §1. Specifically **not ours**: *"up to an order of magnitude"* for
LSE; the per-generation SVE widths; the SVE/NEON FLOP ceilings; GCC version
floors per `-mcpu`; zlib-ng superseding zlib-cloudflare.

### Inferred (labelled, and load-bearing — challenge these first)

- **SVE ≈ NEON FLOP parity on V1/V2.** Derived from AWS's own identical
  `flop-sve-pkc` / `flop-nonsve-pkc` ceilings plus the width/pipe table. AWS
  never states it. This inference is what demotes SVE in §4.3; if it is wrong,
  §4.3's conclusion should be revisited (though the kernel-work cost and the
  signal-ABI blocker stand regardless).
- **libjpeg-turbo and libpng NEON are already enabled.** From upstream defaults
  plus the absence of an overriding flag in our recipes. **Not symbol-verified**
  — a failed compiler feature-probe can silently turn a default off, which is
  exactly the failure mode worth checking.
- A NEON `memset` is worth 4–8x on large fills — from instruction width alone.

### Measured, by this project, and reused here as calibration

- Internet checksum: **0.228 → 0.055 ns/byte, 4.15x on Neoverse V1**, with **no
  SIMD** (`e63fe3f24a`). The methodological headline of §5.3.
- arm64 `memcpy`: **8.4% of receive CPU** (`f76217c69b`,
  [arm64-memcpy.md](arm64-memcpy.md)) — the realism anchor for §6.3.
- **glibc `memcpy` is still 1.1–2.0x faster than ours**
  ([arm64-memcpy.md](arm64-memcpy.md) §6) — the only quantified gap with a
  named NEON fix in this document, and the basis for §6.3a.
- LSE in the kernel: outline calls 1173 → 4, inline LSE 10 → 1179 (`20bf8f2711`)
  — instruction-level only; **no performance number was ever attached.**

### Deferred, and why

- **The profile.** *No measurement of where graphics or image time actually goes
  on this port exists*, and none was taken — the metal is running a WebKit build
  and an llvm12/mesa job, and this review was not permitted to contend with
  them. **Every ranking in §6 is therefore a bound-and-breadth argument, not a
  measured share.** The PMU facility exists (`af7e48b94c`) and **no ratio from it
  has ever been collected**; that remains the highest-leverage missing
  instrument, as `graviton-optimization-plan.md` item 13 already says.
- **Symbol-level verification of codec SIMD** in the built hpkgs (`jsimd_*_neon`
  for libjpeg-turbo; `filter_neon`/`png_have_neon` for libpng). This is the check
  that closes §6.2, and it is cheap — it just needs a quiet builder.
- **Running `-mtune=neoverse-v2` / `-march=armv8.4-a+...` through the actual
  cross-compiler** to confirm it is accepted and emits no SVE. GCC 13.3 is
  documented as sufficient; not exercised here.
- **mesa/llvmpipe** (§6.6) — needs its own investigation of runtime LLVM target
  selection.
- **A GUI/rasterization profile in the QEMU rig**, which is what would move §6.4
  off the bottom of the list.
