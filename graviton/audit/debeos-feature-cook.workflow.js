export const meta = {
  name: 'debeos-feature-cook',
  description: 'Propose-only "feature cook" front half: given a feature spec (and optional design doc), decompose it into investigation angles, fan out to investigate the source, synthesize concrete diffs, adversarially verify each change, and assemble a BAKE-READY batch. Writes deliverables to a scratch dir; never builds (no cross-toolchain here) or commits. The gated tail — merge -> bake -> boot -> hardware-prove -> promote — is the separate human-gated debeos-hardware-proof SOP.',
  whenToUse: 'To develop a well-scoped DeBeOS feature/fix to a reviewable, hardware-provable diff batch. Pass args.spec (what to build + measured ground truth) and optionally args.design_doc (a repo-relative doc to treat as authoritative). Produces diffs only; land them via the hardware-proof runbook.',
  phases: [
    { title: 'Decompose', detail: 'turn the feature spec into 2-N independent investigation angles' },
    { title: 'Investigate', detail: 'one agent per angle, reads the source, locates the change' },
    { title: 'Synthesize', detail: 'produce concrete diffs / build changes + a hardware-proof plan' },
    { title: 'Verify', detail: 'adversarially check each proposed change against the source' },
    { title: 'Assemble', detail: 'write a bake-ready batch and hand off to debeos-hardware-proof.sop.md' },
  ],
}

const REPO = (args && args.repo_root) || '/local/home/benfelip/Haiku-Graviton'
const SPEC = (args && args.spec) || null
const DESIGN_DOC = (args && args.design_doc) || null      // repo-relative path to read as authoritative
const MAX_ANGLES = (args && args.max_angles) || 4
const OUT = (args && args.out) || '/tmp/debeos-feature-cook'

if (!SPEC && !DESIGN_DOC) {
  log('Nothing to cook. Pass args.spec (a feature brief) and/or args.design_doc (a repo-relative design doc path).')
  return { error: 'no spec', processed: 0 }
}

const BRIEF = `
PROJECT: DeBeOS = Haiku on AWS Graviton (arm64). Repo root: ${REPO} (Read, Grep, Bash available).
This host has NO aarch64 cross-toolchain, so you CANNOT build — static/source reasoning + diffs only.
Every change is PROPOSE-ONLY and will be built + hardware-proven separately via the gated
debeos-hardware-proof SOP. Cite file:line for every claim; label measured-fact vs inference.

FEATURE SPEC:
${SPEC || '(see design doc)'}
${DESIGN_DOC ? `\nAUTHORITATIVE DESIGN DOC (read it first): run \`cat ${REPO}/${DESIGN_DOC}\`.` : ''}
`

// ---- Schemas ----
const ANGLES_SCHEMA = {
  type: 'object', additionalProperties: false, required: ['angles'],
  properties: {
    angles: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false, required: ['key', 'prompt'],
        properties: {
          key: { type: 'string', description: 'short kebab-case label' },
          prompt: { type: 'string', description: 'the specific source-investigation task for this angle' },
        },
      },
    },
  },
}
const CHANGESET_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['changes', 'hardware_plan', 'confidence', 'open_questions'],
  properties: {
    changes: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false,
        required: ['title', 'kind', 'file', 'rationale', 'diff'],
        properties: {
          title: { type: 'string' },
          kind: { type: 'string', enum: ['code-diff', 'build-config', 'image-definition', 'mixed'] },
          file: { type: 'string', description: 'primary file the change touches' },
          rationale: { type: 'string' },
          diff: { type: 'string', description: 'unified diff, or the exact build/Jamfile edit' },
        },
      },
    },
    hardware_plan: { type: 'string', description: 'the concrete Graviton workload that proves the feature' },
    confidence: { type: 'number' },
    open_questions: { type: 'array', items: { type: 'string' } },
  },
}
const VERDICT_SCHEMA = {
  type: 'object', additionalProperties: false, required: ['holds', 'reason'],
  properties: {
    holds: { type: 'boolean' },
    reason: { type: 'string' },
    fix_if_wrong: { type: 'string' },
  },
}

// ---- Phase 1: Decompose the spec into investigation angles ----
phase('Decompose')
const decomp = await agent(
  `${BRIEF}\n\nDecompose this feature into 2-${MAX_ANGLES} INDEPENDENT investigation angles that, together, ` +
  `locate every source/build change the feature needs. Each angle is a concrete "find X in the source" task ` +
  `(a specific subsystem, call path, or build definition). Prefer the smallest set that fully covers the feature. ` +
  `Read the design doc first if one was given. Return the angles.`,
  { label: 'decompose', phase: 'Decompose', schema: ANGLES_SCHEMA, agentType: 'general-purpose' }
)
let angles = (decomp && decomp.angles) || []
if (angles.length > MAX_ANGLES) angles = angles.slice(0, MAX_ANGLES)
if (!angles.length) {
  log('Decomposition produced no angles — spec too vague. Refine args.spec.')
  return { error: 'no angles', processed: 0 }
}
log(`Cooking "${(SPEC || DESIGN_DOC).slice(0, 60)}..." via ${angles.length} angle(s): ${angles.map(a => a.key).join(', ')}`)

// ---- Phase 2: Investigate (fan out) ----
const findings = await parallel(angles.map(a => () =>
  agent(
    `${BRIEF}\n\nYOUR ANGLE (${a.key}): ${a.prompt}\n\n` +
    `Read the real source under ${REPO}. Locate the exact change site(s) with file:line, explain WHY, and ` +
    `sketch the minimal change. Write your report to ${OUT}/angle-${a.key}.md (mkdir -p ${OUT}) and return a 6-line summary.`,
    { label: `investigate:${a.key}`, phase: 'Investigate', agentType: 'general-purpose' }
  ).then(text => ({ key: a.key, text }))
)).then(r => r.filter(Boolean))

// ---- Phase 3: Synthesize the change set ----
phase('Synthesize')
const changeset = await agent(
  `${BRIEF}\n\nYou are the lead. From the angle reports below, produce the concrete CHANGE SET that ` +
  `implements the feature: one entry per change with a unified diff (or the exact build/Jamfile/image edit), ` +
  `plus ONE hardware_plan that proves the feature on Graviton. WRITE a human batch to ` +
  `${OUT}/feature-cook-batch.md and return the structured object. Do NOT claim anything builds.\n\n` +
  `=== ANGLE REPORTS ===\n${findings.map(f => `--- ${f.key} ---\n${f.text}`).join('\n\n')}`,
  { label: 'synthesize', phase: 'Synthesize', schema: CHANGESET_SCHEMA, agentType: 'general-purpose' }
)
const changes = (changeset && changeset.changes) || []
if (!changes.length) {
  log('Synthesis produced no changes.')
  return { error: 'no changes', angles: angles.map(a => a.key) }
}

// ---- Phase 4: Verify each change adversarially ----
const verdicts = await parallel(changes.map((c, i) => () =>
  agent(
    `${BRIEF}\n\nAdversarially CHECK this proposed change against the real source/build under ${REPO}. ` +
    `Does it actually achieve its stated effect, at the right place, without over-reach, behavior regressions, ` +
    `or a missed dependency/step? Default holds=false if genuinely uncertain.\n\n` +
    `PROPOSED CHANGE (${i + 1}/${changes.length}) — ${c.title}:\n${JSON.stringify(c, null, 2)}`,
    { label: `verify:${(c.file || '').split('/').pop()}:${i}`, phase: 'Verify', schema: VERDICT_SCHEMA, agentType: 'general-purpose' }
  ).then(v => ({ change: c, verdict: v }))
)).then(r => r.filter(Boolean))

const confirmed = verdicts.filter(v => v.verdict && v.verdict.holds)
const contested = verdicts.filter(v => v.verdict && !v.verdict.holds)
log(`Verify done: ${confirmed.length}/${verdicts.length} changes hold; ${contested.length} contested.`)

// ---- Phase 5: Assemble the bake-ready batch ----
phase('Assemble')
const assembler = await agent(
  `You have Read, Bash, Glob. Assemble the bake-ready batch from the change set + verify verdicts below and the ` +
  `angle reports in ${OUT}. Write ${OUT}/feature-cook-review.md (mkdir -p): a summary table, then each CONFIRMED ` +
  `change with its full diff (\`\`\`diff), rationale, and verify note; then each CONTESTED change with the verifier's ` +
  `objection + fix_if_wrong; then the hardware_plan. END with: PROPOSE-ONLY — to land this, stage the confirmed diffs ` +
  `on a topic branch and run graviton/audit/agent-sops/debeos-hardware-proof.sop.md (merge -> bake candidate -> boot -> ` +
  `run the hardware_plan -> human promotion gate). Do NOT modify source or git state. Return a one-line summary.\n\n` +
  `=== VERDICTS ===\n${JSON.stringify(verdicts.map(v => ({ title: v.change.title, kind: v.change.kind, file: v.change.file, holds: v.verdict && v.verdict.holds, reason: v.verdict && v.verdict.reason, fix_if_wrong: v.verdict && v.verdict.fix_if_wrong })))}\n\n` +
  `=== HARDWARE PLAN ===\n${changeset.hardware_plan}`,
  { label: 'assemble', phase: 'Assemble', agentType: 'general-purpose' }
)

return {
  spec: (SPEC || `design_doc:${DESIGN_DOC}`).slice(0, 120),
  angles: angles.map(a => a.key),
  changes_total: changes.length,
  changes_confirmed: confirmed.length,
  changes_contested: contested.length,
  confirmed: confirmed.map(v => ({ title: v.change.title, kind: v.change.kind, file: v.change.file })),
  contested: contested.map(v => ({ title: v.change.title, objection: v.verdict && v.verdict.reason })),
  hardware_plan: changeset.hardware_plan,
  batch: `${OUT}/feature-cook-review.md`,
  confidence: changeset.confidence,
  open_questions: changeset.open_questions,
  assembler_summary: assembler,
  next: 'PROPOSE-ONLY. Stage the confirmed diffs on a topic branch, then run debeos-hardware-proof.sop.md to bake -> boot -> prove -> (human gate) promote.',
}
