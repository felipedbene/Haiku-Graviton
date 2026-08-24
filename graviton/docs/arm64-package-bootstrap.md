# Bootstrapping arm64 HaikuPorts packages (browser chain, translators, …)

Status as of **2026-08-24**: **SUPERSEDED by `package-chain-status.md`. This file is a
historical record of the first attempt; do not plan from it.**

> ~~Status: **method validated, blocked on a haikuporter chroot/permission setup issue.**
> No arm64 packages exist to shortcut this — they must be built from source.~~
>
> **Every blocking claim in this document is closed.** As of 2026-08-24 there are
> **23 ports / 52 non-bootstrap arm64 hpkgs**, all built against the repaired
> non-dirty chroot and all `pkgman`-installable, across five healthy build guests.
> The chroot/permission problem (`haikuporter` running as an unprivileged user,
> failing to chroot and failing to create the `lib` symlink), the missing `setfattr`,
> and the "Next steps" list below were all resolved.
>
> Also stale: **"there is no arm64 package repo to point at"** and the 58-vs-398
> package comparison — true of *upstream* HaikuPorts, but this tree now has its own
> arm64 hpkg repo. And **"Nothing committed"** — the recipe patches are committed
> under `graviton/haikuports-patches/`.
>
> **The live state, including the one blocker that is still open, is in
> `package-chain-status.md`. Read that instead.** (That file is owned by another
> agent as of this edit and is not summarised here beyond the counts above.)

## Why this is needed

On arm64 the build reports these build-feature packages as unavailable, which gates
off large parts of userland (image translators, GL, media, the browser, MIDI, …):

    giflib glu mesa ffmpeg fluidlite libvorbis fontconfig gutenprint webkit libpng
    libicns jasper jpeg libedit qrencode_kdl tiff libdvdread libdvdnav libraw
    libwebp libavif live555 zstd

Upstream `eu.hpkg.haiku-os.org/haikuports/master/` publishes only `riscv64`,
`x86_64`, `x86_gcc2`. The arm64 repo config lists only 58 `*_bootstrap` packages
(vs 398 real ones for x86_64). So there is **no arm64 package repo to point at** —
these recipes must be built from source via haikuporter.

## Builder (metal) capabilities — the native-build route works

`c7g.metal` (i-0f7f6f3e8922acffd): 64 cores, 125 G RAM, 462 G free, **`/dev/kvm`
present**, `qemu-system-aarch64` installed. So the intended route — boot an arm64
Haiku image under KVM and run haikuporter natively — is viable on this host.

## Environment already set up on the metal

- Cloned into `/opt/haiku/`: `haikuporter`, `haikuports.cross`, `haikuports`
  (`haikuwebkit-1.10.0.recipe` is at `haikuports/haiku-libs/haikuwebkit/`).
- Bootstrap configure succeeded, reusing the existing cross-tools (no ~1h rebuild):

      cd /opt/haiku/haiku/generated.bootstrap
      ../configure \
        --cross-tools-prefix /opt/haiku/haiku/generated.arm64/cross-tools-arm64/bin/aarch64-unknown-haiku- \
        --bootstrap /opt/haiku/haikuporter/haikuporter \
                    /opt/haiku/haikuports.cross /opt/haiku/haikuports

  → `generated.bootstrap`, `HAIKU_IS_BOOTSTRAP=1`, arch arm64.

## Method validated, then blocked

`jam -q -j64 @bootstrap-raw` drove haikuporter and built **3440 targets** before
failing on 4 (`gcc_bootstrap`, `gcc_bootstrap_syslibs[_devel]`, `zlib_bootstrap_source`):

    Setting up sysroot for non-chroot build: .../gcc_bootstrap/work-.../boot/cross
    [Errno 13] Permission denied: 'lib'

Root cause: haikuporter runs as `ubuntu`, cannot chroot, falls back to a non-chroot
sysroot setup, and fails creating the `lib` symlink. Environmental, not an arm64
code-porting wall. (`binutils_cross_x86 … cannot be built for arm64` lines are
benign x86-recipe noise.) `configure` also warned **`setfattr` not found** — host is
missing the `attr` package, likely needed for package xattrs later.

## Next steps

1. `apt install attr` on the metal (provides `setfattr`).
2. Run the bootstrap with chroot capability (as root) — or fix haikuporter's
   non-chroot sysroot permissions — then re-run `jam @bootstrap-raw` to completion.
3. Bootstrap yields only the *bootstrap* package set. Build the browser chain
   **after**: boot the bootstrap arm64 image under KVM on the metal and run
   `haikuporter` natively for `haikuwebkit` + its deps (libpng, jpeg, …).
   Expect **multi-hour-to-multi-day** builds and likely arm64 patches to
   third-party recipes (webkit especially).

## Left in place on the metal

`generated.bootstrap` and the cloned port trees remain for the next attempt. No
recipe patches were needed yet (the blocker is environmental). Nothing committed.
