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
import * as ssm from 'aws-cdk-lib/aws-ssm';
import * as s3assets from 'aws-cdk-lib/aws-s3-assets';
import * as path from 'path';
import * as fs from 'fs';
import * as crypto from 'crypto';
import { SharedConfig, DEBEOS_TAGS } from './config';

/**
 * #506: extract the NATIVEBUILD_VERSION marker from the tree's build driver at synth
 * time. This binds the build Lambda's staleness assertion to the single source of truth
 * (graviton/scripts/haiku-nativebuild) instead of a duplicated constant that could drift.
 * A missing marker is a hard synth error, not a silent "": shipping the guard disabled is
 * exactly the failure #506 exists to prevent.
 */
export function readNativebuildVersion(): string {
  const driver = path.join(__dirname, '..', '..', 'scripts', 'haiku-nativebuild');
  const text = fs.readFileSync(driver, 'utf8');
  const m = text.match(/^NATIVEBUILD_VERSION="([^"]+)"/m);
  if (!m) {
    throw new Error(
      `#506: no NATIVEBUILD_VERSION="..." marker in ${driver}; the build Lambda's ` +
      `staleness guard cannot be bound to the driver. Add the marker before synth.`);
  }
  return m[1];
}

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
    // DeleteObject on the hpkg pool ONLY. The publisher path (haiku-repo-add /
    // haiku-repo-publish-ephemeral) runs under this role's profile and wants
    // `aws s3 sync --delete` to prune objects superseded by a version bump. That
    // was dropped in #269 because the role lacked s3:DeleteObject, so a
    // version-superseding publish left orphaned hpkgs in the pool (harmless -- the
    // index references only surviving files -- but untidy). Granting delete here
    // lets a follow-up restore `--delete` and keep the pool pruned. Scoped to the
    // publish (hpkg) bucket alone: the work bucket keeps write-without-delete.
    builderRole.addToPolicy(new iam.PolicyStatement({
      actions: ['s3:DeleteObject'],
      resources: [`arn:aws:s3:::${publishBucket}/*`],
    }));
    // CloudFront invalidation on the repo CDN. The publisher path
    // (haiku-repo-add / haiku-repo-publish-ephemeral) runs under this role's
    // instance profile and, when HG_CF_DIST is set, invalidates the mutable
    // index paths (/<arch>/repo, repo.info, repo.sha256) after an add so a
    // `pkgman refresh` sees the new package without waiting out the CDN TTL. The
    // role previously lacked cloudfront:CreateInvalidation, so that step failed
    // AccessDenied and the invalidation had to be done out-of-band with admin
    // creds (#164). Granting it here makes the publish self-invalidate. Scoped to
    // the configured repo distribution when known; otherwise to this account's
    // distributions (never a bare `*`). Least-privilege: CreateInvalidation only.
    builderRole.addToPolicy(new iam.PolicyStatement({
      actions: ['cloudfront:CreateInvalidation'],
      resources: [props.config.repoCloudFrontDistId
        ? `arn:aws:cloudfront::${this.account}:distribution/${props.config.repoCloudFrontDistId}`
        : `arn:aws:cloudfront::${this.account}:distribution/*`],
    }));
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
    // #506: the build Lambda stages the driver from S3 onto each builder and refuses to
    // build unless the staged copy matches the driver in THIS tree. Read that driver's
    // NATIVEBUILD_VERSION marker at synth so the assertion is bound to the source of
    // truth (graviton/scripts/haiku-nativebuild) with no hand-copied duplicate. A driver
    // bump therefore only takes effect once the script is republished to S3 AND the
    // stack is redeployed with the new version -- until both happen a wave fails loudly
    // rather than silently running a stale driver.
    const buildFn = mkFn('BuildFn', 'run_ssm.build', {
      WORK_BUCKET: workBucket,
      NATIVEBUILD_VERSION: readNativebuildVersion(),
    });
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
    // ---- how the operator scripts reach the build container (issue #454) ----
    // These jobs run tracked scripts out of graviton/scripts/ UNCHANGED, which is
    // the whole point: the publish/promote primitive an operator runs by hand is
    // the primitive the pipeline runs. Getting the file into the container used to
    // be done by base64-embedding it in the inline buildspec. That does not scale
    // and it hit a hard wall: CodeBuild rejects an inline buildspec over 25,600
    // characters, and base64 inflates by a third. The promote job (three scripts,
    // ~66 KB of source) synthesized a 90,745-character buildspec and CloudFormation
    // refused to create the project -- CREATE_FAILED, "Max buildspec length is
    // 25600" -- while the publish job's was 25,497, i.e. 103 characters of headroom
    // on the critical path for every wave.
    //
    // So the scripts are S3 ASSETS now: CDK uploads each one to the bootstrap asset
    // bucket at deploy time and the buildspec fetches it at runtime, which is the
    // shape these buildspecs already use for the banked Linux host tools. A
    // buildspec no longer grows with the scripts it runs.
    //
    // What that must not cost us is the guarantee that the shipped bytes ARE the
    // tracked file. Previously that was checkable by decoding the base64 out of the
    // synthesized template. It is now checkable the same way it is ENFORCED: the
    // sha256 of the tracked file is computed here, at synth time, and pinned into
    // the buildspec, which verifies the fetched file against it before running
    // anything. That also catches a truncated or partially-written download, which
    // a bare `aws s3 cp` exit code does not reliably do.
    const scriptsDir = path.join(__dirname, '..', '..', 'scripts');
    interface BankedScript {
      readonly name: string;
      readonly asset: s3assets.Asset;
      /** sha256 of the TRACKED file, pinned into the buildspec. */
      readonly sha256: string;
    }
    const bankScript = (id: string, name: string): BankedScript => {
      const file = path.join(scriptsDir, name);
      return {
        name,
        asset: new s3assets.Asset(this, id, { path: file }),
        sha256: crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'),
      };
    };
    // Fetch a banked script and refuse to continue unless it is byte-for-byte the
    // file that was tracked at synth time. The s3 URI arrives in an ENVIRONMENT
    // VARIABLE rather than being interpolated into the buildspec: an asset's
    // location is a deploy-time CloudFormation token, and a token inside the
    // buildspec would turn the whole BuildSpec property into an Fn::Join, which
    // makes the generated shell unreadable both to a human and to the tests that
    // assert on it. The sha256 is a synth-time constant, so it is a plain literal.
    const fetchScript = (s: BankedScript, uriVar: string, dir: string) => [
      `aws s3 cp "$${uriVar}" ${dir}/${s.name} --only-show-errors`,
      `echo "${s.sha256}  ${dir}/${s.name}" | sha256sum -c - >/dev/null || { echo "banked ${s.name} does not match the sha256 pinned at synth time (truncated fetch, or an asset/template mismatch) -- refusing to run it" >&2; exit 1; }`,
    ];
    const repoAddScript = bankScript('RepoAddScript', 'haiku-repo-add');
    const publishLockScript = bankScript('PublishLockScript', 'haiku-publish-lock.sh');
    const promoteScript = bankScript('PromoteGreenScript', 'haiku-repo-promote-green');

    // Fetching the banked Linux host tools is identical for every job that has to
    // run `package`/`package_repo` (the incremental publish and the blue->green
    // promote). ONE copy of it, so the two cannot drift -- a job that resolves
    // the WorkBucket differently, or forgets the ldd sanity check, fails in a way
    // that looks like a packaging bug.
    const fetchHostTools = [
      // Resolve the banked Linux host tools from the bake pipeline's
      // WorkBucket. Its physical name is CDK-auto-generated, so we read the
      // bucket ARN the trunk bake stack publishes to SSM (no generated name
      // is written into this tree, and the deploy-time IAM grant below is
      // scoped to the SAME param -> the same bucket).
      'WB_ARN="$(aws ssm get-parameter --name /debeos/bake/workbucket-arn --query Parameter.Value --output text)"',
      'test -n "$WB_ARN" -a "$WB_ARN" != None || { echo "cannot resolve bake WorkBucket SSM param /debeos/bake/workbucket-arn (is HaikuGravitonBakePipeline deployed?)" >&2; exit 1; }',
      'WB="${WB_ARN#arn:aws:s3:::}"',
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
    ];
    // The harvest prune, BATCHED. One `aws s3 rm` per object cost a full CLI
    // start-up each -- measured at ~1.6 objects/s, so a 3,174-package publish
    // needed ~33 min of pure teardown and was killed at the project timeout with
    // ~1,344 objects removed. `s3api delete-objects` takes up to 1000 keys per
    // request, so the same 3,174 keys are 4 requests. The key set is still EXACTLY
    // /tmp/published.list (split into <=1000-line chunks; the last chunk is short
    // whenever the count is not a multiple of 1000), so the concurrency argument
    // below is unchanged: nothing outside that list is ever a delete candidate.
    // Joined into ONE shell line so the whole prune is a single buildspec command
    // and can be wrapped in the non-fatal guard at its call site.
    //
    // Every step carries its OWN `|| { echo...; exit 1; }` and the body ends in an
    // explicit `exit 0`, deliberately not relying on `set -e`: this whole line runs
    // as the condition of an inverted `if !` (see the call site), and bash suppresses
    // errexit for the condition -- the suppression reaches INSIDE the subshell even
    // though it re-runs `set -e`. Measured while verifying this change: with -e as
    // the only guard, a NoSuchBucket DeleteObjects failure did not abort the loop and
    // the subshell then returned the trailing `if`'s 0, so the prune reported success
    // while deleting nothing. The explicit exits are what make a failure visible.
    const pruneHarvest = [
      // delete-objects takes bucket and keys separately, so split s3://B/P once.
      'H="${HARVEST_S3#s3://}";',
      'PB="${H%%/*}"; PP="${H#*/}";',
      // No prefix would make PP == PB and aim the deletes at bucket-root keys, so
      // refuse instead of guessing.
      'case "$H" in */?*) ;; *) echo "prune: HARVEST_S3 ($HARVEST_S3) has no key prefix" >&2; exit 1;; esac;',
      // The list is haiku-repo-add's own report of what it PUBLISHED (#452). If it is
      // absent the add step did not report, and there is no safe substitute: the
      // previous substitute -- this job's listing of the incoming snapshot -- is the
      // bug, because it names skipped packages too. Refuse rather than guess.
      'if [ ! -f /tmp/published.list ]; then echo "prune: haiku-repo-add wrote no published list (HG_PUBLISHED_LIST_OUT) -- refusing to guess which harvest keys are durable" >&2; exit 1; fi;',
      'if [ ! -s /tmp/published.list ]; then echo "prune: published list empty; nothing to prune"; exit 0; fi;',
      'rm -rf /tmp/prune.d; mkdir -p /tmp/prune.d || { echo "prune: cannot create /tmp/prune.d" >&2; exit 1; };',
      // <=1000 keys per request is the DeleteObjects limit; the final chunk is short
      // whenever the published count is not a multiple of 1000.
      'split -l 1000 /tmp/published.list /tmp/prune.d/chunk. || { echo "prune: could not split the published list" >&2; exit 1; };',
      'for c in /tmp/prune.d/chunk.*; do',
      '[ -e "$c" ] || { echo "prune: no chunk files -- split produced nothing" >&2; exit 1; };',
      // jq -Rn reads the chunk as raw lines and emits the request, so a key never
      // passes through shell quoting; select(length > 0) drops any blank line.
      'jq -Rn --arg p "$PP/" \'{Quiet: true, Objects: [inputs | select(length > 0) | {Key: ($p + .)}]}\' < "$c" > /tmp/prune.d/req.json || { echo "prune: could not build the delete request for $c" >&2; exit 1; };',
      'echo "prune: deleting $(wc -l < "$c") key(s) under s3://$PB/$PP/";',
      'aws s3api delete-objects --bucket "$PB" --delete file:///tmp/prune.d/req.json --output json > /tmp/prune.d/resp.json || { echo "prune: delete-objects FAILED for $c (error above)" >&2; exit 1; };',
      // Quiet:true suppresses the Deleted list, so an "Errors" key in the response
      // is the only per-key failure report there is -- treat it as a prune failure.
      'if grep -q \'"Errors"\' /tmp/prune.d/resp.json; then echo "prune: delete-objects reported per-key errors:" >&2; cat /tmp/prune.d/resp.json >&2; exit 1; fi;',
      'done;',
      'echo "prune: removed $(wc -l < /tmp/published.list) published key(s) from s3://$PB/$PP/";',
      'exit 0',
    ].join(' ');
    const publishProject = new codebuild.Project(this, 'RepoPublish', {
      projectName: 'debeos-repo-publish',
      description: "Incremental DeBeOS repo publish: haiku-repo-add over the wave's harvested hpkgs.",
      // The publish leg scales with the POOL, not with the wave: it pulls the whole
      // published set, re-stamps, rebuilds the index over the union and uploads it.
      // At 3,172 packages that took ~45 of the previous 60-minute budget, and the
      // pool grows every wave -- so the old ceiling was a scheduled surprise, and
      // when it hit, the build died mid-step reporting a misleading FAILED. A
      // CodeBuild timeout is a safety net rather than a budget (unused time is not
      // billed), so the ceiling is generous on purpose.
      timeout: cdk.Duration.hours(3),
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
        // s3 uris of the banked assets (#454): deploy-time tokens, so they travel as
        // variables and keep the buildspec a plain literal string.
        HG_SCRIPT_REPO_ADD: { value: repoAddScript.asset.s3ObjectUrl },
        // The #164 lock library, which haiku-repo-add sources from BESIDE itself
        // (#453). Without it banked here, every pipeline publish wrote the live pool
        // with the lock silently degraded to a warning.
        HG_SCRIPT_LOCK: { value: publishLockScript.asset.s3ObjectUrl },
        ARCH: { value: 'arm64' },
      },
      buildSpec: codebuild.BuildSpec.fromObject({
        version: '0.2',
        phases: {
          install: {
            commands: [
              'export DEBIAN_FRONTEND=noninteractive',
              'apt-get update -qq',
              // jq is load-bearing for the batched harvest prune below: it builds the
              // delete-objects request JSON from the raw key list, so key quoting and
              // escaping are jq's job rather than hand-rolled shell string surgery.
              'apt-get install -y --no-install-recommends curl unzip ca-certificates file jq',
              'if ! command -v aws >/dev/null 2>&1; then curl -sSLf https://awscli.amazonaws.com/awscli-exe-linux-aarch64.zip -o /tmp/awscliv2.zip && (cd /tmp && unzip -q awscliv2.zip && ./aws/install); fi',
            ],
          },
          build: {
            commands: [
              'set -eo pipefail',
              ...fetchHostTools,
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
              ...fetchScript(repoAddScript, 'HG_SCRIPT_REPO_ADD', '/tmp'),
              // The #164 lock library must land in the SAME directory as
              // haiku-repo-add, which sources it from beside ITSELF (#453). It was
              // simply never banked here, so every pipeline publish logged
              // "publishing WITHOUT the #164 concurrency lock" and carried on.
              // HG_REQUIRE_PUBLISH_LOCK makes its absence FATAL on this path: an
              // automated job that banks the library cannot be missing it for any
              // reason other than a packaging bug, and an unlocked read-modify-write
              // of the live pool is exactly what #164 exists to prevent. (The
              // promote job reaches the same conclusion from the other side --
              // haiku-repo-promote-green refuses outright.)
              ...fetchScript(publishLockScript, 'HG_SCRIPT_LOCK', '/tmp'),
              'export HG_REQUIRE_PUBLISH_LOCK=1',
              'export HG_INCOMING_S3="$INCOMING"',
              // The prune below deletes from the DURABLE harvest, so its key set must
              // come from what haiku-repo-add actually PUBLISHED -- not from this
              // job's listing of what it offered (#452). haiku-repo-add writes this
              // file after the packages and the index are uploaded, and omits every
              // package it SKIPPED (unreadable-hpkg / restamp-*-failed), so a skipped
              // package keeps its only copy in the harvest and the next publish can
              // retry it. Nothing here creates the file: if the add step does not,
              // the prune refuses rather than falling back to a wider set.
              'export HG_PUBLISHED_LIST_OUT=/tmp/published.list',
              'rm -f /tmp/published.list',
              'bash /tmp/haiku-repo-add',
              // Prune only the just-published hpkgs from the DURABLE harvest so the
              // next wave's publish re-processes only genuinely NEW output instead
              // of re-syncing + re-stamping the whole accumulated ~1885-pkg /~6 GB
              // harvest on every one-package wave. We reach here only after
              // haiku-repo-add exits 0 (set -eo pipefail above), and the list is the
              // one IT wrote after uploading, so every package named in
              // /tmp/published.list is durable in the LIVE repo and deleting it from
              // the harvest cannot lose it. (Before #452 the list was this job's
              // listing of the incoming snapshot, which also named the packages the
              // add step SKIPPED -- deleting the only copy of build output that never
              // reached the repo.) We delete by EXACT harvested
              // key, so (a) a concurrent builder that harvested a NEW package into
              // $HARVEST_S3 during this publish is never touched, and (b) a package
              // from an acquire-skipped wave stays in the harvest until it is
              // actually published. The live repo -- not the harvest -- is the
              // durable pool; the harvest is a staging area for un-published output.
              //
              // The publish is COMPLETE at this point: the index is rebuilt and
              // uploaded, so everything user-visible has landed. The prune is
              // bookkeeping, and a bookkeeping failure must not report a completed
              // publish as FAILED -- in the state machine that sinks the wave's
              // publish leg. So it runs in a guarded subshell (`if ! ( ... )`):
              // CodeBuild fails a phase on any command's non-zero exit regardless of
              // `set -e`, and the `if` makes this command's own exit status 0. The
              // failure is never silent -- the subshell's own stderr plus the WARNING
              // below name it in the log. Leftovers are harmless: they are already
              // durable in the live repo, and the next publish just re-syncs them.
              // `wc -l` on a list the add step did not write would print a bare shell
              // error here; the prune body diagnoses the missing file properly.
              'echo "== prune $(wc -l < /tmp/published.list 2>/dev/null || echo 0) published package(s) from durable harvest $HARVEST_S3 =="',
              `if ! ( set -eo pipefail; ${pruneHarvest} ); then echo "WARNING: harvest prune did NOT complete -- the publish itself SUCCEEDED (packages + index uploaded to \$HG_REPO_S3). Already-published hpkgs are left under \$HARVEST_S3; the next publish will re-sync and re-stamp them. Investigate the prune errors above." >&2; fi`,
            ],
          },
        },
      }),
    });
    // Publish job IAM: read+write the live repo pool, read the harvest + write
    // the per-run incoming snapshot (both in workBucket), read the banked host
    // tools from the bake WorkBucket, and invalidate the repo's CDN index paths.
    // The tagging pair is load-bearing: the harvest snapshot is an S3->S3
    // `aws s3 sync`, whose CopyObject path reads the source object's tags
    // (GetObjectTagging) and writes them to the copy (PutObjectTagging) --
    // without them the snapshot exits 1 and every publish fails.
    publishProject.addToRolePolicy(new iam.PolicyStatement({
      actions: ['s3:GetObject', 's3:PutObject', 's3:DeleteObject', 's3:ListBucket',
        's3:GetObjectTagging', 's3:PutObjectTagging'],
      resources: [
        `arn:aws:s3:::${publishBucket}`, `arn:aws:s3:::${publishBucket}/*`,
        `arn:aws:s3:::${workBucket}`, `arn:aws:s3:::${workBucket}/*`,
      ],
    }));
    // Read the banked assets out of the CDK bootstrap asset bucket (#454).
    // grantRead scopes this to that bucket + each asset's own key. Both scripts the
    // buildspec fetches need one, including the #164 lock library added in #453 --
    // a fetch the role cannot read fails the build rather than degrading, which is
    // the intended direction for this one.
    repoAddScript.asset.grantRead(publishProject);
    publishLockScript.asset.grantRead(publishProject);
    // Banked host tools live in the bake pipeline's WorkBucket, whose physical
    // name is CDK-auto-generated. Instead of the old fragile name-substring
    // wildcard (arn:aws:s3:::*bakepipeline*workbucket*), import the bucket ARN the
    // trunk bake stack (HaikuGravitonBakePipeline) publishes to SSM and scope the
    // read grant to exactly that bucket. `valueForStringParameter` resolves the
    // param at DEPLOY time via a CloudFormation dynamic reference, so this couples
    // the ops publish path to the trunk bake stack having been deployed -- already
    // an implicit RUNTIME dependency (the buildspec fetches that bucket's host
    // tools). The buildspec resolves the same param at runtime, which is why the
    // former `cloudformation:DescribeStacks` discovery grant is gone too.
    const bakeWorkBucketArnParam = '/debeos/bake/workbucket-arn';
    const bakeWorkBucketArn = ssm.StringParameter.valueForStringParameter(
      this, bakeWorkBucketArnParam);
    publishProject.addToRolePolicy(new iam.PolicyStatement({
      actions: ['s3:GetObject', 's3:ListBucket'],
      resources: [bakeWorkBucketArn, `${bakeWorkBucketArn}/*`],
    }));
    // Runtime read of that same SSM param (the buildspec derives the bucket name
    // from it) -- scoped to the one parameter.
    const bakeWorkBucketParam = ssm.StringParameter.fromStringParameterName(
      this, 'BakeWorkBucketArnParam', bakeWorkBucketArnParam);
    bakeWorkBucketParam.grantRead(publishProject);
    publishProject.addToRolePolicy(new iam.PolicyStatement({
      actions: ['cloudfront:CreateInvalidation'],
      resources: [props.config.repoCloudFrontDistId
        ? `arn:aws:cloudfront::${this.account}:distribution/${props.config.repoCloudFrontDistId}`
        : '*'],
    }));

    // ---- gated blue -> green promote (issue #443) -------------------------
    // The publish project above writes BLUE (debeos-repo/<arch>). NOTHING serves
    // blue: both CloudFront distributions -- prod and beta -- carry OriginPath
    // /debeos-repo-green, so GREEN is the live pool and a wave's output reached the
    // pool without ever reaching users. #443 decided the shape: blue stays the
    // pipeline's staging pool and this job is how a delta reaches users --
    // automated, but GATED.
    //
    // It is deliberately NOT wired into the build-wave state machine. A promote
    // goes live, so the trigger is a human starting this build (or the
    // `haiku-repo-promote-green --apply` wrapper, which starts it for you).
    // MODE defaults to `plan`, so even an unparameterised start-build cannot
    // mutate green; `apply` additionally requires a PLAN_ID whose recorded hash of
    // BOTH pools still matches the pools as they are at apply time.
    //
    // Union only, never a mirror: green is NOT a subset of blue (ten packages
    // existed only in green when this was written, `freetype` and `coreutils`
    // among them), so an `s3 sync --delete` blue -> green would delete packages
    // users can install. `haiku-repo-add` is a union add with no delete path,
    // and haiku-repo-promote-green checks the union post-condition against S3
    // after the add rather than trusting it.
    //
    // Blue is READ-ONLY here in two independent ways: the job snapshots the
    // planned delta into a per-run disposable prefix in the WORK bucket (which is
    // what haiku-repo-add is pointed at, because it clears its own incoming on
    // success), and the IAM below grants blue GetObject only.
    const promoteScriptsDir = '/opt/debeos-promote';
    const promoteProject = new codebuild.Project(this, 'RepoPromoteGreen', {
      projectName: 'debeos-repo-promote-green',
      description: 'Gated blue->green DeBeOS repo promote (plan/apply, union-add only).',
      // Same reasoning as the publish leg: an apply rebuilds the index over the
      // WHOLE green pool (3,000+ packages; the publish leg alone measured ~45 min
      // at 3,172 -- see #447), and the pool grows every wave. A CodeBuild timeout
      // is a safety net, not a budget -- unused time is not billed.
      timeout: cdk.Duration.hours(3),
      // One promote at a time. Two concurrent index rebuilds over green would
      // race; the #164 S3 lock would serialize them, but there is no reason to
      // pay for a second container sitting in the lock's wait loop.
      concurrentBuildLimit: 1,
      environment: {
        // Match the cross-build's arm64 Ubuntu 24.04 so the banked (glibc-linked)
        // Linux host tools run here without an ABI mismatch.
        buildImage: codebuild.LinuxArmBuildImage.fromDockerRegistry('public.ecr.aws/ubuntu/ubuntu:24.04'),
        computeType: codebuild.ComputeType.MEDIUM,
      },
      environmentVariables: {
        // MODE=plan is the DEFAULT on the project itself, so the safe mode is the
        // one you get by doing nothing. apply is reached only by overriding MODE
        // *and* supplying a PLAN_ID.
        MODE: { value: 'plan' },
        PLAN_ID: { value: '' },
        HG_BLUE_S3: { value: `s3://${publishBucket}/debeos-repo/arm64` },        // staging (READ-ONLY)
        HG_GREEN_S3: { value: `s3://${publishBucket}/debeos-repo-green/arm64` }, // LIVE (write target)
        HG_PLAN_S3: { value: `s3://${workBucket}/promote-plans` },
        HG_INCOMING_BASE: { value: `s3://${workBucket}/promote-incoming` },
        // The PUBLIC hostnames of the two distributions that serve green. Both go
        // stale on a promote, so both are invalidated. Public DNS names, not
        // account-scoped ids: the ids are resolved from these aliases at runtime
        // (the mechanism verified on branch fix/443-publish-cdn-invalidation),
        // because a distribution id must not be baked into this tree.
        REPO_HOSTS: { value: 'packages.debene.dev,beta.repository.debene.dev' },
        // s3 uris of the three banked scripts (#454). Deploy-time tokens, so they
        // travel as variables; their sha256 is pinned in the buildspec itself.
        HG_SCRIPT_PROMOTE: { value: promoteScript.asset.s3ObjectUrl },
        HG_SCRIPT_REPO_ADD: { value: repoAddScript.asset.s3ObjectUrl },
        HG_SCRIPT_LOCK: { value: publishLockScript.asset.s3ObjectUrl },
        ARCH: { value: 'arm64' },
      },
      buildSpec: codebuild.BuildSpec.fromObject({
        version: '0.2',
        phases: {
          install: {
            commands: [
              'export DEBIAN_FRONTEND=noninteractive',
              'apt-get update -qq',
              // python3 is load-bearing: haiku-repo-promote-green (the plan/apply
              // implementation, and the same file an operator runs by hand) is
              // Python. The minimal Ubuntu image does not ship it.
              'apt-get install -y --no-install-recommends curl unzip ca-certificates file jq python3',
              'if ! command -v aws >/dev/null 2>&1; then curl -sSLf https://awscli.amazonaws.com/awscli-exe-linux-aarch64.zip -o /tmp/awscliv2.zip && (cd /tmp && unzip -q awscliv2.zip && ./aws/install); fi',
            ],
          },
          build: {
            commands: [
              'set -eo pipefail',
              ...fetchHostTools,
              // Bank the three tracked scripts side by side, because
              // haiku-repo-add resolves BOTH its siblings relative to its own
              // path: haiku-publish-lock.sh (the #164 lock -- without it beside
              // haiku-repo-add the promote would publish UNLOCKED, and
              // haiku-repo-promote-green refuses rather than let that happen) and
              // haiku-aws (absent here, so it correctly falls through to the
              // stock `aws` CLI). Running the operator's own script unchanged is
              // the point: plan output is identical by hand and in this job.
              `install -d ${promoteScriptsDir}`,
              ...fetchScript(promoteScript, 'HG_SCRIPT_PROMOTE', promoteScriptsDir),
              ...fetchScript(repoAddScript, 'HG_SCRIPT_REPO_ADD', promoteScriptsDir),
              ...fetchScript(publishLockScript, 'HG_SCRIPT_LOCK', promoteScriptsDir),
              `chmod +x ${promoteScriptsDir}/haiku-repo-promote-green ${promoteScriptsDir}/haiku-repo-add`,
              // The ordering + classifier logic decides whether a promote REPLACES
              // a live package, so prove it before letting it look at the pools. It
              // needs no credentials and takes milliseconds; a failure here aborts
              // before anything is read, let alone written.
              `python3 ${promoteScriptsDir}/haiku-repo-promote-green --self-test`,
              // MODE (plan|apply) and PLAN_ID come from the environment -- the
              // project's own default is plan. --in-process means "do the work
              // here" rather than the local wrapper's "start this CodeBuild job".
              `python3 ${promoteScriptsDir}/haiku-repo-promote-green --in-process`,
            ],
          },
        },
      }),
    });
    // Promote job IAM. This role is SEPARATE from the builder/publisher role on
    // purpose: the write-the-green-prefix grant is the one #221 records as missing
    // on the builder role, and granting it there would widen every wave builder's
    // reach to the LIVE pool. Here it is the one thing this job exists to do.
    //
    // Blue is READ-ONLY: GetObject (+ObjectTagging, which the S3->S3 CopyObject
    // behind `aws s3 cp` reads) and nothing else. No PutObject, no DeleteObject.
    promoteProject.addToRolePolicy(new iam.PolicyStatement({
      sid: 'ReadBlueStagingPoolOnly',
      actions: ['s3:GetObject', 's3:GetObjectTagging'],
      resources: [`arn:aws:s3:::${publishBucket}/debeos-repo/arm64/*`],
    }));
    // Green is the write target: the packages, the three index objects, and the
    // #164 lock object beside them. DeleteObject is required for the union mirror
    // (haiku-repo-add uploads the union it built and prunes the superseded version
    // of a package it just replaced) and to release the lock.
    promoteProject.addToRolePolicy(new iam.PolicyStatement({
      sid: 'WriteGreenLivePool',
      actions: ['s3:GetObject', 's3:PutObject', 's3:DeleteObject',
        's3:GetObjectTagging', 's3:PutObjectTagging'],
      resources: [`arn:aws:s3:::${publishBucket}/debeos-repo-green/arm64/*`],
    }));
    // Listing both pools is how the delta is computed -- and the per-run snapshot
    // in the WORK bucket has to be listable too, because `haiku-repo-add` reads its
    // incoming prefix back with `aws s3 sync` and a sync enumerates. Writing that
    // snapshot needs only the object-level grant below, so the first real apply
    // staged all 19 packages and then died on ListObjectsV2 against the work
    // bucket (#456): an object-level grant can never satisfy a bucket-level action.
    //
    // s3:ListBucket is BUCKET-level: scoping it to a prefix needs an `s3:prefix`
    // condition that has to match the exact prefix the CLI's paginated
    // ListObjectsV2 asks for, which is brittle enough to fail closed at the wrong
    // moment. Granted bucket-wide on both instead -- it is a read-only
    // enumeration, and the object-level grants are what actually bound this role's
    // reach.
    promoteProject.addToRolePolicy(new iam.PolicyStatement({
      sid: 'ListThePoolAndWorkBuckets',
      actions: ['s3:ListBucket'],
      resources: [
        `arn:aws:s3:::${publishBucket}`,
        `arn:aws:s3:::${workBucket}`,
      ],
    }));
    // Plans and the per-run disposable snapshot live in the WORK bucket, not the
    // pool bucket, so the delta that is staged for a promote is physically outside
    // anything a distribution serves. Scoped to those two prefixes.
    promoteProject.addToRolePolicy(new iam.PolicyStatement({
      sid: 'PlansAndPerRunSnapshots',
      actions: ['s3:GetObject', 's3:PutObject', 's3:DeleteObject',
        's3:GetObjectTagging', 's3:PutObjectTagging'],
      resources: [
        `arn:aws:s3:::${workBucket}/promote-plans/*`,
        `arn:aws:s3:::${workBucket}/promote-incoming/*`,
      ],
    }));
    // The three banked scripts, read from the CDK bootstrap asset bucket (#454).
    for (const s of [promoteScript, repoAddScript, publishLockScript]) {
      s.asset.grantRead(promoteProject);
    }
    // Banked host tools (same bucket + same SSM param as the publish leg).
    promoteProject.addToRolePolicy(new iam.PolicyStatement({
      sid: 'ReadBankedHostTools',
      actions: ['s3:GetObject', 's3:ListBucket'],
      resources: [bakeWorkBucketArn, `${bakeWorkBucketArn}/*`],
    }));
    bakeWorkBucketParam.grantRead(promoteProject);
    // Finding the two serving distributions by their public aliases is a LIST over
    // the account's distributions; there is no resource to scope it to. This is the
    // one bare `*` in this role, and it is bare because the action cannot be
    // scoped -- not for convenience.
    promoteProject.addToRolePolicy(new iam.PolicyStatement({
      sid: 'ResolveServingDistributionsByAlias',
      actions: ['cloudfront:ListDistributions'],
      resources: ['*'],
    }));
    // The two ids are resolved at runtime, so they are not known at deploy time --
    // scoped to this ACCOUNT's distributions rather than to a bare `*`.
    promoteProject.addToRolePolicy(new iam.PolicyStatement({
      sid: 'InvalidateServingDistributions',
      actions: ['cloudfront:CreateInvalidation'],
      resources: [`arn:aws:cloudfront::${this.account}:distribution/*`],
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
    // a publish failure frees the lock before failing; it is not in this list.
    // AcquirePublishLock is also handled separately just below, because an
    // exhausted acquire must NOT fail an already-successful build.)
    for (const s of [launch, checkSsm, buildMap]) {
      s.addCatch(reapFail, { resultPath: '$.error' });
    }
    // A failed *acquire* is not a failed *wave*. If the single-flight lock stays
    // held for the whole retry budget (~56 min), the ConditionalCheckFailedException
    // surfaces here -- but by this point the wave's packages are already built and
    // durably harvested in s3://<workBucket>/hpkg/arm64/. Failing the execution
    // would discard that work. Instead, route an exhausted acquire to the SUCCESS
    // reap path: the build succeeded, we merely skip publishing this run. The
    // harvested output stays put and the NEXT wave's publish folds it into the repo
    // (the incremental add is a union over the full harvest, so nothing is lost).
    // This specific-error catch is added BEFORE the catch-all so it wins for
    // ConditionalCheckFailedException; any OTHER acquire error still fails the wave.
    acquireLock.addCatch(reapSuccess, {
      errors: ['DynamoDB.ConditionalCheckFailedException'],
      resultPath: '$.error',
    });
    acquireLock.addCatch(reapFail, { resultPath: '$.error' });

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
    // Deliberately NOT granted: codebuild:StartBuild on debeos-repo-promote-green.
    // A promote goes LIVE, and this role is the identity a prompt-injectable agent
    // runs as. The gate in #443 is a human reading a plan and applying THAT plan;
    // handing the agent the trigger would make the gate decorative. An operator
    // promotes with their own credentials (graviton/scripts/haiku-repo-promote-green).


    // ---- outputs the DevOps agent needs to start an execution ------------
    new cdk.CfnOutput(this, 'OperatorRoleArn', { value: operatorRole.roleArn });
    new cdk.CfnOutput(this, 'TableName', { value: this.table.tableName });
    new cdk.CfnOutput(this, 'StateMachineArn', { value: this.stateMachine.stateMachineArn });
    new cdk.CfnOutput(this, 'BuilderInstanceProfileArn', { value: builderProfile.attrArn });
    new cdk.CfnOutput(this, 'BuilderSecurityGroupId', { value: builderSg.securityGroupId });
    new cdk.CfnOutput(this, 'WorkBucket', { value: workBucket });
    new cdk.CfnOutput(this, 'RepoPromoteGreenProject', {
      value: promoteProject.projectName,
      description: 'Gated blue->green promote (#443). MODE=plan is the default; ' +
        'MODE=apply needs a PLAN_ID from a plan run.',
    });
    new cdk.CfnOutput(this, 'InstanceConnectEndpointId', { value: eice.attrId });
  }
}
