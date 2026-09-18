# amazon-ssm-agent — haiku/arm64 port (M3)

M3 of the "Go on DeBeOS/arm64" arc (see `../../docs/go-arm64-bringup-scope.md`,
milestone **M4** in that doc's ladder: *the real upstream `amazon-ssm-agent`
compiles for haiku/arm64*).

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
