<img src="data/artwork/debeos/debeos-logo-256.png" alt="DeBeOS" width="140" align="right">

# DeBeOS Maturity Roadmap

The north star for where DeBeOS is going. DeBeOS is **ARM-first** and its guiding
goal after bring-up is **self-sufficiency** — the ability to build real software
*on the machine*, not merely cross-compile it elsewhere. Priorities are ordered by
stage; each stage's status reflects what has actually been measured on real hardware,
and open items are named honestly rather than hidden.

## Stage 0 — Bring-up ✅ *(done)*
Native boot on AWS Graviton, the ENA network driver, remote desktop (in-tree browser
plus a remote client), clean ACPI shutdown, and serial-console boot logs. See
[the "What works today" table in the README](README.md#what-works-today-measured).

## Stage 1 — Self-sufficiency *(in progress)*
Build real software **on DeBeOS**, not just cross-compiled for it.

1. **ripgrep** and **fd** cross-built for `aarch64-unknown-haiku` and packaged as hpkg.
2. **ripgrep built natively on-device** — the real self-sufficiency test, exercising
   networking, TLS and DNS that a self-hosting compiler never touches.
3. **cargo ↔ crates.io works natively** — a live `cargo build` fetches and compiles
   real crates from crates.io on the machine; the BSD socket layer, TLS and DNS are
   all sufficient. (A pure-Rust TLS stack, avoiding the C crypto libraries, is a
   nice-to-have still open.)
4. **Filesystem reliability under heavy load — RESOLVED.** Driving a big native Rust
   build surfaced a page-writer / BFS defect that corrupted freshly written files under
   concurrent I/O; that fix is **merged**, and it also advanced Stage 4's crash-safety
   work (periodic writeback now bounds what a forced stop can lose). **Still open:** a
   repeatable `crate name → native binary + hpkg` script, and a linker limit on very
   large objects — worked around today with `--no-gc-sections`, with a newer binutils in
   the toolchain the proper fix.

## Stage 2 — Package ecosystem
- **Repository — DONE.** A hosted DeBeOS package repository is live, with `pkgman` and
  HaikuDepot pointing at it and remote install working over both HTTP and HTTPS; new
  packages are published on release. **Known hazard, still open:** an image must ship
  *only* the DeBeOS repository as its update source — leaving an upstream base
  repository in place lets an update replace the DeBeOS-patched kernel and boot loader
  with stock packages that do not boot this hardware. Still to decide: whether binaries
  bundle their runtime libraries or rely on a system base package.
- **Porting playbook:** a living "symptom → root cause → fix" document plus a starter
  target-spec template, so per-port flags aren't re-derived by hand each time.
- **Critical mass of tools:** uutils/coreutils (one port, dozens of utilities), then
  Node.js on Haiku's existing libuv port (a real integration project).

## Stage 3 — Platform expansion
Raspberry Pi 5, Raspberry Pi 3, and RISC-V — which forces a clean separation between
"ARM-first / portable OS" and Graviton-specific assumptions.

## Stage 4 — Robustness
A legacy-bug audit, device watchdogs, and **filesystem crash-safety on a forced stop**
(closely related to the Stage-1 reliability work).

## Stage 5 — Identity & distribution
An open question worth resolving early, because it shapes how much Stage 2/3 investment
is worthwhile: **hobby-complete and self-hosted**, versus **community-distributable**.

## Pending feature work (recorded for completeness; not the current priority)

- **Remote desktop for headless Graviton — phased plan.** Graviton EC2 has no display device,
  so `app_server` runs its `RemoteHWInterface` (a display-list stream to a remote client) rather
  than a local framebuffer. Design of record: `graviton/docs/remote-desktop-options.md`.
  - *Phase 0 (days):* latency fixes to the native display-list protocol — `TCP_NODELAY`, compute
    `DrawString` pen-advance locally instead of a per-string round-trip, cache/compress bitmaps,
    LZ4 + a larger send ring. Biggest felt-lag win, no new architecture.
  - *Phase 1 (~1 wk):* an offscreen software HWInterface (Haiku already ships
    `BitmapHWInterface`) rendering the display list into a real in-memory `BBitmap` — the one
    load-bearing new piece; unlocks every pixel route.
  - *Phase 2 (~1–2 wk):* VNC (`libvncserver`, NEON libjpeg-turbo) on that surface — broad
    native-client reach.
  - *Phase 3 (optional):* full-motion via `libx264` (NEON; no ffmpeg needed) over
    WebCodecs/WebTransport. RustDesk deprioritized.
- **mprotect device-area guard — ramfb re-verify owed.** The device-area cache-type guard
  (merged; on the current canonical) is compile-verified only. It is reachable via the
  framebuffer-clone path, which a bare (display-less) Graviton instance cannot exercise, so it
  was never run on hardware. Re-verify on a virtual-framebuffer-equipped guest — this naturally
  rides the remote-desktop offscreen-framebuffer work (Phase 1) above.
- **BFS root auto-grow — loader-driven trigger, hardware one-boot re-verify owed.** A canonical
  AMI booted on a larger root EBS grows its BFS root to fill the disk on the first boot, with no
  rebake and no reboot: the EFI loader extends the trailing BFS GPT partition before the kernel
  mounts the boot volume, so the existing mount-time BFS grow fires on boot one. The in-place
  append grow was hardware-proven earlier (checkfs clean across crash-states + a power cut); the
  loader-time trigger is compile-verified only (`haiku_loader.efi` links against the arm64
  cross-tools) and has not yet been booted. Owed: bake onto a larger EBS and confirm `df /boot`
  widens on first boot, checkfs clean, second boot a no-op (verification plan T4). Design:
  `graviton/docs/develop/bfs-auto-grow-design.md`. Detecting an *online* EBS grow, a hot-attached
  second volume, or multiple NVMe controllers is a separate, still-open kernel item.
