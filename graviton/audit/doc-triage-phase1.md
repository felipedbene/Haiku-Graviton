# graviton/docs — Phase-1 triage manifest (classify only)

> NOTE (Phase-2 pending): two buckets below may be adjusted by the Phase-2 run —
> `device-watchdog-and-bfs-crashsafety.md` (expected to settle as RESOLVED, source-verified)
> and `sequencing.md` (expected to be archived pending verification). They are recorded here
> with their Phase-1 verdicts; Phase 2 confirms or amends them.

> STATUS: Phase 1 of the docs-triage wave — classification only, no doc moved or edited.
> Generated 2026-10-04 against branch instrument/548-drawstring-burst. Issue/PR state from
> `gh`; the 28 docs citing no issue were checked against the fixing commit + live code.
> Phase 2 (human-approved) moves RESOLVED/SUPERSEDED into graviton/docs/archive/ per the
> risk register at the end of this file.

## Totals (61 docs)

| bucket | count |
|---|---|
| LIVING | 27 |
| RESOLVED | 28 |
| SUPERSEDED | 5 |
| UNSURE | 1 |

Referenced-nowhere (measured, worktrees excluded): 7 docs — arm-mcp-server-evaluation,
arm64-anyboot-mbr, chromium-arm64-scope, debeos-identity, lean-ami-builder-gaps,
platform-portability, playbook-parity-hook. (The spec estimated 15; net-checksum and
mimeset-headless, initially suspected nowhere, are in fact cited — net-checksum without the
.md suffix, mimeset-headless via the haiku-drift-check glob.) Start Phase 2 with these 7.

Legend: `[SOLE]` = only in-tree record of why current code/workaround is the way it is.

## Networking / ENA / TCP / perf (16)

| file | bucket | evidence | inbound refs |
|---|---|---|---|
| net-checksum.md | RESOLVED `[SOLE]` | 63e1ca7b15, 62315721f1; M1/M2 methodology | chromium-arm64-scope, ena-tx-offload |
| ena-tx-offload.md | RESOLVED `[SOLE]` | c2753e0030 (-3.54%, p=0.0079); cited from ena.cpp/ena.h | graviton-optimization-plan, net-checksum, storage-measurement, tcp-tso; haiku-drift-check; ena.cpp; ena.h; ena/docs/watchdog-design.md |
| net-receive-profile.md | SUPERSEDED → ena-receive-latency-account.md (ABSENT from trunk) | self-declared; f76217c69b, 830d8d0814, c2753e0030 | arm64-memcpy, ena-multiqueue-headroom, ena-multiqueue-plan, ena-tx-offload, tcp-tso; memcpybench.c |
| throughput-measurement.md | LIVING | canonical throughput baseline; drift-gated | ena-multiqueue-{headroom,plan}, ena-tx-offload, graviton-optimization-plan, net-receive-profile, simd-vectorization-review, tcp-rcvbuf-cliff, tcp-send-autotune; haiku-drift-check |
| storage-measurement.md | LIVING `[SOLE]` | only disk IOPS/throughput baseline; current NVMe path + 2 live defects | ena-tx-offload, storage-perf-gate-proposal; haiku-drift-check |
| ena-multiqueue-headroom.md | RESOLVED `[SOLE]` | self-declared CLOSED; 29826 vs 29823 Mbit/s; #56/#61 | ena-multiqueue-plan, ena-production-readiness, graviton-optimization-plan, tcp-tso, throughput-measurement |
| ena-multiqueue-plan.md | SUPERSEDED → ena-multiqueue-headroom.md | CANCELLED 2026-08-24; 17d45faf8f | ena-multiqueue-headroom, net-receive-profile |
| ena-production-readiness.md | LIVING `[SOLE]` + pub-hygiene | open P1 backlog (ena.cpp:221); names internal NIC-vendor tree | ena/docs/watchdog-design.md |
| tcp-send-autotune.md | RESOLVED `[SOLE]` | 85f9d73594; TCPEndpoint.cpp:351/1610 | graviton-optimization-plan, tcp-rcvbuf-cliff, throughput-measurement, remote-desktop-unified-design; TCPEndpoint.cpp:1610 |
| tcp-rcvbuf-cliff.md | RESOLVED `[SOLE]` (rename/redirect, not plain move) | d32920e691, HW ~3650→4950 Mbit/s; TCPEndpoint.cpp:1226; title self-retracted | graviton-optimization-plan, tcp-send-autotune, throughput-measurement, remote-desktop-unified-design; nettput.cpp |
| tcp-tso.md | RESOLVED `[SOLE]` | TSO impossible (tx cap 0x3); e63fe3f24a, c2753e0030 | net-checksum, net-receive-profile |
| ena-interrupt-moderation.md | LIVING `[SOLE]` | #108 CLOSED but controller merged/live (ena.cpp:374/399/3968, ena.h:681); inert c7g+c8g | ena.h; ena-rx-cadence-falsified |
| ena-rx-cadence-falsified.md | RESOLVED `[SOLE]` | pre-registered A/B falsified cadence hypothesis | ena-interrupt-moderation |
| ena-keepalive-watchdog-false-reset.md | RESOLVED `[SOLE]` | ena.h:282/249, ena.cpp:2238-2241; #408 | ena/docs/watchdog-design.md; ena.h |
| ena-ack-completion-interrupts.md | LIVING | standing measurement of current ACK path (~28% io-irq); lever moot | ena/docs/watchdog-design.md |
| storage-perf-gate-proposal.md | RESOLVED → graviton/scripts/haiku-perf-gate | #112 CLOSED; db509e9c9f, e290a17561 | haiku-perf-gate, haiku-quota-verify |

## arm64 kernel / boot / platform (13)

| file | bucket | evidence | inbound refs |
|---|---|---|---|
| arm64-anyboot-mbr.md | RESOLVED `[SOLE]` | d5d9a4809e, 949bac43cc; AnybootImage:25-31, BootRules:243/253; UEFI boot-test unverified | none |
| platform-portability.md | LIVING (stale) | #94 CLOSED, #429 OPEN; #425/#427/#428 since CLOSED but doc still shows them open — reconcile | none |
| arm64-clock-step.md | RESOLVED `[SOLE]` | a81fbddb28; stale-artifact root cause | builder/README.md:42 |
| arm64-memcpy.md | RESOLVED `[SOLE]` | f76217c69b, HW -8.4% RX CPU; cited by name from 3 source files | memcpy.c:119, memset.c:51, kernel/lib/arch/arm64/Jamfile:28; chromium-arm64-scope, net-receive-profile, simd-vectorization-review |
| arm64-mprotect-query-present.md | RESOLVED `[SOLE]` | 36d365a594, 34093ec4bf; HW A/B c7g; mprotect_probe | device_area_probe.cpp; package-chain-status |
| arm64-serial-console-c7g.md | LIVING | no bug; `--latest` operational truth | ena/docs/watchdog-design.md; ec2-stop-start |
| metal-gicv3-panic.md | RESOLVED `[SOLE]` | 25f1c9e2e4, 132ab3a003, 95a466f0d0, f5367b3602; EL2/VHE | arm64-serial-console-c7g, metal-pci-segment |
| metal-pci-segment.md | RESOLVED `[SOLE]` LOAD-BEARING | f5367b3602 via e270548f33 (per-bridge ECAM); 53 PCI devices c7g.metal | arm64-serial-console-c7g, metal-gicv3-panic |
| scheduler-smp-placement.md | RESOLVED `[SOLE]` | 4cafdaa4c4, 220bdd6f9f; HW A/B 32-thread 9.9x | haiku-drift-check; ena/docs/watchdog-design.md |
| simd-vectorization-review.md | LIVING (SVE/libjpeg sections stale) | SVE→#88 (50f6a9be53), libjpeg→#329/#331 CLOSED; rankings/caveats live | chromium-arm64-scope, port-hygiene; haiku-drift-check; haiku-port-lint |
| mimeset-headless.md | RESOLVED (titular) + LIVE latent defect `[SOLE]` | 3eb457b35e (mimeset.cpp only); Application.cpp:539 exit(0) STILL LIVE | haiku-drift-check (glob) |
| ec2-stop-start.md | RESOLVED `[SOLE]` | 8331882470, 10a7d758bc, e6c9102f8c, gate 128a3f1761 | haiku-drift-check; ssh-arm64-desktop-bake; debeos-hardware-proof.sop.md |
| device-watchdog-and-bfs-crashsafety.md | RESOLVED (Part A) / UNSURE (Part B) `[SOLE]` | #91 CLOSED, PR #252 (4e16ab271e); Part B BFS crash-safety UNVERIFIED | Journal.cpp:1366 |

## Packaging / build / userland-porting / AMI (20)

| file | bucket | evidence | inbound refs |
|---|---|---|---|
| chromium-arm64-scope.md | RESOLVED `[SOLE decision]` | #379 CLOSED; "do not port Chromium now" | none |
| lean-ami-builder-gaps.md | RESOLVED (→ native-ec2-builds) | #33/35/37/38/39/40/51 CLOSED | none |
| playbook-parity-hook.md | LIVING | enablement guide for live haiku-playbook-parity (#90/#136) | none |
| arm-mcp-server-evaluation.md | RESOLVED | verdict "park"; arm-mcp wired nowhere | none |
| native-ec2-builds.md | LIVING | current native-haikuporter-over-SSM build path | AGENTS.md; debeos-metal-build-debug.sop.md; builder/README.md; pipeline/README.md; debeos-arm64-port/SKILL.md; haiku-nativebuild, haiku-crate-to-hpkg, haiku-ssh; fleet-dispatch.sh, fleet-worker.sh; lean-ami-builder-gaps, package-chain-status |
| porting-playbook.md | LIVING | living symptom→class→fix reference (#90; +#488/#505) | ArchitectureRules; haiku-triage-failures, haiku-pattern-miner, haiku-playbook-parity, haiku-port-lint; debeos-arm64-port/SKILL.md; haikuports-patches README + ~25 recipes; port-hygiene, playbook-parity-hook, graviton-optimization-plan, simd-vectorization-review |
| port-hygiene.md | LIVING | linter companion (#341; live haiku-port-lint) | porting-playbook; haikuports-patches recipes README + simde recipe; haiku-port-lint |
| crate-to-hpkg.md | LIVING | live haiku-crate-to-hpkg (#116) | uutils-coreutils-arm64; haiku-crate-to-hpkg; uutils/README.md, build-uutils-hpkg.sh |
| download-mirror.md | LIVING | deployed sources.debene.dev mirror (#172) + live scripts | porting-playbook; haiku-mirror-sync, haiku-bake-builder, haiku-nativebuild |
| native-cpython-365.md | LIVING | native CPython 3.14 state (#365/#552); stale aside → package-chain-status | chromium-arm64-scope, package-chain-status |
| graviton-optimization-plan.md | LIVING | per-item optimization board; open #98-#102 | ena-multiqueue-plan, ena-production-readiness, porting-playbook, sequencing, simd-vectorization-review; ena/docs/watchdog-design.md |
| cloudwatch-metrics-bake.md | LIVING | shipped default-on debeos-cloudwatch daemon (#140/#119) | graviton/ssh/UserBuildConfig |
| ssh-arm64-desktop-bake.md | LIVING | current make-ami recipe; stale aside → package-chain-status (refresh) | pipeline/README.md; pipeline/scripts/import-and-register.sh |
| package-chain-status.md | SUPERSEDED → native-ec2-builds.md + haiku-nativebuild `[SOLE]` | self-declared historical; sole blocker→patch rationale | builder/pyfix.sh; vim/zstd patches; arm64-clock-step, arm64-mprotect-query-present, arm64-package-bootstrap, haiku-graphics-upstream-review, native-cpython-365, sequencing, ssh-arm64-desktop-bake; packages/README.md (~11 edges) |
| arm64-package-bootstrap.md | SUPERSEDED → package-chain-status → native-ec2-builds | self-declared "do not plan from it" | package-chain-status; pipeline/README.md |
| go-arm64-bringup-scope.md | RESOLVED `[SOLE]` | #302 CLOSED, PR #361 MERGED; graviton/go-arm64/ artifacts | go-arm64/{README, debeos-aws/README, main.go, logs/M5,M6-proof, ssm-agent/README} |
| nodejs-arm64-scope.md | RESOLVED `[SOLE]` | #93 CLOSED; Node 20.15.1 RC=0; nodejs20-20.15.1.recipe; python3.10 pin | chromium-arm64-scope |
| uutils-coreutils-arm64.md | RESOLVED (toolchain caveat) `[SOLE]` | #93 CLOSED; 67-applet hpkg; residual rustc ICE needs newer rust_bin | crate-to-hpkg; uutils/README.md, build-uutils-hpkg.sh |
| webpositive-arm64-plan.md | RESOLVED `[SOLE]` | BUILT AND RENDERS; DefaultBuildProfiles 132-133/177-178; #84/#117 CLOSED | chromium-arm64-scope |
| arm64-native-build-out-of-space.md | RESOLVED `[SOLE]` | ENOSPC; cross-build.yml:249-264 (19000); cdk.json:46 (20 GiB); dsb oshst = hygiene | PRIORITY.md; AUDIT_LOG.md; pipeline/README.md |

## Remote-desktop / graphics / framebuffer + project-meta (12)

| file | bucket | evidence | inbound refs |
|---|---|---|---|
| debeos-identity.md | LIVING `[SOLE]` | design-of-record; config.ts:248 hrev59996; #201 AND #92 now CLOSED (status lines stale) | none |
| remote-desktop-broker.md | LIVING | src/servers/remote_broker/ (a797c4d4b2, #415/#423/#436) | haiku-remote-desktop; ssh/files/remote-desktop.sh |
| remote-desktop-m2-flow-control.md | LIVING | RemoteFlowQueue.{cpp,h} (0ceb3a6a79) | RemoteProtocol.h (comment); remote-desktop-unified-design |
| remote-desktop-m2-pricing.md | LIVING | measurement-of-record; RP_CAP_COMPRESS_ZSTD landed | remote-desktop-unified-design, remote-desktop-m2-flow-control |
| remote-desktop-options.md | SUPERSEDED → remote-desktop-unified-design.md | unified-design supersedes it; #95 CLOSED | ROADMAP.md; haiku-drift-check; vfb-route2-streaming, remote-desktop-unified-design |
| remote-desktop-send-buffer-wedge.md | RESOLVED `[SOLE]` | 8fa0bf55d6 discardWithoutReader (present); sole record of why it exists | remote-desktop-unified-design, remote-desktop-m2-flow-control; ssh/files/remote-desktop.sh |
| remote-desktop-unified-design.md | LIVING | design-of-record URP/1 (#118); client repo name deliberately omitted | rdsshc-run; rdprice.py; remote-desktop-m2-pricing, remote-desktop-m2-flow-control |
| vfb-route2-streaming.md | LIVING `[SOLE]` | sole record of measured Route 2 rig (p50 31/p99 81ms); live stream-fb-guest.sh | builder/stream-fb-guest.sh; chromium-arm64-scope, remote-desktop-unified-design |
| framebuffer-guest-capture.md | LIVING | boot-fb-guest.sh + qemu-screendump exist | haiku-graphics-upstream-review, package-chain-status, vfb-route2-streaming, simd-vectorization-review |
| haiku-graphics-upstream-review.md | LIVING | GO-1 VirtioQueue.cpp barrier rec still UNIMPLEMENTED (grep=0); snapshot @28e91d7076 | vfb-route2-streaming |
| sequencing.md | UNSURE (leaning stale) | board @caeccf2466; Phases 2/4/8 overtaken, no single replacement. Settle: ROADMAP.md + #76-95 authoritative? | haiku-graphics-upstream-review, package-chain-status, graviton-optimization-plan; haikuports-patches/README.md + haikuporter-unpack-compressed-tar.patch |
| feature-cook-flow.md | LIVING | documents this run's workflow; 3 engines present | README.md; debeos-metal-build-debug.sop.md |

## Phase-2 risk register (human-approved move; NOT this run)

1. Dangling successor: `ena-receive-latency-account.md` is cited as the resolving successor by
   7 docs but is absent from trunk (only on branch feat/ena-receive-latency-account, 5631bd15d4,
   never merged). Merge it or rewrite the 7 links before archiving the citing docs.
2. `haiku-drift-check:57` DESIGN_GLOBS_REGEX matches ~24 of these docs by name fragment, not by
   directory: an archive/ move keeps matching (no edit needed) but any RENAME must update it.
3. `[SOLE]` docs cited from source comments must stay reachable: arm64-memcpy (memcpy.c:119,
   memset.c:51, Jamfile:28), tcp-rcvbuf-cliff (TCPEndpoint.cpp:1226), tcp-send-autotune
   (TCPEndpoint.cpp:1610), ena-tx-offload / ena-keepalive / ena-interrupt-moderation
   (ena.cpp/ena.h), device-watchdog (Journal.cpp:1366); metal-pci-segment is load-bearing.
4. Content work before/instead of a move: mimeset-headless (Application.cpp:539 exit(0) still
   live), platform-portability (reconcile #425/#427/#428 closed, #429 open), debeos-identity
   (refresh #201/#92 status lines — both CLOSED).
5. tcp-rcvbuf-cliff is a rename/redirect (self-retracted title), not a plain move.
6. Publication-hygiene gate before moving ena-production-readiness (names internal NIC-vendor tree).
7. Highest inbound-edge moves: package-chain-status (~11 edges incl. 2 shipped patches),
   remote-desktop-options (4). Repoint the stale asides in ssh-arm64-desktop-bake and
   native-cpython-365 at native-ec2-builds instead of moving those LIVING docs.
8. Add one AGENTS.md line stating graviton/docs/archive/ is history, not current context
   (no such line or directory exists today).
