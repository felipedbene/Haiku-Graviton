import { readFileSync } from "node:fs";
import { sliceAnsi } from "../lib/slice.mjs";
import { stripANSI } from "../lib/strip.mjs";
const dir = process.argv[2];
const dec = (x) => x && typeof x === "object" ? (x.u ? undefined : x.nan ? NaN : x.inf ? Infinity : x.ninf ? -Infinity : x) : x;
let ok = 0, bad = 0;
for (const c of JSON.parse(readFileSync(dir + "/slice.cases.json", "utf8"))) {
	const args = [c.s, dec(c.a[0]), dec(c.a[1]), ...c.o.map(dec)];
	let got; try { got = sliceAnsi(...args); } catch (e) { got = "THROW " + e.message; }
	if (got === c.out) ok++; else { bad++; if (bad <= 12) console.log("MISMATCH slice", JSON.stringify(args), "got", JSON.stringify(got), "exp", JSON.stringify(c.out)); }
}
const sOk = ok, sBad = bad;
for (const c of JSON.parse(readFileSync(dir + "/strip.cases.json", "utf8"))) {
	const got = stripANSI(dec(c.s));
	if (got === c.out) ok++; else { bad++; if (bad - sBad <= 8) console.log("MISMATCH strip", JSON.stringify(c.s), "got", JSON.stringify(got), "exp", JSON.stringify(c.out)); }
}
console.log(`sliceAnsi: ${sOk} match / ${sBad} mismatch; stripANSI: ${ok - sOk} match / ${bad - sBad} mismatch`);
process.exit(bad ? 1 : 0);
