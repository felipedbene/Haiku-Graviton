// Run under Node: compare compat/lib/width.mjs against the stock-Bun vectors.
import { readFileSync } from "node:fs";
import { stringWidth } from "../lib/width.mjs";
const dir = process.argv[2];
const buf = readFileSync(dir + "/width.vectors.bin");
let ok = 0, bad = 0;
const fail = (what) => { bad++; if (bad <= 15) console.log("MISMATCH", what); };
for (let cp = 0; cp < 0x110000; cp++) {
	if (cp >= 0xd800 && cp <= 0xdfff) continue;
	const s = String.fromCodePoint(cp);
	const got = [stringWidth(s), stringWidth(s, { ambiguousIsNarrow: false }), stringWidth(s, { countAnsiEscapeCodes: true })];
	for (let k = 0; k < 3; k++) got[k] === buf[cp * 3 + k] ? ok++ : fail(`cp U+${cp.toString(16)} opt${k} got ${got[k]} exp ${buf[cp * 3 + k]}`);
}
const cpOk = ok, cpBad = bad;
for (const line of readFileSync(dir + "/width.cases.jsonl", "utf8").split("\n")) {
	if (!line) continue;
	const c = JSON.parse(line);
	const got = { d: stringWidth(c.s), w: stringWidth(c.s, { ambiguousIsNarrow: false }), c: stringWidth(c.s, { countAnsiEscapeCodes: true }),
		cw: stringWidth(c.s, { countAnsiEscapeCodes: true, ambiguousIsNarrow: false }) };
	for (const k of ["d", "w", "c", "cw"]) got[k] === c[k] ? ok++ : fail(`${k} ${JSON.stringify(c.s)} got ${got[k]} exp ${c[k]}`);
}
const misc = JSON.parse(readFileSync(dir + "/width.misc.json", "utf8"));
const mine = { undef: stringWidth(undefined), num: stringWidth(12345), nul: stringWidth(null), obj: stringWidth({}),
	optStr0: stringWidth("§", { ambiguousIsNarrow: "" }), optStrF: stringWidth("§", { ambiguousIsNarrow: "false" }),
	opt0: stringWidth("§", { ambiguousIsNarrow: 0 }), optNull: stringWidth("§", { ambiguousIsNarrow: null }),
	protoPolluted: (() => { Object.prototype.ambiguousIsNarrow = false; const r = stringWidth("§", {}); delete Object.prototype.ambiguousIsNarrow; return r; })(),
	inherited: stringWidth("§", Object.create({ ambiguousIsNarrow: false })) };
for (const k in misc) mine[k] === misc[k] ? ok++ : fail(`misc ${k} got ${mine[k]} exp ${misc[k]}`);
console.log(`stringWidth: codepoints ${cpOk} match / ${cpBad} mismatch; total ${ok} match / ${bad} mismatch`);
process.exit(bad ? 1 : 0);
