export const meta = {
  name: 'debeos-bugfix',
  description: 'Propose-only DeBeOS bug-fix pass (v2): one unified workflow for both a targeted slice (from PRIORITY.md) and an unattended overnight sweep (from collect_candidates.py). SOP-confirm each finding in an isolated worktree, two-sided adversarial verify (refuters attack real-bugs, weighted prosecutors attack false-positives), then ADJUDICATE every dispute to a verdict instead of dumping it on the human. Writes NOTHING to the tree, needs no AWS. Resumable via resumeFromRunId.',
  whenToUse: 'After PRIORITY.md / candidates.json exist. mode:"slice" for a small targeted set (args.subsystems/limit); mode:"overnight" for the full clustered sweep (args.findings or args.candidates_file). Never applies or commits; hardware proof is the separate gated debeos-hardware-proof runbook.',
  phases: [
    { title: 'Collect', detail: 'slice: parse PRIORITY.md (subsystem/limit) | overnight: load the deterministic clustered candidates' },
    { title: 'Fix', detail: 'one SOP agent per finding, isolated worktree, idempotent (skips finished reports)' },
    { title: 'Verify', detail: 'refuters attack each real-bug; weighted prosecutors attack each false-positive' },
    { title: 'Adjudicate', detail: 'a fresh judge resolves every disputed-FP to real-bug / false-positive / needs-hardware with reasoning' },
    { title: 'Report', detail: 'scoreboard + review batch, disputes already resolved' },
  ],
}

// ---- Inputs (all optional; sensible defaults) ----
const REPO = (args && args.repo_root) || '/local/home/benfelip/Haiku-Graviton'
const SCRATCH = (args && args.scratch_root) || '/tmp/debeos-bugfix'
const REPORTS = SCRATCH + '/reports'
const SOP = REPO + '/graviton/audit/agent-sops/debeos-bugfix.sop.md'
// Mode is inferred from what the caller supplies, or forced with args.mode.
const MODE = (args && args.mode) || ((args && (args.findings || args.candidates_file)) ? 'overnight' : 'slice')
const SUBSYSTEMS = (args && args.subsystems) || null   // slice: e.g. ['bfs','network-stack']; null = all active-surface
const LIMIT = (args && args.limit) || 10               // slice: cap agents; medium size guideline is <15
const REFUTERS = (args && args.refuters) || 2          // adversarial refuters (defense) per real-bug
const PROSECUTORS_BASE = (args && args.prosecutors) || 1   // baseline prosecutors (offense) per false-positive
const ADJUDICATORS = (args && args.adjudicators) || 1  // judges per disputed-FP (tie-breaker with full context)

// v2 lesson (2026-08-27 overnight run): the prosecutor found EVERY missed bug, and they hid in the
// down-ranked fp-prone/vendored findings on the highest-surface subsystems. So spend more offense
// exactly there. Refuters killed 0 real-bugs, so defense stays at the (cheaper) baseline.
const HIGH_SURFACE = new Set(['ena', 'network-stack'])
function prosecutorsFor(c) {
  let n = PROSECUTORS_BASE
  if (HIGH_SURFACE.has(c.subsystem)) n += 1
  if (c.fp_prone || c.vendored) n += 1     // the checkers/paths that historically hid real bugs behind an FP verdict
  return Math.min(n, 3)
}

const ACTIVE = ['ena', 'network-stack', 'bfs', 'app_server-remote', 'kernel/arm64']

// ---- Schemas ----
const FINDINGS_SCHEMA = {
  type: 'object', additionalProperties: false, required: ['findings'],
  properties: {
    findings: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: true, required: ['file', 'line', 'checker', 'subsystem', 'description'],
        properties: {
          file: { type: 'string', description: 'repo-relative path' },
          line: { type: 'integer' },
          checker: { type: 'string' },
          subsystem: { type: 'string', enum: ACTIVE },
          description: { type: 'string' },
          rank: { type: 'integer' },
          sibling_lines: { type: 'array', items: { type: 'integer' } },
          fp_prone: { type: 'boolean' },
          vendored: { type: 'boolean' },
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
    file: { type: 'string' }, line: { type: 'integer' },
    confidence: { type: 'string', enum: ['high', 'medium', 'low'] },
    report_path: { type: 'string' },
    one_line: { type: 'string' },
  },
}
const VERDICT_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['refuted', 'reason'],
  properties: {
    refuted: { type: 'boolean', description: 'true if the proposed fix does NOT hold up' },
    reason: { type: 'string' },
    failure_mode: { type: 'string', enum: ['over-reach', 'behavior-change', 'build-break', 'actually-false-positive', 'none'] },
  },
}
const PROSECUTOR_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['disputed', 'reason'],
  properties: {
    disputed: { type: 'boolean', description: 'true if a CREDIBLE path makes this dismissed finding an actual bug' },
    trigger_path: { type: 'string', description: 'concrete inputs / state / control-flow that would trigger it, or empty if none' },
    reason: { type: 'string' },
  },
}
// Adjudicator: given the dismissal AND the prosecutor's counter-argument, issues the FINAL verdict.
const ADJUDICATION_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['verdict', 'reasoning'],
  properties: {
    verdict: { type: 'string', enum: ['real-bug', 'false-positive', 'needs-hardware'] },
    reasoning: { type: 'string', description: 'Why the dismissal or the prosecutor wins, cited to file:line.' },
    root_cause_correction: { type: 'string', description: 'If the finding is real but the prosecutor mis-located the root cause, the corrected mechanism; else empty.' },
    fix_sketch: { type: 'string', description: 'Minimal fix if verdict is real-bug; else empty.' },
    priority: { type: 'string', enum: ['high', 'medium', 'low'] },
  },
}

// ---- Phase 1: Collect (mode-specific) ----
phase('Collect')
let CLUSTERS = (args && args.findings) || null
if (!CLUSTERS && MODE === 'overnight') {
  const cf = (args && args.candidates_file) || (REPO + '/graviton/audit/review/candidates.json')
  const loaded = await agent(
    `You have Bash and Read. Run \`cat ${cf}\`. It is a JSON array of clustered findings produced by a deterministic script. ` +
    `Return it as the \`findings\` array — copy EVERY element VERBATIM (fields include file, line, checker, subsystem, description, sibling_lines, fp_prone, vendored). ` +
    `Do NOT summarize, reorder, drop, or invent elements; the count you return MUST equal the number of elements in the file.`,
    { label: 'load-candidates', phase: 'Collect', schema: FINDINGS_SCHEMA, agentType: 'general-purpose' }
  )
  CLUSTERS = (loaded && loaded.findings) || null
}
if (!CLUSTERS && MODE === 'slice') {
  const subFilter = SUBSYSTEMS ? `Only include findings whose subsystem is one of: ${SUBSYSTEMS.join(', ')}.` : 'Include all active-surface subsystems.'
  const parsed = await agent(
    `Read ${REPO}/PRIORITY.md. It is the Phase-3 triage table for a DeBeOS static-analysis audit. ` +
    `Extract the ranked candidate findings (the numbered rows across the "Standouts" table and the per-subsystem tables). ` +
    `For each, return file (repo-relative, e.g. "src/add-ons/kernel/file_systems/bfs/Journal.cpp" — expand shorthand like "bfs/Journal.cpp" to the full repo path), ` +
    `line (the first line number listed; if a row lists several, emit one finding per distinct line but keep sibling_lines populated so they cluster), checker (the id column), subsystem, description, and rank (the row number). ` +
    `${subFilter} Preserve PRIORITY.md's order (lowest rank number first). Use Read/Grep only; do not modify anything.`,
    { label: 'parse-priority', phase: 'Collect', schema: FINDINGS_SCHEMA, agentType: 'general-purpose' }
  )
  CLUSTERS = (parsed && parsed.findings) || []
}

// Defensive subsystem filter + slice cap, logging what is dropped (no silent truncation).
if (SUBSYSTEMS) CLUSTERS = (CLUSTERS || []).filter(f => SUBSYSTEMS.includes(f.subsystem))
const total = (CLUSTERS || []).length
if (MODE === 'slice' && total > LIMIT) {
  log(`slice mode: capping at ${LIMIT} of ${total} findings (raise args.limit to do more; ${total - LIMIT} deferred).`)
  CLUSTERS = CLUSTERS.slice(0, LIMIT)
}
if (!CLUSTERS || CLUSTERS.length === 0) {
  log(MODE === 'overnight'
    ? 'No candidates. Run: python3 graviton/audit/collect_candidates.py FINDINGS.md > graviton/audit/review/candidates.json, then pass args.candidates_file or args.findings.'
    : 'No findings matched the scope. Nothing to do.')
  return { mode: MODE, total: 0, processed: 0, note: 'no findings in scope' }
}
log(`${MODE} run: ${CLUSTERS.length} finding(s), propose-only, ${REFUTERS} refuters/real-bug, prosecutors ${PROSECUTORS_BASE}..3 (weighted), ${ADJUDICATORS} adjudicator/dispute.`)

// ---- Phases 2-4: Fix -> Verify -> Adjudicate, one pipeline (no barriers) ----
const results = await pipeline(
  CLUSTERS,
  // Stage 1: SOP per finding (idempotent).
  (c, _orig, i) => {
    const base = c.file.split('/').pop().replace(/[^A-Za-z0-9]/g, '_')
    const reportPath = `${REPORTS}/${String(i).padStart(3, '0')}-${c.subsystem.replace('/', '_')}-${base}-${c.line}.json`
    const sibs = (c.sibling_lines && c.sibling_lines.length > 1) ? ` (sibling lines in same cluster: ${c.sibling_lines.join(', ')} — cover them together, one diff)` : ''
    const findingLine = `${c.file}:${c.line} | ${c.checker} | ${c.subsystem} | ${c.description}${sibs}`
    return agent(
      `You have Read, Grep, Bash, Edit. FIRST run \`test -f ${reportPath} && cat ${reportPath}\`: if that file already exists and its JSON has a "classification", this finding was already processed — do NOT re-analyze, just return its one-line summary (classification, file, line, confidence, report_path) from the file and stop. Otherwise proceed:\n` +
      `Read the SOP at ${SOP} and follow it EXACTLY for this ONE finding. ` +
      `Propose-only: work in a detached git worktree under ${SCRATCH}, write NOTHING to ${REPO}'s tree, never commit, never checkout graviton, remove the worktree before returning.\n` +
      `Parameters:\n- finding: ${findingLine}\n- repo_root: ${REPO}\n- output_file: ${reportPath}\n- scratch_root: ${SCRATCH}\n\n` +
      `Note: this finding may be a known-FP-prone cppcheck value-flow check on vendored/BSD code (${c.fp_prone ? 'FLAGGED fp-prone' : 'not flagged'}${c.vendored ? ', vendored path' : ''}) — judge on the code, do not assume. Write the JSON report to output_file; return only the one-line summary.`,
      { label: `fix:${c.subsystem}:${base}:${c.line}`, phase: 'Fix', schema: DIGEST_SCHEMA, agentType: 'general-purpose' }
    )
  },
  // Stage 2: two-sided adversarial verify.
  (digest, c, i) => {
    if (!digest) return { digest: null, kind: 'error', verdicts: [], prosecutions: [], verifiedReal: false, disputedFP: false }
    const fl = `${(digest.file || '').split('/').pop()}:${digest.line}`
    if (digest.classification === 'real-bug') {
      // DEFENSE: refuters try to break the fix; majority-refute kills it.
      return parallel(Array.from({ length: REFUTERS }, (_, k) => () =>
        agent(
          `You have Read, Grep, Bash. An automated pass proposed a fix for a DeBeOS bug; its full report (root cause, unified diff, static verification) is at ${digest.report_path}. ` +
          `Adversarially try to REFUTE the fix: read the report AND the real code under ${REPO}. Is the "bug" actually a false positive? Does the diff over-reach, change behavior, or risk breaking the build? ` +
          `Be skeptical — default refuted=true if genuinely uncertain. Return the verdict object.`,
          { label: `refute:${fl}:${k}`, phase: 'Verify', schema: VERDICT_SCHEMA, agentType: 'general-purpose' }
        )
      )).then(vs => {
        const v = vs.filter(Boolean)
        const survives = v.filter(x => x.refuted).length < Math.ceil(REFUTERS / 2)
        return { digest, kind: survives ? 'real-bug' : 'refuted-real', verdicts: v, prosecutions: [], verifiedReal: survives, disputedFP: false }
      })
    }
    if (digest.classification === 'false-positive') {
      // OFFENSE (weighted): prosecutor(s) argue it is actually real; a credible path => disputed-FP.
      const nP = prosecutorsFor(c)
      return parallel(Array.from({ length: nP }, (_, k) => () =>
        agent(
          `You have Read, Grep, Bash. An automated pass dismissed a static-analysis finding as FALSE-POSITIVE; its report (why it was dismissed) is at ${digest.report_path}. ` +
          `Adversarially argue the OPPOSITE: that the finding (${c.checker} at ${digest.file}:${digest.line}) is a REAL bug. Read the report AND the real code under ${REPO}. ` +
          `Try to construct a concrete triggering path (inputs / state / call sequence) where it actually bites — including callers, error paths, lock scopes, ARM64 weak-memory reordering, and non-obvious inputs the dismissal may have overlooked. ` +
          `Set disputed=true ONLY if you found a genuinely credible path (put it in trigger_path); if the dismissal holds up, disputed=false. Do not manufacture doubt. Return the verdict object.`,
          { label: `prosecute:${fl}:${k}`, phase: 'Verify', schema: PROSECUTOR_SCHEMA, agentType: 'general-purpose' }
        )
      )).then(ps => {
        const p = ps.filter(Boolean)
        const disputed = p.some(x => x.disputed)
        return { digest, kind: disputed ? 'disputed-FP' : 'false-positive', verdicts: [], prosecutions: p, verifiedReal: false, disputedFP: disputed }
      })
    }
    return { digest, kind: digest.classification, verdicts: [], prosecutions: [], verifiedReal: false, disputedFP: false }
  },
  // Stage 3: ADJUDICATE disputes (v2). Resolve each disputed-FP to a verdict instead of leaving it for the human.
  (r, c, i) => {
    if (!r || !r.disputedFP) return r
    const fl = `${(r.digest.file || '').split('/').pop()}:${r.digest.line}`
    const triggers = (r.prosecutions || []).filter(p => p.disputed).map(p => p.trigger_path).filter(Boolean)
    return parallel(Array.from({ length: ADJUDICATORS }, (_, k) => () =>
      agent(
        `You are a senior kernel engineer acting as the FINAL judge on a disputed static-analysis finding. Two automated passes disagree:\n` +
        `- A fix-agent classified ${c.checker} at ${r.digest.file}:${r.digest.line} as FALSE-POSITIVE. Its full reasoning is in the report at ${r.digest.report_path} (read it).\n` +
        `- A prosecutor argued it is a REAL bug via this path: ${JSON.stringify(triggers)}.\n\n` +
        `You have Read, Grep, Bash. Read the ACTUAL code under ${REPO} yourself — the function, its callers, the lock scopes, and (for arm64) any weak-memory reordering that matters. Do NOT trust either side; decide from the source.\n` +
        `Rule to ONE verdict: 'real-bug' (a genuine defect — say whether the prosecutor located the root cause correctly, and if not, give the corrected mechanism in root_cause_correction), 'false-positive' (the dismissal wins; the prosecutor's path is not actually reachable), or 'needs-hardware' (only a Graviton run can decide). ` +
        `If 'real-bug', give the minimal fix in fix_sketch and a priority. Cite file:line throughout. Be decisive.`,
        { label: `adjudicate:${fl}:${k}`, phase: 'Adjudicate', schema: ADJUDICATION_SCHEMA, agentType: 'general-purpose' }
      )
    )).then(js => {
      const j = js.filter(Boolean)
      // Majority verdict; ties or any 'real-bug' escalate toward review-worthy.
      const tally = j.reduce((m, x) => (m[x.verdict] = (m[x.verdict] || 0) + 1, m), {})
      const verdict = j.length
        ? (tally['real-bug'] >= Math.ceil(j.length / 2) ? 'real-bug'
          : tally['needs-hardware'] ? 'needs-hardware'
          : 'false-positive')
        : 'disputed-unresolved'
      const kind = verdict === 'real-bug' ? 'adjudicated-real'
        : verdict === 'needs-hardware' ? 'adjudicated-needs-hardware'
        : verdict === 'false-positive' ? 'adjudicated-fp'
        : 'disputed-FP'
      return { ...r, kind, adjudications: j, adjudicated_verdict: verdict, verifiedReal: verdict === 'real-bug' }
    })
  }
)

const ok = results.filter(Boolean)
const cnt = ok.reduce((m, r) => { const k = r.kind || 'error'; m[k] = (m[k] || 0) + 1; return m }, {})
const confirmedReal = ok.filter(r => r.verifiedReal)                       // survived-refute real-bugs + adjudicated-real
const refutedReal = ok.filter(r => r.kind === 'refuted-real')
const adjudicatedReal = ok.filter(r => r.kind === 'adjudicated-real')      // disputes the judge PROMOTED to real
const adjudicatedNH = ok.filter(r => r.kind === 'adjudicated-needs-hardware')
const stillDisputed = ok.filter(r => r.kind === 'disputed-FP')             // adjudicator couldn't run/resolve -> human
log(`Fix+Verify+Adjudicate done. kinds=${JSON.stringify(cnt)}; real (confirmed+adjudicated)=${confirmedReal.length}; refuted=${refutedReal.length}; adjudicated-real=${adjudicatedReal.length}; needs-hardware=${adjudicatedNH.length}; unresolved-disputes=${stillDisputed.length}`)

// ---- Phase 5: Report ----
phase('Report')
const reviewPath = `${REPO}/graviton/audit/review/${MODE}-review.md`
const scoreboardPath = `${REPO}/graviton/audit/review/${MODE}-scoreboard.md`
const outcomes = ok.map(r => ({
  file: r.digest && r.digest.file, line: r.digest && r.digest.line, kind: r.kind,
  verifiedReal: r.verifiedReal,
  refuted_by: (r.verdicts || []).filter(v => v.refuted).length,
  dispute: (r.prosecutions || []).filter(p => p.disputed).map(p => p.trigger_path).slice(0, 1)[0] || '',
  adjudicated_verdict: r.adjudicated_verdict || '',
  adjudication_reasoning: (r.adjudications || []).map(a => a.reasoning).slice(0, 1)[0] || '',
  fix_sketch: (r.adjudications || []).map(a => a.fix_sketch).filter(Boolean).slice(0, 1)[0] || '',
  root_cause_correction: (r.adjudications || []).map(a => a.root_cause_correction).filter(Boolean).slice(0, 1)[0] || '',
}))
const assembler = await agent(
  `You have Read, Bash, Glob. Assemble the review artifacts from the DeBeOS bug-fix reports in ${REPORTS} (one JSON per finding — read them all) plus these verify+adjudicate outcomes: ${JSON.stringify(outcomes)}.\n` +
  `Roles: real-bugs were attacked by REFUTERS (kind 'real-bug' survived, 'refuted-real' killed). False-positives were attacked by weighted PROSECUTORS; every dispute was then resolved by an ADJUDICATOR into 'adjudicated-real' (a MISSED bug the fix-agent got wrong — highest value), 'adjudicated-needs-hardware', or 'adjudicated-fp' (dismissal upheld). 'disputed-FP' means the adjudicator could not run — needs a human.\n` +
  `Write TWO files (mkdir -p as needed):\n` +
  `1. ${scoreboardPath} — one-glance SCOREBOARD: total processed; counts by kind; ACTIONABLE lists in priority order: (a) real-bugs that survived refute, (b) **adjudicated-real** (with the adjudicator's corrected root cause + fix_sketch + priority — these were missed by the first pass), (c) adjudicated-needs-hardware, (d) refuted real-bugs (why), (e) any unresolved disputed-FP, (f) plain false-positive count + rate.\n` +
  `2. ${reviewPath} — full batch: each surviving/adjudicated real-bug with root cause + full unified diff (\`\`\`diff) + commit message + static-verification + verdicts + Graviton hardware plan; then needs-hardware; then plain false-positives (one-line reasons); then a parked incidental-observations appendix.\n` +
  `End both with: PROPOSE-ONLY — nothing applied or committed; each real-bug diff still needs its Graviton hardware plan before shipping. Do NOT modify any source or git state. Return a one-line summary with the counts and both file paths.`,
  { label: 'assemble-review', phase: 'Report', agentType: 'general-purpose' }
)

return {
  mode: MODE,
  scope: { repo: REPO, subsystems: SUBSYSTEMS || 'all-active-surface', findings: CLUSTERS.length, processed: ok.length },
  kinds: cnt,
  real_bugs_total: confirmedReal.length,
  adjudicated_real: adjudicatedReal.length,
  adjudicated_needs_hardware: adjudicatedNH.length,
  real_bugs_refuted: refutedReal.length,
  unresolved_disputes: stillDisputed.length,
  false_positive_rate: ok.length ? +(((cnt['false-positive'] || 0) + (cnt['adjudicated-fp'] || 0)) / ok.length).toFixed(2) : null,
  scoreboard: scoreboardPath,
  review: reviewPath,
  assembler_summary: assembler,
  note: 'PROPOSE-ONLY: no files changed, nothing committed, no AWS used. Review the scoreboard first; disputes are already adjudicated.',
}
