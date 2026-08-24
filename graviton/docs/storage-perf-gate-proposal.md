# Proposal: a storage assertion in haiku-perf-gate

**Status: PROPOSAL. Not implemented, not deployed.** Wants review before anything
lands, on the grounds that a gate which flakes gets disabled and then protects
nothing.

Baseline numbers, method and controls are in
[storage-measurement.md](storage-measurement.md).

## What to assert, and why only these two

Two assertions. Both were chosen because they are stable across runs *and*
sensitive to the code most likely to regress; everything else measured is either
noisy, at a hardware ceiling that would mask a regression, or both.

### 1. Sequential read at depth 8

```
disktput -f /dev/disk/nvme/1/raw -m seqread -b 256K -t 8 -T 120 -s 64G -J
```

- **Measured:** 1007.72, 1007.72, 1007.32 MiB/s across three runs at 120–180 s —
  **stable to 0.04%**, which is the tightest repeatability of anything measured.
- **Sensitive to:** the NVMe driver's submission and completion path, the DMA
  restrictions, and devfs. A regression in any of them shows here.
- **Threshold:** fail below **900 MiB/s** (a 10% margin). Do *not* assert an
  upper bound: this row sits at the volume's provisioned ceiling, so "faster"
  is not meaningful and an upper bound would fire on EBS variance.

Depth 8 specifically, not depth 1 and not depth 16. Depth 1 is pure device
latency and would pass through almost any software regression. Depth 16 is
already clipped by the volume ceiling in both directions and so has no headroom
to show an improvement or a mild regression.

### 2. Durability across a hard power loss

This is the assertion that would have caught the bug this project actually
shipped — an sshd host key with the right size, mode and mtime and 411 bytes of
zeros in it.

```
disktput -f /pw/probe -m seqwrite -b 256K -n 64M -t 1 -s 64M -P     # no fsync, no sync
aws ec2 stop-instances --force ; aws ec2 start-instances
disktput -f /pw/probe -m verify -b 256K -n 64M -t 1 -s 64M          # assert 0 bad blocks
```

- **Asserts:** `0 zero, 0 stale, 0 corrupt`. A length check would not have caught
  the historical failure; `-P` stamps each block with its own offset so the
  verify distinguishes "never written" from "some other block's data".
- **Written with no `fsync` and no `sync` on purpose**, so the assertion covers
  the page writer's periodic flush — the thing that was broken — rather than only
  the explicit-sync path.
- `--force`, because DeBeOS does not act on the inbound ACPI shutdown request, so
  this is a genuine power loss rather than a clean unmount.

## The volume is part of the assertion

**The gate must attach a dedicated scratch volume: gp3, 100 GiB, 16,000 IOPS,
1,000 MiB/s.** Not the root volume, which is 2 GiB gp3 sitting at the gp3 floor of
3,000 IOPS / 125 MiB/s with a 300 MiB filesystem on it. Measured there, the 900
MiB/s threshold is unreachable by construction and the assertion would fail every
image forever.

The gate must also **re-read the provisioning with `describe-volumes` and refuse
to run if it does not match**, rather than trusting its own launch template. A
gate whose threshold silently becomes unreachable because a volume spec drifted
is worse than no gate.

The span must be **written in full before the read is measured** — a never-written
gp3 block reads back as zeros without the backend being touched, which would pass
the threshold while measuring nothing. That costs ~65 s at 1 GiB/s and is not
optional.

## Three outcomes, not two — this is the part that stops it flaking

The existing gate treats "sshd never answered" as a failed image. That cannot be
carried over here, because there is a **live intermittent: roughly one warm reboot
in six on `c7g.large` loses the NIC, and it reproduces on the canonical image.**
A durability assertion that reboots the machine will meet that bug, and if its
only vocabulary is pass/fail it will blame storage for it and then be switched
off.

So the gate reports one of three states, and only one of them blocks:

| state | meaning | action |
|---|---|---|
| **PASS** | thresholds met, 0 bad blocks | promote |
| **FAIL** | node reachable, assertion violated | **block** |
| **INCONCLUSIVE** | node unreachable, or provisioning wrong | retry, then skip |

**FAIL requires the node to have answered.** An unreachable node is never a
storage verdict — it is an absent measurement.

Distinguishing them concretely:

- **Use `c7g.4xlarge`, not `c7g.large`.** The NIC intermittent is characterised on
  `c7g.large`; 4xlarge is also the only size with enough EBS bandwidth for the
  threshold to mean anything.
- **Prefer stop/start over warm reboot.** The intermittent is described on *warm
  reboot*; a stop/start is a cold boot on fresh placement. This should be stated
  as a mitigation with an unknown residual, not as immunity — nobody has measured
  the NIC bug's rate across stop/start.
- **Prove liveness before judging.** After restart, require ssh *and* a trivial
  command to succeed. If ssh never comes up, retry the stop/start **once**; if it
  fails again, emit INCONCLUSIVE with the console output attached.
- **Separate the two assertions' failure domains.** Run the depth-8 read *before*
  any reboot, so a throughput regression is reported even when the durability
  half ends INCONCLUSIVE. This is the single most useful property of the design:
  the cheap, stable, reboot-free assertion never depends on the flaky one.
- **Read back the pre-loss checksum from outside the node.** Record the expected
  SHA-256 on the harness before stopping, so the verdict does not depend on a file
  the node writes and then reads.

## Cost

~65 s initialise, 120 s read, ~30 s write, ~3 min stop/start, ~60 s verify —
call it **7 minutes plus one stop/start** per candidate. If that is too much for
every build, the depth-8 read alone is ~3 minutes with no reboot and still catches
driver regressions; the durability half could run on a schedule rather than per
image.

## Open question for review

Whether the durability assertion belongs in the promotion gate at all, or in a
nightly. It is the highest-value assertion in this document and also the only one
that reboots, which is where the known intermittent lives. My recommendation is:
**depth-8 read per candidate, durability nightly**, so the flaky-adjacent half can
never block a promotion — and so that when it does fail, someone looks at it
instead of disabling it.
