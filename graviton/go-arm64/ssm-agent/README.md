# amazon-ssm-agent — haiku/arm64 port (M3 + M4 + M5 + M6 + M7)

> **M7 (2026-09-18): EC2-native registration gate CLEARED + packaged as an hpkg.**
> The real agent registered **EC2-native** (an `i-` node via IMDS + instance
> role, *no* hybrid activation) on a real Graviton `c7g.large`, reported
> AgentVersion `3.3.0.0` / ResourceType `EC2Instance`, and served
> `AWS-RunShellScript` (Status Success, RC 0, real stdout, n=2). It is packaged as
> a zlib `amazon_ssm_agent-3.3.3270.0-1-arm64.hpkg` whose bundled `launch_daemon`
> job starts it EC2-native on cold boot (proven). Full detail, the build script,
> and the bake-wiring + `debeos-ssm-agent` fallback plan are in
> [`package/`](package/). This is the productionization step toward baking the
> real agent as the fleet SSM agent (issue #302).


M3/M4/M5 of the "Go on DeBeOS/arm64" arc (see `../../docs/go-arm64-bringup-scope.md`).
M3/M4 got the real upstream `amazon-ssm-agent` to *compile + link* for
haiku/arm64; **M5 got it to RUN and reach `Online` as an SSM managed node.**

**M5 update (2026-09-18): the real agent registered and reached
`PingStatus=Online` on real Graviton (Haiku arm64), reporting its own version
`3.3.0.0`, `PlatformName=Haiku`.** It ran as a hybrid-activation `mi-` node
(distinct identity from the box's baked `debeos-ssm-agent`). Everything up to
and including the SSM control channel and health heartbeat works; **command
execution (Run Command / Session Manager / association documents) is blocked by
one toolchain bug — `fork`+`exec` faults on haiku/arm64** (blocker **B1**). Full
runtime-blocker ledger and evidence: [`../logs/M5-proof.txt`](../logs/M5-proof.txt)
and the "M5 — runtime" section at the end of this file. The single combined
`amazon-ssm-agent-haiku-arm64.patch` now also carries the M5 agent-side arms
(platform-type enum, exec-free platform/hostname/fingerprint, UDP-dial IP).

**M6 update (2026-09-18): B1 is FIXED — Run Command executes.** The fork+exec
fault was a latent arm64 bug in the *fork's* `runtime.pipe1` (the return address
is spilled at `0(RSP)`, which `pipe()` then overwrote with a file descriptor;
the `RET` jumped to it and faulted). Fixed in the toolchain by routing
`syscall.Pipe` through the generic libroot dispatcher instead of the `pipe1`
shim (`../patches/0004-haiku-arm64-M6-pipe-forkexec.patch`). With that toolchain,
the same agent binaries reach Online *and* the core spawns `ssm-agent-worker`
(fork+exec), and an `AWS-RunShellScript` to the `mi-` node returns
**Status Success, RC 0** with real stdout — the functional milestone M5 could not
reach. Proof: [`../logs/M6-proof.txt`](../logs/M6-proof.txt). Deployment note:
launch the agent with a PATH that includes `/boot/system/bin` (Haiku's `sh`),
else the now-healthy fork+exec of `ssm-document-worker` reaches `execve` and
returns a clean "sh not found in $PATH".

**M4 update (2026-09-18):** the three genuine toolchain gaps M3 surfaced
(`syscall.Flock`, `syscall.Statfs`, and an `Uname` for the detailed-info
gatherer) are now wired into the Haiku Go port (fork patch
`../patches/0003-haiku-arm64-M4-flock-statfs-uname.patch`), and the agent's
degraded stubs are replaced with the real syscalls. `go build ./...` still exits
0. Full M4 ledger and proof: [`../logs/M4-proof.txt`](../logs/M4-proof.txt) and
the "M4 — gaps closed" section at the end of this file.

## Result

**`GOOS=haiku GOARCH=arm64 CGO_ENABLED=0 go build ./...` exits 0** for the full
`amazon-ssm-agent` module, and all **eight** release binaries link as AArch64
Haiku ELF executables (`interpreter /system/runtime_loader`, `NEEDED
libroot.so`):

`amazon-ssm-agent`, `ssm-agent-worker`, `updater`, `ssm-cli`,
`ssm-document-worker`, `ssm-session-logger`, `ssm-session-worker`,
`ssm-setup-cli`.

Evidence: [`../logs/M3-proof.txt`](../logs/M3-proof.txt).

This is a **compile + link** milestone. On-hardware start / registration /
Run Command are the next milestones (M5/M6) and are not claimed here.

## How to reproduce (from a Linux host)

```sh
# 1. A haiku/arm64 gc Go toolchain (the korli/go fork + the M0/M2 patchset in
#    ../patches/). "go version" must report go1.26.1; sys_haiku_arm64.s must
#    carry the M2 errno fix (MOVWU, patches/0002).
export GOROOT=/path/to/korli-go        # after GOOS=haiku GOARCH=arm64 ./make.bash
export PATH=$GOROOT/bin:$PATH

# 2. Upstream agent at the pinned tag (ships its own vendor/ tree).
git clone --depth 1 --branch v3.3.3270.0 https://github.com/aws/amazon-ssm-agent
cd amazon-ssm-agent
git apply /path/to/amazon-ssm-agent-haiku-arm64.patch

# 3. Cross-build the whole module, fully offline.
GOOS=haiku GOARCH=arm64 CGO_ENABLED=0 GOTOOLCHAIN=local GOFLAGS=-mod=vendor \
    go build ./...
```

Verified: a pristine `v3.3.3270.0` clone + `git apply` of the patch +
`go build ./...` returns exit 0 and produces an AArch64 Haiku ELF.

- **Tag:** `v3.3.3270.0` (latest release at the time of writing).
- **`go.mod` floor:** `go 1.24` — satisfied by the fork's 1.26.1.
- **cgo:** off. No cgo bridge to `libroot` is needed; `crypto/tls` (pure-Go) and
  the pure-Go DNS resolver cover the network path (proven in M2).

## What the patch does — GOOS-arm ledger

94 files. Three kinds of change; none required touching the Go toolchain for M3
(the fork already carries the OS layer and the M2 errno fix).

### 1. Reuse the existing unix path (87 files, build-tag only)

Haiku is POSIX-shaped and the fork places `haiku` in Go's `unix` build-tag set,
so the vast majority of the agent's `_unix.go` files compile unchanged. These
files only had `haiku` appended to their `//go:build … freebsd || linux || …`
constraint (and the legacy `// +build` line). They fall into: appconfig / path
constants, process control (`SysProcAttr{Setpgid}`, `syscall.Kill`,
`syscall.Sync`, `syscall.Credential`), platform-info and inventory gatherers,
session/shell/pty (`syscall.Syscall(SYS_IOCTL, …)` — the fork provides the
generic `Syscall` shim and `SYS_IOCTL`), TLS cert-pool, logging, the setup-cli
service/package managers, and the vendored `go-git` worktree (`_unix_other`).
The `serialport` and `startup` packages join their existing **skip/stub**
variant (the non-Linux no-op path) rather than the real one.

### 2. New Haiku arms where a unix syscall is genuinely absent (5 files)

| File | Why | Approach |
|---|---|---|
| `agent/fileutil/advisorylock/advisorylock_haiku.go` | Haiku libroot has no `flock(2)`; `syscall.Flock`/`LOCK_*` are not wrapped | **Real** impl via `syscall.FcntlFlock(F_SETLK)` + `Flock_t` (all present for haiku/arm64) |
| `vendor/github.com/go-git/go-billy/v5/osfs/os_haiku.go` | Same: `os_posix.go` uses `golang.org/x/sys/unix` `Flock`/`LOCK_EX`, which the agent's vendored x/sys/unix has no haiku port for | **Real** `FcntlFlock` locks; `rename`/`umask` identical to POSIX arm. `os_posix.go` gets `&& !haiku` |
| `agent/fileutil/fileutil_diskspace_haiku.go` (+ `..._statfs.go`) | `syscall.Statfs`/`Statfs_t` not wrapped for haiku | `GetDiskSpaceInfo` split out: statfs version keeps the old unix set; **haiku returns a 1 TiB sentinel** so pre-download space guards don't spuriously block. *Degraded* — TODO: back with `statvfs`. |
| `vendor/github.com/fsnotify/fsnotify/haiku.go` | fsnotify v1.5.1 has backends for inotify/kqueue/FEN/Windows only — none for Haiku | **Stub** `Watcher` (open, idle channels; `Add`/`Remove` no-op). *Degraded* — never fires events. TODO: back with Haiku `watch_node`. |
| `agent/plugins/inventory/gatherers/instancedetailedinformation/dataProvider_haiku.go` | The unix gatherer parses `lscpu` and calls `unix.Uname` (`golang.org/x/sys/unix`, no haiku port) | **Degraded** gatherer returns empty detailed-CPU info; not on the control path. TODO: populate from `get_system_info`. |

### 3. One pre-existing file edited in the body (1 file)

`agent/fileutil/fileutil_unix.go` — `GetDiskSpaceInfo` (and its `syscall`
import) moved out into the two split files above; `haiku` added to its tag so
the remaining portable helpers are shared.

## Genuine toolchain gaps surfaced (not blockers for M3, follow-ups for the fork)

These are absences in the **fork's** `syscall` / vendored `x/sys/unix` for
haiku/arm64. Each is worked around above; a proper fix belongs in the toolchain,
not the agent:

1. **`syscall.Flock` / `LOCK_*`** — not wrapped (libroot has fcntl locking, which
   `FcntlFlock` already exposes). Low effort to add a `Flock` shim.
2. **`syscall.Statfs` / `Statfs_t`** — not wrapped. Needs a `statvfs`/`fs_stat_dev`
   binding.
3. **`golang.org/x/sys/unix` has no `*_haiku_arm64` tables** in the agent's
   vendored copy (the toolchain's own `cmd/vendor` copy does, from M0). Anything
   importing `x/sys/unix` (`unix.Uname`, `unix.Flock`) needs those tables
   generated; only 1 agent file + 1 vendored dep hit it, both worked around.

None of these block the compile+link milestone.

## M4 — gaps closed

Gaps 1 and 2 are now **real syscalls in the fork** (patch
`../patches/0003-haiku-arm64-M4-flock-statfs-uname.patch`); gap 3's single
caller is de-stubbed via `syscall.Uname`, and the full `x/sys/unix` port is
deferred with reason. `GOOS=haiku GOARCH=arm64 CGO_ENABLED=0 go build ./...`
still exits 0; all eight binaries still link (proof:
[`../logs/M4-proof.txt`](../logs/M4-proof.txt)).

| Gap | M3 state | M4 state |
|---|---|---|
| `syscall.Flock` + `LOCK_*` | worked around with `FcntlFlock`; `LOCK_*` actually already present | **REAL.** `Flock` wraps libroot `flock(2)` (which *does* exist — the M3 "no flock(2)" note was wrong: see `headers/posix/sys/file.h`, `src/system/libroot/posix/sys/flock.c`). |
| `syscall.Statfs` / `Statfs_t` | 1 TiB sentinel | **REAL.** `Statfs`/`Fstatfs` wrap `statvfs(3)`/`fstatvfs(3)`; `Statfs_t` mirrors `struct statvfs` (88 bytes, compile-time asserted). |
| `x/sys/unix` `Uname` (gatherer) | empty stub | **REAL for the one caller.** `syscall.Uname` wraps `uname(2)`; the gatherer fills `KernelVersion`. Full `x/sys/unix` haiku/arm64 port **deferred** (a whole new-GOOS generated-table effort — `mkall.sh`/`mksyscall`/`cgo -godefs` against Haiku headers — disproportionate to the single `unix.Uname` call it was needed for). |

Agent changes made by M4 (folded into `amazon-ssm-agent-haiku-arm64.patch`):

| File | M3 | M4 |
|---|---|---|
| `agent/fileutil/advisorylock/advisorylock_haiku.go` | new `FcntlFlock` file | **deleted** — upstream `advisorylock_unix.go` now covers haiku (its build tag gains `haiku`; it uses `syscall.Flock` + `LOCK_*`) |
| `vendor/.../go-billy/v5/osfs/os_haiku.go` | `FcntlFlock(F_SETLKW)` | `syscall.Flock(LOCK_EX/UN)` — mirrors `os_posix.go` exactly |
| `agent/fileutil/fileutil_diskspace_haiku.go` | 1 TiB sentinel | real `syscall.Statfs`; bytes = count × `Frsize` |
| `agent/plugins/inventory/gatherers/instancedetailedinformation/dataProvider_haiku.go` | empty stub | `KernelVersion` from `syscall.Uname`; CPU topology still empty (no `lscpu`; deferred to `get_system_info()`) |

Still stubbed / deferred after M4:

- `vendor/.../fsnotify/fsnotify/haiku.go` — idle `Watcher` stub. Not one of the
  three toolchain gaps; needs a Haiku `watch_node` backend. Deferred.
- CPU topology in the detailed-info gatherer — needs Haiku `get_system_info()`
  (no `lscpu`). Deferred.
- Full `golang.org/x/sys/unix` haiku/arm64 port — deferred (see gap 3 above).

Verified entirely by cross-build on Linux (compile + link + static struct
layout). On-hardware start / registration (M5/M6) is unchanged and not claimed.

## M5 — runtime: registered + Online on real Graviton

On a real Graviton `c7g.large` (Haiku hrev59996, arm64), the real agent
registered and reached **`PingStatus=Online`** as a hybrid-activation `mi-` node
reporting its own version. Observed via `aws ssm describe-instance-information`:

| field | value |
|---|---|
| PingStatus | **Online** |
| AgentVersion | **3.3.0.0** (the real agent) |
| PlatformType | Linux (closed-enum trick, B2) |
| PlatformName | Haiku |
| PlatformVersion | R1~beta6+development (`syscall.Uname`) |
| IPAddress | the instance's private IP (UDP-dial, B3) |

What works: binary runs; RSA identity keygen; on-prem Vault write; fingerprint;
`RegisterManagedInstance` over pure-Go TLS; OnPrem credential refresh; MGS
control-channel websocket; `UpdateInstanceInformation` health pings → Online.

What's blocked: **command execution** (Run Command documents, association
documents, Session Manager pty) and the normal **core→worker spawn** — all need
`fork`+`exec`, which faults on haiku/arm64 (blocker **B1**). To reach Online the
worker was launched directly (`./ssm-agent-worker`), which assumes the OnPrem
identity and heartbeats without spawning a child.

### M5 runtime-blocker ledger (summary; full detail in `../logs/M5-proof.txt`)

| # | What | Layer | Status |
|---|---|---|---|
| **B1** | `fork`+`exec` faults (SIGSEGV/SIGBUS) before the child execs — even for a real binary. Root: `syscall.forkAndExecInChild`→`forkx`→`runtime.syscall_forkx` (`exec_libc.go`, the shared aix/haiku/solaris path) forks via `asmcgocall`; the post-fork child state is invalid for the runtime's libc-call path on haiku/arm64. | **toolchain** | **DEFERRED** — needs a Haiku `forkAndExecInChild` (raw nosplit trampolines, or `posix_spawn`). The make-or-break for Run Command / Session Manager. Days of runtime-asm work. |
| **B2** | `UpdateInstanceInformation` rejects `PlatformType=haiku` (closed enum) → no heartbeat. | agent | **FIXED** — `haiku`→`PlatformType=Linux` in `agent/ssm/service.go`; name/version still report Haiku. |
| **B3** | `net.Interfaces()` returns 0 on Haiku → empty `IPAddress` → ping rejected. | toolchain (worked around in agent) | **WORKED AROUND** — `platformIp_others.go` resolves the egress IP by UDP `connect`+`LocalAddr` (no interface enumeration). Proper fix (implement `net.Interfaces` for Haiku) deferred. |
| **B4** | `getPlatformDetails`/`Hostname` shell out (`lsb_release`, `hostname --fqdn`) → repeating B1 landmines on the health path. | agent | **FIXED** — exec-free via `syscall.Uname` + `os.Hostname()` (`platform_haiku_details.go`, `platform_unix.go`). |
| **B5** | `-register` fingerprint gatherer shells out (`dmidecode`, `ls`) → B1. | agent | **FIXED** — in-process hardware hash (`hardwareInfo_haiku.go`; hostname + net addrs/MACs). |

Notes: the advanced-instances activation tier was **not** needed (MDS accepted
the command on the standard tier; the wall was B1, not the tier). `syscall.Uname`
(compile-only in M4) is now hardware-verified here.

### How to reproduce the runtime (on a Haiku arm64 instance)

```sh
# 0. Build the 8 binaries as in "How to reproduce" above; copy them into a
#    dedicated dir on the instance, e.g. /boot/home/ssm-real/bin (the agent's
#    path logic then uses that dir as its program folder — isolated from any
#    other agent). Create a hybrid activation with an ssm.amazonaws.com-trusted
#    role carrying AmazonSSMManagedInstanceCore:
#      aws ssm create-activation --iam-role <role> --registration-limit 1 ...
# 1. Register (writes the on-prem Vault under /var/lib/amazon/ssm):
./amazon-ssm-agent -register -id <ActivationId> -code <ActivationCode> \
    -region <region> -y
# 2. Run the worker directly to reach Online (core→worker spawn hits B1):
./ssm-agent-worker
# 3. Confirm:
aws ssm describe-instance-information --filters \
    Key=InstanceIds,Values=<mi-...>
```
