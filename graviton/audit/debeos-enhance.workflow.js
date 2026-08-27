export const meta = {
  name: 'debeos-enhance',
  description: 'Propose-only DeBeOS enhancement pass: parse the curated ENHANCEMENTS.md backlog, implement each enhancement as a FULL unified diff in an isolated worktree via the debeos-enhance SOP, then run a PRAGMATIC champion-vs-skeptic review (does it ship? concrete blocker?). Writes NOTHING to the tree, needs no AWS. Resumable via resumeFromRunId.',
  whenToUse: 'After ENHANCEMENTS.md exists. Choose scope with args.subsystems / args.limit. The enhancement analogue of debeos-bugfix: no false-positive branch, no prosecutor/adjudicator (an enhancement is chosen, not a maybe-noise finding); shorter, with a champion-vs-skeptic design debate instead. Never applies or commits; hardware proof is the separate gated debeos-hardware-proof runbook.',
  phases: [
    { title: 'Collect', detail: 'parse ENHANCEMENTS.md into candidates (filtered by subsystem/limit)' },
    { title: 'Design', detail: 'one SOP agent per enhancement, isolated worktree, produces a full diff (or needs-decomposition/reject)' },
    { title: 'Review', detail: 'pragmatic champion vs skeptic per implemented enhancement: does it deliver + is there a concrete blocker' },
    { title: 'Report', detail: 'scoreboard + review batch for human approval' },
  ],
}

// ---- Inputs ----
const REPO = (args && args.repo_root) || '/local/home/benfelip/Haiku-Graviton'
const SCRATCH = (args && args.scratch_root) || '/tmp/debeos-enhance'
const REPORTS = SCRATCH + '/reports'
const SOP = REPO + '/graviton/audit/agent-sops/debeos-enhance.sop.md'
const BACKLOG = (args && args.backlog_file) || (REPO + '/graviton/audit/ENHANCEMENTS.md')
const SUBSYSTEMS = (args && args.subsystems) || null
const LIMIT = (args && args.limit) || 8              // full diffs are heavier than one-line fixes; keep near the <15-agent guideline
const PREPARSED = (args && args.enhancements) || null
const ACTIVE = ['ena', 'network-stack', 'bfs', 'app_server-remote', 'kernel/arm64']

// ---- Schemas ----
const BACKLOG_SCHEMA = {
  type: 'object', additionalProperties: false, required: ['enhancements'],
  properties: {
    enhancements: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: false,
        required: ['id', 'title', 'subsystem', 'description', 'rationale', 'acceptance'],
        properties: {
          id: { type: 'string' },
          title: { type: 'string' },
          subsystem: { type: 'string', enum: ACTIVE },
          description: { type: 'string' },
          rationale: { type: 'string' },
          acceptance: { type: 'string', description: 'observable behaviour the hardware plan must show' },
        },
      },
    },
  },
}
const DESIGN_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['classification', 'id', 'subsystem', 'confidence', 'report_path'],
  properties: {
    classification: { type: 'string', enum: ['implemented', 'needs-decomposition', 'reject', 'out-of-scope', 'error'] },
    id: { type: 'string' }, subsystem: { type: 'string' },
    confidence: { type: 'string', enum: ['high', 'medium', 'low'] },
    report_path: { type: 'string' },
    one_line: { type: 'string' },
  },
}
// Pragmatic champion: judges SHIPPABILITY, not architectural ideology.
const CHAMPION_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['delivers_acceptance', 'ship'],
  properties: {
    delivers_acceptance: { type: 'boolean', description: 'does the diff actually meet the stated acceptance criterion?' },
    ship: { type: 'boolean', description: 'pragmatically worth shipping as-is (pending hardware proof)?' },
    value: { type: 'string', description: 'the concrete value it delivers' },
  },
}
// Pragmatic skeptic: raises ONLY concrete, material blockers -- not taste.
const SKEPTIC_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['blocking', 'concern'],
  properties: {
    blocking: { type: 'boolean', description: 'true ONLY for a concrete, material problem: a real regression, real over-reach beyond the enhancement, a genuinely simpler equivalent, or it does not deliver the acceptance criterion. NOT style/taste.' },
    concern_type: { type: 'string', enum: ['regression-risk', 'scope-creep', 'over-engineering', 'simpler-alternative', 'doesnt-deliver', 'maintenance-burden', 'none'] },
    concern: { type: 'string' },
    simpler_alternative: { type: 'string', description: 'the concretely simpler approach, if that is the concern; else empty' },
  },
}

// ---- Phase 1: Collect ----
phase('Collect')
let items = PREPARSED
if (!items) {
  const subFilter = SUBSYSTEMS ? `Only include enhancements whose subsystem is one of: ${SUBSYSTEMS.join(', ')}.` : 'Include all active-surface subsystems.'
  const parsed = await agent(
    `Read ${BACKLOG}. It is the curated DeBeOS enhancement backlog (a markdown table / entries). ` +
    `Extract each enhancement candidate as: id, title, subsystem (one of ${ACTIVE.join(', ')}), description (what to build), rationale (why it's worth it), acceptance (the observable behaviour that proves it works). ` +
    `${subFilter} Preserve the backlog's order. Use Read/Grep only; do not modify anything.`,
    { label: 'parse-backlog', phase: 'Collect', schema: BACKLOG_SCHEMA, agentType: 'general-purpose' }
  )
  items = (parsed && parsed.enhancements) || []
}
if (SUBSYSTEMS) items = items.filter(e => SUBSYSTEMS.includes(e.subsystem))
const total = items.length
if (items.length > LIMIT) {
  log(`Capping at ${LIMIT} of ${total} enhancements (raise args.limit; ${total - LIMIT} deferred).`)
  items = items.slice(0, LIMIT)
}
if (items.length === 0) {
  log('No enhancements matched the scope. Add entries to ENHANCEMENTS.md or widen the filter.')
  return { total: 0, processed: 0, note: 'no enhancements in scope' }
}
log(`Processing ${items.length} enhancement(s): ${items.map(e => e.id + ':' + e.subsystem).join(', ')}`)

// ---- Phases 2-3: Design -> Review (pipeline, no barrier) ----
const results = await pipeline(
  items,
  // Stage 1: SOP per enhancement (idempotent), produces a full diff or a classification.
  (e, _orig, i) => {
    const base = String(e.id).replace(/[^A-Za-z0-9]/g, '_')
    const reportPath = `${REPORTS}/${String(i).padStart(2, '0')}-${e.subsystem.replace('/', '_')}-${base}.json`
    const line = `${e.id} | ${e.subsystem} | ${e.title} | ${e.description} | rationale: ${e.rationale} | acceptance: ${e.acceptance}`
    return agent(
      `You have Read, Grep, Bash, Edit. FIRST run \`test -f ${reportPath} && cat ${reportPath}\`: if it exists with a "classification", this enhancement was already processed — return its one-line summary from the file and stop. Otherwise proceed:\n` +
      `Read the SOP at ${SOP} and follow it EXACTLY for this ONE enhancement. ` +
      `Propose-only: work in a detached git worktree under ${SCRATCH}, write NOTHING to ${REPO}'s tree, never commit, never checkout graviton, remove the worktree before returning.\n` +
      `Parameters:\n- enhancement: ${line}\n- repo_root: ${REPO}\n- output_file: ${reportPath}\n- scratch_root: ${SCRATCH}\n\n` +
      `Write the JSON report to output_file; return only the one-line summary (classification + id + subsystem + confidence).`,
      { label: `design:${e.subsystem}:${e.id}`, phase: 'Design', schema: DESIGN_SCHEMA, agentType: 'general-purpose' }
    )
  },
  // Stage 2: pragmatic champion-vs-skeptic, only for implemented enhancements.
  (digest, e, i) => {
    if (!digest) return { digest: null, kind: 'error', champion: null, skeptic: null }
    if (digest.classification !== 'implemented') {
      return { digest, kind: digest.classification, champion: null, skeptic: null }
    }
    const tag = `${digest.id}:${digest.subsystem}`
    return parallel([
      () => agent(
        `You have Read, Grep, Bash. You are the CHAMPION in a PRAGMATIC review of a proposed DeBeOS enhancement. Its full report (assessment, unified diff, static check, hardware plan) is at ${digest.report_path}; read it AND the real code under ${REPO}. ` +
        `Judge SHIPPABILITY, not architectural ideology: does the diff actually deliver the acceptance criterion "${e.acceptance}"? Is it pragmatically worth shipping as-is (pending hardware proof)? State the concrete value. Return the champion object.`,
        { label: `champion:${tag}`, phase: 'Review', schema: CHAMPION_SCHEMA, agentType: 'general-purpose' }
      ),
      () => agent(
        `You have Read, Grep, Bash. You are the SKEPTIC in a PRAGMATIC review of a proposed DeBeOS enhancement (report at ${digest.report_path}; read it AND the real code under ${REPO}). ` +
        `Raise a BLOCKING concern ONLY for something concrete and material: a real regression it introduces, real scope-creep/over-reach beyond the enhancement, a genuinely simpler equivalent that does the same job, or that it does NOT actually deliver the acceptance criterion "${e.acceptance}". ` +
        `Do NOT block on style, taste, or hypothetical purity — this is a pragmatic gate. If there is no material blocker, set blocking=false. Return the skeptic object.`,
        { label: `skeptic:${tag}`, phase: 'Review', schema: SKEPTIC_SCHEMA, agentType: 'general-purpose' }
      ),
    ]).then(([champion, skeptic]) => {
      // Pragmatic rule: ship unless the skeptic found a concrete blocker OR the diff doesn't deliver.
      const blocked = (skeptic && skeptic.blocking) || (champion && champion.delivers_acceptance === false)
      return { digest, kind: blocked ? 'needs-revision' : 'ship-ready', champion, skeptic }
    })
  }
)

const ok = results.filter(Boolean)
const cnt = ok.reduce((m, r) => { const k = r.kind || 'error'; m[k] = (m[k] || 0) + 1; return m }, {})
const shipReady = ok.filter(r => r.kind === 'ship-ready')
const needsRevision = ok.filter(r => r.kind === 'needs-revision')
const needsDecomp = ok.filter(r => r.kind === 'needs-decomposition')
log(`Design+Review done. kinds=${JSON.stringify(cnt)}; ship-ready=${shipReady.length}; needs-revision=${needsRevision.length}; needs-decomposition=${needsDecomp.length}`)

// ---- Phase 4: Report ----
phase('Report')
const reviewPath = `${REPO}/graviton/audit/review/enhance-review.md`
const scoreboardPath = `${REPO}/graviton/audit/review/enhance-scoreboard.md`
const outcomes = ok.map(r => ({
  id: r.digest && r.digest.id, subsystem: r.digest && r.digest.subsystem, kind: r.kind,
  delivers: r.champion ? r.champion.delivers_acceptance : null,
  value: r.champion ? r.champion.value : '',
  blocking: r.skeptic ? r.skeptic.blocking : null,
  concern_type: r.skeptic ? r.skeptic.concern_type : '',
  concern: r.skeptic ? r.skeptic.concern : '',
  simpler_alternative: r.skeptic ? r.skeptic.simpler_alternative : '',
}))
const assembler = await agent(
  `You have Read, Bash, Glob. Assemble the review artifacts from the DeBeOS enhancement reports in ${REPORTS} (one JSON per enhancement — read them all) plus these champion/skeptic outcomes: ${JSON.stringify(outcomes)}.\n` +
  `Kinds: 'ship-ready' (delivers the acceptance criterion, no material blocker), 'needs-revision' (skeptic found a concrete blocker or it doesn't deliver — the highest-value section to read), 'needs-decomposition' (worth it but too big — a proposed split), 'reject' (not worth it — rationale), plus out-of-scope/error.\n` +
  `Write TWO files (mkdir -p as needed):\n` +
  `1. ${scoreboardPath} — one-glance SCOREBOARD: total processed; counts by kind; ACTIONABLE lists in order: (a) ship-ready (id, subsystem, one-line value), (b) needs-revision (the skeptic's concern + simpler_alternative), (c) needs-decomposition (the proposed split), (d) rejects (why).\n` +
  `2. ${reviewPath} — full batch: for each ship-ready enhancement: the assessment + FULL unified diff (\`\`\`diff) + commit message + static-check + champion value + skeptic note + Graviton hardware plan; then needs-revision (diff + the blocking concern); then needs-decomposition (proposed split); then rejects; then a parked incidental-observations appendix.\n` +
  `End both with: PROPOSE-ONLY — nothing applied or committed; each ship-ready diff still needs its Graviton hardware plan before shipping. Do NOT modify any source or git state. Return a one-line summary with the counts and both file paths.`,
  { label: 'assemble-review', phase: 'Report', agentType: 'general-purpose' }
)

return {
  scope: { repo: REPO, subsystems: SUBSYSTEMS || 'all-active-surface', requested: total, processed: ok.length },
  kinds: cnt,
  ship_ready: shipReady.length,
  needs_revision: needsRevision.length,
  needs_decomposition: needsDecomp.length,
  scoreboard: scoreboardPath,
  review: reviewPath,
  assembler_summary: assembler,
  note: 'PROPOSE-ONLY enhancement pass: no files changed, nothing committed, no AWS used. Review the scoreboard first.',
}
