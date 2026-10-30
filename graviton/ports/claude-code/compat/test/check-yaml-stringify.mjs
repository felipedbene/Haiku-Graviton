// Run under Node: compares compat/lib/yaml-stringify.mjs with the stock Bun vectors.
import { readFileSync } from "node:fs";
import { run, runDivergent, maxDepth } from "./yaml-stringify-corpus.js";
import { stringify } from "../lib/yaml-stringify.mjs";
const exp = JSON.parse(readFileSync(process.argv[2] + "/yaml-stringify.cases.json", "utf8"));
const got = run(stringify, 2000);
if (got.length !== exp.cases.length) { console.log(`case count differs: node ${got.length}, bun ${exp.cases.length}`); process.exit(1); }
const clip = (x) => { const s = JSON.stringify(x); return s.length > 600 ? s.slice(0, 600) + `...(${s.length} chars)` : s; };
let ok = 0, bad = 0;
const groups = {};
got.forEach(([name, res, log], i) => {
	const [eName, eRes, eLog] = exp.cases[i];
	const g = name.startsWith("fuzz ") ? "fuzz" : "corpus";
	groups[g] ??= [0, 0];
	if (name === eName && JSON.stringify(res) === JSON.stringify(eRes) && JSON.stringify(log) === JSON.stringify(eLog)) { ok++; groups[g][0]++; }
	else { bad++; groups[g][1]++; if (bad <= 25) console.log("MISMATCH", JSON.stringify(name), "\n  got", clip(res), clip(log), "\n  exp", JSON.stringify(eName), clip(eRes), clip(eLog)); }
});
for (const [g, [o, b]] of Object.entries(groups)) console.log(`YAML.stringify ${g}: ${o} match / ${b} mismatch`);
console.log(`YAML.stringify total: ${ok} match / ${bad} mismatch`);
console.log("known divergences (not counted; Bun output depends on JSC value representation or engine error text):");
runDivergent(stringify).forEach(([name, s2, s0], i) => {
	const [, e2, e0] = exp.divergent[i];
	console.log(`  ${name}\n    space=2  node ${JSON.stringify(s2)}  bun ${JSON.stringify(e2)}\n    minified node ${JSON.stringify(s0)}  bun ${JSON.stringify(e0)}`);
});
console.log("max nesting depth: node", JSON.stringify(maxDepth(stringify)), "bun", JSON.stringify(exp.maxDepth));
process.exit(bad ? 1 : 0);
