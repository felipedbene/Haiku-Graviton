import { readFileSync } from "node:fs";
import { corpus } from "./deepequals-corpus.js";
import { deepEquals } from "../lib/deepequals.mjs";
const exp = JSON.parse(readFileSync(process.argv[2] + "/deepequals.cases.json", "utf8"));
const P = corpus();
let ok = 0, bad = 0;
P.forEach(([n, a, b], i) => {
	const r = (f) => { try { return f(); } catch (e) { return "THROW " + e.message; } };
	const got = [n, r(() => deepEquals(a, b)), r(() => deepEquals(a, b, true)), r(() => deepEquals(a, b, true, true))];
	for (let k = 1; k < 4; k++) {
		if (got[k] === exp[i][k]) ok++;
		else { bad++; console.log("MISMATCH", n, ["loose", "strict", "strict+skipProto"][k - 1], "got", got[k], "exp", exp[i][k]); }
	}
});
console.log(`deepEquals: ${ok} match / ${bad} mismatch (${P.length} pairs x 3 modes)`);
process.exit(bad ? 1 : 0);
