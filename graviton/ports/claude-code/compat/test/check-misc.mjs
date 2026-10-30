import { readFileSync } from "node:fs";
import { observe } from "./misc-cases.js";
import * as misc from "../lib/misc.mjs";
const exp = JSON.parse(readFileSync(process.argv[2] + "/misc.cases.json", "utf8"));
const got = JSON.parse(JSON.stringify(await observe(misc)));
let ok = 0, bad = 0;
for (const k of new Set([...Object.keys(exp), ...Object.keys(got)])) {
	if (JSON.stringify(got[k]) === JSON.stringify(exp[k])) ok++;
	else { bad++; console.log("MISMATCH", k, "got", JSON.stringify(got[k]), "exp", JSON.stringify(exp[k])); }
}
console.log(`misc: ${ok} match / ${bad} mismatch`);
process.exit(bad ? 1 : 0);
