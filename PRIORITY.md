# PRIORITY.md — Phase 3 triage

Top candidates from `FINDINGS.md` for a Phase-4 deep pass. Ranked by **exercised surface**
(ENA > network stack > BFS > app_server-remote > arm64/other) × **real-bug severity** (finding
class, not the tool's generic level) × **Phase-2 upstream match**. Pure-style noise
(narrowing-conversions, reserved-identifiers, `unknownMacro`, `internalAstError`) is excluded and
stays parked in `FINDINGS.md`.

> **Not yet confirmed.** Phase 3 ranks; Phase 4 confirms against the code + a real workload. The
> "bet" column is my prior on whether it survives manual review, not a verdict. 168 active-surface
> bug-shaped candidates exist; this is the top 28.

> **CORRECTION — 2026-08-27 (read before trusting the BFS ranking).** This list originally floated
> BFS to the top partly on an *observed* "volume-wide corruption / page-writer defect under
> concurrent I/O." That premise is **falsified.** On hardware, the acute failure (freshly written
> files vanishing as `B_ENTRY_NOT_FOUND` during a 200+-crate parallel Rust build) was measured to be
> **out of disk space** on an 8.8 GiB image — not a filesystem or DMA defect. A concurrent-write
> instrument found **zero** corruption across cached, pure-`O_DIRECT`/raw-DMA, and rename-churn
> patterns under memory pressure; squeezing free space reproduced the exact errno. Fixed by the
> 20 GiB image bump (now the canonical AMI). See
> `graviton/docs/arm64-native-build-out-of-space.md`. The NVMe `dsb oshst` barrier was hardware
> hygiene, **not** the fix. (A secondary `darling` 0.24.1 `rustc` ICE is an upstream compiler bug,
> unrelated.)
>
> **Consequence for this list:** the BFS static findings below still stand *as code-quality
> candidates on their own merit* — an always-true condition is worth a look regardless — but they
> are **no longer backed by an observed corruption bug**, so BFS **urgency drops**. The upstream
> Trac panics (#11399/#14180/#20230) are real and independent of the ENOSPC story, so #5/#7 keep
> their upstream weight. Net: work BFS for correctness, not as a corruption emergency. This is a
> case of static analysis + two expert workflows over-attributing to BFS what measurement pinned to
> disk space — logged in `graviton/audit/AUDIT_LOG.md`.

## Standouts (cross-subsystem, by real-bug severity)

| # | file:line | id | subsystem | sev | upstream | bet | why it matters / Phase-4 verify |
|---|---|---|---|---|---|---|---|
| 1 | `network/stack/radix.c:641,645` | uninitvar `m0` | network-stack | **crit** | — | med | Uninitialized var in the BSD routing radix tree — used by every route lookup DeBeOS runs. Confirm `m0`'s definedness on all paths; verify under the jumbo-frame iperf3 harness + route churn. |
| 2 | `network/stack/radix.c:375,384` | nullPointer `rn` | network-stack | **crit** | — | med | Possible NULL deref in radix node walk. Same file/subsystem as #1; check the guard preceding the deref. |
| 3 | `network/datalink_protocols/arp/arp.cpp:896,897` | nullPointer `entry` | network-stack | **crit** | — | med | NULL deref in ARP entry handling — on the hot path for every L2 send on ENA. Verify with ARP table churn. |
| 4 | `bfs/Journal.cpp:793` | knownConditionTrueFalse | bfs | med | — | med | `LogEntryLength() > FreeLogBlocks()` is *always true* — a journal free-space guard that never takes its intended branch. Worth confirming as a logic defect on its own merit. (Was ranked "high/crash-safety"; downgraded — the corruption it was tied to is now explained as ENOSPC, see 2026-08-27 correction.) Verify via checkfs + a genuine log-full case. |
| 5 | `bfs/BPlusTree.cpp:1458` | knownConditionTrueFalse | bfs | **high** | **#11399, #14180** | high | `keyIndex > node->NumKeys()` always true — a B+tree bounds check in the file with two open upstream panics (`BPlusTree::Find()` KDL, B+tree header-size KDL). Strongest static⋂upstream signal in the sweep. Verify with checkfs on a large/hostile volume. |
| 6 | `bfs/Inode.cpp:2222` | knownConditionTrueFalse | bfs | **high** | — | med | `newOffset > size` always true — possible off-by-one/width bug in inode size/offset math (truncation/extend path). |
| 7 | `bfs/BlockAllocator.cpp:890` | knownConditionTrueFalse | bfs | **high** | ties #20230 | med | `group.fLargestLength >= bestLength` always true in block allocation; #20230 is an open `get_cached_block` negative-block-number panic. |
| 8 | `ena/ena-com/ena_eth_com.c:46,165` | int-mult-used-as-pointer | ena | **high** | — | med | Multiplication in `int` then used as a pointer offset in the ENA datapath — truncation/overflow risk on large descriptor counts. Keep vendored shape; verify with jumbo TX/RX + `ena_fault`. |
| 9 | `arch/arm64/arch_debug.cpp:418` | nullPointer `argv` | kernel/arm64 | **high** | — | low | NULL deref in the arm64 kernel debugger command path (`argv`). Only reached from KDL, so low blast radius, but it's in the boot-critical arch. |
| 10 | `bfs/Volume.cpp:128` | uninitMemberVar `fDevice` | bfs | **high** | — | med | `Volume::fDevice` not initialized in the ctor — a mount that errors before assignment could act on a garbage device fd. |
| 11 | `bfs/BPlusTree.cpp:701` | implicit-widening (size) | bfs | med | **#14180** | med | Widening after a 32-bit multiply in B+tree size math — the arithmetic behind the #14180 header-size KDL. |
| 12 | `bfs/CheckVisitor.cpp:20,32` | uninitMemberVar | bfs | med | ties #14456 | low | `check_index::name`, `CheckVisitor::control` uninitialized — checkfs internals (open checkfs KDLs upstream). |

## Rest of the top 28 (grouped by subsystem for the deep-pass selection)

**ENA** (highest exercised-surface weight; findings mostly vendored `ena-com/`, keep close to upstream)
| # | file:line | id | sev | note |
|---|---|---|---|---|
| 13 | `ena/ena-com/ena_com.c:377,466,1005,1017` | implicit-widening→size_t | med | DMA/ring size math; confirm no 32-bit overflow before widening. |
| 14 | `ena/ena-com/ena_com.c:917,1952,1993` | assignment-in-if | med | admin-queue paths; readability→bug risk. |
| 15 | `ena/ena.cpp:404` | cert-err33 (ignored `snprintf` ret) | low | version string; low risk. |
| 16 | `ena/ena.cpp:703` | cert-err34 (`atoi`) | low | driver_settings parse; no error signalling. |

**network-stack** (beyond #1–3)
| # | file:line | id | sev | note |
|---|---|---|---|---|
| 17 | `network/protocols/ipv4/ipv4_address.cpp:140,172`, `ipv6_address.cpp:190` | nullPointerRedundantCheck `b` | med | check-after-use ordering; often benign but worth a read on the addr path. |
| 18 | `network/stack/datalink.cpp:435` | nullPointerRedundantCheck `domain` | med | datalink send path. |
| 19 | `network/devices/tunnel/tunnel.cpp:337,344` | implicit-widening→size_t | med | tunnel device buffer math. |
| 20 | `network/protocols/*` (dialup/ppp/bluetooth) | suspicious-string-compare, cert-err33/34, branch-clone | low | PPP/dialup/bluetooth — **not exercised on Graviton**; park unless RPi work needs them. |

**BFS** (beyond #4–7,10–12)
| # | file:line | id | sev | note |
|---|---|---|---|---|
| 21 | `bfs/Inode.cpp:1014,2760,3011,3027`, `Volume.cpp:222,621` | assignment-in-if | med | inode/volume paths; verify each is intentional. |
| 22 | `bfs/kernel_interface.cpp:309,350,389,2426` | branch-clone | low | identical branches — dead/duplicated logic to simplify. |
| 23 | `bfs/FileSystemVisitor.cpp:19` | uninitMemberVar `fParent` | med | visitor base; used by checkfs + indexing. |
| 24 | `bfs/BPlusTree.cpp:577,1819,1939,1986,2002,2228,2709,2729` | assignment-in-if | med | B+tree mutation paths (same file as #5/#11). |

**app_server-remote**
| # | file:line | id | sev | note |
|---|---|---|---|---|
| 25 | `remote/RemoteHWInterface.cpp:285,291` | redundantAssignment `result` | med | `result` overwritten before use — possible dropped error in the remote interface init. |
| 26 | `remote/RemoteHWInterface.cpp:66` | cert-err34 (`sscanf`) | low | port/geometry parse; no error check. |
| 27 | `remote/RemoteDrawingEngine.cpp:1122–1130` | incorrect-roundings | low | `(double+0.5)` cast — coordinate rounding; cosmetic, use `lround`. |

**kernel/arm64**
| # | file:line | id | sev | note |
|---|---|---|---|---|
| 28 | `arch/arm64/gicv3_its.cpp:173–606`, `arch_int.cpp:407,411` | implicit-widening→size_t | med | ITS table + exception-path size math. Likely constant-safe, but given the GICv3-ITS bring-up history, confirm no truncation on real redistributor counts. |

---

## Recommended deep-pass order (my call; you choose)

*(Order revised 2026-08-27 after the BFS-corruption→ENOSPC correction above.)*

1. **network-stack** — ⚠️ **UPDATE 2026-08-27 (shakedown):** the `radix.c` cluster (#1/#2 `uninitvar m0`
   at :641/:645, and the null-`rn` derefs at :375/:384) is **CONFIRMED false-positive** — verified
   against the code: `m0` is assigned unconditionally at line 632 inside the `if`-condition (cppcheck
   drops the side effect), and `last` is guaranteed non-NULL by the radix treetop invariant. Classic
   cppcheck value-flow FPs on vendored 4.4BSD code. **`arp.cpp:896` (null `entry`) was NOT reached in
   the shakedown (crowded out by the 4 radix lines) and remains the one live network-stack lead.**
   Verify arp.cpp; skip the radix cluster.
2. **BFS** — #5/#7 keep their weight via the real, independent upstream panics (#11399/#14180/#20230);
   #4/#6/#10 are worth a correctness pass but are **no longer a corruption emergency** (see the
   correction banner). Verify with checkfs on hostile volumes, not the durability harness.
3. **ENA** — #8 (int-mult-as-pointer) is the one worth real scrutiny; the rest are low-risk vendored nits.

Everything below the top 28, and all `other-parked`, stays in `FINDINGS.md` for a future run.
