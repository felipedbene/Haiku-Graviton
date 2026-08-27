# DeBeOS package management docs

How DeBeOS builds, activates, and distributes native `.hpkg` packages on arm64.

- [`vending-design.md`](vending-design.md) — the package-management & vending design: the native
  package manager (local `file://` repo, proven on hardware), the remote CDN-vending architecture
  (private S3 + CloudFront/OAC), HTTP remote install **proven** on hardware (and why the old "EINTR"
  report was a phantom — a missing `repo.info` 403), the one remaining gap (HTTPS/openssl, parked),
  and the `haiku-repo-publish` build+publish tool.
- [`modular-ami-buildout.md`](modular-ami-buildout.md) — the gated 6-phase runbook that turns the
  fatty AMI into a **modular** one (lean base + CDN-vended features): closure → build missing →
  publish repo → lean bake → prove parity → promote, and the `haiku-package-closure` /
  `haiku-repo-publish` / `haiku-s3` tooling that drives it.

Related:
- Building the chain itself lives in `../package-chain-status.md` and `../arm64-package-bootstrap.md`.
- The propose-only bug-fix / enhancement tooling that drives fixes to hardware proof is in
  `../../audit/`.
