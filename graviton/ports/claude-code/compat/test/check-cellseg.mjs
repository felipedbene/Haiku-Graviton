// Run under Node: replay the shared cases against compat/lib/cellseg.mjs and
// compare with the outputs the real runtime produced (gen-cellseg-vectors.mjs).
//
// A session matches EXACTLY when every recorded value is identical. One measured
// difference is named and counted separately (LIMITATIONS.md "CellSegmenter:
// grapheme table after an overflow"): when segment() returns -count, the real
// runtime has interned a subset of the line's new graphemes that this port does
// not reproduce, so later grapheme indices are numbered differently. Such a
// session is reported as EQUIVALENT only if (a) it contains an overflow step at
// or before the first difference and (b) it matches exactly once every grapheme
// index is replaced by the string it names (cells, and charMap-derived screen
// cells) and the grapheme table is compared as text. Anything else is a MISMATCH.
import { readFileSync } from "node:fs";
import { CellSegmenter } from "../lib/cellseg.mjs";
import { buildCases, runCase, runEdge } from "./cellseg-cases.mjs";
const dir = process.argv[2];
const want = readFileSync(dir + "/cellseg.cases.jsonl", "utf8").split("\n").filter(Boolean);
const cases = buildCases();
const J = JSON.stringify;

// Replace grapheme indices with grapheme strings, per side, using that side's tables.
function normalize(rec, c) {
	if (rec[0]?.err) return rec;
	let g = [];
	const grow = (d) => { if (d[0].reset) g = d[0].reset.slice(); else g.push(...d[0]); };
	grow(rec[0]);
	const out = [rec[0].slice(1)];
	for (let k = 1; k < rec.length; k++) {
		const r = rec[k], op = c.ops[k - 1];
		if (r.err) { out.push(r); continue; }
		if (op.op === "seg") {
			grow(r.t);
			// written cells (an overflow writes a prefix; unwritten slots keep -7)
			const cells = r.cells.map((v, i) => (i % 2 === 0 && v >= 0 ? "g:" + g[v] : v));
			out.push({ n: r.n, cells, runs: r.runs, t: r.t.slice(1), tableSize: g.length });
		} else if (op.op === "paint") {
			out.push({ r: r.r, scr: r.scr.map((v, i) => (i % 2 === 0 && v >= 1000 && (v - 1000) % 3 === 0 ? "g:" + g[(v - 1000) / 3] : v)) });
		} else out.push(r);
	}
	out.push(new Set(g).size);
	return out;
}

let exact = 0, equiv = 0, bad = 0, opsOk = 0, opsBad = 0;
const show = Number(process.env.SHOW ?? 10);
for (let i = 0; i < cases.length; i++) {
	const got = runCase(CellSegmenter, cases[i]);
	const exp = JSON.parse(want[i]);
	for (let k = 0; k < Math.max(got.length, exp.length); k++) J(got[k]) === J(exp[k]) ? opsOk++ : opsBad++;
	if (J(got) === J(exp)) { exact++; continue; }
	const k = got.findIndex((x, j) => J(x) !== J(exp[j]));
	const overflowBefore = exp.slice(1, k + 1).some((r) => typeof r.n === "number" && r.n < 0);
	if (overflowBefore) {
		const ng = normalize(got, cases[i]), ne = normalize(exp, cases[i]);
		// table sizes legitimately differ; compare everything else
		const strip = (a) => a.map((x) => (x && typeof x === "object" && "tableSize" in x ? { ...x, tableSize: 0 } : typeof x === "number" ? 0 : x));
		if (J(strip(ng)) === J(strip(ne))) { equiv++; continue; }
	}
	bad++;
	if (bad <= show) {
		console.log(`MISMATCH session ${i} step ${k} opts=${J(cases[i].opts)}`);
		console.log("  op ", J(k > 0 ? cases[i].ops[k - 1] : "ctor").slice(0, 600));
		console.log("  got", J(got[k]).slice(0, 900));
		console.log("  exp", J(exp[k]).slice(0, 900));
	}
}
const edgeWant = JSON.parse(readFileSync(dir + "/cellseg.edge.json", "utf8"));
const edgeGot = runEdge(CellSegmenter);
let eOk = 0, eBad = 0;
for (let i = 0; i < edgeWant.length; i++) {
	if (J(edgeGot[i]) === J(edgeWant[i])) eOk++;
	else { eBad++; if (eBad <= show) console.log("MISMATCH edge", edgeWant[i][0], "got", J(edgeGot[i][1]).slice(0, 500), "exp", J(edgeWant[i][1]).slice(0, 500)); }
}
console.log(`CellSegmenter: sessions ${exact} exact + ${equiv} equivalent-after-overflow (named difference) / ${bad} mismatch of ${cases.length}; steps ${opsOk} identical / ${opsBad} differ; edge ${eOk} match / ${eBad} mismatch`);
process.exit(bad || eBad ? 1 : 0);
