import { Construct } from 'constructs';
import * as ssm from 'aws-cdk-lib/aws-ssm';

/**
 * Tags every DeBeOS-owned stack applies. `auto-stop`/`auto-delete` are the
 * account's spring-clean/idle-reaper protection keys (honored by the cleanup);
 * `Project` groups the fleet. Single source for both the bake and ops stacks.
 *
 * `auto-delete` must be `'no'`, not `'off'`: SpringClean's manifest asks for the
 * value "no", so `'off'` protects nothing while reading exactly like protection.
 * Older DeBeOS resources used `'off'` and that is how the live canonical
 * ami-05cf86c1c19e3ca58 ended up reapable and had to be protected by hand (see
 * haiku-canonical's promote path, #520). This value predated that discovery.
 *
 * Scope note: these are CDK *stack* tags, so they land on the durable pipeline
 * and ops infrastructure, NOT on the AMIs the bake scripts register at runtime.
 * Baked-image retention is deliberately the opposite policy -- only the current
 * canonical and the live builder AMI are protected, and superseded ones are
 * released so `cmd_prune` and the reaper can retire them.
 */
export const DEBEOS_TAGS: Record<string, string> = {
  Project: 'haiku-graviton',
  'auto-stop': 'no',
  'auto-delete': 'no',
};

/**
 * All tunable inputs for the bake pipeline. Every field is sourced from CDK
 * context (see cdk.json) with an environment-variable fallback, so nothing is
 * hardcoded in the stack and no secrets live in the tree.
 */
export interface HaikuPipelineConfig {
  /**
   * Target AWS account. Defaults to `CDK_DEFAULT_ACCOUNT` — the account the
   * deploying credentials belong to — so the id is never written down here.
   */
  readonly account: string;
  /**
   * Target region. Defaults to {@link DEFAULT_REGION}; see {@link resolveRegion}
   * for how AWS_REGION / AWS_DEFAULT_REGION and the context key feed it.
   */
  readonly region: string;

  /** GitHub owner/org of the Haiku fork. */
  readonly repoOwner: string;
  /** GitHub repository name (the Haiku source tree). */
  readonly repoName: string;
  /** Branch to bake (this fork's `graviton`). */
  readonly branch: string;
  /**
   * ARN of a CodeConnections (formerly CodeStar) connection to GitHub. Created
   * once in the console; it is the only "credential" and it is a reference, not
   * a secret value.
   */
  readonly connectionArn: string;

  /** Haiku buildtools repo (arm64 cross-tools sources). */
  readonly buildtoolsRepo: string;
  readonly buildtoolsBranch: string;
  /** haiku-on-ec2 repo (provides scripts/make-gpt-image.sh). */
  readonly haikuOnEc2Repo: string;
  readonly haikuOnEc2Branch: string;

  /** CodeBuild compute type for the cross-build (e.g. BUILD_GENERAL1_2XLARGE). */
  readonly buildComputeType: string;
  /**
   * CodeBuild compute type for the Register stage (import + register).
   *
   * Register does almost no CPU work — it uploads the ~20 GiB raw image to S3
   * and then polls `import-snapshot` — so this knob buys network bandwidth, not
   * compute. The upload measured ~130 MiB/s (~1.1 Gbps) on BUILD_GENERAL1_SMALL,
   * which is that container's network baseline, NOT client serialization:
   * s5cmd's parallel multipart made no difference (issue #152). A larger compute
   * type raises the network baseline, so the default is BUILD_GENERAL1_LARGE.
   * NOTE: the ARM environment supports SMALL / LARGE / XLARGE / 2XLARGE only —
   * there is no MEDIUM for arm_container, so LARGE is the smallest bump. Set this
   * back to BUILD_GENERAL1_SMALL to trade the ~1–2 min upload saving for the
   * lower per-minute rate.
   */
  readonly registerComputeType: string;
  /** arm64 Ubuntu 24.04 build image matching the validated bake environment. */
  readonly buildImage: string;
  /**
   * HAIKU_REVISION stamped into the build.
   *
   * Deliberately PINNED (not derived from HEAD): this string becomes the
   * `r1~beta6_<revision>` package version line, and every already-published
   * hpkg in the DeBeOS pool carries the pinned value. Changing it would reorder
   * `pkgman` version comparisons against the published pool (see issue #201 and
   * the note at build/jam/images/definitions/minimum). The pipeline clone is
   * also tag-shallow, so `determine_haiku_revision` cannot derive a stable
   * value on its own. The user-visible DeBeOS identity is carried separately in
   * AboutSystem; source builds (full clone) get a HEAD-tracking
   * debeos-r<n>-g<sha> from determine_haiku_revision. Moving this to a
   * DeBeOS-native version requires the repo/version-model work (#82/#92).
   */
  readonly haikuRevision: string;

  /** AMI name prefix (a timestamp is appended at register time). */
  readonly amiNamePrefix: string;
  /** Root EBS volume size in bytes for the raw image / registered AMI. */
  readonly rootVolumeBytes: string;

  /**
   * Optional deterministic name for the S3 work bucket (raw image + cross-tools
   * cache). Set this so the pre-existing `vmimport` role can be authorized
   * against it out of band. Empty => CDK generates a name.
   */
  readonly workBucketName?: string;

  /**
   * Bucket that `ssm-run` routes remote command output through, written by the
   * *builder's* instance role rather than by this app. Named
   * `haiku-graviton-<account>-<region>` by convention and defaulted from
   * {@link account}/{@link region}, so the account id is never spelled out in
   * this tree. Override with `HAIKU_GRAVITON_BUCKET` (the same variable the
   * scripts under `graviton/` honour) or `-c haiku:ssmOutBucketName=`.
   */
  readonly ssmOutBucketName: string;

  // ---- shared DeBeOS constants (consumed by BOTH the bake and the ops stack) --
  /**
   * Published hpkg pool bucket (CDN-backed via packages.debene.dev). The ops
   * build-wave publishes rebuilt packages here. There is ONE pool for every
   * region, so this is an explicit override only (`HAIKU_PUBLISH_BUCKET` or
   * `-c haiku:publishBucketName=`). Unset, a stack reads it from the
   * {@link REPO_PARAMS} SSM parameter at deploy time; see {@link repoBucketName}.
   */
  readonly publishBucketName?: string;
  /**
   * Region of {@link publishBucketName}, handed to the publish and promote jobs
   * as HAIKU_REPO_REGION. Override only (`HAIKU_REPO_REGION` or
   * `-c haiku:repoRegion=`); unset, it comes from SSM like the bucket.
   */
  readonly repoRegion?: string;
  /** SSM param holding the BUILDER AMI id (toolchain+haikuporter image). */
  readonly builderAmiParam: string;
  /** SSM param holding the CANONICAL (lean runtime) AMI id. */
  readonly canonicalAmiParam: string;

  /**
   * CloudFront distribution fronting the DeBeOS package repo
   * (packages.debene.dev). The build-wave's incremental publish invalidates the
   * mutable index paths (`/<arch>/repo`, `repo.info`, `repo.sha256`) on it after
   * an add, so a `pkgman refresh` sees the new package without waiting out the
   * CDN TTL. OPTIONAL and NOT written down here (the id embeds no account but is
   * still environment-specific): set `-c haiku:repoCloudFrontDistId=` or
   * `HAIKU_REPO_CF_DIST`. Unset => the publish skips invalidation (index serves
   * stale until TTL); it never blocks a publish.
   */
  readonly repoCloudFrontDistId?: string;

  /**
   * Hardware performance gate (the Test stage, between Register and Approve).
   *
   * The gate boots the candidate AMI for real and measures it, so it needs to
   * know where to put the instance and who its traffic peer is. There is no
   * persistent builder any more (the shared metal one was terminated): the gate
   * self-provisions an ephemeral peer, drives it over SSM, and terminates it on
   * exit. That peer is a freshly bootstrapped UBUNTU arm64 host (its AMI resolved
   * at runtime from Canonical's public SSM parameter, {@link peerAmiParam}), NOT
   * the Haiku canonical AMI -- every peer-side command in the gate is Linux
   * (`sudo -u ubuntu`, an ssh client at /home/ubuntu/.ssh/haiku-ed25519, `python3
   * .../nettput-peer.py`), so the peer has to be Linux. The instance profile it is
   * launched with is created by the stack (a dedicated least-priv role, not a
   * shared broad one): it carries AmazonSSMManagedInstanceCore plus read on the
   * baron ssh-key secret and the nettput tools object and write on the ssm-out
   * prefix, so it is defined in the stack rather than configured here.
   *
   * testInstanceType must not be a t-family instance: T instances throttle CPU
   * to a baseline once credits run out, and CPU cost per byte is half of what
   * the gate measures.
   *
   * The floors are regression thresholds, set far below the measured figures
   * (~4900 receive, ~3850 transmit on a c7g.large) on purpose. A gate that trips
   * on ordinary variance gets switched off, and then it guards nothing.
   */
  readonly peerAmiParam: string;
  /** VPC (by Name tag) the perf-gate launches its peer and candidate into. */
  readonly testVpcName: string;
  /**
   * Optional overrides. EMPTY BY DESIGN: the stack looks the subnet up in
   * testVpcName and creates its own self-referencing security group, so there is
   * no id to go stale. These previously defaulted to a hardcoded subnet/SG that a
   * VPC consolidation deleted, which broke the Test stage on every pipeline and
   * only surfaced ~25 minutes into a bake. Set them to pin a one-off run.
   */
  readonly testSubnetId: string;
  readonly testSecurityGroupId: string;
  readonly testInstanceType: string;
  readonly testKeyName: string;
  readonly minReceiveMbps: string;
  readonly minTransmitMbps: string;
}

/**
 * The region DeBeOS first ran in. It is the region whose resource names are left
 * unsuffixed by {@link regionalName}, and the region whose DebeosOpsStack owns the
 * package-state table, so every name and resource that exists there stays exactly
 * as it is. It is NOT the default region any more: see {@link DEFAULT_REGION}.
 */
export const HOME_REGION = 'us-west-2';

/**
 * The region a deploy targets when nothing names one. DeBeOS is moving to
 * us-east-2, so that is where an unqualified deploy lands. This is deliberately a
 * separate constant from {@link HOME_REGION}: moving the naming home as well would
 * rename every IAM role in both regions, and renaming a live role replaces it.
 */
export const DEFAULT_REGION = 'us-east-2';

/**
 * SSM String parameters naming the package repo: one bucket and one region for
 * every publisher, wherever it runs. Create both in each region a stack deploys
 * to; a stack deployed without them fails, rather than publishing somewhere else.
 */
export const REPO_PARAMS = {
  bucket: '/debeos/repo/bucket',
  region: '/debeos/repo/region',
} as const;

/**
 * The repo bucket for a stack: the explicit override if one was given, otherwise
 * a reference to {@link REPO_PARAMS}.bucket that CloudFormation resolves at
 * deploy time (not `valueFromLookup`, which would cache it in cdk.context.json).
 * `scope` must be inside a Stack.
 */
export function repoBucketName(scope: Construct, cfg: Pick<SharedConfig, 'publishBucketName'>): string {
  return cfg.publishBucketName
    ?? ssm.StringParameter.valueForStringParameter(scope, REPO_PARAMS.bucket);
}

/** The repo region for a stack, resolved like {@link repoBucketName}. */
export function repoRegionName(scope: Construct, cfg: Pick<SharedConfig, 'repoRegion'>): string {
  return cfg.repoRegion
    ?? ssm.StringParameter.valueForStringParameter(scope, REPO_PARAMS.region);
}

/**
 * Region resolution, most specific first: HAIKU_REGION, then `-c haiku:region`,
 * then the standard AWS_REGION / AWS_DEFAULT_REGION (only if it is one of
 * {@link DEBEOS_REGIONS}), then {@link DEFAULT_REGION}.
 * CDK_DEFAULT_REGION is deliberately NOT consulted: it is whatever the deploying
 * profile's config says (us-east-1 on a stock workstation), and a stack must not
 * silently move region because of a laptop's ~/.aws/config.
 */
export function resolveRegion(scope: Construct): string {
  const fromCtx = scope.node.tryGetContext('haiku:region') as string | undefined;
  const explicit = process.env.HAIKU_REGION || fromCtx;
  if (explicit) return explicit;
  const ambient = process.env.AWS_REGION || process.env.AWS_DEFAULT_REGION;
  if (!ambient) return DEFAULT_REGION;
  // A region picked up from the ambient AWS variables -- which a shell may carry
  // for unrelated work -- must be one DeBeOS already runs in. Anything else would
  // quietly stand up whole new pipelines there; name it explicitly instead.
  if (!DEBEOS_REGIONS.includes(ambient)) {
    throw new Error(
      `AWS_REGION/AWS_DEFAULT_REGION is ${ambient}, which is not a DeBeOS region ` +
      `(${DEBEOS_REGIONS.join(', ')}). To deploy there on purpose, set HAIKU_REGION ` +
      `or -c haiku:region=${ambient}.`);
  }
  return ambient;
}

/** Regions DeBeOS runs in; an ambient AWS_REGION outside this list is refused. */
export const DEBEOS_REGIONS: readonly string[] = ['us-west-2', 'us-east-2'];

/**
 * Name for a resource whose namespace is account-GLOBAL (IAM roles and instance
 * profiles) but which every regional copy of a stack creates. In the home region
 * the name is unchanged -- renaming a live role is a replacement -- and anywhere
 * else it gains a `-<region>` suffix so a second region's deploy does not collide
 * with the first one's role.
 */
export function regionalName(base: string, region: string): string {
  return region === HOME_REGION ? base : `${base}-${region}`;
}

function ctx(scope: Construct, key: string, envKey: string, fallback?: string): string {
  const fromCtx = scope.node.tryGetContext(key);
  const value = process.env[envKey] ?? fromCtx ?? fallback;
  if (value === undefined) {
    throw new Error(`Missing required config '${key}': set it in cdk.json context or env ${envKey}`);
  }
  return String(value);
}

/**
 * The subset of config shared by the bake pipeline AND the ops build stack:
 * account/region, the two buckets, and the AMI SSM params. The ops stack needs
 * only these, so it can synth/deploy WITHOUT the bake-only inputs
 * (connectionArn, peerAmiParam, ...).
 */
export type SharedConfig = Pick<HaikuPipelineConfig,
  'account' | 'region' | 'ssmOutBucketName' | 'publishBucketName' | 'repoRegion'
  | 'builderAmiParam' | 'canonicalAmiParam' | 'repoCloudFrontDistId'>;

export function loadSharedConfig(scope: Construct): SharedConfig {
  const account = ctx(scope, 'haiku:account', 'HAIKU_ACCOUNT', process.env.CDK_DEFAULT_ACCOUNT);
  const region = resolveRegion(scope);
  // Optional: no ctx() (which throws when absent) -- an unset CDN id is valid
  // (the publish just skips invalidation), so read it directly and leave it
  // undefined when neither env nor context supplies it.
  const repoCloudFrontDistId = process.env.HAIKU_REPO_CF_DIST
    ?? (scope.node.tryGetContext('haiku:repoCloudFrontDistId') as string | undefined);
  // Optional the same way: unset means "read it from SSM in the stack".
  // An empty env var counts as unset, so it cannot hide a -c value.
  const optional = (key: string, envKey: string): string | undefined => {
    const fromEnv = process.env[envKey];
    const v = fromEnv !== undefined && fromEnv !== '' ? fromEnv : scope.node.tryGetContext(key);
    return v === undefined || v === '' ? undefined : String(v);
  };
  return {
    account,
    region,
    ssmOutBucketName: ctx(scope, 'haiku:ssmOutBucketName', 'HAIKU_GRAVITON_BUCKET',
      `haiku-graviton-${account}-${region}`),
    publishBucketName: optional('haiku:publishBucketName', 'HAIKU_PUBLISH_BUCKET'),
    repoRegion: optional('haiku:repoRegion', 'HAIKU_REPO_REGION'),
    builderAmiParam: ctx(scope, 'haiku:builderAmiParam', 'HAIKU_BUILDER_AMI_PARAM',
      '/haiku-graviton/builder-ami-id'),
    canonicalAmiParam: ctx(scope, 'haiku:canonicalAmiParam', 'HAIKU_CANONICAL_AMI_PARAM',
      '/haiku-graviton/canonical-ami-id'),
    repoCloudFrontDistId: repoCloudFrontDistId || undefined,
  };
}

export function loadConfig(scope: Construct): HaikuPipelineConfig {
  // Deliberately NOT read from the environment. A bucket name is a
  // replacement-triggering property, so whether HAIKU_WORK_BUCKET happened to be
  // exported in the deploying shell decided whether the work bucket survived the
  // deploy. It did not survive once: the rename orphaned the vmimport
  // authorization the Register stage depends on, and stranded the cross-tools
  // cache. Reading it only from context means the value lives in the tree, is
  // reviewed with the rest of the change, and cannot differ between two people
  // deploying the same commit.
  const workBucketName = scope.node.tryGetContext('haiku:workBucketName');

  // Shared fields (account/region/buckets/AMI params) come from loadSharedConfig
  // -- the single source the ops stack also uses.
  const shared = loadSharedConfig(scope);

  return {
    ...shared,

    repoOwner: ctx(scope, 'haiku:repoOwner', 'HAIKU_REPO_OWNER'),
    repoName: ctx(scope, 'haiku:repoName', 'HAIKU_REPO_NAME', 'haiku'),
    branch: ctx(scope, 'haiku:branch', 'HAIKU_BRANCH', 'graviton'),
    connectionArn: ctx(scope, 'haiku:connectionArn', 'HAIKU_CONNECTION_ARN'),

    buildtoolsRepo: ctx(scope, 'haiku:buildtoolsRepo', 'HAIKU_BUILDTOOLS_REPO', 'https://github.com/felipedbene/buildtools.git'),
    buildtoolsBranch: ctx(scope, 'haiku:buildtoolsBranch', 'HAIKU_BUILDTOOLS_BRANCH', 'master'),
    haikuOnEc2Repo: ctx(scope, 'haiku:haikuOnEc2Repo', 'HAIKU_ON_EC2_REPO', 'https://github.com/felipedbene/haiku-on-ec2.git'),
    haikuOnEc2Branch: ctx(scope, 'haiku:haikuOnEc2Branch', 'HAIKU_ON_EC2_BRANCH', 'main'),

    buildComputeType: ctx(scope, 'haiku:buildComputeType', 'HAIKU_BUILD_COMPUTE', 'BUILD_GENERAL1_2XLARGE'),
    // LARGE, not SMALL: Register is bandwidth-bound on the raw-image upload, and
    // ARM has no MEDIUM (see registerComputeType). #152.
    registerComputeType: ctx(scope, 'haiku:registerComputeType', 'HAIKU_REGISTER_COMPUTE', 'BUILD_GENERAL1_LARGE'),
    buildImage: ctx(scope, 'haiku:buildImage', 'HAIKU_BUILD_IMAGE', 'public.ecr.aws/ubuntu/ubuntu:24.04'),
    haikuRevision: ctx(scope, 'haiku:haikuRevision', 'HAIKU_REVISION', 'hrev59996'),

    amiNamePrefix: ctx(scope, 'haiku:amiNamePrefix', 'HAIKU_AMI_PREFIX', 'haiku-graviton'),
    rootVolumeBytes: ctx(scope, 'haiku:rootVolumeBytes', 'HAIKU_ROOT_VOLUME_BYTES', '2147483648'),

    workBucketName: workBucketName ? String(workBucketName) : undefined,
    // account/region/ssmOutBucketName/publishBucketName/builderAmiParam/
    // canonicalAmiParam are provided by ...shared above.

    // Public SSM parameter the self-provisioned peer's Ubuntu arm64 AMI id is
    // resolved from at runtime (never a hardcoded ami-id). Canonical publishes and
    // rotates this; the default is Ubuntu 24.04 LTS arm64, gp3-backed. The peer's
    // instance profile is NOT configured here -- the stack creates a dedicated
    // least-priv role/profile for it (SSM + baron-secret read + nettput read +
    // ssm-out write) and passes its name to the gate.
    peerAmiParam: ctx(scope, 'haiku:peerAmiParam', 'HAIKU_PEER_AMI_PARAM',
      '/aws/service/canonical/ubuntu/server/24.04/stable/current/arm64/hvm/ebs-gp3/ami-id'),
    testVpcName: ctx(scope, 'haiku:testVpcName', 'HAIKU_TEST_VPC_NAME',
      'DebeosOpsStack/BuildVpc'),
    testSubnetId: ctx(scope, 'haiku:testSubnetId', 'HAIKU_TEST_SUBNET', ''),
    testSecurityGroupId: ctx(scope, 'haiku:testSecurityGroupId', 'HAIKU_TEST_SG', ''),
    // Never a t-family instance: burstable CPU throttles to a baseline when
    // credits run out, which corrupts the CPU-cost-per-byte half of the result.
    testInstanceType: ctx(scope, 'haiku:testInstanceType', 'HAIKU_TEST_TYPE', 'c7g.large'),
    testKeyName: ctx(scope, 'haiku:testKeyName', 'HAIKU_TEST_KEY', 'haiku-uaf-ed25519'),
    minReceiveMbps: ctx(scope, 'haiku:minReceiveMbps', 'HAIKU_MIN_RX_MBPS', '3000'),
    minTransmitMbps: ctx(scope, 'haiku:minTransmitMbps', 'HAIKU_MIN_TX_MBPS', '2000'),
  };
}
