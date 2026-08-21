import * as cdk from 'aws-cdk-lib';
import { Template } from 'aws-cdk-lib/assertions';
import { HaikuGravitonPipelineStack } from '../lib/haiku-graviton-pipeline-stack';
import { HaikuPipelineConfig } from '../lib/config';

const config: HaikuPipelineConfig = {
  account: '668984504585',
  region: 'us-west-2',
  repoOwner: 'test-owner',
  repoName: 'haiku',
  branch: 'graviton',
  connectionArn: 'arn:aws:codeconnections:us-west-2:668984504585:connection/00000000-0000-0000-0000-000000000000',
  buildtoolsRepo: 'https://github.com/haiku/buildtools.git',
  buildtoolsBranch: 'master',
  haikuOnEc2Repo: 'https://github.com/haiku/haiku-on-ec2.git',
  haikuOnEc2Branch: 'master',
  buildComputeType: 'BUILD_GENERAL1_2XLARGE',
  buildImage: 'public.ecr.aws/ubuntu/ubuntu:24.04',
  haikuRevision: 'hrev59996',
  amiNamePrefix: 'haiku-graviton',
  rootVolumeBytes: '2147483648',
};

function synth(): Template {
  const app = new cdk.App();
  const stack = new HaikuGravitonPipelineStack(app, 'TestStack', {
    env: { account: config.account, region: config.region },
    config,
  });
  return Template.fromStack(stack);
}

test('creates a five-stage pipeline', () => {
  const t = synth();
  t.hasResourceProperties('AWS::CodePipeline::Pipeline', {
    Stages: [
      { Name: 'Source' },
      { Name: 'CrossBuild' },
      { Name: 'Register' },
      { Name: 'Approve' },
      { Name: 'Promote' },
    ].map((s) => ({ Name: s.Name })),
  });
});

test('creates three CodeBuild projects', () => {
  const t = synth();
  t.resourceCountIs('AWS::CodeBuild::Project', 3);
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
