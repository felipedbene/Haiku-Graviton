# Lean canonical AMI → builder: gaps to bake in

Running record of what the lean canonical AMI (`@minimum-mmc` + OpenSSH, the runtime
image) lacks when turned into a native `haikuporter` builder — so a dedicated builder
image can bake them in and stop rediscovering them. Established 2026-08-30 while
provisioning the first native builder and building glib2.

See `native-ec2-builds.md` for the full working provisioning + build sequence.

These builder-AMI gaps are now tracked as GitHub issues: #33 #35 #37 #38 #39 #40 #51.
The per-gap detail — missing build toolchain / `python3` symlink / `pkgconf` /
`haikuporter` + ports tree / basic userland, the bootstrap→DeBeOS "allow vendor change"
quirk on every install, the NVMe single-controller limit and 20 GiB root, the
unpublished `*_devel` and gtk-doc/docbook repo gaps, and the hazard that installing repo
packages onto a builder can break its own gcc toolchain — now lives in those issues.
