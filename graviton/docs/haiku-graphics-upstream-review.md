# Upstream Haiku graphics work: what landed, and does any of it help us?

A review of upstream Haiku's recent display/graphics work against this fork, to answer
one question: **"Haiku added graphics support recently — should we adopt any of it?"**

Evidence is local git history (`haiku-upstream/master` at `28e91d7076`, 2026-08-23) plus
measurements taken on our own instances. Every load-bearing claim carries a SHA or a
`file:line`. Claims are labelled **VERIFIED** (read or measured here) or **INFERRED**.

---

## Bottom line

**The premise is mostly wrong, and the good news is better than the premise.**

1. There is no recent upstream *graphics stack* work that we are missing. Of the 14
   upstream commits not in `graviton`, **zero** touch any graphics path. Every notable
   graphics commit from 2024 through 2026-08 is **already in our branch** — 22 of 22
   checked. We are not behind.
2. The `virtio_gpu` driver and accelerant we most wanted **already exist, are already in
   `graviton`, and are not arch-gated** — so they are compiled and packaged for arm64
   today. They are not new (Jan 2024) and have had **zero functional changes in 23
   months**.
3. The genuinely recent and genuinely relevant upstream work is not in the graphics
   directories at all — it is the **arm64/QEMU virtio bus fixes of August 2026**, one of
   which states it was tested on arm64 with `virtio-gpu-{device,pci}` and that "GPU works
   fine". That commit is already ours.
4. For **bare Graviton EC2 there is nothing to adopt, ever**, because there is no display
   device. This is not a gap to close; it is the shape of the platform.

**Single highest-value action:** add memory barriers and `volatile` to the **PCI**
virtqueue implementation (`src/add-ons/kernel/bus_managers/virtio/VirtioQueue.cpp`,
`virtio_ring.h`), which today has **zero** of either, then switch one KVM rig from `ramfb`
to `virtio-gpu-pci`. This is a small, self-contained arm64 correctness fix that benefits
*every* virtio device we use — not just the GPU — and it is the one thing standing between
us and a display with real modesetting. See [GO-1](#go-1) and
[Effort and risk](#effort-and-risk).

---

## 1. What upstream actually landed

### 1a. The virtio GPU driver — real, but old

**VERIFIED.** The complete history of both virtio graphics paths is **four commits, ever**:

| SHA | Date | Subject |
|---|---|---|
| `0b733c9c80` | 2024-01-18 | `virtio_gpu: initial driver` |
| `b352d8ccd8` | 2024-01-24 | `virtio_gpu: add the accelerant to the regular image` |
| `d49d21bba0` | 2024-01-29 | `graphics/virtio: Disable tracing.` |
| `b90dc7a4f3` | 2024-09-05 | `virtio: Explicitly request queue sizes where needed.` |

Three are the original landing week. The fourth is one line of mechanical API churn
(`alloc_queues(..., virtioQueues)` → `alloc_queues(..., virtioQueues, NULL)`). **All four
are in `graviton`.** So "Haiku recently added a virtio GPU driver" is off by two and a
half years, and we already have it.

### 1b. The recent work that *does* matter — arm64 virtio, August 2026

**VERIFIED.** This is the find of the review. `b57d5d2f41` (2026-08-20, **in `graviton`**),
`virtio: Fix input event delivery under QEMU`, says in its own message:

> MMIO had the virtqueue laid out in a way incompatible with the legacy transport, which
> QEMU offers by default […] virtio-blk was working by chance, but virtio-input and
> **virtio-gpu dropped events silently**.
> […]
> **Mouse and GPU work fine**, keyboard needs mapping from AT layout.
> Tested on arm64, under QEMU 11.1 […] with devices: `virtio-{tablet,keyboard,gpu}-{device,pci}`.

Someone upstream is actively exercising virtio-gpu on arm64 under QEMU *right now*, and
reports it working. That is the strongest single piece of external evidence available for
the option we care about, it is four days old, and **we already have it**.

Two follow-ups from the same push are among our 14 missing commits:

- `c32c67cba4` 2026-08-23 `virtio_mmio: serialize queue access and add memory barriers` — **NOT in `graviton`**
- `45491eda98` 2026-08-23 `virtio_mmio: don't write queueSel from Dequeue()` — **NOT in `graviton`**

Both touch only `busses/virtio/virtio_mmio/`, i.e. the **MMIO** transport. They do not
affect `virtio-gpu-pci`. Worth taking on the next merge; not urgent for the GPU.

### 1c. app_server and the display stack, 2025–2026

**VERIFIED** — a real and steady stream of work, and **all of it already in `graviton`**.
The substantive items:

| SHA | Date | Subject | In `graviton`? |
|---|---|---|---|
| `c216360337` | 2026-06-17 | `kernel/interface: 30-bit RGB pixel format support` | IN |
| `6db74d85be` | 2026-06-30 | `efi: Base framebuffer info on selected video mode` | IN |
| `0e76803f9e` | 2026-07-01 | `vesa: Update the kernel framebuffer information when changing modes.` | IN |
| `0088ac13b0` | 2026-05-28 | `app_server: Refactors to screen configuration management.` | IN |
| `22a2180aa5` | 2026-06-14 | `app_server: Always store the current configuration when adding screens.` | IN |
| `ad6cb5efd3` | 2026-06-30 | `app_server: More fixes to screen configuration management.` | IN |
| `dbaf648047` | 2026-06-23 | `app_server: Properly lock the HWInterface in cursor routines.` | IN |
| `8bd9d9bb9b` | 2026-07-01 | `app_server: Fix lock order inversion in HWInterface cursor routines.` | IN |
| `32a55535a4` | 2026-03-10 | `app_server: Fix incorrect size for bitmap hardware cursor` | IN |
| `30cb44085c` | 2026-06-01 | `If B_SET_CURSOR_BITMAP hook fails, try B_SET_CURSOR_SHAPE` | IN |
| `9be0e148a2` | 2026-03-24 | `vesa & framebuffer: Clone the framebuffer instead of having it be user-accessible.` | IN |
| `861f644f19` | 2026-03-24 | `app_server & BWindowScreen: Do away with AS_GET_FRAME_BUFFER_CONFIG.` | IN |
| `f6be811473` | 2026-03-24 | `BDirectWindow: Clone the framebuffer from app_server, if necessary.` | IN |
| `03f77fd7d9` | 2024-12-10 | `app_server: drop legacy 2D hardware acceleration` | IN |
| `f8e01ad15c` | 2025-06-26 | `app_server: Render cursors from vector icons based on the font size.` | IN |
| `2b5263a0d9` | 2025-03-31 | `drivers/framebuffer: do not attempt to map whole PCI BAR` | IN |

The March-2026 trio (`9be0e148a2`, `861f644f19`, `f6be811473`) is a framebuffer-exposure
rework — the framebuffer is now cloned rather than handed to userland directly. Note
`03f77fd7d9` **dropped legacy 2D hardware acceleration from app_server entirely**, which
means an accelerant no longer needs blit/fill hooks to be first-class. That materially
lowers the bar for `virtio_gpu`'s minimal hook set (§2c).

The rest of the 2025–2026 traffic in these paths is `intel_extreme`/`radeon_hd`/`s3`
device-ID and 64-bit fixes, AGG/BPicture drawing correctness, and Interface Kit widget
work. **NOT-APPLICABLE**: we have no Intel, AMD or S3 hardware and never will.

### 1d. Things that do not exist

**VERIFIED by absence:**

- **Vulkan: does not exist.** `git log --grep=vulkan -i` over all of upstream returns two
  commits whose messages merely contain the substring in other words. There is no Vulkan
  in Haiku.
- **Mesa/Gallium is not in the Haiku tree at all.** It is an external package. In-tree
  there is only `headers/libs/glut/GL/glut.h` and a fake renderer under
  `src/tests/add-ons/opengl/`. No upstream Haiku source change can advance our Mesa build.
- **`libglvnd` is not in-tree** either; one 2023 commit (`11e68e0c54`) adapts an app to it.
- **Multi-monitor: still architecturally absent.** `ScreenManager::_ScanDrivers()` creates
  exactly one interface and stops — *"Eventually we will loop through drivers […] For now,
  we'll just load one and be done with it."* And
  `src/servers/app/VirtualScreen.cpp:153 // TODO: this works only for single screen configurations`.
- **HiDPI**: the 2025–2026 commits are per-application widget scaling (Installer, Expander,
  Printers, ProcessController). There is no display-stack scaling feature. Irrelevant to us.

---

## 2. State of `virtio_gpu`, and whether it reaches arm64

### 2a. It is NOT arch-gated — this is the good news

Our standing hazard is that arch-gated kernel add-ons rot silently because they never
compile. **`virtio_gpu` is not subject to it.** VERIFIED in `build/jam/packages/Haiku`,
where the arch-gating syntax is a `target@arch,arch` suffix:

```
build/jam/packages/Haiku:136   AddNewDriversToPackage graphics :
build/jam/packages/Haiku:137       virtio_gpu                      # <-- no @arch suffix
build/jam/packages/Haiku:366   AddFilesToPackage add-ons accelerants :
build/jam/packages/Haiku:367       framebuffer.accelerant
build/jam/packages/Haiku:368       virtio_gpu.accelerant           # <-- no @arch suffix
```

Contrast the same file's gated entries — `virtio_block@arm,arm64,riscv64`,
`virtio_mmio@riscv64,arm,arm64`, `wmi@x86,x86_64`. Neither
`src/add-ons/kernel/drivers/graphics/Jamfile` nor `src/add-ons/accelerants/Jamfile`
conditions its `SubInclude` on architecture either. **So `virtio_gpu` builds and ships on
arm64 in any profile that uses `packages/Haiku`.**

**Two caveats, VERIFIED:**

- The **bootstrap** package omits it — `build/jam/packages/HaikuBootstrap:242-244` lists
  only `framebuffer.accelerant`, and no `virtio_gpu` driver.
- `SYSTEM_ADD_ONS_ACCELERANTS` in `build/jam/images/definitions/minimum` gives **arm64
  nothing** (x86 gets `vesa.accelerant`; riscv64 gets `ati`/`radeon_hd`). That is fine
  only because `packages/Haiku` adds `framebuffer.accelerant` and `virtio_gpu.accelerant`
  unconditionally on top. Worth remembering if a profile ever bypasses `packages/Haiku`.

### 2b. What the driver actually does

**VERIFIED** by reading `src/add-ons/kernel/drivers/graphics/virtio/virtio_gpu.cpp`
(897 lines) and `viogpu.h`:

- **2D only, deliberately.** It negotiates exactly one feature —
  `virtio_gpu.cpp:480-481` negotiates `VIRTIO_GPU_F_EDID` and nothing else. `VIRTIO_GPU_F_VIRGL`
  is defined in `viogpu.h:53` and never requested. No 3D contexts, no `SUBMIT_3D`, no
  `RESOURCE_BLOB`. **It will not accelerate GL.**
- **Real modesetting** — `virtio_gpu_set_display_mode` creates a new 2D resource,
  re-attaches the same backing, and re-issues `SET_SCANOUT`. This is the headline
  difference from `ramfb`, whose accelerant refuses any *actual* mode change:
  `framebuffer_set_display_mode` returns `B_OK` only when the requested mode already
  equals `current_mode`, and otherwise
  `src/add-ons/accelerants/framebuffer/mode.cpp:103 return B_UNSUPPORTED;`.
- **Transport-agnostic.** It matches on generic attributes only —
  `virtio_gpu.cpp:764-772` checks `B_DEVICE_BUS == "virtio"` and
  `VIRTIO_DEVICE_TYPE_ITEM == VIRTIO_DEVICE_ID_GPU` (16). Both `virtio-gpu-pci` (PCI ID
  `0x1050` → `0x1050-0x1040` = 16) and `virtio-gpu-device` (MMIO) publish those, so
  **either QEMU device model binds** — but they run completely different queue code, which
  is the crux of §6.
- **No hardware cursor.** The cursor queue is allocated (`:487-495`) and drained, but
  `VIRTIO_GPU_CMD_UPDATE_CURSOR` is never issued. app_server does a software cursor.
- **Unconditional 50 Hz full-screen blit.** `virtio_gpu.cpp:395-404` runs a
  `B_DISPLAY_PRIORITY` kernel thread that re-uploads the *entire* framebuffer every 20 ms
  with no damage tracking. **INFERRED** from the geometry: at 1920×1080×4 that is roughly
  415 MB/s of guest→host copy, permanently, even on an idle desktop. On a metal host
  running several build guests this is not free.
- **A fixed 31.6 MiB physically contiguous allocation** at open —
  `virtio_gpu.cpp:591-598` allocates 3840×2160×4 as `B_FULL_LOCK | B_CONTIGUOUS`
  regardless of the actual mode, "so we can fit every mode".

### 2c. The accelerant

**VERIFIED** — 15 hooks, all in the mandatory/mode group (`hooks.cpp:17-51`). Notably
**absent**: `B_SET_CURSOR_*`, `B_FILL_RECTANGLE`, `B_SCREEN_TO_SCREEN_BLIT`, overlays,
DPMS, `B_PROPOSE_DISPLAY_MODE`. `B_ACCELERANT_RETRACE_SEMAPHORE` returns `-1`
(`accelerant.cpp:200`) so there is no vsync. Since `03f77fd7d9` removed app_server's
legacy 2D acceleration, the missing blit/fill hooks cost us nothing.

Mode list (`mode.cpp:50-78`): with EDID present — and QEMU's virtio-gpu does advertise
`VIRTIO_GPU_F_EDID` — we get a full EDID-derived list with no support filter. Without
EDID, exactly one mode.

Two latent defects found while reading, **VERIFIED**, neither yet hit by us:

- `virtio_gpu_get_accelerant_clone_info` issues `VIRTIO_GPU_GET_DEVICE_NAME`
  (`accelerant.cpp:124`), an ioctl the driver's switch (`virtio_gpu.cpp:717-746`) does not
  handle → falls to `default:` → `B_DEV_INVALID_IOCTL`. **Cloning the accelerant fails.**
- `virtio_gpu_open()` (`:556`) unconditionally recreates the `commandDone` semaphore and
  the shared and framebuffer areas on **every** open, with no `open_count` guard, and
  `virtio_gpu_close()` (`:651`) unconditionally deletes `commandDone`. A second opener
  leaks the first's resources; the first closer breaks the second. This is the
  `open_count` guard our own Phase 5 notes already listed as a needed fix; it is **still
  unfixed upstream**.

### 2d. It wins over the plain framebuffer automatically

**VERIFIED** in `src/servers/app/drawing/interface/local/AccelerantHWInterface.cpp`:
`_RecursiveScan` over `/dev/graphics/` explicitly **skips** `vesa` and `framebuffer`
(`:227-232`) and only opens them as a last resort (`:267-281`). `virtio_gpu` publishes
`/dev/graphics/virtio/0`, so **it is preferred with no configuration change**.

The failure mode matters: if `virtio_gpu_open()` fails, the fallback arithmetic in
`_OpenGraphicsDevice` does **not** reach the framebuffer branch, so app_server gets
`B_ENTRY_NOT_FOUND` and **no display at all** rather than degrading to `ramfb`. The
safe-mode "fail-safe video mode" option forces framebuffer/vesa and is the escape hatch.

---

## 3. What `graviton` is missing, and what we carry

### 3a. Missing: nothing that matters

**VERIFIED.** `graviton..haiku-upstream/master` is **14 commits**. Their subjects are EFI
bootloader options, USB audio, arm64 SMP-via-FADT, two `virtio_mmio` fixes, three
`pch_i2c` fixes, three kernel thread/lock fixes, translations, and a device-tree unit
test. **Zero touch any graphics path.** Only two are even adjacent:

- `c32c67cba4`, `45491eda98` — the `virtio_mmio` barrier/serialization fixes (§1b).
  MMIO-only; take them on the next routine merge.

Separately, `2716c1adb9` (arm64 SMP via FADT) and `28e91d7076` (EFI bootloader options)
are arm64-relevant but not graphics. Out of scope here; flagging them for whoever does the
next merge.

### 3b. Carried locally: three small deltas, low conflict risk

**VERIFIED** — `git diff haiku-upstream/master refs/heads/graviton` over
`src/add-ons/kernel/drivers/graphics/`, `src/add-ons/accelerants/` and
`headers/private/graphics/` is **empty**. We carry no local changes to any graphics driver
or accelerant. The graphics-adjacent deltas we do carry:

1. **`3389f22b2c`** `app_server: don't dereference a screen's owner unconditionally on
   change` — 8 lines in `ScreenManager.cpp::ScreenChanged()`. It is a genuine upstream
   bug: `ReleaseScreens()` sets `item->owner = NULL`, and `AcquireScreens()` leaves it NULL
   for unclaimed screens, yet `ScreenChanged()` dereferenced it unconditionally. Merge
   conflict risk is **low** — upstream's 2026 screen-config refactors (`0088ac13b0`,
   `ad6cb5efd3`, `22a2180aa5`, `2a0ccdfd34`) are all already in our branch and none touch
   this function. Good upstream bug-report candidate.
2. **`8446374dda`** `app_server/remote: bind the remote display to loopback, not
   INADDR_ANY` — 15 lines in `RemoteHWInterface.cpp`. Security-motivated; the remote
   protocol has no auth or encryption, so SSH is the entire security model.
3. **`a736a16598`** `RemoteDesktop: pass a path to execl()`. Upstream's documented `-s`
   (SSH) mode has been broken for years — `execl("ssh", "-C", ...)` ate `-C` as `argv[0]`.
   Only findable by actually running it, which tells us nobody upstream does.

Also relevant and ours: substantial local work in `src/add-ons/kernel/busses/pci/ecam/`
(~240 lines of per-root-bridge MCFG selection and bus rebasing). That is the code that has
to enumerate a `virtio-gpu-pci` device in the first place, so it is on the critical path
for [GO-1](#go-1).

**We are behind upstream in `busses/virtio/virtio_mmio/`** — we lack the barriers and the
`InterruptsSpinLocker` from `c32c67cba4`. MMIO path only.

---

## 4. Does it help us? Ranked, with a verdict each

### Goal (a) — a usable desktop on real Graviton EC2

**NOT-APPLICABLE. Nothing upstream can help, now or later.**

There is no display device on a Graviton instance. The load-bearing proof is our own:
**Haiku's EFI loader reports `GOP protocol not found` on a booting c7g**, so it hands the
kernel `frame_buffer.enabled = false`. A driver cannot bind to a device that is not there.
Every item in §1c is irrelevant to bare EC2 — not disappointing, just the platform.

What we do on bare EC2 instead is already built and shipping: app_server's
`RemoteHWInterface` display-list backend, selected by `TARGET_SCREEN`
(`src/kits/app/AppMisc.cpp:223` → `src/servers/app/AppServer.cpp:118` →
`src/servers/app/ScreenManager.cpp:146`), viewed from a Linux browser through the in-tree
HTML5 client over an SSH tunnel. **VERIFIED** it is not arch-gated:
`src/servers/app/drawing/interface/remote/Jamfile` builds `libasremote.a` unconditionally
and `src/servers/app/Jamfile:113` links it unconditionally, so the remote backend is
inside **every** app_server on every architecture.

| Item | Verdict |
|---|---|
| `virtio_gpu` driver + accelerant | NOT-APPLICABLE — no virtio GPU on EC2 |
| `intel_extreme` / `radeon_hd` / `s3` / `nvidia` work | NOT-APPLICABLE — no such hardware |
| 30-bit RGB, EFI framebuffer mode info, vesa modeset | NOT-APPLICABLE — no framebuffer |
| Multi-monitor, HiDPI | NOT-APPLICABLE — no monitor |

### Goal (b) — better display in our KVM guests (how we verify GUI work)

**This is the only goal where anything is on the table, and the win is real but bounded.**

**Measured on our c7g.metal builder (2026-08-25, load average 5.34):** all five live QEMU
guests and all ten display-device references in the rig boot scripts use **`ramfb`**.
There is **zero** `virtio-gpu` in any rig script or any running guest. The rigs also use
`usb-tablet`/`usb-kbd` (3 and 2 references), consistent with our earlier finding that
`virtio-tablet-pci` yields no input.

So although `graviton/docs/sequencing.md:223` records virtio-gpu-pci as previously proven
to paint a desktop, **nothing we run today uses it.** Switching is an unrealized change,
not a done deal.

| Item | Verdict | Why |
|---|---|---|
| Switch a KVM rig to `virtio-gpu-pci` | **GO** — see [GO-1](#go-1) | Real `B_SET_DISPLAY_MODE` and an EDID mode list vs `ramfb`'s `B_UNSUPPORTED`; app_server prefers it automatically |
| PCI virtqueue barriers + `volatile` | **GO — do this first** | Zero barriers today; arm64 hazard; benefits every virtio device |
| `virtio_gpu` `open_count` guard | **GO (small)** | Still unfixed upstream; bites on a second opener |
| Accelerant-clone ioctl gap | **GO (trivial)** | Unhandled ioctl makes clone fail |
| `virtio_mmio` fixes `c32c67cba4`/`45491eda98` | **GO (routine merge)** | Correct, but MMIO-only; not needed for `-pci` |
| Damage-based flush instead of 50 Hz full blit | **NO-GO for now** | Our own measurement says the payoff is small and latency is floored by QEMU's ~30 ms poll |
| app_server cursor-locking fixes | already ours | No action |

### Goal (c) — unblocking or improving GL for apps (we are mid-build on Mesa)

**NO-GO. Nothing upstream helps, and `virtio_gpu` specifically does not.**

- **VERIFIED:** `build/jam/repositories/HaikuPorts/arm64` contains **no** `mesa`, `llvm`
  or `glu` entries whatsoever, while the x86_64 list pins `mesa-22.0.5-3`,
  `mesa_swpipe-22.0.5-3`, `llvm12-12.0.1-8`, `glu-9.0.0-8`. And
  `build/jam/BuildFeatures:185` gates the whole `mesa` build feature on
  `IsPackageAvailable mesa_devel`. GL for arm64 is a **package-availability** problem, not
  an upstream-source problem. Our LLVM blocker is the whole story.
- **VERIFIED:** `virtio_gpu` never negotiates `VIRTIO_GPU_F_VIRGL` (`virtio_gpu.cpp:480-481`).
  It cannot provide hardware GL. Even a working virtio-gpu display leaves us on
  llvmpipe/lavapipe software rendering.
- **VERIFIED:** upstream Haiku has no Vulkan at all.

The one indirect benefit: once Mesa exists, `GLInfo` and GL apps need *a* display to be
verified on, and a virtio-gpu guest is a better one than `ramfb`. That is goal (b) again,
not goal (c).

---

## 5. Can we do better than `ramfb` + `screendump`?

**Yes, modestly — and the ceiling is lower than it looks.** Three options, honestly ranked:

1. **`virtio-gpu-pci` + QEMU VNC — best for interactive work.** Buys real guest-initiated
   modesetting, an EDID mode list, and (from our own prior measurements) keystroke→
   framebuffer **p50 31 ms / p99 81 ms**, window drag 1.35 Mbit/s, idle 1.7% of one core,
   reconnect 8/8 clean, two simultaneous clients, mode switch to 2048×1152. It does **not**
   remove the need for `screendump`; host-side capture stays the reliable
   non-interactive verification path.
2. **`ramfb` + `screendump` — keep it for automated verification.** It is simple, has a
   calibrated content-inspection check (`graviton/scripts/qemu-screendump` reports distinct
   colour counts rather than trusting an exit code), and has no virtqueue in the path. For
   "did the browser paint?" in CI this remains the right tool. Do not retire it.
3. **`RemoteHWInterface` + the in-tree HTML5 client — keep as the bare-EC2 rescue console.**
   Works today, no arch gating, viewable from a Linux browser. But bitmaps cross **raw and
   uncompressed** through a 16 KB ring, and you **cannot screenshot from inside** a remote
   session — `RemoteHWInterface::FrontBuffer()` returns NULL and
   `RemoteDrawingEngine::ReadBitmap()` waits for the *client* to send pixels back. Capture
   must be client-side or host-side.

**Does recent upstream work strengthen or weaken the prior conclusion that streaming a
real desktop from the Linux host under KVM+virtio-gpu is the sound architecture?**
**It strengthens it, mildly.** `b57d5d2f41` (2026-08-20) is independent confirmation that
virtio-gpu works on arm64 under QEMU, from someone who tested both device models. Nothing
in 2025–2026 upstream weakens it. But note the two frictions that remain unaddressed
upstream: no hardware cursor and no damage tracking (a permanent 50 Hz full-screen blit),
so the "real desktop" is software-composited and costs host bandwidth continuously.

**One documentation correction, VERIFIED.** `graviton/docs/package-chain-status.md:25`
says the netsurf render happened via "`RemoteHWInterface`/ramfb". That is self-contradictory
and wrong: a `ramfb` guest binds `AccelerantHWInterface` with `framebuffer.accelerant`,
and `ScreenManager.cpp:138` only builds a `RemoteHWInterface` when **no** local screen was
acquired (`if (added == 0 && target != NULL)`). `graviton/docs/framebuffer-guest-capture.md`
has it right. Do not cite `package-chain-status.md:25` as evidence about the remote path.

---

## 6. Effort and risk

### GO-1 — PCI virtqueue barriers, then switch one rig to `virtio-gpu-pci`

**This is the recommendation.** Do the barriers *first*; they are the de-risking step for
the rig switch.

**Why it is needed. VERIFIED, and this is the sharpest finding in the review.** There are
two independent virtqueue implementations in the tree, and only one is fixed:

| Transport | Implementation | Barriers? |
|---|---|---|
| `virtio-gpu-pci` | `src/add-ons/kernel/bus_managers/virtio/VirtioQueue.cpp` | **ZERO** |
| `virtio-gpu-device` (MMIO) | `src/add-ons/kernel/busses/virtio/virtio_mmio/VirtioDevice.cpp` | 3 (upstream) |

`git grep -c 'memory_write_barrier\|memory_read_barrier\|memory_full_barrier'` over
`bus_managers/virtio/` returns **no hits at all**, and `virtio_ring.h` contains **zero**
uses of `volatile`. The publish path is:

```
VirtioQueue.cpp:360-367   fRing.avail->ring[available] = index;
                          fRing.avail->idx++;            // no barrier before the doorbell
VirtioQueue.cpp:321-323   UpdateAvailable(insertIndex); NotifyHost();
virtio_pci.cpp:594-595    volatile uint16* notifyAddr = ...; *notifyAddr = queue;
```

On arm64 a store to Device memory is **not** ordered against earlier stores to Normal
Cacheable memory without a `DSB`/`DMB`, so the host can observe the doorbell before the
descriptors. x86's TSO hides this completely — which is exactly the class of bug this
project has been burned by before. The consume path is equally unprotected *and*
non-`volatile`, so the driver's polling loop (`virtio_gpu.cpp:146-147
while (!queue_dequeue(...)) spin(10);`) can have its `fRing.used->idx` load hoisted by the
compiler — a **permanent hang**, not a stale read.

That upstream needed precisely these barriers added to the *other* transport two days ago
(`c32c67cba4`) is strong corroboration that the pattern is required, not theoretical.

**Files:** `src/add-ons/kernel/bus_managers/virtio/VirtioQueue.cpp`,
`src/add-ons/kernel/bus_managers/virtio/virtio_ring.h`, plus a rig script change
(`-device ramfb` → `-device virtio-gpu-pci`) under `graviton/builder/`.

**Needs an upstream merge?** **No.** The barrier fix is ours to write; it is small,
self-contained, and would benefit every virtio device on arm64 (net, block, scsi, input),
not just the GPU. The two `virtio_mmio` commits can ride the next routine merge separately.

**arm64-specific gaps to expect:**
- The 31.6 MiB `B_CONTIGUOUS` allocation (`virtio_gpu.cpp:596-598`) may fail on a
  fragmented or small guest. It fails cleanly and loudly with a `create_area` error.
- If `virtio_gpu_open()` fails there is **no fallback to `ramfb`** (§2d) — app_server gets
  no display. Keep a `ramfb` rig alongside and know that safe-mode fail-safe video
  recovers a wedged guest.
- Input: use `usb-tablet`/`usb-kbd` on the xhci bus, as our rigs already do.
  `virtio-tablet-pci` gave us no input, and `b57d5d2f41` notes the virtio keyboard still
  needs AT-layout mapping.

**Most likely failure mode, INFERRED from the code above:** `virtio_gpu_open()` hangs or
times out during the `GET_DISPLAY_INFO` / `CREATE_2D` / `ATTACH_BACKING` / `SET_SCANOUT`
handshake, **intermittently and boot-to-boot flaky** — the signature of a missing barrier.

**Risk: LOW-to-MODERATE, and fully contained.** No bare-EC2 AMI is affected: no EC2 image
has a virtio GPU, so the rig change cannot regress the canonical image. Barrier additions
are conservative by construction. Keep one `ramfb` rig as the control.

### How we would verify it — by capability, not exit code

A `screendump` that returns cleanly is not evidence of a picture, and a successful boot is
not evidence the GPU bound. Each of these is an artifact that announces itself:

1. **The driver bound, not merely shipped:** `/dev/graphics/virtio/0` exists in the guest.
   Absence of `/dev/graphics/framebuffer` being *used* is not enough — both nodes can
   exist.
2. **app_server chose it:** `listimage <app_server team> | grep accelerant` names
   `virtio_gpu.accelerant`, not `framebuffer.accelerant`. This is the discriminating test,
   because §2d says virtio_gpu should win automatically — if it does not, `open()` failed.
3. **The capability `ramfb` cannot fake:** `B_SET_DISPLAY_MODE` actually changes the mode.
   Set a mode **different from the current one** from *inside* the guest, then confirm from
   the *host* that QEMU's surface geometry changed. `ramfb`'s accelerant returns `B_OK` for
   a no-op same-mode set but `B_UNSUPPORTED` for any real change
   (`framebuffer/mode.cpp:103`) — so it must be a genuine change, and a host-observed
   geometry change is then proof positive that virtio_gpu is driving the display.
   **This is the single best test.**
4. **Pixels, inspected not assumed:** `graviton/scripts/qemu-screendump` and read its
   distinct-colour-count verdict, exactly as with `ramfb`.
5. **Barriers didn't just "not break it":** the failure they prevent is intermittent, so a
   single clean boot proves nothing. Repeat the boot-and-bind cycle enough times to have
   power against a flaky failure, and record the count. Repeatability is not validity, but
   a single trial is not even repeatability.

### Lower-priority GOs

- **`virtio_gpu` `open_count` guard** — `virtio_gpu.cpp:556`/`:651`. Small. Verify by
  opening the device twice and showing the first close does not break the second client.
- **Accelerant clone ioctl** — add `VIRTIO_GPU_GET_DEVICE_NAME` to the driver's switch
  (`virtio_gpu.cpp:717-746`) or stop issuing it (`accelerant.cpp:124`). Trivial.
- **Upstream our two app_server bug fixes as reports** — `3389f22b2c` (null owner deref)
  and the `execl` fix. Note the project's standing position on upstreaming; these are
  bug *reports*, cheap either way.

### Explicit NO-GOs

- **Damage-based flush** for virtio_gpu — our own measurement says small payoff, latency
  floored by QEMU's poll interval. Revisit only if host bandwidth becomes the constraint.
- **Chasing Mesa via upstream Haiku** — wrong tree entirely (§4c).
- **A GPU instance size** to host this — our own note prices `g5g.metal` at **+18%** over
  `c7g.metal` and concludes it is worth it only for many concurrent streams or 4K, never as
  an enabler.
- **Multi-monitor / HiDPI / Vulkan** — do not exist, or do not apply.

---

## What I could not verify

Stated plainly, so nothing here is mistaken for a measurement:

- **I did not build anything and did not boot a virtio-gpu guest.** Every claim about
  virtio_gpu's *runtime* behaviour on arm64 is code reading plus upstream's own report in
  `b57d5d2f41`, not our own observation. The prior "proven under virtio-gpu-pci" record in
  `graviton/docs/sequencing.md:223` is earlier work I did not re-run — and the builder
  measurement shows nothing currently uses it.
- **The ~415 MB/s blit figure is arithmetic**, not a measured host-bandwidth number.
- **The barrier hazard is a code-inspection finding.** It is well-corroborated by
  upstream's parallel MMIO fix, but we have not observed a hang attributable to it.
- **Framebuffer cache coherency:** the guest framebuffer is ordinary cached memory with no
  cache maintenance before `TRANSFER_TO_HOST_2D`. I *infer* this is safe under arm64 KVM
  because host and guest mappings share a coherency domain, but I found no code that makes
  it safe by construction. It would be a real hazard under emulation or a non-coherent
  setup.
- I relied on local git history throughout and used no secondary sources; the only
  external voice quoted is an upstream commit message, cited by SHA.
