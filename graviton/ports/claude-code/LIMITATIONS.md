# LIMITATIONS — what does not work, or does not match Bun, and why

Each entry: what, why, observable effect in Claude Code, and what a real fix would need.
Entries come from measurement (diff tests against stock Bun 1.4.4, runs on DeBeOS);
nothing here is a guess.

## Bun API compat (Node 24)

### `Bun.YAML.stringify`: `+0`
- **What:** Bun prints `+0` for a zero that JSC holds as a double (`let h=0.5; h-h`,
  `[1.5, 0]`, `new Float64Array([0])`); the port prints `0`.
- **Why:** JSC's int/double representation is not observable from V8. It isn't stable
  even inside Bun: the same array printed `- +0` in one mode and `0` in the other.
- **Effect:** frontmatter written by Claude Code could contain `0` where Bun would write
  `+0`. Both parse to the number 0. Frontmatter values are user text, so in practice this
  doesn't arise.
- **Fix would need:** nothing portable. JSC's internal number tagging can't be read from JS.

### `Bun.YAML.stringify`: engine error wording
- **What:** when the *engine* throws while reading a value (a revoked Proxy, a boxed
  Number whose `valueOf` returns a BigInt), the message is V8's, not JSC's. Errors thrown
  by user code (getters, `valueOf`) pass through unchanged.
- **Effect:** none on Claude Code's own data (plain frontmatter objects).
- **Fix would need:** a table mapping V8 to JSC messages per error site. Not attempted.

### `Bun.YAML.stringify`: maximum nesting depth
- **What:** the line-for-line recursive port overflows at ~1,950 levels on Node 24's
  default stack; Bun reaches ~23,400. Between the two, Bun returns output and the port
  throws `RangeError("Maximum call stack size exceeded.")` (JSC's wording, matched).
- **Effect:** none expected; frontmatter is shallow.
- **Fix would need:** an iterative rewrite of the walk (a deliberate departure from the
  line-for-line port), or a larger `--stack-size`.

### `Bun.zstdDecompressSync` / `zstdDecompress`: error message
- **What:** corrupt input throws an `Error` in both runtimes, but the text differs
  (Bun: `Decompression failed: InvalidZstdData`; Node: zlib's message).
- **Effect:** the bundle wraps it (`embedded asset is missing or corrupt`), so it isn't
  visible.
- **Fix would need:** a map from zlib zstd error codes to Bun's names.

### `Bun.TOML.parse`: dates and times are strings, not Temporal objects
- **What:** Bun 1.4.4 returns `Temporal.Instant`/`PlainDateTime`/`PlainDate`/`PlainTime`.
  Node 24 has no `Temporal` by default, so the port returns the string Bun's object
  stringifies to (Temporal canonical form, ported: offsets folded to UTC, leap second
  clamped, fraction ≤ 9 digits). `String(x)` is identical; `typeof` is not.
- **Effect:** the bundle's only TOML input is another agent's `config.toml` import, which
  it validates with zod; that schema has no date fields. Not observable in Claude Code
  as far as the call site shows.
- **Fix would need:** Node 24's V8 13.6 has Temporal behind `--harmony-temporal` ("in
  progress / experimental", measured: `typeof Temporal` = `object` with the flag). The
  port already exposes `parseWith(input, onDateTime)` to construct real Temporal objects.
  That would make an experimental engine flag part of the runtime. Owner's call.

### `Bun.TOML.parse`: Blob input, error-object extras, depth
- **Blob:** Bun reads a `Blob` argument synchronously; Node can't, so the port throws a
  `TypeError` saying to pass `await blob.text()`. The bundle passes file text.
- **Error objects:** type, message and `code` match (`SyntaxError "TOML Parse error: …"`,
  `ERR_INVALID_ARG_TYPE`, `ERR_OUT_OF_RANGE`). JSC adds `line`/`column`/`sourceURL` to
  error objects and V8 doesn't; `code` is an own non-enumerable property here and not own in Bun.
- **Depth:** Bun overflows its native stack on very deep nesting (inline tables > ~10k,
  arrays > ~20k, headers > ~50k); the port uses an explicit stack (toml.rs's own
  non-recursive structure) and parses those. Ten named vectors cover this, and the checker
  fails if any of them starts matching.

### `Bun.spawn`: features refused explicitly
Each throws `… is not supported by the Node compat layer` instead of silently changing
behaviour; the diff test (213/218 match) requires exactly that throw for these five:
- **`terminal`** (a PTY via `Bun.Terminal` or Terminal options): Node has no PTY API, and
  compat has no `Bun.Terminal`. Effect: Claude Code's background-agent PTY host
  (`--bg-pty-host`, `chunk-yeqe6mfj.js`/`chunk-yhsj03zd.js`) can't run; the bundle reports
  "Bun.Terminal unavailable (running under Node?)" for it. **Background agents that need
  a PTY are unavailable.** A fix would need a PTY addon built for Haiku (forkpty is in
  libroot/libbsd) exposed as `Bun.Terminal`.
- **`cgroup`** (Linux only; ignored elsewhere, like Bun on non-cgroup platforms). The bundle
  passes it only on linux/wsl, so on Haiku it's never reached.
- **`ipc`** callback, **`"pipe"` at stdio index ≥ 3**, **`resourceUsage()`** after exit: the
  bundle doesn't use them.

### `Bun.spawn`: other measured differences
- A **relative** `Bun.file(path)` used as a stdio target is resolved against the
  directory current when `Bun.file()` was called; Bun opens it in the child after its
  chdir. Differs only for a relative path combined with `cwd`; the bundle passes absolute
  paths.
- The implementation relies on two undocumented Node 24 internals
  (`ChildProcess.prototype.spawn` for a synchronous spawn errno, `child._handle.kill`).
  Pinned by the Node version; re-check on a Node upgrade.

### `Bun.serve` / `listen` / `connect`
- **Response defaults (the 1 of 42 mismatch):** Node's `Response` sets its default
  `content-type` when the object is built: `text/plain;charset=UTF-8` vs Bun's
  `charset=utf-8`; `application/json` vs Bun's `application/json;charset=utf-8`. It also
  sets it on HEAD, where Bun omits it. That is Node's fetch implementation, not the
  net layer, which can't tell a default from a header the handler set. Clients compare
  media types case-insensitively, so the effect on Claude Code's local servers is nil.
- **Refused explicitly** (throw "not implemented"): `serve` with `routes`/`static`,
  `websocket`, `unix`, `baseURI`, `app`, `http2`/`http3`, `http1:false`, or tls keys beyond
  cert/key/ca/passphrase/requestCert/rejectUnauthorized; `listen`/`connect` with `tls`,
  `unix`, `fd`; writing a Blob to a socket. No bundle call site uses them.
- **`hostname` is required**: Bun binds every interface when it is omitted; the port throws
  instead of guessing. Every bundle call site passes it.
- **Dev error page / stderr wording**: status, content-type and the fact that a page is
  served match; the HTML and Bun's stderr formatting are not byte-identical.
- **Not yet run on Haiku:** the `SO_REUSEPORT` fallback (`development:false`; libuv
  ENOTSUP expected on Haiku → bind without it, as uSockets does), dual-stack requestIP.
- **Absent members** (never touched by the bundle): Server `fetch`/`upgrade`/`publish`/
  `reload`/`timeout`/…, Listener `reload`/`fd`/…, Socket `flush`/`shutdown`/TLS getters/….
  The full list is in `compat/lib/net.mjs`'s header.

### Self re-exec under Node: resolved (PATCHES.md L1)
The 26 relaunch sites now come back through `claude.mjs`, which loads compat and
restores `--zero-fill-buffers`. Verified on the Linux box: flag-less relaunches of
`--version`, `doctor --help` and `mcp --help` give output identical to Bun's. Left over:
the one site passing Bun's `--smol` (`chunk-7hh7yp6n.js`) belongs to a `Bun.build` plugin
(`hF`). No module calls `Bun.build`, and it is absent in compat, so the site is
unreachable. `process.execve` on Haiku has to be confirmed on the Node 24.21.0 build.

### Global `Worker`
- **Uncaught-error message:** Bun's `ErrorEvent.message` embeds a formatted source excerpt
  and stack; the port's is `Name: message`. Type, class, and the fact that the worker
  keeps running all match (the diff test compares the message by that line).
- **Refused explicitly:** options other than `name`/`workerData`/`type` (Bun's `preload`,
  `smol`, `env`, `argv`, …) and relative or non-`file:` URLs. No bundle call site uses
  them, except node-forge's optional prime-generation workers (`forge/prime.worker.js`,
  relative), which are reached only when a caller passes `workers`. The bundle doesn't.

### Function hooks written in TypeScript
`chunk-7hh7yp6n.js` `our()` transpiles `.ts` hook modules with `new Bun.Transpiler({loader})`,
which is absent. It throws inside a try and is re-raised as the hook's load error, so a
TypeScript function hook fails to load with "Bun.Transpiler is not a constructor". JS hooks
are unaffected. A fix would need Bun's transpiler semantics (TS, TSX, decorators).
Node's `module.stripTypeScriptTypes` covers only part of that and isn't output-compatible,
so it isn't offered as a substitute.

### `Bun.YAML.parse`: depth limits and inputs over 2 GiB
- **Depth:** Bun's nesting limit is a native-stack check that differs per construct (deepest
  accepted: flow seq 4723, flow map 4515, compact seq 4085, block map 3364). The port
  reproduces those four exactly with a per-construct cost model fitted to stock Bun, and
  re-runs a parse that overflows Node's default stack on a worker thread with a 32 MB stack,
  waited on synchronously. Bun itself wavers by one level at the limit, so inputs exactly
  there can't match deterministically; limits for untested construct mixes are estimates.
  Not yet run on Haiku (needs `worker_threads` and a 32 MB thread stack there).
- **> 2 GiB input:** Bun throws a `RangeError` naming `input.byteLength`; not implemented.

## Bun APIs left absent (stock Bun 1.4.4 has them; measured on the Linux box)

Each is absent from compat, so the bundle's own feature detection sees the truth.
Call sites and the behaviour that follows, from the relocated tree:

- **`Bun.Image`** (decode/resize/re-encode). `chunk-9vt0p3th.js` `je()` checks
  `typeof Bun.Image!=="function"` and returns an image whose every operation rejects with
  `ERR_IMAGE_UNAVAILABLE`. Bridge attachments (`chunk-nmdw6kba.js`) log "could not make a
  preview copy … uploading the original". The effect on the Read tool for images over the
  API size limit is **to be measured** in acceptance (needs the API key). A fix would need
  image codecs (JPEG/PNG/WebP decode and encode, resize). On Node that means a native
  library (libvips/sharp) built for Haiku, or pure-JS codecs, whose output can't be
  byte-identical to Bun's.
- **`Bun.WebView`**: guarded by `"WebView" in Bun` (`chunk-n77pfb2e.js`); the web-browser
  view is unavailable.
- **`Bun.JSONL`**: `chunk-41hx5waz.js` `c1e()` uses the bundle's own JS JSONL parser when
  `Bun.JSONL?.parseChunk` is absent; both skip unparseable lines. No observable difference
  is expected; not separately diff-tested.
- **`Bun.isStandaloneExecutable`**: absent, so `rp()` is false, which is true: Claude Code
  isn't a standalone binary here. That selects the bundle's non-compiled paths
  (relaunch via `argv[1]`, PATCHES.md L1).
- **`Bun.Terminal`**, **`Bun.Transpiler`**: see the spawn and function-hook entries above.

## Platform audit (in progress)

### Environment metadata reports `linux` on Haiku, by upstream design
`chunk-4hr1rwfw.js` builds its environment record with
`platform: ["win32","darwin"].includes(process.platform) ? process.platform : "linux"`.
The fallback `"linux"` is the same literal in the darwin build, so it isn't a fold. Upstream
maps every OS other than Windows/macOS to `linux` there (and `CLAUDE_CODE_HOST_PLATFORM`
accepts only win32/darwin/linux). Telemetry and the host-platform hint therefore say
`linux` on Haiku. Not patched: changing it would invent a value upstream's consumers
don't handle. `M()`/`getPlatform()` (P5) is the helper that reports `haiku`.

### Bash tool sandbox (bubblewrap/seccomp) — AUDIT S02
- **What:** the Bash sandbox has no Haiku backend. `chunk-v0b8hfja.js`
  `function dI(){let e=Fv();if(e==="linux")return Tsi()!=="1";return e==="macos"||e==="windows"}`
  is false for `haiku`, so `isSandboxingEnabled()` is false and initialisation never runs.
- **Why:** the Linux backend is built on bubblewrap, user namespaces and seccomp. Haiku
  has none of these.
- **Effect:** with `sandbox.enabled: true`, commands run **unsandboxed**. `-p` and
  `mcp serve` print `⚠ Sandbox disabled: sandbox is enabled but haiku is not supported
  (requires macOS, Linux, or WSL2)` / `Commands will run WITHOUT sandboxing…` to stderr.
  The interactive UI only logs it at warn level and sets a setup-issue counter.
  `autoAllowBashIfSandboxed` does not apply (it is gated on `isSandboxingEnabled()`), so
  the normal permission prompts stay. With `sandbox.failIfUnavailable: true` (set
  automatically by the SDK path whenever `enabled` is true), Claude Code refuses to start:
  `Error: sandbox required but unavailable: … refusing to start without a working sandbox.`
  It never crashes, and it never claims a command was sandboxed.
- **Fix would need:** a Haiku confinement mechanism the kernel doesn't offer today: per-team
  filesystem and network restriction, plus a backend in `chunk-v0b8hfja.js`'s
  `Qt`/`fI`/`yK` dispatch.

### Subprocess env scrubbing (`CLAUDE_CODE_SUBPROCESS_ENV_SCRUB=1`) — AUDIT S01
- **What:** `chunk-0ane94zt.js` `was()` needs a `bwrap` binary (`YAn()`).
- **Why:** the scrubbing isolates subprocesses with bubblewrap, which Haiku doesn't have.
- **Effect:** with the variable set, startup throws `bubblewrap is required for subprocess
  env scrubbing and isolation. Install with: sudo apt-get install -y bubblewrap, …` (the
  apt hint is wrong for Haiku). With it unset, which is the default, nothing happens.
- **Fix would need:** the same confinement mechanism as the Bash sandbox.

### Daemon service install (systemd) — AUDIT S03
- **What:** `claude daemon install|start|stop|restart` as a managed service.
- **Why:** the only service backend left in the Linux build is systemd
  (`~/.config/systemd/user/com.anthropic.claude-daemon.service`, `systemctl --user`).
  Haiku has neither systemd nor launchd.
- **Effect:** `uXe()` (stat of `/run/user/<uid>/systemd`) fails, so the user sees
  `Service install isn't available on haiku — the daemon still runs on demand when a
  client connects.` (exit 1). The daemon still starts on demand. The hub UI's uninstall
  (`G0t`) is not gated: it ignores `systemctl`'s exit code and reports success.
- **Fix would need:** a backend for Haiku's launch_daemon user jobs, which is unverified
  here, wired into the `ven`/`ft`/`G0t`/`xre` functions in place of the systemd block, plus a
  `uXe` equivalent.

### Voice input — AUDIT S04
- **What:** voice mode (`chunk-w6mjdp2k.js`).
- **Why:** it records through Anthropic's native `audio-capture.node` (the loader in
  `chunk-wfe5p2va.js` returns null for platforms other than darwin, linux and win32), then
  falls back to `arecord`, then SoX `rec`. None of these exists on DeBeOS, and Graviton
  has no audio hardware.
- **Effect:** `Voice mode requires SoX for audio recording. Install SoX manually: macOS:
  brew install sox / Ubuntu/Debian: sudo apt-get install sox …`.
- **Fix would need:** a Haiku capture path (a Media Kit addon or a SoX port with a Haiku
  backend) and a microphone. On Graviton only the error path can be verified.

### Proxy client-process attribution — AUDIT S08
- **What:** `chunk-m3w7djw9.js` `qt`/`Gr`/`Ur` name the local process behind each proxy
  connection.
- **Why:** they read `/proc/net/tcp{,6}`, `/proc/<pid>/fd` and `/proc/<pid>/comm`, and
  Haiku has no `/proc`.
- **Effect:** `clientProcess` is recorded as `<proc-net-unreadable>` in proxy connection
  data. Traffic is unaffected.
- **Fix would need:** a socket→team lookup on Haiku. `fdinfo` (`src/bin/fdinfo.cpp`) lists
  a team's fds as dev:ino, not sockets by port.

### Claude Desktop deep links — AUDIT S16
- **What:** opening Claude Desktop (`claude://`) from the CLI (`chunk-wj68m7s3.js`) and
  registering the scheme (`chunk-nyjcftf8.js`).
- **Why:** Claude Desktop doesn't exist for Haiku. The Linux path uses `xdg-mime` and
  `xdg-open`.
- **Effect:** Desktop is reported as `not-installed`. Registration returns early
  (`if(!["darwin","linux","win32"].includes(process.platform))return;`).
- **Fix would need:** a Claude Desktop for Haiku.

### Clipboard images and screenshots — AUDIT S17
- **What:** pasting an image from the clipboard and screenshot-to-clipboard
  (`chunk-ef9gz064.js`, `chunk-dz91hxpc.js` `cm`).
- **Why:** the table falls back to the Linux entry (`I[process.platform]||I.linux`),
  which uses `xclip`/`wl-paste`. Haiku's `clipboard` tool handles `text/plain` only
  (`src/bin/clipboard.cpp`).
- **Effect:** no image is ever found in the clipboard. Text paste is covered separately
  (AUDIT S18, class b).
- **Fix would need:** a small helper that reads or writes a BClipboard `image/png` entry.
  How Haiku apps store images on the clipboard wasn't checked.

### `/restart` command — AUDIT S27
- **What:** `/restart` (`/update`) is hidden.
- **Why:** `chunk-zy2k3hny.js` `kon`: `if(e!=="macos"&&e!=="linux")return!1;`.
  Self-relaunch is only enabled on those two platforms.
- **Effect:** the command isn't offered on Haiku.
- **Fix would need:** a confirmed `process.execve` relaunch on Haiku's Node 24 (also
  pending for L1), then `haiku` added to the gate.

### Inter-agent messaging over a Unix socket from the shell — AUDIT S35
- **What:** the prompt text in `chunk-74z2fna5.js` tells the model to send messages with
  `printf … | nc -N -U "$CLAUDE_CODE_MESSAGING_SOCKET"` or with socat.
- **Why:** Haiku has no `nc -U`, and whether `socat` is available is unknown.
- **Effect:** the model's suggested command fails. Messaging through Claude Code's own
  tools is unaffected.
- **Fix would need:** a Unix-socket client available on DeBeOS, and the prompt text naming it.

### Claude in Chrome — AUDIT S36
- **What:** browser-extension detection and native-messaging setup (`chunk-fejj39g0.js`
  `Rw`, `chunk-xjn8qbfw.js`).
- **Why:** the switches have no case for Haiku browsers. Chrome, Edge and Brave don't
  exist on Haiku.
- **Effect:** `[Claude in Chrome] Unsupported platform for extension detection: haiku`.
- **Fix would need:** a Chromium-family browser on Haiku that supports native messaging.

### Launching a terminal from a deep link — AUDIT S37
- **What:** `chunk-bfxj2kma.js` `M`/`y` pick a terminal emulator.
- **Why:** `switch(process.platform){…default:return null}` only knows macOS, Linux and
  Windows terminals.
- **Effect:** `No terminal emulator detected`.
- **Fix would need:** a haiku case that launches `/boot/system/apps/Terminal` (the command
  form is unverified).
