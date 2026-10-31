// Run under Node: property tests of CellSegmenter.segment() against the Bun
// ports it builds on (width.mjs: Bun's grapheme-break table and stringWidth,
// bit-exact with Bun 1.4.4; strip.mjs: Bun's stripANSI). The inputs are the
// existing corpora: the adversarial alphabets of gen-width-vectors.js and
// gen-slice-vectors.js, every ordered pair of each, plus the same 60,000
// xorshift sequences gen-width-vectors.js draws.
//
// Properties, per string s (segmenter with no substitutions):
//   P1 order:   the cells' graphemes, concatenated, are a subsequence of
//               stripANSI(s) with lone surrogates removed, in order (nothing
//               invented, nothing reordered).
//   P2 breaks:  no cell grapheme contains a grapheme break by width.mjs's
//               table (state carried as Bun does), and no cell holds a control.
//   P3 width:   a cell's width equals stringWidth(grapheme), except for the two
//               measured rules (cellseg.mjs clusterCpWidth): clusters with a
//               variation selector or a leading Prepend. Those are counted
//               and left to the real-runtime diff test, which pins them.
//   P4 reorder: with no strong right-to-left letter, reordered=true and false
//               give identical cells and runs.
import { readFileSync } from "node:fs";
import { CellSegmenter } from "../lib/cellseg.mjs";
import { stringWidth, graphemeBreak, fusedClassify } from "../lib/width.mjs";
import { stripANSI } from "../lib/strip.mjs";

const here = new URL(".", import.meta.url).pathname;
function alphabet(file) {	// the `const A = [...]` literal of a generator script
	const src = readFileSync(here + file, "utf8");
	const m = src.match(/^const A = (\[[\s\S]*?\]);$/m);
	if (!m) throw new Error("no alphabet in " + file);
	return new Function("return " + m[1])();
}
const corpora = [alphabet("gen-width-vectors.js"), alphabet("gen-slice-vectors.js")];
const inputs = [];
for (const A of corpora) for (const a of A) for (const b of A) inputs.push(a + b);
{	// gen-width-vectors.js's sequence loop, same seed and draws
	const A = corpora[0];
	let st = 0x9e3779b9n;
	const rnd = (n) => { st ^= st << 13n; st &= (1n << 64n) - 1n; st ^= st >> 7n; st ^= st << 17n; st &= (1n << 64n) - 1n; return Number(st % BigInt(n)); };
	for (let k = 0; k < 60000; k++) { let s = ""; const n = 1 + rnd(12); for (let i = 0; i < n; i++) s += A[rnd(A.length)]; inputs.push(s); }
}

const isControl = (cp) => cp < 0x20 || (cp >= 0x7f && cp <= 0x9f);
const isPrepend = (cp) => (fusedClassify(cp) & 0x1f) === 1;
const hasVS = (g) => /[︎️]/u.test(g);
const RTL = /[\p{Script=Hebrew}\p{Script=Arabic}\p{Script=Syriac}\p{Script=Thaana}\p{Script=Nko}]/u;
const fail = { P1: 0, P2: 0, P3: 0, P4: 0 };
const pass = { P1: 0, P2: 0, P3: 0, P3rule: 0, P4: 0 };
const examples = [];
const note = (p, s, why) => { fail[p]++; if (examples.length < 12) examples.push(`${p} ${JSON.stringify(s)}: ${why}`); };

for (const s of inputs) {
	const seg = new CellSegmenter({ substitute: [] });
	const cells = new Int32Array(4 * s.length + 8), runs = new Int32Array(4 * s.length + 8);
	const n = seg.segment(s, cells, runs, false);
	const gs = [], ws = [];
	for (let i = 0; i < n; i++) { gs.push(seg.graphemes[cells[2 * i]]); ws.push(cells[2 * i + 1] & 255); }
	// P1
	// lone surrogates are skipped units (a cluster continues across them)
	const plain = stripANSI(s).replace(/[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/g, "");
	let at = 0, ok1 = true;
	for (const g of gs) { const j = plain.indexOf(g, at); if (j < 0) { ok1 = false; break; } at = j + g.length; }
	ok1 ? pass.P1++ : note("P1", s, `cells ${JSON.stringify(gs)} not in order in ${JSON.stringify(plain)}`);
	// P2
	let ok2 = true;
	for (const g of gs) {
		if (g === "\t") continue;
		const cps = [...g].map((c) => c.codePointAt(0));
		if (cps.some(isControl)) { ok2 = false; break; }
		let state = 0;
		for (let k = 1; k < cps.length; k++) {
			const [brk, ns] = graphemeBreak(cps[k - 1], cps[k], state);
			state = ns;
			if (brk) { ok2 = false; break; }
		}
		if (!ok2) break;
	}
	ok2 ? pass.P2++ : note("P2", s, `a cell spans a grapheme break: ${JSON.stringify(gs)}`);
	// P3
	let ok3 = true;
	for (let i = 0; i < gs.length; i++) {
		const g = gs[i];
		if (g === "\t") continue;
		const cps = [...g].map((c) => c.codePointAt(0));
		if (hasVS(g) || isPrepend(cps[0])) {
			// a measured-rule cell: not stringWidth by design; its width is
			// pinned by the real-runtime diff test (check-cellseg.mjs) instead
			pass.P3rule++;
		} else if (ws[i] !== stringWidth(g)) { ok3 = false; note("P3", s, `cell ${JSON.stringify(g)} width ${ws[i]} vs stringWidth ${stringWidth(g)}`); break; }
	}
	if (ok3) pass.P3++;
	// P4
	if (!RTL.test(s)) {
		const seg2 = new CellSegmenter({ substitute: [] });
		const c2 = new Int32Array(cells.length), r2 = new Int32Array(runs.length);
		const n2 = seg2.segment(s, c2, r2, true);
		const same = n2 === n && c2.every((v, i) => v === cells[i]) && r2.every((v, i) => v === runs[i]);
		same ? pass.P4++ : note("P4", s, "reordered output differs without RTL text");
	}
}
console.log(examples.join("\n"));
console.log(`cellseg properties over ${inputs.length} corpus strings: ` +
	`P1 order ${pass.P1} pass / ${fail.P1} fail; P2 breaks ${pass.P2} / ${fail.P2}; ` +
	`P3 width ${pass.P3} strings pass (${pass.P3rule} cells via the measured rule) / ${fail.P3} fail; P4 reorder ${pass.P4} / ${fail.P4}`);
process.exit(fail.P1 + fail.P2 + fail.P3 + fail.P4 ? 1 : 0);
