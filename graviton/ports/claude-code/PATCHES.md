# PATCHES — every change from upstream, with the reason

Each entry: **file · what · why · upstream-able (y/n)**. Bundle rewrites are applied by
`tools/relocate-bundle.py` and `tools/apply-r4.py`, never by hand. Each rule asserts its
exact match count for 2.1.314, so a new version that changes shape stops the build.
Site counts below are the asserted ones.

## A. Bundle rewrites (deterministic pass)

| ID | File(s) | What | Why | Upstream-able |
|---|---|---|---|---|
| R1 | all 2,639 JS modules (250,476 sites) | `"/$bunfs/root/"` → `"<install-prefix>/"` | The bundle addresses its chunks, assets and addons inside the Bun binary's virtual FS. Outside the binary they live on disk at the install prefix. You chose a rewrite over a `/$bunfs` symlink. | n (artifact of extraction) |
| R2 | 118 modules (547 sites) | `import.meta.require` → `__ccRequire`, plus a per-module prelude `const __ccRequire = createRequire(import.meta.url)` | `import.meta.require` is Bun-only. `createRequire` is the Node equivalent; Node ≥22.12 `require()`s ESM natively. | n |
| R3 | 9 modules (9 sites) | `import.meta.dir` → `import.meta.dirname` | Bun-only property name for the same value (Node ≥20.11). | n |
| R4 | `chunk-zy2k3hny.js` (1 site) | top-level `__ccRequire("…/chunk-vgzpjjhb.js")` → `import * as __ccR4_0 from "…/chunk-vgzpjjhb.js"` | The require runs while `zy2k3hny` is still evaluating, and `vgzpjjhb` statically imports `zy2k3hny`. Node's `require(esm)` refuses that cycle (`ERR_REQUIRE_CYCLE_MODULE`, observed on `--help`). A static import cycle is ordinary ESM. `tools/analyze-requires.mjs` (acorn) found 186 evaluation-time requires; only this one is cycle-capable, directly or indirectly. | n |

### Folded platform/arch restored (P1, one entry per site)

Bun's bundler replaced `process.platform`/`process.arch` with the target's literals and
removed the branches it proved dead. These sites survived as `switch` statements, so
the runtime value is restored. Branches already deleted from the artifact can't be
restored; they are covered by the audit (LIMITATIONS.md).

| ID | File · function | What the switch decides | Upstream-able |
|---|---|---|---|
| P1.1 | `chunk-8tqbvyg9.js` · `T` | Chrome/Chromium/Edge/Brave executable search paths | n |
| P1.2 | `chunk-bfxj2kma.js` · `M` | terminal-emulator detection (macOS / Linux / Windows probe) | n |
| P1.3 | `chunk-bfxj2kma.js` · `y` | how to launch a command in that terminal | n |
| P1.4 | `chunk-fejj39g0.js` · `Rw` | browser profile directory per OS | n |
| P1.5 | `chunk-jwstday3.js` · `Fv` | **the platform helper**: `macos`/`linux`/`windows`/`unknown` | n |
| P1.6 | `chunk-n8d1bv23.js` · `h` | Claude Desktop download URL per OS | n |
| P1.7 | `chunk-nyjcftf8.js` · `x` | OS-specific install step (folded error string: see audit) | n |
| P1.8 | `chunk-nyjcftf8.js` · `v` | "is this installed" check per OS | n |
| P1.9 | `chunk-tcztx3fg.js` · `H` | OS family reported in environment metadata | n |
| P2 | `chunk-v0b8hfja.js` · `xE` | `switch("arm64")` → `switch(process.arch)`: seccomp-filter arch selection | n |
| P3 | `chunk-jwstday3.js` · `Fv` | adds `case "haiku": return "haiku"` to the platform helper | n |

### Every folded site (P4, `tools/apply-folds.py`)

P1/P2 were found by eye. P4 finds all of them mechanically: `tools/find-folds.py` aligns
the linux-arm64 tokens against the darwin-arm64 build (decides `"linux"`) and the
linux-x64 build (decides `"arm64"`), both pinned by npm integrity. A literal is a fold
only where the other build has the other OS/arch word and 4 aligned neighbours agree
on each side. The 126 sites in 60 modules are listed with module, offset, position and
snippet in `tools/folds-2.1.314.json`; `apply-folds.py` asserts the count, the source
sha256 and each original text, then acorn-parses every rewritten module.

| ID | File(s) | What | Why | Upstream-able |
|---|---|---|---|---|
| P4 | 60 modules (126 sites: platform 102, arch 22, both 2) | each folded `"linux"`/`"arm64"` → `process.platform`/`process.arch` (in template text, `${…}`) | The folded literal reports Linux on Haiku. The 10 P1/P2 switch sites are part of the 126; P4 verifies and skips them when relocation already ran P1/P2. Includes the main platform host `new u({platform:"linux",…})` in `chunk-8dvz2fjk.js`. | n |

| P5 | `chunk-8dvz2fjk.js` · `getPlatform()` (`tools/apply-haiku-names.py`) | adds `else if(platform==="haiku") → "haiku"` | The main host's platform helper (`M()`) returns `macos`/`windows`/`wsl`/`linux`/`unknown`; with P4 live, Haiku would be `unknown`. You asked for an explicit haiku case. | n |
| P6 | `chunk-8dvz2fjk.js` · `G1e` | adds `case"haiku":return"Haiku"` | display-name map; the default returned the raw `haiku` | n |
| P7 | `chunk-yt5m1rse.js` · `nTo` | adds `e==="haiku"?"Haiku"` | display-name map compared with a peer's `osPlatform`; every other OS became `"Linux"` | n |

Not rewritten, by design: `chunk-9deh4a76.js` `"arm64-linux"` (napi addon dir; the
addon is a Linux ELF and stays absent on Haiku either way), `chunk-3c6t0j11.js`'s
macOS-only message, and `chunk-szzxb7zd.js` `te()` (an `Fv` result folded in place, not a
raw platform value). Code the bundler deleted is in the audit (LIMITATIONS.md).

### Haiku implementations (P8–P17, `tools/apply-haiku-impl.py`)

These are the class (b) sites from AUDIT.md. Each one is an exact, count-asserted
substitution that adds a branch taken only when `process.platform==="haiku"` (or the
local `M()` value is `"haiku"`). The Linux expression stays in place on the other side
of the test. The script refuses a second run, records `rule_totals`/`rule_sites` and the
new file sha256s in RELOCATION.json, and acorn-parses every module it rewrote.
`build-tree.sh` runs it after `apply-haiku-names.py`.

| ID | File · function (sites) | What | Why | Upstream-able |
|---|---|---|---|---|
| P8 | `chunk-hv6m45s7.js` · `iqn` (1) | `case"haiku":` joins `case"unknown"`, giving `{unavailable:"unsupported_platform"}` | AUDIT S20. With P5 the switch had no case for `"haiku"` and no default, so it returned `undefined` and every `openTreeAnchor` caller threw a TypeError | n |
| P9 | `chunk-aa47jmf3.js` · `s`, `h`, `Zvn`, `m` (4) | URLs and files open with `/bin/open`; "reveal" opens the parent folder in Tracker | S15. `xdg-open`/`dbus-send` don't exist. Haiku's `open` takes URLs (`BUrl::OpenWithPreferredApplication`, `src/bin/open.cpp`) and paths | n |
| P10 | `chunk-9deh4a76.js` · `Sbe`, `Z`, `xGt` (3) | copy with `clipboard -i`, paste with `clipboard -p` (one trailing newline stripped). A non-zero exit is logged at error level, naming the registrar | S18. `src/bin/clipboard.cpp`. It is a BApplication, so a headless box without a registrar fails, and that failure is logged | n |
| P11 | `chunk-wg6ymv3p.js` · daemon re-auth (1) | `notify --title Claude "<message>"`; a failure is logged | S23. Haiku has no `notify-send`; `src/bin/notify.cpp` | n |
| P12 | `chunk-ddrd3r4k.js` · `ir` (1) | `haiku:1024` | S24. `PATH_MAX` is 1024 (`headers/posix/limits.h:44`) | n |
| P13 | `chunk-k1pd3pap.js` · `wIn` (1), `chunk-v5xftfk7.js` · `Me` (1), `chunk-s7yyd1p6.js` · `kio`, `vio` (2) | `wIn` skips the `/proc/self/fd` readlink and falls through to its existing realpath tail. `Me` uses `realpath` plus a `stat` dev/ino match against the handle. A token fd is read directly (`t9e`), the path sockets already take | S10. Without `/proc` these failed closed every time and gave no reason | n |
| P14 | `chunk-12xea136.js` · `ye`, `qo` (2) | the held folder's path is the verified real path, re-checked as `fstat(handle)`==`lstat(path)` by dev/ino | S11. `/proc/self/fd/N` paths don't exist on Haiku | n |
| P15 | `chunk-m6kk55rf.js` · `s` + its 4 opens (5) | directory handles record the path they were opened at (`O_DIRECTORY\|O_NOFOLLOW`, unchanged); `s()` returns it on Haiku | S12. See LIMITATIONS for the weaker guarantee | n |
| P16 | `chunk-szzxb7zd.js` · `Bo`, `y9` (1); `chunk-zy2k3hny.js` · `sfr` (1) | on Haiku `Bo`/`sfr` = 64 (`O_CLOEXEC`) and `y9` = 0 | S38. The Linux build bakes in Linux flag numbers: 524288 (`O_CLOEXEC`) is Haiku's `O_NOFOLLOW` and 2097152 (`O_PATH`) is Haiku's `O_DIRECTORY` (`headers/posix/fcntl.h`). So every `\|Bo` open in the 26 modules that import it would have refused a symlink as the last path component (ELOOP). `y9`=0 is the darwin value and is already handled (`fejj39g0`: `if(y9===0…)return"not_held"`) | n |
| P17 | `chunk-6h8148nz.js` · `QOe`, `fas`, `tgr`, `mas`, `G3e`, `b`, `eIe` (7); `chunk-fgccedbt.js` · `HL` (1); `chunk-2hxx68dq.js` · `Wn` (1); `chunk-s0dw5891.js` lock `procStart` (1) | start token, ancestry, process table and command line come from `<prefix>/bin/cc-teaminfo` (`native/cc-teaminfo.c`, `get_team_info`). The helper functions are appended to the three modules that run it. A missing helper produces one `ClaudeCodeHaiku` warning on stderr | S06/S21/S32. With no `/proc`, and a `ps` that has no `ppid`/`lstart` columns, pid-reuse protection and kill-tree enumeration failed silently | n |

`chunk-xyt7m0qn.js` `rjn` (AUDIT S22) is **not** rewritten. On Haiku it runs for every
daemon-lock check (`rE`), and `/proc/<pid>/cmdline` is never readable there, so
answering false would reject every live daemon. With P17 the daemon lock records
`procStart` (`nle(cS())`), and `uon` then proves identity by start token. That makes
`rjn`'s "can't tell, say yes" answer safe. `team_info.args` is cut to 63 bytes, so it
can't stand in for the cmdline match.

`native/cc-teaminfo.c` is built on Haiku (`cc -O2 -o cc-teaminfo cc-teaminfo.c`, libroot
only) and installed with `CC_TEAMINFO=<binary> build-tree.sh …`. Its output format is
documented in the source header.

## B. Runtime support installed next to the bundle

| ID | File | What | Why | Upstream-able |
|---|---|---|---|---|
| C1 | `bun-compat.mjs` (`node --import`) | load hook serving Bun "text"-loader files as their contents | Bun's `text` loader makes `import`/`require` of a file yield its string. Node has no such loader. The hook serves exactly the 98 files `LOADERS.json` marks as text, keyed by file because two `.mjs` files are text, not code. | n |
| C2 | `node_modules/ws` | npm `ws` 8.22.0, integrity-pinned in `tools/deps.lock` | Bun provides `ws` as a built-in; the bundle imports it externally (4 sites). | n |
| C3 | `node_modules/node-fetch` | re-exports Node's native `fetch`/`Headers`/`Request`/`Response`/`FormData`/`Blob`/`File` | Bun's built-in `node-fetch` is the same re-export of its native fetch. 1 dynamic import, used when `window` is absent. | n |

`Bun.*` members in `bun-compat.mjs`. Each is a port of Bun 1.4.4's source (or of its
contract, where noted) diff-tested against stock Bun 1.4.4 on the Linux reference box;
the vector counts are the last run's. Anything not listed is absent, never a stub.

| ID | Member(s) · `compat/lib/` | Why the bundle needs it | Diff test | Upstream-able |
|---|---|---|---|---|
| C4 | `hash` (wyhash + crc32/xxHash64…) · `hash.mjs` | content hashes; must be bit-exact or cached keys change | 3,353/3,353 | n |
| C5 | `stringWidth` · `width.mjs`, `width-tables.mjs` (generated from Bun's `stringWidthTables.h`) | TUI layout | 3,608,650/3,608,650 | n |
| C6 | `stripANSI` · `strip.mjs` | TUI text | 20,008/20,008 | n |
| C7 | `sliceAnsi` · `slice.mjs`, `sgr.mjs` | TUI text | 40,900/40,900 | n |
| C8 | `wrapAnsi` · `wrap.mjs` | TUI text | 40,000/40,000 | n |
| C9 | `semver.order`/`satisfies` · `semver.mjs` | update/plugin version checks | 100,009/100,009 | n |
| C10 | `deepEquals` · `deepequals.mjs` | state comparison | 354/354 | n |
| C11 | `which` · `which.mjs` | locating rg, git, shells | 38/38 | n |
| C12 | `sleepSync`, `gc`, `SHA256`, `zstdDecompress(Sync)`, `generateHeapSnapshot`, `file`, `stdin`, `main` · `misc.mjs` (contract ports on Node primitives) | assorted call sites | 35/35 | n |
| C13 | `spawn` · `spawn.mjs` | every tool subprocess (Bash, Grep, git) | 213/218 + 5 explicit refusals | n |
| C14 | `YAML.parse` · `yaml-parse.mjs` | frontmatter, config | 97,532 × 6 seeds | n |
| C15 | `YAML.stringify` · `yaml-stringify.mjs` | frontmatter writes | 119,328 | n |
| C16 | `TOML.parse` · `toml.mjs` | another agent's `config.toml` import | 120,762 + 10 named differences | n |
| C17 | `serve`, `listen`, `connect` · `net.mjs` (on node:http/https/net) | gateway, OAuth callback, MCP and relay sockets | 41/42 (the 1: Node's `Response` defaults, LIMITATIONS.md) | n |
| C18 | `http.maxHeaderSize` setter · `net.mjs` `installHttpMaxHeaderSize` | the gateway assigns it; Node's property is getter-only, so the assignment throws in strict ESM | covered by C17's header-limit scenario | n |
| C20 | global `Worker` (Web Worker API) · `worker.mjs`, `worker-boot.mjs` on `worker_threads` | Bun's global; Node has none. Used by the stdout writer thread (`chunk-2hxx68dq.js` `writeOnAThread`, blob CommonJS script) and the function-hooks host (`chunk-zy2k3hny.js`, ESM file + `workerData`). Without it the first is caught and degrades to `NO_THREAD`, and the second throws `ReferenceError`. | 13/13 scenarios + unref exit, twice | n |
| C21 | global `fetch` wrapper + `FetchSession` · `fetch.mjs` (undici `Agent`s from C22 as the `dispatcher` of Node's own fetch; Response/Request stay Node's globals) | `ra()` (`chunk-ffffjxsk.js`) spreads Bun-only init options into fetch at 43 call sites in 21 modules (incl. SDK `fetchOptions`): `proxy` (string, `{url, headers}`, `{url, respectNoProxy:false}`, `false`), `unix`, `tls` (ca/cert/key/passphrase, plus `rejectUnauthorized`, and `serverName` + `checkServerIdentity` in the gateway's `Ga`), `timeout:false`, `keepalive:false`; MCP and CCR reads pass `decompress:false`. Node's fetch silently ignored all of them (a proxy bypassed, a socket or CA ignored). `Bun.FetchSession` carries first-party analytics (`chunk-fgccedbt.js` `post()` via `nIn()`); without it every batch failed, retried 8×, and was dropped. Also: Bun's env proxies (`HTTP(S)_PROXY`/`ALL_PROXY`/`NO_PROXY`, read live) for plain fetch, a caller's `Host` header, 407 from a forwarding proxy returned as a response, idle timeout as `TimeoutError`, `BUN_CONFIG_HTTP_IDLE_TIMEOUT`. Unreproducible options throw "not supported by the Node compat layer". | 55/55 scenarios (42 identical, 13 identical but for 2 named differences, LIMITATIONS.md) + 7 required refusals | n |
| C22 | `node_modules/undici` | npm `undici` 7.30.1, integrity-pinned in `tools/deps.lock`; `build-tree.sh` refuses a tree whose major differs from `process.versions.undici` | Node 24 bundles undici 7.30.1 (24.22.2) but exports no `Agent`/dispatcher; same major = same handler API that Node's fetch drives. | n |
| C19 | `unsafe: {}` (namespace, no members) | `Bun.unsafe.setJITPolicy?.()` is guarded only by `typeof Bun`; the JSC JIT knob itself has no V8 counterpart and stays absent | — | n |

| ID | File | What | Why | Upstream-able |
|---|---|---|---|---|
| L1 | `claude.mjs` (the entry the launcher runs) | imports `bun-compat.mjs`, then `cli`. Without `--zero-fill-buffers` in `execArgv`, it first `process.execve`s itself with it (same pid and stdio) | Outside a standalone binary, the bundle relaunches as `process.execPath process.argv[1] …` (26 modules). With `argv[1]` = this file, every relaunch gets the compat layer and Bun's baked-in compile flag. `NODE_OPTIONS` was rejected because it would leak into the user's own node processes. | n |

## C. Recipes

| ID | File | What | Why | Upstream-able |
|---|---|---|---|---|
| K1 | `haikuports-patches/recipes/nodejs24-24.21.0.recipe` | upstream HaikuPorts recipe (`21c227aca765`) with `devel:libicu* >= 74` → `>= 78` | Forces full ICU 78 instead of the `icu74` **bootstrap** build (incomplete locale data; `Intl.NumberFormat('de-DE')` throws, PR #386). | y, if upstream wants full ICU |

## D. Pipeline (separate PR, needed to build K1)

| ID | File | What | Why | Upstream-able |
|---|---|---|---|---|
| X1 | `graviton/scripts/haiku-provision-native-builder` | pin haikuporter to `faa30769682f` (`HP_REF`) | Unpinned HEAD broke every rebase bake: #865, PR #866. | n |
