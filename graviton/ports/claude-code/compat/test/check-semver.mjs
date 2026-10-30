import { readFileSync } from "node:fs";
import { order, satisfies } from "../lib/semver.mjs";
const V = JSON.parse(readFileSync(process.argv[2] + "/semver.cases.json", "utf8"));
const T = (f) => { try { return { v: f() }; } catch (e) { return { e: e.message }; } };
let ok = 0, bad = 0;
for (const [kind, fn] of [["order", order], ["satisfies", satisfies]]) {
	let kOk = 0, kBad = 0;
	for (const [a, b, exp] of V[kind]) {
		const got = T(() => fn(a, b));
		if (JSON.stringify(got) === JSON.stringify(exp)) { ok++; kOk++; }
		else { bad++; kBad++; if (kBad <= 10) console.log("MISMATCH", kind, JSON.stringify([a, b]), "got", JSON.stringify(got), "exp", JSON.stringify(exp)); }
	}
	console.log(`semver.${kind}: ${kOk} match / ${kBad} mismatch`);
}
process.exit(bad ? 1 : 0);
