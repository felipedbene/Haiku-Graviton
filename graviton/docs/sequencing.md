# Graviton/ARM64 — project sequencing

Status board and ordered plan for the whole Graviton effort. For the *optimization*
worklist specifically (compiler baseline, ENA throughput, MMU granules), see
[graviton-optimization-plan.md](graviton-optimization-plan.md); this document
sequences everything, including bringup fixes, tooling, and the userland build.

**Goal hierarchy.** The browser (WebPositive/HaikuWebKit) is a *trial by fire* — the
hardest single target, used to prove the pipeline end to end. A **functional desktop**
is the per-arch pinned package set in `build/jam/repositories/HaikuPorts/<arch>`, not
the whole tree: x86_64 pins **246 entries** = 185 excluding `_devel`/`_source`/
`_debuginfo`, of which ~35 are pure data (user-guide translations, fonts, sounds, wifi
firmware) — so **≈150 packages actually compiled**. arm64 currently pins **34, all
`_bootstrap`-flavoured**. The **full ~3936-recipe tree is the end game** and is what a
builder fleet is for.

> Nothing below is committed. All fixes live as `graviton-*.patch` at the repo root
> plus the working tree, and as candidate AMIs. Per project policy these are **not**
> offered upstream.

## Phase 0 — Boot bringup (DONE)

Fixes that made arm64 boot and build at all, all in the working tree:

| Fix | Evidence |
|---|---|
| ACPI excluded on arm64 → no PCI→USB→disk→boot volume | `build/jam/packages/HaikuBootstrap`, `acpi@x86,x86_64` → `…,arm64` |
| `usb_disk` + `<usb>xhci` missing from package and boot-module symlinks | same file |
| btrfs `zlib.h` race under `-j64` | `src/add-ons/kernel/file_systems/btrfs/Jamfile` |
| bootstrap profile requested a nonexistent `python` | `build/jam/DefaultBuildProfiles` → `python3.10` + `libffi` |

## Phase 1 — Compiler baseline (DONE, hardware-verified)

Optimization-plan items **2** (`-mcpu=neoverse-n1+crypto`), **1** (LSE atomics, subsumed),
**3** (crc/crypto baseline, subsumed) and **9** (cacheline audit — verified already
correct, no change). Patch: `graviton-mcpu-neoverse.patch`.

The plan assumed atomics were inline LL/SC. They were worse: GCC 13.3 defaults to
`-moutline-atomics`, and the kernel's `__aarch64_have_lse_atomics` is an uninitialised
`.bss` symbol with **no constructor**, so it is permanently zero — every atomic paid a
call plus `adrp`/`ldrb`/`cbz`/`ret` **and then still took the LL/SC retry loop**.

Measured at the instruction level (same objects, both ways): kernel outline calls
**1173 → 4**, inline LSE **10 → 1179**, LL/SC pairs **10 → 2**, `ldapr` **0 → 168**;
`libroot.so` **96 → 4** outline. Both binaries got *smaller*. Boots clean on real
Neoverse cores. Incidental beneficiary: the **ENA driver**, whose per-interrupt
counters (`ena.cpp:173`, `:194`), keepalive timestamps, refcounts and ena-com's
`ATOMIC32_*`/waitqueue all sit on these primitives.

**Open:** no *performance* measurement yet — the instruction change is proven, the
speedup is inferred. A contended-atomics A/B benchmark is the one thing needed to
close this phase without qualification.

## Phase 2 — Toolchain and userland (IN PROGRESS — critical path)

Three blockers were removed today; the chain had never been able to run unattended.

- **`packagefs` mount-failure panic — FIXED** (`graviton-packagefs-unmount.patch`).
  `Volume::~Volume` deleted its indices *outside* the write-locked scope while index
  destructors call `RemoveNodeListener`, which asserts that lock. Invisible on unmount
  (which holds the lock across `delete`), fatal on every **failing** mount (which drops
  it first). Plus a NULL-deref in `_SystemVolumeIfNotSelf()` when `Mount()` fails early.
  Survives 900 mount operations; unpatched panicked on failing-mount #1. Not
  arm64-specific.
- **Wall clock booted to 1970 — FIXED** (`graviton-arm64-rtc.patch`). `arch_rtc_get_hw_time()`
  was literally `return 0;`. Now seeded from EFI `GetTime()` sampled late in the loader.
- **`--do-bootstrap` abandoned as a strategy.** Its planner must order all 111 cyclic
  ports; we drove stuck 59 → 40, then hit a self-referential PEP-517 Python knot that
  edge-cutting cannot break.

**Ordered chain** (native builds need prerequisites *installed*, not merely planned —
each `.hpkg` is installed before the next port builds):

```
perl → autoconf → automake → libtool → xz_utils → openssl3 → curl
     → cmake → sqlite → libpng/libjpeg_turbo/libwebp → fontconfig → libxml2
```

Scouted levers: **cmake** with bundled libs breaks the cmake↔curl↔openssl3↔zstd cycle
(its recipe *already* comments out five deps "to avoid circular deps", so the technique
is established); **meson** is pure Python — retarget to python3.10 and skip PEP-517
entirely; **ninja** bootstraps with `python3 ./configure.py --bootstrap`.

**Two-stage discipline is mandatory.** Anything crippled to break a cycle
(bundled-libs cmake, the groff/gtkdocize doc-dep cuts) is a *stage-1 expedient* written
to `hpkg-out/arm64/stage1/` and **must be rebuilt properly** once its real deps exist,
or the whole tree inherits the defect.

### Stage-1 expedients: the argument, and the ledger (2026-08-22)

**The objection, stated fairly: cutting doc deps is deferring pain, not removing it.**
That is true, and the debt must be tracked. The reason to take the cut anyway is that
the pain is *asymmetric*. Today the doc deps form a **serial deadlock** — nothing in the
chain can build, one port at a time, on an otherwise-idle 64-vCPU box. Once
perl/autoconf/automake/libtool/cmake exist natively, texinfo's cycle **dissolves**,
because the tools it needs are simply present; the same work becomes an
embarrassingly-parallel rebuild across as many builder guests as we care to run. We are
converting an unbreakable serial blocker into cheap parallel work, not avoiding it.

**The real cost is not effort, it is silent contamination.** A stage-1 package with empty
`.info`/man pages that leaks into a shipping repo, or a downstream port that
build-depends on a doc artifact and fails confusingly much later. So the mitigation is
bookkeeping, enforced:

- stage-1 artifacts go to **`hpkg-out/arm64/stage1/`** and never to a shipping repo;
- every cut is commented **in the recipe** with what was cut, why, and the rebuild
  condition (see the `BUILD()` note in the autoconf source-package recipe);
- this ledger lists them, and each must be retired.

| Port | Cut | Why | Retire when |
|---|---|---|---|
| `autoconf-2.72` | `makeinfo` replaced by a stub that creates an empty `-o` file; `make install-html` dropped | `makeinfo` cannot run at all in this image (see hazard 1) | a real `texinfo` exists → rebuild, expect real docs |

**Hazards found the hard way, all of which cost time (2026-08-22):**

1. **`texinfo_bootstrap` is a non-functional doc stub.** It ships the `Texinfo/`
   directory but **zero `.pm` files**, and `Texinfo/ModulePath.pm` is nowhere in the
   image, so `texi2any` dies in its `BEGIN` block under *any* perl. This is not
   autoconf-specific: **every port whose build invokes `makeinfo` fails identically.**
   Expect to repeat this cut until texinfo is real.
2. **`input-source-packages/` silently overrules the ports tree.** 116 `*_source_rigged`
   hpkgs left over from the abandoned `--do-bootstrap` run carry their *own* recipe, and
   haikuporter prefers them — so a fix to the tree recipe is **ignored without warning**
   (it cost one wasted build cycle before the `<source-package>::` line in the log was
   noticed). Patch the recipe under
   `input-source-packages/develop/sources/<port>/`, or check that path first.
   **Do not simply delete these:** the guest's bootstrap `curl` reports
   `Protocol "https" disabled`, so with the local source packages gone the guest cannot
   fetch *any* source. Removing them is gated on a real HTTPS `curl` — which is already
   a toolchain checkpoint below.
3. **The builder guest's clock is ~4 h 53 m behind the host** (measured: host
   `1787436517` vs guest `1787418948`). Every make-based port therefore prints
   `Clock skew detected. Your build may be incomplete.` and any mtime-based build trick
   (e.g. pre-`touch`ing targets) is silently defeated. This is **our own bug**, not a
   haikuports one — the RTC is seeded from EFI `GetTime()` (see Phase 2) and lands wrong
   under QEMU. Worth fixing early: "your build may be incomplete" across the whole
   userland is not a warning to live with.
4. **`LIBRARY_PATH` replaces the loader path, it does not prepend.** Cost a day-equivalent
   of misdiagnosis on perl. See the dedicated note in Phase 2.

**Checkpoints:** unattended `zlib` ×3 → toolchain milestone (`tar`, `xz`, a real HTTPS
`curl` fetch, `python3 -c "import ssl, zlib, lzma, ctypes"`). Note that two of these are
now known-broken and load-bearing for other work: `curl` has no HTTPS (hazard 2) and
`python3` has no `gzip`/`zlib` module at all, which is why seeding the recipe tree had to
stream an *uncompressed* tar into `python3 tarfile`.

## Phase 3 — Tooling: guest agent over serial (NEXT IMAGE BAKE)

A minimal Haiku guest agent giving **command execution and file transfer with zero
networking**, read from a second serial port by a launch_daemon job (first cut can be a
shell loop; a small C daemon if framing demands it).

**Do not port `qemu-guest-agent`:** it needs **GLib**, and `glib2` is meson-only —
blocked behind the very PEP-517 knot Phase 2 routes around — and its native transport is
**virtio-serial**, for which Haiku has no driver (only `pc_serial`/`usb_serial`).
`pc_serial` is already packaged **ungated in every image** (`build/jam/packages/Haiku:203`,
`HaikuBootstrap:146`), so the cheap transport exists today.

Motivation, all of which has already cost time: bootstrap images have dead guest
networking (`net_server`/`syslog_daemon`/`power_daemon` die); the EC2 test instance's
security group allows `:22` only from external CIDRs so the metal cannot SSH in;
corporate egress blocks outbound `:22` from the workstation; and guests are currently
driven by base64-through-SSM plus hand-rolled QEMU-monitor Python.

Scope honestly: it does **not** solve in-chroot command injection (a haikuporter
limitation), clipboard/resize (spice-vdagent territory, assessed as not worth porting),
or anything on real EC2 (no QEMU there).

**Sequencing rule: fold into the next image bake, never an in-flight one** — the value
only exists once baked in, since if you can SSH in to start it, you didn't need it.

## Phase 4 — Functional desktop (≈150 packages)

Reach x86_64 parity for arm64: grow `build/jam/repositories/HaikuPorts/arm64` from 34
bootstrap entries to the real ~185. Includes `haikuwebkit-1.10.0` (the target version;
x86_64 pins it).

**Rust is avoidable for the browser:** our libavif recipe already uses the C decoder
**dav1d**, no `rav1e` exists in the tree, and the tree *already contains*
`media-libs/libavif/libavif1.0-1.4.2.recipe` — the soname-16 line WebKit requires
(`devel:libavif >= 16` is an soname, not a release). WebKit also needs `cmd:ruby`;
`ruby-3.2.9` covers arm64 and is plain autotools. Rust remains required for the wider
tree — deferred, not dodged.

**WebPositive is gated off for arm64** in `build/jam/DefaultBuildProfiles` purely because
`haikuwebkit` isn't pinned for us; `build/jam/BuildFeatures:303` already keys off
`IsPackageAvailable haikuwebkit_devel`, so enabling it is a one-line change plus the repo
entry once the package exists.

## Phase 5 — Remote desktop for daily use

Stage 1 is **done and measured**; Stage 2 is the remaining work.

- **Proven:** under KVM with `virtio-gpu-pci` (or `ramfb`), app_server paints a real
  desktop — no code changes. Driven over QEMU's VNC: keystroke→framebuffer **p50 31 ms /
  p99 81 ms**, window drag **1.35 Mbit/s** at 0.188 bytes per changed pixel, idle 1.7% of
  one core, **reconnect 8/8 clean**, two simultaneous clients, guest-initiated mode
  switching to 2048×1152.
- **Use `usb-tablet`/`usb-kbd` on the xhci bus** — `virtio-tablet-pci` yields *no input*;
  `virtio_input` never binds despite being installed.
- Ordered fixes: (1) host-driven resize wiring (client `SetDesktopSize` returns status=4
  today), (2) `virtio_gpu_open` `open_count` guard, (3) damage-based flush **last** —
  measured payoff is small and latency is floored by QEMU's 30 ms poll.
- Then Stage 2: headless wlroots + Sunshine/Moonlight, or a WebRTC gateway if
  browser-only access is required. Bare EC2 has **no display device at all**, so this
  path is inherently KVM-hosted; the legacy RemoteHWInterface survives only as a rescue
  console.
- **KVM requires a `.metal` size** (verified: metal initialises KVM as a host,
  virtualized instances only detect being a guest). `g5g.metal` is the sole GPU option
  that can host this, at **+18%** over `c7g.metal` — worth it only for many concurrent
  streams or 4K, never as an enabler.

## Phase 6 — Graceful reboot, inbound half

Outbound is **DONE** (`graviton-arm64-reset.patch`): `arch_cpu_shutdown()` was an empty
stub, now PSCI `SYSTEM_RESET`/`SYSTEM_OFF` with the conduit plumbed through
`kernel_args`. Hardware-verified that the conduit plumbing works; see the caveat below.

**The inbound design is now settled empirically.** Sampling `/proc/interrupts` at 5 Hz on
an Amazon Linux guest while issuing `reboot-instances`, **exactly one** interrupt fired —
`IRQ 49 = ARMH0061:00 hwirq 3, "ACPI:Event"` — while **all 32 `ACPI:Ged` counters stayed
at 0**. So EC2 uses the **PL061 `_AEI`/`_Exx` path on virtualized instances too**; the GED
exists but is unused. Reference: Amazon Linux on that instance reached `stopped` in 68 s.

| | c7g.metal | t4g.medium |
|---|---|---|
| `arm_boot_arch` | `0x0001` → **SMC** | `0x0003` → **HVC** |
| `ACPI0013` (GED) | absent | present (32 GSIVs 48-79), **unused** |
| `ARMH0061` (PL061) | `_AEI` + `_E00`, IRQ 23 | `_AEI` + `_E03`/`_E04`, IRQs 49-50 |

**The PSCI conduit differs by platform, and so does the GPIO pin.** Two consequences:
the implementation must **walk `_AEI`** rather than hardcode a pin (`_E00`/pin 0 on metal
vs `_E03`/pin 3 on t4g); and **the `smc` branch still cannot be tested on any virtualized
instance** — it needs metal, or QEMU with TF-A as BL31 to provide PSCI over SMC (plain
AAVMF/EDK2 only ever offers `hvc`, and cannot boot at EL3).

Remaining, in dependency order:

1. A **PL061 GPIO driver** (`ARMH0061`), MMIO + IRQ from `_CRS`.
2. An **ACPI GPIO-event layer**: walk `_AEI`, hook the GSIV, evaluate the matching `_Exx`
   so the AML's `Notify(\_SB.PWRB, 0x80)` fires. Haiku has zero support for this.
3. **Un-gate `acpi_button` for arm64** — `build/jam/images/definitions/minimum:347-354`
   restricts power drivers to `x86,x86_64`; `/dev/power` is empty as a result.
4. Skip the bogus fixed-button registration when `HW_REDUCED_ACPI` is set
   (`src/add-ons/kernel/bus_managers/acpi/Module.cpp:190,208`).

`power_daemon` is already in the arm64 image and headless-capable — it is starved of a
device, not a handler.

## Phase 7 — Optimization, remainder

Optimization-plan items **5** (ENA multi-queue + RSS — the real throughput lever),
**4** (LLQ), **6** (jumbo frames), **8** (spinlock WFE/SEV), **7** (barrier tuning:
ena-com uses `dsb sy` throughout), **10** (16K/64K granules — research spike only).

**Gate: these need a measurable workload**, i.e. a userland that can generate real
network and desktop load. Deliberately sequenced after Phase 2/4.

## Phase 8 — Fleet scale-out

Buildmaster plus a Graviton builder fleet for the ~3936-recipe long tail. Gated on a
green toolchain: until the chain builds *anything*, every builder would fail identically
on the same cycle.

## Blocker: headless images lose networking (IN PROGRESS)

On any image without a framebuffer — i.e. **every bare EC2 instance**, since Graviton has
no display device — boot produces a deterministic cascade:
`app_server: Failed to initialize virtual screen configuration` →
`Can't reconnect to app server!` → **`Killing team … (net_server)`**. With `net_server`
dead there is no DHCP and no interface, so **you cannot SSH into a Haiku EC2 instance at
all.** This blocked the in-guest half of two separate hardware verifications today (the
RTC's in-guest `date`, and the PSCI reset/power-off tests), each of which had to fall back
to reading the serial console. It is pre-existing, not a regression, and the security group
is *not* the cause.

Being fixed as `graviton-headless-netserver.patch`. The likely template is `power_daemon`,
which is a `BServer` constructed with **`initGUI=false`** and therefore survives headless.
Explicitly **not** the fix: shipping a fake screen or the `TARGET_SCREEN` remote-desktop
plumbing in every image — that hides the defect rather than fixing it. Related: arm64 has
**no serial break-in for KDL** (the break-in path is x86/PS2-only), so KDL is unreachable
on a live guest and a harness image is needed to drive one.

## Cross-cutting operational notes

- **Never `git clone /opt/haiku/haiku`** on the builder — it sits on upstream master with
  the graviton work uncommitted, so a clone silently loses GICv3/ITS and hangs at
  `usb xhci: starting XHCI host controller`. Use
  `git clone --depth 1 -b graviton https://github.com/felipedbene/Haiku-Graviton`, and
  pass `-sHAIKU_REVISION=hrev59996` (a shallow clone breaks revision detection, and jam
  then *deletes* `build/haiku-revision`).
- `@minimum-mmc` is the working image profile; `@nightly-mmc` cannot build for arm64
  (needs `libmidi.so@fluidlite` + the wonderbrush translator); `@bootstrap-mmc` is the
  ~20 GB self-hosting builder.
- `aws ec2 get-console-output` works on Graviton, **including on stopped instances** —
  the primary observability channel now that the UART stride fix makes the log
  trustworthy (page-table lines went 2% → 100% intact; it had been corrupting ENA
  capability registers).
- `create-replace-root-volume-task` requires the instance **running**, and `--dry-run`
  does *not* catch that. Replacing *back* to an earlier AMI can be rejected
  (`enhanced networking component [sr-iov] cannot be removed`), and snapshot-based replace
  is rejected unless the snapshot was of a previously attached root — so keep a
  detach/attach rollback path, and keep the old root volumes `available` rather than
  deleting them.
- `get-console-output` is a **sliding ~64 KB window**, so counting boot banners does not
  detect a reboot (the count stays at 1). Use the loader's `rtc: firmware time is …`
  timestamp as the discriminator instead — this nearly produced a false conclusion.
- `reboot-instances` against Haiku currently takes **~4 min 05 s** before AWS's hard-reboot
  fallback fires, matching AWS's documented ~4 minute grace period. That is the signature
  of the guest ignoring the event, not of a slow reboot.
