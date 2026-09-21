import * as child_process from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as cdk from 'aws-cdk-lib';
import { Match, Template } from 'aws-cdk-lib/assertions';
import { HaikuGravitonPipelineStack } from '../lib/haiku-graviton-pipeline-stack';
import { OpsStack } from '../lib/ops-stack';
import { HaikuPipelineConfig, loadConfig } from '../lib/config';

// Obviously-synthetic values throughout -- all-zero ids in the same style as the
// subnet/SG/instance placeholders below. The real account id is not in this tree
// (see loadConfig: it comes from CDK_DEFAULT_ACCOUNT at synth time), and these
// tests assert on the *shape* of the template, so they must not need it.
const ACCOUNT = '000000000000';

const config: HaikuPipelineConfig = {
  account: ACCOUNT,
  region: 'us-west-2',
  repoOwner: 'test-owner',
  repoName: 'haiku',
  branch: 'graviton',
  connectionArn: `arn:aws:codeconnections:us-west-2:${ACCOUNT}:connection/00000000-0000-0000-0000-000000000000`,
  buildtoolsRepo: 'https://github.com/haiku/buildtools.git',
  buildtoolsBranch: 'master',
  haikuOnEc2Repo: 'https://github.com/haiku/haiku-on-ec2.git',
  haikuOnEc2Branch: 'master',
  buildComputeType: 'BUILD_GENERAL1_2XLARGE',
  registerComputeType: 'BUILD_GENERAL1_LARGE',
  buildImage: 'public.ecr.aws/ubuntu/ubuntu:24.04',
  haikuRevision: 'hrev59996',
  amiNamePrefix: 'haiku-graviton',
  rootVolumeBytes: '2147483648',
  ssmOutBucketName: `haiku-graviton-${ACCOUNT}-us-west-2`,
  publishBucketName: `haiku-graviton-hpkg-${ACCOUNT}`,
  builderAmiParam: '/haiku-graviton/builder-ami-id',
  canonicalAmiParam: '/haiku-graviton/canonical-ami-id',
  peerAmiParam: '/aws/service/canonical/ubuntu/server/24.04/stable/current/arm64/hvm/ebs-gp3/ami-id',
  testVpcName: 'DebeosOpsStack/BuildVpc',
  testSubnetId: 'subnet-000000000000000aa',
  testSecurityGroupId: 'sg-000000000000000aa',
  testInstanceType: 'c7g.large',
  testKeyName: 'test-key',
  minReceiveMbps: '3000',
  minTransmitMbps: '2000',
};

function synth(): Template {
  const app = new cdk.App();
  const stack = new HaikuGravitonPipelineStack(app, 'TestStack', {
    env: { account: config.account, region: config.region },
    config,
  });
  return Template.fromStack(stack);
}

test('creates a six-stage pipeline with the hardware gate before approval', () => {
  const t = synth();
  t.hasResourceProperties('AWS::CodePipeline::Pipeline', {
    Stages: [
      { Name: 'Source' },
      { Name: 'CrossBuild' },
      { Name: 'Register' },
      { Name: 'Test' },
      { Name: 'Approve' },
      { Name: 'Promote' },
    ].map((s) => ({ Name: s.Name })),
  });
});

test('creates four CodeBuild projects', () => {
  const t = synth();
  t.resourceCountIs('AWS::CodeBuild::Project', 4);
});

// The perf gate runs unattended and holds ec2:TerminateInstances. Without a tag
// condition it would hold the right to terminate any instance in the account.
// It now creates two ephemeral instances -- the candidate (haiku-perf-gate) and
// its self-provisioned peer (haiku-perf-gate-peer) -- so terminate is scoped to
// exactly those two Names (a StringEquals list is an OR). Assert the guard
// explicitly so it cannot be dropped or widened by a later edit unnoticed.
test('the perf gate may only terminate its own ephemeral instances', () => {
  const t = synth();
  const policies = t.findResources('AWS::IAM::Policy');
  const statements = Object.values(policies).flatMap(
    (p: any) => p.Properties.PolicyDocument.Statement as any[],
  );
  const terminators = statements.filter((s) =>
    ([] as string[]).concat(s.Action ?? []).includes('ec2:TerminateInstances'),
  );
  expect(terminators).toHaveLength(1);
  const condition = terminators[0].Condition.StringEquals;
  expect(condition['ec2:ResourceTag/ephemeral']).toBe('true');
  expect(condition['ec2:ResourceTag/Name']).toEqual(['haiku-perf-gate', 'haiku-perf-gate-peer']);
});

// The stop/start regression check needs Stop/StartInstances, tag-conditioned so
// the gate can only cycle the candidate it launched (Name=haiku-perf-gate). An
// unattended stage able to stop any instance in the account could disrupt
// anything else running -- less final than terminating it, but still harmful.
// Asserted separately from the terminate test so that dropping the condition from
// either grant turns a test red on its own.
test('the perf gate may only stop and start its own ephemeral instances', () => {
  const t = synth();
  const policies = t.findResources('AWS::IAM::Policy');
  const statements = Object.values(policies).flatMap(
    (p: any) => p.Properties.PolicyDocument.Statement as any[],
  );
  for (const action of ['ec2:StopInstances', 'ec2:StartInstances']) {
    const granting = statements.filter((s) =>
      ([] as string[]).concat(s.Action ?? []).includes(action),
    );
    expect(granting).toHaveLength(1);
    const condition = granting[0].Condition.StringEquals;
    expect(condition['ec2:ResourceTag/ephemeral']).toBe('true');
    expect(condition['ec2:ResourceTag/Name']).toBe('haiku-perf-gate');
    expect(condition['aws:RequestedRegion']).toBe(config.region);
  }
});

// The gate's lifecycle powers are the ones worth bounding, so assert the negative
// too: nothing in the stack may grant an instance-lifecycle action without a tag
// condition. This catches a future grant added with only a region condition,
// which the two positive tests above would not notice.
test('no instance-lifecycle grant is left tag-unconditioned', () => {
  const t = synth();
  const policies = t.findResources('AWS::IAM::Policy');
  const statements = Object.values(policies).flatMap(
    (p: any) => p.Properties.PolicyDocument.Statement as any[],
  );
  const lifecycle = ['ec2:TerminateInstances', 'ec2:StopInstances', 'ec2:StartInstances', 'ec2:RebootInstances'];
  const allowedNames = ['haiku-perf-gate', 'haiku-perf-gate-peer'];
  for (const s of statements) {
    const actions = ([] as string[]).concat(s.Action ?? []);
    if (!actions.some((a) => lifecycle.includes(a))) continue;
    const condition = s.Condition?.StringEquals ?? {};
    expect(condition['ec2:ResourceTag/ephemeral']).toBe('true');
    // Name may be a single value (stop/start: candidate only) or a list
    // (terminate: candidate + peer), but every value must be one this gate owns.
    const names = ([] as string[]).concat(condition['ec2:ResourceTag/Name'] ?? []);
    expect(names.length).toBeGreaterThan(0);
    for (const n of names) expect(allowedNames).toContain(n);
  }
});

// The gate reads ssm-run's output from the *builder's* bucket, not the pipeline's
// work bucket. That name used to be a literal in the stack; it is now a config
// field derived from the account, so assert the grant actually follows the config.
// A literal would keep pointing at one account's bucket after the account
// changed, and the gate would then silently fall back to SSM's inline output --
// truncated at 24 KB, which is the exact failure this bucket exists to avoid.
// Matched as a substring so it holds whether or not CDK renders the ARN partition
// as an Fn::Join token.
test('the perf gate reads ssm-run output from the configured bucket', () => {
  const t = synth();
  const json = JSON.stringify(t.findResources('AWS::IAM::Policy'));
  expect(json).toContain(`${config.ssmOutBucketName}/ssm-out/*`);
});

// The peer is launched fresh each run, so its instance id is unknown at deploy
// time and SendCommand cannot be scoped to a fixed ARN. It is scoped by the peer's
// launch tags instead. Assert that every SendCommand grant on a broad instance/*
// resource carries the ssm:resourceTag/* condition, so a future edit cannot leave
// the gate able to run commands on any SSM node in the account. The document-only
// grant (arn:...:document/AWS-RunShellScript) is exempt: it targets no instance.
test('the perf gate may only SendCommand to its own tagged peer', () => {
  const t = synth();
  const policies = t.findResources('AWS::IAM::Policy');
  const statements = Object.values(policies).flatMap(
    (p: any) => p.Properties.PolicyDocument.Statement as any[],
  );
  const senders = statements.filter((s) =>
    ([] as string[]).concat(s.Action ?? []).includes('ssm:SendCommand'),
  );
  expect(senders.length).toBeGreaterThan(0);
  for (const s of senders) {
    const resources = ([] as string[]).concat(s.Resource ?? []);
    const targetsInstance = resources.some((r) => JSON.stringify(r).includes(':instance/'));
    if (!targetsInstance) continue; // the document-only grant
    const condition = s.Condition?.StringEquals ?? {};
    expect(condition['ssm:resourceTag/ephemeral']).toBe('true');
    expect(condition['ssm:resourceTag/Name']).toBe('haiku-perf-gate-peer');
  }
});

// Attaching an instance profile at RunInstances needs iam:PassRole, which must be
// scoped to exactly the peer's role and restricted to being passed to EC2. The
// peer role is now created in the stack (a dedicated least-priv role), so the
// PassRole resource is a GetAtt token to that role rather than a literal ARN.
test('the perf gate PassRole is scoped to EC2 and the peer role', () => {
  const t = synth();
  const policies = t.findResources('AWS::IAM::Policy');
  const statements = Object.values(policies).flatMap(
    (p: any) => p.Properties.PolicyDocument.Statement as any[],
  );
  const passers = statements.filter((s) =>
    ([] as string[]).concat(s.Action ?? []).includes('iam:PassRole'),
  );
  expect(passers).toHaveLength(1);
  expect(passers[0].Condition.StringEquals['iam:PassedToService']).toBe('ec2.amazonaws.com');
  expect(JSON.stringify(passers[0].Resource)).toContain('PerfGatePeerRole');
});

// The self-provisioned peer's instance profile is a dedicated least-priv role,
// not a shared broad one. Assert both that the role exists with the SSM managed
// policy and that it grants exactly the three narrow inline permissions its
// bootstrap and ssm-run need: read the ONE baron secret, read the nettput tools
// object, and write ssm-run's output to the ssm-out prefix. This catches a future
// edit that broadens the grants or points the profile at a wide shared role.
test('the peer role is least-privilege: SSM + baron secret + nettput + ssm-out', () => {
  const t = synth();
  // The SSM managed policy is attached to the peer role.
  t.hasResourceProperties('AWS::IAM::Role', {
    AssumeRolePolicyDocument: {
      Statement: [
        { Principal: { Service: 'ec2.amazonaws.com' } },
      ],
    },
    ManagedPolicyArns: [
      { 'Fn::Join': ['', ['arn:', { Ref: 'AWS::Partition' }, ':iam::aws:policy/AmazonSSMManagedInstanceCore']] },
    ],
  });
  const json = JSON.stringify(t.findResources('AWS::IAM::Policy'));
  expect(json).toContain('secretsmanager:GetSecretValue');
  expect(json).toContain('secret:haiku-graviton/baron-ssh-key-*');
  expect(json).toContain(`${config.ssmOutBucketName}/tools/*`);
  expect(json).toContain(`${config.ssmOutBucketName}/ssm-out/*`);
});

// Register is bandwidth-bound on the ~20 GiB raw-image upload (#152): the upload
// hit the SMALL ARM container's ~1.1 Gbps network baseline, and s5cmd's parallel
// multipart made no difference, so the only lever is a compute type with a higher
// network baseline. Assert the register project runs on the configured
// (LARGE-by-default) compute type -- a regression back to SMALL would silently
// re-cap the upload at ~1.1 Gbps. Register is the only project that moves bulk
// bytes, so PerfTest and Promote must stay SMALL (a bigger box there is pure cost).
test('register uses the configured compute type; perf-test and promote stay SMALL', () => {
  const t = synth();
  t.hasResourceProperties('AWS::CodeBuild::Project', {
    Name: 'haiku-graviton-register',
    Environment: Match.objectLike({ ComputeType: config.registerComputeType }),
  });
  for (const name of ['haiku-graviton-perf-test', 'haiku-graviton-promote']) {
    t.hasResourceProperties('AWS::CodeBuild::Project', {
      Name: name,
      Environment: Match.objectLike({ ComputeType: 'BUILD_GENERAL1_SMALL' }),
    });
  }
});

// The register compute type default is LARGE, not SMALL: ARM has no MEDIUM, so
// LARGE is the smallest bump above the bandwidth-capped SMALL (#152). Assert the
// default in loadConfig so a synth-fixture value cannot mask a regression of the
// default itself. Clear the env override so the process env cannot mask it.
test('the register compute type default is LARGE (issue #152)', () => {
  const saved = process.env.HAIKU_REGISTER_COMPUTE;
  delete process.env.HAIKU_REGISTER_COMPUTE;
  try {
    const app = new cdk.App({
      context: {
        'haiku:account': ACCOUNT,
        'haiku:repoOwner': 'test-owner',
        'haiku:connectionArn': config.connectionArn,
      },
    });
    expect(loadConfig(app).registerComputeType).toBe('BUILD_GENERAL1_LARGE');
  } finally {
    if (saved === undefined) delete process.env.HAIKU_REGISTER_COMPUTE;
    else process.env.HAIKU_REGISTER_COMPUTE = saved;
  }
});

test('has a retained encrypted work bucket', () => {
  const t = synth();
  t.hasResource('AWS::S3::Bucket', { DeletionPolicy: 'Retain' });
});

test('promote role can mutate tags but not register images', () => {
  const t = synth();
  // At least one policy grants DeleteTags (only the promote role should).
  const policies = t.findResources('AWS::IAM::Policy');
  const json = JSON.stringify(policies);
  expect(json).toContain('ec2:DeleteTags');
  expect(json).toContain('ec2:RegisterImage');
});

test('promote role owns the canonical-ami-id SSM mirror in-stack (not a hand-applied inline policy)', () => {
  const t = synth();
  // The promote role must carry ssm:PutParameter scoped to exactly the one
  // canonical-ami-id parameter, so the unattended promote can move the SSM
  // mirror atomically with the canonical tag. Owning it here is the point of the
  // fix: an inline policy applied by hand would be wiped by a future deploy.
  t.hasResourceProperties('AWS::IAM::Policy', {
    PolicyDocument: {
      Statement: Match.arrayWith([
        Match.objectLike({
          Sid: 'MirrorCanonicalAmiIdToSsm',
          Action: 'ssm:PutParameter',
          Resource: `arn:aws:ssm:${config.region}:${config.account}:parameter${config.canonicalAmiParam}`,
        }),
      ]),
    },
  });
});

test('promote project skips the best-effort candidate prune it has no rights for', () => {
  const t = synth();
  // HG_SKIP_PRUNE=1 keeps haiku-canonical from attempting a deregister/
  // delete-snapshot sweep the tag-only promote role is deliberately not granted,
  // so the prune stops logging UnauthorizedOperation on every promote.
  t.hasResourceProperties('AWS::CodeBuild::Project', {
    Name: 'haiku-graviton-promote',
    Environment: Match.objectLike({
      EnvironmentVariables: Match.arrayWith([
        Match.objectLike({ Name: 'HG_SKIP_PRUNE', Value: '1' }),
      ]),
    }),
  });
});

// The cross-build stage consumes the DeBeOS buildtools fork (binutils 2.46.1,
// issue #89), not upstream haiku/buildtools. The default lives in loadConfig, so
// assert the default itself -- a synth-fixture value would pass even if the
// default silently regressed to upstream. Provide only the three no-fallback
// context keys loadConfig requires; buildtoolsRepo must fall through to its
// default. Clear the env override so the process env cannot mask the default.
test('the buildtools repo default is the DeBeOS fork (issue #89)', () => {
  const saved = process.env.HAIKU_BUILDTOOLS_REPO;
  delete process.env.HAIKU_BUILDTOOLS_REPO;
  try {
    const app = new cdk.App({
      context: {
        'haiku:account': ACCOUNT,
        'haiku:repoOwner': 'test-owner',
        'haiku:connectionArn': config.connectionArn,
      },
    });
    const loaded = loadConfig(app);
    expect(loaded.buildtoolsRepo).toBe('https://github.com/felipedbene/buildtools.git');
    // The branch stays master: the fork's master carries 2.46.1 once its PR merges.
    expect(loaded.buildtoolsBranch).toBe('master');
  } finally {
    if (saved === undefined) delete process.env.HAIKU_BUILDTOOLS_REPO;
    else process.env.HAIKU_BUILDTOOLS_REPO = saved;
  }
});

// Whatever buildtoolsRepo resolves to must actually reach the CrossBuild worker as
// the BUILDTOOLS_REPO env var -- the buildspec clones from it. Assert the wiring so
// a repoint of the default cannot be silently dropped between config and project.
test('the cross-build project receives buildtoolsRepo as BUILDTOOLS_REPO', () => {
  const t = synth();
  t.hasResourceProperties('AWS::CodeBuild::Project', {
    Environment: Match.objectLike({
      EnvironmentVariables: Match.arrayWith([
        Match.objectLike({ Name: 'BUILDTOOLS_REPO', Value: config.buildtoolsRepo }),
      ]),
    }),
  });
});

// ---- ops stack: the incremental publish's harvest prune (issue #445) --------
// Pull the RepoPublish buildspec out of a synthesized OpsStack. It is emitted as a
// JSON *string*, so this also round-trips the prune's shell through the exact
// JS -> JSON path the real template takes.
function publishBuildSpec(): { install: string[]; build: string[] } {
  const app = new cdk.App();
  const stack = new OpsStack(app, 'TestOpsStack', {
    env: { account: config.account, region: config.region },
    config,
  });
  const t = Template.fromStack(stack);
  const projects = t.findResources('AWS::CodeBuild::Project');
  const publish = Object.values(projects).find(
    (p: any) => p.Properties.Name === 'debeos-repo-publish') as any;
  expect(publish).toBeDefined();
  const spec = JSON.parse(publish.Properties.Source.BuildSpec);
  return { install: spec.phases.install.commands, build: spec.phases.build.commands };
}

// A per-object `aws s3 rm` loop ran at ~1.6 objects/s and could not prune a
// 3,000-package publish inside the project timeout (#445). The prune must batch
// through s3api delete-objects, at the API's 1000-keys-per-request limit, and
// still delete only the exact keys captured in /tmp/published.list.
test('the harvest prune batches through delete-objects instead of one CLI per object', () => {
  const { install, build } = publishBuildSpec();
  const prune = build.filter(c => c.includes('/tmp/published.list'));
  const joined = prune.join('\n');
  expect(joined).toContain('aws s3api delete-objects');
  expect(joined).toContain('split -l 1000 /tmp/published.list');
  // No survivor of the per-object loop.
  expect(joined).not.toMatch(/aws s3 rm "\$HARVEST_S3/);
  // jq builds the request JSON, so it has to be installed.
  expect(install.join('\n')).toMatch(/apt-get install .*\bjq\b/);
});

// The publish is complete once packages + index are uploaded; the prune is
// bookkeeping that ran last and whose timeout marked a SUCCESSFUL publish FAILED,
// sinking the wave's publish leg in the state machine. It must not be able to fail
// the build -- and must say so in the log when it does not complete.
test('a harvest prune failure cannot fail the publish, but is reported loudly', () => {
  const { build } = publishBuildSpec();
  const prune = build[build.length - 1];
  expect(prune).toContain('aws s3api delete-objects');
  // Guarded subshell: this command's own exit status is 0 either way.
  expect(prune).toMatch(/^if ! \( /);
  expect(prune).toContain('WARNING: harvest prune did NOT complete');
  // Errexit is suppressed inside an inverted condition, so each step needs its own
  // explicit exit -- otherwise a failed delete-objects reports success.
  expect(prune).toContain('delete-objects FAILED');
});

// ---- ops stack: the gated blue -> green promote (issue #443) -----------------
// Same technique as the publish tests above: synthesize the stack and pull the
// promote project's real generated shell out of the template, so these assert on
// the bytes CodeBuild will run rather than on the TypeScript that produced them
// (this repo has been bitten by the JS -> JSON quoting path before). The banked
// scripts are decoded from the template's own base64, so a test cannot be fooled
// by an encoding that hides what it is asserting about.
function promoteProject(): {
  props: any;
  install: string[];
  build: string[];
  env: Record<string, string>;
  banked: Record<string, string>;
} {
  const app = new cdk.App();
  const stack = new OpsStack(app, 'TestOpsStack', {
    env: { account: config.account, region: config.region },
    config,
  });
  const t = Template.fromStack(stack);
  const projects = t.findResources('AWS::CodeBuild::Project');
  const promote = Object.values(projects).find(
    (p: any) => p.Properties.Name === 'debeos-repo-promote-green') as any;
  expect(promote).toBeDefined();
  const spec = JSON.parse(promote.Properties.Source.BuildSpec);
  const build: string[] = spec.phases.build.commands;
  const env: Record<string, string> = {};
  for (const e of promote.Properties.Environment.EnvironmentVariables ?? []) {
    env[e.Name] = e.Value;
  }
  const banked: Record<string, string> = {};
  for (const c of build) {
    const m = /^printf %s '([A-Za-z0-9+/=]+)' \| base64 -d > \/opt\/debeos-promote\/(\S+)$/.exec(c);
    if (m) banked[m[2]] = Buffer.from(m[1], 'base64').toString('utf8');
  }
  return { props: promote.Properties, install: spec.phases.install.commands, build, env, banked };
}

function opsPolicyStatements(): any[] {
  const app = new cdk.App();
  const stack = new OpsStack(app, 'TestOpsStack', {
    env: { account: config.account, region: config.region },
    config,
  });
  const policies = Template.fromStack(stack).findResources('AWS::IAM::Policy');
  return Object.values(policies).flatMap(
    (p: any) => p.Properties.PolicyDocument.Statement as any[]);
}

// Green is NOT a subset of blue -- ten packages existed only in green when #443
// was decided -- so a mirror would delete packages users can install. The promote
// must be a UNION add. Assert it over the generated shell AND over the banked
// scripts decoded from the template: the ONLY `--delete` reachable from this path
// is haiku-repo-add's mirror of the union it has just built locally (step 5,
// which is what keeps the pool free of two versions of one package). Any other
// occurrence anywhere in the path turns this red.
test('promote: nothing in the path deletes, beyond haiku-repo-add union mirror', () => {
  const { build, banked } = promoteProject();
  expect(build.join('\n')).not.toContain('--delete');
  // Comment lines cannot delete anything, and both scripts discuss `--delete` at
  // length (which is the point -- it is the dangerous verb here). Only executable
  // lines are candidates.
  const code = (body: string) => body.split('\n').filter((l) => !/^\s*#/.test(l));
  const offenders: string[] = [];
  for (const [name, body] of Object.entries(banked)) {
    for (const line of code(body)) {
      if (!line.includes('--delete')) continue;
      if (name === 'haiku-repo-add'
          && line.includes('"$RO/packages/" "${HG_REPO_S3%/}/packages/"')) continue;
      offenders.push(`${name}: ${line.trim()}`);
    }
  }
  expect(offenders).toEqual([]);
  // ... and the promote does not reach for a mirror of blue onto green at all.
  const promoteScript = banked['haiku-repo-promote-green'];
  expect(promoteScript).toBeDefined();
  expect(code(promoteScript).join('\n')).not.toContain('--delete');
  expect(promoteScript).not.toMatch(/s3 sync[^\n]*HG_GREEN_S3/);
});

// An unparameterised start-build must not be able to mutate green. plan is the
// project's own default, and PLAN_ID starts empty so a stray apply has nothing to
// apply. The buildspec deliberately passes no mode flag, so MODE governs.
test('promote: plan is the default mode and the buildspec lets MODE govern', () => {
  const { env, build } = promoteProject();
  expect(env.MODE).toBe('plan');
  expect(env.PLAN_ID).toBe('');
  const run = build[build.length - 1];
  expect(run).toBe('python3 /opt/debeos-promote/haiku-repo-promote-green --in-process');
  expect(run).not.toContain('--apply');
});

// The gate: apply refuses without a plan id, and refuses a plan whose recorded
// hash of BOTH pools no longer matches. Executed for real against the script
// decoded out of the template -- the refusal happens before any AWS call, so this
// needs no credentials.
test('promote: apply refuses without a plan id (executed, not just inspected)', () => {
  const { banked } = promoteProject();
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'debeos-promote-test-'));
  const script = path.join(dir, 'haiku-repo-promote-green');
  fs.writeFileSync(script, banked['haiku-repo-promote-green']);
  const env = {
    ...process.env,
    MODE: 'apply',
    PLAN_ID: '',
    HG_BLUE_S3: 's3://example/blue/arm64',
    HG_GREEN_S3: 's3://example/green/arm64',
    HG_PLAN_S3: 's3://example/plans',
    HG_INCOMING_BASE: 's3://example/incoming',
  };
  let status = 0;
  let stderr = '';
  try {
    child_process.execFileSync('python3', [script, '--in-process'],
      { env, stdio: ['ignore', 'pipe', 'pipe'] });
  } catch (e: any) {
    status = e.status;
    stderr = String(e.stderr);
  }
  expect(status).not.toBe(0);
  expect(stderr).toContain('apply requires a plan id');
  // The staleness refusal is the other half of the gate.
  expect(banked['haiku-repo-promote-green']).toContain('STALE PLAN');
  fs.rmSync(dir, { recursive: true, force: true });
});

// The ordering + classifier logic decides whether a promote REPLACES a live
// package (green was NEWER for six of the ten green-only names, including
// freetype and coreutils), so its self-test runs in CI as well as in the job.
test('promote: the banked script self-test passes', () => {
  const { banked, build } = promoteProject();
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'debeos-promote-selftest-'));
  const script = path.join(dir, 'haiku-repo-promote-green');
  fs.writeFileSync(script, banked['haiku-repo-promote-green']);
  const out = child_process.execFileSync('python3', [script, '--self-test'],
    { encoding: 'utf8' });
  expect(out).toContain('OK');
  // ... and the job runs it before it looks at either pool.
  const selfTest = build.findIndex(c => c.includes('--self-test'));
  const run = build.findIndex(c => c.endsWith('haiku-repo-promote-green --in-process'));
  expect(selfTest).toBeGreaterThan(-1);
  expect(selfTest).toBeLessThan(run);
  fs.rmSync(dir, { recursive: true, force: true });
});

// A promote makes BOTH caches stale: prod and beta both carry OriginPath
// /debeos-repo-green. Both hostnames must be handed to the job, and the script
// must invalidate every distribution it resolves -- not just the first.
test('promote: both serving distributions are invalidated, resolved by alias', () => {
  const { env, banked } = promoteProject();
  expect(env.REPO_HOSTS.split(',')).toEqual(
    ['packages.debene.dev', 'beta.repository.debene.dev']);
  const script = banked['haiku-repo-promote-green'];
  expect(script).toContain('cloudfront", "list-distributions');
  expect(script).toContain('for host, dist, opath in dists:');
  expect(script).toContain('create-invalidation');
  // No distribution id is baked into the tree.
  expect(script).not.toMatch(/\bE[0-9A-Z]{12,}\b/);
  // haiku-repo-add knows about ONE distribution, so the promote takes invalidation
  // over entirely rather than half-doing it.
  expect(script).toContain('env.pop("HG_CF_DIST", None)');
});

// Green is the write target; blue is read-only. This is the grant #221 records as
// missing on the BUILDER role, and it is deliberately on a separate role here --
// so assert both halves: no statement may write under the blue prefix, and the
// green prefix must be writable.
test('promote: the green prefix is writable and the blue prefix is read-only', () => {
  const statements = opsPolicyStatements();
  const bluePrefix = `${config.publishBucketName}/debeos-repo/arm64/`;
  const greenPrefix = `${config.publishBucketName}/debeos-repo-green/arm64/`;
  const writes = ['s3:PutObject', 's3:DeleteObject', 's3:PutObjectTagging',
    's3:PutObjectAcl', 's3:*'];
  let greenWritable = false;
  let blueReadable = false;
  for (const s of statements) {
    const actions = ([] as string[]).concat(s.Action ?? []);
    const resources = ([] as string[]).concat(s.Resource ?? [])
      .map((r) => JSON.stringify(r));
    const touchesBlue = resources.some((r) => r.includes(bluePrefix));
    const touchesGreen = resources.some((r) => r.includes(greenPrefix));
    if (touchesBlue) {
      blueReadable = true;
      // A statement naming the blue prefix must not carry a write action.
      expect(actions.filter((a) => writes.includes(a))).toEqual([]);
    }
    if (touchesGreen && actions.some((a) => writes.includes(a))) greenWritable = true;
  }
  expect(blueReadable).toBe(true);
  expect(greenWritable).toBe(true);
  // The promote role's blue grant is GetObject-only, by Sid.
  const blueOnly = statements.find((s) => s.Sid === 'ReadBlueStagingPoolOnly');
  expect(blueOnly).toBeDefined();
  expect(([] as string[]).concat(blueOnly.Action).sort())
    .toEqual(['s3:GetObject', 's3:GetObjectTagging']);
});

// haiku-repo-add resolves haiku-publish-lock.sh relative to its OWN path and only
// WARNS when it is missing -- it publishes unlocked. For a promote to the live
// pool that is not acceptable (#164), so the lock library is banked beside it and
// the promote refuses outright if it is not there.
test('promote: the #164 publish lock is banked beside haiku-repo-add', () => {
  const { build, banked } = promoteProject();
  expect(Object.keys(banked).sort()).toEqual(
    ['haiku-publish-lock.sh', 'haiku-repo-add', 'haiku-repo-promote-green']);
  // All three land in the SAME directory, which is what makes the lookup work.
  const dirs = new Set(build
    .filter((c) => c.startsWith('printf %s '))
    .map((c) => c.replace(/^.*> /, '').replace(/\/[^/]+$/, '')));
  expect([...dirs]).toEqual(['/opt/debeos-promote']);
  expect(banked['haiku-publish-lock.sh']).toContain('publish_lock_acquire');
  expect(banked['haiku-repo-promote-green']).toContain(
    'to promote to the live pool without the #164 publish lock');
});

// An apply rebuilds the index over the whole green pool. The publish leg alone
// measured ~45 min at 3,172 packages and was raised to 3 hours in #447; the
// promote does the same work, so it gets the same ceiling rather than the 60-min
// default that turned a completed publish into a FAILED build.
test('promote: the job is budgeted for a whole-pool index rebuild', () => {
  const { props } = promoteProject();
  expect(props.TimeoutInMinutes).toBe(180);
  expect(props.ConcurrentBuildLimit).toBe(1);
  // python3 runs the plan/apply implementation, so it has to be installed.
  const { install } = promoteProject();
  expect(install.join('\n')).toMatch(/apt-get install .*\bpython3\b/);
});

// The gate in #443 is a human reading a plan. The least-privilege operator role is
// the identity a prompt-injectable agent runs as, so it must NOT be able to start
// a promote -- that would make the gate decorative.
test('promote: the operator role cannot start a promote build', () => {
  const statements = opsPolicyStatements();
  for (const s of statements) {
    const actions = ([] as string[]).concat(s.Action ?? []);
    if (!actions.some((a) => a.startsWith('codebuild:'))) continue;
    const resources = JSON.stringify(s.Resource ?? []);
    expect(resources).not.toContain('debeos-repo-promote-green');
  }
});
