# HANDOFF — `remote-latency-phase0`

Branch: `remote-latency-phase0` (off `graviton`, 2 code commits + this doc)
Scope: reduce interactive lag in the app_server remote drawing interface
(`src/servers/app/drawing/interface/remote/`) — the protocol behind the remote client
and the only way to reach a headless Graviton desktop.
Status: **compile-reviewed, NOT built** (this workstation has no jam/cross-tools).
Must be built and boot-tested before merge — see "Build & verify" below.

This is the safe, server-only slice of "Option A" from the remote-desktop options
analysis. The higher-value protocol changes are deferred with reason — read
"Deferred (do not skip)" before planning the next branch.

---

## Why

A Graviton EC2 instance has no display device (EFI hands the kernel
`frame_buffer.enabled = false`), so `app_server` runs `RemoteHWInterface`: it
streams a display list to a remote client that renders, and takes input back.
The remote client wraps this. It works but is laggy.

An audit of the interface located the lag. It is **not bandwidth** — the protocol
is vector draw commands, which are cheap. It is:

1. **Synchronous round-trips on text.** `DrawString` (`RemoteDrawingEngine.cpp:899`),
   `DrawString`-with-offsets (`:930`) and `StringWidth` (`:963`) block the
   app_server drawing thread for a full network RTT waiting for a `*_RESULT`
   reply — on every label, menu item, list row and caret blink.
2. **Nagle's algorithm.** The connection was left at defaults; small requests were
   held to coalesce and then hit the peer's delayed ACK, adding ~40–200 ms to each
   of those round-trips and to every bitmap flush. `TCP_NODELAY` was never set.
3. **Raw uncompressed bitmaps.** `RP_DRAW_BITMAP` ships `4·N·M` bytes with no
   compression or caching (`RemoteMessage.cpp:91-111`; there is a
   `// TODO: cache/checksum` at `RemoteDrawingEngine.cpp:383`). A 256×256 icon is
   ~256 KiB, re-sent every frame.
4. **16 KiB send ring buffer** (`RemoteHWInterface.cpp:97`): any draw larger than
   the buffer stalled the producing thread while the sender drained it 4 KiB at a
   time.

---

## What shipped on this branch

Two commits, both in `src/servers/app/drawing/interface/remote/RemoteHWInterface.cpp`:

### 1. `remote: disable Nagle on the connection to cut interactive latency`
Set `TCP_NODELAY` on the accepted connection in `_NewConnection()`, via the send
endpoint (which shares the socket fd with the receiver, so inbound input and
result replies are covered too). Added `#include <netinet/tcp.h>`.

```c
int noDelay = 1;
sendEndpoint->SetOption(TCP_NODELAY, IPPROTO_TCP, &noDelay, sizeof(noDelay));
```

This is the amplifier: it does not remove the round-trips (see Deferred), but it
takes the Nagle+delayed-ACK penalty off each one and off every bitmap flush.
Best-effort — a `SetOption` failure only preserves the prior (laggier) behaviour.
Rationale: the interface is loopback-only and reached over an SSH tunnel, so this
latency is paid on every interaction.

### 2. `remote: enlarge the send ring buffer to 1 MiB to avoid bitmap stalls`
`StreamingRingBuffer(16 * 1024, true)` → `StreamingRingBuffer(1 * 1024 * 1024, true)`.
Covers a burst of icons plus a wallpaper tile against the drain, removing the
per-bitmap producer stall documented in the `_NewConnection()` comment (Deskbar
tray icons). The ring is a fixed up-front `malloc`, so this costs 1 MiB per
session — negligible. Receive buffer left at 16 KiB (small messages only).

Neither change alters the wire protocol, so an updated server interoperates with
an unchanged client.

---

## Deferred (do not skip this section)

The audit's highest-value fix — **making `DrawString` fire-and-forget** — was
NOT done here because it is **unsafe as a server-only change**:

- `DrawString`, `StringWidth` and `ReadBitmap` all share one result semaphore,
  `fResultNotify`.
- The client replies with `RP_DRAW_STRING_RESULT` for *every* `RP_DRAW_STRING`,
  and `_DrawingEngineResult()` (`RemoteDrawingEngine.cpp:1086`) releases the
  semaphore unconditionally per reply.
- If the server stops waiting but the client keeps replying, the surplus releases
  desync the shared semaphore: a later `StringWidth`/`ReadBitmap` acquires a stale
  count and returns a garbage/racy value.

Doing it correctly needs a **coordinated client + server change** (in the remote
client, which is not in this tree): either the client stops
sending the result for `DrawString`, or replies carry sequence numbers so the
dispatcher can match them. The same coordination applies to the other two
deferred wins, both of which need client-side decode:

- **LZ4 stream compression** in `NetSender`/`NetReceiver`.
- **Bitmap compression + checksum cache** for `RP_DRAW_BITMAP` (NEON
  libjpeg-turbo for photos; skip re-sending unchanged bitmaps per the existing
  TODO).

These belong in a "Phase 0.5" branch that bumps the protocol version and changes
both ends together. That is where most of the remaining lag lives.

---

## Build & verify (required before merge)

Per `AGENTS.md`, from a non-Haiku host with `buildtools` checked out beside this
repo:

```bash
mkdir -p generated.arm64 && cd generated.arm64
../configure --cross-tools-source ../../buildtools --build-cross-tools arm64
jam -q app_server            > build.log 2>&1; tail -n 40 build.log   # component build
jam -q @nightly-anyboot      > build.log 2>&1; tail -n 40 build.log   # bootable image
```

Boot the image under QEMU (arm64) or bake an AMI and boot on Graviton in
us-west-2 (`graviton/scripts/haiku-canonical`; test hosts are SSM-managed — drive
with `aws ssm`, not SSH).

Verify the change actually took effect (don't infer from the diff):
- Connect via the remote client over the SSH tunnel and confirm the interface chosen is
  `RemoteHWInterface` (not a local framebuffer).
- Latency: scrolling a text-heavy list / opening menus should feel materially
  snappier than the pre-`TCP_NODELAY` build on the same link. For a hard number,
  compare on a shaped/higher-RTT link (e.g. `tc netem` on the tunnel host) — Nagle
  removal shows up most there.
- Bitmap stall: open Deskbar with several tray icons + a wallpaper; the initial
  paint should no longer hitch. Confirm no regression in memory (1 MiB/session).

---

## Risk

- Both changes are small, localized, and do not touch the wire format.
- `SetOption(TCP_NODELAY, IPPROTO_TCP, …)` matches Haiku's `BNetEndpoint`
  signature `(option, level, data, length)`; `<netinet/tcp.h>` is in-tree.
- `TCP_NODELAY` trades a little bandwidth for latency — correct for interactive
  use; no downside on loopback/tunnel.
- Main residual risk is simply that it is unbuilt here: build + boot before merge.

---

## Merge

Topic branch → `graviton` (never commit to `graviton` directly). Squash or
rebase as the maintainer prefers; the two code commits are independent and can
merge in either order. Patch series is also exported outside the tree at
`remote-latency-phase0-patches/`.

## References
- Options analysis / decision record: `remote-desktop-options.md` (kept outside
  the tree; ask the maintainer). Summarizes the six-dimension investigation
  (native protocol, offscreen `BitmapHWInterface`, VNC vs RustDesk, codec +
  transport, DCV internals, POSIX layer) and the phased path this branch begins.
- The offscreen `BitmapHWInterface` desktop-screen work (~3–5 days) is the next
  structural piece: it unlocks the pixel routes (VNC first) once Phase 0/0.5 land.
