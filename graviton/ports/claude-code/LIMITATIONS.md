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

### `Bun.YAML.parse`: depth limits and inputs over 2 GiB
- **Depth:** Bun's nesting limit is a native-stack check that differs per construct (deepest
  accepted: flow seq 4723, flow map 4515, compact seq 4085, block map 3364). The port
  reproduces those four exactly with a per-construct cost model fitted to stock Bun, and
  re-runs a parse that overflows Node's default stack on a worker thread with a 32 MB stack,
  waited on synchronously. Bun itself wavers by one level at the limit, so inputs exactly
  there can't match deterministically; limits for untested construct mixes are estimates.
  Not yet run on Haiku (needs `worker_threads` and a 32 MB thread stack there).
- **> 2 GiB input:** Bun throws a `RangeError` naming `input.byteLength`; not implemented.

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
