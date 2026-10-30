# RECON — Claude Code native on DeBeOS arm64

Recon date 2026-10-30. Every claim below cites a command or artifact. An earlier draft
of this file contained unverified claims (Node "installed" on canonical, a fabricated
`node --version` transcript, git "verified", "no nodejs patchset"); they were wrong and
are replaced here.

## 0.0 Tree inventory

- `origin/graviton` HEAD: `d31f20fba8` (2026-10-30 10:44 -0500).
- Last 60 days: 749 commits. By subject prefix: ops 36, docs 30, arm64 30, tcp 26,
  remote 20, ports 17, tooling 16, pipeline 15, bake 14, app_server/remote 13,
  ladybird 13, haikuports 13, app_server 12, ena 11, recipe 10, (233 unprefixed).

### Term hits → state (evidence)

| Item | Recipe in tree | Built | Published (green `packages.debene.dev/arm64/repo`) | Verified on hardware |
|---|---|---|---|---|
| nodejs20 20.15.1 | `graviton/haikuports-patches/recipes/nodejs20-20.15.1.recipe` (overlay: python3.10 pin only; patchset is upstream HaikuPorts') | PR #386 (merged 2026-09-19): native RC=0 on c7g.8xlarge | **yes**, `nodejs20-20.15.1-2` | **yes, today**: `pkgman install nodejs20` on canonical → `v20.15.1`; `process.platform/arch/v8/uv/icu` = `haiku arm64 11.3.244.8-node.23 1.52.1 74.1` |
| npm | — | — | **no** — nodejs20 provides only `cmd:node`, `cmd:corepack` | `npm: command not found` after install |
| V8 | bundled in Node 20 (Haiku V8 port lives in the HaikuPorts patchset, per PR #386) | via Node 20 | via nodejs20 | via node above |
| libuv | upstream HaikuPorts | yes | `libuv-1.52.1-1` | via node (`uv=1.52.1`) |
| ICU | icu74 (bootstrap); icu78 recipe in overlay | yes | `icu74-74.1_bootstrap`, `icu78-78.3` | node links icu74 **bootstrap**; PR #386: `Intl.NumberFormat('de-DE')` throws (incomplete icudata) |
| ripgrep | none in tree (crate→hpkg tooling, `graviton/docs/crate-to-hpkg.md`) | yes | `ripgrep-15.2.0-1` | **yes, today**: `ripgrep 15.2.0` |
| git | upstream HaikuPorts | yes | `git-2.54.0-1` | **yes, today**: `git version 2.54.0` |
| bash | upstream | yes | `bash-5.3_bootstrap-1` (**bootstrap build**) | on canonical: `GNU bash, version 5.3.0(1)-release (aarch64-unknown-haiku)` |
| rust | `rust_bin` | yes | `rust_bin-1.101.0~nightly_e9bbadf68-1` | not exercised today |
| bun | **none** — no recipe, issue, PR, or branch | — | — | — |
| claude | only tooling hooks / SOP mentions; no port | — | — | — |

Canonical AMI `ami-058776e58bd902fba` (SSM `/haiku-graviton/canonical-ami-id`), booted as
`i-0138eac1bd963243d` (SSM Online, PlatformName `Haiku`); `uname`:
`Haiku … R1~beta6+development hrev59996 Oct 19 2026 arm64`.
Installed out of the box: `bash`, `icu74`, `openssl3`. **Missing**: node, rg, git, npm,
bun, and `which` itself. Repo preconfigured: `http://packages.debene.dev/arm64`.

Tracker history: #93 (Node.js + coreutils native, closed), PRs #374/#386 (Node scope +
build, merged), #116/#362 (crate→hpkg tooling, merged). Nothing open on any of these.

## a. Claude Code distribution (current)

`npm view @anthropic-ai/claude-code@2.1.314`:
- `bin: { claude: "bin/claude.exe" }`, `engines.node: ">=22.0.0"`, no dependencies;
  `optionalDependencies` = per-platform native packages (linux x64/arm64 ± musl,
  darwin x64/arm64, win32 x64/arm64). **No Haiku package.**
- `@anthropic-ai/claude-code-linux-arm64@2.1.314` contains one file, `claude`:
  ELF aarch64, 297,067,016 bytes, interpreter `/lib/ld-linux-aarch64.so.1`,
  **Bun v1.4.4** standalone executable (`Bun v1.4.4 (Linux arm64)` string).
- Embedded payload: ELF section `.bun` (179.7 MB), trailer `\n---- Bun! ----\n`.
  Offsets struct: byte_count 179,700,121; modules table 149,292 bytes =
  **2871 modules × 52-byte records**; entry_point_id 7; compile_exec_argv 19 bytes.
  First module `/$bunfs/root/chunk-4se81ra6.js`, contents 779 B, bytecode 102.9 MB
  (bytecode blob shared/large — to be understood in step 1).
- License text: `© Anthropic PBC. All rights reserved. Use is subject to the Legal
  Agreements outlined here: https://code.claude.com/docs/en/legal-and-compliance.`

Remaining for (a): full runtime-dependency inventory → RUNTIME-DEPS.md (plan step 2).

## b–e

Pending extraction plan steps 1–5. Not answered yet; nothing here is claimed.
