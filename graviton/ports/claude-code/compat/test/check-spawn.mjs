// Compares the Bun.spawn observations recorded by spawn-scenarios.mjs under stock
// Bun (argv[2]) and under Node + compat (argv[3]). Equality is exact JSON
// equality per scenario. The scenarios name no PIDs or timings (they record
// relations and buckets instead), so nothing is normalised here.
//
// DECLARED: features compat refuses on purpose. For these the Bun side is shown,
// and the Node side must be the explicit "not supported by the Node compat layer"
// error; they are counted apart from matches, never as matches.
import { readFileSync } from "node:fs";
const DECLARED = new Set(["terminal options object", "cgroup option", "ipc option", "stdio extra pipe", "resourceUsage"]);
const load = (f) => new Map(JSON.parse(readFileSync(f, "utf8")));
const A = load(process.argv[2]), B = load(process.argv[3]);
let ok = 0, bad = 0, declared = 0;
for (const [name, a] of A) {
	const b = B.get(name);
	const ja = JSON.stringify(a), jb = JSON.stringify(b);
	if (DECLARED.has(name)) {
		const refused = JSON.stringify(b).includes("is not supported by the Node compat layer");
		if (refused) { declared++; console.log("DECLARED", name, "\n  bun ", ja, "\n  node", jb); }
		else { bad++; console.log("MISMATCH (declared-unsupported scenario did not refuse)", name, "\n  bun ", ja, "\n  node", jb); }
		continue;
	}
	if (ja === jb) ok++;
	else { bad++; console.log("MISMATCH", name, "\n  bun ", ja, "\n  node", jb); }
}
for (const name of B.keys()) if (!A.has(name)) { bad++; console.log("MISMATCH only in node:", name); }
console.log(`spawn: ${ok} match / ${bad} mismatch / ${declared} declared-unsupported (${A.size} scenarios)`);
process.exit(bad ? 1 : 0);
