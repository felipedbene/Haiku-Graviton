# Lean canonical AMI → builder: gaps to bake in

Running record of what the lean canonical AMI (`@minimum-mmc` + OpenSSH, the runtime image)
lacks when turned into a native `haikuporter` builder, so these can be baked into a
dedicated builder image later and stop being rediscovered. Established 2026-08-30 while
provisioning the first native builder and building glib2. Everything below was
`pkgman`-installable from the DeBeOS repo unless noted — the fix is to **bake it in**.

See `native-ec2-builds.md` for the full working provisioning + build sequence.

## Missing entirely (had to install)

- **Build toolchain:** `gcc`, `binutils`, `make`, `cmake`, `meson`, `ninja`, `git`,
  `diffutils`, `patch`, `gawk`, `wget`, `haiku_devel`.
- **Python:** none in base. `python3.14` installs (see vendor conflict) but there is **no
  `python3` symlink** — `haikuporter`'s `#!/usr/bin/env python3` shebang fails
  ("env: python3: No such file"). Bake `python3 -> python3.14`.
- **pkg-config:** the package is **`pkgconf`**, NOT `pkgconfig`/`pkg_config` (those names do
  not exist). A batch `pkgman install` with any wrong name aborts the WHOLE batch — install
  per-package or use exact names.
- **haikuporter + ports tree:** not packaged; git-cloned from
  `github.com/haikuports/{haikuporter,haikuports}`. Bake these (or ship a `haikuporter`
  package) plus a ready `haikuports.conf` (its sample lives in the *haikuporter* repo; set
  `PACKAGER` + `TREE_PATH`).
- **Basic userland:** `awk`, `which` absent from base.

## Quirks

- **Vendor conflict on every toolchain install:** the base ships bootstrap "Haiku Project"
  libs (`zlib_bootstrap`, `sqlite`, …) that collide with DeBeOS repo versions, so `pkgman`
  demands "allow vendor change" (solution 1) each time. Non-interactive:
  `printf '1\n…\ny\n' | pkgman install …`. Fix: bake DeBeOS-vendored base libs so there is
  no conflict.

## Kernel / driver (not lean-image, tracked separately)

- **NVMe driver enumerates only one controller:** a second hot-attached EBS is invisible,
  and an online root-EBS grow is not seen (disk size cached at boot — `partition_grow` says
  "already fills disk" at the old size). Growth path = launch with a big root
  (`--block-device-mappings`) + first-boot `partition_grow` + `resizefs`.
- Root is 20 GiB (tight; a single package build fits, larger closures will not).

_(Append as more surface during builds — e.g. per-recipe build tools like `bison`, `flex`,
`bash_completion`, `setuptools_python310`, discovered building glib2.)_
