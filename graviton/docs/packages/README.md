# DeBeOS package management docs

How DeBeOS builds, activates, and distributes native `.hpkg` packages on arm64.

- [`vending-design.md`](vending-design.md) — the package-management & vending design: the native
  package manager (local `file://` repo, proven on hardware), the remote CDN-vending architecture
  (private S3 + CloudFront/OAC), the measured client gap that blocks remote install on arm64
  (HTTPS handler missing + an HTTP-fetch `EINTR` bug), and the fix plan to enable remote install.

Related:
- Building the chain itself lives in `../package-chain-status.md` and `../arm64-package-bootstrap.md`.
- The propose-only bug-fix / enhancement tooling that drives fixes to hardware proof is in
  `../../audit/`.
