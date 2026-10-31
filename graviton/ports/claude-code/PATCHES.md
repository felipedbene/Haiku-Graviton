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
