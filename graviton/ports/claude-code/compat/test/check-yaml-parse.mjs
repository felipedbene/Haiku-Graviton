// Run under Node: compare compat/lib/yaml-parse.mjs with the stock-Bun
// outcomes written by gen-yaml-parse-vectors.js.
//   node check-yaml-parse.mjs <dir-with-yaml-parse.cases.json>
import { readFileSync } from "node:fs";
import { parse } from "../lib/yaml-parse.mjs";
import { outcome, materialize } from "./yaml-parse-serialize.js";

const V = JSON.parse(readFileSync(process.argv[2] + "/yaml-parse.cases.json", "utf8"));
const show = (d) => JSON.stringify(typeof d === "string" && d.length > 300 ? d.slice(0, 300) + `...(${d.length} chars)` : d);
const cut = (s) => (s.length > 300 ? s.slice(0, 300) + `...(${s.length} chars)` : s);
let bad = 0;
for (const [kind, cases] of Object.entries(V)) {
	let kOk = 0, kBad = 0;
	for (const [d, exp] of cases) {
		const got = outcome(() => parse(materialize(d)));
		if (got === exp) { kOk++; continue; }
		kBad++;
		if (kBad <= 10) console.log("MISMATCH", kind, show(d), "\n  got", cut(got), "\n  exp", cut(exp));
	}
	bad += kBad;
	console.log(`yaml-parse.${kind}: ${kOk} match / ${kBad} mismatch`);
}
process.exit(bad ? 1 : 0);
