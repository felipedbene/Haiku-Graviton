# haiku-graviton bake pipeline (CDK)

A cloud-native, IaC-defined replacement for the manual / SSM-driven bake that
turns the `graviton` branch of Haiku into a reachable arm64 EC2 AMI. This is a
self-contained **public AWS CDK** app (TypeScript, `aws-cdk-lib`) — it is a
personal AWS/GitHub project and deliberately uses **no** Amazon-internal
constructs.

> Nothing here is deployed for you. `cdk synth`/`diff` are safe; `cdk deploy`
> and the bake it drives are the operator's call. See **Deploying** below.

---

## Why a CDK pipeline and not EC2 Image Builder

**Decision: CDK-defined CodePipeline. Image Builder is the wrong tool for this
workload.**

EC2 Image Builder's model is: *launch an instance of a **base AMI**, run
components on it to customize/harden it, snapshot the result.* It assumes the OS
you are imaging already boots as an EC2 instance and that "building" means
running steps *inside* that running OS.

Our bake is neither of those things:

1. **It is a cross-build of a foreign OS on a Linux host.** Haiku for arm64 is
   produced by `configure --build-cross-tools arm64` + `jam @minimum-mmc` on an
   Ubuntu/Graviton builder, with OpenSSH cross-built into an `hpkg` and injected
   via `graviton/ssh/UserBuildConfig`. There is no running Haiku instance to run
   "components" in, and Image Builder cannot drive a `jam` cross-compile.
2. **The artifact reaches EC2 via disk-image import, not a base AMI.** The `.mmc`
   build output is converted MBR→GPT (`make-gpt-image.sh`) and turned into an AMI
   with `ec2 import-snapshot` + `ec2 register-image`. Image Builder has no
   equivalent of "import this raw disk I produced elsewhere."

Image Builder *could* only play a role **after** a Haiku base AMI already exists
— e.g. a customization component layered onto a booted Haiku instance. But Haiku
on EC2 is headless and managed over SSH; there is no in-guest agent/SSM story to
run Image Builder components, and every customization we need
(`openssh`, launch jobs, the desktop bringup, the single-owner-of-port-22 fix)
must happen **at image-assembly time inside `jam`**, not by mutating a booted
instance. So even the "layer a component" option buys nothing here.

The bake is a *build + disk-image import* pipeline, which maps cleanly onto
**CodePipeline + CodeBuild** (build) and a CodeBuild/Lambda control-plane step
(import + register + tag). That is what this app models.

**Honest tradeoff:** choosing CDK/CodePipeline means we own the builder
environment (the arm64 Ubuntu image, the cross-tools cache, the `jam` recipe)
and the import/register orchestration ourselves — Image Builder would have
managed a build instance lifecycle for us. For a cross-built foreign OS that is
the right trade: Image Builder's managed lifecycle is worthless when it can't
perform the actual build step.

---

## Architecture

```
GitHub (fork/branch)            CodeBuild arm64 (Graviton, Ubuntu 24.04)
  graviton branch  ──Source──▶  CrossBuild
                                  configure --build-cross-tools arm64  (S3-cached)
                                  jam @minimum-mmc            (pass 1: stage pkgs)
                                  build-openssh-arm64.sh      (OpenSSH -> hpkg)
                                  cp UserBuildConfig ; jam @minimum-mmc (pass 2)
                                  make-gpt-image.sh           (MBR .mmc -> GPT raw)
                                        │  artifact: haiku-ec2.raw
                                        ▼
                                CodeBuild  Register
                                  s3 cp raw -> import/
                                  ec2 import-snapshot ; wait
                                  ec2 register-image  arm64/uefi/ena/hvm /dev/xvda
                                  tag candidate=true   (canonical NOT set)
                                        │  exports AMI_ID
                                        ▼
                                Manual Approval  ◀── human gate for canonical
                                        │
                                        ▼
                                CodeBuild  Promote
                                  graviton/scripts/haiku-canonical promote $AMI_ID
                                  haiku-canonical check  (single-canonical invariant)
```

The buildspecs (`buildspecs/*.yml`) are intentionally thin — they invoke the
**existing** vendored recipe rather than reimplementing it:
`graviton/ssh/build-openssh-arm64.sh`, `graviton/ssh/UserBuildConfig`,
`haiku-on-ec2`'s `make-gpt-image.sh`, and `graviton/scripts/haiku-canonical`.

### Files

| Path | Purpose |
|---|---|
| `bin/pipeline.ts` | CDK app entrypoint; loads config, instantiates the stack. |
| `lib/config.ts` | All parameters (account/region/branch/compute/repos), from CDK context with env fallbacks. Nothing hardcoded, no secrets. |
| `lib/haiku-graviton-pipeline-stack.ts` | The pipeline, three CodeBuild projects, least-privilege IAM, the S3 work bucket, and the manual approval gate. |
| `buildspecs/cross-build.yml` | Cross-tools + OpenSSH + two-pass `jam @minimum-mmc` + `make-gpt-image.sh`. |
| `buildspecs/register-image.yml` | Invokes the import/register helper; exports `AMI_ID`. |
| `buildspecs/promote.yml` | Runs `haiku-canonical promote` + `check`. |
| `scripts/import-and-register.sh` | `import-snapshot` → `register-image` (arm64/uefi/ena) → candidate tags. |

---

## Parameters

Set in `cdk.json` `context`, or override per-invocation with `-c key=value`
(or the matching env var). Required ones have no safe default:

| Context key | Env | Default | Notes |
|---|---|---|---|
| `haiku:account` | `HAIKU_ACCOUNT` | `668984504585` | Isengard account. |
| `haiku:region` | `HAIKU_REGION` | `us-west-2` | |
| `haiku:repoOwner` | `HAIKU_REPO_OWNER` | — **required** | GitHub owner of the fork. |
| `haiku:repoName` | `HAIKU_REPO_NAME` | `haiku` | |
| `haiku:branch` | `HAIKU_BRANCH` | `graviton` | |
| `haiku:connectionArn` | `HAIKU_CONNECTION_ARN` | — **required** | CodeConnections GitHub connection ARN. |
| `haiku:buildComputeType` | `HAIKU_BUILD_COMPUTE` | `BUILD_GENERAL1_2XLARGE` | ~72 vCPU/144 GB arm64, comparable to the `c7g` builder. This is the "instance type" knob for the cross-build. |
| `haiku:buildImage` | `HAIKU_BUILD_IMAGE` | `public.ecr.aws/ubuntu/ubuntu:24.04` | Must be arm64 Ubuntu 24.04 to match the validated bake host. |
| `haiku:buildtoolsRepo` / `Branch` | | haiku/buildtools | arm64 cross-tools sources. |
| `haiku:haikuOnEc2Repo` / `Branch` | | haiku/haiku-on-ec2 | provides `make-gpt-image.sh`. |
| `haiku:haikuRevision` | `HAIKU_REVISION` | `hrev59996` | stamped into the build + a tag. |
| `haiku:rootVolumeBytes` | `HAIKU_ROOT_VOLUME_BYTES` | `2147483648` | 2 GiB root volume. |
| `haiku:workBucketName` | (context only, **not** env) | (generated) | **leave empty.** See the warning below. |

> **Do not name the work bucket, and do not set `HAIKU_WORK_BUCKET`.** A bucket
> name is a replacement-triggering CloudFormation property, so changing it — in
> either direction, including setting it to the name the bucket already has —
> destroys and recreates the bucket, and the recreate collides with the retained
> original. This bit once: a deploy from a shell that did not export
> `HAIKU_WORK_BUCKET` renamed the bucket, which orphaned the `vmimport`
> authorization the Register stage depends on (it cannot import a snapshot from a
> bucket `vmimport` cannot read) and stranded the cross-tools cache in the old
> bucket. Nothing was lost — the bucket is `RETAIN` — but the next bake would have
> failed. An always-generated name cannot drift with whoever is deploying.
>
> Consequence: after a *first* deploy, read `WorkBucketName` from the stack
> outputs and authorize `vmimport` against that name, rather than choosing a name
> up front. There is a policy per bucket on the `vmimport` role
> (`vmimport-haiku-work`, `vmimport-haiku-work2`, ...) for exactly this reason.

| `haiku:builderInstanceId` | `HAIKU_BUILDER_INSTANCE` | the metal builder | SSM-managed peer/driver for the Test stage. |
| `haiku:testSubnetId` / `testSecurityGroupId` | `HAIKU_TEST_SUBNET` / `HAIKU_TEST_SG` | project subnet / `haiku-graviton-test` | where the perf gate boots the candidate. |
| `haiku:testInstanceType` | `HAIKU_TEST_TYPE` | `c7g.large` | **never a t-family type** — burstable CPU throttles once credits run out, which corrupts the CPU-cost-per-byte measurement. |
| `haiku:minReceiveMbps` / `minTransmitMbps` | `HAIKU_MIN_RX_MBPS` / `HAIKU_MIN_TX_MBPS` | `3000` / `2000` | regression floors, well under the measured ~4950/~4490. |

The **only** credential is the CodeConnections ARN — a reference to a connection
you authorize once in the console, not a secret in the tree. The Ed25519
`authorized_keys` handling is untouched: it stays baked in by
`UserBuildConfig`/`build-openssh-arm64.sh` exactly as in the recipe (IMDS merge
at boot, baked key as fallback).

---

## Prerequisites before a deploy would work

1. **CDK bootstrap** the account/region once: `cdk bootstrap aws://668984504585/us-west-2`.
2. **CodeConnections GitHub connection** — create in the console, complete the
   GitHub handshake, paste its ARN into `haiku:connectionArn`.
3. **`vmimport` role** — already exists in this account (`arn:aws:iam::668984504585:role/vmimport`).
   Its policy must allow reading the import objects. After the first synth/deploy
   you get the exact ARN as a stack output (`VmimportPolicyHint`); ensure the
   role's policy includes, for the work bucket:
   ```json
   { "Effect": "Allow",
     "Action": ["s3:GetObject", "s3:GetBucketLocation", "s3:GetBucketAcl"],
     "Resource": ["arn:aws:s3:::<workBucket>", "arn:aws:s3:::<workBucket>/import/*"] }
   ```
   (Do **not** let this app rewrite the shared `vmimport` role — authorize it out of band.)

---

## Deploying (left to the operator)

```sh
cd graviton/pipeline
npm install
npx cdk synth        # safe — no AWS calls, emits CloudFormation
npx cdk diff         # safe — compares against deployed state
# --- everything below creates/changes AWS resources; operator's call ---
npx cdk deploy \
  -c haiku:repoOwner=<you> \
  -c haiku:connectionArn=arn:aws:codeconnections:us-west-2:668984504585:connection/xxxx
```

A bake run is **manual**: `triggerOnPush` is off (bakes are expensive), so start
executions from the CodePipeline console/CLI. When the pipeline reaches
**Approve**, review the candidate AMI id (surfaced in the approval message as
`#{reg.AMI_ID}`) — boot-test it — then approve to run **Promote**.

### What deploying creates

- 1 **CodePipeline** (V2) with 5 stages.
- 3 **CodeBuild** projects (cross-build 2XLARGE arm64; register + promote SMALL arm64).
- 1 **S3** work bucket (raw image + cross-tools cache; retained on stack delete).
- 3 **CloudWatch Logs** groups (1-month retention).
- **IAM** roles for the pipeline + each CodeBuild project (see below) and the
  CDK-managed artifact bucket + pipeline role.

### Least privilege

- **CrossBuild** role: read/write only the `cache/*` prefix of the work bucket +
  its log group. No EC2/AMI permissions — it only compiles.
- **Register** role: read/write `import/*`; `ec2:ImportSnapshot`,
  `Describe{ImportSnapshotTasks,Snapshots,Images}`, `RegisterImage`, and
  `CreateTags`, all constrained to `us-west-2` via an `aws:RequestedRegion`
  condition. (These EC2 actions don't support resource-level scoping, so the
  region condition is the tightest available bound.)
- **Promote** role: only `ec2:DescribeImages` + `CreateTags` + `DeleteTags`,
  region-scoped. This is the sole role allowed to mutate the `canonical` tag.

### Rough cost

Dominated by the cross-build. `BUILD_GENERAL1_2XLARGE` arm64 is ~$0.20/min;
a cold build (cross-tools ~1h + two `jam` passes) is roughly **$25–40** of
CodeBuild, a warm build (cross-tools restored from S3) considerably less. Add
negligible S3 (a couple GB of raw image, expired after 14 days, plus the
cross-tools cache), the EBS snapshot created by the import, and near-zero
CodePipeline/Logs. The registered AMI + its snapshot incur ongoing EBS snapshot
storage (~$0.05/GB-month) until deregistered.

---

## Canonical promotion gating

Canonical is **never** set automatically. Register tags the new AMI
`candidate=true` only. A **ManualApproval** action sits between Register and
Promote; only after a human approves does the Promote stage run
`haiku-canonical promote <AMI_ID>`, which atomically strips `canonical=true`
from every prior holder and sets it on the new AMI, then `haiku-canonical check`
re-asserts the "exactly one canonical" invariant (failing the build if broken).

---

## Limits / what does not cleanly automate

These are inherent to the bake, not to this pipeline — flagged honestly:

- **Cross-tools ~1h build.** Mitigated by the S3 cross-tools cache (build once,
  restore thereafter), but the first run and any toolchain bump pays the ~1h.
- **Two-pass `jam`.** `build-openssh-arm64.sh` reads the `haiku`/`haiku_devel`
  package **staging dirs**, which only exist after `jam` has assembled the base
  packages. So the buildspec runs `jam @minimum-mmc` once to stage, cross-builds
  OpenSSH, injects `UserBuildConfig`, then runs `jam @minimum-mmc` again for the
  final image. The second pass mostly reuses cached objects, but it is real
  extra time and the ordering is load-bearing.
- **Full-package (browser/desktop) builds are NOT in scope here.** Per
  `docs/arm64-package-bootstrap.md`, the HaikuPorts arm64 set must be
  bootstrapped from source and is currently **blocked on a haikuporter
  chroot/permission issue** and needs a **KVM** builder to run haikuporter
  natively — CodeBuild containers have no `/dev/kvm`. This pipeline bakes
  **`@minimum-mmc` + the OpenSSH injection** (the actual shipping profile); a
  full-userland image would need a different, KVM-capable builder (e.g. a
  managed `c7g.metal` EC2 builder driven by SSM, which could be added as an
  alternate CrossBuild backend).
- **Build image assumptions.** The buildspec `apt-get`s the Haiku build deps on
  an arm64 Ubuntu 24.04 image; a curated CodeBuild arm64 image is Amazon Linux,
  so we pin an Ubuntu image from ECR Public to match the validated environment.
- **`import-snapshot` latency is unbounded-ish.** The register step polls for up
  to ~60 min; very large images or busy regions can exceed that and would need
  the poll budget raised.
- **virtio/ENA MSI-X hazard** (`docs/ssh-arm64-desktop-bake.md` gotcha 3) is a
  runtime property of the produced AMI, not something the pipeline can verify —
  boot-test before approving canonical.

## Note on `cdk synth`

If the CDK toolchain is unavailable in a given environment, the code is still
complete and coherent; run `npm install && npx cdk synth` where Node + network
are available. See the repo commit message for whether synth was exercised here.
