module.exports = {
  testEnvironment: 'node',
  roots: ['<rootDir>/test'],
  testMatch: ['**/*.test.ts'],
  // Resolve .ts BEFORE .js. Jest's default order puts js first, so in any checkout
  // where `tsc` has ever run, `import ... from '../lib/ops-stack'` silently binds to
  // a stale compiled lib/ops-stack.js instead of the source -- the suite then tests
  // whatever the tree looked like when that artifact was built, and reports failures
  // that do not exist (or, worse, passes on code that is no longer there). cdk.json
  // already guards synth against exactly this with `ts-node --prefer-ts-exts`; this
  // is the same guard for the tests.
  moduleFileExtensions: ['ts', 'tsx', 'js', 'mjs', 'cjs', 'json', 'node'],
  transform: {
    '^.+\\.tsx?$': 'ts-jest',
  },
};
