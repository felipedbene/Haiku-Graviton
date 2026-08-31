<img src="data/artwork/debeos/debeos-logo-256.png" alt="DeBeOS" width="140" align="right">

# DeBeOS Maturity Roadmap

The north star for where DeBeOS is going. DeBeOS is **ARM-first** and its guiding
goal after bring-up is **self-sufficiency** — the ability to build real software
*on the machine*, not merely cross-compile it elsewhere. Priorities are ordered by
stage; each stage's status reflects what has actually been measured on real hardware.
Actionable milestones are linked to GitHub issues, where the current detail lives;
this file is the vision, not the tracker.

## Stage 0 — Bring-up ✅ *(done)*
Native boot on AWS Graviton, the ENA network driver, remote desktop (in-tree browser
plus a remote client), clean ACPI shutdown, and serial-console boot logs. See
[the "What works today" table in the README](README.md#what-works-today-measured).

## Stage 1 — Self-sufficiency *(in progress)*
Build real software **on DeBeOS**, not just cross-compiled for it.

1. **ripgrep** and **fd** cross-built for `aarch64-unknown-haiku` and packaged as hpkg — done.
2. **ripgrep built natively on-device** — done; exercises networking, TLS, DNS.
3. **cargo ↔ crates.io works natively** — done for HTTP/HTTPS install; pure-Rust TLS is a nice-to-have.
4. **Filesystem reliability under heavy load** — page-writer/BFS defect fixed and merged.
5. **Linker limit on very large objects** — worked around today with `--no-gc-sections`; proper fix is a newer binutils in the toolchain (#89).
6. **Repeatable `crate name → native binary + hpkg` script** (#116).

## Stage 2 — Package ecosystem
- **Repository — DONE.** Hosted DeBeOS package repository is live over both HTTP and HTTPS; `pkgman` and HaikuDepot point at it. Known hazard, still open: an image must ship *only* the DeBeOS repository as its update source, or a `pkgman update` swaps in stock upstream kernel/loader packages that don't boot this hardware.
- **Porting playbook** — living "symptom → root cause → fix" document plus a starter target-spec template (#90).
- **Critical mass of tools** — uutils/coreutils, then Node.js on Haiku's libuv port (#93).
- **Identity / bundling policy** — decide whether binaries bundle their runtime libraries or rely on a system base package (#92).

## Stage 3 — Platform expansion
Raspberry Pi 5, Raspberry Pi 3, and RISC-V — which forces a clean separation between
"ARM-first / portable OS" and Graviton-specific assumptions (#94).

## Stage 4 — Robustness
A legacy-bug audit, device watchdogs, and **filesystem crash-safety on a forced stop**
(closely related to the Stage-1 reliability work) (#91).

## Stage 5 — Identity & distribution
An open question worth resolving early, because it shapes how much Stage 2/3 investment
is worthwhile: **hobby-complete and self-hosted**, versus **community-distributable** (#92).

## Pending feature work (recorded for completeness; not the current priority)

- **Remote desktop for headless Graviton — phased plan.** Graviton EC2 has no display device,
  so `app_server` runs its `RemoteHWInterface` (a display-list stream to a remote client) rather
  than a local framebuffer. Design of record: `graviton/docs/remote-desktop-options.md`.
  - *Phase 0 (days):* latency fixes to the native display-list protocol — `TCP_NODELAY`, local
    `DrawString` pen-advance, cache/compress bitmaps, LZ4 + a larger send ring. Biggest felt-lag
    win, no new architecture.
  - *Phase 1 (~1 wk):* an offscreen software HWInterface rendering the display list into a real
    in-memory `BBitmap` — the one load-bearing new piece; unlocks every pixel route (#95).
    An alternative virtual-framebuffer route (#118) is under consideration alongside it.
  - *Phase 2 (~1–2 wk):* VNC on that surface — broad native-client reach.
  - *Phase 3 (optional):* full-motion via `libx264` (NEON) over WebCodecs/WebTransport.
- **mprotect device-area guard — ramfb re-verify owed.** The device-area cache-type guard is
  compile-verified only; it is reachable via the framebuffer-clone path, which a bare
  (display-less) Graviton instance cannot exercise. Re-verify on a virtual-framebuffer-equipped
  guest — naturally rides the remote-desktop offscreen-framebuffer work (Phase 1) above (#83).
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
