# RESULTS — Claude Code 2.1.314 on DeBeOS (Haiku arm64, AWS Graviton)

Every claim links a raw log under `evidence/` (instance ID + UTC timestamp in the name).
Hosts: Haiku builder `i-0e7bed4928e0f7bf7` (c8g.4xlarge, builder image), Haiku acceptance
box `i-0e90d4b253c8134b3` (c8g.2xlarge, canonical `ami-058776e58bd902fba`, hrev59996),
Linux reference `i-0a14e8aca787249b7` (Ubuntu arm64, stock Bun 1.4.4, Node 24.22.2).

## Status at a glance

| Area | Result | Evidence |
|---|---|---|
| nodejs24 **24.22.2-1** (issue 954), against openssl3 3.5.9-3 + brotli 1.2.1-2 | built natively, **staged, not published**; h2coalesce954 0/20 (was 20/20), many-writes-and-destroy 0 hung / 5120 (was 46 / 2560); suites 4582/23 + 119/2, every change attributed; Claude Code acceptance unchanged | section "2026-11-03 — nodejs24 24.22.2-1" below |
| nodejs24 24.21.0 built natively (full ICU 78.3) | built, **not published** | `20261031T043619Z_i-0e7bed4928e0f7bf7_nodejs24-harvest-teaminfo.log` |
| Node acceptance on Haiku (Intl, zstd, execve, workers, TLS) | 21/23 pass; `fs.watch` ENOSYS ×2 | `20261031T113433Z_i-0e7bed4928e0f7bf7_node24-accept-haiku-v2.log` |
| node-gyp N-API addon on Haiku | builds + loads (`add(2,40) = 42`) with `CC=gcc CXX=g++` | `20261031T135458Z_i-0e7bed4928e0f7bf7_node24-gyp-addon-cxx.log` |
| Node test suites on Haiku (`parallel` + `sequential`) | parallel 4110 ok (323 skip) / 397 not ok (275 timeout); sequential 94 ok / 26 not ok (5 timeout) | `20261031T174513Z_i-0e7bed4928e0f7bf7_node24-test-suites-results.log` |
| claude_code hpkg built natively by haikuporter | BUILD_OK, deterministic pass asserted on Haiku | `20261031T141546Z_i-0e7bed4928e0f7bf7_claude_code-recipe-build.log` |
| `claude --version`, `claude doctor` on Haiku | pass (`Platform: haiku-arm64`, rg found) | `20261031T142235Z_i-0e90d4b253c8134b3_claude-accept-install.log` |
| Compat layer vs stock Bun, replayed **on Haiku** | all pure suites pass (hash, width 3.6M, slice, wrap, semver, deepEquals, YAML, TOML) | same log |
| Interactive TUI, Haiku vs real binary on Linux | 60/64 byte-identical across tmux 3.4 (Linux) vs 3.7c (Haiku); the 4 keycap snapshots are the terminal: with tmux 3.7c on both sides (built from the Haiku recipe's pinned source), all 33 text-scenario snapshots incl. those 4 are identical (`20261101T060055Z_workstation_tui-compare-tmux37c.log`) | `20261031T143250Z_workstation_tui-compare-linuxA-haikuB.log`, `…tmux-keycap-{haiku,linux}.log` |
| Model access from Haiku (Amazon Bedrock, instance-profile credentials via IMDS) | `-p` → `pong`, rc 0, 6 s | `20261101T002859Z_i-0e90d4b253c8134b3_bedrock-headless-smoke.log` |
| Headless `-p` with Read, Grep, Bash | correct answer `aurora-7\|src/a.js,src/deep/b.js\|Haiku`; each tool call and result in the stream | `20261101T003029Z_i-0e90d4b253c8134b3_bedrock-headless-tools.log` |
| Grep tool vs direct rg (60 MB tree, 3273 files) | Grep tool 0.407 s (PreToolUse→PostToolUse hooks) vs rg median 0.144 s; same 3 files; hooks work | `20261101T003123Z_i-0e90d4b253c8134b3_bedrock-grep-timing.log` |
| Interactive TUI with the model: edit, bash, resize | edit applied and verified on disk; `echo bash-ok-$(uname -s)` → `bash-ok-Haiku`; reflow at 70×24 and 130×40 | `20261101T003220Z_i-0e90d4b253c8134b3_bedrock-tui-session.log` |
| `/compact` + recall | Haiku 5/5; Linux real binary 3/3 and node+compat 3/3 (first 2 Linux tries had no IMDS role yet) | `20261101T004621Z_i-0e90d4b253c8134b3_compact-haiku-x5.log`, `20261101T004647Z_i-0a14e8aca787249b7_compact-linux-x5.log` |
| `/compact` stuck after a refused command + queued message | same hang with the real binary on Linux: upstream behaviour, not the port | `20261101T005814Z_i-0a14e8aca787249b7_bedrock-tui-session-real-linux.log` |
| End-user install from the DeBeOS repo | `pkgman install claude_code` → claude_code 2.1.314-2 + libuv 1.52.1-3 (pulled by `libuv_fs_event`); `--version`, `doctor`, FileChanged hook, vectors all pass; 0 `.node` files. On images whose libuv is vendor "Haiku Project", pkgman asks once to allow the vendor change to DeBeOS | `20261101T034345Z_i-0e90d4b253c8134b3_enduser-fresh-install-3.log` |
| fs.watch in Claude Code, before/after the libuv backend | libuv 1.52.1-1: `FileChanged: watcher error: ENOSYS`, hook fired 0×; 1.52.1-2+: `fs.watch probe: delivers`, hook fired 2× for 2 external edits | `20261101T024050Z_i-0e90d4b253c8134b3_filechanged-before-after-libuv.log` |
| 30-min working soak (TUI + model, tool-using task every ~15 s) | Haiku 123/123 tasks, 4–6 s each, one process throughout; Linux real binary all tasks 3–4 s; Linux node+compat 125/125. RSS over the same workload: Haiku 428 → 575 MB (+147 MB); Linux node+compat 467 → 780 MB (+313 MB, `20261101T051952Z_i-0a14e8aca787249b7_working-soak-v5-nodecompat-linux.log`), so the growth is the runtime and conversation, not the port | `20261101T021011Z_i-0e90d4b253c8134b3_working-soak-v3-haiku.log`, `20261101T015433Z_i-0a14e8aca787249b7_working-soak-v3-real-linux.log`, `20261101T024336Z_i-0a14e8aca787249b7_working-soak-v3-nodecompat-linux.log` |
| 30-min soak with RSS (idle TUI) | alive throughout, 11 threads; RSS 323 MB → 185 MB after GC, then flat (184.8–185.1 MB) | `20261031T143535Z_i-0e90d4b253c8134b3_tui-soak-30min.log` |
| Ctrl-C at onboarding; headless `-p` without credentials | identical to the real binary on Linux (picker ignores Ctrl-C; `Not logged in · Please run /login`, rc 1) | `20261031T150644Z_i-0e90d4b253c8134b3_tui-ctrlc-and-headless.log`, `20261031T150818Z_i-0a14e8aca787249b7_tui-ctrlc-real-linux.log` |
| Global Worker scenarios replayed on Haiku | 13/13 match the Bun recording | `20261031T151048Z_i-0e90d4b253c8134b3_compat-scenarios-on-haiku.log` |
| Bun.spawn / Bun.serve scenarios on Haiku | spawn harness needs Haiku observers (it reads /proc); net 34/42 — being classified | same log |

## 2026-11-01 — fresh trunk image 7036360701

This is the acceptance and Node-suite slice above, re-run on one fresh launch of the trunk bake
`ami-01b12b4c6e286377e` (`haiku-oven-1793564825`, `debeos-r70738-g7036360701`, hrev59996; not canonical,
not promoted). Instance `i-09e325e6c72394c08`: c8g.4xlarge, 60 GiB, `subnet-0d4f6b196024e9b92`,
`sg-0efc97cc673949063`, key `haiku-rdclient`, `ssm-instance-profile`, tags `Project=haiku-graviton`
and `auto-stop=no`. It has been terminated. The packages came from the DeBeOS repo, which is
`packages.debene.dev` -> `debeos-repo-green` (`20261101T221650Z_workstation_trunk-repo-origin-and-ami.log`).
They were claude_code 2.1.314-2, nodejs24 24.21.0-2 and libuv 1.52.1-4, installed by `pkgman install`
with no `-y` and the answers fed on stdin (`20261101T204555Z_i-09e325e6c72394c08_verify-trunk-install.log`).
Each check is the script from the earlier evidence log, run unchanged unless noted. Evidence names
below drop the `_i-09e325e6c72394c08_` part.

| Check | Canonical-era result | Trunk 7036360701 | Evidence (`20261101T…`) |
|---|---|---|---|
| Node acceptance (Intl, zstd, execve, workers, TLS, fs.watch) | 21/23, `fs.watch` ENOSYS ×2 (libuv -1) | **23/23 ALL PASS**. fs.watch delivers `rename`/`change`, from libuv 1.52.1-4 and not from the image | `204718Z…trunk-node24-accept` |
| node-gyp N-API addon, `CC=gcc CXX=g++` | `add(2,40) = 42` | `add(2,40) = 42`, after installing `haiku_devel` and `binutils` from the repo: the repo's bootstrap `gcc` pulls in neither (`stdint.h` missing, then `as` missing) | `204759Z…gyp-addon-cxx`, `204838Z…-devel`, `204913Z…-binutils` |
| `claude --version`, `doctor`, `cc-teaminfo`, compat vectors | pass | pass: `Platform: haiku-arm64`, `Search: OK`, `check-vectors: ALL PASS`, 0 `.node` files | `205012Z…trunk-claude-accept` |
| `claude -p` via Bedrock (instance profile) | `pong`, 6 s | `pong`, rc 0, 7 s; JSON `is_error:false` | `205237Z…trunk-bedrock-headless-smoke` |
| Headless Read/Grep/Bash | `aurora-7\|src/a.js,src/deep/b.js\|Haiku` | identical answer | `205312Z…trunk-bedrock-headless-tools` |
| Grep tool vs rg | 0.407 s vs rg 0.144 s | 0.389 s vs rg 0.142 s, same 3 files, hooks fire | `205340Z…trunk-bedrock-grep-timing` |
| Interactive TUI: edit, bash, resize, /compact | edit on disk, `bash-ok-Haiku`, reflow; /compact hang after the queued message (upstream) | same on every point | `205420Z…trunk-bedrock-tui-session` |
| `/compact` + recall ×5 | 5/5 | 5/5 | `205641Z…trunk-compact-haiku-x5` |
| End-user repo install + FileChanged through `libuv_fs_event` | hook fired 2×, `fs.watch probe: delivers` | hook fired 2×, `delivers`, vectors pass | `205933Z…trunk-enduser-fresh-install-3` |
| Ctrl-C at onboarding, `-p` with no credentials | identical to Linux | identical to the earlier Haiku run | `210229Z…trunk-tui-ctrlc-and-headless` |
| Worker scenarios vs Bun | 13/13 | 13/13. In the same old script, the spawn/net checks now refuse the old recording (`no __platform entry`), because the current harness needs the re-recorded set | `210335Z…trunk-compat-scenarios-on-haiku`, `210428Z…trunk-compat-net-stderr` |
| Bun.spawn / Bun.serve scenarios vs Bun (v2 harness and recordings) | spawn 188/23/2/1/4, net 35/6/1 | **identical**: spawn 188 match / 23 platform / 2 unobservable / 1 mismatch / 4 declared; net 35 / 6 / 1 | `210524Z…trunk-scen-haiku-node-v2` |
| TUI snapshots vs real binary on Linux | 60/64 (tmux 3.4), 33/33 (tmux 3.7c) | 60/64, the same 4 keycap snapshots; 33/33 | `210645Z…trunk-tui-capture-B-haiku`, `211247Z_workstation_trunk-tui-compare-{linuxA-haikuB,tmux37c}` |
| 30-min working soak | 123/123, 4–6 s, RSS 428 → 575 MB | **135/135**, 3–5 s (93 at 3 s), one process, RSS 429 → 482 MB | `211458Z…trunk-working-soak-v3-haiku` |
| 30-min idle TUI soak | alive, 11 threads, 323 → 185 MB | alive, 12 threads, 323 → 185 MB, flat | `214544Z…trunk-tui-soak-30min` |

**Node 24 test suites.** Same command as the 24.21.0-2 run:
`: | python3 tools/test.py --shell=/boot/system/bin/node -j 16 -p tap <suite>`, with stdin a pipe
(#870). It ran in a 24.21.0 tree built from the recipe's source tarball (sha256 checked) and its two
patchsets in recipe order (`211329Z…trunk-node-src-prepare`). Plans were 1..4507 and 1..120, so
nothing was dropped (`221638Z…trunk-nodejs24r2-full-suites`).

| Suite | 24.21.0-2 baseline (builder `i-0e7bed4928e0f7bf7`, hrev59996) | Trunk 7036360701 |
|---|---|---|
| parallel | 4172 ok (323 skip) / 335 not ok, 6193 s | **4451 ok (323 skip) / 56 not ok (2 timeout), 396 s** |
| sequential | 109 ok / 11 not ok, 1582 s | **113 ok / 7 not ok (1 timeout), 497 s** |

The baseline was the builder image, not canonical: that is where RESULTS.md measured the suites. The
builder's TAP files are gone. Its per-test list was rebuilt from the logged lists: r1 not-ok, minus
fixed in r2, plus r2's one newly failing test. That gives 335 + 11, matching the logged totals.
Per-test diff, each moved test labelled with its row in the root-cause table:
`235350Z_workstation_trunk-node-suites-vs-canonical`.

- **284 went from fail to pass (280 parallel, 4 sequential).** By triaged cause, with the merged
  fix that addresses it: #871 ×265, by PR 895 (accepted sockets report the peer's FIN). #883 ×5,
  `fs-readfile-*`/`fs-realpath-pipe` (PR 908, `/dev/stdin`, `/dev/fd`). #884 ×2 (PR 904). #886 ×2,
  `dgram-send-empty-{array,buffer}` (PR 903). #885, `net-pipe-connect-errors` (PR 901). #887,
  `cluster-dgram-1` (PR 905). #888, `dgram-multicast-set-interface` (PR 907). #889,
  `dgram-error-message-address` (PR 909). #890, `cluster-concurrent-disconnect` (PR 911). #892,
  `tls-enable-keylog-cli` (PR 910). #882, `fs-error-messages` (PR 906, EEXIST). #879,
  `util-promisify` (PR 912). Two others passed: `seq/cpu-prof-dir-worker` (patchset 1/3; failed in
  the r2 run) and `seq/net-connect-econnrefused`, which RESULTS.md already counted as load-dependent.
  The run was ~15× shorter, and that comes from the #871 timeouts being gone.
- **1 went from pass to fail: `parallel/test-fs-stat-date`.** It is caused by PR 912. Before, BFS
  read atime back as "now", so the test's own guard (`utimes(2,2)` must read back 2000 ms) made it
  skip. Now atime round-trips, the test runs, and it fails 5/5 on BFS's ~262 µs timestamp resolution:
  `expected -1 ± 2.2e-16, got -1.23136`. `bfs.h:226,276-294,346`: -1 ms -> fraction `0xEE20` ->
  `0xEE20 << 14` ns. Filed as **#929** (`223226Z…trunk-fs-stat-date-repro`,
  `223330Z…trunk-bfs-time-resolution-probe`).
- **62 still fail.** None of them has a merged fix on trunk, except three whose triaged fix is in
  but which still fail for a remaining cause (`223525Z…trunk-residual-fixed-issue-tests`):
  - `dgram-connect-send-empty-{array,buffer}`: timeout 3/3. The datagram is now sent (PR 903)
    and queued, but a `poll()` that starts after it arrived never reports it readable. That is
    open **#899**; new evidence commented there (`235018Z…trunk-udp-zero-iovlen-probe2`,
    `234800Z…trunk-udp-zero-length-node-strace`).
  - `fs-promises` (was #879): it now gets past atime and fails on `link()` EPERM. That is open
    #882 (no hard links); commented there.
  - `http-writable-true-after-close` (was #871): same assertion 3/3. The FIN is still seen only
    after the server writes, which PR 895 does not change. Commented on #871.
  - The rest, by the issues that are still open: #873 ×19, #876 ×13 (PR 920 open), class (c) ×9,
    #882 ×4, #881 ×2, #877 ×2, nghttp2 packaging ×2, and one each for #893, #878, #891 (PR 913
    open), #880, euid/egid (uid 0), setproctitle (`ps -p`) and worker-memory.

## 2026-11-02 — fresh canonical image fe7fef0b47

This is the same verification on one fresh launch of the new canonical `ami-03c3545c37f391c1f`
(`haiku-oven-1793580871`, `debeos-r70748-gfe7fef0b47`, `canonical=true`). Over 7036360701 it adds
PRs 913, 920–928, 931–937 and 916. Instance `i-084afae11b14226f8`: c8g.4xlarge, 60 GiB,
`subnet-0c98257f993bfc1b2`, `sg-0efc97cc673949063`, key `haiku-rdclient`, `ssm-instance-profile`,
tags `Project=haiku-graviton`, `auto-stop=no`, Name `cc-verify-trunk-fe7fef0b47`. It has been
terminated (`20261102T080118Z_workstation_fe7-repo-origin-and-ami.log`). The packages came from
green, the same as before: claude_code 2.1.314-2, nodejs24 24.21.0-2 and libuv 1.52.1-4
(`20261102T055220Z_i-084afae11b14226f8_fe7-install.log`). The scripts are the ones used for
7036360701, with fresh presigned URLs. Evidence names below drop the `_i-084afae11b14226f8_` part.

| Check | 7036360701 | fe7fef0b47 | Evidence (`20261102T…`) |
|---|---|---|---|
| Node acceptance | 23/23 | 23/23 ALL PASS | `055314Z…fe7-node24-accept` |
| node-gyp addon (with `haiku_devel` + `binutils` from the repo) | `add(2,40) = 42` | `add(2,40) = 42` | `055343Z…fe7-node24-gyp-addon-cxx` |
| `--version`, `doctor`, `cc-teaminfo`, vectors | pass | pass (`haiku-arm64`, `Search: OK`, `ALL PASS`) | `055447Z…fe7-claude-accept` |
| `claude -p` via Bedrock | `pong`, 7 s | `pong`, rc 0, 8 s | `055703Z…fe7-bedrock-headless-smoke` |
| Headless Read/Grep/Bash | correct | `aurora-7\|src/a.js,src/deep/b.js\|Haiku` | `055732Z…fe7-bedrock-headless-tools` |
| Grep tool vs rg | 0.389 s / 0.142 s | 0.392 s / 0.144 s | `055812Z…fe7-bedrock-grep-timing` |
| Interactive TUI: edit, bash, resize, /compact | pass | pass (`bash-ok-Haiku`, edit on disk; same /compact behaviour) | `055905Z…fe7-bedrock-tui-session` |
| `/compact` + recall ×5 | 5/5 | 5/5 | `060121Z…fe7-compact-haiku-x5` |
| FileChanged through `libuv_fs_event` | fired 2× | fired 2×, `delivers` | `060402Z…fe7-enduser-fresh-install-3` |
| Ctrl-C, `-p` with no credentials | identical | identical | `060630Z…fe7-tui-ctrlc-and-headless` |
| Worker scenarios | 13/13 | 13/13 | `060746Z…fe7-compat-scenarios-on-haiku` |
| Bun.spawn vs Bun | 188 / 23 platform / 2 / **1 mismatch** / 4 | 188 / **24** platform / 2 / **0 mismatch** / 4, check rc 0 | `060830Z…fe7-scen-haiku-node-v2` |
| Bun.serve/listen/connect vs Bun | 35 / 6 / 1 | 35 / 6 / 1 (the Response-defaults mismatch, Node's) | same |
| TUI snapshots vs Linux | 60/64, 33/33 | 60/64 (same 4 keycap snapshots), 33/33 | `061007Z…fe7-tui-capture-B-haiku`, `061539Z_workstation_fe7-tui-compare-{linuxA-haikuB,tmux37c}` |
| 30-min working soak | 135/135, 3–5 s | 123/123, 4–8 s, one process, RSS 420 → 577 MB | `063556Z…fe7-working-soak-v3-haiku` |
| 30-min idle soak | 323 → 185 MB | alive, 12 threads, 324 → 184 MB | `070633Z…fe7-tui-soak-30min` |

- **Spawn:** `bad interpreter` was the one mismatch. It is now an errno-convention platform
  difference only (spawn fails with ENOENT, as Bun on Linux does). PR 926 caused this (libroot fails
  exec/spawn of a script whose interpreter is missing). `KNOWN_DEFECTS` in
  `compat/test/platform-diff.mjs` still describes the old behaviour; the checker doesn't need it now.
- **Working soak:** the per-task time includes the Bedrock round trip. Local tool execution measured
  the same as on 7036360701 (Grep 0.392 s vs 0.389 s, rg 0.144 s vs 0.142 s). The slower tasks are
  not attributed to the image.

**Node 24 suites** (`061723Z…fe7-nodejs24r2-full-suites`). Same tree preparation
(`061553Z…fe7-node-src-prepare`), same command, stdin a pipe. Full plans: 1..4507, 1..120.

| Suite | 7036360701 | fe7fef0b47 |
|---|---|---|
| parallel | 4451 ok (323 skip) / 56 not ok (2 timeout) | **4469 ok (323 skip) / 38 not ok (3 timeout)**, 583 s |
| sequential | 113 ok / 7 not ok (1 timeout) | 113 ok / 7 not ok (1 timeout), 496 s |

Per-test diff: `20261102T080118Z_workstation_fe7-node-suites-vs-7036360701.log`.

- **19 went from fail to pass:**
  - #876 ×13 (`domain-no-error-handler-abort-on-uncaught-0…9`, `domain-throw-error-then-throw-from-uncaught-exception-handler`, `domain-with-abort-on-uncaught-exception`, `crypto-no-algorithm`), by PR 920: a crash or abort() is now reported by its signal.
  - #893 `cluster-worker-kill-signal`, by PR 923: a dying team's fds close before its death is reported.
  - #878 `fs-write-sigxfsz`, by PR 922 (RLIMIT_FSIZE).
  - #891 `http2-max-invalid-frames`, by PR 913 (TCP reset after close).
  - #881 `os-process-priority`, by PR 925 (getpriority round trip; set 19/10/5/1/0 reads back the same).
  - #880 `process-umask-mask`, by PR 924.
  - #877 `seq/child-process-emfile`, by PR 921 (the NOFILE hard limit can be lowered: `ulimit -Hn 256` works).
- **1 went from pass to fail: `seq/net-connect-econnrefused`.** It passed 9/10 and then 30/30 alone
  (`073735Z…fe7-residual-changed-tests`, `075637Z…fe7-residual-probes2`). It is the same
  load-dependent single failure this suite showed in the 24.21.0-2 run. One run in 40 alone is not
  enough to substantiate a defect, so it is not filed and not attributed.
- **State changed among tests failing both times:**
  - `worker-init-failure`: FAIL -> TIMEOUT. PR 921 lets `ulimit -n 128` work, so the test now runs and hangs (0/2 alone; a direct run hit the 60 s cap). That is open **#918**; commented there.
  - `parallel/test-os` stays FAIL with a new cause. PR 925 cleared the priority assertion at `test-os.js:92`, and it now fails at `:104`, `os.cpus().length > 0`. `os.cpus()` is `[]`, but the kernel reports 16 CPUs and 16 CORE topology nodes (`075855Z…fe7-cpu-topology-probe`). The HaikuPorts patchset comments out the body of `GetCPUInfo` (`src/node_os.cc`), so this is a nodejs24 packaging defect and the image is fine. That is open **#917**; commented there.
- **Still failing, unchanged:** 42 others. #873 ×19, class (c) ×9, #882 ×5 (incl. `fs-promises`), #899 ×2 (`dgram-connect-send-empty-*`), nghttp2 packaging ×2, and one each for #929 (`fs-stat-date`), #871 (`http-writable-true-after-close`), uid 0 (`process-euid-egid`), `setproctitle` and `worker-memory`.

## 2026-11-03 — nodejs24 24.22.2-1 (issue 954), fresh image 4ef344f5fa

Upgrades the nodejs24 port from 24.21.0-3 to **24.22.2-1**. As of 2026-11-02, 24.22.2 (released
2026-10-27) is the newest 24.22.x on nodejs.org. Upstream fixed the http2 hang behind #954 in
24.22.0 (nodejs/node PRs 66016 and 63249). The image is the trunk bake `ami-0714f26bbe99a9bfb` (4ef344f5fa,
now `canonical=true`), the same image as the section below. The staged build was **compiled against
openssl3 3.5.9-3** (PR 983, `bn(64,64)`, staged at `openssl359r3/`, not yet in green) and
**tested with 3.5.9-3 installed**. Evidence names below drop the `_i-…_node2422-` part.
Instances: builder `i-066dc28d2cbad4d38` (c7g.8xlarge, 100 GiB), "before" box `i-026c80afb9d541163`,
baseline box `i-0ca2654bea9e3130b`, final box `i-08822cbd7be76701c` (c8g.4xlarge, 60 GiB, all three).
Linux arm64 controls: `i-0fa1104eea75865c6`, `i-02fbec148c76653db`, `i-07e75f6fd31c11c0e`. All are
tagged `Project=haiku-graviton`, `auto-stop=no`, and all have been terminated.

**Source.** The 24.22.2 tarball sha256 `b2f5443a…` matches `SHASUMS256.txt`. That file's signature
verifies against key `5BE8A3F6…B168D356` (Antoine du Hamel), which is listed in nodejs/release-keys
`keys.list` (`180603Z_workstation…source-verify`). After the build succeeded, the tarball was mirrored to
`sources.debene.dev/nodejs/` the #172 way, and the copy read back matches the same sha256
(`223740Z_workstation…source-mirror`). 24.21.0 was never mirrored; that URL still returns 403.

**Patchsets** (PATCHES.md K5, `181220Z_workstation…patchset-rebase`). Every hunk was carried over
unchanged, none was dropped, and none was reworked:
- **HaikuPorts `nodejs-24.21.0.patchset` → `nodejs-24.22.2.patchset`.** It has 6 commits and 30 files.
  They apply cleanly with `git am` and also with a 3-way rebase, which gives an identical tree. Seven
  hunks only moved: `common.gypi` 1–6 (+3/+3/+3/+6/+6/+14 lines) and the x86_64 `openssl.gypi` (+3).
  Regenerated with `git format-patch`, the patchset now applies with no offset, and the only lines
  that differ from the old one are `From`/`index`/`@@`. Upstream changed 7 of the patched files
  between the two releases, each away from the patched lines; V8 is still 13.6.233.17.
- **DeBeOS `nodejs-24.21.0-debeos.patchset` → `nodejs-24.22.2-debeos.patchset`.** It has 4 patches.
  On top of the rebased HaikuPorts set they apply byte-for-byte, with no offset. The only change is
  that three "Applies on top of" lines now name the new file.
- `patch -p1` on Haiku applied the pair the same way: 30 + 4 files, rc 0, with no
  offset/fuzz lines (`003903Z…final-node-src-prepare`).

**Build** (haikuporter on the builder, libuv 1.52.1-5 from green):
1. **Attempt 1** stopped at `src/node_zlib.cc:2153`: `BROTLI_DECODER_ERROR_FORMAT_BLOCK_SWITCH` was
   not declared (`202117Z…build1-failure`). Node 24.22.0 exposes that brotli error code with the
   CVE-2026-111497 fix, and only brotli **1.2.1** has it. Green serves 1.2.0, which is vulnerable;
   that is filed as **#985**. HaikuPorts' own `brotli-1.2.1.recipe` built unmodified as
   brotli/_devel/_python310/_python3.14 1.2.1-2 (`202300Z…brotli121-build`). The nodejs24 recipe
   now requires `lib:libbrotli{dec,enc} >= 1.2.1`.
2. **Attempt 2**, built against openssl3 3.5.9-2: `exit 0` (`222920Z…build2-result-baseline-ossl2`).
   It is kept only as an engineering baseline and is not staged.
3. **Attempt 3**, built after installing the sha-checked `openssl359r3/` set (`223011Z…builder-start3-ossl3`):
   `configuration.h` has `SIXTY_FOUR_BIT_LONG`, and the haikuporter chroot lists
   `openssl3-3.5.9-3`/`openssl3_devel-3.5.9-3`. Result `exit 0`. The packaged node reports openssl
   3.5.9, brotli 1.2.1, nghttp2 1.63.0 and libuv 1.52.1. `listimage` shows it loading
   `/boot/system/lib/libcrypto.so.3`, sha256 `3bc4c731…`; the 3.5.9-2 box had `66a6050f…`.
   RSA sign/verify, DH and `checkPrime` pass (`003626Z…build3-ossl3-result-and-stage`).

**#954 on the final build** (`004015Z…final-954`). The "before" column is the same image with green
nodejs24 24.21.0-3 (`182356Z…954-before`):

| Check | 24.21.0-3 (before) | 24.22.2-1, openssl 3.5.9-2 (baseline) | **24.22.2-1, openssl 3.5.9-3 (staged)** |
|---|---|---|---|
| `h2coalesce954.mjs coalesced` ×20 | **20/20 server never closes** | 0/20 | **0/20** |
| `h2coalesce954.mjs separate` ×20 (control) | 0/20 | 0/20 | 0/20 |
| `test-http2-many-writes-and-destroy`, `hang954.py` 8-way, 30 s deadline | **46 hung / 2560** (12+11+12+11), all fin-wait2/close-wait like #954 | 0 / 5120 | **0 hung, 0 nonzero / 5120**; max completed run 0.53 s |

**Node suites, 24.22.2 tree** (the recipe's tarball and both patchsets,
`003903Z…final-node-src-prepare`). Run as `: | python3 tools/test.py --shell=/boot/system/bin/node -j 16 -p tap`,
stdin a pipe, full plans (`004249Z…final-full-suites`):

| Suite | 4ef344f5fa rev 3 (RESULTS, below) | same image, 24.21.0-3 (control) | **24.22.2-1 (staged)** |
|---|---|---|---|
| parallel | plan 1..4507: 4487 ok (323 skip) / 20 not ok (0 timeout) | 4487 ok (323 skip) / 20 not ok (1 timeout) | **plan 1..4605: 4582 ok (333 skip) / 23 not ok (1 timeout)**, 498 s |
| sequential | 1..120: 118 / 2 | 118 / 2 | **1..121: 119 / 2**, 119 s |

24.22.2 adds 99 test files: 98 in parallel and 1 in sequential, which accounts for the plan growth.
There are 10 more skips (323 → 333). Neither the rev-3 run nor the control kept per-test skip lists,
so those 10 are not attributed one by one; the final run's skip list, each with its reason, is in
`005406Z…final-residual-changed-tests`. Per-test diff:
`20261103T012324Z_workstation_node2422-suites-vs-4ef-rev3.log`. Attribution, against the 4ef rev-3 run:
- **1 went from fail to pass:** `child-process-spawnsync-args`. It was the one-off `spawnSync pwd
  EPIPE` in the rev-3 run, 1 failure in 43, and is not attributed to anything.
- **1 went from pass to fail: `http2-sensitive-headers`.** It fails 5/5 here and passes on 24.21.0.
  The cause is the system nghttp2 1.63.0, not DeBeOS: on Linux, one 24.22.2 `--shared-nghttp2`
  build fails 0/10 with nghttp2 1.63.0 loaded and passes 10/10 with 1.70.0 loaded
  (`225046Z…linux-shared-nghttp2-ab`). Filed as **#991**, together with the two older
  `misbehaving-flow-control` failures of the same cause.
- **`net-connect-abort-controller` TIMEOUT.** It passes 10/10 alone and hangs 4/48 under `--repeat 48`.
  It also timed out on the same image with 24.21.0-3 (control), and Linux gives 48/48. This is
  **#946**, which was fixed after this image; commented there.
- **2 new tests fail:**
  - `fs-rmSync-special-char-additional-error`: uid 0. As root, `rmSync` succeeds and the cleanup
    `chmod` then hits ENOENT. Linux root gives the same 0/5, and a normal Linux user passes 5/5
    (`224915Z…linux-two-tests`). Same class as `process-euid-egid`.
  - `perf-hooks-sliding-window-histogram`: intermittent, about 5% (1/1 suite, 1/10 and 2/40 alone,
    2/48 under load). Linux passes 296/296. `uv_hrtime()` is monotonic on Haiku: 0 backward steps in
    2.4 G reads (`010139Z…hrtime-sliding-window-probe`). The cause is not established; filed as **#997**.
- **22 still fail, unchanged:** the same list as the rev-3 run.
- **Baseline (openssl 3.5.9-2) vs staged (3.5.9-3):** the same failures, apart from the
  intermittent `sliding-window-histogram` and the #946 timeout, plus 1 more skip. The baseline's
  per-test skip list was not kept, so that skip is not attributed.

**Claude Code acceptance, staged build** (claude_code 2.1.314-2 from green; install
`003812Z…final-install`). These are the 4ef344f5fa scripts unchanged, with fresh presigned URLs:

| Check | 4ef344f5fa (24.21.0-2) | **24.22.2-1 + openssl 3.5.9-3** | Evidence (`20261103T…`) |
|---|---|---|---|
| Node acceptance (Intl, zstd, execve, workers, TLS, fs.watch) | 23/23 | **23/23 ALL PASS**: undici 7.30.1, ICU 78.3; fs.watch delivers `rename`/`change` | `010257Z…final-node24-accept` |
| node-gyp addon (`haiku_devel` + `binutils`) | 42 | `add(2,40) = 42` | `010322Z…final-node24-gyp-addon-cxx` |
| `--version`, `doctor`, `cc-teaminfo`, vectors | pass | pass (`haiku-arm64`, `Search: OK`, `check-vectors: ALL PASS`) | `010355Z…final-claude-accept` |
| `claude -p` via Bedrock | `pong`, 7 s | `pong`, rc 0, 9 s; `is_error:false` | `010534Z…final-bedrock-headless-smoke` |
| Headless Read/Grep/Bash | correct | `aurora-7\|src/a.js,src/deep/b.js\|Haiku` | `010604Z…final-bedrock-headless-tools` |
| Grep tool vs rg | 0.469 s / 0.150 s | 0.445 s / rg median 0.148 s, same 3 files, hooks fire | `010634Z…final-bedrock-grep-timing` |
| Interactive TUI: edit, bash, resize, /compact | pass | pass: edit on disk, `bash-ok-Haiku`, both reflows, same /compact behaviour | `010705Z…final-bedrock-tui-session` |
| `/compact` + recall ×5 | 5/5 | **5/5** (12–22 s) | `010853Z…final-compact-haiku-x5` |
| FileChanged through `libuv_fs_event` (fs.watch) | fired 2×, `delivers` | fired 2×, `delivers`; vectors pass | `011154Z…final-uninstall-claude`, `011207Z…final-enduser-fresh-install-3` |
| Ctrl-C, `-p` with no credentials | identical | identical (only the session id differs) | `011416Z…final-tui-ctrlc-and-headless` |
| Worker scenarios | 13/13 | 13/13 | `011500Z…final-compat-scenarios-on-haiku` |
| Bun.spawn / Bun.net vs Bun | 188/24/2/0/4; 35/6/1 | identical: spawn 188 / 24 / 2 / **0** / 4; net 35 / 6 / 1 (Node's Response defaults). Run over SSH, as at 4ef. The same script over SSM gives 1 spawn mismatch, `env spread`, because SSM runs with `HOME` unset; the scenario prints `$HOME` (`compat/test/spawn-diff-step.sh:5`) | `022350Z…final-scen-haiku-node-v2-ssh` (SSM run: `011532Z…final-scen-haiku-node-v2`) |
| TUI snapshots vs Linux | 60/64, 33/33 | 60/64 (same 4 keycap snapshots), 33/33; all 64 snapshots byte-identical to the 4ef344f5fa capture | `011634Z…final-tui-capture-B-haiku`, `012248Z_workstation_node2422-tui-compare` |
| 30-min working soak | 123/123, 4–6 s, RSS 419 → 582 MB | **123/123**, 4–7 s (110 at 4–5 s), one process throughout, RSS 426 → 561 MB | `012215Z…final-working-soak-v3-haiku` |
| 30-min idle soak | 324 → 184 MB | alive, 12 threads, 314 → 187 MB, flat at the end | `015256Z…final-tui-soak-30min` |

**Staged, not published:** `s3://haiku-graviton-<ACCOUNT_ID>-us-west-2/node2422/` holds
nodejs24 and nodejs24_devel 24.22.2-1, and brotli, brotli_devel, brotli_python310 and
brotli_python3.14 1.2.1-2, with `SHA256SUMS`. The set was re-downloaded and verified on the
workstation (`003704Z_workstation…staged-set-check`). It needs openssl3 3.5.9-3 from
`openssl359r3/` in the same publish: the package only requires `lib:libcrypto >= 3`, so pkgman
cannot stop it from being paired with 3.5.9-2.

## 2026-11-03 — final canonical 010361cf (all fixes published)

This is the full verification on one fresh launch of canonical `ami-010361cfdaa9d22e1`
(`haiku-oven-1793718976`, `debeos-r70778-g9b0cf53355`, `canonical=true`; the image has openssl3 3.5.9-3
and `openssh_debeos` 10.4p1-1). Everything came from the live repo, with no local files. The baseline
is the node2422 run in the section above.

Instance `i-010c19cd3ba4fb3ba`: c8g.4xlarge, 60 GiB, `subnet-02f8f7929c0cd9607`,
`sg-0efc97cc673949063`, key `haiku-rdclient`, `ssm-instance-profile`, tags `Project=haiku-graviton`,
`auto-stop=no`, Name `cc-verify-final-010361cf`. It has been terminated
(`20261103T175100Z_workstation_fin-repo-origin-and-ami.log`; the repo is `packages.debene.dev` ->
`debeos-repo-green`). Evidence names below drop the `_i-010c19cd3ba4fb3ba_` part.

**Installed** (`20261103T171411Z…fin-install`).
- **`pkgman install claude_code`**, answers fed on stdin, pulled in:
  - claude_code 2.1.314-2, nodejs24 24.22.2-1, libuv 1.52.1-6, brotli 1.2.1-2, nghttp2 1.63.0-1, icu78 78.3-1, c_ares 1.34.8-1, ripgrep 15.2.0-1, git 2.54.0-1, curl 8.21.0-1.
  - Their own dependencies (expat, libiconv, mandoc, libexecinfo, libssh2, bzip2, libedit, libunistring, gettext_libintl, libpcre2, libpsl, nano).
- **Then, for the tests and node-gyp:**
  - haiku_devel r1~beta6_hrev59996-1, binutils 2.41_2026_03_10_bootstrap-2, gcc 13.3.0_2026_03_29_bootstrap-1, make 4.4.1_bootstrap-1, nodejs24_devel 24.22.2-1.
  - tmux 3.7c-1, python3.14 3.14.7-1, tar 1.35-2, gzip 1.14-1, patch 2.7.6-2.
- **Already on the image:** haiku r1~beta6_hrev59996-1, openssl3 3.5.9-3, openssh_debeos 10.4p1-1.
- **`process.versions`:** node v24.22.2, openssl 3.5.9, brotli 1.2.1, nghttp2 1.63.0, uv 1.52.1, ICU 78.3, undici 7.30.1. 0 `.node` files.

**Bedrock env.** Every model-backed check ran with `CLAUDE_CODE_USE_BEDROCK=1 AWS_REGION=us-west-2
ANTHROPIC_MODEL=us.anthropic.claude-haiku-4-5-20251001-v1:0`, using instance-profile credentials.
- Three node2422 scripts had used `claude-sonnet-5`: headless tools, grep timing and the TUI session. They ran with Haiku 4.5 here. That model change is the only edit, and it is visible in each log's script header.
- With Haiku 4.5 the TUI starts in **manual** permission mode; the node2422 Sonnet session was in auto mode.

| Check | node2422 (4ef344f5fa, staged) | **010361cf (green)** | Evidence (`20261103T…`) |
|---|---|---|---|
| Node acceptance | 23/23 | **23/23 ALL PASS** (undici 7.30.1, ICU 78.3, fs.watch delivers) | `171521Z…fin-node24-accept` |
| node-gyp addon (`haiku_devel` + `binutils`) | 42 | `add(2,40) = 42` | `171550Z…fin-node24-gyp-addon-cxx` |
| `--version`, `doctor`, `cc-teaminfo`, vectors | pass | pass (`haiku-arm64`, `Search: OK`, `check-vectors: ALL PASS`) | `171632Z…fin-claude-accept` |
| `claude -p` via Bedrock | `pong`, 9 s | `pong`, rc 0, 8 s, `is_error:false` | `171825Z…fin-bedrock-headless-smoke` |
| Headless Read/Grep/Bash | correct | correct: `aurora-7\|src/a.js, src/deep/b.js\|Haiku` (Haiku 4.5 puts a space after the comma) | `171856Z…fin-bedrock-headless-tools` |
| Grep tool vs rg | 0.445 s / 0.148 s | 0.386 s / rg median 0.151 s, same 3 files, hooks fire | `171926Z…fin-bedrock-grep-timing` |
| Interactive TUI: edit, bash, resize, /compact | pass | **pass**: edit on disk, `bash-ok-Haiku` (after the manual-mode prompt for `$(uname -s)`), reflow at 70 and 130 columns, and `/compact` completes with correct recall (`"edited on Haiku"`) | `171957Z…fin-bedrock-tui-session` |
| `/compact` + recall ×5 | 5/5 | 5/5 (11–14 s) | `172346Z…fin-compact-haiku-x5` |
| End-user reinstall + FileChanged through `libuv_fs_event` | fired 2×, `delivers` | uninstall, then `pkgman install claude_code`: fired 2×, `delivers`; vectors pass | `172729Z…fin-uninstall-claude`, `172759Z…fin-enduser-fresh-install-3` |
| Ctrl-C, `-p` with no credentials | identical | identical | `173013Z…fin-tui-ctrlc-and-headless` |
| Worker scenarios | 13/13 | 13/13 | `173111Z…fin-compat-scenarios-on-haiku` |
| Bun.spawn / Bun.net vs Bun (SSH) | 188/24/2/0/4; 35/6/1 | identical | `173156Z…fin-scen-haiku-node-v2-ssh` |
| TUI snapshots vs Linux | 60/64, 33/33 | 60/64 (same 4 keycap snapshots), 33/33; all 64 byte-identical to the node2422 capture | `173305Z…fin-tui-capture-B-haiku`, `173903Z_workstation_fin-tui-compare-{linuxA-haikuB,tmux37c}`, `173917Z_workstation_fin-tui-B-vs-node2422-B` |
| 30-min working soak | 123/123 | **run 1: 0/12 FAIL; run 2: 122/122**, 4–7 s, one process, RSS 427 → 548 MB | `175034Z…fin-working-soak-v3-haiku`, `185943Z…fin-working-soak-v3-haiku-run2` |
| 30-min idle soak | 314 → 187 MB | alive, 12 threads, 322 → 186 MB | `182323Z…fin-tui-soak-30min` |

**Working soak run 1 failed, and the cause was the model's tool choice.**
- **What happened.** Every task stopped at a permission prompt, and each prompt took more than the script's 150 s limit to clear. The session transcript (`185859Z…fin-soak-failed-transcript`) shows the model sent each task as one compound Bash command: `echo soak-k-ok > /boot/system/cache/tmp/ws-haiku/soak-k.txt && cat …`.
  - The settings allowlist `Bash(echo:*)` does not cover that, so manual mode asked for confirmation.
  - Each prompt was then answered by the next task's keystrokes. All 6 such commands ran with `RESULT ok` and their files exist.
- **Why it isn't the port.** With the same settings and model, the process, cwd and realpath behave correctly: `process.cwd()`, `realpathSync` and `$PWD` all give `/boot/system/cache/tmp/…` (`185531Z…fin-cwd-permission-probe`). The same task loop, 6 tasks with a debug log, passes 6/6 using `Bash(echo)` + `Write` (`185735Z…fin-soak-prompt-probe`).
- **Run 2,** the unchanged script, passed 122/122. Both runs are kept and reported. The soak harness depends on the model choosing a tool the allowlist covers.

**Node 24 suites** (24.22.2 tree, built from the same recipe tarball and both patchsets as node2422; the patchsets' sha256 matches the copies on this branch: `173918Z…fin-node-src-prepare`, 30 + 4 files, rc 0). Same command, stdin a pipe, full plans (`174044Z…fin-full-suites`):

| Suite | node2422 (staged 24.22.2-1, libuv-5, 4ef344f5fa) | **010361cf (green 24.22.2-1, libuv-6)** |
|---|---|---|
| parallel | plan 1..4605: 4582 ok (333 skip) / 23 not ok (1 timeout) | **4584 ok (332 skip) / 21 not ok (0 timeout)**, 251 s |
| sequential | 1..121: 119 / 2 | 119 / 2, 115 s |

Per-test diff, including skip lists: `20261103T175100Z_workstation_fin-node-suites-vs-node2422.log`.
- **2 went from fail to pass:**
  - `net-connect-abort-controller` (TIMEOUT -> ok): caused by PR 974, merged after the 4ef344f5fa bake, which fixes #946 (a closing listener now resets un-accepted connections). Under `--repeat 48` at 16-way it gives 48/48, against 4/48 hung on 4ef, and 40/40 alone (`174803Z…fin-residual-changed-tests`).
  - `perf-hooks-sliding-window-histogram` (FAIL -> ok): **not a fix.** It is still intermittent here: 3/48 fail under 16-way load and 36/40 pass alone. #997 stays as it is.
- **0 went from pass to fail. No state changes among the rest.**
- **1 fewer skip:** `parallel/test-fs-read-stream` skipped on node2422 with "mkfifo not available" and runs and passes here. `mkfifo` is `/boot/system/bin/mkfifo` from `coreutils-9.9_bootstrap-1` (`174951Z…fin-mkfifo-probe`). Why `spawnSync('mkfifo')` failed on the node2422 box was not recorded, so this is not attributed.
- **23 still fail, unchanged:**
  - class (c) ×9.
  - #882 (no hard links; closed as an accepted, documented BFS limit) ×5.
  - #991 (system nghttp2 1.63.0: `http2-misbehaving-flow-control{,-paused}`, `http2-sensitive-headers`) ×3.
  - uid 0 ×2 (`process-euid-egid`, `fs-rmSync-special-char-additional-error`).
  - one each: #929 (`fs-stat-date`; closed as an accepted BFS timestamp-resolution limit), #871 (`http-writable-true-after-close`), `setproctitle`, `worker-memory`.
- Nothing new was filed: every failure is already tracked or classified, and run 1 of the working soak is not a port defect.

## 2026-11-02 — fresh trunk image 4ef344f5fa

This is the same verification on one fresh launch of the trunk bake `ami-0714f26bbe99a9bfb`
(`haiku-oven-1793631413`, `debeos-r70760-g4ef344f5fa`, haiku-oven-bake execution 8a625ec5; not
canonical, not promoted). The baseline is canonical `ami-03c3545c37f391c1f` (fe7fef0b47, section above).
Over that baseline the image adds PRs 937 (#898), 935 (#919), 931 (#875), 932/942 (#867), 941 (#897),
944 (#918), 945 (#869), 948 (#873), 953 (#899) and 955 (#900).

Instance `i-0b36f8b4076970921`: c8g.4xlarge, 60 GiB, `subnet-0c98257f993bfc1b2`,
`sg-0efc97cc673949063`, key `haiku-rdclient`, `ssm-instance-profile`, tags `Project=haiku-graviton`,
`auto-stop=no`, Name `cc-verify-trunk-4ef344f5fa`. It has been terminated
(`20261102T162553Z_workstation_4ef-repo-origin-and-ami.log`). The packages came from green:
claude_code 2.1.314-2, nodejs24 24.21.0-2 and libuv 1.52.1-4 (`151348Z…4ef-install`). Evidence names
below drop the `_i-0b36f8b4076970921_` part.

| Check | fe7fef0b47 | 4ef344f5fa | Evidence (`20261102T…`) |
|---|---|---|---|
| Node acceptance | 23/23 | 23/23 ALL PASS | `151432Z…4ef-node24-accept` |
| node-gyp addon (`haiku_devel` + `binutils`) | 42 | `add(2,40) = 42` | `151502Z…4ef-node24-gyp-addon-cxx` |
| `--version`, `doctor`, `cc-teaminfo`, vectors | pass | pass. `doctor` no longer prints `Cannot open directory …/ssl/certs/` (PR 945) | `151610Z…4ef-claude-accept` |
| `claude -p` via Bedrock | `pong`, 8 s | `pong`, rc 0, 7 s | `151815Z…4ef-bedrock-headless-smoke` |
| Headless Read/Grep/Bash | correct | `aurora-7\|src/a.js,src/deep/b.js\|Haiku` | `151847Z…4ef-bedrock-headless-tools` |
| Grep tool vs rg | 0.392 s / 0.144 s | 0.469 s / 0.150 s, same 3 files, hooks fire | `151917Z…4ef-bedrock-grep-timing` |
| Interactive TUI: edit, bash, resize, /compact | pass | pass: edit on disk, `bash-ok-Haiku`, both reflows, same /compact behaviour | `152007Z…4ef-bedrock-tui-session` |
| `/compact` + recall ×5 | 5/5 | 5/5 | `152212Z…4ef-compact-haiku-x5` |
| FileChanged through `libuv_fs_event` | fired 2× | fired 2×, `delivers` | `152506Z…4ef-enduser-fresh-install-3` |
| Ctrl-C, `-p` with no credentials | identical | identical; the cert-dir warning is gone (0 vs 2 lines) | `152733Z…4ef-tui-ctrlc-and-headless` |
| Worker scenarios | 13/13 | 13/13 | `152900Z…4ef-compat-scenarios-on-haiku` |
| Bun.spawn / Bun.net vs Bun | 188/24/2/0/4; 35/6/1 | identical | `152944Z…4ef-scen-haiku-node-v2` |
| TUI snapshots vs Linux | 60/64, 33/33 | 60/64 (same 4 keycap snapshots), 33/33. The per-scenario stderr files are now empty (cert-dir warning gone) | `153732Z…4ef-tui-capture-B-haiku`, `154334Z_workstation_4ef-tui-compare-{linuxA-haikuB,tmux37c}` |
| 30-min working soak | 123/123, 4–8 s | 123/123, 4–6 s, one process, RSS 419 → 582 MB | `162519Z…4ef-working-soak-v3-haiku` |
| 30-min idle soak | 324 → 184 MB | alive, 12 threads, 324 → 184 MB | `165609Z…4ef-tui-soak-30min` |

**Node 24 suites, green packages** (`155120Z…4ef-nodejs24r2-full-suites`, same tree and command,
full plans):

| Suite | fe7fef0b47 | 4ef344f5fa |
|---|---|---|
| parallel | 4469 ok / 38 not ok (3 timeout) | **4486 ok (323 skip) / 21 not ok (1 timeout)**, 367 s |
| sequential | 113 ok / 7 not ok (1 timeout) | **118 ok / 2 not ok**, 140 s |

Per-test diff: `20261102T162553Z_workstation_4ef-node-suites-vs-fe7fef0b47.log`.

- **23 went from fail to pass:**
  - #873 ×19, by PR 948 (dual-stack IPv6): `cluster-net-listen-ipv6only-false`, `http-autoselectfamily`, `http2-connect`, `http2-ip-address-host`, `https-autoselectfamily`, `https-connect-address-family`, `net-autoselectfamily`, `net-autoselectfamily-ipv4first`, `net-boundsocket`, `net-connect-options-ipv6`, `net-dns-custom-lookup`, `net-dns-lookup-skip`, `net-pingpong`, `tcp-wrap-listen`, `tls-connect-address-family`, `seq/net-connect-local-error`, `seq/net-server-address`, `seq/net-server-listen-ipv6-link-local`, `seq/tls-psk-client`. This is exactly the list of 19 from the triage.
  - #899 ×2, by PR 953: `dgram-connect-send-empty-{array,buffer}`.
  - #918, by PR 944: `worker-init-failure`.
  - `seq/net-connect-econnrefused`: back to passing. It is the load-dependent test from the fe7fef0b47 run, so it is not attributed to a fix.
- **1 went from pass to fail: `parallel/test-child-process-fork-closed-channel-segfault`** (TIMEOUT).
  - Alone it passes 10/10. Under 16-way load it hangs about 3% of the time (3/48 with `--repeat 16`). The hung primary never gets its worker's `close` (`160041Z…fork-closed-channel-segfault-repro`, `160742Z…fork-closed-channel-hang-state`).
  - Same-box A/B: the test with `listen(0, "0.0.0.0")` also hangs (dual-stack 5/96, IPv4-only 1/96; `161131Z…fork-closed-channel-ab-v4only`, edit confirmed in `162115Z…ab-sedcheck`). So PR 948's dual-stack path is not the trigger.
  - fe7fef0b47 was not tried under this load, so this does not show a regression. Cause not established. Filed as **#978**.
- **22 still fail, unchanged:** class (c) ×9, #882 ×5, nghttp2 packaging ×2, `test-os` (#917, packaging), #929, #871 (`http-writable-true-after-close`), uid 0, `setproctitle`, `worker-memory`.

**Node 24 suites, staged rev-3 packages** (`172820Z…4ef-rev3-nodejs24-full-suites`).
- **Where the packages came from.** `agent-917-full/` was empty at 15:12, 15:49, 15:55 and 16:25Z. The six objects appeared at 17:12Z, and I found them at 17:27Z after the soaks. SHA256SUMS verified on the workstation and again on the instance.
- **Install.** nodejs24 24.21.0-3, nodejs24_devel, libuv 1.52.1-5, libuv_devel and libuv_debuginfo, installed from the files (`172740Z…4ef-rev3-install`).
- **`os.cpus()`.** It now returns 16 entries (`Neoverse-V2`, 2798 MHz), against `[]` with rev 2. Every `times` field is still 0 (open #939).

| Suite | rev 2 (green) | rev 3 (staged) |
|---|---|---|
| parallel | 4486 ok / 21 not ok | **4487 ok / 20 not ok (0 timeout)**, 275 s |
| sequential | 118 / 2 | 118 / 2 |

Diff: `20261102T173732Z_workstation_4ef-rev3-suites-vs-rev2.log`.
- **`parallel/test-os` went from fail to pass:** the patched `GetCPUInfo` in nodejs24 rev 3. This confirms #917.
- **`fork-closed-channel-segfault` passed this time.** That fits the intermittent hang in #978.
- **`parallel/test-child-process-spawnsync-args` failed once** with `spawnSync pwd EPIPE` (`173605Z…4ef-rev3-spawnsync-args-repro`). It then passed 10/10 alone and 32/32 under 16-way load. One failure in 43 runs is not enough to call it a defect or to attribute it, so it is not filed. Incidental observation.

**Per-fix repros on the baked image**, each built from its trunk source (sha256 in the log) and run
the way its merged evidence ran it:

| Issue (PR) | Repro | Expected "after" | 4ef344f5fa | Evidence (`20261102T…`) |
|---|---|---|---|---|
| #898 (937) | `atime898 /boot/home/a898 /tmp/a898` | 0 failures | `cases=136 failures=0 errno-notes=10` | `154408Z…4ef-repro-atime898` |
| #919 (935) | `exit919`, stdout a pipe and a file | 0 failed | `exit919: 0 failed` ×2 | `154437Z…4ef-repro-exit919` |
| #897 (941) | `random897` | 0 failed | `0 failed`: F_SETFL/FIONBIO/O_NONBLOCK pass. FIONREAD and the unknown ioctl still report `General system error`, which the repro prints but doesn't count (the ENOTTY follow-up is not merged) | `154506Z…4ef-repro-random897` |
| #918 (944) | `pipe918 one/full/full2`, `leak 200000` | EMFILE, no leak | EMFILE ×3; 200000 EMFILE, page delta 3 (0 bytes per call) | `154534Z…4ef-repro-pipe918` |
| #873 (948) | `ipv6_873` ×3 | 0 failed | `0 failed` ×3 | `154654Z…4ef-repro-ipv6_873` |
| #899 (953) | `udp899` ×2 | 0 failed | `228 passed, 0 failed` ×2 | `154722Z…4ef-repro-udp899` |
| #900 (955) | `tcp900 <own IPv4>` ×3 | 0 FAIL | `0 FAIL` ×3; syslog matches are the 5 boot banners only (`154914Z…syslog-panic-lines`) | `154830Z…4ef-repro-tcp900` |
| #869 (945) | cert dir + Node CA load | dir exists, no warning | `drwxr-xr-x` 0:0 755; Node 119 system CAs, HTTPS 200, 0 stderr bytes; `doctor` 0 warning lines; `s_client` verify 0 | `154602Z…4ef-repro-869` |
| #875 (931) | `haiku-repo-publish-ephemeral-selftest` + siblings, workstation | PASS | PASS (BSD and GNU base64), coherence and launch selftests PASS, `bash -n` both | `155039Z_workstation_4ef-875-selftests` |
| #867 (932/942) | pipeline jest, `node:24-bookworm` | 149/149 | **149/149**, all 18 `867:` tests pass | `155412Z_workstation_4ef-867-875-jest-full-suite` |

The two jest runs before that log were set-up errors of mine, so I deleted them. Both used an archive
missing `graviton/go-arm64/` and the repo-root `data/system/data/licenses/` that the tests read.

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

## Node test suites

**Current (nodejs24 24.21.0-2 + libuv 1.52.1-4, published 2026-11-01):** parallel 4172 ok
(323 skip) / 335 not ok; sequential 109 ok / 11 not ok (`20261101T093714Z_i-0e7bed4928e0f7bf7_nodejs24r2-full-suites.log`). Against the first run, 78
tests went from failing to passing (62 parallel, 16 sequential: CPU profiler, inspector,
SIGPIPE/stdio-close, process title, thread CPU usage, and fs.watch through libuv) and 0 new
failures, apart from `sequential/test-net-connect-econnrefused`. That one timed out once under the
16-way run and passed 10/10 alone (`20261101T114801Z_i-0e7bed4928e0f7bf7_nodejs24r2-regression-diff.log`, `20261101T114914Z_i-0e7bed4928e0f7bf7_nodejs24r2-econnrefused-x10.log`); rev 2 doesn't touch the connect path, so
it is counted as load-dependent, not a regression. Targeted rerun of the patched paths:
`20261101T093457Z_i-0e7bed4928e0f7bf7_nodejs24r2-targeted-tests.log`. Repo-installed stack verified: `--cpu-prof` writes a profile, process title and
`threadCpuUsage` work, Claude Code `--version`/`doctor`/`-p` pass (`20261101T120352Z_i-0e90d4b253c8134b3_enduser-rev2-install.log`).

First run (24.21.0-1) and its full triage follow.

`tools/test.py -j 16 -p tap parallel sequential` on the builder, stdin a pipe. Under
`/dev/null` stdin every test raised in test.py's per-test `F_SETFL` and was dropped
silently, printing a false "All tests passed" with zero results (#870); the numbers
above are from the run that actually executed them. Failures by area (parallel):
http2 190 (185 timeouts), fs 42, net 37 (28 timeouts), diagnostics 16, tls 13, http 13,
domain 12, process 10, cluster 7, child 7, dgram 6, watch 5, inspector 5, others ≤4;
sequential: cpu-prof 10, watch 4, net 4, others 1 each. The http2/net/tls/http
timeouts are consistent with #871 (accepted sockets never see the peer's EOF) and the
fs/watch failures with `fs.watch` ENOSYS; each failing test is listed in the log.
Triage of the remaining failures (`20261101T004721Z_i-0e7bed4928e0f7bf7_node-failure-triage.log`, first diagnostic line per test):

| Cause | Tests (examples) | Status |
|---|---|---|
| accepted sockets never see the peer's EOF | http2 ×185 timeouts, net/http/tls timeouts | #871 |
| `fs.watch` ENOSYS (libuv) | fs-watch*, watch-mode*, runner-watch | libuv Haiku backend in progress (owner's choice) |
| IPv6 bind without v6-only refused | http/https/tls `*-autoselectfamily`, `*-connect-address-family`, cluster ipv6only | #873 |
| `/usr/bin/env` absent (Haiku has no `/usr`; `#!/usr/bin/env` shebangs still work, measured `20261101T004810Z_i-0e7bed4928e0f7bf7_usr-bin-env-probe.log`) | child-process-default-options, child-process-env | Haiku layout; the bundle only *names* `/usr/bin/env` in its shebang parser |
| `uv_getrusage_thread` ENOTSUP | process-threadCpuUsage*, worker-cpu-usage | libuv lacks a Haiku implementation |
| CPU profiler / inspector exit -21 | v8-cpu-profile, sequential cpu-prof ×10, inspector-has-idle/promises | V8 sampler lacks Haiku arm64 registers; see below |
| abort-on-uncaught / exit-code semantics | domain-no-error-handler-abort-on-uncaught ×10 | #876; see below |
| credentials, umask, priority, proctitle | process-euid-egid, process-umask-mask, os-process-priority, setproctitle | #880, #881, libuv proctitle, uid 0; see below |
| other single failures | dgram multicast, uv_pipe_chmod, stdout-close-catch, … | each classified below |

### Every failing test, root-caused (2026-11-01)

All 423 `not ok` results (397 parallel + 26 sequential) were re-run on the builder
(i-0e7bed4928e0f7bf7, hrev59996, node 24.21.0, libuv 1.52.1-2 installed) with
`: | python3 tools/test.py --shell=/boot/system/bin/node -j N -p tap -v <tests>`, then
each one was reduced to a C or Node repro (Linux control: i-0a14e8aca787249b7, official
node v24.21.0 linux-arm64). Each evidence label resolves to
`evidence/20261101T*_<instance>_<label>.log`. The builder runs as uid 0 (`uid=0(baron)`).
Note: test.py multiplies `--timeout` by 3 on this host.

**How the timeouts were attributed.** All 256 suite timeouts in http2/net/tls/http were re-run
(`me-rerun-t871-timeout20`: 253 timeout again, 3 pass once). Then each was run directly with a
preload that dumps libuv's handles, and the connected TCP handles were matched against `netstat -n`
(`me-timeouts-871-signature`). 253 hung with a libuv handle still open and readable whose kernel
socket was already `time-wait`/`closed`, which is the #871 signature. The 5 that had passed or exited
once hung 10/10 with the same signature (`me-timeouts-racy-and-psk`). `tls-psk-client` is #873.
Of the 46 `fs.watch` failures, 45 pass now on libuv's Haiku fs-event backend (`me-rerun-fswatch`).

| Class | parallel | sequential | total |
|---|---|---|---|
| (a) Haiku defect (kernel, libroot, fs, net stack) | 326 | 7 | 333 |
| (b) Node/libuv/V8 lacks a Haiku code path | 61 | 17 | 78 |
| (c) test assumption that does not hold on Haiku | 7 | 2 | 9 |
| (d) environment / packaging | 3 | 0 | 3 |
| **total** | **397** | **26** | **423** |

Class (a) by issue: #871 268, #873 19, #876 13, #882 5, #883 5, #886 4, #884 2, #879 2, #881 2, #892 2, #877 2, #890 1, #893 1, #889 1, #888 1, #887 1, #878 1, #891 1, #885 1, #880 1.

| # | Class | Tests | Cause | Evidence | Issue / patch |
|---|---|---|---|---|---|
| 255 | a | `diagnostics-channel-http2-client-stream-body-multiple-buffers`, `diagnostics-channel-http2-client-stream-body-multiple-buffers-and-strings`, `diagnostics-channel-http2-client-stream-body-no-chunks`, `diagnostics-channel-http2-client-stream-body-single-buffer`, `diagnostics-channel-http2-client-stream-body-single-string`, `diagnostics-channel-http2-client-stream-close`, `diagnostics-channel-http2-client-stream-created`, `diagnostics-channel-http2-client-stream-finish`, `diagnostics-channel-http2-client-stream-start`, `diagnostics-channel-http2-server-stream-close`, `diagnostics-channel-http2-server-stream-close-error`, `diagnostics-channel-http2-server-stream-created`, `diagnostics-channel-http2-server-stream-created-start-timing`, `diagnostics-channel-http2-server-stream-error`, `diagnostics-channel-http2-server-stream-start`, `diagnostics-channel-http2-server-stream-finish`, `h2-large-header-cause-client-to-hangup`, `http-client-parse-error`, `http-extra-response`, `http-max-http-headers`, `http-multi-line-headers`, `http-no-content-length`, `http-response-no-headers`, `http-response-status-message`, `http-should-keep-alive`, `http-socket-encoding-error`, `http-upgrade-advertise`, `http-upgrade-server`, `http2-altsvc`, `http2-async-local-storage`, `http2-autoselect-protocol`, `http2-buffersize`, `http2-byteswritten-server`, `http2-capture-rejection`, `http2-clean-output`, `http2-client-data-end`, `http2-client-destroy`, `http2-client-onconnect-errors`, `http2-client-priority-before-connect`, `http2-client-promisify-connect`, `http2-client-proxy-over-http2`, `http2-client-request-listeners-warning`, `http2-client-request-options-errors`, `http2-client-rststream-before-connect`, `http2-client-set-priority`, `http2-client-setLocalWindowSize`, `http2-client-setNextStreamID-errors`, `http2-client-settings-before-connect`, `http2-client-stream-destroy-before-connect`, `http2-client-unescaped-path`, `http2-client-upload`, `http2-client-upload-reject`, `http2-client-write-before-connect`, `http2-client-write-empty-string`, `http2-compat-client-upload-reject`, `http2-compat-errors`, `http2-compat-expect-continue`, `http2-compat-expect-continue-check`, `http2-compat-expect-handling`, `http2-compat-method-connect`, `http2-compat-serverrequest`, `http2-compat-serverrequest-end`, `http2-compat-serverrequest-headers`, `http2-compat-serverrequest-host`, `http2-compat-serverrequest-pause`, `http2-compat-serverrequest-pipe`, `http2-compat-serverrequest-settimeout`, `http2-compat-serverrequest-trailers`, `http2-compat-serverresponse`, `http2-compat-serverresponse-createpushresponse`, `http2-compat-serverresponse-destroy`, `http2-compat-serverresponse-drain`, `http2-compat-serverresponse-end`, `http2-compat-serverresponse-end-after-statuses-without-body`, `http2-compat-serverresponse-finished`, `http2-compat-serverresponse-flushheaders`, `http2-compat-serverresponse-headers`, `http2-compat-serverresponse-headers-after-destroy`, `http2-compat-serverresponse-headers-send-date`, `http2-compat-serverresponse-settimeout`, `http2-compat-serverresponse-statuscode`, `http2-compat-serverresponse-statusmessage`, `http2-compat-serverresponse-statusmessage-property`, `http2-compat-serverresponse-statusmessage-property-set`, `http2-compat-serverresponse-trailers`, `http2-compat-serverresponse-write`, `http2-compat-serverresponse-writehead`, `http2-compat-serverresponse-writehead-array`, `http2-compat-short-stream-client-server`, `http2-compat-socket`, `http2-compat-socket-destroy-delayed`, `http2-compat-socket-set`, `http2-compat-write-early-hints`, `http2-compat-write-early-hints-invalid-header`, `http2-compat-write-head-after-close`, `http2-compat-write-head-destroyed`, `http2-compat-write-information`, `http2-connect-method`, `http2-connect-method-extended`, `http2-connect-method-extended-cant-turn-off`, `http2-cookies`, `http2-create-client-secure-session`, `http2-create-client-session`, `http2-createwritereq`, `http2-date-header`, `http2-debug`, `http2-destroy-after-write`, `http2-dont-override`, `http2-empty-frame-without-eof`, `http2-endafterheaders`, `http2-exceeds-server-trailer-size`, `http2-forget-closed-streams`, `http2-goaway-opaquedata`, `http2-graceful-close`, `http2-head-request`, `http2-info-headers`, `http2-info-headers-errors`, `http2-invalidargtypes-errors`, `http2-invalidheaderfield`, `http2-invalidheaderfields-client`, `http2-large-write-multiple-requests`, `http2-max-concurrent-streams`, `http2-max-session-memory-leak`, `http2-max-session-memory-stalled-headers`, `http2-methods`, `http2-misused-pseudoheaders`, `http2-multi-content-length`, `http2-multiheaders`, `http2-multiheaders-raw`, `http2-multiplex`, `http2-no-more-streams`, `http2-no-wanttrailers-listener`, `http2-onping`, `http2-options-max-headers-block-length`, `http2-options-max-reserved-streams`, `http2-options-server-request`, `http2-options-server-response`, `http2-origin`, `http2-pack-end-stream-flag`, `http2-perf_hooks`, `http2-ping`, `http2-pipe`, `http2-priority-cycle-`, `http2-priority-event`, `http2-raw-headers`, `http2-raw-headers-defaults`, `http2-removed-header-stays-removed`, `http2-request-remove-connect-listener`, `http2-res-corked`, `http2-res-writable-properties`, `http2-respond-errors`, `http2-respond-file`, `http2-respond-file-204`, `http2-respond-file-304`, `http2-respond-file-404`, `http2-respond-file-compat`, `http2-respond-file-error-dir`, `http2-respond-file-error-pipe-offset`, `http2-respond-file-errors`, `http2-respond-file-fd`, `http2-respond-file-fd-errors`, `http2-respond-file-fd-invalid`, `http2-respond-file-fd-leak`, `http2-respond-file-fd-range`, `http2-respond-file-filehandle`, `http2-respond-file-push`, `http2-respond-file-range`, `http2-respond-file-with-pipe`, `http2-respond-nghttperrors`, `http2-respond-no-data`, `http2-respond-with-fd-errors`, `http2-response-splitting`, `http2-sent-headers`, `http2-server-close-callback`, `http2-server-close-idle-connection`, `http2-server-errors`, `http2-server-push-disabled`, `http2-server-push-stream`, `http2-server-push-stream-errors`, `http2-server-push-stream-errors-args`, `http2-server-push-stream-head`, `http2-server-rst-before-respond`, `http2-server-rst-stream`, `http2-server-session-destroy`, `http2-server-set-header`, `http2-server-setLocalWindowSize`, `http2-server-shutdown-before-respond`, `http2-server-startup`, `http2-server-timeout`, `http2-session-graceful-close`, `http2-session-reuse-different-authority`, `http2-session-settings`, `http2-session-stream-state`, `http2-session-timeout`, `http2-settings-unsolicited-ack`, `http2-short-stream-client-server`, `http2-single-headers-validation-disabled`, `http2-socket-proxy-destroy`, `http2-status-code`, `http2-status-code-invalid`, `http2-stream-client`, `http2-stream-destroy-event-order`, `http2-timeouts`, `http2-too-large-headers`, `http2-too-many-headers`, `http2-too-many-streams`, `http2-trailers`, `http2-trailers-after-session-close`, `http2-unbound-socket-proxy`, `http2-window-size`, `http2-write-callbacks`, `http2-write-empty-string`, `http2-zero-length-write`, `https-eof-for-eom`, `net-after-close`, `net-allow-half-open`, `net-autoselectfamily-default`, `net-bytes-written-large`, `net-connect-options-allowhalfopen`, `net-connect-options-port`, `net-dns-lookup`, `net-keepalive`, `net-listen-after-destroying-stdin`, `net-normalize-args`, `net-onread-static-buffer`, `net-pause-resume-connecting`, `net-persistent-keepalive`, `net-persistent-nodelay`, `net-persistent-ref-unref`, `net-server-max-connections`, `net-server-pause-on-connect`, `net-socket-byteswritten`, `net-socket-close-after-end`, `net-socket-connect-without-cb`, `net-socket-connecting`, `net-socket-local-address`, `net-socket-ready-without-cb`, `net-socket-tos`, `net-throttle`, `net-writable`, `net-write-after-end-nt`, `net-write-slow`, `tls-client-cert-resumption`, `tls-client-mindhsize`, `tls-env-extra-ca`, `tls-env-extra-ca-with-options`, `tls-inception`, `tls-max-send-fragment`, `tls-on-empty-socket`, `tls-session-reuse-different-servername`, `tls-wrap-econnreset`, `tls-wrap-econnreset-localaddress`, `tls-wrap-econnreset-socket`, `seq/http2-large-file`, `seq/net-localport` | #871: still hung at 16 s with a connected tcp handle libuv holds open+readable whose kernel socket is already time-wait/closed (the FIN was never delivered) | me-rerun-t871-timeout20, me-timeouts-871-signature, me-timeouts-racy-and-psk | #871 |
| 15 | a | `cluster-net-listen-ipv6only-false`, `http-autoselectfamily`, `http2-connect`, `http2-ip-address-host`, `https-autoselectfamily`, `https-connect-address-family`, `net-autoselectfamily`, `net-autoselectfamily-ipv4first`, `net-boundsocket`, `net-connect-options-ipv6`, `net-dns-custom-lookup`, `net-dns-lookup-skip`, `net-pingpong`, `tls-connect-address-family`, `seq/net-server-listen-ipv6-link-local` | #873: `::`/`::1` bind or listen without v6-only refused (EINVAL); default `listen(0)` falls back to 0.0.0.0 so `[::1]` connects are refused | me-rerun-ipv6-env-rusage, me-http2-ip-address-host-ipv6 | #873 |
| 10 | a | `child-process-disconnect`, `child-process-fork-net`, `child-process-fork-net-socket`, `child-process-send-keep-open`, `cluster-disconnect`, `cluster-shared-leak`, `process-getactivehandles`, `process-getactiveresources-track-active-handles`, `set-http-max-http-headers`, `timers-socket-timeout-removes-other-socket-unref-timer` | #871: accepted sockets in kernel time-wait/closed while libuv still holds an active readable handle (handle dump) | f4-timeout-handle-dump, f4-rerun-all-2 | #871 |
| 10 | a | `domain-no-error-handler-abort-on-uncaught-1`, `domain-no-error-handler-abort-on-uncaught-0`, `domain-no-error-handler-abort-on-uncaught-2`, `domain-no-error-handler-abort-on-uncaught-3`, `domain-no-error-handler-abort-on-uncaught-4`, `domain-no-error-handler-abort-on-uncaught-5`, `domain-no-error-handler-abort-on-uncaught-6`, `domain-no-error-handler-abort-on-uncaught-7`, `domain-no-error-handler-abort-on-uncaught-8`, `domain-no-error-handler-abort-on-uncaught-9` | `abort()` calls `debugger()` (`exit.cpp:287-299`); debug_server kills the team (`DebugServer.cpp:167` -> `team.cpp:3864`), so the wait status is SIGKILLTHR (21), not SIGABRT; Node has no name for 21 and reports `exit(0, null)` | f2-abort-probe, f2-abort-waitstatus, f2-abort-domain-child, f2-patched-node-tests | #876 (+ patchset 3/3 names the signal) |
| 5 | a | `fs-readfile-eof`, `fs-readfile-pipe`, `fs-readfile-pipe-large`, `fs-readfilesync-pipe-large`, `fs-realpath-pipe` | devfs has no `/dev/stdin` (nor /dev/stdout, /dev/stderr, /dev/fd); `echo hi \| cat /dev/stdin` fails too | f3-probe-devstdin-link | #883 |
| 4 | a | `dgram-connect-send-empty-buffer`, `dgram-connect-send-empty-array`, `dgram-send-empty-array`, `dgram-send-empty-buffer` | `sendmsg` with iov {NULL,0} returns EFAULT: `socket.cpp:199` calls `get_iovecs_from_user` without permitNull (the 3 timeouts wait for a datagram never sent) | f4-c-syscall-probe, f4-dgram-node-probes, f4-timeout-handle-dump | #886 |
| 3 | a | `tcp-wrap-listen`, `seq/net-connect-local-error`, `seq/net-server-address` | #873: dual-stack `::` bind refused (EINVAL); listen() falls back to 0.0.0.0; local bind to ::1 EINVAL | f4-ipv6-v6only-probe, f4-linux-ipv6-v6only-probe | #873 |
| 2 | a | `cluster-listen-pipe-readable-writable`, `net-server-listen-path` | `fchmod` on a socket returns B_UNSUPPORTED (`vfs.cpp:9943-9947`, `socket.cpp:363`) so libuv `uv_pipe_chmod` (pipe.c:475) never falls back to chmod | f4-c-syscall-probe, f4-linux-control-c-probes, f4-rerun-all-2 | #884 |
| 2 | a | `domain-throw-error-then-throw-from-uncaught-exception-handler`, `domain-with-abort-on-uncaught-exception` | same: abort reported as signal 21 | f2-child-signal-probes, f2-patched-node-tests | #876 |
| 2 | a | `fs-link`, `permission-fs-symlink-target-write` | no hard links: `bfs_link` returns B_UNSUPPORTED (`kernel_interface.cpp:1119`) and libroot `link.c:91` maps it to EPERM | f3-probe-devstdin-link, f3-link-sites-in-tests, f3-rerun | #882 |
| 2 | a | `fs-promises`, `util-promisify` | BFS reports atime = now (`bfs/kernel_interface.cpp:48-52`) and drops atime in utimes | f2-rlimit-cwd-atime-dlerror | #879 |
| 2 | a | `os`, `os-process-priority` | libroot `priority.c` nice <-> BeOS priority mapping does not round-trip (set 1 -> get 0, set 10 -> get 9) | f3-probe-umask-priority-title | #881 |
| 2 | a | `trace-events-fs-async`, `trace-events-fs-sync` | no hard links: `bfs_link` returns B_UNSUPPORTED (`kernel_interface.cpp:1119`) and libroot `link.c:91` maps it to EPERM; a copy without the hard-link case passes | f3-trace-events-without-hardlinks | #882 |
| 2 | a | `worker-init-failure`, `seq/child-process-emfile` | `vfs_setrlimit` refuses to lower the NOFILE hard limit; `ulimit -n N` gets EPERM even as root | f2-rlimit-cwd-atime-dlerror | #877 |
| 1 | a | `cluster-concurrent-disconnect` | accepted AF_UNIX end gets ENOTCONN, not EPIPE: `_Spawn` (`UnixStreamEndpoint.cpp:731`) never sets fWasConnected (:767) | f4-unix-pair-and-libpath | #890 |
| 1 | a | `cluster-worker-kill-signal` | SIGCHLD is sent and the child reapable (`thread.cpp:2284-2291`) before its fds close (`~Team`, `team.cpp:518`), so `exit` precedes `disconnect` (50/50 C, 20/20 Node) | f2-sigchld-before-fd-close, f2-cluster-exit-order | #893 |
| 1 | a | `crypto-no-algorithm` | same: child aborts at `node.cc:1266` (CSPRNG) and dies by 21 | f2-child-signal-probes | #876 |
| 1 | a | `dgram-error-message-address` | IPv6 bind to a non-local address returns B_ERROR (`ipv6.cpp:1231`) -> "Unknown system error -1" | f4-c-syscall-probe | #889 |
| 1 | a | `dgram-multicast-set-interface` | IPV6_MULTICAST_IF takes an in6_addr instead of the RFC 3493 interface index (`ipv6.cpp:1137-1139`) | f4-misc-probes, f4-linux-control-c-probes | #888 |
| 1 | a | `cluster-dgram-1` | `sendto` 0.0.0.0 returns EDESTADDRREQ (`ipv4.cpp:1595`); all 40 sends fail | f4-misc-probes, f4-dgram-node-probes | #887 |
| 1 | a | `debugger-unavailable-port` | #871: the blocker server's accepted sockets never see the FIN, so `close()` never completes (`node inspect` itself behaves correctly) | f1-debugger-port-probe-fetch, f1-inspector-ws-and-debugger-port-hang | #871 |
| 1 | a | `fs-error-messages` | no hard links: `bfs_link` returns B_UNSUPPORTED (`kernel_interface.cpp:1119`) and libroot `link.c:91` maps it to EPERM; also EPERM instead of EEXIST for an existing name (`vfs.cpp:6706`) | f3-probe-devstdin-link, f3-hardlink-only-cause | #882 |
| 1 | a | `fs-write-sigxfsz` | no `RLIMIT_FSIZE` (`thread.cpp` common_getrlimit/setrlimit -> EINVAL) | f2-rlimit-cwd-atime-dlerror | #878 |
| 1 | a | `http-writable-true-after-close` | #871: the accepted socket sees the client FIN only after the server writes (Linux before), so both finish and close fire; Haiku 10/10 fail, Linux 10/10 pass | f4-http-writable-events2, f4-linux-http-writable-events | #871 |
| 1 | a | `http2-max-invalid-frames` | data after the peer closed is not answered with RST until the window fills (~9.4 s; `TCPEndpoint.cpp:2809-2824`) | f4-tcp-rst-after-close, f4-nghttp2-170-ab | #891 |
| 1 | a | `inspector-network-websocket` | #871: accepted WebSocket socket stays `readableEnded false`, `server.close()` never completes | f1-inspector-ws-and-debugger-port-hang | #871 |
| 1 | a | `net-pipe-connect-errors` | AF_UNIX connect to a non-socket path returns EINVAL (`UnixStreamEndpoint.cpp:262-263`); Linux ECONNREFUSED | f4-c-syscall-probe, f4-linux-control-c-probes | #885 |
| 1 | a | `process-umask-mask` | libroot `umask.c:23` keeps bits outside 0777: `umask(010664)` reads back 010664 | f3-probe-umask-priority-title | #880 |
| 1 | a | `runner-worker-id` | concurrent `appendFileSync` loses lines (O_APPEND not atomic on BFS); 17/48 runs under load; passes 10/10 at -j 1 | f2-runner-worker-id-usagefile, f2-runner-worker-id-load | #892 |
| 1 | a | `tls-enable-keylog-cli` | BFS O_APPEND not atomic: `bfs_write` reads `inode->Size()` unlocked (`kernel_interface.cpp:1443`), locks later (`Inode.cpp:1608`); keylog loses a line 3/10; C repro loses 698/2000 appends | f3-probe-keylog-append2, f3-keylog-events-vs-lines | #892 |
| 1 | a | `seq/tls-psk-client` | #873: the test passes `-4` to `openssl s_server` only when `!common.hasIPv6`; s_server then listens on `[::]` only (dual-stack refused), and `net.connect(PORT)` to localhost -> 127.0.0.1 is refused until the 5 s timeout; with `-4` it connects | me-timeouts-871-signature, me-tls-psk-client-probe, me-tls-psk-client-v6only | #873 |
| 45 | b | `fs-options-immutable`, `fs-promises-watch`, `fs-promises-watch-ignore-function`, `fs-promises-watch-ignore-glob`, `fs-promises-watch-ignore-mixed`, `fs-promises-watch-ignore-regexp`, `fs-promises-watch-iterator`, `fs-watch-close-when-destroyed`, `fs-watch-abort-signal`, `fs-watch-encoding`, `fs-watch-enoent`, `fs-watch-ignore-function`, `fs-watch-ignore-glob`, `fs-watch-ignore-recursive-glob`, `fs-watch-ignore-mixed`, `fs-watch-ignore-recursive-glob-subdirectories`, `fs-watch-ignore-recursive-mixed`, `fs-watch-ignore-regexp`, `fs-watch-ignore-recursive-regexp`, `fs-watch-recursive-add-file-with-url`, `fs-watch-recursive-add-folder`, `fs-watch-recursive-add-file`, `fs-watch-recursive-add-file-to-existing-subfolder`, `fs-watch-recursive-add-file-to-new-folder`, `fs-watch-recursive-delete-race`, `fs-watch-recursive-assert-leaks`, `fs-watch-recursive-symlink`, `fs-watch-recursive-delete`, `fs-watch-recursive-update-file`, `fs-watch-ref-unref`, `fs-watch-recursive-sync-write`, `fs-watch-recursive-promise`, `fs-watch-recursive-watch-file`, `runner-watch-mode-complex`, `runner-complex-dependencies`, `runner-global-setup-watch-mode`, `watch-file-shared-dependency`, `watch-mode-files_watcher-clear`, `watch-mode-files_watcher`, `watch-mode-kill-signal-default`, `watch-mode-kill-signal-override`, `seq/watch-mode`, `seq/watch-mode-inspect`, `seq/watch-mode-restart-esm-loading-error`, `seq/watch-mode-worker` | `fs.watch` ENOSYS at run time: libuv had no Haiku fs-event backend; passes now with libuv 1.52.1-2 (Haiku backend) | me-rerun-fswatch | fixed (libuv backend, published) |
| 19 | b | `debugger-profile`, `debugger-profile-command`, `inspector-has-idle`, `inspector-promises`, `inspector-stop-profile-after-done`, `tick-processor-arguments`, `v8-cpu-profile`, `seq/cpu-prof-default`, `seq/cpu-prof-dir-absolute`, `seq/cpu-prof-dir-and-name`, `seq/cpu-prof-dir-relative`, `seq/cpu-prof-dir-worker`, `seq/cpu-prof-drained`, `seq/cpu-prof-exit`, `seq/cpu-prof-kill`, `seq/cpu-prof-name`, `seq/cpu-prof-worker-argv`, `seq/diagnostic-dir-cpu-prof`, `seq/worker-prof` | V8 `SignalHandler::FillRegisterState` (`deps/v8/src/libsampler/sampler.cc:524-533`) has Haiku ia32/x64 branches only; on arm64 pc/sp/fp are NULL, the SIGPROF stack walk segfaults; the crash is reported as signal 21 (#876) | f1-cpu-prof-probe, f1-cpu-prof-strace, f1-baseline-system-node, f1-patched-node-tests | patch `nodejs-24.21.0-debeos.patchset` (1/3); passes after rebuild |
| 4 | b | `cli-node-print-help`, `process-external-stdio-close-spawn`, `process-external-stdio-close`, `stdout-close-catch` | HaikuPorts patchset comments out both `sigaction` CHECKs in `ResetSignalHandlers` (`src/node.cc` ~512-519), so SIGPIPE is never ignored and node dies by SIGPIPE (rc 135); the kernel gives EPIPE under SIG_IGN | f2-sigpipe-probe, f2-sigpipe-c-and-nodecc, f2-sigaction-loop, f2-patched-node-tests | patch `nodejs-24.21.0-debeos.patchset` (2/3); passes after rebuild |
| 3 | b | `process-threadCpuUsage-main-thread`, `process-threadCpuUsage-worker-threads`, `worker-cpu-usage` | libuv `uv_getrusage_thread` (`src/unix/core.c:1144-1179`) handles Apple/RUSAGE_LWP/RUSAGE_THREAD only and returns UV_ENOTSUP on Haiku | me-uv-getrusage-thread-source, me-libuv-getrusage-thread-patch-test (3/3 pass) | patch `libuv-1.52.1-haiku-getrusage-thread.patch` |
| 2 | b | `process-title-cli`, `seq/process-title` | libuv builds `src/unix/no-proctitle.c` on Haiku (CMakeLists.txt:420, Makefile.am:473): `uv_get_process_title` is always "" | f3-probe-umask-priority-title, f3-libuv-proctitle-patch-test (passes) | patch `libuv-1.52.1-haiku-proctitle.patch` |
| 1 | b | `inspector-contexts` | libuv builds `src/unix/no-proctitle.c` on Haiku (CMakeLists.txt:420, Makefile.am:473): `uv_get_process_title` is always ""; context name is title+`[pid]` | f3-libuv-proctitle-patch-test (passes) | patch `libuv-1.52.1-haiku-proctitle.patch` |
| 1 | b | `setproctitle` | libuv builds `src/unix/no-proctitle.c` on Haiku (CMakeLists.txt:420, Makefile.am:473): `uv_get_process_title` is always ""; with the patch it then fails at test line 41 `ps -p PID -o args=` (Haiku `ps` has no -p and shows the kernel command line) - a test assumption (c) remains | f3-libuv-proctitle-patch-test, f3-libuv-proctitle-clean-patch | patch `libuv-1.52.1-haiku-proctitle.patch` (then c) |
| 1 | b | `trace-events-metadata` | libuv builds `src/unix/no-proctitle.c` on Haiku (CMakeLists.txt:420, Makefile.am:473): `uv_get_process_title` is always ""; no `process_name` event for the new title | f3-probe-metadata-keylog, f3-libuv-proctitle-patch-test (passes) | patch `libuv-1.52.1-haiku-proctitle.patch` |
| 1 | b | `worker-node-options` | libuv builds `src/unix/no-proctitle.c` on Haiku (CMakeLists.txt:420, Makefile.am:473): `uv_get_process_title` is always ""; fixture asserts `process.title === 'foo'` | f3-libuv-proctitle-patch-test (passes) | patch `libuv-1.52.1-haiku-proctitle.patch` |
| 1 | b | `worker-memory` | `uv_resident_set_memory` (`src/unix/haiku.c:92-108`) sums `area.ram_size`, excluding file-backed text; start RSS 11.6 MiB vs Linux 42 MiB inflates the growth ratio (11x vs 3.5x); anonymous growth matches Linux | f2-worker-memory-areas, f2-rss-text-areas, f2-worker-memory-linux-node24 | proposed: count mapped text in RSS; no small fix |
| 2 | c | `child-process-default-options`, `child-process-env` | spawns `/usr/bin/env`; Haiku has no `/usr` (`#!/usr/bin/env` shebangs still work) | me-rerun-ipv6-env-rusage, 20261101T004810Z usr-bin-env-probe | - |
| 2 | c | `process-dlopen-error-message-crash`, `seq/module-loading` | expect `dlerror()` to name the file; Haiku returns only strerror (`dlfcn.c:68-74`); the tests already exempt AIX/IBMi/OpenBSD for this | f2-rlimit-cwd-atime-dlerror | - |
| 1 | c | `child-process-exec-env` | line 23 `exec('/usr/bin/env', ...)`; Haiku has no `/usr`, sh exits 127 | f2-rlimit-cwd-atime-dlerror | - |
| 1 | c | `cwd-enoent-improved-message` | Haiku `getcwd` returns the stale path after rmdir (POSIX does not require ENOENT); skip list at line 9 covers SunOS/Windows/AIX/IBMi only | f2-rlimit-cwd-atime-dlerror | - |
| 1 | c | `repl-underscore` | line 169 `/^ {2}errno: -(2\|4058),$/` hardcodes the Linux/Windows ENOENT | f2-rerun | - |
| 1 | c | `util-exception-with-host-port` | line 6 `const ENOENT = -os.constants.errno.ENOENT;` assumes positive errno; Haiku errno is negative (-2147459069) | f2-rlimit-cwd-atime-dlerror | - |
| 1 | c | `seq/fs-watch` | lines 38-41 `expectFilePath = isWindows \|\| isLinux \|\| isMacOS \|\| isAIX`; Haiku's backend reports the filename, so line 108 `assert.strictEqual(filename, null)` fails | me-rerun-fswatch, me-seq-fs-watch-source | - |
| 2 | d | `http2-misbehaving-flow-control`, `http2-misbehaving-flow-control-paused` | `--shared-nghttp2` links the system nghttp2 1.63.0; Node 24.21 bundles 1.70.0 and passes with it loaded via LIBRARY_PATH | f4-nghttp2-170-ab, f4-unix-pair-and-libpath | packaging: nghttp2 recipe >= 1.70 |
| 1 | d | `process-euid-egid` | builder runs as uid 0 so the superuser branch runs (test lines 57-65); Haiku has no `nobody`/`nogroup` accounts -> ERR_UNKNOWN_CREDENTIAL | f3-probe-umask-priority-title | - |

Rows sum to 423 = 397 parallel + 26 sequential.

**Issues filed in this triage** (each searched open and closed first; one per defect):
#876 abort/crash reported as SIGKILLTHR · #877 NOFILE hard limit cannot be lowered · #878 no
RLIMIT_FSIZE · #879 BFS atime · #880 umask extra bits · #881 getpriority round trip · #882 no hard
links / EPERM for EEXIST · #883 no /dev/stdin, /dev/fd · #884 fchmod on a socket B_UNSUPPORTED ·
#885 AF_UNIX connect to a non-socket EINVAL · #886 zero-length sendmsg EFAULT · #887 UDP sendto
0.0.0.0 · #888 IPV6_MULTICAST_IF takes an address · #889 IPv6 bind non-local B_ERROR · #890 AF_UNIX
ENOTCONN for EPIPE · #891 data after close() not reset · #892 BFS O_APPEND not atomic · #893 SIGCHLD
before the fds close. New evidence was commented on #871 and #873.

**Patches (class b)** are in `patches/` here. None is wired into a recipe or published.
- `nodejs-24.21.0-debeos.patchset` applies on top of the HaikuPorts `nodejs-24.21.0.patchset`
  (dry run: `me-node-patches-apply-check`). It holds 3 patches:
  1. V8 `sampler.cc`: fill pc/sp/fp/lr from Haiku's arm64 `struct vregs`. Rebuilt node: all 19
     profiler tests pass (`f1-patched-node-tests`).
  2. `node.cc` `ResetSignalHandlers`: restore the sigaction CHECKs and skip SIGKILLTHR, so SIGPIPE
     is ignored again. The 4 SIGPIPE tests pass and 10 signal regression tests still pass
     (`f2-patched-node-tests`).
  3. `src/api/utils.cc`: name SIGKILLTHR, so a child killed by it is no longer reported as
     `exit(0, null)`.
- `libuv-1.52.1-haiku-proctitle.patch` builds `proctitle.c` and implements
  `uv__set_process_title` with `rename_thread`. 5 of the 6 title tests pass against the patched
  lib; libuv's own process_title tests pass (`f3-libuv-proctitle-patch-test`,
  `f3-libuv-proctitle-clean-patch`).
- `libuv-1.52.1-haiku-getrusage-thread.patch` implements `uv_getrusage_thread` with
  `get_thread_info`. All 3 threadCpuUsage/worker-cpu-usage tests pass against the patched lib
  (`me-libuv-getrusage-thread-patch-test`).
- Not patched: `worker-memory` (libuv `uv_resident_set_memory` leaves out file-backed text) and the
  nghttp2 pair (packaging: the system nghttp2 1.63.0 vs the bundled 1.70.0).

Builder state left behind: F1's incremental rebuild in the haikuporter work tree
(`deps/v8/src/libsampler/sampler.cc` patched, `out/Release/node` rebuilt; originals saved in
`/tmp/f1-sampler.cc.orig`, `/tmp/f1-node.orig`), a `/sources` symlink, `/boot/home/f2-node`
(scratch tree), and leftover hung `node` test processes from the suite runs. The published
packages were not touched.

## Claude Code on Haiku

- `claude_code-2.1.314-1-arm64.hpkg` is built by haikuporter on Haiku from the pinned
  npm sources; every rule of the deterministic pass reports its asserted count in the
  build log (R1–R4, P4 116+10, P5–P17).
- Installed system-wide; the launcher refuses a non-system install (R1 paths are absolute).
- `doctor` prints OpenSSL's missing default cert dir on stderr (#869); TLS works.

## Open items (blocked or owner decisions)

1. Model-backed tests ran through Amazon Bedrock with instance-profile credentials (owner's
   suggestion), so no API key was needed. The acceptance box's `ssm-instance-profile`
   carries `PowerUserAccess`, which is broader than this needs; a dedicated
   SSM+Bedrock profile (`cc-bedrock-ssm`) is pending the owner.
2. `fs.watch` on Haiku: libuv backend (system library change) or polling (compat).
3. **Current published revisions:** claude_code 2.1.314-2 (requires `libuv_fs_event`; R5 drops
   the Linux ELF addons); nodejs24 24.21.0-2 (+_devel; DeBeOS patchset: V8 sampler on Haiku
   arm64, SIGPIPE/SIGKILLTHR); libuv 1.52.1-4 (+_devel, _debuginfo; fs-event backend, process
   title, per-thread rusage; provides `libuv_fs_event`).
   **First publish on 2026-11-01 (owner's approval):** nodejs24 24.21.0-1 + _devel,
   claude_code 2.1.314-1, libuv 1.52.1-2 + _devel (the Haiku fs-event backend). Green went
   3289 → 3292 objects (libuv 1.52.1-1 replaced); `pkgman search` on a Haiku box resolves all
   five from the DeBeOS repo; prod and beta indexes invalidated. libuv_debuginfo 1.52.1-2
   followed in a second publish (3292 objects, 1.52.1-1 replaced); a Haiku box resolves
   libuv, libuv_devel and libuv_debuginfo all at 1.52.1-2 from the DeBeOS repo. Earlier note, kept for the record: publishing was held until approval. claude_code redistributes
   Anthropic's proprietary bundle under its own terms (`LICENSE`: Anthropic Legal
   Agreements) — whether DeBeOS may publish it is a licensing decision for the owner.
4. Defects filed during this work: #867 (wave builder S3 client), #868 (bash /dev/fd),
   #869 (OpenSSL cert dir), #870 (devfs F_SETFL), #876–#893 (Node test triage, above), plus new
   evidence on #843, #871, #873.
