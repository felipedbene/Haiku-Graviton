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

// Independent parallel bake pipelines for on-demand experimental / kernel-test
// bakes. Each is a standalone copy -- its own pipeline, CodeBuild projects, and
// auto-named work bucket -- reusing the same source connection, so baking an
// experimental branch never touches the trunk `HaikuGravitonBakePipeline` or
// the canonical path. Base config is shared today; each variant is the seam to
// customize into a purpose-built pipeline (e.g. a kernel-debug profile) later.
// Deploy ONLY the variant id(s) you want; never `--all`.
for (const variant of ['2', '3', '4']) {
  new HaikuGravitonPipelineStack(app, `HaikuGravitonBakePipeline${variant}`, {
    env: { account: config.account, region: config.region },
    description:
      `Parallel bake pipeline (variant ${variant}) for on-demand experimental / ` +
      'kernel-test bakes; independent of the trunk pipeline, reuses the source connection.',
    config: { ...config, amiNamePrefix: `haiku-graviton${variant}`, workBucketName: undefined },
    tags: { project: 'haiku-graviton', component: `bake-pipeline-${variant}` },
  });
}

app.synth();
