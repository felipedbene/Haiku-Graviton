import { readFileSync } from "node:fs";
import { wrapAnsi } from "../lib/wrap.mjs";
const dec = (x) => x && typeof x === "object" ? (x.u ? undefined : x.nan ? NaN : x.inf ? Infinity : x) : x;
let ok = 0, bad = 0;
for (const c of JSON.parse(readFileSync(process.argv[2] + "/wrap.cases.json", "utf8"))) {
	let got; try { got = wrapAnsi(c.s, dec(c.c), dec(c.o)); } catch (e) { got = "THROW " + e.stack; }
	if (got === c.out) ok++; else { bad++; if (bad <= 10) console.log("MISMATCH", JSON.stringify([c.s, c.c, c.o]), "\n  got", JSON.stringify(got), "\n  exp", JSON.stringify(c.out)); }
}
console.log(`wrapAnsi: ${ok} match / ${bad} mismatch`);
process.exit(bad ? 1 : 0);
