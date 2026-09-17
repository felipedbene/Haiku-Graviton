# Go toolchain bring-up on DeBeOS (Haiku arm64) — scoping

**Status:** **M0 DONE** (cross-build proven, 2026-09-17) — the rest is still
scoping. A `GOOS=haiku GOARCH=arm64` gc Go toolchain now cross-builds from a
Linux host from the `korli/go` fork plus a small arm64 patchset; a trivial
program cross-compiles to a AArch64 Haiku ELF and an arm64 bootstrap tarball
was produced. Artifacts, patchset, overlay recipe and proof logs:
[`../go-arm64/`](../go-arm64/). Running on hardware is M1 and not yet done. This
document decides *whether and how* to bring the Go toolchain to DeBeOS on
Graviton (arm64), with the concrete end goal of compiling the upstream
**`amazon-ssm-agent`** (github.com/aws/amazon-ssm-agent, Apache-2.0, written in
Go) so it can eventually replace DeBeOS's current custom native
`debeos-ssm-agent`.

**Bottom line up front.** A Go/arm64 toolchain for DeBeOS is *tractable* and
follows almost exactly the pattern that already worked for Rust here: the
hard, generic part (the OS port) already exists upstream-of-us, and only the
**arch half** (Haiku × arm64) is missing. The recommended path is **gc Go
(the official compiler), by extending the existing Haiku Go fork to arm64** —
**not** gccgo. But the real `amazon-ssm-agent` is a large codebase and porting
it is a second, separate project on top of the toolchain. The custom
`debeos-ssm-agent` **already works end-to-end** (registration, Run Command,
Session Manager, patch manager — all hardware-proven), so the honest verdict is:
**pursue the Go toolchain as its own valuable goal; keep shipping
`debeos-ssm-agent` in the meantime; treat the real-agent port as a stretch to
re-evaluate only after the toolchain is proven.**

---

## 1. Go's Haiku / arm64 reality

### 1.1 Upstream gc Go has no `haiku` GOOS

Confirmed against the official source-install docs (`go.dev/doc/install/source`):
the supported `GOOS` set is `aix, android, darwin, dragonfly, freebsd, illumos,
ios, js, linux, netbsd, openbsd, plan9, solaris, wasip1, windows`. **`haiku`
is not among them.** There is no Haiku support in the upstream golang/go tree,
for any architecture.

### 1.2 …but a maintained Haiku Go *fork* exists (x86_64 only)

HaikuPorts carries `dev-lang/golang` with a current recipe
**`golang-1.26.1.recipe`** (plus a legacy `golang-1.4.3.recipe` kept only for
historical bootstrap). Key facts from the recipe:

- **Source is a Haiku fork, not upstream+patch.** `SOURCE_URI` points at
  `github.com/korli/go` at tag **`go1.26.1-haiku1`**. The `haiku` GOOS lives in
  that fork's tree.
- **`ARCHITECTURES="x86_64"`** — no secondary architectures, **no arm64/aarch64
  anywhere** in the recipe.
- **Bootstrap is a prebuilt amd64 tarball** (`SOURCE_URI_2 =
  go-1.26.1-haiku-amd64-bootstrap.tbz`); `GOROOT_BOOTSTRAP` points at it. There
  is no arm64 bootstrap.

So: Go on Haiku is real and *current* (1.26.1 — note that already exceeds
`amazon-ssm-agent`'s `go 1.25` floor), but it is **x86_64-only**, and the
bootstrap is x86_64-only. arm64 is greenfield.

### 1.3 What the fork patches, and which parts are arch-specific

Inspecting `korli/go` at `go1.26.1-haiku1`:

- **`src/runtime/netpoll_haiku.go`** — the netpoller. It is a **`poll(2)`-based**
  implementation (derived from the AIX poll-based netpoller), and it reaches
  `poll` **dynamically from `libroot.so`** via
  `//go:cgo_import_dynamic libc_poll poll "libroot.so"`. This is the important
  architectural tell: the Haiku Go port does **not** make raw kernel syscalls; it
  calls Haiku's libc (`libroot`) the same way the Darwin/Solaris/AIX ports call
  their libc. Haiku has no stable syscall ABI, so this is the correct and only
  sane approach.
- **`src/runtime/defs_haiku.go`, `defs_haiku_amd64.go`** — OS constants and the
  arch-specific struct/register definitions.
- **`src/syscall/asm_haiku_amd64.s`, `linkname_haiku.go`, `mksyscall_haiku.pl`,
  `exec_haiku_test.go`** — the syscall shim: arch-specific asm stubs that marshal
  into `libroot`, an OS-neutral linkname list, and the generator script for the
  per-arch `zsyscall_haiku_*.go` / `ztypes_haiku_*.go` tables.

The split is clean and mirrors what we learned porting Rust
(`[[rust-cross-compiles-to-haiku-arm64]]`): the **OS layer is arch-neutral**
(the netpoller logic, `os`, most of `syscall`, the linknames), and only a small
**arch layer** is amd64-specific. The arm64 gap is therefore:

| Needs writing for haiku/arm64 | Analogue that already exists (amd64) |
|---|---|
| `src/syscall/asm_haiku_arm64.s` (libroot call stubs) | `asm_haiku_amd64.s` |
| `src/runtime/defs_haiku_arm64.go` (regs, sigcontext) | `defs_haiku_amd64.go` |
| `src/runtime/sys_haiku_arm64.s`, `signal_haiku_arm64.go` (thread/signal/g-register setup) | amd64 equivalents |
| regenerated `zsyscall_haiku_arm64.go`, `ztypes_haiku_arm64.go` | amd64 generated files (rerun `mksyscall_haiku.pl` + cgo `-godefs`) |
| an arm64 **bootstrap** for haiku/arm64 | prebuilt amd64 bootstrap tarball |

Everything else (the netpoller, `net`, `os`, the bulk of `runtime` and
`syscall`) is shared and already Haiku-aware. The arm64 *backend* of gc Go is
fully mature upstream (`asm_arm64.s`, `atomic_arm64.s`, `cpuflags_arm64.go`,
etc. all exist); as with Rust, only the **Haiku × arm64 combination** is absent.

---

## 2. Two toolchain paths

### 2.1 gc Go (official compiler) — RECOMMENDED

**Approach:** fork-of-a-fork — extend `korli/go @ go1.26.1-haiku1` with the
arm64 arch files listed above, then **cross-build the toolchain from a working
Go on a Linux/arm64 host** targeting `GOOS=haiku GOARCH=arm64`. Go's toolchain
cross-compiles cleanly (it is itself pure Go once bootstrapped), which
**sidesteps the bootstrap chicken-and-egg**: you never need Go running on Haiku
to *produce* Go for Haiku — exactly the insight that made the Rust bring-up
cheap (build *for* Haiku *from* Linux).

**Netpoller:** already solved by the fork. Haiku offers POSIX `poll(2)` and
`select(2)` via `libroot`; it has **no `epoll`/`kqueue` equivalent** (there is a
Haiku-native `wait_for_objects()`, but the fork does not use it). `poll` scales
worse than `epoll`/`kqueue` at very high fd counts, but `amazon-ssm-agent` holds
only a handful of long-lived connections (a control-channel websocket, an MDS
poll loop), so a poll-based netpoller is entirely adequate. This is **not** a
from-scratch design item — `netpoll_haiku.go` exists and is arch-neutral.

**Bootstrap:** produce a `go-<ver>-haiku-arm64-bootstrap` tarball once (cross
from Linux), then it self-hosts. This is the one genuinely new artifact.

**Version:** the fork is at 1.26.1 ≥ ssm-agent's `go 1.25` floor. No version gap.

### 2.2 gccgo (GCC Go frontend) — NOT recommended

DeBeOS already has a working arm64 GCC cross-toolchain, so "reuse GCC's backend +
libgo" looks attractive on paper. It fails on two independent counts:

1. **Language/library version.** gccgo tracks well behind upstream gc. The
   gofrontend/`libgo` shipped with recent GCC corresponds to roughly the **Go
   1.18** language and standard library — far short of the **`go 1.25`** that
   `amazon-ssm-agent`'s `go.mod` demands. gccgo cannot build the agent
   regardless of any OS work. *(Exact gccgo↔Go mapping for the specific GCC
   version DeBeOS ships needs confirmation, but no released gccgo is anywhere
   near 1.25.)*
2. **libgo still needs a Haiku port.** gccgo doesn't get Haiku support for free
   from GCC's backend — its runtime, `libgo`, has its own OS-portability layer
   and there is **no upstream Haiku target in libgo**. Porting libgo to
   haiku/arm64 is comparable in effort to the gc runtime port — so gccgo costs
   *more* (a fresh libgo port) and delivers *less* (a Go version too old to
   compile the target).

**Verdict:** gc Go wins decisively. The OS port already exists (the fork), only
the arch half is missing, the version is already sufficient, and cross-building
from Linux removes the bootstrap wall. gccgo would mean a from-scratch libgo
port to reach a Go version that still can't build `amazon-ssm-agent`.

---

## 3. `amazon-ssm-agent` requirements and the Haiku gaps

From the public repo (`go.mod`, README):

- **Go version:** `go 1.25`. Satisfied by the fork (1.26.1).
- **cgo:** the README does not mention cgo; AWS ships the agent as static
  per-arch binaries and the Linux/arm64 build is `CGO_ENABLED=0`-friendly. **This
  is the single biggest de-risk for a fresh GOOS** — a pure-Go build needs *no*
  working cgo bridge to `libroot` for the agent itself. *(Needs confirmation that
  no transitive dependency force-enables cgo; the listed deps — aws-sdk-go,
  go-git/v5, gorilla/websocket, mangos/v3, smux, x/crypto, x/net — are all pure
  Go.)*
- **Crypto/TLS:** Go brings its **own pure-Go `crypto/tls`**. This *solves* the
  exact problem that forced `debeos-ssm-agent` to bundle static mbedTLS and a
  hand-rolled HTTP client ("haiku/arm64 has no HTTPS-capable curl",
  `[[rust-cross-compiles-to-haiku-arm64]]`). A genuine architectural win for Go.
- **Linux-isms:**
  - *init/service:* the agent has no hard systemd dependency in its core; it
    integrates per-OS (systemd unit / upstart / launchd / Windows SCM). On Haiku
    it would need a **`launch_daemon` job** — which `debeos-ssm-agent` **already
    provides** and can be reused verbatim.
  - */proc, dmidecode, netlink:* used by the **inventory/gatherer plugins**
    (`platform`, `network`, `instance-detailed-information`, etc.), not by the
    core control path. On Haiku these would return empty or need small Haiku
    equivalents; they **degrade rather than block** the agent.
  - *kernel:* the stated Linux floor ("kernel 3.2+") is a Linux packaging note,
    not a portability constraint.
- **Goroutine / networking load:** modest — a control-channel websocket, an MDS
  long-poll, a few worker goroutines per command. Well within a poll-based
  netpoller.
- **Registration:** IMDS role creds → SSM registration / control channel. This
  is precisely what `debeos-ssm-agent` **already does natively and proven**
  (`[[native-ssm-mgmt-agent-proven]]`: Online, Run Command, cold-boot
  auto-register on real Graviton Haiku).

### 3.1 What `debeos-ssm-agent` already solves that the real agent would need Haiku equivalents for

The custom agent is not a throwaway — it has already paid down most of the
platform-integration cost, and a real-agent port would need to re-supply each:

| Concern | `debeos-ssm-agent` today | Real agent on Haiku would need |
|---|---|---|
| HTTPS on haiku/arm64 | static mbedTLS + hand-rolled HTTP/1.1 | Go's own `crypto/tls` — *solved by the language* |
| Boot clock (1970) fix | IMDS `Date` header | same shim, or reuse |
| Service autostart | `launch_daemon` job | reuse the same launch job |
| SSM node registration | native, proven | same MDS/control-channel flow (Go SDK) |
| Run Command / Session Manager / Patch Manager | implemented + hardware-proven | provided by the real agent (that's the point) |
| `PlatformType` enum | reports `Linux` (closed enum); Name/Version report truth | identical constraint |
| Inventory gatherers | (minimal) | Haiku stubs for /proc-style gatherers |

---

## 4. Milestone ladder + risks

Each milestone has a binary pass/fail check. Milestones are cumulative.

| # | Milestone | Pass/fail check |
|---|---|---|
| **M0** ✅ | Cross-build a haiku/arm64 gc Go toolchain: add arm64 arch files to the fork; produce a `go-<ver>-haiku-arm64-bootstrap`; `GOOS=haiku GOARCH=arm64` `make.bash` succeeds from a Linux host | **DONE** — `make.bash` builds "packages and commands for target, haiku/arm64"; `std` + `cmd` cross-build; a program cross-compiles to a AArch64 Haiku ELF; `go-1.26.1-haiku-arm64-bootstrap.tbz` produced. See [`../go-arm64/`](../go-arm64/) |
| **M1** | Hello world | a cross-compiled `haiku/arm64` binary runs on a Graviton Haiku instance, prints, exits 0 |
| **M2** | `net/http` + goroutines + netpoller | an HTTPS GET returns 200; N concurrent goroutines + connections complete under load with no netpoller hang |
| **M3** | cgo (only if needed) | a cgo "hello" calling a `libroot` function links and runs — **skip if the agent builds `CGO_ENABLED=0`** |
| **M4** | `amazon-ssm-agent` compiles for haiku/arm64 | binary is produced (expect a few `//go:build` GOOS arms + a service-integration shim) |
| **M5** | It runs | agent starts, reads config, does not crash on init |
| **M6** | Registers + Run Command | `describe-instance-information` shows Online; `send-command AWS-RunShellScript` succeeds — the bar `debeos-ssm-agent` already meets |

### Highest-risk items

1. **The arm64 arch port of the runtime (M0)** — the make-or-break. Writing
   `asm_haiku_arm64.s`, `sys_haiku_arm64.s`, `signal_haiku_arm64.go`,
   `defs_haiku_arm64.go`: the g-register convention, signal/`ucontext` layout,
   and the libroot call stubs are fiddly runtime asm. **Medium-high risk**,
   de-risked by (a) the amd64 files as a direct template and (b) the Rust
   precedent that the arch/OS split is clean and the arm64 backend is mature.
2. **Bootstrap chicken-and-egg (M0)** — mitigated: Go cross-compiles its own
   toolchain from Linux without cgo, so this is a *build-recipe* problem, not a
   wall, **provided the fork compiles for the haiku/arm64 combination.**
3. **cgo on a fresh GOOS/arch (M3)** — real work if required (needs the arm64
   gcc bridge with the right calling convention), but **likely avoidable** if the
   agent and its deps build `CGO_ENABLED=0`. Confirm early.
4. **Netpoller correctness under load (M2)** — poll-based, arch-neutral, already
   written; risk is integration bugs surfacing on arm64, not design.
5. **ssm-agent Linux gatherers / service integration (M4–M6)** — ordinary
   porting; gatherers degrade to empty, service integration reuses the existing
   launch job. Low technical risk, non-trivial volume.

### Honest effort estimate

- **Toolchain (M0–M2):** the expensive 80% (the OS port) is *already done* by the
  fork. The arm64 arch port is a focused runtime-asm task — realistically
  **a few weeks**, with genuine tail risk in the signal/g-register asm that could
  stretch it if the arm64 stubs fight back.
- **Real agent (M4–M6):** a further **several weeks to a couple of months** of
  ordinary porting (service shim, gatherer stubs, GOOS build tags), plus ongoing
  maintenance to track upstream.

Do not undersell it: a *working amazon-ssm-agent that registers* is a
**multi-week-to-multi-month** effort in total. The *toolchain alone* is the
cheaper, sooner, and independently valuable milestone.

---

## 5. Verdict — is this worth it?

**The Go toolchain: yes, on its own merits.** A gc Go/arm64 toolchain unblocks a
whole class of Go software on DeBeOS (not just the SSM agent — cf. the native
AWS CLI and CloudWatch-agent asks, issues #120/#119), and it removes the "no
HTTPS-capable curl" tax that forced hand-rolled TLS. It follows a pattern we've
already executed once (Rust). Recommend pursuing it as a first-class goal.

**Replacing `debeos-ssm-agent` with the real agent: not now.** The custom agent
already delivers the end-to-end capability (register, Run Command, Session
Manager, patch manager — hardware-proven). The real agent buys **feature parity
+ maintainability + upstream feature tracking**, not a new capability, and it
brings a large, Linux-shaped codebase with its own porting and maintenance
burden.

**Recommended sequencing / interim:**

1. **Keep `debeos-ssm-agent` as the shipping fleet agent.** It works; don't gate
   fleet control on the port.
2. **Build the Go/arm64 toolchain** (M0–M2) as its own tracked effort. Prove
   hello-world + net/http + netpoller on real Graviton Haiku.
3. **Only then re-evaluate** the real-agent port (M4–M6) against the cost of
   continuing to maintain the custom agent. A likely outcome: land the toolchain,
   compile a couple of small Go tools first (AWS CLI-v2 is Python, but e.g. a Go
   CloudWatch shim or `session-manager-plugin` are candidates), and adopt the
   real `amazon-ssm-agent` only if/when custom-agent maintenance outweighs the
   port + upkeep.

---

## References

- Upstream Go supported GOOS/GOARCH: `go.dev/doc/install/source`
- Haiku Go port: HaikuPorts `dev-lang/golang/golang-1.26.1.recipe`; fork
  `github.com/korli/go` tag `go1.26.1-haiku1`
- `amazon-ssm-agent`: `github.com/aws/amazon-ssm-agent` (`go.mod` → `go 1.25`;
  Apache-2.0)
- Rust precedent (closest analogue — arch/OS split, cross-from-Linux, split-libc
  seams): memory `[[rust-cross-compiles-to-haiku-arm64]]`
- Existing native agent (what this would replace): memory
  `[[native-ssm-mgmt-agent-proven]]`; `debeos-ssm-agent`
- Related issues: #120 (native AWS CLI), #119 (native CloudWatch agent), #116
  (crate→hpkg toolchain), #35 (builder AMI), #58 (distro-convergence EPIC)
