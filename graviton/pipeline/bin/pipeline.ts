#!/usr/bin/env node
import * as cdk from 'aws-cdk-lib';
import { loadConfig } from '../lib/config';
import { HaikuGravitonPipelineStack } from '../lib/haiku-graviton-pipeline-stack';

const app = new cdk.App();
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

app.synth();
