# Chromium on DeBeOS arm64 (headless Graviton) — feasibility scope (#379)

Scoping only. **No Chromium checkout was fetched and nothing was built this pass** —
that is the correct call for a ~100 GB / 16-32 GB-RAM / multi-hour tree, not a punt
(see the verdict). Chromium-side facts are from public sources (chromium.googlesource.com,
the SwiftShader/ANGLE/GN repos, HaikuPorts). Platform facts (SVE, no-GOP, NEON, JIT
class) are this project's own measurements or in-tree reads, cross-referenced to the
docs cited at the end. Throughout: **VERIFIED** = read from a cited primary source or
measured here; **INFERRED** = reasoned from verified facts; **NOT MEASURED** = could not
confirm, and said so.

---

## TL;DR verdict

- **The Chromium *platform port* is greenfield; the *build tooling* is not.** There is
  **no upstream Haiku Ozone platform** and **no standalone Chromium recipe** in
  HaikuPorts. The only prior art is the community **QtWebEngine-on-Haiku** port
  (bundled Chromium ~M87 / Qt 5.15), which is **x86_64-only** and routed through Qt's
  Linux-ish shim, not an Ozone "haiku" backend. It proves Chromium's low layers
  (`base/`, process, IPC) *can* be made to compile on Haiku — real de-risking of the
  bottom of the stack — but gives you **neither arm64 nor a headless integration**.
- **The one genuine narrowing:** Chromium already ships a **`headless` Ozone platform**
  that renders to a PNG in software with **no windowing system and no GPU** — exactly
  the headless-Graviton shape. A DeBeOS headless target rides that platform and never
  needs a Haiku `app_server`/windowing Ozone backend. That removes the single largest
  chunk of greenfield UI work — but `base/`, `sandbox/`, IPC/mojo and `//net` still need
  Haiku+arm64 bring-up.
- **GPU compensation is architecturally sound but its SIMD ceiling is lower than the
  issue assumed.** SwiftShader (via ANGLE) is a CPU Vulkan rasterizer that supports
  aarch64 and vectorizes through its **Reactor JIT** (LLVM backend) to **128-bit NEON**.
  **SVE is not in the picture for SwiftShader** because SwiftShader/Reactor has no SVE
  path (fixed-width `Float4`) — the rasterizer emits NEON regardless of kernel SVE. So
  the "NEON/SVE" premise in #379 is **NEON only** *for SwiftShader specifically*, on
  those grounds alone. (Correction: an earlier draft claimed "this kernel traps every SVE
  instruction to SIGILL on Graviton 5" — that is WRONG. A c9g/Graviton5 probe on the
  current canonical shows EL0 SVE **executes cleanly, VL=128-bit**; Graviton3 is VL=256-bit;
  #331's 6.37x llama.cpp SVE speedup on G3 is hardware-proven. SVE is enabled fleet-wide;
  the only caveat is that vector length varies by generation, so an artifact must read VL
  at runtime and not hardcode it.)
- **A real hazard the JIT introduces:** Reactor JIT-compiles shader code at runtime
  (W^X-managed, i-cache-maintained generated code). That is **the same class of problem
  #93's V8/Node bring-up hits** — and Haiku already solved it in-tree for V8 — but it is
  **unproven for SwiftShader's arm64 codegen specifically**, and it runs in the GPU
  process, which Chromium itself flags as a "high security risk due to JIT-ed code."
- **gn and ninja are already ported to Haiku arm64** (HaikuPorts recipes, `ARCHITECTURES="all !x86_gcc2"`),
  and native **CPython 3.14 / 3.10** are proven in the DeBeOS pool (#365). So the
  build-driver layer is *not* a blocker; `depot_tools` (no Haiku CIPD binaries) is used
  only as a fetch harness, with Haiku's own gn/ninja/python.
- **Recommendation: DO NOT attempt a Chromium port now. Stay on HaikuWebKit/WebPositive,
  which already builds and renders on arm64 (~55 min), and treat headless Chromium as a
  *long, separate, opt-in* research track** gated behind a cheap M1 probe. The honest
  size gap is stark: WebPositive is *nine small ports + one build already done*; a
  Chromium headless port is a **multi-person-month platform bring-up** (base/sandbox/mojo/net
  on Haiku-arm64) on top of a 100 GB tree — for a headless PNG/stream renderer, not the
  native browser DeBeOS users actually want. See §8 for the one scenario that would flip
  this.

---

## 1. The port surface: what exists, what is greenfield

### 1.1 Upstream Ozone platforms — no Haiku (VERIFIED)

Chromium's window/GPU abstraction is **Ozone**. `//ui/ozone/platform/` upstream contains
exactly: `cast/`, `drm/`, `flatland/`, `headless/`, `wayland/`, `x11/`. There is **no
`haiku/` (or `beos/`, or `app_server/`) Ozone platform**, and none in any public branch.
Everything that draws to a *display* on Chromium goes through one of those six backends;
a native windowed Chromium on Haiku would need a seventh, written from scratch against
`app_server`/`BWindow`/`BBitmap`. That is the expensive greenfield object, and the
headless target (§1.3) is how you avoid needing it.

### 1.2 No standalone Chromium; QtWebEngine is the only prior art, and it is x86-only (VERIFIED)

- HaikuPorts `www-client/` has **no `chromium` recipe**. The browsers there are
  Firefox-family, NetSurf, Dillo, the text browsers, Ladybird (own engine), and
  QtWebEngine-based shells (Falkon, Dooble, Otter, qutebrowser).
- **QtWebEngine bundles Chromium and has a Haiku port** — but:
  - `dev-qt/qtwebengine/qtwebengine-5.15.18.recipe` declares
    `ARCHITECTURES="!x86_gcc2 ?x86_64"`, `SECONDARY_ARCHITECTURES="?x86"`. **arm64 is
    not listed at all.**
  - `qtwebengine_bin-5.15.18.recipe` is a prebuilt hpkg, `ARCHITECTURES="x86_64"`,
    pulling from community GitHub repos.
  - Its bundled Chromium is **~M87 (2020-era)** — old, and reached through a
    `qtwebengine_isLinuxPlatformSupported()` shim + `chromium_overrides.cpp` edits that
    make Haiku look like Linux inside the bundled tree, *not* an Ozone "haiku" backend.

**What this prior art is worth:** it demonstrates Chromium's `base/`, process model and
IPC can be made to compile and run on Haiku (x86_64). That de-risks the *lowest* layers.
It does **nothing** for arm64, for a modern Chromium, or for a clean Ozone integration —
and QtWebEngine additionally drags in the entire Qt stack, which the WebKit scope
(`webpositive-arm64-plan.md` §7) already priced as strictly more than HaikuWebKit.

### 1.3 The `headless` Ozone platform is the scope-narrowing key (VERIFIED)

Chromium's `headless` Ozone platform "draws graphical output to a PNG image (no GPU
support; software rendering only) and will not output to the screen" (output dir via
`--ozone-dump-file=`). Modern headless is a separate binary, **`chrome-headless-shell`**
(the old in-Chrome `--headless` was removed), driven over the DevTools protocol.

This matters enormously for DeBeOS: **a headless Graviton target needs no windowing
backend at all.** It rides the existing `headless` platform, renders in software, and
emits pixels as PNG / DevTools screencast frames. So the port surface reduces to the
**OS-abstraction layers, not the UI layer**:

| Chromium layer | Haiku-arm64 status | Greenfield cost |
|---|---|---|
| `base/` (files, threads, sync, time, process) | compiles on Haiku x86 via QtWebEngine prior art; arm64 unproven | **medium** — port exists in spirit, needs arm64 + modernisation |
| `//ui/ozone/platform/headless` | exists upstream, OS-generic | **low** — reuse; wire to Haiku file/PNG output |
| `sandbox/` | Linux seccomp-BPF/namespaces; **Haiku has neither** | **high** — must stub/replace (`--no-sandbox` for a start) |
| IPC / **mojo** | POSIX handle passing; Haiku ports/areas differ | **medium-high** — handle transport + shared-memory (`BArea`) mapping |
| `//net` | BSD sockets mostly portable; needs Haiku DNS/cert glue | **medium** |
| **windowing** Ozone backend (`app_server`) | not needed for headless | **avoided** (this is the big win) |
| V8 (Blink's engine) | Haiku V8 port exists as Node's patchset (#93); arm64 codegen unproven | **shared with #93** — see §4 |

**Verdict:** even with the UI layer removed, a modern arm64 headless Chromium is a
**greenfield OS-abstraction port** across `base`/`sandbox`/`mojo`/`net`, not a recipe
tweak. The `headless` platform makes it *tractable in principle*; it does not make it
small.

---

## 2. GPU compensation: SwiftShader/ANGLE on CPU — sound, but NEON-only

The headless Graviton reality is real and settled: **no GPU, no video device, and the
Nitro arm64 UEFI publishes no boot GOP** — measured on c7g, Haiku's loader prints
`GOP protocol not found` and sets `frame_buffer.enabled = false`
(`graviton-has-no-video-device`). Any Chromium GPU work must therefore be done on the CPU.

### 2.1 How SwiftShader is actually enabled today (VERIFIED — corrects #379)

There is **no app-level `use_swiftshader=true` GN toggle** in modern Chromium. SwiftShader
is the CPU **Vulkan** driver, consumed **through ANGLE** ("SwANGLE"), and selected **at
runtime**:

- GL ES: `--use-gl=angle --use-angle=swiftshader`
- WebGL fallback: `--use-gl=angle --use-angle=swiftshader-webgl --enable-unsafe-swiftshader`
- Direct Vulkan: `--use-vulkan=swiftshader` (build feature `enable_swiftshader_vulkan`)

Chromium explicitly flags SwiftShader as "a high security risk due to JIT-ed code running
in Chromium's GPU process" and has deprecated silent auto-fallback to it — a posture worth
recording, though for a trusted headless render farm it is an acceptable risk.

### 2.2 SwiftShader vectorizes to NEON via a JIT — no SVE (VERIFIED + measured)

- SwiftShader is "a high-performance CPU-based implementation of the Vulkan 1.3 API." Its
  code generator is **Reactor**, an embedded JIT with backends `LLVM` (default),
  `LLVM-Submodule`, or `Subzero`. **aarch64 is a supported target** (`ARCH="aarch64"`,
  `aarch64asmparser aarch64codegen`).
- SIMD width: Reactor's vector type is a fixed **`Float4`** (4-wide) → maps to **128-bit
  NEON**, emitted by the LLVM/Subzero aarch64 backend. **No `+sve` anywhere; SVE is not a
  SwiftShader path.** (VERIFIED absent in SwiftShader; INFERRED that codegen uses NEON.)

**#379's premise is half-right, for ONE reason only:**

1. **SwiftShader has no SVE codegen** (fixed 4-wide `Float4`) — so SwiftShader emits **NEON
   only**, regardless of what the kernel supports. This alone settles SwiftShader = NEON.
2. **SVE is NOT blocked on this kernel** (correction of an earlier draft): #88 enabled EL0/EL1
   SVE (`arch_cpu.cpp` sets `CPACR_EL1.ZEN=0b11`, with per-thread save/restore), and a
   Graviton5 (`c9g`) probe on the current canonical shows EL0 SVE **executes cleanly, VL=128-bit**
   (Graviton3 = VL=256-bit). #331's **6.37x** llama.cpp SVE speedup on Graviton3 is
   hardware-proven (SVE_CNT=32, real SMMLA GEMM). So the `simd-vectorization-review.md` "SVE
   traps to SIGILL / ZEN=0" statements are STALE (pre-#88) and are being reconciled tree-wide.
   The only real SVE caveat is that **vector length varies by generation** (256-bit G3 →
   128-bit G4/G5), so SVE code must read `RDVL`/`svcntb()` at runtime and not hardcode a width.

**For SwiftShader specifically, plan for NEON only** — not because SVE is unavailable, but
because SwiftShader doesn't emit it. The many-vCPU angle *does* hold: SwiftShader spreads raster/
compositor work across cores, and Graviton's high core counts are the real lever — plus
this project's NEON `memcpy` and 64-bit-widened checksum wins (`arm64-memcpy.md`,
`net-checksum`) help the byte-shovelling around the rasterizer.

Also relevant: **Haiku arm64 has no runtime SIMD dispatch and `R_AARCH64_IRELATIVE`
(ifunc) fails to load silently** (`simd-vectorization-review.md` §2.1). SwiftShader's
Reactor sidesteps this by *JIT-ing* code at runtime rather than ifunc-dispatching
pre-compiled variants — so it is not blocked by the ifunc gap — but any AOT SIMD in
Chromium/ANGLE that relies on ifunc/`target_clones` would be, and must be compiled to a
single NEON baseline.

### 2.3 The JIT is the real portability risk (shared with #93)

Reactor JIT-compiles shaders at runtime: it allocates executable pages (W^X), writes
generated aarch64 code, and must do i-cache maintenance before executing it. **This is the
exact class of problem the V8/Node bring-up (#93) faces**, and Haiku already carries
in-tree fixes for it in V8's platform layer (`platform-haiku.cc`, W^X mmap, snapshot
handling — see `nodejs-arm64-scope.md` §2). That precedent de-risks the *category*. But
SwiftShader/Reactor is a **different JIT** (LLVM/Subzero, not V8's), so its arm64 W^X and
i-cache path on Haiku is **unproven** and is a likely first-failure site. It is also why
SwiftShader needs a working LLVM on the build/target — the same `llvm12`-class dependency
the WebKit scope worked hard to *avoid*, here unavoidable (or use the Subzero backend,
which is vendored and lighter but less optimised).

---

## 3. Display tie-in: two models, and which fits headless Graviton

There are two distinct ways Chromium pixels reach a DeBeOS user, and they demand very
different amounts of port work:

**Model A — headless render, then stream the artifacts (recommended, low port cost).**
`chrome-headless-shell` on the `headless` Ozone platform renders pages to PNG / DevTools
screencast frames entirely in software (SwANGLE for WebGL/canvas). No `app_server`, no
windowing backend, no framebuffer. The output is *already pixels in a file/stream*, so the
#118 machinery is barely needed — you serve PNG/DevTools frames directly, or pipe them
into the existing capture/stream path. This is the natural fit for a headless render farm,
screenshot service, or PDF/print backend, and it is the only model that keeps the port
surface to §1.3's OS-abstraction layers.

**Model B — a windowed browser inside the #118 framebuffer guest (high port cost).** To
give a *user* an interactive Chromium window, Chromium must paint into a Haiku window,
which requires the **greenfield windowing Ozone backend** (§1.1). Only then does the
#118 route (`vfb-route2-streaming.md`) apply: run the browser inside a Haiku KVM guest on
a Graviton `.metal` host with a synthetic display (ramfb/virtio-gpu), and let the Linux
host capture/encode/stream `app_server`'s framebuffer — the same path that already streams
the Haiku desktop with no Haiku change. **But that path streams whatever `app_server`
paints; it does not remove the need for the Ozone-haiku backend that makes Chromium paint
into `app_server` in the first place.** So Model B = all of §1.3 *plus* the windowing
backend *plus* #118. It is the "real browser" outcome and the expensive one.

For headless Graviton, **Model A is the target.** If DeBeOS ever wants an *interactive*
Chromium desktop, that is Model B, and at that point HaikuWebKit/WebPositive (which already
paints natively) is almost certainly the cheaper way to get an interactive engine on
screen.

---

## 4. V8: the #93 read-through

Chromium's Blink uses **the same V8** that Node.js (#93) exercises. From
`nodejs-arm64-scope.md`: the Haiku V8 platform port already exists (as Node's patchset —
`platform-haiku.cc`, pointer-compression disabled on Haiku, snapshot disabled, gcc forced),
the arm64 V8 backend is first-class, and the residual risk is Haiku-arm64 JIT glue (W^X,
i-cache, signal/stack-unwind on generated code) — **not** the engine itself. Node's build
is GYP-driven and self-contained; Chromium's is GN-driven and pulls V8 as part of the
100 GB tree, so **Chromium does not get to reuse Node's build**, but it *does* inherit the
same "V8 on Haiku arm64 is a platform-glue exercise, not a backend port" conclusion. If
#93 lands a working native V8 on Graviton, that is the single strongest positive signal for
Chromium feasibility — it proves the hardest shared component runs. Conversely, if #93
stalls on arm64 JIT glue, Chromium would hit the identical wall (twice: V8 *and*
SwiftShader/Reactor).

---

## 5. Build reference: linux/arm64 args.gn and the Haiku deltas

### 5.1 The closest real reference (VERIFIED from Chromium build docs)

Chromium officially supports linux + `target_cpu="arm64"`. A headless/release build's GN
args look like this (values from `docs/linux/build_instructions.md`; `target_os/cpu` and
headless target per convention/`headless/README.md`):

```gn
# out/HeadlessArm64/args.gn  — linux/arm64 reference (the STARTING point, not Haiku)
target_os          = "linux"
target_cpu         = "arm64"
is_debug           = false
is_official_build  = true       # full optimization, smaller binary
symbol_level       = 0          # + blink_symbol_level=0, v8_symbol_level=0 to shrink/speed
blink_symbol_level = 0
enable_nacl        = false      # headless/embedder builds drop NaCl
use_sysroot        = true       # cross-on-Linux uses the Debian arm64 sysroot
# SwiftShader/ANGLE are built as part of the GPU stack; selected at RUNTIME:
#   chrome-headless-shell --use-gl=angle --use-angle=swiftshader --ozone-platform=headless
# (there is no use_swiftshader=true app toggle)
```

Build driver: `fetch --nohooks chromium` → `gclient runhooks` → `gn gen out/HeadlessArm64`
→ `autoninja -C out/HeadlessArm64 chrome` (or the `headless_shell`/`chrome-headless-shell`
target).

### 5.2 What changes for a Haiku-arm64 target (INFERRED — this is the port)

```gn
# out/HaikuArm64Headless/args.gn  — DRAFT for DeBeOS (aspirational; not yet buildable)
target_os          = "haiku"    # <-- does not exist in Chromium's GN today; must be ADDED
target_cpu         = "arm64"
is_debug           = false
is_clang           = false      # Haiku toolchain is gcc; Chromium assumes clang -> big delta
use_custom_libcxx  = false      # use Haiku libstdc++ / libroot, not Chromium's bundled libc++
use_sysroot        = false      # build NATIVELY on Haiku against system headers/libroot
use_glib           = false
use_gio            = false
use_dbus           = false
use_udev           = false
use_ozone          = true
ozone_platform     = "headless" # ride the existing PNG/software backend
ozone_auto_platforms = false
ozone_platform_headless = true
enable_nacl        = false
use_partition_alloc_as_malloc = false   # PartitionAlloc leans on Linux mm; validate on Haiku
# sandbox: Haiku has no seccomp-BPF / namespaces -> start with --no-sandbox, stub sandbox/
```

The load-bearing deltas, each a real work item:

1. **`target_os="haiku"` does not exist in Chromium's GN/`//build/config`.** Adding a new
   OS to GN's `is_*`/`current_os` machinery, `//build/config/BUILD.gn`, and hundreds of
   `if (is_linux)` sites is itself a substantial patch. QtWebEngine's Haiku port dodged
   this by masquerading as Linux (`isLinuxPlatformSupported` shim) — a viable but ugly
   shortcut that fights the tree at every `is_linux` fork.
2. **gcc, not clang.** Chromium is clang-first (its own bundled clang, PGO/ThinLTO tuned
   for it). Haiku's toolchain is gcc (13.3 in-tree). `is_clang=false` builds are
   second-class upstream and routinely bit-rot; this is a persistent maintenance tax, the
   same one HaikuWebKit lives with (`webpositive-arm64-plan.md` §1).
3. **No sandbox.** Haiku has neither seccomp-BPF nor Linux namespaces. `--no-sandbox` to
   start; a real Haiku sandbox is its own project.
4. **libc / mm assumptions.** No glibc; PartitionAlloc and V8 make Linux mm assumptions
   (W^X, `mmap` flags) that Haiku maps to `create_area`/`BArea` — the JIT class again (§2.3).
5. **mojo handle transport.** POSIX fd passing → Haiku port/area handle transport.
6. **`use_sysroot=false`, native build.** No Debian sysroot; build against Haiku's own
   `haiku_devel` headers and libroot on a native Graviton builder.

### 5.3 Tooling is already there (VERIFIED)

| Tool | Haiku arm64 status |
|---|---|
| **gn** | HaikuPorts `dev-build/gn` recipe, `ARCHITECTURES="all !x86_gcc2"` — builds on arm64. GN is portable C++ (bootstraps with python + ninja). |
| **ninja** | HaikuPorts `dev-build/ninja-1.13.2`, `ARCHITECTURES="all !x86_gcc2"`; already in the DeBeOS pool (`ninja-1.13.2-3`, per the WebKit closure). |
| **Python** | Native CPython **3.14** and **3.10** proven in the DeBeOS pool with full stdlib + pip (#365, `native-cpython-365.md`). depot_tools wants ≥3.9. |
| **depot_tools** | Ships prebuilt gn/ninja via CIPD **with no Haiku build** (INFERRED, high-confidence). Use it only as a `fetch`/`gclient` harness; point the build at Haiku's own gn/ninja/python. |

So the *build-driver* layer is not the blocker. `gn gen` will run once `target_os="haiku"`
exists in the GN config; the blocker is the C++ platform port, not the meta-build.

---

## 6. Build-resource cost (VERIFIED from Chromium's Linux build doc)

- **Disk:** "At least 100 GB free." ~50-80 GB build output, ~30 GB git cache. This alone
  exceeds the default DeBeOS AMI disk and every guest shape used to date — a Chromium
  builder needs a **dedicated, large-disk native Graviton instance** (budget 150-200 GB).
- **RAM:** "at least 8 GB… more than 16 GB highly recommended"; ≥32 GB effective for the
  link spike, or `is_component_build=true` + `symbol_level=0` to survive on less (slower,
  "may have broken functionality"). This is the same governor as WebKit
  (`webpositive-arm64-plan.md` §5), one order of magnitude larger.
- **Checkout:** `fetch chromium` is "~30 min on a fast connection, many hours on slow" —
  and it runs over the DeBeOS NAT egress path, a real risk for a multi-GB fetch.
- **Build time:** the doc gives **no** full-build wall-clock (only an old partial micro-
  benchmark). A full Chromium build on a many-core box is **hours**; treat any specific
  hour figure as **NOT MEASURED**. On a large Graviton (many vCPU, 64 GB) expect *several*
  hours for a first clean build; incremental builds are far cheaper.
- **Python 3.9+**: satisfied (§5.3).

Honest framing: this is **10-30x the WebKit build's resource envelope** on every axis
(disk especially), before a single line of the port is written.

---

## 7. Milestone plan (if the track is ever opened)

Each milestone is a real go/no-go; **M1 is cheap and answers most of the risk.** Do not
proceed past a red gate.

- **M0 — decision gate (this doc).** Is the port worth opening at all? Default answer: no,
  stay on WebKit (§8). Everything below is conditional on M0 flipping.
- **M1 — tooling probe on a native Graviton Haiku builder (cheap, ~½ day, DO THIS FIRST if
  M0 flips).** Install HaikuPorts `gn` + `ninja`, native python3.14, and `depot_tools`;
  confirm `gclient`/`fetch` run and `gn --version` works on Haiku arm64. **Do NOT fetch the
  full tree yet** — first prove the harness runs. Deliverable: "depot_tools + gn + ninja
  run on Haiku arm64" (a real, bounded fact), or the specific failure. *This is the
  smallest honest forward motion and the one artifact worth producing before any port
  work.* **NOT RUN this pass** — see §9.
- **M2 — GN `target_os="haiku"` bring-up.** Add Haiku to Chromium's GN config
  (`//build/config`, `is_*` plumbing) enough that `gn gen out/HaikuArm64Headless` succeeds
  with the §5.2 args. No compile yet — just a valid build graph. Gate: `gn gen` exits 0.
- **M3 — `base/` compiles for haiku-arm64.** The largest single compile milestone; reuse
  QtWebEngine-Haiku's `base/` patches as a starting point, modernise from M87 to current,
  fix arm64. Gate: `autoninja base` links.
- **M4 — headless render.** `sandbox` stubbed (`--no-sandbox`), mojo handle transport,
  `//net` glue, V8 (leaning on #93), SwiftShader/Reactor JIT proven on Haiku arm64 (§2.3).
  Gate: `chrome-headless-shell --ozone-platform=headless --dump-dom` on a trivial page,
  then a **PNG screenshot of a real page** — the true feasibility proof.
- **M5 — SwANGLE / WebGL.** `--use-gl=angle --use-angle=swiftshader`; render a WebGL
  canvas headless. Validates the CPU-GPU-compensation thesis end to end on NEON.
- **M6 — stream tie-in (Model A).** Pipe PNG/DevTools screencast frames into the existing
  capture/stream path; or (Model B, separate and larger) the greenfield windowing Ozone
  backend + #118. Model B is explicitly out of scope for a headless track.

Realistic sizing: **M2-M4 is multi-person-month** platform work. M1 is the only step that
is hours, not months, and it is the one that should gate the rest.

---

## 8. Recommendation: stay on HaikuWebKit; hold Chromium as a gated research track

**Recommended: stay on HaikuWebKit/WebPositive, do NOT open the Chromium port now.**

The comparison is lopsided:

| | HaikuWebKit / WebPositive | Headless Chromium (arm64) |
|---|---|---|
| Engine on arm64 today | **BUILT + RENDERS** (~55 min, screenshot-verified; `webpositive-arm64-plan.md`) | **greenfield platform port**, nothing built |
| Port surface | recipe fork + jam lines; app has ~zero arch-specific code | base/sandbox/mojo/net + GN `target_os="haiku"`, all greenfield on arm64 |
| Native UI | **yes** — real BeOS/Haiku window | headless only (PNG/stream); interactive = Model B = *more* work than WebKit |
| Toolchain | gcc (native) | gcc against a clang-first tree (constant tax) |
| Build cost | ~230 MB source, 16-32 GB RAM, ~1 h | ~100 GB tree, 32 GB RAM, several hours |
| JIT risk | none (WebKit interpreter/baseline; no CPU-GPU JIT needed) | V8 JIT **and** SwiftShader/Reactor JIT, both unproven on Haiku-arm64 |
| Effort to *useful* | **already there** | multi-person-month to first headless PNG |

WebKit already gives DeBeOS a **modern, native, interactive browser on arm64** for a tiny
fraction of the cost, with none of the JIT/sandbox/GN-OS-port risk. Chromium's headless
mode buys a *different* thing — a scriptable, DevTools-driven, WebGL-capable **render/
automation backend**, not a better desktop browser.

**The one scenario that flips this:** a concrete need for **headless browser automation /
server-side rendering / WebGL-in-software at scale** (a render farm, a screenshot/PDF
service, a Puppeteer-style CI backend) that WebKit cannot serve — *and* #93 having already
proven native V8 runs on Graviton (retiring the biggest shared JIT risk). If both hold,
open the track at **M1** (the cheap tooling probe) and re-evaluate at the M4 gate. Absent
that specific need, this is not a good use of multi-month effort.

**Hybrid worth noting:** if only *automation/scripting against a modern engine* is wanted
(not Chromium specifically), **Ladybird** (own engine, in HaikuPorts) or a WebKit-based
headless (`jsc` shell falls out of the HaikuWebKit build for free, per
`webpositive-arm64-plan.md` §7) may satisfy the need at a fraction of Chromium's cost.
Price those before committing to Chromium.

---

## 9. What I did not verify (stated plainly)

- **Nothing was built or fetched.** No Chromium checkout, no `gn gen`, no `depot_tools`
  run on Haiku. M1 (the tooling probe) is **NOT RUN** — it is the recommended first
  artifact, not a result. "gn config drafted" (§5.2) is a draft; "Chromium builds on
  Haiku arm64" is unproven and would be a multi-pass effort.
- **The exact current `chrome-headless-shell` GN target name and whether `enable_nacl=false`
  is required vs. recommended** — the headless README documents usage, not GN args.
- **A quoted full-build wall-clock** — Chromium's doc gives none; the "several hours"
  figure is INFERRED, not measured.
- **SwiftShader/Reactor's W^X + i-cache path on Haiku arm64** — the JIT category is
  de-risked by V8's in-tree port, but SwiftShader's own arm64 codegen on Haiku is
  **unproven** and is a likely first-failure site.
- **The llama.cpp 6.37x SVE figure** cited in #379 — **NOT corroborated** in this tree,
  and moot regardless because SVE traps to `SIGILL` on this kernel (`simd-vectorization-review.md`
  §4) and SwiftShader has no SVE path.
- **Whether any unmerged/experimental arm64 QtWebEngine-Haiku work exists** outside the
  HaikuPorts master recipes (korli/threedeyes repos were not exhaustively crawled).

## Sources

Public Chromium/tooling:
- Ozone platforms + overview: <https://chromium.googlesource.com/chromium/src/+/main/ui/ozone/platform/>,
  <https://chromium.googlesource.com/chromium/src/+/main/docs/ozone_overview.md>
- Linux build (RAM/disk/python/checkout): <https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md>
- ARM cross-build (dated): <https://chromium.googlesource.com/chromium/src/+/main/docs/linux/chromium_arm.md>
- Headless: <https://chromium.googlesource.com/chromium/src/+/main/headless/README.md>
- SwiftShader in Chromium: <https://chromium.googlesource.com/chromium/src/+/main/docs/gpu/swiftshader.md>
- SwiftShader / Reactor (aarch64, JIT backends): <https://github.com/google/swiftshader> (`CMakeLists.txt`, `docs/Reactor.md`)
- GN portability: <https://gn.googlesource.com/gn/+/main/README.md>
- HaikuPorts recipes: `dev-qt/qtwebengine`, `dev-build/gn`, `dev-build/ninja`, `www-client/*` on
  <https://github.com/haikuports/haikuports>

This project (measured here / in-tree):
- `graviton/docs/simd-vectorization-review.md` (SVE traps; no ifunc; NEON safe)
- `graviton/docs/nodejs-arm64-scope.md` (#93 V8-on-Haiku; JIT class)
- `graviton/docs/webpositive-arm64-plan.md` (the WebKit alternative, built + renders)
- `graviton/docs/vfb-route2-streaming.md` (#118 display path)
- `graviton/docs/native-cpython-365.md` (#365 native Python)
- memory `graviton-has-no-video-device` (no GPU / no GOP on Graviton, measured c7g)
</content>
