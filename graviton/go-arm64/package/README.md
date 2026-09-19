# golang hpkg — the NATIVE Go toolchain for DeBeOS (Haiku) arm64 (M8, #378)

This directory packages a **self-hosting** Go distribution: the `go` command
and every tool it drives (`compile`, `asm`, `link`, `cover`, `vet`, `cgo`,
`gofmt`, …) as **AArch64 Haiku ELF**, so `go build` runs *on* a Graviton
DeBeOS box — not just as a `GOOS=haiku` cross-compile from Linux. It is the
distribution deliverable of issue #378, on top of the M0–M6 toolchain arc
(see `../README.md`).

## What proves it works

`go version` → `go version go1.26.1 haiku/arm64` on the box; a native
`go build` of a hello program spawns the native `compile`/`asm`/`link` tools
(the M6 `fork`+`exec` path, patch 0004) and produces a runnable AArch64 ELF; a
multi-package module and the real `debeos-aws` (aws-sdk-go-v2) both build and
run natively. Full transcript: [`../logs/M8-native-distribution-proof.txt`](../logs/M8-native-distribution-proof.txt).

## Layout (package root → `/boot/system`)

```
develop/lib/go/            # GOROOT (auto-detected from the bin/go symlink)
  bin/{go,gofmt}           #   native AArch64 Haiku ELF
  pkg/tool/haiku_arm64/*   #   native compile, asm, link, cover, vet, cgo, ... (19)
  pkg/include/*  src/*  lib/*  go.env  VERSION  LICENSE
bin/{go,gofmt}             # relative symlinks -> ../develop/lib/go/bin/*
data/profile.d/go.sh       # GOROOT/GOPATH/PATH (belt-and-suspenders; auto-detect suffices)
data/licenses/Go-BSD-3-Clause
.PackageInfo
```

`pkg/haiku_arm64` is intentionally **absent** — modern Go (≥1.20) ships no
prebuilt std `.a`; `go build` compiles std on demand into `GOCACHE`, which is
exactly what exercises the native compiler on the box.

**zlib is mandatory** (`package create -z zlib`): the lean image's packagefs
has no zstd reader, so a zstd hpkg lands in `system/packages` but fails to
activate at first boot — the same constraint that governs the `amazon_ssm_agent`
and OpenSSH packages here.

## Build it

1. **Cross-build the native toolchain** on a Linux host, from a korli/go
   checkout with the DeBeOS patchset (`../patches/0001..0004`) applied and a
   working host `bin/go`:

   ```sh
   GOOS=haiku GOARCH=arm64 CGO_ENABLED=0 GOTOOLCHAIN=local \
       ./bin/go install std cmd
   ```

   This regenerates `bin/haiku_arm64/{go,gofmt}` and `pkg/tool/haiku_arm64/*`.
   Rebuild after any fork patch — the M6 fork+exec fix (0004) **must** be in the
   native `go`, since `go build` spawns compile/link through it.

2. **Stage the GOROOT** (Linux): `SRC=/path/to/korli-go sh stage-goroot.sh`
   → a packagefs layout in `$ROOT` (default `/tmp/go-pkg-378/root`).

3. **Create the hpkg** on a Haiku box (the `package` tool is Haiku-only): copy
   `$ROOT` + this dir's scripts over and `sh build-hpkg-golang.sh`
   → `golang-<ver>-1-arm64.hpkg` (zlib). `verify-native.sh` installs it and
   runs the acceptance builds.

No archiver/gzip ships on the lean image; `pkgman install -y tar gzip` first if
you transfer a tarball.

## cgo

`CGO_ENABLED=0` (the distribution default). Native cgo — an arm64 gcc bridge to
`libroot` — is a follow-up; it is unneeded for pure-Go builds (the SSM agent,
`debeos-aws`, and these tests all build cgo-off).

## Baking (wiring plan — NOT done here)

Same pattern the `debeos_ssm_agent` bake uses: drop the hpkg with
`AddPackageFilesToHaikuImage system packages` (the lean image has no DeBeOS
repo as a `HAIKU_REPOSITORIES` member, and `package_repo` rejects the DeBeOS
vendor), plus a version-agnostic `--include "golang-*"` in the pool sync. This
task harvests to **STAGING only** and does not touch canonical/pipeline/green.
```
