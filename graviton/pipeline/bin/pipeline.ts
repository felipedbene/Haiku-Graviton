#!/usr/bin/env node
import * as cdk from 'aws-cdk-lib';
import { loadConfig, loadSharedConfig } from '../lib/config';
import { HaikuGravitonPipelineStack } from '../lib/haiku-graviton-pipeline-stack';
import { OpsStack } from '../lib/ops-stack';

const app = new cdk.App();

// The ops build stack needs only the SHARED config (account/region/buckets/AMI
// params), so it always synthesizes/deploys without any bake-only input. It is
// the relocated DebeosOpsStack -- same stack id + construct tree, so migrating
// it here from ~/repology is a CloudFormation UPDATE, not a replace.
const shared = loadSharedConfig(app);
new OpsStack(app, 'DebeosOpsStack', {
  env: { account: shared.account, region: shared.region },
  description: 'DeBeOS staleness->rebuild ops: DynamoDB backlog+state, Step Functions ' +
    'build-wave, transient Graviton builders, hardened VPC.',
  config: shared,
});

// The bake pipeline requires a CodeConnections ARN (and a perf-gate builder).
// Only wire it when a connection ARN is supplied, so `cdk deploy DebeosOpsStack`
// works with no bake context, and the bake's fail-by-name is preserved when you
// actually target it.
const connectionArn = app.node.tryGetContext('haiku:connectionArn') ?? process.env.HAIKU_CONNECTION_ARN;
if (connectionArn) {
  const config = loadConfig(app);
  new HaikuGravitonPipelineStack(app, 'HaikuGravitonBakePipeline', {
    env: { account: config.account, region: config.region },
    description:
      'Cloud-native bake pipeline for the Haiku-on-Graviton arm64 AMI: cross-build ' +
      '(OpenSSH hpkg + jam @minimum-mmc via UserBuildConfig injection) -> GPT image ' +
      'assembly -> import-snapshot -> register-image (arm64/uefi/ena) -> gated canonical promotion.',
    config,
    tags: {
      project: 'haiku-graviton',
      component: 'bake-pipeline',
    },
  });
}

app.synth();
