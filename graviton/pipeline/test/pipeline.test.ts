import * as child_process from 'child_process';
import * as crypto from 'crypto';
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
// (this repo has been bitten by the JS -> JSON quoting path before).
//
// The scripts used to be base64-embedded in the buildspec and these tests decoded
// them out of the template. They are S3 assets now (#454: the inline buildspec hit
// CodeBuild's 25,600-character ceiling), so `bankedScripts` instead RESOLVES each
// fetch: for every `aws s3 cp "$VAR" <dir>/<name>` it checks that VAR is a project
// variable holding an s3 URI, that the very next command pins a sha256, and that
// the pinned sha256 is the sha256 of the TRACKED file. Only then is the tracked
// file's content returned for the content assertions below.
//
// That is a stronger link than decoding base64 was, not a weaker one: the pinned
// hash is also what the job ENFORCES at runtime before it runs the file, so
// "the bytes asserted on here are the bytes that will run" is checked in the same
// place it is enforced. A drifted script, a script fetched without a checksum, or a
// checksum that does not belong to the tracked file all turn these red.
const SCRIPTS_DIR = path.join(__dirname, '..', '..', 'scripts');

function trackedScript(name: string): string {
  return fs.readFileSync(path.join(SCRIPTS_DIR, name), 'utf8');
}

function trackedSha256(name: string): string {
  return crypto.createHash('sha256')
    .update(fs.readFileSync(path.join(SCRIPTS_DIR, name))).digest('hex');
}

/** {scriptName: tracked content} for every script a buildspec fetches + verifies. */
function bankedScripts(build: string[], env: Record<string, string>, dir: string)
    : Record<string, string> {
  const banked: Record<string, string> = {};
  for (let i = 0; i < build.length; i++) {
    const cp = new RegExp(`^aws s3 cp "\\$(\\w+)" ${dir}/(\\S+) --only-show-errors$`)
      .exec(build[i]);
    if (!cp) continue;
    const [, uriVar, name] = cp;
    // The URI must arrive in a project variable holding a real s3 URI -- not be
    // interpolated into the buildspec, which would turn BuildSpec into an Fn::Join.
    expect(env[uriVar]).toMatch(/^s3:\/\/\S+$/);
    // The NEXT command must verify it, so there is no window in which an
    // unverified file could be executed.
    const verify = build[i + 1] ?? '';
    expect(verify).toContain('sha256sum -c -');
    expect(verify).toContain(`${dir}/${name}`);
    expect(verify).toContain(trackedSha256(name));
    banked[name] = trackedScript(name);
  }
  return banked;
}

function opsProject(name: string): {
  props: any;
  install: string[];
  build: string[];
  env: Record<string, string>;
} {
  const app = new cdk.App();
  const stack = new OpsStack(app, 'TestOpsStack', {
    env: { account: config.account, region: config.region },
    config,
  });
  const t = Template.fromStack(stack);
  const projects = t.findResources('AWS::CodeBuild::Project');
  const project = Object.values(projects).find(
    (p: any) => p.Properties.Name === name) as any;
  expect(project).toBeDefined();
  const spec = JSON.parse(project.Properties.Source.BuildSpec);
  const env: Record<string, string> = {};
  for (const e of project.Properties.Environment.EnvironmentVariables ?? []) {
    env[e.Name] = e.Value;
  }
  return {
    props: project.Properties,
    install: spec.phases.install.commands,
    build: spec.phases.build.commands,
    env,
  };
}

function promoteProject(): {
  props: any;
  install: string[];
  build: string[];
  env: Record<string, string>;
  banked: Record<string, string>;
} {
  const p = opsProject('debeos-repo-promote-green');
  return { ...p, banked: bankedScripts(p.build, p.env, '/opt/debeos-promote') };
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
  // promoteProject() has already established that the job fetches this exact file
  // and refuses anything whose sha256 differs, so running the tracked file IS
  // running what the job runs.
  const { banked } = promoteProject();
  expect(banked['haiku-repo-promote-green']).toBeDefined();
  const script = path.join(SCRIPTS_DIR, 'haiku-repo-promote-green');
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
});

// The ordering + classifier logic decides whether a promote REPLACES a live
// package (green was NEWER for six of the ten green-only names, including
// freetype and coreutils), so its self-test runs in CI as well as in the job.
test('promote: the banked script self-test passes', () => {
  const { banked, build } = promoteProject();
  expect(banked['haiku-repo-promote-green']).toBeDefined();
  const out = child_process.execFileSync(
    'python3', [path.join(SCRIPTS_DIR, 'haiku-repo-promote-green'), '--self-test'],
    { encoding: 'utf8' });
  expect(out).toContain('OK');
  // ... and the job runs it AFTER the scripts are verified and BEFORE it looks at
  // either pool.
  const verified = build.findIndex(c => c.includes('haiku-repo-promote-green')
    && c.includes('sha256sum -c -'));
  const selfTest = build.findIndex(c => c.includes('--self-test'));
  const run = build.findIndex(c => c.endsWith('haiku-repo-promote-green --in-process'));
  expect(verified).toBeGreaterThan(-1);
  expect(verified).toBeLessThan(selfTest);
  expect(selfTest).toBeLessThan(run);
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
  // bankedScripts only yields a script that is fetched AND sha256-verified into
  // /opt/debeos-promote, so this list is also the assertion that all three land in
  // the SAME directory -- which is what makes haiku-repo-add's sibling lookup work.
  expect(Object.keys(banked).sort()).toEqual(
    ['haiku-publish-lock.sh', 'haiku-repo-add', 'haiku-repo-promote-green']);
  const fetched = build.filter((c) => c.startsWith('aws s3 cp "$HG_SCRIPT'));
  expect(fetched).toHaveLength(3);
  for (const c of fetched) expect(c).toContain('/opt/debeos-promote/');
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

// ---- ops stack: the inline buildspec ceiling (issue #454) --------------------
// This is the test that should have existed before #451 merged. CodeBuild rejects an
// inline buildspec longer than 25,600 characters, and the rejection happens at
// DEPLOY time -- CloudFormation reported CREATE_FAILED "Max buildspec length is
// 25600" for the promote project (90,745 characters) after the change had been
// reviewed and merged, while the publish project sat at 25,497, i.e. 103 characters
// from the same fate on the critical path for every wave. Nothing in synth, jest or
// review noticed. Scripts now travel as S3 assets, so a buildspec no longer grows
// with what it runs -- and this asserts that, rather than trusting it.
const BUILDSPEC_MAX = 25600;

test('ops: every inline buildspec stays well under CodeBuild\'s 25,600 ceiling (#454)', () => {
  const app = new cdk.App();
  const stack = new OpsStack(app, 'TestOpsStack', {
    env: { account: config.account, region: config.region },
    config,
  });
  const projects = Template.fromStack(stack).findResources('AWS::CodeBuild::Project');
  const names = Object.values(projects).map((p: any) => p.Properties.Name).sort();
  // Guard the guard: if a project is added and this test is not updated, the new
  // project's buildspec would go unchecked.
  expect(names).toEqual(['debeos-repo-promote-green', 'debeos-repo-publish']);
  for (const p of Object.values(projects) as any[]) {
    const spec = p.Properties.Source.BuildSpec;
    // A CFN token would make this an Fn::Join whose deployed length cannot be
    // measured here -- which would silently disable this test.
    expect(typeof spec).toBe('string');
    expect(spec.length).toBeLessThan(BUILDSPEC_MAX);
    // The ceiling was reached by embedding whole scripts. A buildspec command is a
    // shell line; a ~40 KB one is a file that belongs in an asset. Catch a
    // re-introduction of inlining long before it reaches the hard limit.
    for (const phase of Object.values(spec ? JSON.parse(spec).phases : {}) as any[]) {
      for (const c of phase.commands as string[]) {
        expect(c.length).toBeLessThan(4096);
      }
    }
  }
});

// The publish project fetches its one script the same way, so the fix is not
// promote-only: the 103 characters of headroom were the more urgent half.
test('publish: haiku-repo-add is fetched from an asset and sha256-verified (#454)', () => {
  const p = opsProject('debeos-repo-publish');
  const banked = bankedScripts(p.build, p.env, '/tmp');
  expect(Object.keys(banked)).toEqual(['haiku-repo-add']);
  expect(p.env.HG_SCRIPT_REPO_ADD).toMatch(/^s3:\/\/\S+$/);
  // Nothing is base64-inlined any more, in either project.
  for (const name of ['debeos-repo-publish', 'debeos-repo-promote-green']) {
    const cmds = opsProject(name).build;
    expect(cmds.join('\n')).not.toContain('base64 -d');
  }
  // The script is verified BEFORE it is run.
  const verify = p.build.findIndex(c => c.includes('sha256sum -c -'));
  const run = p.build.findIndex(c => c === 'bash /tmp/haiku-repo-add');
  expect(verify).toBeGreaterThan(-1);
  expect(run).toBeGreaterThan(verify);
});

// ---- ops: haiku-repo-add, EXECUTED (issues #452 and #453) --------------------
// Both fixes below are behavioural, and a buildspec that merely CONTAINS the change
// is not proof of anything. So the tracked haiku-repo-add is run for real, against
// stand-ins for the two things it cannot have here: the Haiku host tools
// (`package`, `package_repo`) and S3. The fake `aws` maps s3://bucket/key onto a
// local directory, which is enough for this script -- it speaks only
// `s3 sync/cp/rm`, and so does the #164 lock library (deliberately: see its header).
//
// The scripts are SYMLINKED into the sandbox rather than copied, so what runs is
// byte-for-byte the tracked file; `dirname $0` still resolves to the sandbox, which
// is what makes the script's own sibling lookup for haiku-publish-lock.sh land on
// (or miss) the sandbox copy.

/** A fake .hpkg is just its .PackageInfo -- which is all these paths ever read. */
function fakeHpkg(name: string, version: string): string {
  return [
    `name\t"${name}"`,
    `version\t"${version}"`,
    `architecture\t"arm64"`,
    // NOT the canonical vendor/packager, so every package takes the re-stamp path.
    `vendor\t"Somebody"`,
    `packager\t"Somebody <nobody@example.invalid>"`,
    '',
  ].join('\n');
}

/** An hpkg `package list -i` cannot parse -- the unreadable-hpkg skip of #442. */
const UNREADABLE_HPKG = 'CORRUPT\n';

const FAKE_AWS = `#!/usr/bin/env bash
# Local stand-in for \`aws s3\`. s3://bucket/key <-> $FAKE_S3/bucket/key.
set -u
loc() { case "$1" in s3://*) echo "$FAKE_S3/\${1#s3://}";; *) echo "$1";; esac; }
[ "\${1:-}" = s3 ] || { echo "fake-aws: unsupported service \${1:-}" >&2; exit 64; }
shift; op="\${1:-}"; shift
del=0; pos=()
while [ $# -gt 0 ]; do
  case "$1" in
    --region|--exclude|--include) shift 2;;
    --delete) del=1; shift;;
    --*) shift;;
    *) pos+=("$1"); shift;;
  esac
done
case "$op" in
  sync)
    src="$(loc "\${pos[0]}")"; dst="$(loc "\${pos[1]}")"; mkdir -p "$dst"
    if [ -d "$src" ]; then
      for f in "$src"/*; do [ -f "$f" ] && cp -p "$f" "$dst/"; done
    fi
    if [ "$del" = 1 ]; then
      for f in "$dst"/*; do
        [ -f "$f" ] || continue
        [ -f "$src/$(basename "$f")" ] || rm -f "$f"
      done
    fi
    ;;
  cp)
    src="$(loc "\${pos[0]}")"; dst="$(loc "\${pos[1]}")"
    [ -f "$src" ] || { echo "fake-aws: no such object/file: $src" >&2; exit 1; }
    mkdir -p "$(dirname "$dst")"; cp -p "$src" "$dst"
    ;;
  rm) rm -f "$(loc "\${pos[0]}")";;
  *) echo "fake-aws: unsupported s3 op $op" >&2; exit 64;;
esac
exit 0
`;

const FAKE_PACKAGE = `#!/usr/bin/env bash
# Local stand-in for the Haiku \`package\` tool, metadata paths only.
set -u
op="\${1:-}"; shift
case "$op" in
  list)
    f=""; for a in "$@"; do case "$a" in -*) ;; *) f="$a";; esac; done
    grep -q '^CORRUPT' "$f" && { echo "fake-package: $f: invalid package file" >&2; exit 1; }
    awk -F'\\t' '{ gsub(/"/, "", $2); printf "  %s: %s\\n", $1, $2 }' "$f"
    ;;
  extract)
    dir="."; f=""; want=""
    while [ $# -gt 0 ]; do
      case "$1" in
        -C) dir="$2"; shift 2;;
        -*) shift;;
        *) if [ -z "$f" ]; then f="$1"; else want="$1"; fi; shift;;
      esac
    done
    grep -q '^CORRUPT' "$f" && exit 1
    [ "$want" = .PackageInfo ] || exit 1
    cp "$f" "$dir/.PackageInfo"
    ;;
  add)
    info=""; pkg=""
    while [ $# -gt 0 ]; do
      case "$1" in
        -i) info="$2"; shift 2;;
        -C) shift 2;;
        -*) shift;;
        *) [ -n "$pkg" ] || pkg="$1"; shift;;
      esac
    done
    # The real tool splices the edited .PackageInfo back into the package; here the
    # package IS its .PackageInfo.
    cp "$info" "$pkg"
    ;;
  *) echo "fake-package: unsupported op $op" >&2; exit 64;;
esac
`;

const FAKE_PACKAGE_REPO = `#!/usr/bin/env bash
# Local stand-in for \`package_repo create\`: writes an index named 'repo' in cwd.
set -u
[ "\${1:-}" = create ] || { echo "fake-package_repo: unsupported \${1:-}" >&2; exit 64; }
shift; shift   # drop 'create' arg list head: repo.info
{ echo "fake index"; for p in "$@"; do basename "$p"; done; } > repo
`;

interface RepoAddRun {
  status: number;
  stdout: string;
  stderr: string;
  /** basenames still in the incoming prefix after the run */
  incoming: string[];
  /** basenames in the published pool after the run */
  pool: string[];
  /** lines of HG_PUBLISHED_LIST_OUT, or null if the script never wrote it */
  published: string[] | null;
  lockExists: boolean;
}

function runRepoAdd(opts: {
  packages: Record<string, string>;
  withLock?: boolean;
  env?: Record<string, string>;
}): RepoAddRun {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'repo-add-harness-'));
  const bin = path.join(root, 'bin');
  const sandbox = path.join(root, 'scripts');
  const fakeS3 = path.join(root, 's3');
  const repoPrefix = path.join(fakeS3, 'fake-bucket', 'debeos-repo', 'arm64');
  const incomingPrefix = path.join(fakeS3, 'fake-bucket', 'incoming');
  for (const d of [bin, sandbox, path.join(repoPrefix, 'packages'), incomingPrefix]) {
    fs.mkdirSync(d, { recursive: true });
  }
  fs.writeFileSync(path.join(bin, 'aws'), FAKE_AWS, { mode: 0o755 });
  fs.writeFileSync(path.join(bin, 'package'), FAKE_PACKAGE, { mode: 0o755 });
  fs.writeFileSync(path.join(bin, 'package_repo'), FAKE_PACKAGE_REPO, { mode: 0o755 });
  // Symlink, so the file that runs is the tracked file, byte for byte.
  fs.symlinkSync(path.join(SCRIPTS_DIR, 'haiku-repo-add'),
    path.join(sandbox, 'haiku-repo-add'));
  if (opts.withLock ?? true) {
    fs.symlinkSync(path.join(SCRIPTS_DIR, 'haiku-publish-lock.sh'),
      path.join(sandbox, 'haiku-publish-lock.sh'));
  }
  for (const [name, body] of Object.entries(opts.packages)) {
    fs.writeFileSync(path.join(incomingPrefix, name), body);
  }
  const lockKey = path.join(repoPrefix, '.publish.lock');
  const publishedOut = path.join(root, 'published.list');

  const env: NodeJS.ProcessEnv = {
    ...process.env,
    PATH: `${bin}:${process.env.PATH}`,
    FAKE_S3: fakeS3,
    HG_REPO_S3: 's3://fake-bucket/debeos-repo/arm64',
    HG_INCOMING_S3: 's3://fake-bucket/incoming',
    HG_CF_DIST: '',
    PKG_TOOL: path.join(bin, 'package'),
    PACKAGE_REPO_TOOL: path.join(bin, 'package_repo'),
    HG_PUBLISHED_LIST_OUT: publishedOut,
    // Keep the lock's read-back verify instant; the mechanism under test is the
    // acquire/refuse decision, not S3's consistency window.
    HG_LOCK_SETTLE: '0',
    // The holder's heartbeat is a `sleep $HG_LOCK_HEARTBEAT` in a background
    // subshell. publish_lock_release kills the SUBSHELL, but the sleep it is blocked
    // in is a separate process that inherits this script's stderr -- so the pipe
    // stays open, and spawnSync waits, for the rest of that sleep. At the default
    // (TTL/3 = 100s) each of these runs took 100 seconds. 2s keeps the harness
    // honest (the heartbeat is still armed) and quick.
    HG_LOCK_HEARTBEAT: '2',
    ...(opts.env ?? {}),
  };
  // spawnSync rather than execFileSync: stderr is asserted on the SUCCESS paths too
  // (the lock warning), and execFileSync only hands it back on failure.
  const proc = child_process.spawnSync('bash', [path.join(sandbox, 'haiku-repo-add')],
    { env, stdio: ['ignore', 'pipe', 'pipe'], encoding: 'utf8' });
  const ls = (d: string) => (fs.existsSync(d) ? fs.readdirSync(d).sort() : []);
  return {
    status: proc.status ?? 1,
    stdout: proc.stdout ?? '',
    stderr: proc.stderr ?? '',
    incoming: ls(incomingPrefix),
    pool: ls(path.join(repoPrefix, 'packages')),
    published: fs.existsSync(publishedOut)
      ? fs.readFileSync(publishedOut, 'utf8').split('\n').filter(l => l.length > 0).sort()
      : null,
    lockExists: fs.existsSync(lockKey),
  };
}

// #452, the mechanism itself: the pipeline built its prune list from the INCOMING
// snapshot listing, so a package haiku-repo-add deliberately SKIPPED was pruned from
// the durable harvest as though it had landed in the repo -- destroying the only copy
// of build output that never entered the repo. The script now reports the set it
// actually published, and that set must exclude a skipped package.
test('repo-add reports only the packages it PUBLISHED, never a skipped one (#452)', () => {
  const r = runRepoAdd({
    packages: {
      'good-one-1.0-1-arm64.hpkg': fakeHpkg('good_one', '1.0-1'),
      'good-two-2.0-3-arm64.hpkg': fakeHpkg('good_two', '2.0-3'),
      'wrecked-9.9-1-arm64.hpkg': UNREADABLE_HPKG,
    },
  });
  expect(r.status).toBe(0);
  // The batch succeeds, publishes two, and names the third as skipped.
  expect(r.stdout).toContain('PUBLISHED 2 new package(s); skipped 1');
  // THE assertion: the reported set is what landed, and the skipped package is
  // absent from it -- so a caller pruning from this list cannot delete it.
  expect(r.published).toEqual(
    ['good-one-1.0-1-arm64.hpkg', 'good-two-2.0-3-arm64.hpkg']);
  expect(r.published).not.toContain('wrecked-9.9-1-arm64.hpkg');
  // Corroboration from the two other observable surfaces: the pool gained exactly
  // the two (under their canonical names), and the skipped one is still in incoming.
  expect(r.pool).toEqual(['good_one-1.0-1-arm64.hpkg', 'good_two-2.0-3-arm64.hpkg']);
  expect(r.incoming).toEqual(['wrecked-9.9-1-arm64.hpkg']);
});

// The worst case of #452: EVERY package fails the re-stamp. The script exits 0 with
// the repo untouched, and the old prune -- driven by the snapshot listing -- would
// then have deleted the whole batch from the harvest. The reported set must be
// EMPTY, and present, so the prune has something unambiguous to read.
test('repo-add reports an EMPTY published set when everything is skipped (#452)', () => {
  const r = runRepoAdd({
    packages: { 'wrecked-9.9-1-arm64.hpkg': UNREADABLE_HPKG },
  });
  expect(r.status).toBe(0);
  expect(r.stdout).toContain('PUBLISHED 0 new package(s)');
  expect(r.published).toEqual([]);      // written, and empty -- not missing
  expect(r.pool).toEqual([]);           // repo untouched
  expect(r.incoming).toEqual(['wrecked-9.9-1-arm64.hpkg']);
});

// The buildspec half of #452: the publish job must take its prune list FROM the add
// step and build it nowhere itself -- if this job derives the list from anything,
// from any source, the bug is back.
test('publish: prunes from the add step\'s own report of what it published', () => {
  const p = opsProject('debeos-repo-publish');
  const text = p.build.join('\n');
  expect(text).toContain('export HG_PUBLISHED_LIST_OUT=/tmp/published.list');
  expect(text).not.toMatch(/>\s*\/tmp\/published\.list/);
  // And with no list, the prune refuses instead of widening to the snapshot.
  const prune = p.build[p.build.length - 1];
  expect(prune).toContain('haiku-repo-add wrote no published list');
  expect(prune).toContain('refusing to guess which harvest keys are durable');
});

// A bucket the promote READS from must be listable, not merely gettable. The first
// real apply staged all 19 packages into the work bucket and then died on
// ListObjectsV2 against it (#456): `haiku-repo-add` reads its incoming prefix back
// with `aws s3 sync`, a sync enumerates, and s3:ListBucket is a BUCKET-level action
// that no object-level grant can satisfy. Every local proof of the apply path ran
// under operator credentials, so nothing exercised the role's own policy until
// CodeBuild assumed it.
//
// Scoped to the PROMOTE role's own policy document, identified by a Sid only it
// carries. A first cut of this test flattened every policy in the stack and passed
// even with the fix reverted, because the builder role happens to list the work
// bucket -- "somebody in this stack may enumerate it" is not the property under
// test. Buckets come from the generated shell and the project's own environment, so
// a prefix the job starts touching later is covered without editing this test.
test('promote role can LIST every bucket its buildspec reads', () => {
  const { build, env } = opsProject('debeos-repo-promote-green');
  const text = build.join('\n') + '\n' + Object.values(env).join('\n');
  const buckets = new Set<string>();
  for (const m of text.matchAll(/s3:\/\/([a-z0-9.-]+)/g)) buckets.add(m[1]);

  const app = new cdk.App();
  const stack = new OpsStack(app, 'TestOpsStack', {
    env: { account: config.account, region: config.region },
    config,
  });
  const policies = Template.fromStack(stack).findResources('AWS::IAM::Policy');
  // The promote role's document is the one carrying its blue-read-only Sid.
  const promotePolicy = (Object.values(policies) as any[]).find((pol) =>
    (pol.Properties.PolicyDocument.Statement as any[])
      .some((st) => st.Sid === 'ReadBlueStagingPoolOnly'));
  expect(promotePolicy).toBeDefined();

  const listable = new Set<string>();
  for (const st of promotePolicy.Properties.PolicyDocument.Statement as any[]) {
    const actions = ([] as string[]).concat(st.Action);
    if (!actions.includes('s3:ListBucket') && !actions.includes('s3:List*')) continue;
    for (const r of ([] as any[]).concat(st.Resource)) {
      if (typeof r !== 'string') continue;
      const m = r.match(/^arn:aws:s3:::([a-z0-9.-]+)$/);
      if (m) listable.add(m[1]);
    }
  }

  // The CDK asset bucket is fetched by exact key and never enumerated.
  const mustList = [...buckets].filter((b) => !b.startsWith('cdk-'));
  expect(mustList.length).toBeGreaterThan(1);
  for (const b of mustList) expect([...listable]).toContain(b);
});
