// Compares the Bun.spawn observations recorded by spawn-scenarios.mjs under stock
// Bun (argv[2], the reference) with another run (argv[3]: Node + compat, on the
// same platform or on another one). The scenarios name no PIDs, timings or host
// paths (they record relations, buckets and placeholders instead). On the same
// platform equality is exact JSON equality per scenario; across platforms only
// the named differences of platform-diff.mjs are accepted, and each is printed.
//
// DECLARED: features compat refuses on purpose. For these the Bun side is shown,
// and the Node side must be the explicit "not supported by the Node compat layer"
// error; they are counted apart from matches, never as matches. The cgroup
// option is refused only on Linux: Bun honours it only where cgroups exist and
// ignores it elsewhere, and so does compat (spawn.mjs), so on another platform
// it is an ordinary scenario that must match.
import { readFileSync } from "node:fs";
import { splitRecording, compare, printNotes, KNOWN_DEFECTS } from "./platform-diff.mjs";
const load = (f) => splitRecording(JSON.parse(readFileSync(f, "utf8")));
const A = load(process.argv[2]), B = load(process.argv[3]);
const cross = A.meta.platform !== B.meta.platform;
console.log(`reference: ${A.meta.runtime} on ${A.meta.platform}; compared: ${B.meta.runtime} on ${B.meta.platform}${cross ? " (cross-platform: named platform differences apply)" : ""}`);
const DECLARED = new Set(["terminal options object", "ipc option", "stdio extra pipe", "resourceUsage"]);
if (B.meta.platform === "linux") DECLARED.add("cgroup option");
let ok = 0, bad = 0, declared = 0, platform = 0, unobs = 0;
for (const [name, a] of A.map) {
	if (!B.map.has(name)) { bad++; console.log("MISMATCH only in reference:", name); continue; }
	const b = B.map.get(name);
	const ja = JSON.stringify(a), jb = JSON.stringify(b);
	if (DECLARED.has(name)) {
		const refused = jb.includes("is not supported by the Node compat layer");
		if (refused) { declared++; console.log("DECLARED", name, "\n  bun ", ja, "\n  node", jb); }
		else { bad++; console.log("MISMATCH (declared-unsupported scenario did not refuse)", name, "\n  bun ", ja, "\n  node", jb); }
		continue;
	}
	const r = compare(a, b, A.meta, B.meta, name);
	if (r.same) { ok++; continue; }
	if (r.ok) {
		const kinds = [...new Set(r.notes.map((n) => n.kind))];
		const isUnobs = kinds.includes("unobservable");
		if (isUnobs) unobs++; else platform++;
		console.log(`${isUnobs ? "UNOBSERVABLE" : "PLATFORM"} [${kinds.join(", ")}]`, name);
		printNotes(r.notes);
		continue;
	}
	bad++;
	const defect = cross && KNOWN_DEFECTS[name] ? ` (known ${B.meta.platform} defect: ${KNOWN_DEFECTS[name]})` : "";
	console.log("MISMATCH" + defect, name, "\n  bun ", ja, "\n  node", jb);
	printNotes(r.notes.filter((n) => n.kind === "MISMATCH"));
}
for (const name of B.map.keys()) if (!A.map.has(name)) { bad++; console.log("MISMATCH only in compared run:", name); }
const extra = cross ? ` / ${platform} platform-difference / ${unobs} unobservable-on-${B.meta.platform}` : "";
console.log(`spawn: ${ok} match${extra} / ${bad} mismatch / ${declared} declared-unsupported (${A.map.size} scenarios)`);
process.exit(bad ? 1 : 0);
