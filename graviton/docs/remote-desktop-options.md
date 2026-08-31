# Remote desktop for Haiku on Graviton: the options

Status: design doc / decision record.

## The problem, and the one constraint that shapes every answer

A Graviton EC2 instance has **no display device** — Haiku's EFI loader gets
`GOP protocol not found` on c7g (measured on our own instances), so the kernel boots with
`frame_buffer.enabled = false` and `app_server` has nothing to paint on locally.
Its fallback is `RemoteHWInterface`: it forwards a **display list** (vector drawing orders —
`RP_FILL_RECT`, `RP_DRAW_STRING`, `RP_DRAW_BITMAP`, …) to a remote client that renders.
`RemoteHWInterface::FrontBuffer()` returns `NULL` (`RemoteHWInterface.cpp:590-594`); the pixels
only ever exist on the client.

Two hard facts settle the architecture:

- **The pixels of the *native* Haiku desktop only ever exist inside `app_server`.** Haiku
  graphics/input flow through `app_server`/`input_server` over BeAPI `BMessage`/port IPC — not
  through any POSIX device. There is no `/dev/fb`, no evdev, no `/dev/uinput`, no DRM. So you
  cannot capture the desktop at the POSIX layer, and every "compat layer" (XLibe,
  Wayland-on-Haiku) bridges the *wrong* direction (foreign app → app_server), never
  app_server → wire. The only way to get the composited desktop out as pixels is an
  `app_server` `HWInterface`.
- **Haiku is not Linux-ABI-compatible** (its own libroot, its own `runtime_loader`, no
  linuxulator). Linux binaries cannot run on it, so any Linux-only remote-desktop server can
  only ever be a *Linux sidecar*, never native.

This splits the space in two:

- **Command-streaming** (Haiku's native remote interface) works as-is.
- **Pixel** protocols (VNC, video, RustDesk, a Linux sidecar) need a captureable framebuffer.
  On Haiku that means an **offscreen `HWInterface`** rendering the display list into a real
  in-memory `BBitmap`. This turns out to be cheap — see Option B.

## A proven reference model (GPU-less CPU encode)

AWS's remote-display product (NICE DCV) is publicly documented to run headless on GPU-less
instances by publishing an in-memory virtual framebuffer and encoding it **entirely on CPU**
— on Graviton that is NEON-accelerated `libx264` (`preset ultrafast`), with an adaptive
per-region tiler (a fast near-lossless path plus JPEG, falling back under congestion, and
`libx264` for motion), codecs negotiated per connection, capture shared across clients and
encoders per-client, over QUIC/UDP (native client) or WebSocket/TCP (HTML5 client). It is free
for your own use on EC2. That published design — **offscreen software framebuffer + per-region
NEON `libx264`/JPEG/fast-path + input injected back** — is the blueprint to *copy*, and it
demonstrates the model works interactively on GPU-less Graviton. It is not a component we can
reuse natively (it is Linux-only), and there is no known prior art for a non-Linux hobby OS on
EC2 — this is greenfield.

## The options

### Option A — Fix the native display-list protocol (no framebuffer needed) — DO FIRST

Auditing `src/servers/app/drawing/interface/remote/` shows the lag is **not bandwidth — it's
synchronous round-trips**, and the fixes are small:

| Fix | Evidence | Effort |
|---|---|---|
| **Make `DrawString` fire-and-forget** — it blocks a full RTT on `RP_DRAW_STRING_RESULT` for pen-advance on *every* text draw. The server already has the `ServerFont` and computes metrics locally in `StringWidth`'s fallback (`:975`) — drop the round-trip. | `RemoteDrawingEngine.cpp:899`, `:963` | Med |
| **Set `TCP_NODELAY`** — never set; Nagle + delayed-ACK adds ~40–200 ms to each round-trip. One `setsockopt`. | `NetSender.cpp:83`, `RemoteHWInterface.cpp:71-88` | **Low, huge** |
| **Compress/cache bitmaps** — `RP_DRAW_BITMAP` ships raw `4·N·M` bytes (a 256×256 icon = 262 KB) every frame; there's a `// TODO: cache/checksum` at `:383`. Route through NEON libjpeg-turbo / add a checksum cache. | `RemoteMessage.cpp:91-111` | Med |
| **Add LZ4 stream compression** + grow the 16 KiB send ring buffer. | `NetSender.cpp:71-94`, `RemoteHWInterface.cpp:97` | Low–Med |

- **Prerequisite:** none. Improves the code the remote client already wraps.
- **Risk:** low. **Payoff:** removes most felt lag; text stays vector-crisp. The `TCP_NODELAY`
  + `DrawString` fixes alone likely resolve the "laggy" complaint.

### Option B — Offscreen framebuffer (the shared unlock for all pixel routes) — ~3–5 days

The load-bearing prerequisite for VNC / video / RustDesk / a Linux sidecar turns out to be
nearly free: **Haiku already ships `BitmapHWInterface`**
(`src/servers/app/drawing/BitmapHWInterface.cpp`) — a software HWInterface that renders into a
real malloc-backed `ServerBitmap`, implements every pure virtual, and is proven in production
for offscreen windows/layers. Pixel readout already exists in
`BitmapDrawingEngine::ExportToBitmap` (`:82-101`).

Remaining work (~3–5 person-days): (1) give it desktop-screen mode-management (copy ~40 lines
from `RemoteHWInterface.cpp:405-464`); (2) register it in `ScreenManager` (`:138-218`) via an
env/target trigger; (3) expose a lock → `CopyBackToFront` → read `FrontBuffer()->Bits()`
capture hook. Format is `B_RGBA32`, always contiguous. The load-bearing risk is already retired.

### Option C — VNC / RFB (the pragmatic pixel v1) — ~1–2 weeks on top of B — RECOMMENDED PIXEL ROUTE

- Port `libvncserver` (zlib/libjpeg-turbo/libpng/openssl all build on Haiku/arm64; BSD sockets +
  `rfbProcessEvents` single-threaded, no DRM/X11 dep).
- Integration is ~1 file: point `rfbScreenInfoPtr->frameBuffer` at the `BitmapHWInterface`
  surface, one `rfbMarkRectAsModified` per dirty rect, two input callbacks (keysym→Haiku keycode
  is the fiddly bit).
- Tight encoding uses **JPEG via NEON libjpeg-turbo**. macOS Screen Sharing, RealVNC, TigerVNC
  connect to a stock server (VNC-password auth; tunnel over SSH, or enable libvncserver's TLS).
  GPLv2+.
- **Best for:** instant broad native-client reach with minimal work. Weak for full-motion video;
  higher-latency than WebRTC.

### Option D — Native video pipeline (WebCodecs + WebTransport) — full-motion track

On the offscreen surface, feed dirty regions to **`libx264` linked directly** (you do **not**
need ffmpeg — it isn't built for arm64 in the tree yet, and libx264 is fully NEON: 433 NEON
symbols). Cost: **~0.3–0.7 of one Graviton core** for 1080p30 ultrafast+zerolatency.

- **Transport: WebCodecs + WebTransport (QUIC)** — lowest-latency *and* simplest for a raw
  elementary stream (push NAL units, no RTP/SDP). WebRTC is the batteries-included fallback; MSE
  rejected. Input rides WebTransport datagrams, never WebSocket. Latency budget: **~30–60 ms
  LAN, ~60–120 ms WAN** — encode is not the bottleneck.
- **Best for:** full-motion media/animation; the true home for Graviton NEON encode.
- **Build caveat:** do **not** pass `-mcpu=neoverse-*` — GCC then emits SVE, which traps on
  Haiku. Use `-mtune` only.

### Option E — RustDesk — v2-only

Rust is ported, so it builds in principle, but the surface shim is the *small* part: RustDesk
hard-codes five OSes, so you patch `cfg(haiku)` pervasively across
`scrap`/`enigo`/`platform`/`server`, port libvpx/aom/libyuv/opus, and stand up + operate an
`hbbs`/`hbbr` rendezvous/relay host. **~8–16+ person-weeks.** AGPL-3.0. Revisit only if
WAN/NAT-traversal + AV1 become hard needs.

### Option F — Linux sidecar (north star, one hard blocker)

A Linux/X11 sidecar could stream *any* fullscreen X window with a mature remote-desktop stack.
**The single blocker: nothing produces Haiku pixels on the Linux box.** You'd need a Linux/X11
renderer for Haiku's display-list protocol rendering fullscreen into a virtual X server — and
only an old, unmaintained Qt prototype appears to exist. **Reviving that Qt-on-X11 client is the
critical path.** Two hops (Haiku→sidecar display-list, then sidecar encode) add only a LAN RTT
if co-located; the encode already dominates. Best kept as a research spike.

## Decision matrix

| Option | Needs offscreen FB? | Effort | Motion video | Text crispness | Client reach |
|---|---|---|---|---|---|
| A — fix native protocol | No | Days | poor | excellent | the in-tree remote client |
| B — offscreen HWInterface | (is the FB) | ~3–5 days | — | — | (enabler only) |
| C — VNC (libvncserver) | Yes (B) | ~1–2 wk | fair | good | excellent (Apple/Real/Tiger) |
| D — libx264 + WebTransport | Yes (B) | Med–High | excellent | good (hybrid) | browser |
| E — RustDesk | Yes (B) | 8–16+ wk | excellent | good | RustDesk app only |
| F — Linux sidecar | Sidecar's FB | High (Qt client) | excellent | excellent | best-in-class |

## Recommendation — a phased path

1. **Phase 0 (days): Option A.** `TCP_NODELAY` + local `DrawString` metrics + bitmap
   cache/compress. Directly fixes the felt lag; no new architecture.
2. **Phase 1 (~1 week): Option B.** Wire `BitmapHWInterface` into a desktop screen with a
   capture hook. The one piece of load-bearing new code; unlocks everything.
3. **Phase 2 (~1–2 weeks): Option C (VNC).** Cheapest pixel v1, lights up NEON turbojpeg,
   instant macOS Screen Sharing / RealVNC / TigerVNC support.
4. **Phase 3 (optional): Option D** for full-motion (libx264 + WebCodecs/WebTransport browser
   client), and/or **Option F** (Linux sidecar) as the north-star spike once the Qt renderer is
   revived.
5. **Deprioritize Option E (RustDesk)** unless WAN/NAT + AV1 become requirements.

The through-line: Option A buys a big win in days with no new architecture; the offscreen
`BitmapHWInterface` surface is the single unavoidable new piece (~week); after that, VNC → video
→ sidecar are all *front-end choices* on the same surface, and the CPU-NEON-`libx264` model is
already proven to work GPU-less at interactive latency.
