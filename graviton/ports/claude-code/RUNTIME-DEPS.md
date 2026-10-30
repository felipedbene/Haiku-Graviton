# RUNTIME-DEPS — what the 2.1.314 bundle needs from its runtime

The counts are a static census over 2,644 JS modules (50 MB) via `tools/inventory.py`
plus targeted greps. Runtime confirmation is noted where it exists. "Guarded" uses a
heuristic (a `typeof Bun` check or `try{` close by) and still needs per-site review.

## 1. The bundle is built for Linux: platform is constant-folded

Bun's bundler substituted the target platform at build time:

- `function Fv(){switch("linux"){case"darwin":return"macos";case"linux":return"linux";case"win32":return"windows";default:return"unknown"}}`.
  This is the platform helper, and it returns `"linux"` unconditionally.
- 9 `switch("linux")` sites in 7 modules; `switch("arm64")` ×1; addon paths
  are baked as `"arm64-linux"`.
- Only **4** runtime `process.platform` reads and 11 `os.platform()` calls survive.
  Every `if (process.platform === "darwin")`-style branch was dead-code-eliminated,
  so the Linux code is now inlined without any guard.

Run on DeBeOS unmodified, this code believes it is on Linux (hard rule #1). The
eliminated branches cannot be recovered from this artifact. This affects **both**
runtime options, (i) Node + compat and (ii) a Bun port.

## 2. Bun language/loader features

| Feature | Uses | Node 22 status |
|---|---|---|
| `import.meta.require(...)` (synchronous require from ESM) | **547 in 118 modules** | absent; **runtime-confirmed first failure**: `TypeError: (intermediate value).require is not a function` |
| `import.meta.dir` | **9** (Bun-only; the census's first figure, 141, wrongly counted 132 `import.meta.dirname`, which Node supports) | absent; Node has `import.meta.dirname` |
| `bun:jsc` | 3 modules: `heapStats().objectTypeCounts`, `edenGC()` (diagnostics; both inside try) | absent |
| `// @bun @bytecode` pragma | every chunk | comment; harmless |
| absolute `/$bunfs/` specifiers | all modules | resolvable via the `/$bunfs` symlink |

## 3. `Bun.*` globals used (33)

Compared against `Object.keys(Bun)` of stock Bun 1.4.4: all present **except `Bun.ant`**.

| API | uses (unguarded/guarded) | Node equivalent |
|---|---|---|
| `Bun.ant.*` (Anthropic-only native ext.) | 2/19 | none. Members: `getPeerPid`, `getPeerUid`, `loginTty`, `ttyName`, `memoryPressureLevel`, `posixFsAt`, `setDumpable`, `CellSegmenter`, `windowsProcessList`, `windowsProcessCreationTime`, `windowsFolderSecurity`, `windowsSandboxDeniesArg`. Call sites feature-detect it or catch errors; it is also absent from stock Bun, which runs `--version`/`--help` |
| `hash` | 14/1 | none (wyhash); needs a real implementation |
| `spawn` | 3/3 | `child_process.spawn` (different API) |
| `sliceAnsi`, `stringWidth`, `wrapAnsi`, `stripANSI` | 6/0, 3/0, 3/0, 1/0 | none built in (npm equivalents exist) |
| `serve`, `listen`, `connect` | 4/0, 2/0, 1/1 | `http`/`net` (different API) |
| `Image` | 2/2 | none |
| `FetchSession` | 1/3 | none |
| `Terminal` (PTY) | 1/1 | none (node-pty is a native addon) |
| `WebView` | 0/1 | none |
| `stdin` | 3/0 | `process.stdin` (different API) |
| `which` | 3/0 | none built in |
| `YAML`, `TOML`, `JSONL` | 2/0, 1/0, 0/1 | none built in |
| `deepEquals` | 2/0 | `util.isDeepStrictEqual` |
| `semver` | 1/1 | none built in |
| `zstdDecompress(Sync)` | 1/0, 0/1 | `zlib.zstdDecompress*` (experimental in Node 22) |
| `SHA256`, `file`, `sleepSync`, `gc`, `generateHeapSnapshot`, `Transpiler`, `unsafe`, `main`, `version`, `isStandaloneExecutable` | 1–2 each | partial / none |

Fetch options outside the web spec (`unix:`, `proxy:`, …): about 10 sites in 8 modules (heuristic).

## 4. Native addons (both Linux ELF; Haiku needs source or a real reimplementation)

| Addon | Loaded from | Purpose (by name/call site) |
|---|---|---|
| `clipboard-napi.node` | `./vendor/clipboard-napi/arm64-linux{,-musl}/…`, loaded in a try loop | clipboard (image paste) |
| `audio-capture.node` | `./vendor/audio-capture/arm64-linux{,-musl}/…`, loaded in a try loop | voice input |

## 5. External binaries spawned (literal first arguments; minification hides more)

`gh` ×20, `defaults` 8, `git` 7, `open` 7, `systemctl` 7, `tmux` 6, `which` 5, `ps` 4,
`osascript` 3, `killall` 2, `pdftoppm` 2, `pbcopy`/`pbpaste` 2, `wslpath` 2, `ssh` 2,
`powershell.exe` 2, `code` 2, `xclip`/`xsel`/`wl-copy` 2, `reg` 2, `security` 2, `npm`,
`pdfinfo`, `az`, `azd`, `xdg-open`, `xdg-mime`, `unzip`, `cursor`, `windsurf`, `glab`,
`mdfind`, `ls`, `taskkill`, `id`. Also `bash`/`sh`, which the Bash tool resolves at runtime.

**ripgrep:** selector `ule()`: when env `USE_BUILTIN_RIPGREP` is `0`/`false`/`no`/`off`
(helper `os()` in `chunk-sgsz8gxv.js`), it uses `rg` from PATH (`mode:"system"`). When running as a standalone Bun exe, it
re-execs itself as `rg` (`argv0:"rg"`, embedded mode). Otherwise it falls back to `rg` on
PATH. So system rg is a **supported** path, and DeBeOS has `ripgrep-15.2.0` (verified).
