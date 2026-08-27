export const meta = {
  name: 'debeos-bugfix-overnight',
  description: 'Unattended propose-only overnight pass: SOP-confirm each clustered finding in an isolated worktree, adversarially verify every real-bug, assemble a morning scoreboard. Writes NOTHING to the tree, needs no AWS. Resumable via resumeFromRunId.',
  whenToUse: 'Overnight/unattended. Feed args.findings from graviton/audit/collect_candidates.py (deterministic collect+cluster). Never applies or commits; hardware proof is a separate daytime gated step.',
  phases: [
    { title: 'Collect', detail: 'load the deterministic clustered candidate list' },
    { title: 'Fix', detail: 'one SOP agent per clustered finding, isolated worktree' },
    { title: 'Verify', detail: 'refuters attack each real-bug; a prosecutor attacks each false-positive (disputed-FP -> human review)' },
    { title: 'Report', detail: 'morning scoreboard + review batch' },
  ],
}

const REPO = (args && args.repo_root) || '/local/home/benfelip/Haiku-Graviton'
const SCRATCH = (args && args.scratch_root) || '/tmp/debeos-bugfix'
const REPORTS = SCRATCH + '/reports'
const SOP = REPO + '/graviton/audit/agent-sops/debeos-bugfix.sop.md'
const REFUTERS = (args && args.refuters) || 2            // adversarial refuters (defense) per real-bug
const PROSECUTORS = (args && args.prosecutors) || 1     // adversarial prosecutors (offense) per false-positive
const FINDINGS_ARRAY_SCHEMA = {
  type: 'object', additionalProperties: false, required: ['findings'],
  properties: {
    findings: {
      type: 'array',
      items: {
        type: 'object', additionalProperties: true, required: ['file', 'line', 'checker', 'subsystem', 'description'],
        properties: {
          file: { type: 'string' }, line: { type: 'integer' }, checker: { type: 'string' },
          subsystem: { type: 'string' }, description: { type: 'string' },
          sibling_lines: { type: 'array', items: { type: 'integer' } },
          fp_prone: { type: 'boolean' }, vendored: { type: 'boolean' },
        },
      },
    },
  },
}

let CLUSTERS = (args && args.findings) || null
if (!CLUSTERS) {
  const cf = (args && args.candidates_file) || (REPO + '/graviton/audit/review/candidates.json')
  phase('Collect')
  const loaded = await agent(
    `You have Bash and Read. Run \`cat ${cf}\`. It is a JSON array of clustered findings produced by a deterministic script. ` +
    `Return it as the \`findings\` array — copy EVERY element VERBATIM (fields include file, line, checker, subsystem, description, sibling_lines, fp_prone, vendored). ` +
    `Do NOT summarize, reorder, drop, or invent elements; the count you return MUST equal the number of elements in the file.`,
    { label: 'load-candidates', phase: 'Collect', schema: FINDINGS_ARRAY_SCHEMA, agentType: 'general-purpose' }
  )
  CLUSTERS = (loaded && loaded.findings) || null
}

if (!CLUSTERS || !CLUSTERS.length) {
  log('No candidates. Run: python3 graviton/audit/collect_candidates.py FINDINGS.md > graviton/audit/review/candidates.json, then pass args.candidates_file or args.findings.')
  return { error: 'no candidates', processed: 0 }
}
log(`Overnight run: ${CLUSTERS.length} clustered findings, propose-only, ${REFUTERS} refuters per real-bug.`)

const DIGEST_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['classification', 'file', 'line', 'confidence', 'report_path'],
  properties: {
    classification: { type: 'string', enum: ['real-bug','false-positive','needs-hardware','needs-rework','out-of-scope','error'] },
    file: { type: 'string' }, line: { type: 'integer' },
    confidence: { type: 'string', enum: ['high','medium','low'] },
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
    failure_mode: { type: 'string', enum: ['over-reach','behavior-change','build-break','actually-false-positive','none'] },
  },
}
// Prosecutor: argues a "false-positive" dismissal is WRONG — the finding is actually a real bug.
const PROSECUTOR_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['disputed', 'reason'],
  properties: {
    disputed: { type: 'boolean', description: 'true if a CREDIBLE path makes this dismissed finding an actual bug' },
    trigger_path: { type: 'string', description: 'concrete inputs / state / control-flow that would trigger it, or empty if none found' },
    reason: { type: 'string' },
  },
}

// Fix (SOP per cluster) -> Verify (adversarial refuters only if real-bug). Pipeline: no barrier.
const results = await pipeline(
  CLUSTERS,
  (c, _orig, i) => {
    const base = c.file.split('/').pop().replace(/[^A-Za-z0-9]/g, '_')
    const reportPath = `${REPORTS}/${String(i).padStart(3,'0')}-${c.subsystem.replace('/','_')}-${base}-${c.line}.json`
    const sibs = (c.sibling_lines && c.sibling_lines.length > 1) ? ` (sibling lines in same cluster: ${c.sibling_lines.join(', ')} — cover them together, one diff)` : ''
    const findingLine = `${c.file}:${c.line} | ${c.checker} | ${c.subsystem} | ${c.description}${sibs}`
    return agent(
      `You have Read, Grep, Bash, Edit. FIRST run \`test -f ${reportPath} && cat ${reportPath}\`: if that file already exists and its JSON has a "classification", this finding was already processed — do NOT re-analyze, just return its one-line summary (classification, file, line, confidence, report_path) from the file and stop. Otherwise proceed:\n` +
      `Read the SOP at ${SOP} and follow it EXACTLY for this ONE clustered finding. ` +
      `Propose-only: work in a detached git worktree under ${SCRATCH}, write NOTHING to ${REPO}'s tree, never commit, never checkout graviton, remove the worktree before returning.\n` +
      `Parameters:\n- finding: ${findingLine}\n- repo_root: ${REPO}\n- output_file: ${reportPath}\n- scratch_root: ${SCRATCH}\n\n` +
      `Note: this finding may be a known-FP-prone cppcheck value-flow check on vendored/BSD code (${c.fp_prone ? 'FLAGGED fp-prone' : 'not flagged'}${c.vendored ? ', vendored path' : ''}) — judge on the code, do not assume. Write the JSON report to output_file; return only the one-line summary.`,
      { label: `fix:${c.subsystem}:${base}:${c.line}`, phase: 'Fix', schema: DIGEST_SCHEMA, agentType: 'general-purpose' }
    )
  },
  (digest, c, i) => {
    if (!digest) return { digest: null, kind: 'error', verdicts: [], verifiedReal: false, disputedFP: false }
    const fl = `${(digest.file||'').split('/').pop()}:${digest.line}`
    // real-bug -> DEFENSE: N refuters try to break the fix; majority-refute kills it.
    if (digest.classification === 'real-bug') {
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
        return { digest, kind: survives ? 'real-bug' : 'refuted-real', verdicts: v, verifiedReal: survives, disputedFP: false }
      })
    }
    // false-positive -> OFFENSE: prosecutor(s) argue it is actually real; a credible path => disputed-FP for human review.
    if (digest.classification === 'false-positive') {
      return parallel(Array.from({ length: PROSECUTORS }, (_, k) => () =>
        agent(
          `You have Read, Grep, Bash. An automated pass dismissed a static-analysis finding as FALSE-POSITIVE; its report (why it was dismissed) is at ${digest.report_path}. ` +
          `Adversarially argue the OPPOSITE: that the finding (${c.checker} at ${digest.file}:${digest.line}) is a REAL bug. Read the report AND the real code under ${REPO}. ` +
          `Try to construct a concrete triggering path (inputs / state / call sequence) where it actually bites — including callers, error paths, and non-obvious inputs the dismissal may have overlooked. ` +
          `Set disputed=true ONLY if you found a genuinely credible path (put it in trigger_path); if the dismissal holds up, disputed=false. Do not manufacture doubt. Return the verdict object.`,
          { label: `prosecute:${fl}:${k}`, phase: 'Verify', schema: PROSECUTOR_SCHEMA, agentType: 'general-purpose' }
        )
      )).then(ps => {
        const p = ps.filter(Boolean)
        const disputed = p.some(x => x.disputed)
        return { digest, kind: disputed ? 'disputed-FP' : 'false-positive', prosecutions: p, verifiedReal: false, disputedFP: disputed }
      })
    }
    return { digest, kind: digest.classification, verdicts: [], verifiedReal: false, disputedFP: false }
  }
)

const ok = results.filter(Boolean)
const cnt = ok.reduce((m, r) => { const k = r.kind || 'error'; m[k] = (m[k] || 0) + 1; return m }, {})
const confirmedReal = ok.filter(r => r.verifiedReal)
const refutedReal = ok.filter(r => r.kind === 'refuted-real')
const disputedFPs = ok.filter(r => r.disputedFP)   // FPs the prosecutor thinks are actually real -> human review
log(`Fix+Verify done. post-verify kinds=${JSON.stringify(cnt)}; real-bugs surviving=${confirmedReal.length}; refuted=${refutedReal.length}; DISPUTED false-positives (need review)=${disputedFPs.length}`)

phase('Report')
const reviewPath = `${REPO}/graviton/audit/review/overnight-review.md`
const scoreboardPath = `${REPO}/graviton/audit/review/overnight-scoreboard.md`
const verifyOutcomes = ok.map(r => ({
  file: r.digest && r.digest.file, line: r.digest && r.digest.line,
  kind: r.kind, verifiedReal: r.verifiedReal, disputedFP: r.disputedFP,
  refuted_by: (r.verdicts || []).filter(v => v.refuted).length,
  dispute: (r.prosecutions || []).filter(p => p.disputed).map(p => p.trigger_path).slice(0, 1)[0] || '',
}))
const assembler = await agent(
  `You have Read, Bash, Glob. Assemble the morning artifacts from the DeBeOS overnight bug-fix reports in ${REPORTS} (one JSON per finding — read them all) plus these adversarial-verify outcomes: ` +
  `${JSON.stringify(verifyOutcomes)}. ` +
  `Note the two adversarial roles: real-bugs were attacked by REFUTERS (kind 'real-bug' survived, 'refuted-real' was killed); false-positives were attacked by a PROSECUTOR (kind 'disputed-FP' means the prosecutor found a credible path that it may be a REAL bug after all — these need human review).\n` +
  `Write TWO files (mkdir -p as needed):\n` +
  `1. ${scoreboardPath} — a one-glance SCOREBOARD: total processed; counts by post-verify kind; the ACTIONABLE lists in priority order: (a) real-bugs that SURVIVED refute, (b) **DISPUTED false-positives** (prosecutor's trigger_path — the highest-value section, since these are candidate MISSED bugs), (c) real-bugs REFUTED (with why), (d) plain false-positives count + rate, (e) error/needs-rework.\n` +
  `2. ${reviewPath} — the full batch: for each SURVIVING real-bug: root cause + full unified diff (\`\`\`diff) + commit message + static-verification + refuter verdicts + Graviton hardware plan. Then each DISPUTED-FP: the original dismissal + the prosecutor's counter-argument/trigger_path + a recommendation (re-open? confirm FP?). Then refuted real-bugs, then plain false-positives (one-line reasons), then a parked incidental-observations appendix.\n` +
  `End both with: PROPOSE-ONLY — nothing applied or committed; each surviving diff still needs its Graviton hardware plan before shipping. Do NOT modify any source or git state. Return a one-line summary with the counts (including disputed-FP) and both file paths.`,
  { label: 'assemble-morning', phase: 'Report', agentType: 'general-purpose' }
)

return {
  scope: { repo: REPO, clustered_findings: CLUSTERS.length, processed: ok.length },
  post_verify_kinds: cnt,
  real_bugs_confirmed: confirmedReal.length,
  real_bugs_refuted: refutedReal.length,
  disputed_false_positives: disputedFPs.length,
  false_positive_rate: ok.length ? +(( (cnt['false-positive']||0) / ok.length ).toFixed(2)) : null,
  scoreboard: scoreboardPath,
  review: reviewPath,
  assembler_summary: assembler,
  note: 'PROPOSE-ONLY overnight run: no files changed, nothing committed, no AWS used. Review the scoreboard first.',
}
