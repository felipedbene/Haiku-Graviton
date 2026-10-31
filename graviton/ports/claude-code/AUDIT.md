# AUDIT — Linux-only and platform-dependent code in Claude Code 2.1.314, read for Haiku

Input: the relocated, fold-restored tree (P4 + P5–P7 applied, 2,874 files) and
`compat/test/folds-2.1.314-report.json`. Under Node 24 on DeBeOS `process.platform` is
`"haiku"`, so after P3/P5 `Fv()` and `M()` (`getPlatform()`) both return `"haiku"`.

Each site gets exactly one class:

- **a**: works on Haiku as-is (the code shown says why).
- **b**: needs a real Haiku implementation (see "Proposed haiku implementations").
- **c**: unavailable on Haiku. Each one has an entry in LIMITATIONS.md, "Platform audit".

Nothing here has been run on DeBeOS yet. It comes from reading the minified code, and
the excerpts below are verbatim from `/tmp/cc-p5`. Haiku facts come from this tree's
own sources where I could check them (`src/bin/ps.c`, `clipboard.cpp`, `open.cpp`,
`notify.cpp`, `headers/os/kernel/OS.h`, `headers/posix/limits.h`). Everything else is
marked unverified.

## Counts

| Class | Sites |
|---|---|
| a | 12 (S05, S07, S09, S13, S14, S19, S26, S28, S30, S31, S33, S34) |
| b | 14 (S06, S10, S11, S12, S15, S18, S20, S21, S22, S23, S24, S25, S32, S38) |
| c | 12 (S01, S02, S03, S04, S08, S16, S17, S27, S29, S35, S36, S37) |

S38 (Linux open-flag numbers baked into the bundle) was found while implementing the
class (b) fixes and was added afterwards.

**Status of class (b):** all of them are implemented by `tools/apply-haiku-impl.py`
(PATCHES.md P8–P17), except two:
- S25 (`Bun.ant.CellSegmenter`) is owned separately.
- S22 (`rjn`) is kept on purpose, made safe by P17's lock `procStart` (see PATCHES.md).

| Site | Rule |
|---|---|
| S20 | P8 |
| S15 | P9 |
| S18 | P10 |
| S23 | P11 |
| S24 | P12 |
| S10 | P13 |
| S11 | P14 |
| S12 | P15 |
| S38 | P16 |
| S06/S21/S32 | P17 (`native/cc-teaminfo.c`; building and verifying it on Haiku is still pending) |

S20 is a regression that P5 itself introduced. S25 isn't Haiku-specific: it applies to
the Node compat layer on every OS.

## The Bash tool sandbox on Haiku (S02): it reports clearly and never crashes

The platform check runs before any bubblewrap/seccomp code. `chunk-v0b8hfja.js`:

```
function dI(){let e=Fv();if(e==="linux")return Tsi()!=="1";return e==="macos"||e==="windows"}
function Xy(){if(M()==="windows")return xV();return We().baseIsSupportedPlatform()}   // = Qt.isSupportedPlatform = dI
function zp(){return $l()&&Xp()&&Xy()}
function Pc(){if(X())return!1;…if(zy())return!0;if(!zp())return!1;return Oc().errors.length===0}  // isSandboxingEnabled
function zy(){return e$()&&!X()&&(!Xy()||Oc().errors.length>0)}   // e$ needs startedOnce
async function ew(e){…if(!Pc())return;…   // initialize: never runs, so startedOnce stays false
function mw(){if(!$l())return;if(!Xp())return;if(!Xy()){let s=M();…return`sandbox is enabled but ${s} is not supported (requires macOS, Linux, or WSL2)`}
```

`Fv()==="haiku"` makes `dI()` false. `Pc()` (`isSandboxingEnabled`) is false and
`getSandboxUnavailableReason()` (`mw`) returns
`sandbox is enabled but haiku is not supported (requires macOS, Linux, or WSL2)`. The
bwrap dependency check `G1(...)` runs only inside `if(s==="linux")` in `fI`, so it is
never reached. What the user then sees depends on the settings:

- **`sandbox.enabled: true`, `failIfUnavailable` unset or false** (the default for the
  interactive CLI). Commands run **unsandboxed**.
  - Interactive (`chunk-2bjf1wyg.js` `Az.start`): `if(L)t(\`sandbox disabled: ${L}\`,{level:"warn"})`
    goes to the debug log, and `setupIssues.sandboxIssueCount` is set to 1 in app state.
    Nothing is written to stderr, so this is the quietest mode.
  - `-p`/headless (`chunk-k3xv1gfh.js`) and `mcp serve` (`chunk-ksa7d46j.js`) write to stderr:
    `⚠ Sandbox disabled: sandbox is enabled but haiku is not supported (requires macOS, Linux, or WSL2)` /
    `Commands will run WITHOUT sandboxing. Network and filesystem restrictions will NOT be enforced.`
  - The sandbox-based auto-approval does not take effect. Every consumer gates it as
    `qe.isSandboxingEnabled()&&qe.isAutoAllowBashIfSandboxedEnabled()` (`chunk-dsqe13gb.js`,
    `chunk-pqeq78yn.js`), so the ordinary permission prompts apply.
- **`sandbox.failIfUnavailable: true`**: `isSandboxRequired` (`$d`) is true and Claude
  Code refuses to start (exit 1):
  `Error: sandbox required but unavailable: sandbox is enabled but haiku is not supported (requires macOS, Linux, or WSL2)` /
  `sandbox.failIfUnavailable is set — refusing to start without a working sandbox.`
  The SDK path sets this flag by itself whenever `enabled===true` (`chunk-xrt4j2zq.js`:
  `if(s.enabled===!0&&m.failIfUnavailable===void 0)m.failIfUnavailable=!0`).
- **`CLAUDE_CODE_SUBPROCESS_ENV_SCRUB=1`** (S01): startup throws from `was()`:
  `bubblewrap is required for subprocess env scrubbing and isolation. Install with: sudo apt-get install -y bubblewrap, …`.
  The refusal is loud, but the apt hint is wrong for Haiku.
- **`sandbox.enabledPlatforms` set by policy without `"haiku"`**: `mw()` returns early at
  `if(!Xp())return;`. That is upstream's design for excluded platforms: sandbox off, no
  message.

None of these modes crashes, and none presents commands as sandboxed. The one
weak spot is the interactive default, where the only signal is a debug-log line and a
setup-issue counter.

## Site table

| ID | File · function | On Linux | On Haiku | Class | Evidence |
|---|---|---|---|---|---|
| S01 | `chunk-0ane94zt.js` · `was`, `YAn`, `QJ` | subprocess env scrub needs bwrap | with `CLAUDE_CODE_SUBPROCESS_ENV_SCRUB=1`: startup throws (`await was()` in `chunk-wkcv70t3.js` is unguarded); without it `Pf()` is false and `was` returns | c | `function YAn(){let e=ogr();if(e)return sL(e);return sL("bwrap")}` … `if(…z(),YAn()===null){let E=ogr();throw Error(…"bubblewrap is required …sudo apt-get install -y bubblewrap…")}` |
| S02 | `chunk-v0b8hfja.js` · `dI`/`Xy`/`Pc`/`mw`/`$d`; startup in `chunk-2bjf1wyg.js`, `chunk-k3xv1gfh.js`, `chunk-ksa7d46j.js` | bwrap + seccomp sandbox | unsupported: see the section above | c | `function dI(){let e=Fv();if(e==="linux")return Tsi()!=="1";return e==="macos"\|\|e==="windows"}` |
| S03 | `chunk-070nc6an.js` · `ven`, `ft`, `G0t`, `xre`; gate `uXe` in `chunk-zv5y38vw.js` | writes `~/.config/systemd/user/com.anthropic.claude-daemon.service`, `systemctl --user …` | the systemd block is unconditional (its guard was folded), but every caller first checks `uXe()`, which stats `/run/user/<uid>/systemd`. That directory doesn't exist, so the user gets `Service install isn't available on haiku — the daemon still runs on demand when a client connects.` and exit 1. Three are not gated by `uXe`: `xre` (imported directly by `chunk-9nyp7h4s.js`/`chunk-zhk3tsa7.js`) returns false on the spawn error; `G0t` (uninstall, hub UI in `chunk-66f6cexh.js`) ignores the `systemctl` exit codes, treats the missing unit file as gone, and returns `{ok:!0}` | c | `async function uXe(){let r=i.XDG_RUNTIME_DIR\|\|\`/run/user/${process.getuid()}\`;try{return(await ct(Ie(r,"systemd"))).isDirectory()}catch{return!1}}`; `if(!await uXe())return H(\`Service install isn't available on ${process.platform} …\`)` |
| S04 | `chunk-w6mjdp2k.js` · `we`, `me`, `F`, `x`, `E`; `chunk-wfe5p2va.js` · `r` | native audio addon, else `arecord`, else SoX `rec` | the addon loader returns null for non-darwin/linux/win32. `arecord`/`sox` aren't installed, `apt-get`/`dnf`/`pacman` aren't found, so voice reports `Voice mode requires SoX … Install SoX manually: macOS: brew … Ubuntu/Debian: sudo apt-get install sox …` | c | `if(t!=="darwin"&&t!=="linux"&&t!=="win32")return null;` (wfe5p2va); `m("arecord",["-f","S16_LE",…])` |
| S05 | `chunk-953zb56b.js` · `v`, `g`, `$si`, `PSn`, `A` | user-namespace uid mapping from `/proc/self/uid_map` | each read is in try/catch and yields `undefined`. `$si` returns undefined unless uid is 65534, `PSn` returns undefined, `A` returns `n===65534`. That is the no-namespace answer, which is correct on Haiku | a | `async function v(){try{return U(await x("/proc/self/uid_map","utf8"))}catch{return}}` |
| S06 | `chunk-6h8148nz.js` · `b`, `G3e`, `eIe`, `QOe`, `fas`, `tgr`, `mas`; same pattern in `chunk-pbrfzy56.js` `Qo`/`Jo`, `chunk-br003cdq.js` `Ctn`, `chunk-mhbfazcy.js` `Nt`/`Cco`, `chunk-yjz82mhm.js` `QWo`, `chunk-2hxx68dq.js` `Fn`/`Kn` | process start-time token and ancestry from `/proc/<pid>/stat`, used to prove that a PID still names the same process | the guard was folded into a bare block, which makes the `ps -o lstart=` fallback dead code. The read throws ENOENT, `catch{return}` gives `undefined`, so `ud(pid)` is always `undefined`. Haiku's `ps` has no `-o lstart`/`-p` anyway (S21). See "Swallowed errors" for the consequences | b | `async function b(e,t){…try{{let i=await le().readFile(\`/proc/${e}/stat\`,…);return nL(i)}let r=…"ps"…` |
| S07 | `chunk-f80wrce4.js` · `E`; parsers in `chunk-w2ys3j7c.js` (`TPi`, `RPi`) | startup telemetry sample (page faults, read bytes) | each read is caught, so `procStat`/`storageReadBytes` stay undefined. Telemetry only | a | `try{s=TPi(S("/proc/self/stat","utf8"))}catch{}try{r=RPi(S("/proc/self/io","utf8"))}catch{}` |
| S08 | `chunk-m3w7djw9.js` · `qt`, `Gr`, `Ur` | names the local process behind a proxy connection (`/proc/net/tcp`, `/proc/<pid>/fd`, `/proc/<pid>/comm`) | `clientProcess` becomes the sentinel `<proc-net-unreadable>` | c | `for(let h of["/proc/net/tcp","/proc/net/tcp6"]){let l;try{l=await Jt(h,"utf8")}catch{continue}…return s?Mr:Lr}` |
| S09 | `chunk-k1pd3pap.js` · `a9e` (hardened read), `pi` (pinWriteTarget), `v7`, `Kt`; `chunk-nmdw6kba.js`, `chunk-8g9dfmp7.js`, `chunk-m2rdp425.js`, `chunk-bxrsjz9y.js` | binds an opened fd to its path through `/proc/self/fd` | every `/proc` step is behind `c==="linux"\|\|c==="wsl"` on `M()`, so Haiku takes the generic POSIX branch (open with `O_NOFOLLOW`, compare dev/ino) | a | `let R=await _Jt(k,p,c,!N,r);if(c==="linux"\|\|c==="wsl"){let v=await Kt(R,e,s)…}return{ioPath:k,…}` |
| S10 | `chunk-k1pd3pap.js` · `wIn`; `chunk-v5xftfk7.js` · `Me`; `chunk-s7yyd1p6.js` · `kio`/`vio` | fd→path check through `readlink /proc/self/fd/N`, with no platform guard | fails closed. `wIn` always returns `undefined`, so `chunk-3wepgp9f.js` `Gt` returns `{kind:"restricted"}` for every file and `chunk-fecm6y76.js` remote `read_file` returns refused. `Me` (artifact publish) returns null. A token passed as a descriptor number in `kio`'s env var isn't read: `/proc/self/fd/<n>` gives ENOENT, which logs `Failed to read … from file descriptor` and falls back to the well-known file (the direct read `t9e` runs only on EACCES/EPERM) | b | `try{let a=await o.readlink(\`/proc/self/fd/${e.fd}\`);…return a}catch{return}try{if(await o.realpath(r)!==r)return}catch{return}return r` (the tail is the darwin path and is unreachable) |
| S11 | `chunk-12xea136.js` · `ye`, `qo`, `Zr` (`SERVED_FILE_CALLS`) | holds a download folder and hands out `/proc/self/fd/N` as its path | `qo` throws ENOENT from `realpath`, so every served-file write fails. (`Zr`'s `supported only on Linux` throw needs `process.getuid` to be missing, which isn't the case on Haiku) | b | `return{path:\`/proc/self/fd/${s.fd}\`,…}` / `async function qo(e,r){let n=await Nr(\`/proc/self/fd/${e.fd}\`);…}` |
| S12 | `chunk-m6kk55rf.js` · `lQe`, `I1s`, `wqn`, `dQe` | the `O_DIRECTORY` chain: `realpath(/proc/self/fd/N)` and `/proc/self/fd/N/<child>` | `lQe` returns an error string, so desktop-root attachment saves fail with `nothing was saved: the attachments folder cannot be used (…)` (`chunk-k3xv1gfh.js` logs `not_used`; `chunk-58wmt1zj.js` `C` skips silently). `I1s` returns `at:/proc/self/fd/N`, so kept-copy moves in `chunk-3wxsf36h.js` fail. `O_DIRECTORY` itself exists on Haiku; only the `/proc` path is the problem. `dQe` with `Ehe()===null` uses plain `mkdir` and works | b | `function s(t){return\`/proc/self/fd/${t.fd}\`}` … `if(o=await c(e,f),a=void 0,await b(s(o))!==e)return"behind_a_link"` |
| S13 | `/proc/self/fd` sites with a designed fallback: `chunk-7hh7yp6n.js` `At`, `chunk-bxrsjz9y.js` `$Yr`, `chunk-gs173es5.js` `pr`, `chunk-k1pd3pap.js` `BD`, `chunk-fejj39g0.js` `dy`, `chunk-zy2k3hny.js` `dzo`, `chunk-t56jh66m.js` `FIo`, `chunk-839fjcr3.js` `Jt` | path re-check after open | each falls back: `At` → `ju(o)`, `$Yr` → "two-sample fallback", `BD` → `realpath`+dev/ino, `dy` → `{unread:…}` logged. `dzo`/`FIo`/`Jt` skip the re-check when the read returns null (see "Swallowed errors") | a | `return ku(\`/proc/self/fd/${e}\`).catch((t)=>{let r=B(t);if(r!=="ENOENT"&&r!=="ENOTDIR")throw t;return ju(o)})` |
| S14 | `chunk-r3zdrvw5.js` · `xke` | credential store `"file"` (folded; darwin: `"keychain"`) | the same plaintext file store as Linux. Works | a | `function xke(){return"file"}` |
| S15 | `chunk-aa47jmf3.js` · `m`, `Zvn`, `h`, `p` | opens URLs/files with `xdg-open`; headless detection via `$DISPLAY` | `xdg-open` is missing, so `m` returns `{ok:!1,reason:"opener_missing"}` and `Zvn`/`h` return false. `p()` is linux-only, so Haiku is never treated as headless. `$BROWSER` overrides `m` | b | `return y(await it(o\|\|"xdg-open",[r],f))`; `function p(){return M()==="linux"&&!i.DISPLAY&&!i.WAYLAND_DISPLAY}` |
| S16 | `chunk-wj68m7s3.js` · `D`, `k`, `l`; `chunk-nyjcftf8.js` · `x` (guarded) | Claude Desktop deep links (`xdg-mime`, `xdg-open`) | `D()` is false, so the status is `not-installed`. `nyjcftf8` returns early: `if(!["darwin","linux","win32"].includes(process.platform))return;` | c | `async function D(e){{let{code:o,stdout:r}=await it("xdg-mime",["query","default",\`x-scheme-handler/${e}\`]);…}return!1}` |
| S17 | `chunk-ef9gz064.js` (image paste/screenshot table) | `xclip`/`wl-paste` image commands | `I[process.platform]\|\|I.linux` falls back to the Linux commands, which fail, so the result is "no image" and `null` | c | `s=n[process.platform]\|\|n.linux` / `linux:{checkImage:\`xclip -selection clipboard -t TARGETS -o …\|\| wl-paste -l …\`` |
| S18 | `chunk-9deh4a76.js` · `Z` (copy), `xGt` (paste), `Sbe`, `hR` | `wl-copy`/`xclip`/`xsel`/addon | `Z` has no haiku case and returns false, so copying falls back to OSC 52 or tmux. `xGt` hits `default:return""` | b | `switch(M()){case"macos":…case"linux":{…}case"wsl":…case"windows":…}return!1` |
| S19 | folded booleans: `chunk-zy2k3hny.js` `K5o`, `chunk-x42qr0ry.js` `ci`, `chunk-k1pd3pap.js` `zt`, `chunk-jpga7bcc.js` `uut`, `chunk-72whyak8.js` ×2, `chunk-fgccedbt.js` keychain `s`, `chunk-3wxsf36h.js` `x`; `chunk-szzxb7zd.js` `te()`, `Lxi` | macOS-vs-other constants | each one is the non-macOS value, which is right for Haiku: case-sensitive paths, no keychain, `/desktop` hidden, `te()` takes the POSIX open flags. `Q.O_NOATIME??0` gives 0, so `Nms` skips `O_NOATIME` | a | `function K5o(){return!1}`, `function ci(){return!1}`, `var zt=!1`, `function te(){return"linux"}`, `Lxi=Q.O_NOATIME??0` |
| S20 | `chunk-hv6m45s7.js` · `iqn` (`openTreeAnchor`) | switch on `e.platform()` (= `M()`): windows / linux+wsl / macos / unknown | **no `case"haiku"` and no default**, so the function returns `undefined`. Callers then do `"unavailable"in l` and throw `TypeError: Cannot use 'in' operator…`: worktree open/remove in `chunk-qyxhe6nc.js` (`ae`, `Ze`; `Ze`'s callers turn it into `unavailable:<TypeError text>`) and the trash sweep in `chunk-evaf4vzt.js`. Before P5, Haiku hit `case"unknown"` and got `{unavailable:"unsupported_platform"}` | b | `case"macos":{…}case"unknown":return{unavailable:"unsupported_platform"}}}` |
| S21 | `chunk-fgccedbt.js` · `HL`/`Rf`/`UL` (Bash `killProcessTree`); `chunk-2hxx68dq.js` · `Wn`/`jn` (process table for non-Linux) | `ps -A -o pid= -o ppid= …` | Haiku's `ps` takes only `-ihaso:`, and `-o` accepts only the columns `Team Id Threads Gid Uid` (`src/bin/ps.c`). No line matches `/^\s*(\d+)\s+(\d+)…/`, so descendants come back empty. `mk` still kills the process group (`process.kill(-e,n)`), but children that left the group are not found | b | `s=NL("ps",["-A","-o","pid=","-o","ppid=",...y],…)` |
| S22 | `chunk-xyt7m0qn.js` · `rjn`, `q0s`, `uon` | proves a PID is the daemon (`/proc/<pid>/cmdline`, `ps -ww -o args=`) | `rjn` **returns true** when `/proc` is unreadable. `q0s`'s ps fallback finds nothing, so it returns false. `uon` returns false because `ud()` is undefined | b | `async function rjn(e){let r;try{r=await u(\`/proc/${e}/cmdline\`,"utf8")}catch{return!0}…` |
| S23 | `chunk-wg6ymv3p.js` (daemon re-auth) | `notify-send "Claude" "…needs re-authentication"` | `execFileNoThrow`: no notification and no error | b | `c("notify-send",["Claude","Your Claude assistant needs re-authentication"])` |
| S24 | `chunk-ddrd3r4k.js` · `On` | path-length precheck table | `ir["haiku"]` is undefined, so there is no precheck and long paths fail later with the OS's ENAMETOOLONG. Haiku's `PATH_MAX` is 1024 (`headers/posix/limits.h:44`) | b | `var ir={macos:1024,linux:4096,wsl:4096};function On(e,n,i){let h=ir[M()];if(h===void 0…)return;` |
| S25 | `chunk-pmgbk1rv.js` · `d7n` ← `chunk-x65c4var.js` screen buffer (`class pf{…native=ps(b6e)`, `w1`), `chunk-zy2k3hny.js` `UBt` | `Bun.ant.CellSegmenter` (Anthropic's Bun) | **throws, nothing catches it.** `bun-compat.mjs` has no `Bun.ant`, and RUNTIME-DEPS.md's "call sites feature-detect or catch" is wrong for this member. The interactive TUI's cell buffer constructs it. This affects every OS under Node, not just Haiku | b | `if(typeof Bun>"u"\|\|typeof Bun.ant?.CellSegmenter!=="function")throw Error("This runtime has no Bun.ant.CellSegmenter. Run Claude Code on the @anthropic-ai/bun-internal version pinned in package.json.")` |
| S26 | `chunk-2tvmykr7.js` · `x`, `chunk-zy2k3hny.js` · `sBo` (tmux install hints) | apt/dnf text | `default:` gives the generic `install tmux using your system's package manager` | a | `default:return"Install tmux using your system package manager."` |
| S27 | `chunk-zy2k3hny.js` · `kon` (`/restart`) | enabled on macos+linux | hidden on Haiku | c | `let e=M();if(e!=="macos"&&e!=="linux")return!1;` |
| S28 | memory/cgroup: `chunk-zy2k3hny.js` `_En`, `e2n`, `Gan`; `chunk-pbrfzy56.js` `Wn`, cgroup dir; `chunk-fgccedbt.js` statm | `/proc/meminfo`, cgroup v2 | guarded by `M()` (`e2n`: `{scarce:!0,detail:\`${e}: kernel verdict taken as is\`}`) or caught, so the values are absent | a | `async function _En(e=PRr()){if(e.platform!=="linux"&&e.platform!=="wsl")return;…` |
| S29 | file watching: every `fs.watch` / `fs.watchFile` / chokidar site (table in "File watching on Haiku" below) | native fs events (inotify on Linux) | **measured:** Node 24.21.0 on Haiku throws `Error: ENOSYS: function not implemented, watch '<path>'` synchronously from `fs.watch` (libuv has no Haiku fs-event backend). Sites that poll (chokidar `usePolling`, `fs.watchFile`) are unaffected, as far as `watchFile` works (unmeasured). The rest lose live reload. One site, settings, does it silently, and no site crashes | c | `function tt(t,e,i,s,r){…try{return xt(t,{persistent:e.persistent},a)}catch(o){s(o);return}}` (chokidar, `chunk-9a0z9ab4.js`) |
| S30 | `chunk-a6cj61xj.js` · `Bke` | managed-settings platform chain `["linux"]` | chain is `["haiku"]`: settings keyed by `linux` don't apply, unkeyed ones do | a | `function zo(e){return e==="wsl"?["wsl","linux"]:[e]}` |
| S31 | `chunk-xrt4j2zq.js` · `Lt` (SDK native binary) | `@anthropic-ai/claude-agent-sdk-linux-arm64[-musl]` | the only caller (`chunk-tnczr922.js` scheduled worker) passes `pathToClaudeCodeExecutable:g.cmd` (`$u({pinToCurrentBinary:!0})`), so `Lt` is never reached | a | `pathToClaudeCodeExecutable:g.cmd,executableArgs:g.prefixArgs` |
| S32 | process-start & identity users: `chunk-6h8148nz.js` `yke`, `P`, `qv`, `cge`; consumers `chunk-g8415bcb.js`, `chunk-yeqe6mfj.js`, `chunk-zv5y38vw.js` | kill or adopt a background REPL/PTY host only after proving its identity | see S06 and "Swallowed errors": identity is never proven | b | `async function yke(e,t,n="SIGTERM"){…if(await f(e,t)==="other")return!1;…` |
| S33 | other `Bun.ant` uses (S25 excepted) | Anthropic-only Bun members | each is feature-detected or caught (listed below) | a | `typeof Bun.ant?.windowsProcessList!=="function"` |
| S34 | `chunk-74z2fna5.js` UDS-messaging socket check | sticky-dir ownership (`changed-code`) | POSIX mode bits only, so it works on Haiku | a | `if((y.mode&16)!==0)return o(y.uid)&&a!==void 0&&y.gid===a&&a===n` |
| S35 | `chunk-74z2fna5.js` (literal) | tells the model to use `printf … \| nc -N -U "$CLAUDE_CODE_MESSAGING_SOCKET"` | no `nc -U` on Haiku, so inter-agent UDS messaging from the shell doesn't work. Whether `socat` is installed is unknown | c | `${"nc -N -U"} "$CLAUDE_CODE_MESSAGING_SOCKET"` (darwin build: `"nc -U"`) |
| S36 | `chunk-fejj39g0.js` · `Rw`, `chunk-xjn8qbfw.js` · `NYo`/`AYr`/open | Claude in Chrome: browser profiles, native-messaging paths | `switch(process.platform)` has no haiku case, so the profile list is empty: `[Claude in Chrome] Unsupported platform for extension detection: haiku` | c | `switch(process.platform){case"darwin":h=s.macos;break;case"linux":h=s.linux;break;case"win32":{…}}` |
| S37 | `chunk-bfxj2kma.js` · `M`, `y` | launches a deep link in a terminal emulator | `default:return null`, so the log says `No terminal emulator detected` | c | `switch(process.platform){case"darwin":return S();case"linux":return F();case"win32":return A();default:return null}` |
| S38 | `chunk-szzxb7zd.js` `Bo`, `y9` (26 / 6 importing modules); `chunk-zy2k3hny.js` `sfr` | Linux `open(2)` flag numbers baked in: `Bo`=524288 (`O_CLOEXEC`), `y9`=2097152 (`O_PATH`) | on Haiku 0x80000 is `O_NOFOLLOW` and 0x200000 is `O_DIRECTORY` (`headers/posix/fcntl.h:57,61`), so every `\|Bo` open refuses a symlink as its last path component and every `\|y9` open refuses a regular file. The darwin build has `Bo=16777216` (its own `O_CLOEXEC`) | b | `var Bo=524288,GIe=0,y9=2097152,Lxi=Q.O_NOATIME??0` (report: `changed-code` vs darwin `16777216,ihe=536870912`) |


## File watching on Haiku (S29): per-site effect of `fs.watch` → ENOSYS

Bundled chokidar (`chunk-9a0z9ab4.js`) catches the throw:
`try{return xt(t,{persistent:e.persistent},a)}catch(o){s(o);return}`. It forwards the
error to the watcher's `"error"` event only if a listener is attached:
`_handleError(t){…if(t&&!this.closed&&this.listenerCount(m.ERROR)>0&&e!=="ENOENT"&&e!=="ENOTDIR"…)this.emit(m.ERROR,t)…}`.
With `usePolling:!0` it uses `fs.watchFile` and never calls `fs.watch`. `CHOKIDAR_USEPOLLING=1`
forces polling for every chokidar watcher that doesn't pass `usePolling` itself; that is
upstream behaviour, not a port fix. Non-filesystem `watch(` methods (`chunk-z5vf16gh.js`
preview DOM observer and `watch.call` tool, `chunk-t5jayd9k.js`/`chunk-2bjf1wyg.js` `cr.watch`
response timer, `chunk-x65c4var.js` event-loop lag, `chunk-9rabqmrv.js` watchdog) are out of scope.

| # | Site | Feature | Mechanism → on ENOSYS | Effect on Haiku |
|---|---|---|---|---|
| W1 | `chunk-x42qr0ry.js` `Gi` (`kE.watch(Se,{…usePolling:!1})`), probe `$s`/`bu`, `md` (`Cu`) | settings hot reload (`settings.json`, the drop-in dir, files that appear later) | chokidar logs ``[settings] watcher error: ENOSYS…``. The switch to polling only happens for `EMFILE`/`ENFILE`/`ENOSPC` (`wu=new Set(["EMFILE","ENFILE","ENOSPC"])`, `io`). The probe sees `watch() threw` and answers `"unprobed"`, which `ud` treats as healthy and records `settings_watcher_start` as started | **silent**: edits made outside Claude Code (editor, another session, managed settings) take effect only after a restart. Nothing reaches the user |
| W2 | `chunk-zy2k3hny.js` FileChanged (`kE.watch(Ge,…)`, native) | `FileChanged` hooks | caught, warn ``FileChanged: watcher error``, telemetry `file_watcher_start`/`fs_error` | FileChanged hooks never fire |
| W3 | `chunk-zv5y38vw.js` `sr` bg-dispatch (`usePolling` only on macOS) | daemon picks up queued background-session dispatches | caught, logged at error level ``[bg-dispatch] watcher error``, telemetry `tengu_bg_dispatch_watcher_failed`. A cold-start drain (`ar`) runs once | dispatches queued after the daemon started aren't noticed until the next drain (daemon restart) |
| W4 | `chunk-kdwj4kwy.js` `Lvr` (`usePolling:M()==="macos"`) | daemon config reload | caught, warn ``[daemon-config] watcher error`` | daemon config edits need a daemon restart |
| W5 | `chunk-kg7g93yq.js` `f0r` (native) | custom theme files | caught, warn ``[theme] watcher error`` | theme edits need a restart |
| W6 | `chunk-1hkvhg4r.js` ScheduledTasks (`r.watch(c,{persistent:!1…})`) | scheduled-task file changes | caught, warn; the periodic `setInterval(ae,Je)` rescan keeps running | degrades to the rescan interval |
| W7 | `chunk-zy2k3hny.js` writeObserver `dRs`/`lRs` (`Lpt`, non-Linux path) | detecting files a Bash command changed (Bash edit diff) | the throw rejects `dRs`, and `iVn`'s `.catch` logs ``writeObserver: <root>: Error: ENOSYS…`` and records `bash_edit_diff`/`watch_failed`. Sentinels: ``writeObserver: sentinels: …`` | no Bash edit diffs or outside-write attribution |
| W8 | `chunk-2bjf1wyg.js` jobStateNameSync (`lHt`) | live sync of a background job's name | `catch(Xe){return t(…"[jobStateNameSync] watch skipped: "…),()=>{}}`: debug log only | **silent**: the name is read once, then never updated |
| W9 | `chunk-2bjf1wyg.js` useBgSessionPr (`Beo`) | live PR link of a background session | same pattern: debug log ``watch skipped`` | **silent**: the PR link is read once |
| W10 | `chunk-qk3r1ha0.js` task-list watcher (`b`) | task list updated by other processes (teammates/agents) | `try{let e=b(i,this.#h);…}catch{}`: **swallowed**. In-process updates still arrive through `Vri` | **silent**: other processes' task-list changes never show |
| W11 | `chunk-fgccedbt.js` `Zd` file-event watcher | event watch with poll fallback | `catch(r){this.pollInstead(void 0,d(r));return}` → `fs.watchFile` | works if `watchFile` works |
| W12 | `chunk-fgccedbt.js` `fc` config freshness (`~/.claude.json`) | config refresh from other sessions | `fs.watchFile` (`SJt`) by default; the event path is Linux-only (`Gh`) | works if `watchFile` works |
| W13 | `chunk-zdsa7zt3.js` skills (`usePolling:ie`, `ie=!0`) | skills/commands reload | polling | works if `watchFile` works |
| W14 | `chunk-pyn8k20s.js` keybindings (`usePolling:!0`) | keybindings reload | polling | same |
| W15 | `chunk-zy2k3hny.js` plugin dirs ×3 (`tOe(…)`: `usePolling:!0`; `jUn` `usePolling:!0`) | plugin reload | polling | same |
| W16 | `chunk-bh2kkzyk.js` `Lo` (`usePolling:!0`) | dev mods reload | polling | same |
| W17 | `chunk-fejj39g0.js` memory-watcher (`usePolling:gC`, `gC=typeof Bun<"u"`) | memory files sync | polling, because `bun-compat.mjs` defines `globalThis.Bun` | same. Without the compat global it would be W5-like |
| W18 | `chunk-9ch9n4ez.js` `QDi` (`M()==="linux"&&…?He:null`) | working-sync watch | Linux only | not reached |

No site crashes. W1, W8, W9 and W10 are the silent ones; W1 is the one that matters.
Fixing this (a Haiku `uv_fs_event` backend over node monitoring in libuv, or polling in
the compat layer) is an owner decision and is not implemented here.

## Swallowed errors that would hide Haiku breakage

1. **Process identity, S06/S32.** `ud()`/`G3e()` are always `undefined`. The checks that
   use them read the unknown value as "same process": `function P(e,t){return t===void 0||t===e||C(e,t)}`
   and `async function qv(e,t){if(t===void 0)return!0;…}`, so **PID-reuse protection is
   off without any log line.** Going the other way, `yke` refuses to kill a live group
   it can't prove:
   `f(e,t)` → `_ke` gives undefined → `process.kill(e,0); return"other"` → `yke` returns false.
   Cleanup of background REPL/PTY hosts (`chunk-yeqe6mfj.js`, `chunk-zv5y38vw.js`,
   `chunk-g8415bcb.js`) leaves those process groups running, and nothing is logged.
2. **`rjn` (S22) returns true when `/proc/<pid>/cmdline` can't be read.** The daemon
   "is it ours" test passes for any PID.
3. **`wIn` (S10) swallows the readlink error.** `Gt` in `chunk-3wepgp9f.js` reports
   `restricted`, the same as a permission denial, and remote `read_file`
   (`chunk-fecm6y76.js`) reports `read denied`. The user can't tell either one from a
   policy refusal.
4. **`Me` (S10, artifact publish) has `catch{return null}` around the whole read,** so an
   artifact silently looks absent.
5. **`dzo`, `FIo`, `Jt` (S13) drop a safety re-check without saying so:**
   `Gq(\`/proc/self/fd/${Ie.fd}\`).catch(()=>null);if(Je!==null&&…)`, `try{O=he(…)}catch{}if(O!==null&&…)`,
   `ce(…).catch(()=>null);if(ne!==null&&…)`. Each "destination moved" check passes
   without running.
6. **Process-tree enumeration (S21).** A `ps` that rejects its flags returns non-matching
   text rather than an error code that is checked. `Rf` hands back an empty set and
   `killProcessTree` never logs `enum_spawn`.
7. **`notify-send` (S23)** goes through `execFileNoThrow` inside `try{…}catch{}`.
8. **`iqn` (S20).** `qyxhe6nc` `Ze`'s callers (`_e`, `Rse`) and `SWi` catch the TypeError and
   turn it into `unavailable: Cannot use 'in' operator…` or `{kind:"threw"}`. The real
   cause, a missing platform case, never shows.
9. **Sandbox, interactive default (S02).** The only signal that commands run unsandboxed
   is a debug-log line and `sandboxIssueCount`. This is upstream's design for any
   unsupported OS. It is not a code fault, but on Haiku it is the default experience.

## Consumers of `M()` (getPlatform) and `Fv` — the required list

The scan (acorn over all `chunk-*.js`) followed every call of the binding imported as `M`
from `chunk-8dvz2fjk.js` or as `Fv` from `chunk-jwstday3.js`: **974 call sites**. For
Haiku the only question is whether a site's outcome differs from plain Linux (not WSL).
A test against `"windows"`, `"macos"`, `"wsl"` or `"unknown"` gives the same answer for
`"haiku"` as for `"linux"`. A site behaves differently only when it tests `"linux"` (directly, through
an assigned variable, in a `switch` with `case"linux"`, or through a set/table holding
`linux`), or when it falls through a switch that has no default. That is **96 direct sites
plus 12 through arguments**, listed below. The remaining **877** sites (which include the pass-through rows at the end
of the table) are counted per module at the end.

### Behaviour that turns OFF or changes on Haiku (was Linux)

| Site | Test | What changes for `"haiku"` |
|---|---|---|
| `chunk-070nc6an.js`@55203, @59074 | `M()==="linux"` | telemetry `platform_linux:false` (all three platform flags are false) |
| `chunk-070nc6an.js`@68546 `Kn` | `r!=="linux"&&r!=="wsl"` | `logind.conf KillUserProcesses` warning skipped |
| `chunk-2bjf1wyg.js`@123137 | `M()==="linux"` | sandbox-violation counter UI: sandbox is off anyway |
| `chunk-2hxx68dq.js`@36839 | `r==="linux"\|\|r==="wsl"` | process-group discovery goes from `/proc` to `Wn` (`ps -A -o …`, S21) |
| `chunk-2hxx68dq.js`@37086 `qe` | `i==="linux"\|\|…` | pinned-process proof: neither the macOS nor the Linux branch runs, so nothing is proven |
| `chunk-2tvmykr7.js`@6648 `x` | switch, `case"linux"` | agent-swarm tmux hint takes the generic default |
| `chunk-5d3v9q6g.js`@25800 `Ze` | `l===` | telemetry flags only |
| `chunk-6h8148nz.js`@1209 `qno`, @1782 `z3e` | `!=="linux"&&!=="wsl"` | `/proc` stat reader returns undefined; `z3e` returns `!0` without inspecting `/proc` (its Linux check that `/proc/self` is this pid and lists ≥16 pids is skipped) |
| `chunk-6nh2mjmf.js`@84309 `Ju` | `!=="linux"&&!=="wsl"` | container detection (`/run/.containerenv` …) skipped |
| `chunk-6nh2mjmf.js`@95158 `On` | `r=n\|\|M()==="linux"\|\|…` | private-dir chmod repair only when harness-owned |
| `chunk-6nh2mjmf.js`@159500 | `==="linux"\|\|==="wsl"` | plugin-eval sandbox deny-list expansion skipped (sandbox off) |
| `chunk-7b09tcre.js`@2398 `te` | `M()==="linux"` | snap pwsh workaround skipped |
| `chunk-7hp2kn2d.js`@1358 | `M()!=="linux"` → `throw WB("unexpected_error")` | this remote-file operation, built only on `/proc/self/fd`, refuses with `unexpected_error` (c-like; covered by S10's fix pattern) |
| `chunk-8g9dfmp7.js`@37020 | `==="linux"\|\|==="wsl"` | `write_files` fd-path re-check skipped; dev/ino checks still run |
| `chunk-90zad8kv.js`@3262 `DHn` | `M()==="linux"` | `start_shell_task_v1` capability (flag `tengu_mossy_lathe`) off |
| `chunk-9ch9n4ez.js`@15990 `QDi` | `M()==="linux"` | working-sync watch off; polling path |
| `chunk-9deh4a76.js`@3776, @4526, @4738, @5806, @6636, @7674 | probe / switches | clipboard: no native tool. `Sbe` falls through to tmux/osc52, `K8s` gives `not_linux`, `Z` returns false, `xGt` returns `""` (S18) |
| `chunk-9hawyp34.js`@28299 `xt` | `n==="linux"\|\|n==="wsl"` | default deny rule `Read(//proc/**/environ)` not added (no /proc on Haiku) |
| `chunk-9hawyp34.js`@28623 `ke` | switch | credential tools list `default` (no `secret-tool`) |
| `chunk-9hawyp34.js`@35077 `Cn` | `d=r==="linux"\|\|…` | MCP credential-path matching uses the plain path (same as Linux minus the `/proc` forms) |
| `chunk-aa47jmf3.js`@2376 `p` | `M()==="linux"&&!DISPLAY…` | never "headless", so it always tries an opener (S15) |
| `chunk-aj9ghnwb.js`@27584 | `M()==="linux"?…/proc…:X(y)` | uses the `realpath` branch |
| `chunk-bxrsjz9y.js`@28296 | `h=f==="linux"\|\|…` | open-by-fd path off; generic open |
| `chunk-cba2p5gt.js`@5049 `M3o` | `t=o==="linux"\|\|…` | Linux-only path aliases not generated |
| `chunk-dz91hxpc.js`@73818 `cm` | `u==="macos"`/`"linux"`/`"windows"` | screenshot-to-clipboard unsupported (S17) |
| `chunk-f798ac4e.js`@3933 `Z` | `M()!=="linux"` | takes the non-`/proc` branch `t(e,()=>h(e))` |
| `chunk-fejj39g0.js`@431977 `bnt` | `M()==="linux"?Kkt:UAe` | uses the generic comparator `UAe` |
| `chunk-fejj39g0.js`@526486 | `An()&&M()==="linux"` | `gh` config-dir handling for remote skipped |
| `chunk-fgccedbt.js`@97393 `Gh` | `…&&M()==="linux"` | `CLAUDE_CODE_CONFIG_WATCH_EVENTS` ignored; polling |
| `chunk-fgccedbt.js`@851711 | `z===y:C.has(B)\|\|M()==="linux"` | unclean-exit sweep doesn't assume ownership |
| `chunk-g2ed4z08.js`@14328 `be`, @14653 `we` | `!=="linux"` | Linux sandbox glob warnings off |
| `chunk-jwstday3.js`@9344 | `p==="macos"` / `"linux"` | sandbox proxy env: no Linux bridge vars (sandbox off) |
| `chunk-k1pd3pap.js`@13858 `a9e`, @17567 `pi` | `c==="linux"\|\|…` | hardened read/write: generic branch (S09) |
| `chunk-k3xv1gfh.js`@782423 | `CLAUDE_CODE_REMOTE&&M()==="linux"` | remote `list_directory` handler off |
| `chunk-ky5pek6j.js`@1024 `RTo` | switch `linux,wsl,macos` | tty path: `default`, so non-blocking stdout setup is unsupported (`unsupported_platform` is not logged) |
| `chunk-m2rdp425.js`@15188 | `i==="linux"…?E1e(fd):i==="macos"?/dev/fd:e` | task output uses the plain path |
| `chunk-mhbfazcy.js`@2893 `zt` | `!=="linux"&&!=="wsl"` | kills with `process.kill(-pid)` directly (POSIX; works) |
| `chunk-nh9cpt34.js`@17813 `br` | `M()==="linux"?XDG_CONFIG_HOME` | Zed keymap in `~/.config/zed` |
| `chunk-nmdw6kba.js`@21829 | `a==="linux"\|\|…` | fd-path check off; canonical-path re-check runs instead |
| `chunk-pbrfzy56.js`@32371, @40120 | `!=="linux"&&!=="wsl"` | tool memory cgroup off (`useToolMemoryCgroup` inert) |
| `chunk-qk4q5mdy.js`@19660 `Zn` | `macos/windows/wsl` | `.gitattributes` match is case-sensitive, same as Linux |
| `chunk-s0dw5891.js`@7786 | `==="linux"\|\|==="wsl"` | lock `procStart` undefined |
| `chunk-v0b8hfja.js` Fv @831030, @835806, @836117 `dI`, @836322 `fI`, @837148 `pI`, @838376 `sg`, @838538 `ig`, @851181 `AK` | `Fv()==="linux"` | the whole Linux sandbox backend is off; `dI` is false (S02) |
| `chunk-v0b8hfja.js`@939120 | `Na=M()==="linux"\|\|…` | git lock pre-creation `Kl` off |
| `chunk-v0b8hfja.js`@948826 | `Ft=M()!=="linux"` | case-fold default true on Haiku. This is a **difference**: BFS is case-sensitive, so sandbox path matching folds case where Linux doesn't. Sandbox is off, so there is no effect |
| `chunk-v0b8hfja.js`@954254, @990617 `uw`, @996594 `kV`, @1005294, @1026510 `Eue`, @1048177, @1051407 | `linux/wsl(/macos)` | sandbox internals: unreachable with the sandbox off |
| `chunk-wv3sjy7g.js`@19532 | `M()==="linux"?{noChildLeft:!0}` | spawn without `noChildLeft` |
| `chunk-x42qr0ry.js`@29484 | `usePolling===!1&&(linux\|\|wsl)` | settings watcher not project-scoped |
| `chunk-x65c4var.js`@247441 | right-click paste on `windows/wsl/linux`, middle-click on `linux` | mouse paste off on Haiku |
| `chunk-xjn8qbfw.js`@3820, @4308, @5390 | switches | Claude in Chrome: no browser (S36) |
| `chunk-z5vf16gh.js`@167127 | `y==="linux"\|\|…` | picture fd-path check skipped; dev/ino check remains |
| `chunk-zy2k3hny.js`@261245 `u0e` | `!=="linux"&&!=="wsl"` | command CPU telemetry off |
| `chunk-zy2k3hny.js`@911445 `Ege` | `n!=="macos"&&!(n==="linux"&&…)` | remote-tools hook sandbox `{unavailable:"platform"}` |
| `chunk-zy2k3hny.js`@921469 | `ht=M()==="linux"` | `HRo` temp candidate not added |
| `chunk-zy2k3hny.js`@1395411 | `linux\|\|wsl` | `tengu_lucky_finch` fd verdict gives `unbound` |
| `chunk-zy2k3hny.js`@2047150 `kon` | `!=="macos"&&!=="linux"` | `/restart` hidden (S27) |
| `chunk-zy2k3hny.js`@2489532 `Gan` | `M()!=="linux"` | VmRSS sample off |
| `chunk-zy2k3hny.js`@3055226 `mwr` | `linux\|\|wsl ? fwr : proxyPort` | git credential helper port from `getProxyPort()`; sandbox off, so undefined |
| `chunk-zy2k3hny.js`@3203221 `xEr` | label | sandbox label `""` |
| `chunk-zy2k3hny.js`@3245328 `rZe` | `M()==="linux"` | `DISPLAY`/`WAYLAND_DISPLAY` not blanked for child |
| `chunk-zy2k3hny.js`@3247933 `tRr` | macos/linux/windows | running-IDE detection empty |
| `chunk-zy2k3hny.js`@3630893 `sBo` | switch | generic tmux hint |
| `chunk-zy2k3hny.js`@3709283 `mMn` | `linux\|\|wsl` | thread CPU sampler off |
| `chunk-zy2k3hny.js`@4408207 `CWt` | `!=="linux"&&!=="wsl"` | fd→path returns null (designed) |
| `chunk-zy2k3hny.js`@4433415 `Pks` | `!=="linux"&&!=="wsl"` | plugin install uses `qGn` (non-fd path) |
| `chunk-zy2k3hny.js`@4762958 `e2n` | `!=="linux"&&!=="wsl"` | memory-scarcity verdict `scarce:true, "haiku: kernel verdict taken as is"` |
| `chunk-zy2k3hny.js`@4808715 `sVn`, @4809061, @4810183, @4814251, @4827009 `MRs` | `M()==="linux"` | fs-watch backend: non-Linux (`dRs`/`iRs`/`ADe`) path (S29) |
| Through arguments: `chunk-zy2k3hny.js` `R6t` | `!["linux","wsl"].includes(M())` | uses `dVe` (non-fd path) |
| `chunk-985cqpjs.js` `Eo` | `Bu=new Set(["linux","macos","wsl"])` | MCP auto-discovery off |
| `chunk-6s5ezrj7.js` | `c=new Set(["linux","macos"])` | `cc-plugin-easel` (charts) unavailable |
| `chunk-ddrd3r4k.js` `On` | `ir[M()]` | no path-length precheck (S24) |
| `chunk-r3gx0qwp.js` `mEi` | `n!=="linux"&&n!=="wsl"` | pid domain is `"haiku"` (no machine-id/ns) |
| `chunk-zy2k3hny.js` `_En(PRr())` | `e.platform!=="linux"` | available-memory probe returns undefined |
| `chunk-a6cj61xj.js` `Bke`/`zo` | chain | `["haiku"]` (S30) |
| `chunk-wewsndh6.js` `oYr`→`o` | switch, no default | target triple undefined, so per-triple plugin binaries are skipped |
| `chunk-c83f1ekr.js` `Xe` | switch `linux/wsl` | `XDG_*_DIR` ignored; `~/Desktop` etc. |
| `chunk-hv6m45s7.js` `iqn` via `chunk-y2b13z76.js` `platform:()=>M()` | switch, no default | **returns undefined** (S20) |
| `chunk-6nh2mjmf.js`, `chunk-a6cj61xj.js`, `chunk-v0b8hfja.js` `Xp` | `enabledPlatforms.includes(M())` | policy lists must name `"haiku"` |
| telemetry/status: `chunk-1wesbfvz.js`, `chunk-74z2fna5.js`, `chunk-p1je9wt1.js`, `chunk-ywbhxc0w.js`, `chunk-9m4nq254.js`, `chunk-6b7sh4wc.js`, `chunk-mq4wyke8.js`, `chunk-n0vk7gyp.js`, `chunk-y8z88azs.js`, `chunk-k40pmb1v.js`, `chunk-n4peae6z.js`, `chunk-x42qr0ry.js` | value passed on | report `"haiku"` |

`Fv()` has 27 call sites. The ones that test `"linux"` are all inside the sandbox backend
(`chunk-v0b8hfja.js`, above) and the proxy env (`chunk-jwstday3.js`@9344). The rest test
`!=="windows"`/`==="windows"` (`chunk-jwstday3.js` path normalisation) and are unchanged.

### No change vs Linux (877 sites, tests only windows/macos/wsl/unknown, or pass the value through)

<details><summary>per-module counts</summary>

| Module | Tests |
|---|---|
| `chunk-070nc6an.js` | `!==windows` ×1, `===macos` ×4, `===windows` ×5 |
| `chunk-0ane94zt.js` | `!==windows` ×1 |
| `chunk-0k9hmtnn.js` | `===windows` ×1 |
| `chunk-0p7kejg5.js` | `!==windows` ×2, `===macos` ×1, `===windows` ×3 |
| `chunk-12xea136.js` | `===windows` ×1 |
| `chunk-194chm4z.js` | `===macos` ×1 |
| `chunk-1d7tfvz6.js` | `!==wsl` ×1 |
| `chunk-1fznv4kd.js` | `!==macos` ×1 |
| `chunk-1neynjgw.js` | `switch(windows/macos/default)` ×1 |
| `chunk-1wesbfvz.js` | `other` ×2 |
| `chunk-2bjf1wyg.js` | `===macos` ×2, `===windows` ×2 |
| `chunk-2czz3v20.js` | `arg` ×1 |
| `chunk-2hxx68dq.js` | `===windows` ×3 |
| `chunk-2nxj6raj.js` | `!==windows` ×3, `===macos` ×4, `===windows` ×3, `assigned` ×1 |
| `chunk-35kgy2mf.js` | `===windows` ×1 |
| `chunk-3c9k06rp.js` | `===windows` ×1 |
| `chunk-3wepgp9f.js` | `===windows` ×1 |
| `chunk-3y29hmne.js` | `===windows` ×1 |
| `chunk-47hjfbd4.js` | `===windows` ×1 |
| `chunk-4hr1rwfw.js` | `===windows` ×4 |
| `chunk-4jdxnp8t.js` | `!==macos` ×1, `===macos` ×1, `===windows` ×1, `assigned` ×2 |
| `chunk-4tdy2bwk.js` | `===windows` ×1 |
| `chunk-575jccbr.js` | `!==windows` ×1, `===windows` ×1 |
| `chunk-5d3v9q6g.js` | `===windows` ×5 |
| `chunk-5mcxmbtx.js` | `!==windows` ×1 |
| `chunk-64d7mf79.js` | `===windows` ×1 |
| `chunk-6b7sh4wc.js` | `===windows` ×1, `arg` ×1 |
| `chunk-6h8148nz.js` | `!==macos` ×1, `!==windows` ×1, `!==wsl` ×1 |
| `chunk-6kgjqqds.js` | `===windows` ×1 |
| `chunk-6nh2mjmf.js` | `!==windows` ×5, `!==wsl` ×1, `===macos` ×5, `===windows` ×20, `===wsl` ×3, `arg` ×1, `assigned` ×1, `other` ×1, `switch(windows/macos/default)` ×1 |
| `chunk-6s5ezrj7.js` | `arg` ×1 |
| `chunk-6x8mfmge.js` | `assigned` ×1, `other` ×3 |
| `chunk-6z8cx01q.js` | `assigned` ×1 |
| `chunk-74640vqc.js` | `===windows` ×1 |
| `chunk-74z2fna5.js` | `other` ×1 |
| `chunk-7b09tcre.js` | `!==windows` ×6, `!==wsl` ×3, `===windows` ×10, `===wsl` ×3 |
| `chunk-7p0kyv0j.js` | `!==windows` ×2 |
| `chunk-7rhy9s5j.js` | `===windows` ×1 |
| `chunk-7s6m7ema.js` | `===windows` ×1 |
| `chunk-88jnpzfk.js` | `switch(macos/windows/default)` ×1 |
| `chunk-8bwkgqrb.js` | `===windows` ×2, `assigned` ×1 |
| `chunk-8g9dfmp7.js` | `===macos` ×1, `===wsl` ×1 |
| `chunk-90zad8kv.js` | `===windows` ×1 |
| `chunk-92j5bc0m.js` | `===windows` ×1 |
| `chunk-953zb56b.js` | `===windows` ×9 |
| `chunk-985cqpjs.js` | `!==macos` ×2, `!==windows` ×1, `===windows` ×1, `arg` ×1 |
| `chunk-9deh4a76.js` | `===macos` ×2, `===wsl` ×1 |
| `chunk-9hawyp34.js` | `!==wsl` ×1, `===macos` ×2, `===windows` ×6, `assigned` ×1 |
| `chunk-9m4nq254.js` | `!==windows` ×1, `other` ×2 |
| `chunk-9mtd6pd5.js` | `!==windows` ×1 |
| `chunk-9nyp7h4s.js` | `===windows` ×2 |
| `chunk-9r0k6z4c.js` | `===windows` ×2 |
| `chunk-a4v6yarn.js` | `arg` ×1 |
| `chunk-a6cj61xj.js` | `!==windows` ×1, `===macos` ×2, `===windows` ×1, `===wsl` ×3, `arg` ×1, `assigned` ×2, `switch(macos/windows/default)` ×1 |
| `chunk-aa47jmf3.js` | `assigned` ×1 |
| `chunk-ajzzvbct.js` | `===windows` ×2 |
| `chunk-art4a89f.js` | `arg` ×1, `assigned` ×1 |
| `chunk-awz2eqb7.js` | `!==windows` ×2 |
| `chunk-b1mveh2c.js` | `!==windows` ×1, `===windows` ×1 |
| `chunk-bk9vg3xq.js` | `===windows` ×2 |
| `chunk-bnaa6xmh.js` | `===windows` ×2 |
| `chunk-bnx5gvrh.js` | `===windows` ×1 |
| `chunk-bxrsjz9y.js` | `!==windows` ×2 |
| `chunk-c6739ezc.js` | `===windows` ×2 |
| `chunk-c83f1ekr.js` | `other` ×1 |
| `chunk-cb9vrsg5.js` | `!==macos` ×1 |
| `chunk-cba2p5gt.js` | `===macos` ×2, `===windows` ×1 |
| `chunk-ccp8dwcb.js` | `===macos` ×1 |
| `chunk-czrq4nwx.js` | `===windows` ×4 |
| `chunk-d29s8rqd.js` | `!==macos` ×2, `===macos` ×1, `===windows` ×1 |
| `chunk-ddrd3r4k.js` | `===windows` ×1, `member` ×1 |
| `chunk-e5fpjs0t.js` | `===windows` ×2 |
| `chunk-e9mw2p2g.js` | `!==windows` ×1, `===windows` ×4 |
| `chunk-ef9gz064.js` | `===wsl` ×3 |
| `chunk-esy1t47y.js` | `!==wsl` ×1 |
| `chunk-f04qse8h.js` | `assigned` ×1 |
| `chunk-f15c0tt7.js` | `===windows` ×2 |
| `chunk-f4p7gttp.js` | `!==windows` ×1 |
| `chunk-f4yvc5wd.js` | `!==windows` ×1, `===windows` ×5 |
| `chunk-f6ketk18.js` | `assigned` ×1 |
| `chunk-fejj39g0.js` | `!==windows` ×4, `===macos` ×3, `===windows` ×40, `===wsl` ×1, `assigned` ×3, `other` ×1, `switch(macos/windows/wsl/default)` ×1 |
| `chunk-fgccedbt.js` | `!==windows` ×1, `===macos` ×1, `===windows` ×3, `assigned` ×3 |
| `chunk-fhyjn0pr.js` | `===windows` ×2, `assigned` ×1 |
| `chunk-fnf8eq08.js` | `!==windows` ×1, `===windows` ×4, `switch(macos/windows/default)` ×1 |
| `chunk-g2ed4z08.js` | `===windows` ×5 |
| `chunk-g8415bcb.js` | `===windows` ×2 |
| `chunk-ghtd0158.js` | `===windows` ×1 |
| `chunk-gs173es5.js` | `===windows` ×3, `===wsl` ×1, `assigned` ×1 |
| `chunk-h0n0s6nm.js` | `===windows` ×1 |
| `chunk-h606cxdh.js` | `!==windows` ×1 |
| `chunk-h8fxxarj.js` | `!==windows` ×1, `===windows` ×1 |
| `chunk-hkht2zmt.js` | `assigned` ×2 |
| `chunk-hmg1wxg5.js` | `===windows` ×1 |
| `chunk-hv1b2wbq.js` | `assigned` ×1 |
| `chunk-j5ajyzcc.js` | `!==windows` ×2, `===windows` ×4, `switch(windows/macos/default)` ×1 |
| `chunk-jv7v8cxs.js` | `===windows` ×1 |
| `chunk-jvv1g8eh.js` | `===windows` ×2, `arg` ×1 |
| `chunk-jwstday3.js` | `!==windows` ×4, `===windows` ×3 |
| `chunk-k3xv1gfh.js` | `===windows` ×2 |
| `chunk-k40pmb1v.js` | `===windows` ×1, `assigned` ×1 |
| `chunk-k9ag8dg7.js` | `!==windows` ×1 |
| `chunk-kd5xgfzm.js` | `assigned` ×2 |
| `chunk-kdwj4kwy.js` | `===macos` ×1 |
| `chunk-kgv4sgj7.js` | `===windows` ×1 |
| `chunk-m2rdp425.js` | `===windows` ×5, `arg` ×5 |
| `chunk-m3ffpq7v.js` | `===windows` ×3 |
| `chunk-mhbfazcy.js` | `!==wsl` ×1 |
| `chunk-mq4wyke8.js` | `===windows` ×1, `arg` ×1 |
| `chunk-n0vk7gyp.js` | `===windows` ×3, `arg` ×1 |
| `chunk-n1vwtt63.js` | `===windows` ×3 |
| `chunk-n4peae6z.js` | `assigned` ×1 |
| `chunk-naezb2sv.js` | `===macos` ×1 |
| `chunk-nh9cpt34.js` | `===macos` ×1, `===wsl` ×1, `assigned` ×1 |
| `chunk-nmdw6kba.js` | `!==macos` ×1 |
| `chunk-npkjmyqf.js` | `===windows` ×1 |
| `chunk-nq1va7m4.js` | `===windows` ×1 |
| `chunk-p1je9wt1.js` | `other` ×1 |
| `chunk-p2kpmyaa.js` | `===windows` ×1 |
| `chunk-peczkv0v.js` | `===wsl` ×1 |
| `chunk-pfrw0e73.js` | `===windows` ×1 |
| `chunk-pm946ns2.js` | `===windows` ×1 |
| `chunk-pyn8k20s.js` | `===macos` ×1, `assigned` ×1 |
| `chunk-qbrctfv6.js` | `===windows` ×1 |
| `chunk-qk4q5mdy.js` | `!==windows` ×1, `===windows` ×7, `===wsl` ×2, `assigned` ×3, `other` ×1 |
| `chunk-qtx2p6mn.js` | `===windows` ×2 |
| `chunk-qyxhe6nc.js` | `!==windows` ×3, `===windows` ×11 |
| `chunk-r3z5k2q8.js` | `===windows` ×2 |
| `chunk-s0dw5891.js` | `===wsl` ×1 |
| `chunk-s24py72n.js` | `!==windows` ×1 |
| `chunk-s7yyd1p6.js` | `===windows` ×2 |
| `chunk-scm0dxjb.js` | `===macos` ×2, `===windows` ×1 |
| `chunk-snxq5vgw.js` | `===windows` ×6, `assigned` ×1 |
| `chunk-t5jayd9k.js` | `!==windows` ×1, `===windows` ×4 |
| `chunk-t6geav6k.js` | `!==macos` ×1 |
| `chunk-tcztx3fg.js` | `===wsl` ×1 |
| `chunk-tdm998wz.js` | `===windows` ×2 |
| `chunk-thjjmes7.js` | `===windows` ×2 |
| `chunk-tsykv7e0.js` | `===windows` ×1 |
| `chunk-v0b8hfja.js` | `!==macos` ×2, `!==windows` ×17, `===macos` ×10, `===windows` ×62, `===wsl` ×8, `assigned` ×6, `other` ×1 |
| `chunk-v5xftfk7.js` | `===windows` ×1 |
| `chunk-vgtbx9sp.js` | `===windows` ×4 |
| `chunk-w5ekmedb.js` | `===windows` ×1 |
| `chunk-w6mjdp2k.js` | `===wsl` ×1 |
| `chunk-wdxqgzha.js` | `!==windows` ×1 |
| `chunk-wewsndh6.js` | `===windows` ×1, `arg` ×1 |
| `chunk-wg6ymv3p.js` | `!==macos` ×1, `!==windows` ×1 |
| `chunk-wkcv70t3.js` | `===windows` ×1 |
| `chunk-wrmepk7d.js` | `===windows` ×1 |
| `chunk-wv3sjy7g.js` | `!==windows` ×3, `===windows` ×4 |
| `chunk-x3grmvv3.js` | `!==windows` ×1, `===windows` ×4 |
| `chunk-x42qr0ry.js` | `!==windows` ×4, `!==wsl` ×11, `===macos` ×2, `===windows` ×19, `===wsl` ×6, `other` ×1 |
| `chunk-x65c4var.js` | `===windows` ×4 |
| `chunk-xazfnezz.js` | `!==windows` ×1, `===windows` ×2 |
| `chunk-xnnp3y57.js` | `===macos` ×1, `===windows` ×16, `assigned` ×1 |
| `chunk-y0gwbrd9.js` | `===windows` ×1 |
| `chunk-y2b13z76.js` | `other` ×1 |
| `chunk-y760q7xq.js` | `===windows` ×1 |
| `chunk-y8z88azs.js` | `===windows` ×1, `arg` ×2 |
| `chunk-yeqe6mfj.js` | `!==macos` ×1, `!==windows` ×3, `===macos` ×2, `===windows` ×8 |
| `chunk-yhsj03zd.js` | `!==windows` ×7, `===windows` ×1 |
| `chunk-ywbhxc0w.js` | `===windows` ×1, `other` ×1 |
| `chunk-z35r3jkk.js` | `===windows` ×2 |
| `chunk-ze4kehan.js` | `assigned` ×1, `other` ×1 |
| `chunk-zeq8h6rm.js` | `===windows` ×2 |
| `chunk-znedvwcw.js` | `!==windows` ×2, `===macos` ×1, `===windows` ×2 |
| `chunk-zv5y38vw.js` | `!==windows` ×3, `===macos` ×2, `===windows` ×7, `assigned` ×1 |
| `chunk-zy2k3hny.js` | `!==macos` ×2, `!==windows` ×21, `!==wsl` ×3, `===macos` ×11, `===windows` ×99, `===wsl` ×2, `arg` ×7, `assigned` ×1, `other` ×5 |

</details>

## Raw `process.platform` / `os.platform()` / `os.type()` after P4

123 reads remain (104 `process.platform`, 16 `os.platform()`, 3 `os.type()`). On Haiku
they all behave like the generic POSIX case, except the following:

| Module · function | Read | Behaviour for `"haiku"` |
|---|---|---|
| `chunk-8dvz2fjk.js` main host | `new u({platform:process.platform,…})` | feeds `M()`, which gives `"haiku"` (P5) |
| `chunk-jwstday3.js` `Fv` | switch | `"haiku"` (P3) |
| `chunk-4hr1rwfw.js` env record | `["win32","darwin"].includes(…)?…:"linux"` | `linux` by upstream design (documented) |
| `chunk-fgccedbt.js`@866799 | `platformRaw:process.env.CLAUDE_CODE_HOST_PLATFORM\|\|process.platform` | `"haiku"` |
| `chunk-tcztx3fg.js` `H`/`oe` | `switch(process.platform){case"darwin":case"linux":case"win32":return process.platform;default:return}` | undefined, so the environment tool inventory (`E_o`) returns early |
| `chunk-wfe5p2va.js` `r` | `t!=="darwin"&&t!=="linux"&&t!=="win32"` | audio addon null (S04) |
| `chunk-ef9gz064.js` ×3 | `n[process.platform]\|\|n.linux` | Linux commands (S17) |
| `chunk-bfxj2kma.js` `M`, `y` | switch, `default:return null`/`!1` | S37 |
| `chunk-fejj39g0.js` `Rw`, `chunk-8tqbvyg9.js` `T` | switch | S36 / no Chrome paths |
| `chunk-nyjcftf8.js` `x`, `v`, guard | `if(!["darwin","linux","win32"].includes(process.platform))return;` | deep-link registration skipped before `x`'s `Unsupported platform: haiku` throw can be reached |
| `chunk-n8d1bv23.js` `h` | `default:` gives the darwin DMG URL | Claude Desktop download link is the macOS one (cosmetic) |
| `chunk-aa47jmf3.js` `m` | `n=process.platform` (unused) | S15 |
| `chunk-j1xyn82g.js` | `process.platform in c.platform_overrides` | MCPB manifests may key `haiku` |
| `chunk-4r1py4tt.js` (MSAL/Azure) | `$E[process.platform]`, `if(!Gf.hasOwnProperty(process.platform))throw` | Azure Arc managed identity throws its "unsupported platform" error; IMDS path absent |
| `chunk-9a0z9ab4.js` (watcher lib) | `Wt=W==="linux"` | non-Linux watcher behaviour (S29) |
| `chunk-zy2k3hny.js` `BTn()` (`os.platform()`) | JetBrains plugin dirs | no case, empty list |
| `chunk-hezf65a0.js` `z` | `switch(u())` darwin/win32 | computer name from the default branch |
| `chunk-zy2k3hny.js`@106774 (`os.type()`) | `` `${Cro()} ${a_t()}` `` | `Haiku <release>` in the model's environment info |
| `chunk-yt5m1rse.js` `nTo`, `chunk-92j5bc0m.js`/`chunk-rwkm017b.js` `G1e(process.platform)` | display names | `Haiku` (P6/P7) |
| `chunk-xrt4j2zq.js` `Lt` | SDK binary dir | S31 |
| `chunk-070nc6an.js`, `chunk-zv5y38vw.js` | message text | `… not available on haiku …` |

All the other reads compare against `"win32"` or `"darwin"` only: path separators, case
folding, `O_NOFOLLOW` flags, shell-profile choice, `PATH` key, IPC length,
`GRACEFUL_FS_PLATFORM`. On Haiku they take the POSIX/Linux value, which is correct there
(BFS is case-sensitive and the paths are POSIX).

## Unguarded Linux-only shell-outs

| Tool | Where | Guard | Haiku |
|---|---|---|---|
| `systemctl` | `chunk-070nc6an.js` `ven`/`ft`/`G0t`/`xre` | callers check `uXe()` (S03) | refused with a message |
| `loginctl` hint, `/etc/systemd/logind.conf` | `chunk-070nc6an.js` `Kn` | `r!=="linux"&&r!=="wsl"` | skipped |
| `xdg-open` | `chunk-aa47jmf3.js` `m`/`Zvn`/`h` (**unguarded**), `chunk-wj68m7s3.js` `k` (guard folded), `chunk-5ck0cv3e.js` (`open` package, MSAL only, `wait:true` so the error rejects) | none | `opener_missing` (S15, S16) |
| `xdg-mime` | `chunk-wj68m7s3.js` `D` (guard folded), `chunk-5ck0cv3e.js` `B` (`E.platform==="linux"`) | partly | false / throws `Only macOS, Linux, and Windows are supported` (MSAL default-browser lookup) |
| `ps -A -o pid= -o ppid=…` | `chunk-fgccedbt.js` `HL` (**unguarded**, Bash kill tree), `chunk-2hxx68dq.js` `Wn`/`jn` (non-Linux branch) | none | empty result (S21) |
| `ps -o lstart= -p` | `chunk-6h8148nz.js` `b`/`G3e`/`eIe` | dead code (folded `/proc` block runs first) | never reached |
| `ps -o ppid= -p` (shell loop), `ps -o command= -p` | `chunk-6h8148nz.js` `tgr`, `mas` (**unguarded**) | none | `readFailed`/`null` |
| `ps -ww -o args= -p` | `chunk-xyt7m0qn.js` `q0s` | after `/proc` fails | false (S22) |
| `ps -o stat= -p`, `ps -o command=`/`ppid=` | `chunk-6h8148nz.js` `$lt`, `chunk-zy2k3hny.js` `ZEr` | `M()==="macos"` | not reached |
| `ps aux \| grep …` | `chunk-zy2k3hny.js` `tRr` | macOS branch | not reached |
| `notify-send` | `chunk-wg6ymv3p.js` | none (also runs on macOS) | silent (S23) |
| `arecord`, `apt-get`/`dnf`/`pacman` probes | `chunk-w6mjdp2k.js` | none | not found (S04) |
| `bwrap`, `socat`, seccomp helper | `chunk-v0b8hfja.js`, `chunk-zy2k3hny.js` `vWe`/`SRo` | `Fv()==="linux"` / sandbox enabled | not reached |
| `nc -N -U` | `chunk-74z2fna5.js` (prompt text) | none | absent (S35) |
| `ip route show` | `chunk-zy2k3hny.js` IDE host | `M()!=="wsl"` returns early | not reached |
| `pwsh` snap probe | `chunk-7b09tcre.js` | `M()==="linux"` | skipped |
| `sudo apt …` hints | `chunk-0ane94zt.js` (S01), `chunk-w6mjdp2k.js` (S04), `chunk-k1pd3pap.js`/`chunk-zy2k3hny.js` (`poppler-utils`), tmux hints | text only | shown verbatim (PDF hint names brew/apt only) |
| `lsof`, `ss`, `netstat`, `systemctl`, `apt`… in `chunk-zy2k3hny.js` | command-classifier tables | not spawned | n/a |

## `Bun.ant` call sites

`bun-compat.mjs` defines no `Bun.ant`, so `Bun.ant` is `undefined`.

| Member | Site | Guard | Haiku/Node |
|---|---|---|---|
| `CellSegmenter` | `chunk-pmgbk1rv.js` `d7n` ← `chunk-x65c4var.js` screen buffer, `chunk-zy2k3hny.js` `UBt` | throws on purpose | **TUI breaks** (S25) |
| `getPeerUid`, `getPeerPid` | `chunk-0p7kejg5.js` `frr`, `UEt` | `try…catch` + warn log | peer uid/pid unknown: `[peer-cred] peer pid unavailable` |
| `getPeerPid` feature probe | `chunk-fgccedbt.js` `DV` | `typeof Bun.ant?.getPeerPid==="function"` | capability not advertised |
| `memoryPressureLevel` | `chunk-1fznv4kd.js` `m` | `try…catch` + log | `lowMem:false` |
| `windowsProcessCreationTime`, `windowsProcessList` | `chunk-6h8148nz.js` `uas`, `chunk-fgccedbt.js` `xf`/`sbi` | `try`/`typeof` | not reached off Windows |
| `ttyName` | `chunk-ky5pek6j.js` `Y` | `try{…}catch{return}` | macOS branch only |
| `loginTty` | `chunk-zy2k3hny.js` `t4s` | `try…catch` → `"ffi…"` | bg controlling-tty unavailable (logged) |
| `posixFsAt` | `chunk-hv6m45s7.js` `Rn` | `globalThis.Bun?.ant?.posixFsAt` | macOS anchor backend only |
| `setDumpable` | `chunk-xn5zep1w.js` | `try…catch` → `prctl unavailable: …` | not dumpable-protected (logged) |
| `windowsFolderSecurity`, `windowsSandboxDeniesArg` | `chunk-v0b8hfja.js` `uM`, `kM` | `M()==="windows"` and `?.` | not reached |

## Proposed haiku implementations (class b)

These are now implemented as PATCHES.md P8–P17. P13–P15 follow the descriptions
below. P17's helper prints `team parent group session uid start_us start_epoch_us args`.
The original proposals are kept for the record.

Each one is a build-time rewrite in the style of `tools/apply-haiku-names.py`: an exact
match count per site, then an acorn parse.

- **S20 `iqn` (P8, do first, it fixes a P5 regression).** In `chunk-hv6m45s7.js`, make
  the switch read `case"haiku":case"unknown":return{unavailable:"unsupported_platform"}`.
  Every caller already handles `unavailable`: worktree code reports `unanchored`, and the
  trash sweep counts the directory as skipped. A real anchored backend would need
  `openat`-style calls, which Node doesn't expose.
- **S15 URL/file opener.** In `chunk-aa47jmf3.js` `m`, `Zvn` and `h`, use `"open"` when
  `process.platform==="haiku"`. Haiku's `/bin/open` opens URLs through
  `BUrl(*argv,true).OpenWithPreferredApplication()` (`src/bin/open.cpp:99-100`) and files
  through the roster. Leave `p()` alone. Users can already get this today with `BROWSER=open`.
- **S18 text clipboard.** In `chunk-9deh4a76.js`: `Z` gets `case"haiku":return it("clipboard",["-i"],r),!0`
  (reads stdin: `src/bin/clipboard.cpp:243`, `AddData("text/plain"…)`). `xGt` gets
  `case"haiku":{let n=await it("clipboard",["-p"],r);return n.code===0?n.stdout:""}`
  (prints `text/plain`, `:320`). `Sbe` returns `"native"` for haiku. `clipboard` needs a
  running registrar/app_server. On a headless SSM-only instance that has to be checked.
- **S06/S21/S22/S32 team identity and tree.** Haiku has no CLI that prints a team's
  parent or start time: `ps -o` accepts only `Team Id Threads Gid Uid` (`src/bin/ps.c`).
  `team_info` does carry both, `team_id parent` and `bigtime_t start_time`
  (`headers/os/kernel/OS.h:258-260`). The minimal fix:
  - A ~40-line native helper shipped with the port, `cc-teaminfo`. `cc-teaminfo <pid>…`
    prints `pid parent group start_time` through `get_team_info`; `cc-teaminfo -A`
    walks `get_next_team_info`.
  - Haiku branches that use it: `chunk-6h8148nz.js` `b`/`G3e`/`eIe` (start token =
    `start_time`), `fas`/`tgr` (ancestry via `parent`), `chunk-fgccedbt.js` `HL`
    (`pid ppid` lines in the format `Rf` already parses), and `chunk-2hxx68dq.js` `Wn`/`jn`.
  - `rjn` (S22) should answer false, not true, when it can't read.
  - An N-API addon is the in-process alternative.
- **S10 fd→path without `/proc`.** In `chunk-k1pd3pap.js` `wIn`, skip the `/proc` readlink
  on haiku and fall through to the tail that is already there (`realpath(r)===r` after the
  dev/ino `lstat` match), which is the darwin shape. In `chunk-v5xftfk7.js` `Me`, use
  `realpath(e)` and require `stat` dev/ino to match the open handle. In `chunk-s7yyd1p6.js`
  `kio`/`vio` on haiku, take the existing `t9e(p,o)` direct-descriptor read rather than
  `/proc/self/fd/<p>`. `chunk-7hp2kn2d.js` (remote op behind `M()!=="linux"`) stays refused
  unless rewritten the same way.
- **S11 served files.** In `chunk-12xea136.js` `ye`/`qo`, return `path:n` (the verified
  real path) and replace `qo` with "fstat(handle) dev/ino equals stat(n) dev/ino".
- **S12 directory chain.** In `chunk-m6kk55rf.js`, `s(o)` returns the joined real path
  instead of `/proc/self/fd/N`. Each hop re-checks with `fstat` dev/ino against
  `lstat(path)` (the ownership checks already use `stat`). This is weaker than an fd-held
  chain. Haiku would need `openat`, which Node lacks.
- **S23 notifications.** In `chunk-wg6ymv3p.js`, haiku runs
  `notify --title Claude "Your Claude assistant needs re-authentication"`
  (`src/bin/notify.cpp` usage: `--title <title>` and `[MESSAGE]`).
- **S24 path length.** In `chunk-ddrd3r4k.js`, `ir` gets `haiku:1024` (`PATH_MAX` from
  `headers/posix/limits.h:44`).
- **S25 `Bun.ant.CellSegmenter`.** This is a compat-layer member, not a Haiku patch. The
  contract is defined by `d7n`'s options (`ambiguousIsNarrow`, `substitute`, `screen`) and
  the `segment`/`setCell`/`graphemes`/`sgrKeys` uses in `chunk-x65c4var.js` and
  `chunk-zy2k3hny.js`. Port it from Anthropic's Bun fork if its source is available; if not,
  write it from that contract and diff-test it against the Linux reference binary's
  rendered output. Until then, measure `--print` versus the interactive TUI under Node.

## Open questions (need a DeBeOS run)

1. ~~Does `fs.watch` work on Haiku?~~ **Answered (measured): no.** It throws ENOSYS
   (`errno -2147454967`, syscall `watch`), evidence
   `evidence/20261031T080733Z_i-0e7bed4928e0f7bf7_node24-accept-haiku.log`. Per-site effects
   are in "File watching on Haiku". Still open: does `fs.watchFile` (stat polling) work?
   The polling sites depend on it.
2. Does `clipboard` work without a desktop session (SSM-only Graviton)?
3. Is the interactive TUI reachable at all without `Bun.ant.CellSegmenter` (S25)? Run it
   on the Linux reference box under Node first.
4. Haiku's default user and uid: does the 0600 mode on `~/.claude/.credentials.json`
   (S14) separate anything?
