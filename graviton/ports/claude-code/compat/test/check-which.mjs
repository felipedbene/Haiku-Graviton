import { readFileSync } from "node:fs";
import { CASES } from "./which-cases.js";
import { bunWhich } from "../lib/which.mjs";
const exp = JSON.parse(readFileSync(process.argv[2] + "/which.cases.json", "utf8"));
let ok = 0, bad = 0;
CASES.forEach((a, i) => {
	let got; try { got = bunWhich(...a); } catch (e) { got = "THROW " + e.message; }
	if (got === exp[i]) ok++; else { bad++; console.log("MISMATCH", JSON.stringify(a).slice(0, 80), "got", JSON.stringify(got), "exp", JSON.stringify(exp[i])); }
});
console.log(`which: ${ok} match / ${bad} mismatch`);
process.exit(bad ? 1 : 0);
