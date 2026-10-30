// Runs the shared net scenarios under Node with compat/lib/net.mjs and compares
// each scenario's observations with what stock Bun recorded (gen-net-vectors.js).
import { readFileSync } from "node:fs";
import http from "node:http";
import { runAll } from "./net-scenarios.js";
import { serve, listen, connect, installHttpMaxHeaderSize } from "../lib/net.mjs";
installHttpMaxHeaderSize(http);
const exp = JSON.parse(readFileSync(process.argv[2] + "/net.cases.json", "utf8"));
const got = await runAll({ serve, listen, connect }, { tlsDir: process.env.NET_TLS_DIR || null, childArgs: [] });
let ok = 0, bad = 0;
const expBy = new Map(exp);
if (expBy.size !== got.length) { bad++; console.log("MISMATCH scenario sets differ:", exp.length, "recorded under Bun,", got.length, "run here"); }
got.forEach(([name, obs]) => {
	if (!expBy.has(name)) { bad++; console.log("MISMATCH (not recorded under Bun)", name); return; }
	const a = JSON.stringify(obs), b = JSON.stringify(expBy.get(name));
	if (a === b) { ok++; return; }
	bad++;
	console.log("MISMATCH", name);
	console.log("  node:", a);
	console.log("  bun: ", b);
});
console.log(`net: ${ok} match / ${bad} mismatch (${got.length} scenarios)`);
process.exit(bad ? 1 : 0);
