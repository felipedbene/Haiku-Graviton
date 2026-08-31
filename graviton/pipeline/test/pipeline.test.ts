import * as cdk from 'aws-cdk-lib';
import { Template } from 'aws-cdk-lib/assertions';
import { HaikuGravitonPipelineStack } from '../lib/haiku-graviton-pipeline-stack';
import { HaikuPipelineConfig } from '../lib/config';

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
  buildImage: 'public.ecr.aws/ubuntu/ubuntu:24.04',
  haikuRevision: 'hrev59996',
  amiNamePrefix: 'haiku-graviton',
  rootVolumeBytes: '2147483648',
  ssmOutBucketName: `haiku-graviton-${ACCOUNT}-us-west-2`,
  publishBucketName: `haiku-graviton-hpkg-${ACCOUNT}`,
  builderAmiParam: '/haiku-graviton/builder-ami-id',
  canonicalAmiParam: '/haiku-graviton/canonical-ami-id',
  peerInstanceProfile: 'AWSSupportPatchwork-SSMRoleForInstances',
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
// scoped to exactly the peer's role and restricted to being passed to EC2.
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
  expect(JSON.stringify(passers[0].Resource)).toContain(`role/${config.peerInstanceProfile}`);
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
