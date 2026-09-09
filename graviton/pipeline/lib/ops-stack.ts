import * as cdk from 'aws-cdk-lib';
import { Construct } from 'constructs';
import * as dynamodb from 'aws-cdk-lib/aws-dynamodb';
import * as ec2 from 'aws-cdk-lib/aws-ec2';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as lambda from 'aws-cdk-lib/aws-lambda';
import * as sfn from 'aws-cdk-lib/aws-stepfunctions';
import * as tasks from 'aws-cdk-lib/aws-stepfunctions-tasks';
import * as logs from 'aws-cdk-lib/aws-logs';
import * as codebuild from 'aws-cdk-lib/aws-codebuild';
import * as path from 'path';
import * as fs from 'fs';
import { SharedConfig, DEBEOS_TAGS } from './config';

export interface OpsStackProps extends cdk.StackProps {
  /** Shared DeBeOS config (buckets, SSM params) -- single source with the bake. */
  readonly config: SharedConfig;
  readonly tableName?: string;
}

/**
 * DeBeOS ops: the DynamoDB backlog+state table (Phase 1) AND the Step Functions
 * build consumer + task Lambdas + transient-builder IAM (Phase 2).
 *
 * The DevOps agent (under SOPs.md) decides what to build and starts an execution
 * per dependency chain; this state machine executes the chain reliably:
 *   Claim -> LaunchBuilder -> WaitForSSM -> [Map: build each pkg in order] ->
 *   [Publish? -> AcquirePublishLock -> IncrementalPublish -> ReleasePublishLock]
 *   -> Reap.  Reap runs on both success and failure.
 *
 * Publish is an INCREMENTAL add (haiku-repo-add) that folds the wave's harvested
 * hpkgs into the live repo without dropping the ~1885 already-published packages
 * -- see the RepoPublish CodeBuild project and the Publish? gate below for why it
 * is a CodeBuild job (needs a Linux host with the banked `package_repo` tool +
 * bulk S3), and how concurrent chains are serialized by a DynamoDB single-flight
 * lock so two incremental publishes never race the shared index.
 */
export class OpsStack extends cdk.Stack {
  public readonly table: dynamodb.Table;
  public readonly stateMachine: sfn.StateMachine;

  constructor(scope: Construct, id: string, props: OpsStackProps) {
    super(scope, id, props);

    // Spring-clean / idle-reaper protection (shared tag scheme). Applied
    // stack-wide so every CFN-managed resource inherits it -- notably the NAT
    // instance, which the idle-stopper would otherwise kill and break egress. The
    // TRANSIENT build instances are created at runtime by launch_builder (not
    // CFN), so they do NOT inherit this and remain reapable, as intended.
    for (const [k, v] of Object.entries(DEBEOS_TAGS)) cdk.Tags.of(this).add(k, v);

    // Buckets + builder AMI param come from the SHARED config (same source the
    // bake pipeline uses) -- no hardcoded account-scoped names here anymore.
    const workBucket = props.config.ssmOutBucketName;
    const publishBucket = props.config.publishBucketName;
    const builderAmiParam = props.config.builderAmiParam;

    // ---- Phase 1: state/backlog table ------------------------------------
    this.table = new dynamodb.Table(this, 'PackageState', {
      tableName: props.tableName ?? 'debeos-package-state',
      partitionKey: { name: 'pkg', type: dynamodb.AttributeType.STRING },
      billingMode: dynamodb.BillingMode.PAY_PER_REQUEST,
      pointInTimeRecoverySpecification: { pointInTimeRecoveryEnabled: true },
      removalPolicy: cdk.RemovalPolicy.RETAIN,
    });
    this.table.addGlobalSecondaryIndex({
      indexName: 'by-build-state',
      partitionKey: { name: 'build_state', type: dynamodb.AttributeType.STRING },
      sortKey: { name: 'queued_at', type: dynamodb.AttributeType.NUMBER },
      projectionType: dynamodb.ProjectionType.ALL,
    });

    // ---- transient builder identity --------------------------------------
    // Role the launched Graviton Haiku builder assumes: SSM-managed + read deps
    // and write hpkgs/logs to the pools (the on-instance debeos-ssm-agent uses
    // this to `s3 cp`).
    const builderRole = new iam.Role(this, 'BuilderRole', {
      assumedBy: new iam.ServicePrincipal('ec2.amazonaws.com'),
      managedPolicies: [
        iam.ManagedPolicy.fromAwsManagedPolicyName('AmazonSSMManagedInstanceCore'),
      ],
    });
    for (const b of [workBucket, publishBucket]) {
      builderRole.addToPolicy(new iam.PolicyStatement({
        actions: ['s3:GetObject', 's3:PutObject', 's3:ListBucket'],
        resources: [`arn:aws:s3:::${b}`, `arn:aws:s3:::${b}/*`],
      }));
    }
    const builderProfile = new iam.CfnInstanceProfile(this, 'BuilderProfile', {
      roles: [builderRole.roleName],
    });

    // ---- hardened build VPC ----------------------------------------------
    // Dedicated VPC for the transient builders. Builders sit in PRIVATE_WITH_EGRESS
    // subnets (no public IP, no inbound); a single NAT (one AZ, testing-cost)
    // carries upstream source-tarball fetches, while S3 (deps, hpkg pool, SSM
    // command output) and the SSM control plane go via endpoints -- off the NAT.
    // Managed NAT *gateway* (~$32/mo) rather than a NAT instance. We ran a
    // t4g.nano fck-nat instance (~$3/mo) here originally -- source-tarball egress
    // for a testing fleet doesn't need the managed NAT's throughput/HA -- but a
    // long-lived Linux host carries an OS-patching SLA: it was flagged RED and
    // auto-escalated by the fleet-patching program (EC2PA-144185). A managed NAT
    // gateway has no host to patch, so it never trips that program. The ~$29/mo
    // delta buys the elimination of that standing compliance toil. Single gateway
    // in one AZ (private subnets in other AZs route to it cross-AZ; fine at this
    // scale) -- CDK's default provider with natGateways:1.
    const vpc = new ec2.Vpc(this, 'BuildVpc', {
      // 3 AZs so spot builders diversify across capacity pools (one instance per
      // launch, but the launcher picks a subnet per instance -> concurrent
      // builders spread across AZs, cutting spot-interruption correlation).
      maxAzs: 3,
      natGateways: 1,
      subnetConfiguration: [
        { name: 'public', subnetType: ec2.SubnetType.PUBLIC, cidrMask: 24 },
        { name: 'builders', subnetType: ec2.SubnetType.PRIVATE_WITH_EGRESS, cidrMask: 22 },
      ],
    });
    // A managed NAT gateway needs no security-group rule: the private (builder)
    // subnets route out through it via their route tables (CDK wires this from
    // natGateways:1 + PRIVATE_WITH_EGRESS). Outbound is allowed by default.

    // S3 GATEWAY endpoint (free) -> hpkg pool, deps, and SSM command output stay
    // off the NAT. This is the one endpoint worth keeping: it carries the bulk
    // data (hpkgs, gzipped logs) and costs nothing.
    //
    // The SSM/logs INTERFACE endpoints were dropped deliberately (~$29/mo): the
    // builders already have NAT egress, so the SSM control plane and CloudWatch
    // logs ride the NAT instead. Only worth adding back for a fully NAT-less
    // (isolated) design.
    vpc.addGatewayEndpoint('S3Endpoint', {
      service: ec2.GatewayVpcEndpointAwsService.S3,
    });

    const builderSg = new ec2.SecurityGroup(this, 'BuilderSg', {
      vpc, allowAllOutbound: true, description: 'DeBeOS native builder (egress only)',
    });
    // All builder subnets (one per AZ) -> the launcher spreads spot across them.
    const builderSubnetIds = vpc.selectSubnets({
      subnetType: ec2.SubnetType.PRIVATE_WITH_EGRESS,
    }).subnetIds;

    // EC2 Instance Connect Endpoint: secure SSH into the PRIVATE builders (no
    // bastion, no public IP). Tunnel your existing key over the AWS API:
    //   aws ec2-instance-connect open-tunnel --instance-id <iid> \
    //     --instance-connect-endpoint-id <eice> --local-port 2222
    //   ssh -p 2222 -i ~/.ssh/haiku-ed25519 user@localhost
    // No hourly cost (data-processing only). Handy for debugging a wedged build.
    const eiceSg = new ec2.SecurityGroup(this, 'EiceSg', {
      vpc, allowAllOutbound: false, description: 'DeBeOS EC2 Instance Connect Endpoint',
    });
    eiceSg.addEgressRule(builderSg, ec2.Port.tcp(22), 'EICE to builders SSH');
    builderSg.addIngressRule(eiceSg, ec2.Port.tcp(22), 'SSH via Instance Connect Endpoint');
    const eice = new ec2.CfnInstanceConnectEndpoint(this, 'Eice', {
      subnetId: builderSubnetIds[0],
      securityGroupIds: [eiceSg.securityGroupId],
      preserveClientIp: false,
    });

    // ---- task Lambdas ----------------------------------------------------
    const lambdasDir = path.join(__dirname, '..', 'ops-lambdas');
    const mkFn = (id: string, handler: string, extraEnv: Record<string, string> = {}) =>
      new lambda.Function(this, id, {
        runtime: lambda.Runtime.PYTHON_3_12,
        architecture: lambda.Architecture.ARM_64,
        code: lambda.Code.fromAsset(lambdasDir),
        handler,
        timeout: cdk.Duration.seconds(60),
        logRetention: logs.RetentionDays.THREE_MONTHS,
        environment: {
          TABLE: this.table.tableName,
          BUILDER_AMI_PARAM: builderAmiParam,
          ...extraEnv,
        },
      });

    const claimFn = mkFn('ClaimFn', 'claim.handler');
    const launchFn = mkFn('LaunchFn', 'launch_builder.handler', {
      SUBNET_IDS: cdk.Fn.join(',', builderSubnetIds),
      SG_ID: builderSg.securityGroupId,
      INSTANCE_PROFILE_ARN: builderProfile.attrArn,
      INSTANCE_TYPE: 'c8g.2xlarge',
      // Builders size their root per launch (overridable per wave via
      // `disk_gib`) instead of inheriting the AMI's baked snapshot size, so the
      // builder AMI can stay lean. DeBeOS grows BFS to fill the volume on boot.
      BUILDER_DISK_GIB: '200',
    });
    const waitSsmFn = mkFn('WaitSsmFn', 'wait_ssm.handler');
    const buildFn = mkFn('BuildFn', 'run_ssm.build', { WORK_BUCKET: workBucket });
    const pollFn = mkFn('PollFn', 'poll_ssm.handler');
    const recordFn = mkFn('RecordFn', 'record.handler');
    const reapFn = mkFn('ReapFn', 'reap.handler');

    // IAM per Lambda (least privilege).
    this.table.grantReadWriteData(claimFn);
    this.table.grantReadWriteData(recordFn);
    launchFn.addToRolePolicy(new iam.PolicyStatement({
      // DescribeImages: the launcher reads the builder AMI's RootDeviceName so its
      // per-launch volume mapping targets the real root device (a mismatched
      // DeviceName is ignored and the builder silently boots at the baked size).
      actions: ['ec2:RunInstances', 'ec2:CreateTags', 'ec2:DescribeImages'],
      resources: ['*'],
    }));
    launchFn.addToRolePolicy(new iam.PolicyStatement({
      actions: ['ssm:GetParameter'], resources: ['*'],
    }));
    launchFn.addToRolePolicy(new iam.PolicyStatement({
      actions: ['iam:PassRole'], resources: [builderRole.roleArn],
    }));
    for (const fn of [waitSsmFn, pollFn]) {
      fn.addToRolePolicy(new iam.PolicyStatement({
        actions: ['ssm:DescribeInstanceInformation', 'ssm:GetCommandInvocation'],
        resources: ['*'],
      }));
    }
    for (const fn of [buildFn, waitSsmFn]) {
      fn.addToRolePolicy(new iam.PolicyStatement({
        actions: ['ssm:SendCommand'], resources: ['*'],  // waitSsmFn fires the readiness probe
      }));
    }
    reapFn.addToRolePolicy(new iam.PolicyStatement({
      actions: ['ec2:TerminateInstances'],
      resources: ['*'],
      conditions: { StringEquals: { 'aws:ResourceTag/Name': 'haiku-native-builder' } },
    }));
    // BuildFn lists the DeBeOS overlay in S3 (to stage recipe bumps onto the tree).
    buildFn.addToRolePolicy(new iam.PolicyStatement({
      actions: ['s3:GetObject', 's3:ListBucket'],
      resources: [`arn:aws:s3:::${workBucket}`, `arn:aws:s3:::${workBucket}/*`],
    }));

    // ---- state machine ---------------------------------------------------
    const li = (id: string, fn: lambda.Function) =>
      new tasks.LambdaInvoke(this, id, { lambdaFunction: fn, payloadResponseOnly: true });

    const reap = (id: string) => li(id, reapFn);

    // Reap-on-failure terminal path (shared catch target).
    const reapFail = reap('ReapOnFailure').next(new sfn.Fail(this, 'BuildWaveFailed'));

    // Per-package iterator (dep order => MaxConcurrency 1). Poll loop: the
    // hours-long wait lives in SFN (Wait), not a Lambda.
    const startBuild = li('StartBuild', buildFn);
    const waitBuild = new sfn.Wait(this, 'WaitBuild', {
      time: sfn.WaitTime.duration(cdk.Duration.seconds(60)),
    });
    const pollBuild = li('PollBuild', pollFn);
    const recordBuilt = li('RecordBuilt', recordFn);   // record.py branches on ok
    const recordFailed = li('RecordFailed', recordFn);
    const buildDone = new sfn.Choice(this, 'BuildDone?')
      .when(sfn.Condition.booleanEquals('$.done', false), waitBuild)  // loop
      .when(sfn.Condition.booleanEquals('$.ok', true), recordBuilt)
      .otherwise(recordFailed);
    startBuild.next(waitBuild);
    waitBuild.next(pollBuild);
    pollBuild.next(buildDone);

    const buildMap = new sfn.Map(this, 'BuildChain', {
      itemsPath: sfn.JsonPath.stringAt('$.claimed_items'),
      maxConcurrency: 1,
      // Each iteration gets the item (pkg,target) + instance_id + run_id.
      // TABLE / WORK_BUCKET come from Lambda env, so the agent's StartExecution
      // input stays minimal: just {chain, target}.
      itemSelector: {
        'pkg.$': '$$.Map.Item.Value.pkg',
        'target.$': '$$.Map.Item.Value.target',
        'instance_id.$': '$.instance_id',
        'run_id.$': '$$.Execution.Name',
      },
      resultPath: sfn.JsonPath.DISCARD, // keep parent context for publish/reap
    });
    buildMap.itemProcessor(startBuild);

    // ---- incremental repo publish (CodeBuild + haiku-repo-add) -----------
    // The wave's built hpkgs are harvested to s3://<workBucket>/hpkg/arm64/ by
    // haiku-nativebuild. Publishing them to the LIVE repo must be INCREMENTAL:
    // haiku-repo-add pulls the full published pool, adds the new package(s),
    // rebuilds the index over the UNION, and uploads it -- so the ~1885 already-
    // published packages are never dropped. (The retired on-builder
    // `haiku-repo-publish all` rebuilt the index from ONLY the builder's local
    // subset and would have clobbered the pool -- see the Publish? gate.)
    //
    // Why CodeBuild and not the Haiku builder: haiku-repo-add needs a Linux host
    // with (a) the `package`/`package_repo` host tools and (b) bulk S3 (it syncs
    // the whole ~1885-object pool). Haiku has neither awscli nor those Linux
    // binaries; the builder can only single-file `s3 cp`. So the publish runs in
    // an arm64 Ubuntu 24.04 CodeBuild container -- the same environment the
    // cross-build bakes the host tools on -- which fetches the banked host tools
    // and runs haiku-repo-add UNCHANGED. This replaces the retired ephemeral
    // Ubuntu peer (haiku-repo-publish-ephemeral) with a managed, SFN-native
    // (.sync) job; the incremental primitive is identical.
    const repoAddB64 = fs.readFileSync(
      path.join(__dirname, '..', '..', 'scripts', 'haiku-repo-add')).toString('base64');
    const publishProject = new codebuild.Project(this, 'RepoPublish', {
      projectName: 'debeos-repo-publish',
      description: "Incremental DeBeOS repo publish: haiku-repo-add over the wave's harvested hpkgs.",
      timeout: cdk.Duration.hours(1),
      // One publish at a time (defence-in-depth with the DynamoDB lock below):
      // two concurrent index rebuilds would race the shared repo.
      concurrentBuildLimit: 1,
      environment: {
        // Match the cross-build's arm64 Ubuntu 24.04 so the banked (glibc-linked)
        // Linux host tools run here without an ABI mismatch.
        buildImage: codebuild.LinuxArmBuildImage.fromDockerRegistry('public.ecr.aws/ubuntu/ubuntu:24.04'),
        computeType: codebuild.ComputeType.MEDIUM,
      },
      environmentVariables: {
        HG_REPO_S3: { value: `s3://${publishBucket}/debeos-repo/arm64` }, // live repo (packages.debene.dev/arm64)
        HARVEST_S3: { value: `s3://${workBucket}/hpkg/arm64` },           // wave harvest (durable)
        INCOMING_BASE: { value: `s3://${workBucket}/hpkg/arm64-incoming` }, // per-run disposable snapshot
        HG_CF_DIST: { value: props.config.repoCloudFrontDistId ?? '' },   // '' => skip invalidation
        ARCH: { value: 'arm64' },
      },
      buildSpec: codebuild.BuildSpec.fromObject({
        version: '0.2',
        phases: {
          install: {
            commands: [
              'export DEBIAN_FRONTEND=noninteractive',
              'apt-get update -qq',
              'apt-get install -y --no-install-recommends curl unzip ca-certificates file',
              'if ! command -v aws >/dev/null 2>&1; then curl -sSLf https://awscli.amazonaws.com/awscli-exe-linux-aarch64.zip -o /tmp/awscliv2.zip && (cd /tmp && unzip -q awscliv2.zip && ./aws/install); fi',
            ],
          },
          build: {
            commands: [
              'set -eo pipefail',
              // Resolve the banked Linux host tools from the bake pipeline's
              // WorkBucket -- discovered via its CFN output so no generated bucket
              // name is written into this tree (same as the ephemeral publisher).
              "WB=\"$(aws cloudformation describe-stacks --stack-name HaikuGravitonBakePipeline --query \"Stacks[0].Outputs[?OutputKey=='WorkBucketName'].OutputValue | [0]\" --output text)\"",
              'test -n "$WB" -a "$WB" != None || { echo "cannot resolve bake WorkBucket for host tools (HaikuGravitonBakePipeline)" >&2; exit 1; }',
              'HT="s3://$WB/cache/host-tools"',
              'install -d /opt/haiku-tools /opt/haiku-tools/lib /opt/haiku-tools/data',
              'aws s3 cp "$HT/package" /opt/haiku-tools/package',
              'aws s3 cp "$HT/package_repo" /opt/haiku-tools/package_repo',
              'chmod +x /opt/haiku-tools/package /opt/haiku-tools/package_repo',
              'aws s3 cp "$HT/lib" /opt/haiku-tools/lib --recursive --only-show-errors',
              'aws s3 cp "$HT/data" /opt/haiku-tools/data --recursive --only-show-errors',
              'export LD_LIBRARY_PATH=/opt/haiku-tools/lib',
              'export HAIKU_BUILD_SYSTEM_DATA_DIRECTORY=/opt/haiku-tools/data',
              'export PKG_TOOL=/opt/haiku-tools/package PACKAGE_REPO_TOOL=/opt/haiku-tools/package_repo',
              'ldd /opt/haiku-tools/package_repo 2>&1 | grep -qi "not found" && { echo "banked host tools missing shared libs" >&2; ldd /opt/haiku-tools/package_repo >&2; exit 1; } || true',
              // Snapshot the wave harvest into a per-run, DISPOSABLE incoming
              // prefix. haiku-repo-add clears its HG_INCOMING_S3 on success, so we
              // point it at this copy -- the durable harvest is left intact, and a
              // concurrent builder still harvesting into it is never disturbed.
              'RUN="${RUN:-$CODEBUILD_BUILD_NUMBER}"',
              'INCOMING="$INCOMING_BASE/$RUN"',
              'echo "== snapshot $HARVEST_S3 -> $INCOMING =="',
              'aws s3 sync "$HARVEST_S3/" "$INCOMING/" --only-show-errors --exclude "*" --include "*.hpkg" --exclude "haiku.hpkg" --exclude "haiku-*" --exclude "haiku_*"',
              'n="$(aws s3 ls "$INCOMING/" | grep -c "[.]hpkg$" || true)"',
              'if [ "${n:-0}" -eq 0 ]; then echo "no harvested hpkg to publish; nothing to do"; exit 0; fi',
              'echo "== incremental-add $n harvested package(s) into $HG_REPO_S3 =="',
              `printf %s '${repoAddB64}' | base64 -d > /tmp/haiku-repo-add`,
              'export HG_INCOMING_S3="$INCOMING"',
              'bash /tmp/haiku-repo-add',
            ],
          },
        },
      }),
    });
    // Publish job IAM: read+write the live repo pool, read the harvest + write
    // the per-run incoming snapshot (both in workBucket), read the banked host
    // tools from the bake WorkBucket, and invalidate the repo's CDN index paths.
    publishProject.addToRolePolicy(new iam.PolicyStatement({
      actions: ['s3:GetObject', 's3:PutObject', 's3:DeleteObject', 's3:ListBucket'],
      resources: [
        `arn:aws:s3:::${publishBucket}`, `arn:aws:s3:::${publishBucket}/*`,
        `arn:aws:s3:::${workBucket}`, `arn:aws:s3:::${workBucket}/*`,
      ],
    }));
    publishProject.addToRolePolicy(new iam.PolicyStatement({
      actions: ['s3:GetObject', 's3:ListBucket'],  // banked host tools live in the bake WorkBucket
      resources: ['arn:aws:s3:::*bakepipeline*workbucket*', 'arn:aws:s3:::*bakepipeline*workbucket*/*'],
    }));
    publishProject.addToRolePolicy(new iam.PolicyStatement({
      actions: ['cloudformation:DescribeStacks'], resources: ['*'],  // discover the bake WorkBucket name
    }));
    publishProject.addToRolePolicy(new iam.PolicyStatement({
      actions: ['cloudfront:CreateInvalidation'],
      resources: [props.config.repoCloudFrontDistId
        ? `arn:aws:cloudfront::${this.account}:distribution/${props.config.repoCloudFrontDistId}`
        : '*'],
    }));

    const reapSuccess = reap('ReapOnSuccess');
    reapSuccess.next(new sfn.Succeed(this, 'BuildWaveDone'));

    // Serialize concurrent chains' publishes with a DynamoDB single-flight lock:
    // an incremental add is read-modify-write over the shared repo index, so two
    // running at once would lost-update (the second's --delete sync could drop
    // the first's just-added packages). Acquire is a conditional PutItem; if the
    // lock is held, DynamoDB.ConditionalCheckFailedException triggers a backoff
    // retry until the holder releases. The lock is a sentinel row in the existing
    // state table. (Release runs on BOTH the success and publish-failure paths;
    // a whole-execution abort between acquire and release would strand the lock
    // -- recover by deleting the row -- an accepted, rare, non-silent case.)
    const lockPk = '__repo_publish_lock__';
    const acquireLock = new tasks.DynamoPutItem(this, 'AcquirePublishLock', {
      table: this.table,
      item: {
        pkg: tasks.DynamoAttributeValue.fromString(lockPk),
        held_by: tasks.DynamoAttributeValue.fromString(sfn.JsonPath.stringAt('$$.Execution.Name')),
        acquired_at: tasks.DynamoAttributeValue.fromString(sfn.JsonPath.stringAt('$$.State.EnteredTime')),
      },
      conditionExpression: 'attribute_not_exists(pkg)',
      resultPath: sfn.JsonPath.DISCARD,
    });
    acquireLock.addRetry({
      errors: ['DynamoDB.ConditionalCheckFailedException'],
      interval: cdk.Duration.seconds(15),
      backoffRate: 2,
      maxAttempts: 30,
      maxDelay: cdk.Duration.seconds(120),
    });
    const runPublish = new tasks.CodeBuildStartBuild(this, 'IncrementalPublish', {
      project: publishProject,
      integrationPattern: sfn.IntegrationPattern.RUN_JOB,  // .sync: SFN waits for the build
      environmentVariablesOverride: {
        RUN: {
          value: sfn.JsonPath.stringAt('$$.Execution.Name'),
          type: codebuild.BuildEnvironmentVariableType.PLAINTEXT,
        },
      },
      resultPath: sfn.JsonPath.DISCARD,
    });
    const releaseLock = new tasks.DynamoDeleteItem(this, 'ReleasePublishLock', {
      table: this.table,
      key: { pkg: tasks.DynamoAttributeValue.fromString(lockPk) },
      resultPath: sfn.JsonPath.DISCARD,
    });
    const releaseLockOnFailure = new tasks.DynamoDeleteItem(this, 'ReleasePublishLockOnFailure', {
      table: this.table,
      key: { pkg: tasks.DynamoAttributeValue.fromString(lockPk) },
      resultPath: sfn.JsonPath.DISCARD,
    });
    // Retry the release on any error: a DeleteItem of a fixed key is idempotent,
    // so a transient DynamoDB throttle/5xx must not be allowed to strand the
    // single-flight lock -- a stranded '__repo_publish_lock__' would block every
    // later wave on AcquirePublishLock for the full acquire budget (~56 min) and
    // then fail it, until an operator deletes the row by hand.
    for (const t of [releaseLock, releaseLockOnFailure]) {
      t.addRetry({
        errors: ['States.ALL'],
        interval: cdk.Duration.seconds(2),
        backoffRate: 2,
        maxAttempts: 8,
        maxDelay: cdk.Duration.seconds(30),
      });
    }
    acquireLock.next(runPublish);
    runPublish.next(releaseLock);
    releaseLock.next(reapSuccess);
    // Publish failed -> free the lock (don't wedge the next wave) then reap+fail.
    runPublish.addCatch(releaseLockOnFailure, { resultPath: '$.error' });
    releaseLockOnFailure.next(reapFail);

    // WaitForSSM registration loop.
    const checkSsm = li('CheckSSM', waitSsmFn);
    const waitSsm = new sfn.Wait(this, 'WaitForSSM', {
      time: sfn.WaitTime.duration(cdk.Duration.seconds(30)),
    });
    const ssmReady = new sfn.Choice(this, 'SSMReady?')
      .when(sfn.Condition.booleanEquals('$.ssm_ready', true), buildMap)
      .otherwise(waitSsm);
    checkSsm.next(ssmReady);
    waitSsm.next(checkSsm);

    // Publish is OPT-IN (default build-only): publish only when the input
    // explicitly sets publish=true. When set, it runs the INCREMENTAL add above
    // (AcquirePublishLock -> IncrementalPublish -> ReleasePublishLock), which
    // folds the wave's harvested hpkgs into the live repo without shrinking the
    // published pool -- safe to leave on. When unset, skip straight to reap so a
    // build-only wave never touches the repo.
    const publishGate = new sfn.Choice(this, 'Publish?')
      .when(sfn.Condition.and(
              sfn.Condition.isPresent('$.publish'),
              sfn.Condition.booleanEquals('$.publish', true)),
            acquireLock)
      .otherwise(reapSuccess);
    buildMap.next(publishGate);

    const claim = li('ClaimBatch', claimFn);
    const launch = li('LaunchBuilder', launchFn);
    claim.next(launch);
    launch.next(checkSsm);

    // Reap on any failure from LaunchBuilder onward. Catches attach to the
    // top-level graph states; a failure inside the Map iterator is caught at the
    // Map itself (iterator states live in their own sub-graph and cannot target
    // a parent-graph catch).
    // (IncrementalPublish has its own catch -> ReleasePublishLock -> reapFail, so
    // a publish failure frees the lock before failing; it is not in this list.)
    for (const s of [launch, checkSsm, buildMap, acquireLock]) {
      s.addCatch(reapFail, { resultPath: '$.error' });
    }

    this.stateMachine = new sfn.StateMachine(this, 'BuildWave', {
      stateMachineName: 'debeos-build-wave',
      definitionBody: sfn.DefinitionBody.fromChainable(claim),
      stateMachineType: sfn.StateMachineType.STANDARD, // multi-hour builds
      timeout: cdk.Duration.hours(24),
      tracingEnabled: true,
    });

    // ---- least-privilege OPERATOR role (prompt-injection blast-radius bound) --
    // The debeos-devops agent should assume THIS, never Admin. Even a fully
    // hijacked agent is boxed to: read/write the state table, start/stop a
    // build-wave, read the pools + AMI param. It CANNOT publish, delete,
    // exfiltrate, launch/terminate instances, or touch anything else. Overlay
    // recipe commits go through git CR (human review), not this role.
    const operatorRole = new iam.Role(this, 'OperatorRole', {
      roleName: 'debeos-operator',
      assumedBy: new iam.AccountPrincipal(this.account),
      description: 'Least-priv identity for the debeos-devops agent (no Admin).',
    });
    this.table.grantReadWriteData(operatorRole);
    operatorRole.addToPolicy(new iam.PolicyStatement({
      actions: ['states:StartExecution', 'states:DescribeExecution', 'states:StopExecution'],
      resources: [
        this.stateMachine.stateMachineArn,
        `arn:aws:states:${this.region}:${this.account}:execution:${this.stateMachine.stateMachineName}:*`,
      ],
    }));
    operatorRole.addToPolicy(new iam.PolicyStatement({
      actions: ['s3:GetObject', 's3:ListBucket'],
      resources: [
        `arn:aws:s3:::${workBucket}`, `arn:aws:s3:::${workBucket}/*`,
        `arn:aws:s3:::${publishBucket}`, `arn:aws:s3:::${publishBucket}/*`,
      ],
    }));
    operatorRole.addToPolicy(new iam.PolicyStatement({
      actions: ['ssm:GetParameter', 'ssm:DescribeInstanceInformation'],
      resources: ['*'],
    }));

    // ---- outputs the DevOps agent needs to start an execution ------------
    new cdk.CfnOutput(this, 'OperatorRoleArn', { value: operatorRole.roleArn });
    new cdk.CfnOutput(this, 'TableName', { value: this.table.tableName });
    new cdk.CfnOutput(this, 'StateMachineArn', { value: this.stateMachine.stateMachineArn });
    new cdk.CfnOutput(this, 'BuilderInstanceProfileArn', { value: builderProfile.attrArn });
    new cdk.CfnOutput(this, 'BuilderSecurityGroupId', { value: builderSg.securityGroupId });
    new cdk.CfnOutput(this, 'WorkBucket', { value: workBucket });
    new cdk.CfnOutput(this, 'InstanceConnectEndpointId', { value: eice.attrId });
  }
}
