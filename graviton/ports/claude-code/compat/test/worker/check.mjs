// check.mjs <bun.json> <node.json>: compare scenario results by name. The one
// field compared loosely is an ErrorEvent's message for an uncaught throw: Bun's
// embeds a formatted source excerpt; the check requires the same "Name: message"
// line to appear in both (LIMITATIONS.md).
import { readFileSync } from "node:fs";
const [a, b] = process.argv.slice(2).map((f) => new Map(JSON.parse(readFileSync(f, "utf8"))));
let same = 0, diff = 0;
const loose = "uncaught throw in worker -> error event";
for (const [k, va] of a) {
	let vb = b.get(k);
	let A = va, B = vb;
	if (k === loose && va && vb && typeof va.message === "string" && typeof vb.message === "string") {
		const line = "TypeError: boom here";
		const ok = va.message.includes(line) && vb.message.includes(line);
		A = { ...va, message: ok }; B = { ...vb, message: ok };
	}
	if (JSON.stringify(A) === JSON.stringify(B)) { same++; console.log("SAME", k); }
	else { diff++; console.log("DIFF", k, "\n  bun: ", JSON.stringify(va), "\n  node:", JSON.stringify(vb)); }
}
for (const k of b.keys()) if (!a.has(k)) { diff++; console.log("EXTRA", k); }
console.log(`worker: ${same} match / ${diff} mismatch (${a.size} scenarios)`);
process.exit(diff ? 1 : 0);
