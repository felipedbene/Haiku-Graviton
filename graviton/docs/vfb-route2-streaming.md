# Route 2: the virtual-framebuffer guest-on-metal streamed remote desktop

Status: design of record for GitHub #118. Scopes how the existing
framebuffer-capture *rig* becomes a *streamed remote desktop*, and names the one
unknown that must be settled before the rest is worth building.

This is the **Route 2** companion to the offscreen-`BitmapHWInterface` plan in
`remote-desktop-options.md` (Route 1, #95). The two coexist and target different
deployments; see "Route 1 vs Route 2" below.

## The one-sentence idea

Run Haiku as a KVM guest on a Graviton `.metal` host, give the guest a *synthetic
display* the hypervisor owns, and let the **Linux host** capture, encode and serve
that display — so `app_server` runs its ordinary local-framebuffer path with
**no Haiku code change**, and every hard problem (capture, encode, transport,
reconnect) lives on the host where mature tools already solve it.

## What is already proven (do not re-litigate)

Three results are load-bearing and already measured on our own hardware. Route 2
builds on them; it does not need to re-establish them.

1. **The desktop paints on a synthetic display, with no Haiku change.** A Haiku
   arm64 guest booted under QEMU/KVM on a Graviton `.metal` host with `-device
   ramfb` gets a GOP from UEFI, the kernel publishes `/dev/graphics/framebuffer`,
   and `app_server` binds it via `AccelerantHWInterface` + `framebuffer.accelerant`
   — a *local* interface, not `RemoteHWInterface`. `-device virtio-gpu-pci` reaches
   the same result via `graphics/virtio/0` + `virtio_gpu.accelerant`, additionally
   negotiating EDID and MSI-X. Rig and calibrated capture evidence:
   `framebuffer-guest-capture.md` (bare desktop 1482 colours / 1.56 % non-modal;
   +one window 2383 / 34.5 %). Idle host cost: ramfb ~0.5 %, virtio-gpu ~1.6 % of
   one core.

2. **QEMU's own VNC already streams that display interactively.** Driving the
   desktop over QEMU's built-in VNC with a hand-written RFB client: menus track on
   hover, apps launch, typed text reads back off the screen (verified by pixel
   diffs, not impressions). Keystroke-to-framebuffer over VNC: **p50 31 ms, p95/p99
   81 ms, 0/200 over 100 ms** — the floor is QEMU's 30 ms VNC poll cadence, not
   Haiku. Idle full frame **9.8 kB** ZRLE at 1280×800 (418:1 vs raw); a 7 s window
   drag was **169 kB/s**. Reconnect 8/8 clean (the host holds real pixels), two
   simultaneous clients both stayed live. This is the Stage-1 transport, and it
   needs **zero new components**.

3. **Input works — but only one way.** `-device usb-tablet -device usb-kbd` on a
   `qemu-xhci` bus works immediately: `usb_hid` loads, `/dev/input` shows
   `keyboard tablet`, absolute positioning is exact. **`virtio-tablet-pci` /
   `virtio-keyboard-pci` produce no input at all** (`virtio_input` never binds — do
   not use them). The QEMU monitor's `mouse_move` also does **not** move the Haiku
   cursor (it queues *relative* motion; the guest has an absolute tablet). The rig
   (`boot-fb-guest.sh`) already wires usb-tablet + usb-kbd.

The contrast that motivates Route 2: the *native* display-list path
(`RemoteHWInterface` → HTML5 client) black-screens on reconnect, stalls a full RTT
per drawn string, has no server-side framebuffer, and silently steals sessions on
a second connect. Route 2 sidesteps all of it by never using that interface.

## What is NOT yet proven — the single blocking unknown

Everything above was measured on **bootstrap-flavoured** guest images, on which
third-party deps are `_bootstrap`, `libmedia`/`libgame`/`libscreensaver` are
absent, and — decisively — **guest networking is dead** (`net_server` dies at
boot). Stage 1's transport reached the guest over QEMU's `hostfwd` user-net for
SSH only; a bootstrap guest cannot be a real session.

> **Blocking unknown (resolve first):** does the **full (non-bootstrap) canonical
> image**, booted as a KVM guest with a framebuffer (ramfb or virtio-gpu) **and**
> usb input on a `c7g.metal`, come up with (a) a painting desktop, (b) live guest
> **networking**, and (c) working **input** — all three at once — so the session is
> a usable desktop and not merely a screendump target?

This is the gate. Until it is green on a full image, the encode/transport/economics
milestones below are premature. It is cheap to answer (one metal, one full image,
one boot) and it is the honest next step, distinct from the paint proof we already
hold. The rig needs one image change to try it: our builder images ship a launch
override declaring `x-vnd.Haiku-app_server`, `x-vnd.Be-TRAK`, `x-vnd.Be-TSKB`
`disabled` (correct for a headless builder, exactly wrong for a framebuffer guest);
remove it, `sync`, restart (see `framebuffer-guest-capture.md`). The canonical image
is not a builder image, so confirm its override state before booting.

## Components

| # | Component | Stage-1 (proven pieces) | Stage-2 (full-motion) |
|---|---|---|---|
| 1 | **Guest display** | `ramfb` → EFI GOP → `framebuffer.accelerant`. Zero Haiku change, lowest host cost. | `virtio-gpu-pci` — exposes damage/EDID, but needs three guest fixes (below). |
| 2 | **Host capture** | QEMU owns the surface; its VNC server does damage tracking. Nothing runs in the guest. | Fixed-cadence capture of the QEMU display into a headless compositor. |
| 3 | **Encode** | ZRLE inside QEMU's VNC (already measured, 21–418× vs raw). | NEON `libx264` (`preset ultrafast,zerolatency`) — ~0.3–0.7 core for 1080p30; H.264/HEVC via Sunshine, or per-region NEON turbojpeg. |
| 4 | **Transport** | QEMU `-vnc unix:<sock>`, carried out over the SSH-over-SSM tunnel. | Sunshine→Moonlight (H.264 + audio + adaptive) **or** a WebRTC/WebTransport gateway for browser-only. |
| 5 | **Input** | `usb-tablet` + `usb-kbd` on `qemu-xhci` (proven). VNC input events drive them. | Same devices; the encoder front-end forwards pointer/key. |
| 6 | **Client** | Any stock VNC client (macOS Screen Sharing, RealVNC, TigerVNC) over the tunnel. | Moonlight (native) or a browser (WebRTC). |

### virtio-gpu is only needed for Stage 2, and needs three guest fixes first

`ramfb` is enough for Stage 1 and is the cheaper, zero-change path. Prefer it until
Stage 2 needs damage information. `virtio_gpu.cpp` (verified) is 2D-only (no
virgl), `B8G8R8X8`/32bpp, single hardcoded scanout, software cursor, and does an
**unconditional full-screen transfer+flush at 50 Hz** even though `app_server`
already computed the damage. Three contained guest fixes make it Stage-2-ready:

1. **`open_count` guard** on `virtio_gpu_open` (currently missing) — two opens
   spawn two 50 Hz threads and leak the 31.6 MiB contiguous area. ~10 lines,
   mirror `framebuffer/device.cpp`.
2. **Host-driven resize** — the config-change handler is NULL and
   `VIRTIO_GPU_EVENT_DISPLAY` is never read, so the host cannot resize the guest.
3. **Damage-based flush** — flush only the rects `app_server` marked dirty instead
   of the whole screen every 20 ms. Lowest priority: Stage-1 latency is floored by
   QEMU's 30 ms poll, so the measured payoff is small and it carries the highest
   regression risk (the unconditional flush is part of why reconnect is reliable).

These are the *only* Haiku code changes Route 2 ever needs, they are optional, and
they land in the guest driver — never in `app_server`.

## Milestones

- **M0 — settle the blocking unknown.** Full canonical image, KVM guest on
  `c7g.metal`, ramfb + usb input. Confirm paint **and** networking **and** input
  together. Instruments: `screenmode` (mode), `listimage <app_server>` grep for
  `framebuffer.accelerant` (the decisive interface check — a `RemoteHWInterface`
  maps no accelerant), a `qemu-screendump` verdict of `content`, and a guest DHCP
  lease / reachable `sshd`. Gate for everything below.
- **M1 — package Stage-1 streaming.** Promote QEMU VNC from the rig's "unused
  fallback" to a first-class path: bind `-vnc` to a unix socket, carry it over the
  SSH-over-SSM tunnel, connect a stock VNC client. Deliverable: a host-side
  launcher (`stream-fb-guest.sh`, this PR) + tunnel recipe. No new code in Haiku.
- **M2 — session hardening.** One metal hosting one long-lived desktop guest:
  overlay-qcow2 lifecycle, `sync`-before-restart discipline (`shutdown -r` hangs the
  guest — documented), reconnect behaviour on a full image, clipboard/audio gaps
  catalogued.
- **M3 — virtio-gpu Stage-2 readiness (optional).** Land the three guest fixes
  above; switch component 1 to `virtio-gpu-pci`; A/B host cost and latency vs ramfb.
- **M4 — full-motion encode (optional).** QEMU display into a headless compositor,
  Sunshine→Moonlight (NEON `libx264`) or a WebRTC gateway. Target ~45–60 ms
  glass-to-glass nearby.
- **M5 — economics.** One metal amortised across many guests; a small session
  broker (which guest, which port, which client). Bare KVM needs `.metal` ($2.32/hr
  on c7g.metal), so per-desktop cost only pencils out shared.

## Route 1 (#95) vs Route 2 (#118)

| | Route 1 (#95) | Route 2 (#118) |
|---|---|---|
| Reaches | a **bare Graviton instance** (no metal) | a **`.metal` + KVM** topology |
| Haiku change | yes — offscreen `BitmapHWInterface` in `app_server` | **none** for Stage 1 (optional guest-driver fixes for Stage 2) |
| Paint proven? | not yet (new server code) | **yes** (this rig) |
| Capture | in-guest, off a `BBitmap` surface | host-side, off the QEMU display |
| Best when | you must run on a plain instance | you already run a metal (builders do) |

They are complementary: Route 1 serves the desktop *from the instance itself*;
Route 2 serves it *from the metal host* and is already proven to paint. Pursue
Route 2 to a usable Stage-1 first (it is closest to done), keep Route 1 for the
bare-instance case.

## Non-goals

Multi-monitor (blocked in `app_server` itself — `VirtualScreen.cpp` `RemoveScreen`
returns `B_ERROR` — *and* virtio-gpu hardcodes scanout 0); any GPU/3D/virgl (no
hardware-accelerated OpenGL on arm64 — the GL/GLES/EGL ABI exists
(`libglvnd_devel` provides `devel:libgl`/`libegl`/`libglesv2`), but there is no
GPU or accelerated driver, so it is software GL only; no DRM/KMS, `app_server` is
pure-CPU AGG); anything on `g5g`
(Graviton2 cores, worse for the builder role; NVENC saves noise against the budget).
See `remote-desktop-options.md` and `haiku-graphics-upstream-review.md`.
