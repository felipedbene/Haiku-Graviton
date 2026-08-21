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
 *   Approve      Manual approval gate — canonical promotion happens only after
 *                a human approves.
 *   Promote      CodeBuild: graviton/scripts/haiku-canonical promote <AMI_ID>,
 *                enforcing the single-canonical invariant.
 */
export class HaikuGravitonPipelineStack extends cdk.Stack {
  constructor(scope: Construct, id: string, props: HaikuGravitonPipelineStackProps) {
    super(scope, id, props);
    const cfg = props.config;

    // ---------------------------------------------------------------------
    // Work bucket: raw disk image (import-snapshot source) + cross-tools cache.
    // The pre-existing `vmimport` role must be able to read the import/ prefix
    // (authorize it out of band — see README). Deterministic name optional.
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
    // Stage 3 project: canonical promotion. The only privileged tag mutation.
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

    const approvalAction = new cpactions.ManualApprovalAction({
      actionName: 'Approve_Canonical_Promotion',
      additionalInformation:
        'Approve to make the freshly-registered AMI (#{reg.AMI_ID}) the single canonical=true image. ' +
        'This removes canonical from every prior holder.',
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
        'Authorize the pre-existing vmimport role to read s3://<bucket>/import/* (see README).',
    });
    new cdk.CfnOutput(this, 'VmimportPolicyHint', {
      value: `arn:aws:s3:::${workBucket.bucketName}/${IMPORT_PREFIX}/*`,
      description: 'Add s3:GetObject/GetBucketLocation on this ARN to the vmimport role policy.',
    });
  }
}
