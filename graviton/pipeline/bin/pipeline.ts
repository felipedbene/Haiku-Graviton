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

  // Independent parallel bake pipelines for on-demand experimental / kernel-test
  // bakes. Each is a standalone copy -- its own pipeline, CodeBuild projects, and
  // auto-named work bucket -- reusing the same source connection, so baking an
  // experimental branch never touches the trunk `HaikuGravitonBakePipeline` or
  // the canonical path. Base config is shared today; each variant is the seam to
  // customize into a purpose-built pipeline (e.g. a kernel-debug profile) later.
  //
  // WHY these exist: the trunk pipeline and its Source branch pin are ONE shared
  // resource, and several agents/sessions drive them through the same role from
  // different machines. graviton/scripts/haiku-bake-lock gives them a mutex, but a
  // mutex only serialises -- it cannot make two bakes run at once. A variant is
  // how concurrent work fans out instead of queueing behind the trunk.
  //
  // workBucketName is forced to undefined so each variant gets its OWN
  // CDK-auto-named bucket; sharing the trunk bucket would let two bakes overwrite
  // each other's import/ object and cross-tools cache.
  //
  // Deploy ONLY the variant id(s) you want; never `--all` (that would also
  // re-synthesize the trunk pipeline).
  //
  // Each variant is self-service on deploy: the stack grants the account's
  // vmimport role read on its own <workBucket>/import/* (see the bucket policy in
  // haiku-graviton-pipeline-stack.ts), so Register can ImportSnapshot without the
  // manual IAM step this used to require. What is still NOT automatic is seeding
  // the work bucket -- copy cache/cross-tools-arm64.tar.zst (skips the ~1h
  // toolchain build) and the whole hpkg-pool/ prefix INCLUDING every *_devel, or
  // build features silently disable and you ship a stub (the openssl-TLS /
  // zstd / ca_root regression class). See graviton/pipeline/README.md.
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

  // The OVEN: a test-only bake pipeline that ends at the hardware gate.
  //
  // Same variant seam as 2/3/4 above (its own amiNamePrefix, so its own pipeline
  // name, CodeBuild project names, peer role and auto-named work bucket), plus the
  // one flag that removes the promotion half of the stack: `testOnly`. Sharing the
  // stack class is deliberate -- CrossBuild, Register and the hardware Test stage
  // are the parts worth keeping identical, and a forked copy of the stack would
  // drift from the trunk exactly where it matters (the thing being validated would
  // stop being the thing that ships).
  //
  // What it is for: bake candidates freely, for PR validation, unattended, without
  // QEMU. It CANNOT promote -- no Approve action, no Promote action, no Promote
  // CodeBuild project, and an explicit IAM Deny on canonical tag mutation and
  // ssm:PutParameter across all of its roles. So an agent may start as many oven
  // bakes as it likes and the `canonical=true` invariant is out of reach.
  //
  // What it still produces: a fully tested, FIRST-CLASS promotion candidate. The
  // image carries candidate=true, oven=true, its source commit/branch/revision and
  // its perf-gate verdict and measured throughput as tags -- so moving canonical
  // becomes "pick the best already-tested oven AMI and
  // `haiku-canonical promote <ami-id>` it", instead of starting a fresh bake and
  // waiting an hour for it to reach an approval gate.
  //
  // It runs in PARALLEL execution mode, so concurrent bakes run beside each other
  // instead of superseding (i.e. silently discarding) one another.
  //
  // Seed its work bucket the same way as the variants above (cross-tools cache +
  // the whole hpkg-pool/ prefix) or the bake ships a feature-capped image.
  new HaikuGravitonPipelineStack(app, 'HaikuGravitonOvenPipeline', {
    env: { account: config.account, region: config.region },
    description:
      'TEST-ONLY "oven" bake pipeline: Source -> CrossBuild -> Register -> Test and ' +
      'nothing after it. No Approve stage, no Promote stage, no Promote project, and ' +
      'no role able to move the canonical tag or write the canonical SSM parameter -- ' +
      'so it is safe to run unattended for PR validation. Its AMIs are still ' +
      'first-class promotion candidates for an out-of-band `haiku-canonical promote`.',
    config: { ...config, amiNamePrefix: 'haiku-oven', workBucketName: undefined },
    testOnly: true,
    tags: { project: 'haiku-graviton', component: 'oven-pipeline' },
  });
}

app.synth();
