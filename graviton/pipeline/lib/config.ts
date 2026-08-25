import { Construct } from 'constructs';

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
  /** HAIKU_REVISION stamped into the build. */
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

  /**
   * Hardware performance gate (the Test stage, between Register and Approve).
   *
   * The gate boots the candidate AMI for real and measures it, so it needs to
   * know where to put the instance and who its traffic peer is. The peer is the
   * SSM-managed metal builder: it already holds the ssh key for Haiku nodes, the
   * nettput peer script and a jumbo-capable interface, so nothing has to be
   * installed anywhere for a measurement to happen.
   *
   * testInstanceType must not be a t-family instance: T instances throttle CPU
   * to a baseline once credits run out, and CPU cost per byte is half of what
   * the gate measures.
   *
   * The floors are regression thresholds, set far below the measured figures
   * (~4900 receive, ~3850 transmit on a c7g.large) on purpose. A gate that trips
   * on ordinary variance gets switched off, and then it guards nothing.
   */
  readonly builderInstanceId: string;
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

  // The account id is deliberately not written down in this tree -- it is a
  // public repository -- so it defaults to CDK_DEFAULT_ACCOUNT, which the CDK CLI
  // sets from the credentials you are already deploying with. That is the
  // idiomatic source, and it means `cdk synth`/`deploy` needs no extra
  // configuration to target the usual account. `-c haiku:account=` or
  // HAIKU_ACCOUNT still override it, and if none of the three is available ctx()
  // throws by name rather than synthesizing a stack with an empty account.
  const account = ctx(scope, 'haiku:account', 'HAIKU_ACCOUNT', process.env.CDK_DEFAULT_ACCOUNT);
  const region = ctx(scope, 'haiku:region', 'HAIKU_REGION', 'us-west-2');

  return {
    account,
    region,

    repoOwner: ctx(scope, 'haiku:repoOwner', 'HAIKU_REPO_OWNER'),
    repoName: ctx(scope, 'haiku:repoName', 'HAIKU_REPO_NAME', 'haiku'),
    branch: ctx(scope, 'haiku:branch', 'HAIKU_BRANCH', 'graviton'),
    connectionArn: ctx(scope, 'haiku:connectionArn', 'HAIKU_CONNECTION_ARN'),

    buildtoolsRepo: ctx(scope, 'haiku:buildtoolsRepo', 'HAIKU_BUILDTOOLS_REPO', 'https://github.com/haiku/buildtools.git'),
    buildtoolsBranch: ctx(scope, 'haiku:buildtoolsBranch', 'HAIKU_BUILDTOOLS_BRANCH', 'master'),
    haikuOnEc2Repo: ctx(scope, 'haiku:haikuOnEc2Repo', 'HAIKU_ON_EC2_REPO', 'https://github.com/felipedbene/haiku-on-ec2.git'),
    haikuOnEc2Branch: ctx(scope, 'haiku:haikuOnEc2Branch', 'HAIKU_ON_EC2_BRANCH', 'main'),

    buildComputeType: ctx(scope, 'haiku:buildComputeType', 'HAIKU_BUILD_COMPUTE', 'BUILD_GENERAL1_2XLARGE'),
    buildImage: ctx(scope, 'haiku:buildImage', 'HAIKU_BUILD_IMAGE', 'public.ecr.aws/ubuntu/ubuntu:24.04'),
    haikuRevision: ctx(scope, 'haiku:haikuRevision', 'HAIKU_REVISION', 'hrev59996'),

    amiNamePrefix: ctx(scope, 'haiku:amiNamePrefix', 'HAIKU_AMI_PREFIX', 'haiku-graviton'),
    rootVolumeBytes: ctx(scope, 'haiku:rootVolumeBytes', 'HAIKU_ROOT_VOLUME_BYTES', '2147483648'),

    workBucketName: workBucketName ? String(workBucketName) : undefined,

    // Not the pipeline's own work bucket: this is the bucket the builder's
    // instance role writes ssm-run output to, and it is named
    // haiku-graviton-<account>-<region> by convention. Derived from the account
    // resolved above so the name matches graviton/scripts/ssm-run without either
    // file naming the account. HAIKU_GRAVITON_BUCKET is the same override the
    // shell scripts honour, so one export retargets the whole toolchain.
    ssmOutBucketName: ctx(scope, 'haiku:ssmOutBucketName', 'HAIKU_GRAVITON_BUCKET',
      `haiku-graviton-${account}-${region}`),

    // No default: an instance id is not derivable, and hardcoding one would put
    // it in a public tree. The Test stage passes it through as
    // HG_BUILDER_INSTANCE, so supply it with -c haiku:builderInstanceId= or
    // HAIKU_BUILDER_INSTANCE at deploy time.
    builderInstanceId: ctx(scope, 'haiku:builderInstanceId', 'HAIKU_BUILDER_INSTANCE'),
    testSubnetId: ctx(scope, 'haiku:testSubnetId', 'HAIKU_TEST_SUBNET', 'subnet-0888405da8f10d1b2'),
    testSecurityGroupId: ctx(scope, 'haiku:testSecurityGroupId', 'HAIKU_TEST_SG', 'sg-0b99fabc8cb8bce88'),
    // Never a t-family instance: burstable CPU throttles to a baseline when
    // credits run out, which corrupts the CPU-cost-per-byte half of the result.
    testInstanceType: ctx(scope, 'haiku:testInstanceType', 'HAIKU_TEST_TYPE', 'c7g.large'),
    testKeyName: ctx(scope, 'haiku:testKeyName', 'HAIKU_TEST_KEY', 'haiku-uaf-ed25519'),
    minReceiveMbps: ctx(scope, 'haiku:minReceiveMbps', 'HAIKU_MIN_RX_MBPS', '3000'),
    minTransmitMbps: ctx(scope, 'haiku:minTransmitMbps', 'HAIKU_MIN_TX_MBPS', '2000'),
  };
}
