# Proposal: a storage assertion in haiku-perf-gate

**Status as of 2026-08-24: PARTLY IMPLEMENTED — one of the three assertions has
landed. This is no longer purely a proposal.**

> ~~**Status: PROPOSAL. Not implemented, not deployed.**~~ Verified against the tree
> on 2026-08-24:
>
> | Assertion | State |
> |---|---|
> | **1. Sequential read at depth 8** (`disktput -m seqread`, 900 MiB/s floor) | **NOT landed.** `graviton/scripts/haiku-perf-gate` contains no `disktput`, no `seqread`, no 900 MiB/s floor and no scratch-volume attachment. |
> | **2. Durability across a hard power loss** | **NOT landed.** |
> | **3. Write liveness / the three-outcome ICMP ladder** | **LANDED.** `haiku-perf-gate` implements it (`db509e9c9f`, `e290a17561`) — see its "Retries only the case that deserves it" block: an ICMP-answering node is a real verdict either way and is decided immediately; a node with no ICMP at all is retried. A fuller prototype exists as `graviton/scripts/haiku-quota-verify`. |
>
> The framing reason still applies to what is left: **a gate which flakes gets
> disabled and then protects nothing.** Assertions 1 and 2 still want review.

Baseline numbers, method and controls are in
[storage-measurement.md](storage-measurement.md).

## What to assert, and why only these three

~~Two assertions. Both were chosen~~ **Three assertions** — there are three numbered
subsections below, and "Two" contradicted this section's own heading one line above.
*(Corrected 2026-08-24.)* They were chosen because they are stable across runs *and*
sensitive to the code most likely to regress; everything else measured is either
noisy, at a hardware ceiling that would mask a regression, or both.

### 1. Sequential read at depth 8

```
disktput -f /dev/disk/nvme/1/raw -m seqread -b 256K -t 8 -T 120 -s 64G -J
```

- **Measured on `c7g.4xlarge`** (Graviton3 / Neoverse V1, 16 vCPU), `us-west-2`,
  **2026-08-24**, on a **dedicated 100 GiB gp3 at 16,000 IOPS / 1,000 MiB/s**:
  1007.72, 1007.72, 1007.32 MiB/s across three runs at 120–180 s —
  **stable to 0.04%**, which is the tightest repeatability of anything measured.
  *(Instance class, date and volume spec added 2026-08-24 — without all three, this
  row and the threshold derived from it mean nothing, and "Use `c7g.4xlarge`, not
  `c7g.large`" is 130 lines further down.)*

  > **What 0.04% repeatability does NOT buy you.** This project has a storage
  > measurement that was repeatable to **under 1% across three reps** and was
  > **2.2× the instance's hard maximum** — every cell was short enough that the EBS
  > token bucket never bound, so all three measured burst credit. Repeatability
  > cannot detect a systematic artefact, because a systematic artefact repeats. The
  > reason *this* row is trustworthy is the bullet below: it sits **at** a known
  > provisioned ceiling rather than above it.
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

### 3. Write liveness, and that back-pressure survived

New since the first draft, and the reason this document needed revisiting: the
page-writer starvation fix landed, and **its failure mode is a node that stops
answering ssh.** A gate that cannot distinguish that from the NIC intermittent
will blame the wrong subsystem; a gate that only asks "did the starvation stop"
will pass a change that removed back-pressure altogether.

```
# 16 GiB of allocating buffered writes, queued ON THE NODE so the load
# outlives the harness.
#
# CONFLICT, flagged 2026-08-24: "use the 125 MiB/s scratch" contradicts the
# section "The volume is part of the assertion" below, which requires a
# 1,000 MiB/s volume and tells the gate to REFUSE TO RUN if the provisioning
# does not match. As written, the gate must refuse to run on the very volume
# this line tells it to use. Decide before either assertion lands: assertion 1
# needs the fast volume to clear its 900 MiB/s floor, while this assertion
# wants a slow one because it reaches the stall in 12 GiB instead of 20.
# Two volumes, or a per-assertion provisioning check -- not one shared
# "the scratch".
# Use the 125 MiB/s scratch: onset is at 12 GiB.
for i in $(seq 1 8); do
    disktput -f /w/new-$i -m seqwrite -b 256K -t 4 -n 2G -s 2G -S
done &
```

Assert **both** of the following, because each catches what the other misses:

| assertion | catches |
|---|---|
| zero samples in the starved state (see the ladder below) | the starvation regressing |
| `waits > 0` **and** `timeouts` ≈ 0 from the console | back-pressure having been **deleted** rather than fixed |

The second is the one that would otherwise be missed. `waits == 0` means the quota
never engages at all — which makes the starvation test pass while leaving dirty
memory unbounded, resurfacing much later as something far harder to attribute.
"Did the starvation stop" answers yes in both the fixed and the broken case.

Counters come from the serial console, so they are readable while userland is
starved:

```
aws ec2 get-console-output --instance-id <id> --latest --output text \
  | grep "quota wait timed out" | tail -1
```

`--latest` is not optional; without it the API returns an empty body and every
grep counts zero. Also assert that no timeout line carries **`queue 0 pages`** —
that string was the signature of the cross-device coupling, and its return would
mean the page-bound global quota had regressed to a duration sum.

## The volume is part of the assertion

**The gate must attach a dedicated scratch volume: gp3, 100 GiB, 16,000 IOPS,
1,000 MiB/s.** Not the root volume, which is 2 GiB gp3 sitting at the gp3 floor of
3,000 IOPS / 125 MiB/s with a 300 MiB filesystem on it. **(See the conflict flagged
in assertion 3's recipe above: that recipe asks for the 125 MiB/s volume, which the
`describe-volumes` check in the next paragraph would reject.)** Measured there, the 900
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

## Three outcomes, and a measured way to tell them apart

The existing gate treats "sshd never answered" as a failed image. That cannot be
carried over, and the reason is sharper than when this was first drafted:

- **A storage liveness regression presents as a node that stops answering ssh.**
- **The live NIC intermittent — roughly one warm reboot in six on `c7g.large`,
  reproducing on canonical — also presents as a node that stops answering ssh.**

Identical outward symptom, opposite causes. A gate that conflates them will either
block good images or, worse, be switched off.

### The discriminator, from measurement rather than guesswork

The starvation was sampled ~72 times while it was happening, and **ICMP answered
and TCP `:22` accepted the connection at every single sample** — only the SSH
banner never arrived. The kernel, the interrupt path and the NIC were demonstrably
alive; it was userland that could not progress. A lost NIC produces the opposite:
no ICMP at all.

So the gate probes three things in order, and the pattern names the cause:

| ICMP | TCP `:22` | SSH banner | meaning | verdict |
|---|---|---|---|---|
| ok | accepts | **arrives** | healthy | continue |
| **ok** | **accepts** | **never** | **userland starved — the storage regression** | **FAIL** |
| ok | refused | — | network stack up, sshd gone | FAIL (but not storage) |
| **fail** | — | — | NIC lost or node gone — apparatus | **INCONCLUSIVE** |

**"The node answered" must mean ICMP answered, not ssh answered.** That is the
correction this revision exists for: ssh not answering is the very symptom under
test, so using it as the liveness precondition would make the assertion unable to
fire.

| state | action |
|---|---|
| **PASS** | thresholds met, 0 bad blocks, `waits > 0`, no starved samples | promote |
| **FAIL** | ICMP answered and an assertion was violated | **block** |
| **INCONCLUSIVE** | no ICMP, or volume provisioning wrong | retry once, then skip |

Further mitigations, unchanged in intent:

- **Use `c7g.4xlarge`, not `c7g.large`.** The NIC intermittent is characterised on
  `c7g.large`, and 4xlarge is also the only size with enough EBS bandwidth for the
  throughput threshold to mean anything.
- **Prefer stop/start over warm reboot** for the durability half. The intermittent
  is described on *warm reboot*; stop/start is a cold boot on fresh placement.
  Stated as a mitigation with an unknown residual, not as immunity.
- **Run the reboot-free assertions first** — sequential read, and the write
  liveness test — so a regression in either is reported even when the durability
  half ends INCONCLUSIVE. This remains the single most useful property of the
  design.
- **Record the expected checksum on the harness** before stopping, so the durability
  verdict never depends on a file the node writes and then reads.

## Cost

~65 s initialise, 120 s read, ~30 s write, ~7 min for the 16 GiB liveness load,
~3 min stop/start, ~60 s verify — call it **14 minutes plus one stop/start** per
candidate. If that is too much for
every build, the depth-8 read alone is ~3 minutes with no reboot and still catches
driver regressions; the durability half could run on a schedule rather than per
image.

## ~~Open question for review~~ — answered in this same section (heading fixed 2026-08-24)

Whether the durability assertion belongs in the promotion gate at all, or in a
nightly. It is the highest-value assertion in this document and also the only one
that reboots, which is where the known intermittent lives.

My recommendation is: **depth-8 read and the write-liveness assertion per
candidate** — both reboot-free, both cheap to attribute — and **durability
nightly**, so the only half that reboots can never block a promotion, and when it
does fail someone looks at it instead of disabling it.

> **Partly acted on already (2026-08-24):** the write-liveness half is in the gate;
> the depth-8 read is not. And note the *other* reboot now in play — the gate performs
> a **stop/start on every run** (`128a3f1761`), so "reboot-free" is no longer a
> property of the gate as a whole. That strengthens rather than weakens the case for
> keeping durability out of it: the ~1-in-6 NIC-attach intermittent on `c7g.large` now
> has an occasion to fire regardless.

The write-liveness assertion earns its place per-candidate rather than nightly
because the defect it guards was a machine that reported healthy and did nothing,
which is precisely the class of regression that should never reach an image anyone
promotes.
