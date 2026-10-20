# amazon-ssm-agent — haiku/arm64 port (M3 + M4 + M5 + M6 + M7)

> **IPC completion latency (issue #847): hardware-verified 2026-10-19 in
> `amazon_ssm_agent-3.3.3270.0-3`.** Every Run Command took ~65 s to report and
> every interactive session left `ipc messaging received timedout signal!`
> behind, although the work finished in milliseconds.
>
> **Cause.** The agent-worker talks to `ssm-document-worker` /
> `ssm-session-worker` through files (`common/filewatcherbasedipc`): the sender
> writes `tmp/<seq>` and renames it into the channel directory, and the
> receiver's `watch()` loop consumes it on an fsnotify `Create` event. The
> vendored fsnotify had only an idle stub on Haiku, so the master never saw the
> worker's `complete` message. It sat on disk until the worker had exited and
> `WaitForProcess` had run out `defaultZombieProcessTimeout` (60 s, `master.go`).
> The stop timer then made `Close()` drain the directory, which is how the
> result still arrived, correct but late. Measured on `-2`
> (`i-0df3578e6d6c771f9`, fresh from canonical `ami-058776e58bd902fba`): the
> worker logged `sending document complete response` at 22:39:15.576 and
> `document worker closed` at .576; 15 s later no `ssm-document-worker` was
> running, and the channel directory still held the unread
> `worker-20261019223915-000` (`reply`) and `-001` (`complete`). The master
> logged `ipc messaging received timedout signal!` at 22:40:18.580 and only then
> `sending document ... complete response`. The master's own message that
> starts a run still reached the worker, because the worker's `watch()` drains
> the directory once at startup, before it waits for events.
>
> **Fix.** `vendor/github.com/fsnotify/fsnotify/haiku.go` is now a polling
> backend. Every 100 ms it lists each watched directory (or stats each watched
> file) and diffs the result against the last snapshot. A new name gives
> `Create` (which covers a rename into the directory), a missing name gives
> `Remove`, a size or mtime change gives `Write`, and a mode change gives
> `Chmod`. Removing the watched path itself gives `Remove` and drops the watch,
> as with inotify. Directory watches are not recursive, and a new batch is
> emitted in name order, which is the IPC sequence order. Only the haiku build
> sees this file; the inotify/kqueue backends are untouched. A `watch_node`
> backend would remove the poll, but it needs a KMessage parser in pure Go
> (`CGO_ENABLED=0`).
>
> **Hardware A/B** (same instance, `c7g.large`; `-2` as baked, then `-3` via
> `pkgman install -y` and `launch_roster stop/start x-vnd.debeos-amazon-ssm-agent`):
>
> | measurement | `-2` (as baked) | `-3` |
> |---|---|---|
> | Run Command, `send-command` to `Success`, client-side poll every 0.5 s | 65.2 / 65.2 / 65.2 s (n=3; a 4th, from the agent log, 63 s) | 1.6 / 1.6 / 1.6 s (n=3; 1.6–1.7 s, n=4, on the unstamped build of the same binaries) |
> | worker `complete` sent to master `complete` (agent log), Run Command | 63.0 s | 66 / 65 / 68 ms |
> | same, interactive session after `exit` | 63.0 / 63.0 / 63.0 s | 25 / 62 / 48 ms |
> | session probe (`echo MARK_$((6*7))`, `ps`, `exit` under `script -qfc`) | PASS 3/3 | PASS 3/3: `MARK_42`, `sh` a child of `ssm-session-worker` (e.g. `SELF=1075 PARENT=1065`), `exit` ends it |
> | new `ipc messaging received timedout signal!` lines | one per run and session | none |
> | channel directories left under `channels/` | one per run | none |
> | `ssm-document-worker` / `ssm-session-worker` left running (`ps`, `top -n 1 -d`) | none | none |
> | idle CPU of `amazon-ssm-agent` + `ssm-agent-worker`, 60 s window | 0.3–0.4 ms/s (n=2, a second fresh `-2` box) | 3.7–4.0 ms/s (n=2) |
>
> The polling costs about 3.5 ms of CPU per second at idle, ~0.35% of one
> core. `-3` is also the first agent built with toolchain patch `0005`
> (`../patches/`, issue #843), so it no longer carries the signal-`g` livelock.
> No spinning worker was seen on either build in this run, so that signature is
> still unconfirmed for the agent.

> **Interactive Session Manager (issue #553): hardware-verified 2026-10-19 in
> `amazon_ssm_agent-3.3.3270.0-2`.** `aws ssm start-session --target <id>` with no
> document (the default `Standard_Stream` shell) never gave a shell. Two layers had
> to be fixed, and the second only showed up once the first was on hardware:
>
> 1. **ssm-user model** (`agent/session/shell/shell_unix.go`, landed in PR #555).
>    The shell layer reused the Linux path build-tag-only, so a root session tried
>    `useradd -m ssm-user` and then `id -u ssm-user`. Both exit 1 on Haiku (its
>    `useradd` has no `-m`, there is no ssm-user, and `getUserCredentials` rejects
>    uid 0 anyway), so the worker died with `Unable to start command: exit status
>    1`. On `runtime.GOOS == "haiku"` the shell now runs as the current user with
>    no privilege switch. Haiku is single-user, so RunAs is ignored rather than
>    failing the session. Because `runtime.GOOS` is a constant, the compiler drops
>    the ssm-user branch from the haiku binary entirely: the `-2`
>    `ssm-session-worker` does not contain `please set the RunAs default user` or
>    `Failed to retrieve uid for`, and the `-1` one does.
> 2. **No pty backend** (`vendor/github.com/creack/pty`, added for `-2`). With layer
>    1 on hardware the session got one step further and failed with `Unable to
>    start command: Failed to start pty: unsupported`. creack/pty v1.1.11 has no
>    Haiku backend, so `pty.Start` returned `ErrUnsupported`. Its raw
>    `syscall.Syscall(SYS_IOCTL, ...)` form cannot work on Haiku either: the fork's
>    `syscall.Syscall` goes through `_kern_generic_syscall`, which reads its first
>    argument as a subsystem name. The earlier "the pty path is already proven on
>    Haiku" note here was wrong. `pty_haiku.go` now opens `/dev/ptmx`, does
>    libroot's `grantpt` (`B_IOCTL_GRANT_TTY`) and `ptsname_r`
>    (`B_IOCTL_GET_TTY_INDEX` -> `/dev/tt/<letter><hex>`), and opens the tty
>    (replica) side.
>    `ioctl_haiku.go`/`asm_haiku.s` route the package's `ioctl` helper, which also
>    backs `Setsize`, through the syscall package's libroot `ioctl` trampoline.
>    `util.go`'s `windowRectCall` now calls that helper, which on every other OS is
>    the same raw syscall it made before (later creack/pty releases do the same).
>
> **Hardware A/B** (`c7g.large` instances launched fresh from canonical
> `ami-06f217c8b8ce37c9e`, scripted `aws ssm start-session` under `script -qfc`
> sending `echo MARK_$((6*7))`, `echo SELF=$$ PARENT=$PPID; ps`, `exit`):
>
> | instance | package | runs | result |
> |---|---|---|---|
> | `i-00c19d0572ed8d744` | `-1` (as baked) | 2 | **FAIL** 2/2: `Unable to start command: exit status 1`, `createLocalUser`/`getUserCredentials` errors in `errors.log` |
> | `i-00c19d0572ed8d744` | `-2` (`pkgman install`, agent restarted) | 3 | **PASS** 3/3: `MARK_42` echoed, `sh` is a child of `ssm-session-worker` (e.g. `SELF=463 PARENT=453`, `ps` shows team 453 = `ssm-session-worker`), `exit` ends the session (`Exiting session with sessionId`), no new errors |
> | `i-070c87925e61a6976` | layer 1 only | 3 | **FAIL** 3/3: `Failed to start pty: unsupported` (found layer 2) |
>
> Run Command on the same `-2` box still returns `Success`/RC 0 (n=3). It takes
> about 65 s to report, but that is the same on `-1` (measured n=2 on a separate
> fresh `-1` instance), so it is not a regression from this change.

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


M3/M4/M5 of the "Go on DeBeOS/arm64" arc (see `../../docs/archive/go-arm64-bringup-scope.md`).
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
session/shell (the pty itself needs a Haiku arm in the vendored creack/pty;
see section 2 and the #553 note above), TLS cert-pool, logging, the setup-cli
service/package managers, and the vendored `go-git` worktree (`_unix_other`).
The `serialport` and `startup` packages join their existing **skip/stub**
variant (the non-Linux no-op path) rather than the real one.

### 2. New Haiku arms where a unix syscall is genuinely absent (5 files)

| File | Why | Approach |
|---|---|---|
| `agent/fileutil/advisorylock/advisorylock_haiku.go` | Haiku libroot has no `flock(2)`; `syscall.Flock`/`LOCK_*` are not wrapped | **Real** impl via `syscall.FcntlFlock(F_SETLK)` + `Flock_t` (all present for haiku/arm64) |
| `vendor/github.com/go-git/go-billy/v5/osfs/os_haiku.go` | Same: `os_posix.go` uses `golang.org/x/sys/unix` `Flock`/`LOCK_EX`, which the agent's vendored x/sys/unix has no haiku port for | **Real** `FcntlFlock` locks; `rename`/`umask` identical to POSIX arm. `os_posix.go` gets `&& !haiku` |
| `agent/fileutil/fileutil_diskspace_haiku.go` (+ `..._statfs.go`) | `syscall.Statfs`/`Statfs_t` not wrapped for haiku | `GetDiskSpaceInfo` split out: statfs version keeps the old unix set; **haiku returns a 1 TiB sentinel** so pre-download space guards don't spuriously block. *Degraded* — TODO: back with `statvfs`. |
| `vendor/github.com/fsnotify/fsnotify/haiku.go` | fsnotify v1.5.1 has backends for inotify/kqueue/FEN/Windows only — none for Haiku | **Polling** `Watcher` (100 ms directory/stat diff; `Create`/`Remove`/`Write`/`Chmod` as inotify reports them). It was an idle stub until `-3`, which stalled every worker IPC exchange by 60 s (#847, see the note above). TODO: a `watch_node` backend would drop the poll. |
| `vendor/github.com/creack/pty/pty_haiku.go` (+ `ioctl_haiku.go`, `asm_haiku.s`) | creack/pty v1.1.11 has no Haiku backend (`pty.Start` returns `ErrUnsupported`), and its raw `syscall.Syscall(SYS_IOCTL, …)` cannot reach Haiku's `ioctl` | **Real** pty: `/dev/ptmx` + `B_IOCTL_GRANT_TTY` + `B_IOCTL_GET_TTY_INDEX` -> `/dev/tt/<letter><hex>`, ioctls through the syscall package's libroot trampoline. `ioctl.go`/`pty_unsupported.go` get `!haiku`; `util.go`'s `windowRectCall` calls the shared `ioctl` helper (#553) |
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
  three toolchain gaps; needs a Haiku `watch_node` backend. Deferred. (Replaced
  by a polling backend in `-3`, #847.)
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
