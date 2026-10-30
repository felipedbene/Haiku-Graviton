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

## B. Runtime support installed next to the bundle

| ID | File | What | Why | Upstream-able |
|---|---|---|---|---|
| C1 | `bun-compat.mjs` (`node --import`) | load hook serving Bun "text"-loader files as their contents | Bun's `text` loader makes `import`/`require` of a file yield its string. Node has no such loader. The hook serves exactly the 98 files `LOADERS.json` marks as text, keyed by file because two `.mjs` files are text, not code. | n |
| C2 | `node_modules/ws` | npm `ws` 8.22.0, integrity-pinned in `tools/deps.lock` | Bun provides `ws` as a built-in; the bundle imports it externally (4 sites). | n |
| C3 | `node_modules/node-fetch` | re-exports Node's native `fetch`/`Headers`/`Request`/`Response`/`FormData`/`Blob`/`File` | Bun's built-in `node-fetch` is the same re-export of its native fetch. 1 dynamic import, used when `window` is absent. | n |
| C4… | `bun-compat.mjs` | `Bun.*` APIs: one entry each as implemented and diff-tested | — | — |

## C. Recipes

| ID | File | What | Why | Upstream-able |
|---|---|---|---|---|
| K1 | `haikuports-patches/recipes/nodejs24-24.21.0.recipe` | upstream HaikuPorts recipe (`21c227aca765`) with `devel:libicu* >= 74` → `>= 78` | Forces full ICU 78 instead of the `icu74` **bootstrap** build (incomplete locale data; `Intl.NumberFormat('de-DE')` throws, PR #386). | y, if upstream wants full ICU |

## D. Pipeline (separate PR, needed to build K1)

| ID | File | What | Why | Upstream-able |
|---|---|---|---|---|
| X1 | `graviton/scripts/haiku-provision-native-builder` | pin haikuporter to `faa30769682f` (`HP_REF`) | Unpinned HEAD broke every rebase bake: #865, PR #866. | n |
