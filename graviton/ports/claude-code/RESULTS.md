# RESULTS — Claude Code 2.1.314 on DeBeOS (Haiku arm64, AWS Graviton)

Every claim links a raw log under `evidence/` (instance ID + UTC timestamp in the name).
Hosts: Haiku builder `i-0e7bed4928e0f7bf7` (c8g.4xlarge, builder image), Haiku acceptance
box `i-0e90d4b253c8134b3` (c8g.2xlarge, canonical `ami-058776e58bd902fba`, hrev59996),
Linux reference `i-0a14e8aca787249b7` (Ubuntu arm64, stock Bun 1.4.4, Node 24.22.2).

## Status at a glance

| Area | Result | Evidence |
|---|---|---|
| nodejs24 24.21.0 built natively (full ICU 78.3) | built, **not published** | `20261031T043619Z_i-0e7bed4928e0f7bf7_nodejs24-harvest-teaminfo.log` |
| Node acceptance on Haiku (Intl, zstd, execve, workers, TLS) | 21/23 pass; `fs.watch` ENOSYS ×2 | `20261031T113433Z_i-0e7bed4928e0f7bf7_node24-accept-haiku-v2.log` |
| node-gyp N-API addon on Haiku | builds + loads (`add(2,40) = 42`) with `CC=gcc CXX=g++` | `20261031T135458Z_i-0e7bed4928e0f7bf7_node24-gyp-addon-cxx.log` |
| Node test suites on Haiku | see "Node test suites" (in progress when written) | — |
| claude_code hpkg built natively by haikuporter | BUILD_OK, deterministic pass asserted on Haiku | `20261031T141546Z_i-0e7bed4928e0f7bf7_claude_code-recipe-build.log` |
| `claude --version`, `claude doctor` on Haiku | pass (`Platform: haiku-arm64`, rg found) | `20261031T142235Z_i-0e90d4b253c8134b3_claude-accept-install.log` |
| Compat layer vs stock Bun, replayed **on Haiku** | all pure suites pass (hash, width 3.6M, slice, wrap, semver, deepEquals, YAML, TOML) | same log |
| Interactive TUI, Haiku vs real binary on Linux | 60/64 byte-identical; 4 = tmux 3.4 vs 3.7c keycap rendering | `20261031T143250Z_workstation_tui-compare-linuxA-haikuB.log`, `…tmux-keycap-{haiku,linux}.log` |
| Headless `-p` with Read/Grep/Bash; edit, bash, /compact in TUI; Grep timing | **not run**: needs `ANTHROPIC_API_KEY` | — |
| 30-min soak with RSS (idle TUI) | alive throughout, 11 threads; RSS 323 MB → 185 MB after GC, then flat (184.8–185.1 MB) | `20261031T143535Z_i-0e90d4b253c8134b3_tui-soak-30min.log` |
| Ctrl-C at onboarding; headless `-p` without credentials | identical to the real binary on Linux (picker ignores Ctrl-C; `Not logged in · Please run /login`, rc 1) | `20261031T150644Z_i-0e90d4b253c8134b3_tui-ctrlc-and-headless.log`, `20261031T150818Z_i-0a14e8aca787249b7_tui-ctrlc-real-linux.log` |
| Global Worker scenarios replayed on Haiku | 13/13 match the Bun recording | `20261031T151048Z_i-0e90d4b253c8134b3_compat-scenarios-on-haiku.log` |
| Bun.spawn / Bun.serve scenarios on Haiku | spawn harness needs Haiku observers (it reads /proc); net 34/42 — being classified | same log |

## Phase 1 — tool sanity on the canonical image

`20261031T031440Z_i-07451948888de2466_phase1-sanity-tools.log`: git 2.54.0 (status, diff,
worktree, check-ignore) and ripgrep 15.2.0 (ignore files, hidden, `--json`, multiline,
UTF-8/CJK, exit codes) behave as on Linux once installed — the lean image ships neither
(nor curl/tar/gzip). bash 5.3 passes everything the shell snapshot uses **except process
substitution**: `cat <(…)` fails on a nonexistent `/dev/fd` (#868).

## Node.js 24 for Haiku

- Built with the overlay recipe (K1: `libicu* >= 78`, full ICU). `process.versions`:
  node 24.21.0, V8 13.6, ICU 78.3, Unicode 17, undici 7.29.1, libuv 1.52.1, OpenSSL 3.5.9.
- Grapheme segmentation (ZWJ family, flags, combining, Hangul, Devanagari, skin tone),
  `Intl.NumberFormat('de-DE')` → `1.234.567,891`, currency, DisplayNames, DateTimeFormat,
  Collator, `zlib.zstd*` round trip, `process.execve`, `worker_threads`, `fs.watchFile`
  polling and HTTPS with the system roots all match the Linux reference run
  (`20261031T075152Z_i-0a14e8aca787249b7_node-accept-linux-ref.log`).
- **`fs.watch` throws `ENOSYS`** (libuv has no Haiku fs-event backend). Effect on Claude
  Code measured per site in AUDIT.md "File watching on Haiku": settings hot reload, the task
  list and `FileChanged` hooks stop updating; nothing crashes. Owner decision pending
  (libuv backend vs polling).
- node-gyp: the official headers download and the addon builds; linking needs `CXX=g++`
  on the builder because the builder's environment names the triplet compiler.
- npm is not packaged (`--without-npm` in the recipe); node-gyp came from npm 12.2.1,
  integrity-pinned.

## Claude Code on Haiku

- `claude_code-2.1.314-1-arm64.hpkg` is built by haikuporter on Haiku from the pinned
  npm sources; every rule of the deterministic pass reports its asserted count in the
  build log (R1–R4, P4 116+10, P5–P17).
- Installed system-wide; the launcher refuses a non-system install (R1 paths are absolute).
- `doctor` prints OpenSSL's missing default cert dir on stderr (#869); TLS works.

## Open items (blocked or owner decisions)

1. `ANTHROPIC_API_KEY` for the model-backed acceptance tests (headless `-p` with tools,
   TUI edit/bash/compact, Grep timing). The key transport (presigned URL into the process
   environment only) is ready; the key has not been provided.
2. `fs.watch` on Haiku: libuv backend (system library change) or polling (compat).
3. Publishing nodejs24 / claude_code: not done, by instruction. claude_code redistributes
   Anthropic's proprietary bundle under its own terms (`LICENSE`: Anthropic Legal
   Agreements) — whether DeBeOS may publish it is a licensing decision for the owner.
4. Defects filed during this work: #867 (wave builder S3 client), #868 (bash /dev/fd),
   #869 (OpenSSL cert dir), #870 (devfs F_SETFL), plus new evidence on #843.
