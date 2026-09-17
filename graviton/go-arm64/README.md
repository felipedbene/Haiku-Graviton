# Go toolchain for DeBeOS (haiku/arm64) — M0 + M1

This directory holds the DeBeOS-side artifacts for bringing the Go toolchain to
haiku/arm64 (AWS Graviton). The design and milestone ladder live in
[`../docs/go-arm64-bringup-scope.md`](../docs/go-arm64-bringup-scope.md). **M0**
is *a `GOOS=haiku GOARCH=arm64` gc Go toolchain that cross-builds from a Linux
host*; **M1** is *a cross-compiled binary that runs on real Graviton Haiku
hardware, prints, and exits 0* — and validates the M0 runtime simplifications.

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

## Status: M1 DONE (runs on real Graviton hardware)

Five cross-compiled `haiku/arm64` binaries (sources in [`tests/`](tests/)) were
pushed to a Graviton `c7g.large` running DeBeOS/Haiku arm64 (hrev59996) over SSM
and executed. All exit 0 — full transcript in [`logs/M1-proof.txt`](logs/M1-proof.txt):

| Test | Result | What it exercises |
|---|---|---|
| `hello.go` — `println("hi")` | **`hi`, exit 0** — the M1 must-pass gate | program entry, runtime init |
| `fmthello.go` — `fmt.Println` | `hello from haiku/arm64`, exit 0 | fmt + buffered stdout via libroot |
| `chans.go` — 8 goroutines + channel + `WaitGroup` | `goroutine-sum 140` (correct), exit 0 | scheduler, goroutines, channels |
| `syscalls.go` — `time.Sleep`+`getpid`+`write(2)` | `syscall-write-ok` / `pid=950 slept=200ms`, exit 0 | timer, getpid, write via the libroot dispatcher |
| `sigtest.go` — nil-deref → SIGSEGV → recover | `recovered-from-signal: … nil pointer dereference`, exit 0 | **sigtramp → sigtrampgo → sigpanic + mcontext decode** |

The bootstrap tarball is hosted (see `SOURCE_URI_3` in the recipe): a `GET` of
that URL returns the exact `sha256=2fe368b4…d667` tarball.

## Status: M2 DONE (net/http + TLS + concurrent netpoll on hardware)

A single combined harness (`tests/m2/combined/main.go`; the three subtests also
stand alone as `tests/m2/{httpsget,concurrent,serverload}.go`) was cross-compiled
`CGO_ENABLED=0`, pushed to a Graviton `c7g.large` (Haiku hrev59996) over SSM, and
run. Full transcript — including the pre-fix failure — in
[`logs/M2-proof.txt`](logs/M2-proof.txt):

| Test | Result | What it exercises |
|---|---|---|
| `httpsget` — real HTTPS GET | **200, TLS 1.3 (cipher 0x1301, ALPN h2), 3-cert chain verified, exit 0** — the M2 must-pass gate | `crypto/tls` (pure-Go, the mbedTLS replacement), pure-Go DNS over UDP, CA bundle load, single-conn netpoll |
| `concurrent` — 50 goroutines × 4 external HTTPS conns | **200/200 ok, no hang** | poll(2) netpoll readiness under concurrent TLS load (the epoll/kqueue-less path) |
| `serverload` — in-proc `http.Server` + 100 goroutines × 20 | **2000/2000 correct checksums, no hang** | accept/read/write readiness, no external dependency |

**Netpoller bug found and fixed (the headline M2 finding).** The first
on-hardware run failed *everything* — DNS `read: operation would block` and TCP
`connect: operation now in progress`, both returned instantly instead of waiting
on the poller. Root cause was not the netpoll logic but the arm64 libroot
dispatcher in `sys_haiku_arm64.s`: it read errno with a **signed** `MOVW` load,
sign-extending Haiku's bit-31-set errno values (all `B_GENERAL_ERROR_BASE`-
relative) to `0xffffffff_8000000b`, so `internal/poll`'s `err == syscall.EAGAIN`
/ `EINPROGRESS` guards — which compare against the 32-bit-masked constant
`0x00000000_8000000b` — never matched, and non-blocking I/O never parked on
netpoll. One-instruction fix (`MOVW`→`MOVWU`, matching amd64's zero-extending
`MOVL`): [`patches/0002-haiku-arm64-M2-errno-zero-extend.patch`](patches/0002-haiku-arm64-M2-errno-zero-extend.patch).
Hardware A/B on that single change: `serverload` 0/200 → 2000/2000. Latent
through M1 because `Errno.Error()` re-masks to 32 bits (strings looked right) and
no M1 test did a non-blocking `== syscall.Exxx` comparison.

## How to reproduce (from a Linux host)

```sh
# 1. A recent upstream Go as the bootstrap (>= go1.24.6). Go cross-builds its
#    own toolchain without cgo, which sidesteps the bootstrap chicken-and-egg.
export GOROOT_BOOTSTRAP=/path/to/go1.25.x
export GOTOOLCHAIN=local            # the fork's go.mod pins a toolchain line

# 2. The fork + this patchset.
git clone -b go1.26.1-haiku1 https://github.com/korli/go korli-go
cd korli-go && git apply /path/to/patches/0001-haiku-arm64-M0-toolchain.patch
git apply /path/to/patches/0002-haiku-arm64-M2-errno-zero-extend.patch

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
- **M0 simplifications — re-verified on hardware in M1 (see `logs/M1-proof.txt`):**
  - `sigtramp` (modern `sigtrampgo` path, as OpenBSD/Darwin arm64 do) —
    **VALIDATED.** `sigtest.go` forced a real SIGSEGV; the runtime caught it,
    ran `sigtramp → sigtrampgo → sigpanic`, and recovered cleanly.
  - The arm64 `mcontext`/`ucontext` offsets, hand-derived from Haiku's arm64
    `struct vregs` — **VALIDATED** by the same test: the handler decoded the
    correct fault and synthesised the exact nil-pointer panic. A wrong offset
    would have mis-decoded the fault (double-fault / hang / garbage panic).
  - `usleep1` / the g-bearing sleep through `sysvicall1` — **VALIDATED**
    (`syscalls.go`: `time.Sleep(200ms)` measured `slept=200ms`).
  - `zsysnum_haiku_arm64.go` copied from the amd64 table — **not exercised as
    raw syscall numbers, by design.** Haiku reaches the kernel through libroot,
    so `getpid`/`write` went through the dispatcher, not raw SVC. The table
    stays cosmetic; regenerating it from the arm64 libroot `syscalls.S.inc`
    remains a correctness-hygiene follow-up, not a blocker.

## Next step (M2)

M0 (cross-build) and M1 (runs on hardware) are done. Next is **M2**: an HTTPS
`net/http` GET returning 200 plus N concurrent goroutines/connections under load
with no netpoller hang — the first real exercise of the poll-based
`netpoll_haiku.go` on arm64.
