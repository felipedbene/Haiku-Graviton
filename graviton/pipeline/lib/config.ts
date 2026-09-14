import { Construct } from 'constructs';

/**
 * Tags every DeBeOS-owned stack applies. `auto-stop`/`auto-delete` are the
 * account's spring-clean/idle-reaper protection keys (honored by the cleanup);
 * `Project` groups the fleet. Single source for both the bake and ops stacks.
 */
export const DEBEOS_TAGS: Record<string, string> = {
  Project: 'haiku-graviton',
  'auto-stop': 'no',
  'auto-delete': 'off',
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
  /** Target region (us-west-2). */
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
   * Published hpkg pool bucket (CDN-backed via packages.debene.dev), named
   * `haiku-graviton-hpkg-<account>` by convention. The bake archives haiku_devel
   * here; the ops build-wave publishes rebuilt packages here. Defaulted from
   * {@link account} so the id is never written down.
   */
  readonly publishBucketName: string;
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
  'account' | 'region' | 'ssmOutBucketName' | 'publishBucketName'
  | 'builderAmiParam' | 'canonicalAmiParam' | 'repoCloudFrontDistId'>;

export function loadSharedConfig(scope: Construct): SharedConfig {
  const account = ctx(scope, 'haiku:account', 'HAIKU_ACCOUNT', process.env.CDK_DEFAULT_ACCOUNT);
  const region = ctx(scope, 'haiku:region', 'HAIKU_REGION', 'us-west-2');
  // Optional: no ctx() (which throws when absent) -- an unset CDN id is valid
  // (the publish just skips invalidation), so read it directly and leave it
  // undefined when neither env nor context supplies it.
  const repoCloudFrontDistId = process.env.HAIKU_REPO_CF_DIST
    ?? (scope.node.tryGetContext('haiku:repoCloudFrontDistId') as string | undefined);
  return {
    account,
    region,
    ssmOutBucketName: ctx(scope, 'haiku:ssmOutBucketName', 'HAIKU_GRAVITON_BUCKET',
      `haiku-graviton-${account}-${region}`),
    publishBucketName: ctx(scope, 'haiku:publishBucketName', 'HAIKU_PUBLISH_BUCKET',
      `haiku-graviton-hpkg-${account}`),
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
