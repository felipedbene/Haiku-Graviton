# Baking CloudWatch host metrics into the canonical AMI

DeBeOS publishes host metrics to CloudWatch from a small **standalone daemon**,
`debeos-cloudwatch`, baked into the canonical image. From the moment an instance
boots it publishes to CloudWatch namespace **`DeBeOS/Host`** every 60s with no
per-instance configuration (**default-on**).

The metrics come from Haiku's own kernel counters
(`get_system_info`/`get_cpu_info`), so `CPUUtilization` is the true busy percent
— not the hypervisor's number, which under-reports a Haiku/arm64 guest by ~55x
(DeBeOS #140).

## Why a standalone daemon (not a flag on the SSM agent)

SSM node registration is a **single-owner** role: two processes on the same
instance both calling `UpdateInstanceInformation` (heartbeat) wedge Run Command
(commands stall Pending). The official `amazon-ssm-agent` owns SSM. So the
metrics feature is factored **out** of any SSM agent into `debeos-cloudwatch`,
which makes **no SSM calls at all** — it only ever calls `PutMetricData`. It
therefore coexists safely with whatever owns SSM and never contends for the
command channel.

`debeos-cloudwatch` is built from the mgmt-agent repo
(`github.com/felipedbene/haiku-mgmt-agent`, `make cloudwatch`), reusing that
repo's SigV4/TLS/HTTP/JSON + metrics + timesync code but none of its MDS/MGS/
runner machinery. It is a separate binary (`/boot/system/bin/debeos-cloudwatch`)
and a separate hpkg (`debeos_cloudwatch-*`).

## What lands in CloudWatch

Namespace `DeBeOS/Host`, dimension `InstanceId=<i-...>`, every `--interval`
seconds (default 60):

| Metric | Unit | Source |
|---|---|---|
| `CPUUtilization` | Percent | summed per-CPU `active_time` delta / (wall × cpu_count) |
| `MemoryUtilization` | Percent | `used_pages / max_pages` |
| `MemoryUsedBytes` | Bytes | `used_pages × B_PAGE_SIZE` |
| `MemoryAvailableBytes` | Bytes | `(max_pages − used_pages) × B_PAGE_SIZE` |
| `DiskUtilization` | Percent | `statvfs("/boot")` |
| `DiskUsedBytes` | Bytes | `statvfs("/boot")` |
| `NetworkInBytes` | Bytes | sum of non-loopback ifaces `receive.bytes` delta |
| `NetworkOutBytes` | Bytes | sum of non-loopback ifaces `send.bytes` delta |
| `ThreadCount` | Count | `used_threads` |

`CPUUtilization` and the network deltas are omitted on the first cycle and
across a counter reset; they appear from the second cycle (~2 intervals of boot).

## How the bake picks it up

The daemon is baked exactly like OpenSSH and the SSM agent — dropped straight
into `system/packages` as a prebuilt hpkg, bypassing the solver and the
DeBeOS-vendor check (see the comment block in `graviton/ssh/UserBuildConfig`).
That file has a stanza that globs the pool for `debeos_cloudwatch-*-arm64.hpkg`
**version-agnostically**, so a newer hpkg drops in with no Jam edit.
`graviton/pipeline/buildspecs/cross-build.yml` already syncs the pool with
`--include "debeos_cloudwatch-*"`.

Two hard requirements the hpkg must satisfy (both already true of the built
artifact — see the mgmt-agent repo `packaging/cloudwatch/`):

1. **zlib compression, not zstd.** The lean arm64 image's packagefs has no zstd
   reader, so a zstd hpkg lands in `system/packages` but fails to activate.
   `packaging/build-cloudwatch-hpkg.sh` uses `package create -z zlib`.
2. **`requires { haiku }` only.** The daemon statically links mbedTLS, so it has
   no runtime package dependency the lean image cannot satisfy.

The pool must contain **exactly one** `debeos_cloudwatch-*` hpkg (readdir-order
`Glob`). The staging step **replaces** the old one; it does not add alongside.

## Operator: staging + bake

Build `debeos-cloudwatch` natively on a Graviton DeBeOS builder (`gcc` + static
mbedTLS from the DeBeOS repo), run `packaging/build-cloudwatch-hpkg.sh`, then
stage the hpkg into the pool the bake reads.

**Pipeline bake** — the cross-build step syncs the lean pool from
`s3://<pipeline-work-bucket>/hpkg-pool/`:

```bash
aws s3 rm  "s3://<pipeline-work-bucket>/hpkg-pool/debeos_cloudwatch-<old>-arm64.hpkg"  # if any
aws s3 cp  debeos_cloudwatch-0.1.0-1-arm64.hpkg \
           "s3://<pipeline-work-bucket>/hpkg-pool/debeos_cloudwatch-0.1.0-1-arm64.hpkg"
```

then trigger the bake pipeline (`HaikuGravitonBakePipeline`): Source →
CrossBuild → Register → Test → Approve (manual) → Promote. Promotion of the
canonical is the gated manual step and is not part of this change.

**Local bake** (per `AGENTS.md`):

```bash
mkdir -p /path/to/pool
cp debeos_cloudwatch-0.1.0-1-arm64.hpkg /path/to/pool/
cp graviton/ssh/UserBuildConfig "$GEN/UserBuildConfig"
HG_POOL_DIR=/path/to/pool HG_SSH_DIR=/opt/haiku/ssh \
  jam -q -j"$(nproc)" -sHAIKU_IMAGE_SIZE=19000 @minimum-mmc
```

The bake log prints `haiku-graviton: baking CloudWatch metrics daemon
debeos_cloudwatch-0.1.0-1-arm64.hpkg into system/packages`.

## Relationship to the SSM agent

This change adds the metrics daemon; it does not touch the SSM-agent stanza.
`debeos-cloudwatch` coexists with whatever owns SSM. When the official
`amazon-ssm-agent` becomes the baked SSM primary (retiring the DeBeOS SSM
agent), `debeos-cloudwatch` is unaffected — it never registered SSM in the first
place, which is the whole reason it is a separate daemon.

## IAM

`PutMetricData` needs `cloudwatch:PutMetricData`. The fleet's launch profile
`ssm-instance-profile` **already grants it** via the attached
`CloudWatchAgentServerPolicy` (and, redundantly, `PowerUserAccess`), so
instances launched by `graviton/scripts/haiku-launch` need no IAM change.

If a leaner launch profile carries only `AmazonSSMManagedInstanceCore`, add
`cloudwatch:PutMetricData` (Resource `*`) to that role — attach
`CloudWatchAgentServerPolicy`, or an inline statement:

```json
{ "Effect": "Allow", "Action": "cloudwatch:PutMetricData", "Resource": "*" }
```

Without the grant the daemon still runs; each cycle logs a `PutMetricData
failed: status=403` warning and no data appears in `DeBeOS/Host`.

## Verifying after a bake

```bash
aws cloudwatch list-metrics --namespace DeBeOS/Host --region us-west-2
aws cloudwatch get-metric-statistics --namespace DeBeOS/Host \
  --metric-name CPUUtilization --dimensions Name=InstanceId,Value=<i-...> \
  --start-time <T-10m> --end-time <now> --period 60 --statistics Average \
  --region us-west-2
```

A sane `CPUUtilization` (single-digit percent on an idle box, not ~0.02%) plus
the instance still reporting SSM `Online` and Run Command still working confirms
the metrics path and that SSM is uncontended.
