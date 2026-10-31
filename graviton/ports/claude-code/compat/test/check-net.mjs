// Runs the shared net scenarios under Node with compat/lib/net.mjs and compares
// each scenario's observations with what stock Bun recorded (gen-net-vectors.js).
// On the recording's platform equality is exact; on another platform only the
// named differences of platform-diff.mjs are accepted, and each is printed.
import { readFileSync } from "node:fs";
import http from "node:http";
import os from "node:os";
import { runAll } from "./net-scenarios.js";
import { splitRecording, compare, printNotes, KNOWN_DEFECTS } from "./platform-diff.mjs";
import { serve, listen, connect, installHttpMaxHeaderSize } from "../lib/net.mjs";
installHttpMaxHeaderSize(http);
const rec = splitRecording(JSON.parse(readFileSync(process.argv[2] + "/net.cases.json", "utf8")));
const here = { platform: process.platform, runtime: "node", errno: os.constants.errno, signals: os.constants.signals };
const cross = rec.meta.platform !== here.platform;
console.log(`reference: ${rec.meta.runtime} on ${rec.meta.platform}; compared: node on ${here.platform}${cross ? " (cross-platform: named platform differences apply)" : ""}`);
const got = await runAll({ serve, listen, connect }, { tlsDir: process.env.NET_TLS_DIR || null, childArgs: [] });
let ok = 0, bad = 0, platform = 0, unobs = 0;
if (rec.map.size !== got.length) { bad++; console.log("MISMATCH scenario sets differ:", rec.map.size, "recorded under Bun,", got.length, "run here"); }
got.forEach(([name, obs]) => {
	if (!rec.map.has(name)) { bad++; console.log("MISMATCH (not recorded under Bun)", name); return; }
	const r = compare(rec.map.get(name), obs, rec.meta, here, name);
	if (r.same) { ok++; return; }
	if (r.ok) {
		const kinds = [...new Set(r.notes.map((n) => n.kind))];
		const isUnobs = kinds.includes("unobservable");
		if (isUnobs) unobs++; else platform++;
		console.log(`${isUnobs ? "UNOBSERVABLE" : "PLATFORM"} [${kinds.join(", ")}]`, name);
		printNotes(r.notes);
		return;
	}
	bad++;
	const defect = cross && KNOWN_DEFECTS[name] ? ` (known ${here.platform} defect: ${KNOWN_DEFECTS[name]})` : "";
	console.log("MISMATCH" + defect, name);
	console.log("  node:", JSON.stringify(obs));
	console.log("  bun: ", JSON.stringify(rec.map.get(name)));
	printNotes(r.notes.filter((n) => n.kind === "MISMATCH"));
});
const extra = cross ? ` / ${platform} platform-difference / ${unobs} unobservable-on-${here.platform}` : "";
console.log(`net: ${ok} match${extra} / ${bad} mismatch (${got.length} scenarios)`);
process.exit(bad ? 1 : 0);
