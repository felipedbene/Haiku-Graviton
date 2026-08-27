export const meta = {
  name: 'debeos-bugfix-batch',
  description: 'Propose-only Phase-4 bug-fix pass: parse PRIORITY.md, confirm+fix each finding in an isolated worktree via the debeos-bugfix SOP, assemble a human review batch. Writes NOTHING to the working tree.',
  whenToUse: 'After PRIORITY.md exists, to systematically work the ranked findings. Choose scope with args.subsystems / args.limit. Never applies or commits — produces diffs for human approval.',
  phases: [
    { title: 'Parse', detail: 'extract ranked findings from PRIORITY.md (filtered by subsystem/limit)' },
    { title: 'Fix', detail: 'one SOP-following agent per finding, each in its own scratch worktree' },
    { title: 'Review', detail: 'aggregate reports into a single review batch for approval' },
  ],
}

// ---- Inputs (all optional; sensible defaults) ----
const REPO = (args && args.repo_root) || '/local/home/benfelip/Haiku-Graviton'
const SCRATCH = (args && args.scratch_root) || '/tmp/debeos-bugfix'
const REPORTS = SCRATCH + '/reports'
const SOP = REPO + '/graviton/audit/agent-sops/debeos-bugfix.sop.md'
const SUBSYSTEMS = (args && args.subsystems) || null   // e.g. ['bfs','network-stack']; null = all active-surface
const LIMIT = (args && args.limit) || 10               // cap agents; medium size guideline is <15
const PREPARSED = (args && args.findings) || null      // skip PRIORITY.md parse if caller supplies findings

// ---- Schemas ----
const FINDINGS_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['findings'],
  properties: {
    findings: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false,
        required: ['file', 'line', 'checker', 'subsystem', 'description'],
        properties: {
          file: { type: 'string', description: 'repo-relative path' },
          line: { type: 'integer' },
          checker: { type: 'string', description: 'cppcheck or clang-tidy id' },
          subsystem: { type: 'string', enum: ['ena', 'network-stack', 'bfs', 'app_server-remote', 'kernel/arm64'] },
          description: { type: 'string' },
          rank: { type: 'integer', description: 'PRIORITY.md rank if present' },
        },
      },
    },
  },
}

const DIGEST_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['classification', 'file', 'line', 'confidence', 'report_path'],
  properties: {
    classification: { type: 'string', enum: ['real-bug', 'false-positive', 'needs-hardware', 'needs-rework', 'out-of-scope', 'error'] },
    file: { type: 'string' },
    line: { type: 'integer' },
    confidence: { type: 'string', enum: ['high', 'medium', 'low'] },
    report_path: { type: 'string' },
    one_line: { type: 'string', description: 'short human summary' },
  },
}

// ---- Phase 1: Parse PRIORITY.md into ranked findings ----
phase('Parse')
let findings = PREPARSED
if (!findings) {
  const subFilter = SUBSYSTEMS ? `Only include findings whose subsystem is one of: ${SUBSYSTEMS.join(', ')}.` : 'Include all active-surface subsystems.'
  const parsed = await agent(
    `Read ${REPO}/PRIORITY.md. It is the Phase-3 triage table for a DeBeOS static-analysis audit. ` +
    `Extract the ranked candidate findings (the numbered rows across the "Standouts" table and the per-subsystem tables). ` +
    `For each, return file (repo-relative, e.g. "src/add-ons/kernel/file_systems/bfs/Journal.cpp" — expand the shorthand like "bfs/Journal.cpp" to the full repo path), ` +
    `line (the first line number listed; if a row lists several, emit one finding per distinct line but keep them adjacent), checker (the id column), subsystem, description, and rank (the row number). ` +
    `${subFilter} Preserve PRIORITY.md's order (lowest rank number first). Use Read/Grep only; do not modify anything.`,
    { label: 'parse-priority', phase: 'Parse', schema: FINDINGS_SCHEMA, agentType: 'general-purpose' }
  )
  findings = (parsed && parsed.findings) || []
}

// Apply subsystem filter defensively + cap, logging what is dropped (no silent truncation).
if (SUBSYSTEMS) findings = findings.filter(f => SUBSYSTEMS.includes(f.subsystem))
const total = findings.length
if (findings.length > LIMIT) {
  log(`Capping at ${LIMIT} of ${total} findings (raise args.limit to do more; ${total - LIMIT} deferred).`)
  findings = findings.slice(0, LIMIT)
}
if (findings.length === 0) {
  log('No findings matched the scope. Nothing to do.')
  return { total: 0, processed: 0, note: 'no findings in scope' }
}
log(`Processing ${findings.length} finding(s): ${findings.map(f => f.subsystem + ':' + (f.file.split('/').pop()) + ':' + f.line).join(', ')}`)

// ---- Phase 2: one SOP-following agent per finding (propose-only, isolated worktree) ----
const digests = await pipeline(
  findings,
  (f, _orig, i) => {
    const base = f.file.split('/').pop().replace(/[^A-Za-z0-9]/g, '_')
    const reportPath = `${REPORTS}/${String(i).padStart(2, '0')}-${f.subsystem.replace('/', '_')}-${base}-${f.line}.json`
    const findingLine = `${f.file}:${f.line} | ${f.checker} | ${f.subsystem} | ${f.description}`
    return agent(
      `You have Read, Grep, Bash, and Edit tools — use them. ` +
      `Read the SOP at ${SOP} and follow it EXACTLY, end to end, for a single finding. ` +
      `The SOP is propose-only: create a detached git worktree under ${SCRATCH}, do ALL editing/analysis there, ` +
      `write NOTHING to ${REPO}'s working tree, never commit, never checkout the graviton branch, and remove the worktree before returning.\n\n` +
      `Parameters:\n` +
      `- finding: ${findingLine}\n` +
      `- repo_root: ${REPO}\n` +
      `- output_file: ${reportPath}\n` +
      `- scratch_root: ${SCRATCH}\n\n` +
      `Write the full JSON report to output_file. Return ONLY the short one-line summary the SOP specifies.`,
      { label: `fix:${f.subsystem}:${base}:${f.line}`, phase: 'Fix', schema: DIGEST_SCHEMA, agentType: 'general-purpose' }
    )
  }
)

const ok = digests.filter(Boolean)
const byClass = ok.reduce((m, d) => (m[d.classification] = (m[d.classification] || 0) + 1, m), {})
log(`Fix phase done: ${JSON.stringify(byClass)}`)

// ---- Phase 3: assemble a single review batch for human approval ----
phase('Review')
const realBugs = ok.filter(d => d.classification === 'real-bug')
const reviewPath = `${REPO}/graviton/audit/review/latest-review.md`
const assembler = await agent(
  `You have Read, Bash, Glob tools. Assemble a human review batch from the DeBeOS bug-fix reports in ${REPORTS} ` +
  `(one JSON file per finding; read them all). Produce a single markdown document and write it to ${reviewPath} ` +
  `(create the directory if needed with mkdir -p). The document MUST:\n` +
  `1. Open with a summary table: rank/file:line, classification, confidence, one-line root cause.\n` +
  `2. For every 'real-bug', include a section with: the root cause, the FULL unified diff (in a \`\`\`diff block), ` +
  `the proposed commit message, the static-verification result (finding cleared? new findings?), and the Graviton hardware-verification plan.\n` +
  `3. List 'false-positive' / 'needs-hardware' / 'needs-rework' / 'out-of-scope' / 'error' findings with their one-paragraph reason.\n` +
  `4. Collect all 'incidental_observations' into a "Parked / noticed en route" appendix.\n` +
  `5. End with an explicit reminder that NOTHING has been applied or committed — this is a propose-only batch awaiting approval, ` +
  `and that each approved diff must still pass its hardware plan on real Graviton before it ships in the canonical AMI.\n` +
  `Do NOT modify any source file or the git tree. Return a one-line summary: counts by classification and the review file path.`,
  { label: 'assemble-review', phase: 'Review', agentType: 'general-purpose' }
)

return {
  scope: { repo: REPO, subsystems: SUBSYSTEMS || 'all-active-surface', requested: total, processed: findings.length },
  classification_counts: byClass,
  real_bug_count: realBugs.length,
  review_file: reviewPath,
  assembler_summary: assembler,
  note: 'PROPOSE-ONLY: no files changed, nothing committed. Review the batch and approve diffs individually.',
}
