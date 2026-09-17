# Go toolchain for DeBeOS (haiku/arm64) — M0

This directory holds the DeBeOS-side artifacts for bringing the Go toolchain to
haiku/arm64 (AWS Graviton). The design and milestone ladder live in
[`../docs/go-arm64-bringup-scope.md`](../docs/go-arm64-bringup-scope.md); this
is the **M0** deliverable: *a `GOOS=haiku GOARCH=arm64` gc Go toolchain that
cross-builds from a Linux host.*

## Status: M0 DONE (cross-build proven)

Built from `github.com/korli/go @ go1.26.1-haiku1` (the maintained, previously
x86_64-only Haiku fork) plus the arm64 patchset in [`patches/`](patches/):

- `GOOS=haiku GOARCH=arm64 ./make.bash` completes — *"Building packages and
  commands for target, haiku/arm64 … Installed Go for haiku/arm64."*
- `go build std` and `go build cmd` (i.e. `go`, `gofmt`, `compile`, `link`,
  `asm`, `vet`, `cgo`, …) both cross-build clean for haiku/arm64.
- A trivial `package main` cross-compiles to a **AArch64 Haiku ELF**:
  `ELF 64-bit … ARM aarch64 … interpreter /system/runtime_loader`, `NEEDED
  libroot.so`. `net/http`-importing programs also link.
- A `go-1.26.1-haiku-arm64-bootstrap.tbz` was produced with
  `GOOS=haiku GOARCH=arm64 ./bootstrap.bash` (the fresh artifact the scope doc
  called out). It contains AArch64 Haiku `go`/`gofmt` and the full tool suite.

See [`logs/M0-proof.txt`](logs/M0-proof.txt) and
[`logs/make.bash-haiku-arm64.log`](logs/make.bash-haiku-arm64.log).

Running a binary on real Graviton hardware is **M1** and is not claimed here.

## How to reproduce (from a Linux host)

```sh
# 1. A recent upstream Go as the bootstrap (>= go1.24.6). Go cross-builds its
#    own toolchain without cgo, which sidesteps the bootstrap chicken-and-egg.
export GOROOT_BOOTSTRAP=/path/to/go1.25.x
export GOTOOLCHAIN=local            # the fork's go.mod pins a toolchain line

# 2. The fork + this patchset.
git clone -b go1.26.1-haiku1 https://github.com/korli/go korli-go
cd korli-go && git apply /path/to/patches/0001-haiku-arm64-M0-toolchain.patch

# 3. Cross-build the toolchain for the target.
cd src && GOOS=haiku GOARCH=arm64 CGO_ENABLED=0 ./make.bash

# 4. (Optional) produce the arm64 bootstrap tarball.
cd src && GOOS=haiku GOARCH=arm64 CGO_ENABLED=0 ./bootstrap.bash
```

CGO stays disabled for M0 (`CGO_ENABLED=0`) — no cgo bridge to `libroot` is
needed, matching the target-application plan in the scope doc.

## What the patchset adds

The OS layer of the Haiku port is architecture-neutral (the `poll(2)`-based
netpoller reaches `libroot.so` dynamically); only the **arm64 arch half** was
missing. Modelled on the existing `*_haiku_amd64.*` files (Haiku conventions)
crossed with the mature `*_arm64` backend and the Darwin/OpenBSD arm64 libc
ports (arm64 ABI). All libroot calls go through the Solaris-style
`asmsysvicall6` dispatcher; `g` lives in `R28`.

New arm64 arch files:

| File | Role |
|---|---|
| `src/runtime/sys_haiku_arm64.s` | libroot dispatcher (`asmsysvicall6`), `miniterrno`, `pipe1`, thread entry `tstart_sysvicall`, `sigtramp`→`sigtrampgo`, `sigfwd`, `usleep1`/`osyield1` |
| `src/runtime/rt0_haiku_arm64.s` | program/`c-shared` entry points |
| `src/runtime/defs_haiku_arm64.go` | OS constants + arm64 `mcontext`/`ucontext` matching Haiku `struct vregs` |
| `src/runtime/signal_haiku_arm64.go` | `sigctxt` register accessors (elr→pc, spsr→pstate, fault from siginfo) |
| `src/runtime/os_haiku_arm64.go` | `cputicks()` (nanotime-based, like the other arm64 libc ports) |
| `src/syscall/{asm,syscall,zerrors,zsyscall,zsysnum,ztypes}_haiku_arm64.go/.s` | syscall shim + generated tables |
| `src/cmd/vendor/golang.org/x/sys/unix/*_haiku_arm64.*` | vendored x/sys/unix arm64 tables (needed by `cmd`) |

Wiring changes to existing files:

- `src/cmd/dist/build.go`, `src/internal/platform/zosarch.go` — register the
  `haiku/arm64` port (cgo off by default).
- `src/runtime/signal_arm64.go` — add `haiku` to the shared arm64 signal
  handler's GOOS list.
- `src/runtime/tls_arm64.h` — Haiku reads the thread pointer from `TPIDR_EL0`.
- `src/cmd/link/internal/ld/sym.go` — haiku/arm64 TLS offset.
- `src/cmd/link/internal/arm64/obj.go` — accept `-H haiku` (ELF) and set the
  `/system/runtime_loader` interpreter.
- `src/cmd/link/internal/arm64/asm.go` — **key linker change.** Every libroot
  service is reached by taking the address of a `cgo_import_dynamic` function
  pointer, which emits `R_ADDRARM64` against a dynamic-import symbol. The
  internal linker only rewrote that to a GOT load for Darwin; the ELF (Haiku)
  path now does the same via an `R_AARCH64_GLOB_DAT` GOT entry.

### Real vs. stubbed

- **Real / structural:** the dispatcher, thread bring-up, signal-context
  decode, defs, syscall tables, and all linker wiring are complete and the
  emitted arm64 assembly disassembles correctly (LR auto-saved, correct
  `libcall` struct offsets, args in R0–R5, `g` in R28).
- **Simplifications, correct for M0, to re-verify on hardware (M1):**
  - `sigtramp` uses the modern `sigtrampgo` path (as OpenBSD/Darwin arm64 do)
    rather than the amd64 port's Solaris-era manual `m.libcall` save/restore.
  - `usleep1` calls libroot directly (its only caller, `usleep_no_g`, runs
    without a g); the g-bearing path goes through `sysvicall1`.
  - `zsysnum_haiku_arm64.go` was copied from the amd64 table. Haiku reaches the
    kernel through libroot, not raw syscall numbers, so these constants are
    largely cosmetic — but they should be regenerated from the arm64 libroot
    `syscalls.S.inc` before relying on any raw-syscall value.
  - The arm64 `mcontext`/`ucontext` offsets are hand-derived from the Haiku
    arm64 `struct vregs`; register decode in a signal handler is exercised only
    on hardware (M1).

## Next step (M1)

Boot a Graviton Haiku instance and run the cross-compiled hello-world: it must
print and exit 0. Then M2 (net/http + goroutines + netpoller under load). The
arm64 bootstrap tarball must be hosted in the DeBeOS artifact store and its URL
filled into `golang-1.26.1.recipe` (`SOURCE_URI_3`).
