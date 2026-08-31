import * as cdk from 'aws-cdk-lib';
import { Construct } from 'constructs';
import * as codebuild from 'aws-cdk-lib/aws-codebuild';
import * as codepipeline from 'aws-cdk-lib/aws-codepipeline';
import * as cpactions from 'aws-cdk-lib/aws-codepipeline-actions';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as s3 from 'aws-cdk-lib/aws-s3';
import * as logs from 'aws-cdk-lib/aws-logs';
import { HaikuPipelineConfig } from './config';

export interface HaikuGravitonPipelineStackProps extends cdk.StackProps {
  readonly config: HaikuPipelineConfig;
}

/**
 * The Haiku-on-Graviton arm64 AMI bake pipeline.
 *
 * Stages (all shell logic is the existing vendored recipe, invoked by thin
 * buildspecs under graviton/pipeline/buildspecs):
 *
 *   Source       GitHub fork/branch (this Haiku tree) via CodeConnections.
 *   CrossBuild   CodeBuild (arm64/Graviton): configure --build-cross-tools,
 *                build-openssh-arm64.sh, inject UserBuildConfig, jam
 *                @minimum-mmc, then make-gpt-image.sh -> haiku-ec2.raw.
 *   Register     CodeBuild: upload raw to S3, ec2 import-snapshot, poll,
 *                register-image (arm64/uefi/ena/hvm/xvda), tag as a candidate
 *                (canonical is NOT set here). Exports AMI_ID.
 *   Test         CodeBuild: self-provision an ephemeral, bootstrapped Ubuntu
 *                arm64 peer, boot the candidate on a real c7g.large, measure
 *                throughput between them over SSM, then stop and start the
 *                candidate and require sshd to answer again. Fails unless it
 *                boots, negotiates MTU 9001, clears both throughput floors and
 *                survives the power cycle. Both ephemeral instances (candidate
 *                and peer) are always torn down.
 *   Approve      Manual approval gate — canonical promotion happens only after
 *                a human approves, now with the Test stage's measurements in
 *                hand rather than a promise that someone checked out of band.
 *   Promote      CodeBuild: graviton/scripts/haiku-canonical promote <AMI_ID>,
 *                enforcing the single-canonical invariant.
 */
export class HaikuGravitonPipelineStack extends cdk.Stack {
  constructor(scope: Construct, id: string, props: HaikuGravitonPipelineStackProps) {
    super(scope, id, props);
    const cfg = props.config;

    // ---------------------------------------------------------------------
    // Work bucket: raw disk image (import-snapshot source) + cross-tools cache.
    // The pre-existing `vmimport` role is granted read on the import/ prefix by a
    // bucket policy below, so no out-of-band authorization is needed. Deterministic
    // name optional.
    // ---------------------------------------------------------------------
    const workBucket = new s3.Bucket(this, 'WorkBucket', {
      bucketName: cfg.workBucketName || undefined,
      blockPublicAccess: s3.BlockPublicAccess.BLOCK_ALL,
      encryption: s3.BucketEncryption.S3_MANAGED,
      enforceSSL: true,
      versioned: false,
      removalPolicy: cdk.RemovalPolicy.RETAIN,
      lifecycleRules: [
        // Raw images are large and disposable once the AMI is registered.
        { prefix: 'import/', expiration: cdk.Duration.days(14) },
      ],
    });

    const IMPORT_PREFIX = 'import';
    const CACHE_PREFIX = 'cache';

    // Let EC2's import-snapshot read the raw image out of this bucket.
    //
    // ec2:ImportSnapshot runs as the account's pre-existing `vmimport` service
    // role, which needs s3:GetObject on the import/ prefix. That used to be an
    // out-of-band step: authorize each new work bucket by hand on the vmimport
    // role. It was the standing footgun of duplicating this pipeline, because a
    // CDK-auto-named bucket gets a FRESH random suffix on every re-create, so a
    // redeployed variant silently pointed at an unauthorized bucket and only
    // failed later, inside Register, as an opaque ImportSnapshot denial.
    //
    // Granted here as a BUCKET policy rather than by mutating the shared role:
    // the grant then lives and dies with the bucket, several variant stacks can
    // never conflict over one role's inline policies, and it cannot be silently
    // dropped the way a grant onto a CDK-imported (immutable) role can be. Same
    // account, so a resource policy alone is sufficient authorization.
    workBucket.addToResourcePolicy(new iam.PolicyStatement({
      sid: 'AllowVmimportReadRawImage',
      principals: [new iam.ArnPrincipal(
        `arn:aws:iam::${cdk.Stack.of(this).account}:role/vmimport`)],
      actions: ['s3:GetObject'],
      resources: [workBucket.arnForObjects(`${IMPORT_PREFIX}/*`)],
    }));
    workBucket.addToResourcePolicy(new iam.PolicyStatement({
      sid: 'AllowVmimportBucketMetadata',
      principals: [new iam.ArnPrincipal(
        `arn:aws:iam::${cdk.Stack.of(this).account}:role/vmimport`)],
      actions: ['s3:GetBucketLocation', 's3:ListBucket'],
      resources: [workBucket.bucketArn],
    }));

    // ---------------------------------------------------------------------
    // Common build environment: arm64 Ubuntu 24.04 (matches the validated bake
    // host). The cross-build needs a big box; the register/promote steps do not.
    // ---------------------------------------------------------------------
    const buildEnvironment: codebuild.BuildEnvironment = {
      buildImage: codebuild.LinuxArmBuildImage.fromDockerRegistry(cfg.buildImage),
      computeType: cfg.buildComputeType as codebuild.ComputeType,
      privileged: false,
    };
    const smallArmEnvironment: codebuild.BuildEnvironment = {
      buildImage: codebuild.LinuxArmBuildImage.AMAZON_LINUX_2_STANDARD_3_0,
      computeType: codebuild.ComputeType.SMALL,
      privileged: false,
    };

    const commonEnvVars: Record<string, codebuild.BuildEnvironmentVariable> = {
      AWS_DEFAULT_REGION: { value: cfg.region },
      HAIKU_REVISION: { value: cfg.haikuRevision },
      HG_SSH_DIR: { value: '/opt/haiku/ssh' },
      WORK_BUCKET: { value: workBucket.bucketName },
      IMPORT_PREFIX: { value: IMPORT_PREFIX },
      CACHE_PREFIX: { value: CACHE_PREFIX },
      BUILDTOOLS_REPO: { value: cfg.buildtoolsRepo },
      BUILDTOOLS_BRANCH: { value: cfg.buildtoolsBranch },
      HAIKU_ON_EC2_REPO: { value: cfg.haikuOnEc2Repo },
      HAIKU_ON_EC2_BRANCH: { value: cfg.haikuOnEc2Branch },
      ROOT_VOLUME_BYTES: { value: cfg.rootVolumeBytes },
      AMI_NAME_PREFIX: { value: cfg.amiNamePrefix },
    };

    // ---------------------------------------------------------------------
    // Stage 1 project: cross-build. Its role only needs the work bucket (for
    // the cross-tools cache) plus logs — it does no AWS control-plane work.
    // ---------------------------------------------------------------------
    const crossBuild = new codebuild.PipelineProject(this, 'CrossBuild', {
      projectName: `${cfg.amiNamePrefix}-cross-build`,
      environment: buildEnvironment,
      timeout: cdk.Duration.hours(6), // cross-tools ~1h + two jam passes.
      environmentVariables: commonEnvVars,
      buildSpec: codebuild.BuildSpec.fromSourceFilename('graviton/pipeline/buildspecs/cross-build.yml'),
      logging: {
        cloudWatch: {
          logGroup: new logs.LogGroup(this, 'CrossBuildLogs', {
            retention: logs.RetentionDays.ONE_MONTH,
            removalPolicy: cdk.RemovalPolicy.DESTROY,
          }),
        },
      },
    });
    workBucket.grantReadWrite(crossBuild, `${CACHE_PREFIX}/*`);
    // Read-only on the optional hpkg pool: the natively-built local packages
    // (translator/webkit build-feature hpkgs + the Rust toolchain) that
    // cross-build.yml syncs to HG_POOL_DIR and stages so jam turns those
    // features on. Populated out-of-band into s3://<workBucket>/hpkg-pool/;
    // absent -> the bake stays SSH-only. Read-only: the pool is an input.
    workBucket.grantRead(crossBuild, 'hpkg-pool/*');

    // ---------------------------------------------------------------------
    // Stage 2 project: import + register. Least-privilege EC2 for the disk
    // import / AMI registration / candidate tagging only. No canonical here.
    // ---------------------------------------------------------------------
    const register = new codebuild.PipelineProject(this, 'RegisterImage', {
      projectName: `${cfg.amiNamePrefix}-register`,
      environment: smallArmEnvironment,
      timeout: cdk.Duration.hours(2), // import-snapshot conversion dominates.
      environmentVariables: commonEnvVars,
      buildSpec: codebuild.BuildSpec.fromSourceFilename('graviton/pipeline/buildspecs/register-image.yml'),
      logging: {
        cloudWatch: {
          logGroup: new logs.LogGroup(this, 'RegisterLogs', {
            retention: logs.RetentionDays.ONE_MONTH,
            removalPolicy: cdk.RemovalPolicy.DESTROY,
          }),
        },
      },
    });
    workBucket.grantReadWrite(register, `${IMPORT_PREFIX}/*`);

    const regionCondition = { StringEquals: { 'aws:RequestedRegion': cfg.region } };

    // Disk import + registration. These EC2 actions do not support
    // resource-level scoping; constrain to the target region instead.
    register.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'ImportAndRegister',
        actions: [
          'ec2:ImportSnapshot',
          'ec2:DescribeImportSnapshotTasks',
          'ec2:DescribeSnapshots',
          'ec2:DescribeImages',
          'ec2:RegisterImage',
        ],
        resources: ['*'],
        conditions: regionCondition,
      }),
    );
    // Tagging: scope to the register region and to snapshot/image creation.
    register.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'TagCandidateResources',
        actions: ['ec2:CreateTags'],
        resources: ['*'],
        conditions: regionCondition,
      }),
    );

    // ---------------------------------------------------------------------
    // Dedicated least-privilege role + instance profile for the self-provisioned
    // perf-gate peer. This is deliberately NOT a broad shared role: the peer is an
    // ephemeral Ubuntu box the gate launches, so it gets exactly the four things
    // its first-boot bootstrap and the gate need and nothing else --
    //   - AmazonSSMManagedInstanceCore, so it registers as an SSM node and ssm-run
    //     can drive it (it is the traffic peer and the driver of every check);
    //   - read on the ONE Secrets Manager secret holding baron's private ssh key
    //     (the wildcard suffix is unavoidable: Secrets Manager appends a 6-char
    //     random suffix to the secret's ARN);
    //   - read on the ONE nettput tools object it copies at boot;
    //   - write on the ssm-out prefix, because ssm-run routes command output
    //     through s3://<ssmOutBucket>/ssm-out/* using the *instance's* role -- the
    //     inline SSM output is truncated at 24 KB, so without this the gate would
    //     silently fall back to a truncated copy.
    // The gate resolves the peer's Ubuntu AMI from a public SSM parameter, so no
    // AMI read grant is needed here.
    const peerRole = new iam.Role(this, 'PerfGatePeerRole', {
      roleName: `${cfg.amiNamePrefix}-perf-gate-peer`,
      assumedBy: new iam.ServicePrincipal('ec2.amazonaws.com'),
      managedPolicies: [
        iam.ManagedPolicy.fromAwsManagedPolicyName('AmazonSSMManagedInstanceCore'),
      ],
    });
    peerRole.addToPolicy(
      new iam.PolicyStatement({
        sid: 'FetchBaronSshKey',
        actions: ['secretsmanager:GetSecretValue'],
        resources: [
          `arn:aws:secretsmanager:${cfg.region}:${cfg.account}:secret:haiku-graviton/baron-ssh-key-*`,
        ],
      }),
    );
    peerRole.addToPolicy(
      new iam.PolicyStatement({
        sid: 'FetchNettputPeerScript',
        actions: ['s3:GetObject'],
        resources: [`arn:aws:s3:::${cfg.ssmOutBucketName}/tools/*`],
      }),
    );
    peerRole.addToPolicy(
      new iam.PolicyStatement({
        sid: 'WriteSsmCommandOutput',
        actions: ['s3:PutObject'],
        resources: [`arn:aws:s3:::${cfg.ssmOutBucketName}/ssm-out/*`],
      }),
    );
    const peerProfileName = `${cfg.amiNamePrefix}-perf-gate-peer`;
    const peerProfile = new iam.CfnInstanceProfile(this, 'PerfGatePeerProfile', {
      instanceProfileName: peerProfileName,
      roles: [peerRole.roleName],
    });

    // ---------------------------------------------------------------------
    // Stage 3 project: hardware regression gate. Self-provisions an ephemeral,
    // bootstrapped Ubuntu arm64 peer, boots the candidate AMI on a real Graviton
    // instance, measures throughput between them over SSM, then stops and starts
    // the candidate and requires sshd to answer again. Fails the pipeline if the
    // image does not boot, does not negotiate jumbo, has lost a large fraction of
    // its throughput, or does not survive a power cycle.
    //
    // The stop/start case was added after a data-loss bug shipped straight past
    // the throughput-only version of this gate: the image measured perfectly and
    // then could not survive being stopped, because the kernel's page writer
    // never flushed a short modified-page queue and the sshd host key came back
    // present-but-zeroed. Anything measured on a machine that never went down is
    // blind to whether the writes reached the disk.
    //
    // Before this stage existed the approval gate could only mean "I tested this
    // out of band and I vouch for it" -- nothing in the pipeline knew whether the
    // image worked. It sits before Approve so a human is deciding with numbers.
    // ---------------------------------------------------------------------
    const perfTest = new codebuild.PipelineProject(this, 'PerfTest', {
      projectName: `${cfg.amiNamePrefix}-perf-test`,
      environment: smallArmEnvironment,
      // Measured: a passing run is ~2.5 min (boot, two runs per direction, a
      // 32 s clean stop, then a second boot), against ~90 s before the stop/start
      // check. A run that fails the stop/start is ~14 min, because it spends the
      // full ACPI grace period stopping and then the whole sshd budget waiting for
      // a node that will never answer. The cycle is what costs the minutes and
      // there is no way to shorten it.
      //
      // 75 min rather than something snug, because every wait in haiku-perf-gate
      // is bounded by an explicit budget and those budgets add up to a ~46 min
      // ceiling (420 boot + 900 measure + 600 stop + 300 start + 420 boot again +
      // 90 cleanup). The timeout must sit above the script's own ceiling: if
      // CodeBuild kills the container first, the EXIT trap never runs and the
      // ephemeral instance leaks, which is exactly what the trap exists to
      // prevent. Better to let the script fail on its own terms and clean up.
      timeout: cdk.Duration.minutes(75),
      environmentVariables: {
        ...commonEnvVars,
        AWS_REGION: { value: cfg.region },
        // No persistent builder any more: leave HG_BUILDER_INSTANCE unset so the
        // gate self-provisions an ephemeral Ubuntu arm64 peer, bootstraps it, drives
        // it over SSM, and terminates it on exit (see graviton/scripts/haiku-perf-gate).
        // The peer is launched with the dedicated least-priv profile defined above,
        // its AMI resolved from a public Canonical SSM param, and it fetches the
        // baron key + nettput script from Secrets Manager and this bucket at boot.
        HG_PEER_INSTANCE_PROFILE: { value: peerProfileName },
        HG_PEER_AMI_PARAM: { value: cfg.peerAmiParam },
        HG_TOOLS_S3: { value: `s3://${cfg.ssmOutBucketName}/tools/nettput-peer.py` },
        HG_TEST_SUBNET: { value: cfg.testSubnetId },
        HG_TEST_SG: { value: cfg.testSecurityGroupId },
        HG_TEST_TYPE: { value: cfg.testInstanceType },
        HG_TEST_KEY: { value: cfg.testKeyName },
        HG_MIN_RX_MBPS: { value: cfg.minReceiveMbps },
        HG_MIN_TX_MBPS: { value: cfg.minTransmitMbps },
      },
      buildSpec: codebuild.BuildSpec.fromSourceFilename('graviton/pipeline/buildspecs/perf-test.yml'),
      logging: {
        cloudWatch: {
          logGroup: new logs.LogGroup(this, 'PerfTestLogs', {
            retention: logs.RetentionDays.ONE_MONTH,
            removalPolicy: cdk.RemovalPolicy.DESTROY,
          }),
        },
      },
    });
    // ssm-run routes command output through S3 because inline SSM output is
    // truncated at 24 KB. That object is written by the *builder's* instance
    // role, not by this project, and that role is scoped to exactly one bucket --
    // so the gate must read from that bucket rather than from the pipeline's work
    // bucket. Granting the work bucket instead would leave the gate silently
    // falling back to the truncated inline copy.
    const ssmOutBucket = s3.Bucket.fromBucketName(
      this, 'SsmOutBucket', cfg.ssmOutBucketName);
    ssmOutBucket.grantRead(perfTest, 'ssm-out/*');

    // Launch and describe ephemeral instances. The gate now launches TWO: the
    // candidate (from the AMI under test) and its own peer/driver (a bootstrapped
    // Ubuntu arm64 box), because the shared persistent builder was terminated and
    // there is no longer one to lean on. RunInstances does not usefully support
    // resource-level scoping for a freshly created instance, so constrain by
    // region and rely on the terminate policy below being narrow (tag-scoped to
    // exactly the two Names this gate uses).
    perfTest.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'LaunchEphemeralTestInstances',
        actions: [
          'ec2:RunInstances',
          'ec2:DescribeInstances',
          'ec2:DescribeImages',
          'ec2:CreateTags',
        ],
        resources: ['*'],
        conditions: regionCondition,
      }),
    );
    // The peer's Ubuntu arm64 AMI id is resolved at runtime from Canonical's public
    // SSM parameter rather than hardcoded. Public parameters live under the `aws`
    // service namespace with no account id in the ARN, so this is scoped to that
    // one public parameter path (empty account field is intentional).
    perfTest.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'ReadPeerAmiParam',
        actions: ['ssm:GetParameter'],
        resources: [
          `arn:aws:ssm:${cfg.region}::parameter${cfg.peerAmiParam}`,
        ],
      }),
    );
    // The peer carries the dedicated least-priv instance profile created above, and
    // attaching a profile at RunInstances requires iam:PassRole on the role inside
    // it. Scoped to exactly that role (whose ARN we hold directly, so no name-shape
    // assumption is needed) and constrained to being passed to EC2.
    perfTest.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'PassPeerInstanceProfileRole',
        actions: ['iam:PassRole'],
        resources: [peerRole.roleArn],
        conditions: {
          StringEquals: { 'iam:PassedToService': 'ec2.amazonaws.com' },
        },
      }),
    );
    // Termination is restricted to instances this gate created: both the
    // candidate (Name=haiku-perf-gate) and the self-provisioned peer
    // (Name=haiku-perf-gate-peer). A StringEquals list is an OR, so this still
    // grants terminate on nothing else. Without the ephemeral+Name condition an
    // unattended stage would hold the right to terminate any instance in the
    // account.
    perfTest.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'TerminateOnlyOwnEphemeralInstances',
        actions: ['ec2:TerminateInstances'],
        resources: ['*'],
        conditions: {
          StringEquals: {
            'aws:RequestedRegion': cfg.region,
            'ec2:ResourceTag/ephemeral': 'true',
            'ec2:ResourceTag/Name': ['haiku-perf-gate', 'haiku-perf-gate-peer'],
          },
        },
      }),
    );
    // Stopping and starting the candidate is the stop/start regression check:
    // Haiku once came back from a stop/start reachable by ping but with nothing
    // on :22, because the page writer never flushed a short modified-page queue
    // and the sshd host key returned present-but-zeroed. Throughput is measured
    // on a machine that never went down, so only a real power cycle sees it.
    //
    // Only the candidate is ever cycled (the peer is a passive traffic partner),
    // so this stays scoped to the candidate's Name alone -- narrower than the
    // terminate grant on purpose. An unattended stage that could stop any instance
    // in the account is the hazard being avoided; a stop is less final than a
    // terminate but would still disrupt anything else running.
    perfTest.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'CycleOnlyOwnEphemeralInstances',
        actions: ['ec2:StopInstances', 'ec2:StartInstances'],
        resources: ['*'],
        conditions: {
          StringEquals: {
            'aws:RequestedRegion': cfg.region,
            'ec2:ResourceTag/ephemeral': 'true',
            'ec2:ResourceTag/Name': 'haiku-perf-gate',
          },
        },
      }),
    );
    // Drive the peer over SSM: it is the traffic peer and the driver that reaches
    // the candidate. The peer's instance id is not known at deploy time (it is
    // launched fresh each run), so SendCommand is scoped by the peer's launch tags
    // instead of a fixed instance ARN -- exactly the ephemeral+Name+Project tuple
    // the gate stamps on it, so no other instance in the account is reachable.
    //
    // Split into two statements on purpose: the AWS-RunShellScript document has no
    // resource tags, so a single statement carrying the ssm:resourceTag/* instance
    // condition would fail to authorize the document half of the call and deny
    // every SendCommand. The document grant therefore stands alone (region-scoped).
    perfTest.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'SendCommandDocument',
        actions: ['ssm:SendCommand'],
        resources: [
          `arn:aws:ssm:${cfg.region}::document/AWS-RunShellScript`,
        ],
      }),
    );
    perfTest.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'SendCommandToEphemeralPeer',
        actions: ['ssm:SendCommand'],
        resources: [
          `arn:aws:ec2:${cfg.region}:${cfg.account}:instance/*`,
        ],
        conditions: {
          StringEquals: {
            'ssm:resourceTag/Project': 'haiku-graviton',
            'ssm:resourceTag/ephemeral': 'true',
            'ssm:resourceTag/Name': 'haiku-perf-gate-peer',
          },
        },
      }),
    );
    perfTest.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'ReadSsmCommandResults',
        actions: ['ssm:GetCommandInvocation', 'ssm:ListCommandInvocations'],
        resources: ['*'],
        conditions: regionCondition,
      }),
    );

    // ---------------------------------------------------------------------
    // Stage 4 project: canonical promotion. The only privileged tag mutation.
    // Tightly scoped to describe/create/delete tags in the target region.
    // ---------------------------------------------------------------------
    const promote = new codebuild.PipelineProject(this, 'Promote', {
      projectName: `${cfg.amiNamePrefix}-promote`,
      environment: smallArmEnvironment,
      timeout: cdk.Duration.minutes(15),
      environmentVariables: { ...commonEnvVars, AWS_REGION: { value: cfg.region } },
      buildSpec: codebuild.BuildSpec.fromSourceFilename('graviton/pipeline/buildspecs/promote.yml'),
      logging: {
        cloudWatch: {
          logGroup: new logs.LogGroup(this, 'PromoteLogs', {
            retention: logs.RetentionDays.ONE_MONTH,
            removalPolicy: cdk.RemovalPolicy.DESTROY,
          }),
        },
      },
    });
    promote.addToRolePolicy(
      new iam.PolicyStatement({
        sid: 'CanonicalTagManagement',
        actions: ['ec2:DescribeImages', 'ec2:CreateTags', 'ec2:DeleteTags'],
        resources: ['*'],
        conditions: regionCondition,
      }),
    );

    // ---------------------------------------------------------------------
    // Artifacts + pipeline wiring.
    // ---------------------------------------------------------------------
    const sourceArtifact = new codepipeline.Artifact('Source');
    const rawArtifact = new codepipeline.Artifact('RawImage');

    const sourceAction = new cpactions.CodeStarConnectionsSourceAction({
      actionName: 'GitHub_Source',
      owner: cfg.repoOwner,
      repo: cfg.repoName,
      branch: cfg.branch,
      connectionArn: cfg.connectionArn,
      output: sourceArtifact,
      triggerOnPush: false, // bakes are expensive; trigger manually / on demand.
      // Full clone: hand CodeBuild a real git clone instead of a zip artifact.
      // The Haiku tree relies on symlinks (e.g. src/libs/libsolv/solv/repo_haiku.h
      // -> ../ext/repo_haiku.h); the default zip artifact turns symlinks into
      // text files, which breaks the build (repo_haiku.h parsed as C). A clone
      // preserves them. Requires the build role to UseConnection (granted below).
      codeBuildCloneOutput: true,
    });

    const crossBuildAction = new cpactions.CodeBuildAction({
      actionName: 'CrossBuild_minimum_mmc',
      project: crossBuild,
      input: sourceArtifact,
      outputs: [rawArtifact],
    });

    const registerAction = new cpactions.CodeBuildAction({
      actionName: 'Import_and_Register',
      project: register,
      // Register needs both the raw image (build output) and the scripts/tools
      // that live in the source tree (haiku-canonical, import helper).
      input: sourceArtifact,
      extraInputs: [rawArtifact],
      variablesNamespace: 'reg',
    });

    const perfTestAction = new cpactions.CodeBuildAction({
      actionName: 'Hardware_Perf_Gate',
      project: perfTest,
      input: sourceArtifact,
      environmentVariables: {
        AMI_ID: { value: registerAction.variable('AMI_ID') },
      },
    });

    const approvalAction = new cpactions.ManualApprovalAction({
      actionName: 'Approve_Canonical_Promotion',
      additionalInformation:
        'The Test stage already booted AMI #{reg.AMI_ID} on real Graviton hardware and ' +
        'checked that it comes up, negotiates MTU 9001, meets both throughput floors, and ' +
        'survives a stop/start with sshd answering again -- its log has the measured ' +
        'numbers and the shutdown duration. Read the log for a WARNING line: a stop that ' +
        'consumed EC2\'s full ACPI grace period means the guest never handled the power ' +
        'button, which passes on purpose but is worth knowing before you promote. ' +
        'Approve to make this the single canonical=true image, which removes canonical ' +
        'from every prior holder.',
    });

    const promoteAction = new cpactions.CodeBuildAction({
      actionName: 'Promote_Canonical',
      project: promote,
      input: sourceArtifact,
      environmentVariables: {
        // Exported by the register stage's buildspec (exported-variables).
        AMI_ID: { value: registerAction.variable('AMI_ID') },
      },
    });

    new codepipeline.Pipeline(this, 'BakePipeline', {
      pipelineName: `${cfg.amiNamePrefix}-bake`,
      pipelineType: codepipeline.PipelineType.V2,
      restartExecutionOnUpdate: false,
      stages: [
        { stageName: 'Source', actions: [sourceAction] },
        { stageName: 'CrossBuild', actions: [crossBuildAction] },
        { stageName: 'Register', actions: [registerAction] },
        { stageName: 'Test', actions: [perfTestAction] },
        { stageName: 'Approve', actions: [approvalAction] },
        { stageName: 'Promote', actions: [promoteAction] },
      ],
    });

    // ---------------------------------------------------------------------
    // Outputs.
    // ---------------------------------------------------------------------
    new cdk.CfnOutput(this, 'WorkBucketName', {
      value: workBucket.bucketName,
      description:
        'S3 bucket for the raw image (import/) and cross-tools cache (cache/). ' +
        'vmimport is granted read on import/* by a bucket policy; no manual IAM step. ' +
        'Seed cache/cross-tools-arm64.tar.zst and hpkg-pool/ before the first bake.',
    });
  }
}
