# URP/1 — the unified DeBeOS remote-desktop protocol (design of record for #118)

Status: design of record. Supersedes the two competing proposals ("evolve `RP_`"
and "clean-sheet codec stream") by merging them, and consolidates the earlier
protocol review, `remote-desktop-options.md` (#95, Route 1), and
`vfb-route2-streaming.md` (#118, Route 2) into one target.

This document decides **what the protocol is**, how **one** protocol serves all
three clients, where each Graviton performance lever plugs into the pipeline, the
latency budget, which defects it closes, the limits it does not pretend to solve,
and a milestone plan from the smallest working slice to full-motion.

Publication note: the flagship client is a C++ cross-platform remote client that
lives in a **separate repository**; it is referred to that way throughout and its
identity is deliberately omitted. Everything stated as measured was measured on
our own rented hardware.

---

## 0. The verdict, in four lines

1. **Evolve, do not replace.** The one protocol is **URP/1** — `RP_` with a real
   handshake, an explicit wire spec, and a second per-region pixel/codec tier
   bolted on. It is a hybrid *inside one protocol*, not two protocols.
2. **The vector display list is the win, not the legacy.** 40 characters of
   crisp, resolution-independent text for **81 bytes** is something no codec
   approaches; a static desktop costs a codec a refresh forever and costs the
   display list nothing. Keep it as the default tier.
3. **Pixels are the exception the display list cannot serve** — video, GL,
   scrolling photo/browser content. Those get a per-region encoded tier
   (Tier P), multiplexed over the same connection, composited on one surface.
4. **No app_server rewrite, no flag day.** All of this lives behind a third
   `HWInterface`; every wire change after the handshake is capability-gated, so
   old clients keep working byte-for-byte and the draw-op path is always the
   fallback.

---

## 1. Why evolve rather than clean-sheet

Three facts settle it.

- **The app-facing boundary names no framebuffer.** Applications reach
  `app_server` by `BMessage`-over-ports (`ServerWindow.cpp:273-279`, `:598`);
  nothing on that path names a `RenderingBuffer`, colour space or resolution. The
  living proof is `RemoteHWInterface`, whose `FrontBuffer()` returns `NULL`
  (`RemoteHWInterface.cpp:590-594`) while unmodified BeAPI apps run against it.
  So *any* display architecture we want is reachable by adding an `HWInterface`
  at the single runtime selection site (`ScreenManager.cpp:120-158`, whose own
  TODO invites a third back end). A new app server is the wrong unit of work; a
  clean-sheet protocol that discards the draw-op path throws away the one thing
  the boundary makes free.

- **The flagship client already *is* the reference wire implementation.** The
  C++ cross-platform remote client (separate repo) speaks the `RP_` vector
  display list: a fixed 6-byte header (`op:u16` + `total_length:u32`, both
  little-endian, length includes the header), packed little-endian bodies, IEEE-754
  bit-cast floats, a `Framer` that rejects `< 6` and `> 64 MiB` and caps the
  pending buffer at 64 MiB, and a `Writer` that back-patches the length at flush.
  It decodes the full composite type set (`Point`, `Rect` with Haiku inclusive
  edges, `Color`, the 17-byte `Font`, `Transform`, `Gradient`, `region`,
  length-prefixed `string`) and carries client-side tessellation. A clean-sheet
  codec protocol orphans that client; evolving `RP_` promotes its `Reader`/`Writer`
  discipline to the **spec of record** and keeps the client as the conformance
  oracle.

- **The measured economics favour the display list for what a desktop mostly
  is.** The pixel case is real but narrow (`§7`), and the draw-op path is
  catastrophically cheaper everywhere else. A protocol that leads with pixels
  pays a permanent tax to fix an occasional problem.

**The clean-sheet proposal was still right about four things**, and URP/1 adopts
all of them: an explicit, spec-stated, fixed-width, little-endian wire format
(not an inherited host ABI); two logical channels (reliable control vs
lossy-tolerant media); presentation timestamps on encoded content; and real
capability negotiation with a version on the wire. It was wrong only about
throwing the display list away to get them.

---

## 2. The wire format — an explicit protocol, not a host ABI

Today's format is an **ABI**: fields are `memcpy` of host-native types with no
byte-swap and no stated width (`RemoteMessage.h:266-276`); `sizeof(enum) == 4`,
struct padding, float representation and little-endian order are all implicit,
and the receiver hardcodes little-endian by luck, not by contract. We are
LE-on-LE today so it is latent, not broken — but this project has already been
burned by a real arm64 float byte-swap defect (`__swap_float`/`__swap_double`
returning the loop offset), which is exactly the bug class an implicit-ABI wire
format cannot be tested against.

URP/1 fixes this by **specification, promoting the C++ client's discipline**:

- **Framing is unchanged and canonical.** 6-byte header (`op:u16`,
  `total_length:u32`, LE, length includes header, back-patched at flush); keep the
  `< 6` / `> 64 MiB` / 64 MiB-pending guards. This is the one part of every stack
  that is already sound and identical across the C++ client, the HTML5 client and
  `rdcapture.py`. Framing stays; only its *guarantees* become normative.
- **Every field is explicit-width, little-endian by declaration, unpadded.**
  `Point{f32,f32}`, `Rect{f32×4}` (inclusive edges; pixel width is
  `IntegerWidth()+1` — stated normatively to end the off-by-one), `Color{u8×4}`,
  `pattern` = 8 raw bytes, enums encoded as explicit `u16`/`u32` (never "assumed
  4"). The HTML5 client's hardcoded `StreamingDataView(buffer, true)` becomes
  *correct by spec* instead of *correct by luck*.
- **The version stamp becomes real.** `fProtocolVersion` (`RemoteHWInterface.cpp:47`)
  is a dead field today; `§3` puts it on the wire.
- **A conformance test freezes the byte layout of one message of each shape.** It
  costs almost nothing and, given the arm64 byte-swap history, byte-order here
  deserves a test rather than trust. The C++ client is the oracle it runs against.

---

## 3. Negotiation — the permission slip for everything after M0

There is no version exchange, no capabilities, no auth in band, and the client
dictates resolution blind (`RemoteHWInterface.cpp:272-311`). URP/1 adds a session
block sent immediately after connect, before any draw op:

- **`RP_HELLO` (client→server):** `proto_version:u32`, a feature bitmap
  (`TIER_P`, `JPEG`/`X264`/`X265`/`AV1`/`OPUS`, `STRING_WIDTH_REPLY`,
  `BITMAP_CACHE` + a client-declared cache byte budget, `RESYNC`,
  `FRAME_BOUNDARY`), the client's max decode dimension, and its requested
  `width,height` (folding in today's `update_display_mode`).
- **`RP_HELLO_ACK` (server→client):** negotiated `proto_version` (min of both),
  the server-capability intersection, the selected codec preference order, and a
  push of **font metric tables** (see D1) so the client answers `string_width`
  locally.
- **`RP_GOODBYE`:** clean teardown that flushes shadow state.

**Compatibility rule.** Because `length` is explicit and both non-native clients
already skip unknown opcodes, old peers ignore new fields and new peers detect old
peers by their absence — the fallback is exactly today's behaviour. The server
**may only use a feature the client advertised**. Unknown *drawing* ops may be
skipped only when marked non-critical; an unknown *state* op or an unknown Tier P
codec is a hard renegotiate, never a silent skip. ("Skip unknown" is a framing
property, not a compatibility strategy — negotiation is.)

This is ~50 additive lines and it is the difference between "we can add a cache
and a codec later" and "we cannot add anything without a flag day". The
`RP_STROKE_*_GRADIENT = 260` block that the HTML5 client's opcode table
(stopping at 244) has never heard of is the version-skew accident already sitting
in the tree, waiting; `RP_HELLO` is what stops it recurring.

---

## 4. Message model — two tiers over one connection, two logical channels

URP/1 carries two content **tiers** multiplexed by opcode range, over two logical
**channels**. Tiers say *what the content is*; channels say *what delivery
guarantee it needs*.

### 4.1 Tiers

**Tier V — vector / command stream (the `RP_` heritage, existing opcodes).**
The full existing taxonomy is kept unchanged: session, state lifecycle, draw-state
setters, clip/copy, stroke/fill/gradient geometry, fast colour primitives, text,
cursor, input. One `RemoteDrawingEngine` == one state token; server-side shadow
`DrawState` + `BRegion`; setter dedup. This tier owns text, UI chrome and static
vector content, and it is where the 81-byte win lives.

**Tier P — pixel / codec (new opcode block, negotiated).** Per-region encoded
pixels for regions the server classifies as high-churn (video, GL/SwiftShader
output, scrolling photo/browser content). New ops carry a **presentation
timestamp** (u64 µs, server monotonic, epoch = `RP_HELLO`):

| op | name | payload |
|----|------|---------|
| `RP_TIER_BEGIN_FRAME` | frame open | `frame_seq:u32`, `damage:region` |
| `RP_CODEC_TILE` | one encoded rect | `rect`, `codec:u16`, `flags:u16` (keyframe/delta), `pts:u64`, `bytes:string` |
| `RP_TIER_END_FRAME` | frame close | `frame_seq:u32` |
| `RP_AUDIO_PACKET` | audio | `codec:u16` (Opus), `pts:u64`, `bytes:string` |
| `RP_FRAME_ACK` | client→server pacing | `frame_seq:u32`, `decode_ms:u16`, `queue_depth:u8` |

Codec ids: `0` raw BGRA, `1` JPEG (still/near-lossless), `2` x264, `3` x265,
`4` AV1, plus Opus for audio. Codec is negotiated and may differ **per tile** —
the proven game-streaming/DCV pattern: JPEG or near-lossless for static tiles,
`x264 --tune zerolatency --intra-refresh` only on moving tiles.

**Damage router (server-side).** For each damage rect the back end keeps a churn
score (updates/s × area). Below threshold → Tier V (crisp text, cheap idle).
Above threshold → cut the region out of the Tier V clip and emit Tier P
`RP_CODEC_TILE`s. A region migrates tier frame-to-frame; the boundary is a clip
subtraction, so the two tiers composite on one surface without overdraw. This
generalises the existing token-less two-drawing-states-over-one-surface trick
(`RP_COPY_RECT_NO_CLIPPING` + `RP_FILL_REGION_COLOR_NO_CLIPPING`) into a
first-class composition.

### 4.2 Channels and transport

- **Control channel — reliable, ordered:** handshake, capability negotiation,
  resize, input, force-keyframe, clipboard, resync, and every Tier V op. A missed
  control op corrupts session state (the classic "skip a state op" hazard), so it
  is never lossy.
- **Media channel — timestamped, lossy-tolerant:** Tier P codec tiles, cursor,
  audio. Frames may be dropped under congestion; the decoder recovers at the next
  keyframe or delta-refresh.

Transport is **pluggable, spec-stable across all of them**:

| transport | control | media | when |
|---|---|---|---|
| **tuned TCP + `TCP_NODELAY`** (loopback, over SSH-over-SSM) | one socket, framed | same socket (HOL blocking accepted) | M0–M3, the path we ship today |
| **QUIC** | reliable bidi stream | `DATAGRAM` unreliable stream | native full-motion |
| **WebRTC** | ordered reliable data channel | `ordered:false,maxRetransmits:0` data channel or a real video track | browser full-motion |

Security stays SSH-as-the-model on the tuned-TCP path (loopback bind, no in-band
auth) exactly as today; QUIC/WebRTC bring their own.

---

## 5. One protocol, three clients

| client | role | Tier V | Tier P | what it gains |
|---|---|---|---|---|
| **C++ cross-platform remote client (separate repo)** | flagship + **conformance oracle** for the wire spec | full | opt-in (needs a video decoder) | client-side font metrics (D1), resync (D4/D5-series), the negotiated cache |
| **HTML5 client** (`src/tools/html5_remote_desktop/`, the one the launcher drives) | zero-install browser path | full | via WebCodecs/WebRTC | its `StreamingDataView(...,true)` becomes correct-by-spec; already answers `string_width` |
| **native in-tree client** (`src/apps/remotedesktop/`) | pixel-accurate but currently broken | full | declares none; stays vector-only | the conformance suite + negotiation is how D9/D10 stop biting |

The negotiation rule (`§3`) is what makes one protocol serve all three without a
flag day: each advertises only what it implements, the server uses only what was
advertised, and a client that advertises no Tier P gets a pure, crisp,
resolution-independent vector desktop — which is the correct product for most
sessions anyway. `rdcapture.py` (`graviton/scripts/rdcapture.py`, ours) stays the
test instrument: it already renders the op stream into a software framebuffer,
answers `RP_DRAW_STRING`/`RP_STRING_WIDTH`/`RP_READ_BITMAP`, and carries a
38-assertion selftest that guards the oracle.

---

## 6. Where each Graviton lever plugs in: capture → encode → transport → decode → present

The pipeline only exists for Tier P; Tier V has no capture/encode stage (the
client rasterises). Levers below are ours, measured here unless noted.

### Capture (Tier P only)
- **Route 2 (#118), host-side, fastest to stand up:** Haiku as a KVM guest on a
  `.metal` host with `-device ramfb` (or `virtio-gpu-pci`) gets a real GOP and
  `app_server` runs its ordinary local-framebuffer path with **no Haiku change**;
  the Linux host owns the surface and captures it. Proven to paint (1482 colours
  bare / 2383 with a window; idle host cost ramfb ~0.5 % / virtio-gpu ~1.6 % of a
  core) and QEMU's own VNC already streams it interactively. This is the pixel
  source for the first full-motion slice because it needs no in-guest encoder.
- **Route 1 (#95), in-guest, portable:** an offscreen `BitmapHWInterface` in
  `app_server` (`BitmapDrawingEngine.cpp:61-78` is the working template;
  `RemoteDrawingEngine` already owns one) gives a captureable surface on **any**
  instance, not just `.metal`, with damage straight from the drawing engine.
- **The missing clock:** there is no "composition finished" event
  (`Desktop::MarkDirty` fans out and never hears back). Both routes must
  accumulate damage in a `BRegion` and let an encoder thread sample-and-clear on
  its own clock; the natural frame boundary is `Window::EndUpdate`
  (`Window.cpp:1970-2000`), promoted to `RP_TIER_END_FRAME`.
- **Lever:** the merged arm64 NEON `memcpy` fix reduces the per-frame surface copy
  cost that both routes pay.

### Encode (Tier P only)
- **NEON `libjpeg-turbo`** for still/near-lossless tiles — and the free win from
  the `libjpeg-arm64` finding: building with `-DCMAKE_SYSTEM_PROCESSOR=aarch64`
  turns NEON *on* (it ships off by default), so the still path is NEON without new
  code.
- **NEON `libx264` linked directly** (no ffmpeg needed) for moving tiles,
  `--tune zerolatency --intra-refresh` with a VBV cap; ~0.3–0.7 of one core for
  1080p30 ultrafast. **Intra-refresh, not periodic keyframes** — a keyframe spike
  through a small ring and an SSH tunnel is a visible hitch.
- **`-mcpu=neoverse-*` / LSE atomics baseline** (merged, `20bf8f2711`) is the
  general compute lever the encoder inherits; SVE is now safe at EL0 (#88) but
  must be vector-length-agnostic and per-generation (the `_g3`/`_g4` flavour
  pattern), so encode kernels either stay NEON or ship per-generation.
- **Out-of-process encoder helper**, mandatory: `x264`/`x265` are GPLv2 and
  `app_server` is MIT and ships in the base image, so the codec lives behind a
  pipe (which also isolates a codec crash from the display server). The in-image
  default codec is permissively licensed (`libvpx`/SVT-AV1/`dav1d`/`openh264`).

### Transport
- **`TCP_NODELAY`** — never set today (only `SO_REUSEADDR` exists anywhere in the
  path); Nagle holds back exactly the small messages that matter (a 14-byte cursor
  move, an 81-byte text line). One `setsockopt`, large payoff.
- **Send-buffer autotune** (`tcp-send-autotune`) and **`SO_RCVBUF` autosizing**
  (`tcp-rcvbuf-cliff`) — the earlier "65535 cliff" work applies directly to the
  bulk Tier P stream.
- **Jumbo frames** (MTU 9001, merged/hw-verified) and **ENA multiqueue headroom**
  — throughput for the media channel once it leaves loopback.
- **`ssh -C`** — one character; measure it (E5) before writing any stream
  compressor of our own. Below-framing LZ4/deflate preserves the skip-unknown
  property if `ssh -C` proves insufficient.
- **ECN** (staged design, `ref-ecn-design.md`) — relevant only to the media
  channel's congestion response; not a throughput lever here.

### Decode / present
- **Entirely the client's problem; no Graviton lever touches it.** This is stated
  so the budget is honest: the server-side levers stop at the wire.

---

## 7. Latency budget

Glass-to-glass, decomposed. Numbers marked *(measured)* are ours on our hardware;
others are targets or arithmetic on verified byte layouts.

| stage | Tier V (draw op) | Tier P (codec) | notes |
|---|---|---|---|
| input capture (client) | ~1 ms | ~1 ms | |
| client→server RTT | LAN ~1 ms / WAN 20–80 ms | same | `TCP_NODELAY` removes ~10–200 ms of Nagle+delayed-ACK |
| server damage coalesce | sub-ms | one frame ≈ 16 ms @ 60 Hz | Tier V flushes per op; Tier P waits for the frame clock |
| serialize / encode | sub-ms (81 B text line) | ~5–15 ms encode | encode is *not* the bottleneck for the desktop |
| transport | 4 KiB-chunked ring drain, ~0.13 ms @ 1 Gbps | bulk, jumbo-framed | |
| decode + present (client) | rasterise ops | video decode | no server lever |

**Anchors from Route 2 (measured):** keystroke-to-framebuffer over QEMU VNC was
**p50 31 ms, p95/p99 81 ms, 0/200 over 100 ms**, floored by QEMU's 30 ms VNC poll
cadence — not by Haiku. Idle full frame **9.8 kB** ZRLE at 1280×800 (418:1 vs
raw); a 7 s window drag ran **169 kB/s**. Those set the realistic Stage-1 bar.

**Targets:** Tier V input→paint improves ≥ 10 ms at a 20 ms RTT from `TCP_NODELAY`
alone; full-motion Tier P glass-to-glass ~45–60 ms nearby, ~60–120 ms WAN, with
encode inside the frame budget.

---

## 8. Defects closed (D1–D10)

The prior review found ten defects by reading the current
`src/servers/app/drawing/interface/remote/` code. URP/1 closes each — most by a
small fix that M0/M1 carries, a few structurally.

| # | defect (site) | how URP/1 closes it |
|---|---|---|
| **D1** | Synchronous waits not guarded by connection state; `discardWithoutReader` makes `Flush()` succeed after discarding, so with **no client attached every `DrawString` stalls 1 s and every screenshot 10 s** (`RemoteDrawingEngine.cpp:893/900/1000`) | Three layers: a connected-state guard before every synchronous wait; **client-side font metrics pushed in `RP_HELLO_ACK`** so `string_width` never crosses the wire; and a negotiated `STRING_WIDTH_REPLY` capability so the server only issues a query the client promised to answer. Structurally impossible after M0. |
| **D2** | `NetSender` never advances its buffer pointer across a short `send()` (`NetSender.cpp:82-90`) → stream corruption (leading bytes duped, tail dropped) | One-line fix: advance `buffer` by `sendSize`. In the M0 defect batch. |
| **D3** | The **receive** ring is never emptied on reconnect (only the send ring is); a mid-message disconnect desyncs framing permanently and `_EventThread` returns → **input dead for the session** (`RemoteHWInterface.cpp:373`) | Empty `fReceiveBuffer` on every accept and respawn the event thread — folded into the generation/resync of `§9`/M1. |
| **D4** | State dedup + discard ⇒ state believed sent but never delivered and never resent (the server half of black-screen-on-reconnect) | Generation-stamped shadow + `RP_RESYNC` + `ReplayState()` that emits the shadow unconditionally, bypassing the dedup guards, for every live engine on accept (M1). |
| **D5** | `escapement_delta` list read out of bounds — `AddList(delta, length)` copies `length` structs from a pointer to **one** delta (`ServerWindow.cpp:3156-3159` vs `RemoteDrawingEngine.cpp:890`) | Send only the valid `delta[0]` (or the real array); removes an OOB stack read and `8·(len−1)` wasted bytes per string. M0 batch. |
| **D6** | `clicks` sent on `RP_MOUSE_UP`, read on `RP_MOUSE_DOWN` → **double-click detection dead over the wire** (`RemoteView.cpp:326-331` vs `RemoteEventStream.cpp:144-148`) | Align the field to one opcode; add the case to the conformance suite. M0 batch. |
| **D7** | `fLatestMouseMovedEvent` not cleared on dequeue → dangling pointer to `PeekLatestMouseMoved()` (`RemoteEventStream.cpp:64/87-90`) | Clear on dequeue. M0 batch. |
| **D8** | Timeout path does not drain `fResultNotify` → a late reply makes the **next** call return the **previous** result (`RemoteDrawingEngine.cpp:964-974`) | Drain the counting semaphore on the timeout path. Largely moot once D1 removes the waits, but fixed regardless. |
| **D9** | Native client gradient decode broken: `ReadGradient` called unconditionally and again in the branch; rect guards test the **arc** opcodes → every gradient mis-decodes, `*_RECT_GRADIENT` reaches `FillRect` with `gradient == NULL` (`RemoteView.cpp` gradient cases) | Fix the decode **and** add gradient cases to the conformance suite so it cannot regress unseen; this is the clearest evidence the protocol had no conformance test, which `§2` now supplies. |
| **D10** | Native client has no `RP_STRING_WIDTH` handler → falls to `default:` and burns the full 1 s timeout every `StringWidth` (`RemoteView.cpp` absent) | Negotiated `STRING_WIDTH_REPLY` + client-side metrics (D1) means the server never issues the query to a client that did not promise it; the conformance suite requires the reply of any client that advertises it. |

Two known specification hazards are also fixed by documentation-plus-assertion,
not redesign: `RP_COPY_RECT_NO_CLIPPING`'s **unstated source/destination
convention and overlap rule** (the shape that makes a mirrored/smeared blit easy
to write and hard to notice — stated normatively, with a scroll-both-directions
conformance case), and the inclusive-edge `BRect` off-by-one (`§2`).

---

## 9. Honest limits

- **No hardware GPU, and none coming on arm64 EC2.** `app_server` is pure-CPU AGG;
  the GL/GLES/EGL ABI exists (`libglvnd_devel`) but there is no accelerated driver,
  so it is software GL only, no DRM/KMS, no virgl. Tier P encodes CPU-rendered
  pixels; it never offloads rendering.
- **AV1 encode is not interactive** on a handful of Neoverse cores in 2026. AV1 is
  a decode-side and archival option, not a live encode candidate; x264/VP8/VP9
  zerolatency is the live path.
- **4:2:0 damages text and thin UI strokes** (why RDP has AVC444 at all). This is
  the whole reason Tier V exists: text stays vector, and Tier P is reserved for
  content where chroma subsampling is invisible. Browser 4:4:4 decode support is
  narrow and is a gating question for any "encode the whole frame" temptation —
  which URP/1 avoids by construction.
- **`TCPEndpoint::fLock` (#61) serialises the receive path**, ~47 % of the
  measured receive ceiling; a single consumer thread is ~100 % wall-clock on that
  lock. For a remote desktop the *inbound* stream is small (input events, frame
  ACKs), so #61 mostly bounds any inbound bulk and the media-channel feedback loop
  rather than the outbound video — but it is a real cap on symmetric or
  clipboard/file-heavy sessions and is out of scope here.
- **GPLv2 codec licensing** forces the out-of-process encoder helper and a
  permissive in-image default (`§6`); "we have x264" and "app_server can link x264
  in the shipping image" are different statements.
- **Multi-monitor is blocked in `app_server` itself** (`VirtualScreen.cpp`
  `RemoveScreen` returns `B_ERROR`) and virtio-gpu hardcodes scanout 0 — single
  head only.
- **Route 2 needs `.metal`**; per-desktop cost only pencils out shared across many
  guests on one metal.
- **The Route 2 blocking unknown is not yet green:** whether the *full*
  (non-bootstrap) canonical image, booted as a KVM guest with framebuffer + USB
  input on `c7g.metal`, comes up with painting desktop **and** live guest
  networking **and** working input *all at once*. Everything full-motion in
  M3–M5 is premature until that one boot is confirmed.

---

## 10. Milestones

Each step pays for itself; none is a flag day (back ends are chosen per Desktop at
runtime from an opaque `target` string, and every wire change after M0 is
capability-gated).

- **M0 — smallest end-to-end vertical slice (no new architecture).** The defect
  batch (D1–D2, D5–D8) + `TCP_NODELAY` + **crop `RP_DRAW_BITMAP` to the rect
  actually drawn** (the single largest byte saving: a browser blit drops from
  ~3.6 MB to the dirty strip; a 16×16 crop from a sprite sheet from ~16 KB to
  ~1 KB, no protocol change) + **`RP_HELLO` capability handshake**. Proves
  negotiation across all three clients with identical rendering, closes the
  headless stall, and buys the felt-latency win. In parallel, settle the Route 2
  blocking unknown (one metal, one full image, one boot). *Gate for everything
  below.*
- **M1 — reconnect made correct.** Content-addressed bitmap cache (negotiated,
  per-connection, client-declared budget, server models the LRU so a miss never
  needs a round trip; ~20:1 on the icon/cursor paths) + generation counter +
  `RP_RESYNC`/`ReplayState()`. Closes D3, D4, and the black-screen-on-reconnect
  by construction.
- **M2 — pricing + flow control.** Run the specified experiments (per-opcode byte
  census, encoder pricing on real captured frame sequences, `ssh -C`, the headless
  stall) to choose codec-on-bitmap vs full-frame video with numbers. Introduce
  `RP_TIER_END_FRAME` as a real frame boundary and a bounded, **op-aware** queue
  (queue messages not bytes; on overflow coalesce within a frame and drop
  *superseded* whole-frame content, never partial messages; worst case degrades to
  `RP_RESYNC`). Replaces "discard when nobody listens" with a policy.
- **M3 — first full-motion, via Route 2.** Host-side NEON `libx264` on the metal
  feeds Tier P over the media channel; the fastest path to moving pixels because
  it needs no in-guest encoder and is already proven to paint. Target ~45–60 ms
  glass-to-glass nearby.
- **M4 — portable capture, in-guest.** Server-side rasterising `BitmapHWInterface`
  (#95, R8) as a third `HWInterface`: in-guest screenshots (impossible today),
  headless GUI verification in CI, a real damage source, and Tier P on **any**
  instance rather than only `.metal`. Pays for itself before any video ships.
- **M5 — full hybrid.** Per-region Tier P codec tiles multiplexed with Tier V
  under the damage router; SPICE-style promotion of a repeatedly-updated
  cache key at fixed geometry into an inter-coded stream; QUIC/WebRTC transport;
  Opus audio. The draw-op path remains the default and the fallback throughout.

---

## 11. Relationship to the existing docs

- `remote-desktop-options.md` (#95, Route 1): its Option A "fix the native
  protocol" *is* M0 here; its Option B offscreen `BitmapHWInterface` is M4; its
  Option D libx264 pipeline is the encode lever of M3/M5. URP/1 is the protocol
  those options were missing.
- `vfb-route2-streaming.md` (#118, Route 2): its Stage-1 QEMU-VNC streaming is the
  measured latency anchor (`§7`) and its `.metal` capture is the M3 pixel source;
  its blocking unknown is M0's parallel gate.
- `remote-desktop-send-buffer-wedge.md`: the `discardWithoutReader` fix it shipped
  is load-bearing and stays — the M1 cache does **not** remove the cold first-paint
  burst that triggered the wedge (a first paint is all misses by construction).
- The earlier protocol review (topic branch, not on trunk) supplied the D1–D10 and
  R1–R10 analysis this consolidates; URP/1 is its recommendation (a hybrid that
  attacks the bitmap path, not the whole frame) carried to a concrete wire spec.
