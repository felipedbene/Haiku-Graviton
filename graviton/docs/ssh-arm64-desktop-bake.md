# arm64 image bake: OpenSSH + reachable/manageable AMI (the "make-ami" recipe)

This documents the previously-undocumented step that turns a plain Haiku arm64
build into the **reachable, SSH-manageable, browser-desktop** AMIs deployed on
EC2 — the piece that was missing when the bake recipe lived only in an ad-hoc
session. The recipe itself is now vendored in-tree at **`graviton/ssh/`** (no
more loose zip). Source of truth: `graviton/ssh/README.md` + the two artifacts
below.

## Key correction to earlier assumptions

The deployed "desktop" canonical AMIs are **NOT** the `@nightly`/`regular`
profile. They are **`@minimum-mmc` + the `graviton/ssh/UserBuildConfig`
injection**. This is why:
- `@minimum-mmc` builds on arm64 while `@nightly` does not (the latter also
  fails on the `userlandfs` FUSE server, separate from the missing packages).
- `openssh` shows as "not available" in a plain build (the arm64 HaikuPorts repo
  is bootstrap-only) — SSH is added by *injecting a locally cross-built hpkg*,
  not by a profile.
- The GUI works headless: it is a browser/remote desktop brought up by injected
  launch jobs, keyed on `TARGET_SCREEN` via an injected `UserSetupEnvironment`.

## The two artifacts

### 1. `graviton/ssh/build-openssh-arm64.sh` — cross-build OpenSSH → arm64 hpkg
- HaikuPorts publishes **no arm64 repo**, so there is no prebuilt `openssh`.
  *(Still true of **upstream** HaikuPorts. Note as of 2026-08-24 this tree has its
  own arm64 hpkg repo — 23 ports / 52 non-bootstrap packages, `pkgman`-installable —
  so read this as "upstream has none", not "none exists". See
  `package-chain-status.md`.)*
  This cross-builds **OpenSSH 10.4p1** (+ static **zlib 1.3.1**) against the
  arm64 cross-tools and the already-built `haiku`/`haiku_devel` package staging
  dirs, then wraps it as `openssh-10.4p1-1-arm64.hpkg`.
- **Independent of jam** — it only *reads* the staging dirs and never writes
  `generated.arm64`, so it is safe to run while an image build is in progress.
- Stages a sysroot at `/opt/haiku/sysroot-arm64` (COPIES headers/libs, never
  symlinks into `generated.arm64` — jam recreates those dirs mid-build and
  symlinks vanish, yielding bogus `configure` results). Has a `sys/mman.h`
  canary to catch an incomplete sysroot early.
- `--without-openssl` (~~no OpenSSL for arm64~~ — **`openssl3` IS now built for arm64
  and `pkgman`-installable, as of 2026-08-24; this is a choice of this build script,
  not a platform limitation any more**): **Ed25519 / ML-DSA keys only — no
  RSA, no ECDSA**. Statically links zlib so sshd's only shared deps are
  `libroot`/`libnetwork`/`libbsd` (all in the base `haiku` package).
- Also emits the image-level files consumed by `UserBuildConfig` under
  `/opt/haiku/ssh/image/`.

### 2. `graviton/ssh/UserBuildConfig` — inject everything, patch nothing
Copy to `generated.arm64/UserBuildConfig` (jam's supported extension point;
`Jamrules` picks it up). Its `UserBuildConfigRulePreImage` hook:
- Adds the `openssh` **hpkg** to `system/packages` (as a package, not loose
  files — packagefs preserves the executable bit; the image copy step does not).
- Drops `sshd_config`/`ssh_config` into `system/settings/ssh` (a packagefs
  *shine-through* = real writable BFS dir).
- Installs `launch_daemon` jobs → `settings/launch/{sshd, cloud_init_lite,
  remote_desktop}`.
- `cloud_init_lite` (in-tree, `src/bin/cloud_init_lite`) fetches the launch SSH
  key from IMDS and **merges** it into `authorized_keys`; the baked-in
  `files/authorized_keys` is the **fallback** for no-keypair / IMDS-blocked
  launches.
- Injects `remote-desktop.sh` + its launch job and `UserSetupEnvironment` (the
  load-bearing half of the browser desktop — sets `TARGET_SCREEN` for the whole
  session; without it the session services crash-loop on a NULL Desktop).
- Redirects the `network/services` file to a copy with the `net_server` ssh
  entry commented out, so **exactly one thing owns port 22** (sshd is driven by
  `launch_daemon`, which restarts it on death; `net_server` would not).
- Adds `findutils grep less sed` (arm64 bootstrap pkgs) so a remote shell is
  usable, and creates the `sshd` privsep user/group (mandatory in OpenSSH ≥7.5).

## Boot glue

> **Updated 2026-08-24: the description below is the PRE-FIX behaviour and it
> describes a bug.** "Generates the host key on first boot" was implemented as
> `if [ ! -f ]`, and **a zero-byte file exists** — so after a stop/start that lost
> the key's contents (the page writer never flushed file data), the guard was
> satisfied by a 411-byte run of zeros and sshd came up unreachable. Fixed and merged
> as `10a7d758bc` "graviton/ssh: make the sshd host key survive an unclean boot":
> `sshd_boot.sh` now **parse-checks the key with `ssh-keygen -y`** rather than testing
> for existence (one exec, and it turns the failure into a self-healing boot) and
> **`sync`s** afterwards. The kernel-side cause was fixed separately
> (`8331882470`). Full story: `ec2-stop-start.md`.
>
> **Do not re-derive the `if [ ! -f ]` pattern from this section.** The general rule
> it taught: an existence test is not an integrity test.

`graviton/ssh/files/sshd_boot.sh` (run by the sshd launch job): generates the
Ed25519 (and ML-DSA) host key on first boot, guards against a hot restart loop,
fixes `authorized_keys` modes, then `exec sshd -D` so `launch_daemon` tracks and
restarts sshd itself. Binds the wildcard address, so it listens even before any
NIC/link appears — virtio_net or ena get served with no further action.

## Gotchas (each one has bitten)
1. **The account is `baron`, uid 0** — `@minimum` leaves `HAIKU_ROOT_USER_NAME`
   unset, so `common-tail` falls back to `baron`. SSH in as `baron`.
2. **Ed25519 keys only** (`--without-openssl`). The existing EC2 key pair
   `haiku-graviton.pem` (RSA-2048) does **not** work against this sshd. Use an
   Ed25519 key (e.g. the baked-in `graviton/ssh/files/authorized_keys` pair).
   *(Still true of the shipped sshd. But the **reason** changed: `openssl3` is now
   available for arm64, so this is now a revisitable build choice rather than a
   platform fact.)*
3. **virtio-net needs `vectors=0`.** Under MSI-X the Haiku guest transmits but
   never receives → DHCP never completes → sshd unreachable though listening.
   Forcing INTx fixes it. ~~**Flagged as a likely ENA hazard too** — worth
   checking against the ENA MSI-X path.~~ **CHECKED, and the hazard did not
   materialise (2026-08-24): ENA receives fine under MSI-X.** One MSI-X vector on
   CPU 0 carries the whole ~10 Gbit/s ceiling. The virtio-net `vectors=0` workaround
   is specific to virtio-net; do not carry it across to ENA.

## Build + bake flow
```sh
# 1. cross-build OpenSSH (safe to run anytime; never touches generated.arm64)
bash graviton/ssh/build-openssh-arm64.sh          # -> /opt/haiku/ssh/*.hpkg + image/

# 2. install the jam hook
cp graviton/ssh/UserBuildConfig /opt/haiku/haiku/generated.arm64/UserBuildConfig

# 3. build the image (no other jam in this generated dir at the same time)
cd /opt/haiku/haiku/generated.arm64
HAIKU_REVISION=hrev59996 jam -q -j$(nproc) @minimum-mmc

# 4. GPT-convert + register (haiku-on-ec2 pipeline)
#    make-gpt-image.sh haiku-minimum.mmc haiku-ec2.raw 2147483648
#    aws ec2 import-snapshot (Format=raw) -> register-image
#      --architecture arm64 --boot-mode uefi --ena-support --virtualization-type hvm
#      --root-device-name /dev/xvda
```
The `UserBuildConfig` expects the build script's output under `HG_SSH_DIR`
(default `/opt/haiku/ssh`). When run from this repo, point `HG_SSH_DIR` at
`graviton/ssh` or stage the artifacts to `/opt/haiku/ssh` as the script does.

## Referenced but not vendored
`graviton/ssh/README.md` points at `docs/ssh-access.md`,
`docs/stage-b-remote-desktop.md`, `docs/gap-analysis.md` — the fuller write-ups
were not in the recipe archive. If they resurface, vendor them alongside this.
