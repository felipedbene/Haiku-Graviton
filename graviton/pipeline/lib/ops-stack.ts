import * as cdk from 'aws-cdk-lib';
import { Construct } from 'constructs';
import * as dynamodb from 'aws-cdk-lib/aws-dynamodb';
import * as ec2 from 'aws-cdk-lib/aws-ec2';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as lambda from 'aws-cdk-lib/aws-lambda';
import * as sfn from 'aws-cdk-lib/aws-stepfunctions';
import * as tasks from 'aws-cdk-lib/aws-stepfunctions-tasks';
import * as logs from 'aws-cdk-lib/aws-logs';
import * as path from 'path';
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
 *   PublishRepo -> Reap.  Reap runs on both success and failure.
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
    // NAT *instance* (t4g.nano, ~$3/mo) instead of a managed NAT gateway
    // (~$32/mo) -- source-tarball egress for a testing fleet doesn't need the
    // managed NAT's throughput/HA. Single instance in one AZ (private subnets in
    // other AZs route to it cross-AZ; fine at this scale).
    const natProvider = ec2.NatProvider.instanceV2({
      instanceType: ec2.InstanceType.of(ec2.InstanceClass.T4G, ec2.InstanceSize.NANO),
    });
    const vpc = new ec2.Vpc(this, 'BuildVpc', {
      // 3 AZs so spot builders diversify across capacity pools (one instance per
      // launch, but the launcher picks a subnet per instance -> concurrent
      // builders spread across AZs, cutting spot-interruption correlation).
      maxAzs: 3,
      natGateways: 1,
      natGatewayProvider: natProvider,
      subnetConfiguration: [
        { name: 'public', subnetType: ec2.SubnetType.PUBLIC, cidrMask: 24 },
        { name: 'builders', subnetType: ec2.SubnetType.PRIVATE_WITH_EGRESS, cidrMask: 22 },
      ],
    });
    // Let the private (builder) subnets route out through the NAT instance.
    natProvider.securityGroup.addIngressRule(
      ec2.Peer.ipv4(vpc.vpcCidrBlock), ec2.Port.allTraffic(),
      'builder subnets to NAT instance egress',
    );

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
    });
    const waitSsmFn = mkFn('WaitSsmFn', 'wait_ssm.handler');
    const buildFn = mkFn('BuildFn', 'run_ssm.build', { WORK_BUCKET: workBucket });
    const publishFn = mkFn('PublishFn', 'run_ssm.publish', { WORK_BUCKET: workBucket });
    const pollFn = mkFn('PollFn', 'poll_ssm.handler');
    const recordFn = mkFn('RecordFn', 'record.handler');
    const reapFn = mkFn('ReapFn', 'reap.handler');

    // IAM per Lambda (least privilege).
    this.table.grantReadWriteData(claimFn);
    this.table.grantReadWriteData(recordFn);
    launchFn.addToRolePolicy(new iam.PolicyStatement({
      actions: ['ec2:RunInstances', 'ec2:CreateTags'],
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
    for (const fn of [buildFn, publishFn, waitSsmFn]) {
      fn.addToRolePolicy(new iam.PolicyStatement({
        actions: ['ssm:SendCommand'], resources: ['*'],  // waitSsmFn fires the readiness probe
      }));
    }
    reapFn.addToRolePolicy(new iam.PolicyStatement({
      actions: ['ec2:TerminateInstances'],
      resources: ['*'],
      conditions: { StringEquals: { 'aws:ResourceTag/Name': 'haiku-native-builder' } },
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

    // Publish once after the chain, then reap on success.
    const startPublish = li('StartPublish', publishFn);
    const waitPublish = new sfn.Wait(this, 'WaitPublish', {
      time: sfn.WaitTime.duration(cdk.Duration.seconds(45)),
    });
    const pollPublish = li('PollPublish', pollFn);
    const reapSuccess = reap('ReapOnSuccess');
    const publishDone = new sfn.Choice(this, 'PublishDone?')
      .when(sfn.Condition.booleanEquals('$.done', false), waitPublish)  // loop
      .otherwise(reapSuccess);
    startPublish.next(waitPublish);
    waitPublish.next(pollPublish);
    pollPublish.next(publishDone);
    reapSuccess.next(new sfn.Succeed(this, 'BuildWaveDone'));

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
    buildMap.next(startPublish);

    const claim = li('ClaimBatch', claimFn);
    const launch = li('LaunchBuilder', launchFn);
    claim.next(launch);
    launch.next(checkSsm);

    // Reap on any failure from LaunchBuilder onward. Catches attach to the
    // top-level graph states; a failure inside the Map iterator is caught at the
    // Map itself (iterator states live in their own sub-graph and cannot target
    // a parent-graph catch).
    for (const s of [launch, checkSsm, buildMap, startPublish, pollPublish]) {
      s.addCatch(reapFail, { resultPath: '$.error' });
    }

    this.stateMachine = new sfn.StateMachine(this, 'BuildWave', {
      stateMachineName: 'debeos-build-wave',
      definitionBody: sfn.DefinitionBody.fromChainable(claim),
      stateMachineType: sfn.StateMachineType.STANDARD, // multi-hour builds
      timeout: cdk.Duration.hours(24),
      tracingEnabled: true,
    });

    // ---- outputs the DevOps agent needs to start an execution ------------
    new cdk.CfnOutput(this, 'TableName', { value: this.table.tableName });
    new cdk.CfnOutput(this, 'StateMachineArn', { value: this.stateMachine.stateMachineArn });
    new cdk.CfnOutput(this, 'BuilderInstanceProfileArn', { value: builderProfile.attrArn });
    new cdk.CfnOutput(this, 'BuilderSecurityGroupId', { value: builderSg.securityGroupId });
    new cdk.CfnOutput(this, 'WorkBucket', { value: workBucket });
    new cdk.CfnOutput(this, 'InstanceConnectEndpointId', { value: eice.attrId });
  }
}
