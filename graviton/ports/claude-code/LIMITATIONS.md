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

### `Bun.spawn` on Haiku, against stock Bun on Linux
`compat/test/scenarios-haiku-step.sh` runs the 218 scenarios with Node 24.21.0 on Haiku
and compares them with the Bun 1.4.4 recording from Linux (`spawn-diff-step.sh`). The
scenarios observe children through `/proc` on Linux and through `cc-teaminfo` (team,
session, exec path, args) and `fdinfo` (open fds) on Haiku, in the same shape.
`check-spawn.mjs` accepts only the named differences of `compat/test/platform-diff.mjs`
and prints each one. Last run (`evidence/*_scen-haiku-node-v2.log`): **188 match / 23
platform-difference / 2 unobservable-on-haiku / 1 mismatch / 4 declared-unsupported**.
- **errno-value (18 scenarios):** the same error code, each side with its own platform's
  number in the same convention (ENOENT is -2 on Linux and -2147459069 on Haiku). The
  checker refuses a number that is in neither convention for that code.
- **signal-table (4):** Bun's "signal must be one of …" message lists the platform's
  signals 1..31 in number order. Haiku numbers them differently (SIGCHLD 5, SIGBUS 30),
  and has no SIGSTKFLT/SIGPWR.
- **exec-directory-errno (`directory as binary`):** exec of a directory is EACCES on
  Linux. On Haiku libroot tests the file before `_kern_exec` (`exec.cpp`
  `__test_executable` → `runtime_loader.cpp` `open_executable`, B_IS_A_DIRECTORY), so it
  is EISDIR. Bun's `posix_spawn` goes through the same libroot. Reproduced with plain
  Node: `compat/test/platform-repro/exec-dir.mjs`.
- **unobservable-on-haiku (`argv0 cmdline`, `no argv0 cmdline keeps cmd[0]`):** Haiku's
  kernel keeps a team's command line as `<exec path> <argv[1]> …`
  (`team.cpp` `Team::SetArgs(path, args + 1, …)`), so argv[0] isn't visible from outside
  the child (`ps`, `team_info`). The rest of argv is compared. The argv[0] that the child
  receives is checked from inside by `argv0 visible as $0`, which matches. Effect: a
  `claude bg-pty-host` process shows up as its binary's path in `ps`.
- **cgroup:** Bun ignores the option off Linux, and so does compat, so on Haiku it is an
  ordinary scenario (it matches) and not one of the declared refusals.
- **The mismatch is a Haiku defect (`bad interpreter`):** exec of a script whose `#!`
  interpreter is missing succeeds on Haiku. The child then dies in runtime_loader
  ("Cannot open file /no/such/interp", exit 3). On Linux, execve fails with ENOENT and
  Bun.spawn throws. libroot's `do_exec` (`src/system/libroot/posix/unistd/exec.cpp`
  84-106) checks the script but not the interpreter it names, and the kernel's
  `exec_team` doesn't check the path before the point of no return. Bun-on-Haiku would
  behave the same way. Effect: the Bash tool gets exit status 3 and that stderr line
  where Linux has a spawn error. Reproduced with plain Node and with bash:
  `compat/test/platform-repro/shebang.mjs`, `evidence/*_platform-repro-haiku.log`. The
  checker keeps this a mismatch and labels it (`KNOWN_DEFECTS`).

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
- **On Haiku** (Node 24.21.0, against the Linux Bun recording;
  `evidence/*_scen-haiku-node-v2.log`): **35 match / 6 platform-difference / 1 mismatch**.
  The mismatch is the Response defaults above, the same as on Linux. The named
  differences (`compat/test/platform-diff.mjs`, each printed by `check-net.mjs`):
  - errno-value (4): ECONNRESET, EADDRINUSE (Bun.listen reports the native errno, 98 on
    Linux), ECONNREFUSED ×2.
  - **no-reuseport-sharing** (`development:false`): Haiku has no SO_REUSEPORT port
    sharing. libuv answers ENOTSUP (`platform-repro/reuseport.mjs`), and Haiku's TCP bind
    ignores the option (`EndpointManager::_BindToAddress`: a listening user of the port
    is EADDRINUSE unless SO_REUSEADDR and TIME_WAIT/CLOSED). So the second server fails
    with the same EADDRINUSE error as `reusePort:false`, which Bun-on-Haiku would show
    too. Effect: none for the bundle, which never binds one port twice.
  - **localhost-resolution** (`hostname:"localhost"`): Haiku's hosts file maps localhost
    to 127.0.0.1 and ::1; Ubuntu's maps it to 127.0.0.1 only (`platform-repro/localhost.mjs`).
    The listener takes the first IPv6 answer, as uSockets does, so on Haiku it serves
    ::1.
- **IPv6 on Haiku (fixed in compat):** Haiku's IPv6 sockets are always v6-only, and
  `setsockopt(IPV6_V6ONLY, 0)` fails with EINVAL (`ipv6.cpp` `ipv6_setsockopt`). libuv sets
  that option on every IPv6 bind, so plain Node can't listen on `::1` or `::` without
  `ipv6Only:true` (`platform-repro/v6only.mjs`). uSockets ignores the setsockopt result,
  so Bun binds. `net.mjs` asks libuv for v6-only on Haiku, which is what the socket is
  anyway. Before the fix, `Bun.serve`/`Bun.listen` on `::1` threw EINVAL.
- **Haiku TCP defect, not a compat difference:** an accepted socket that sends its FIN
  first never sees the peer's FIN through poll(). The kernel reaches TIME_WAIT, but Node
  gets no `end`/`close` (`platform-repro/half-close.mjs`,
  `evidence/*_half-close-haiku.log`). `net.mjs` destroys the socket on `end()`, so the
  scenarios don't hit this. A plain Node server that `end()`s first leaks the socket.
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

### `fetch` options and `Bun.FetchSession` (compat/lib/fetch.mjs)
Diff test: `compat/test/fetch/` (servers in a Node fixture process on 127.0.0.1 and a
unix socket in a temp dir; the checker names each accepted difference as KNOWN).
- **Error objects:** Bun rejects a failed request with `TypeError{code}` (code and
  message of the failure: `UNABLE_TO_VERIFY_LEAF_SIGNATURE`, `ENOENT`, …); Node's fetch
  with `TypeError("fetch failed")` whose `cause` has the code. True for every fetch,
  with or without this layer. The layer gives Bun's own shape only to errors it raises:
  `ERR_PROXY_TUNNEL` (status, statusText, headers) for a refused CONNECT, the error a
  `tls.checkServerIdentity` returns, and the idle-timeout `TimeoutError`. Two Bun codes
  have different Node causes: a handshake the server ends for want of a client
  certificate (`EPROTO` vs `UND_ERR_SOCKET`), a wrong key passphrase
  (`FailedToOpenSocket` vs `ERR_OSSL_BAD_DECRYPT`). Code that reads `e.code` on a
  connection failure sees `undefined` under Node; `e.cause.code` has it.
- **Connection header** on a request whose connection won't be reused
  (`keepalive:false`, a per-request `checkServerIdentity`, a `keepAlive:false`
  session): Bun sends none and closes after the response; Node's HTTP client always
  writes one, so it sends `Connection: close`. Same connections, one extra header.
- **Connection reuse count:** undici 7.30.1 validates an idle keep-alive socket on a
  `setImmediate` before reuse (GHSA-35p6-xmwp-9g52) and its pool opens a second
  connection meanwhile, so sequential requests use up to two connections where Bun
  uses one. Pools are still per session, `close()` still drops idle sockets.
- **Timeout phases:** Bun's idle timer covers connect, TLS and the header block as one
  absolute window, then body idleness. Here: connect timeout, then `headersTimeout`,
  then `bodyTimeout`, each the same (Bun-padded) length, so before the first body byte
  a request can wait up to twice as long. Plain `fetch` calls without Bun options keep
  Node's defaults (10 s connect, 300 s headers/body) unless `BUN_CONFIG_HTTP_IDLE_TIMEOUT`
  is set, which routes them through the layer.
- **Rebuilt responses** (`decompress:false`, a forwarding proxy's 407, or any routed
  response with an idle timer): a new global `Response` with the original status,
  headers, body, `url` and `redirected` (own properties). `type` is `"default"` (Bun's
  value; Node's fetch says `"basic"`), and `clone()` of one loses `url`.
- **Plain fetch outside this layer** (no Bun option, no env proxy, no `Host` header):
  Node's own behaviour: `type:"basic"`, a 407 is a network error, the fetch-spec "bad
  ports" (e.g. 9, 6000) are refused, a `fetch({url})` object input is not read as init.
- **Refused explicitly** (rejection "… is not supported by the Node compat layer"):
  `verbose` (true/"curl"), `compress`, `maxRedirects`, `protocol` h2/h3, `s3://` URLs,
  `tls` keys `minVersion`/`maxVersion`/`secureOptions`/`ALPNProtocols`/`crl`/
  `dhParamsFile`/`ecdhCurve`/`sigalgs`/`sessionTimeout`/`clientRenegotiation*`/
  `allowPartialTrustChain`/`requestCert`/`lowMemoryMode` when not default, a Blob or
  `Bun.file()` as `tls.ca`/`cert`/`key`, `ca` together with `caFile` (same for
  cert/key), a non-http(s) proxy URL, a `dispatcher` together with Bun options, and
  `FetchSession` `keepAlive.idleTimeout`/`maxIdleSockets`. No bundle call site uses them.
- **Version-sensitive internals:** the dispatch handler wrapper relies on Node's
  fetch handing dispatchers a legacy-API handler (`onHeaders(status, rawHeaders, …)`),
  true of undici 7.x. Node 24.21.0 (the Haiku build) must bundle undici 7.x too
  (`build-tree.sh` checks the major); not yet run on Haiku.

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

### `Bun.ant.CellSegmenter`: a contract port of closed native code
- **What:** the class is native code in Anthropic's internal Bun with no source. The port
  reproduces what the real 2.1.314 runtime does on 4,000 random sessions and 94 edge cases
  (`test/check-cellseg.mjs`, vectors made inside the binary by `gen-cellseg-vectors.mjs`),
  not a specification. Inputs far outside that corpus (other screen layouts, scripts the
  corpus lacks) rest on the same rules but are not individually measured.
- **Fix would need:** the source, or more vectors.

### `Bun.ant.CellSegmenter`: grapheme table after an overflow (named difference)
- **What:** when `segment()` returns `-count` (arrays too small), the real runtime has
  interned some of the line's new graphemes and not others, by a rule the vectors did not
  pin down (it depends on run boundaries and UTF-16 length). The port interns those it
  wrote and stops. Grapheme *indices* handed out afterwards then differ; the strings they
  name, cells, runs, styles, links and screens do not. 173/4,000 sessions differ only so.
- **Effect:** none on screen: indices are opaque handles that `pf` resolves through
  `graphemes[]` and the char pool, by string. `graphemes.length` can differ by a few entries,
  so the renderer's table reset (`> 65,536` entries) can fire a little earlier or later.
- **Fix would need:** the native interning order.

### `Bun.ant.CellSegmenter`: bidi reordering (`reordered=true`), 7 of 4,000 sessions differ
- **What:** visual reordering is used only when Claude Code thinks the terminal will not
  reorder itself (`WT_SESSION` set or `TERM_PROGRAM=vscode`). The port runs UAX #9
  (npm `bidi-js`, classes from Unicode 17 `DerivedBidiClass.txt`) plus the measured
  adjustments (no reordering without a strong R/AL letter other than RLM/ALM; controls
  excluded; Prepend-led clusters analysed by their Prepend marks; boundary neutrals take
  the next character's level). The native code is probably ICU's `ubidi`; 7 random sessions
  still order a few cells differently, all on lines mixing Prepend marks (U+0600..),
  Arabic-Indic digits and neutrals.
- **Effect:** only in Windows Terminal / VS Code terminals (not the case on Haiku's own
  Terminal), only for such mixed lines: a few cells in another visual order. The E2E
  harness's Hebrew/Arabic prompt under `TERM_PROGRAM=vscode` matches the real binary.
- **Fix would need:** ICU `ubidi` (not exposed by Node) or the native source.

### `tools/tui-diff.sh`: what the screen diff does not see
- Animation is held still with Claude Code's own `prefersReducedMotion` setting; snapshots
  are taken once the pane is unchanged for 3 s.
- The only pre-login screen that echoes input is "Paste code here if prompted >", and it
  masks input with `*`, showing only the tail. Wide/emoji/ZWJ/combining/RTL text is
  therefore seen as the tail of each typed string (the run log prints what was visible).
  On that screen only the prompt line and below are compared: the OAuth URL above it carries
  a random PKCE challenge, state and link id per run.
- Screens after login (the conversation, tool output, diffs) are not covered: they need an
  account.

### `Bun.ant` exists now: other members' call sites
Defining the namespace changes nothing for members that stay absent, except the text of
one log line. Every bundle use of `Bun.ant` (2.1.314), checked:
- guarded on the member (`typeof Bun.ant?.X === "function"`, `Bun.ant?.X`, `?.()`): same
  result as before — `windowsProcessList` (2 sites), `getPeerPid` feature check (`DV`),
  `windowsFolderSecurity`, `windowsSandboxDeniesArg`, `posixFsAt` (`globalThis.Bun?.ant?.posixFsAt`,
  `typeof e?.openat`), `CellSegmenter` (`d7n`).
- unguarded but inside `try/catch` (they threw `TypeError` before and still do):
  `getPeerUid` (daemon peer check -> `null`, warn), `getPeerPid` (-> undefined, warn),
  `setDumpable` (reports "prctl unavailable"), `loginTty` (-> `"ffi_unavailable"`), `ttyName`
  (-> undefined), `memoryPressureLevel` (macOS only), `windowsProcessCreationTime`. The caught
  message changes from "Cannot read properties of undefined (reading 'X')" to
  "Bun.ant.X is not a function"; it only appears in debug/warn log lines.
- No site tests `typeof Bun.ant` or `"ant" in Bun` by itself.

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

### Path-held directories instead of fd-held ones (P14, P15) — AUDIT S11, S12
- **What:** on Linux, the served-file folder (`chunk-12xea136.js`) and the private
  directory chain (`chunk-m6kk55rf.js`) keep working through `/proc/self/fd/N`, a path
  that stays bound to the opened directory even if someone renames or replaces it. On
  Haiku they work through the real path, re-checked by dev/ino (P14: `fstat` of the
  handle equals `lstat` of the path).
- **Why:** Haiku has no `/proc` and no fd-relative path, and Node exposes no `openat`.
- **Effect:** if a directory is swapped between the check and the use, the swap is
  caught at the next re-check rather than prevented. `wqn` ("does a held handle follow a
  rename?") answers `holds_do_not_follow` on Haiku, so the desktop attachments-in-user-data
  folder is reported `not_used`.
- **Fix would need:** `openat`/`mkdirat`-relative operations from JS (an addon), or
  `/dev/fd`-style directory paths in Haiku.

### Clipboard needs a desktop session (P10) — AUDIT S18
- **What:** copy and paste use Haiku's `clipboard` CLI.
- **Why:** it is a BApplication and needs the registrar/app_server.
- **Effect:** on a headless instance the command fails. Claude Code logs
  `clipboard: Haiku 'clipboard -i' failed (exit N): … -- it needs a running
  registrar/app_server` at error level. Copy still emits OSC 52 to the terminal. Paste
  returns empty. Not yet run on DeBeOS.
- **Fix would need:** nothing for desktop sessions. Headless sessions have no clipboard.

### Team identity helper (P17) — AUDIT S06, S21, S32
- **What:** process start tokens, ancestry, the process table and command lines come from
  `<prefix>/bin/cc-teaminfo` (`native/cc-teaminfo.c`).
- **Why:** Haiku has no `/proc`, and its `ps` has no `ppid`/`lstart` columns.
  `team_info` carries both values.
- **Effect:** built and verified on Haiku: **pending** (it is written against this
  tree's `headers/os/kernel/OS.h` and syntax-checked only). If the binary is missing,
  Claude Code prints one `ClaudeCodeHaiku` warning and the identity checks fail closed,
  as before P17. The command line (`mas`) is `team_info.args`, which the kernel cuts to
  63 bytes. Zombie state isn't read: `qno` (`/proc/<pid>/stat` state) stays Linux-only.
- **Resolved:** the claude_code recipe builds `native/cc-teaminfo.c` on Haiku and installs
  it at `<prefix>/bin/cc-teaminfo` (published in claude_code 2.1.314-1 and later).

### Native file watching (`fs.watch`) — RESOLVED by DeBeOS libuv 1.52.1-3 (AUDIT S29, W1–W18)
- **Resolved 2026-11-01:** libuv now has a Haiku fs-event backend (node monitoring; branch
  `feat/libuv-haiku-fs-event`). It is published as libuv 1.52.1-3, which provides
  `libuv_fs_event`, and claude_code 2.1.314-2 requires that, so a repo install pulls it in
  (`evidence/20261101T034345Z_i-0e90d4b253c8134b3_enduser-fresh-install-3.log`). Measured in Claude Code itself: with the old libuv
  `FileChanged: watcher error: ENOSYS` and the hook never fires; with the new one
  `fs.watch probe: delivers` and the hook fires for each external edit (`evidence/20261101T024050Z_i-0e90d4b253c8134b3_filechanged-before-after-libuv.log`).
  Remaining backend semantics (BFS reports writes at close or ≤1/s; a deleted watched
  entry is reported once) are in libuv's `docs/src/fs_event.rst` on that branch. The
  entry below is the original measurement, kept for the record.
- **What:** on Haiku, Node 24.21.0's `fs.watch(dir)` throws synchronously:
  `Error: ENOSYS: function not implemented, watch '<path>'` (errno -2147454967, syscall
  `watch`). Measured on builder i-0e7bed4928e0f7bf7
  (`evidence/20261031T080733Z_i-0e7bed4928e0f7bf7_node24-accept-haiku.log`).
  `fs.watchFile` (stat polling) is not measured yet.
- **Why:** libuv has no fs-event backend for Haiku.
- **Effect:** nothing crashes. Bundled chokidar catches the throw and emits `"error"` only to
  attached listeners, and every site attaches one or wraps the call in try/catch. Per feature:
  - **Settings hot reload: silently off.** `settings.json` edits made outside Claude Code apply
    only after a restart. The watcher's fallback to polling only covers
    `EMFILE`/`ENFILE`/`ENOSPC`, and the fs.watch probe calls ENOSYS `"unprobed"`, then
    records the watcher as started (W1).
  - **Off, with a log line:**
    - `FileChanged` hooks never fire (W2).
    - The daemon misses background dispatches queued after it started (W3, logged at error level).
    - Daemon config edits need a daemon restart (W4).
    - Custom theme edits need a restart (W5).
    - Bash edit diffs / outside-write attribution are gone (W7).
  - **Degraded:** scheduled tasks are picked up on the periodic rescan only (W6).
  - **Silently off:**
    - Live sync of background-job names (W8) and of background-session PR links (W9).
    - Task-list updates from other processes (W10, `catch{}`).
  - **Unaffected, as long as `fs.watchFile` works:**
    - Already polling: skills, keybindings, plugin dirs, dev mods and memory files
      (W13–W17; memory polls because the compat layer defines `globalThis.Bun`).
    - Fall back to `fs.watchFile`: the file-event watcher and `~/.claude.json` freshness (W11, W12).
  - The upstream switch `CHOKIDAR_USEPOLLING=1` makes chokidar poll for W2–W6, but not for the
    raw `fs.watch` sites W7–W10.
- **Fix would need:** either a Haiku `uv_fs_event` backend in libuv (over node monitoring,
  `watch_node`/`B_WATCH_DIRECTORY`), or polling in the compat layer. That is an owner
  decision. Nothing is implemented in the meantime.
