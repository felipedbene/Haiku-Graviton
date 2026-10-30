// Run under STOCK Bun: reference outputs for Bun.wrapAnsi, deterministic.
let st = 0x77a9a11dn;
const rnd = (n) => { st ^= st << 13n; st &= (1n << 64n) - 1n; st ^= st >> 7n; st ^= st << 17n; st &= (1n << 64n) - 1n; return Number(st % BigInt(n)); };
const W = ["word", "a", "longerword", "supercalifragilistic", " ", "  ", "\t", "\n", "\r", "\r\n", "中文", "日本語の文", "é", "́", "‍", "️", "⃣", "؀x",
	"\u{1f600}", "\u{1f44d}\u{1f3fd}", "\u{1f1fa}\u{1f1f8}", "§±", "\x1b[31m", "\x1b[39m", "\x1b[1m", "\x1b[22m", "\x1b[0m", "\x1b[4m", "\x1b[38;5;1m",
	"\x1b[99m", "\x9b32m", "\x1b]8;;http://ex.am/p q\x07", "\x1b]8;;\x07", "\x1b]8;id=x;http://y\x1b\\", "\x1b]8;;\x1b\\", "\x1b(B", "\x1b7", "\x1b",
	"\x1b[2K", "\x9c", "\x1b]0;t\x07", "\x1bPx\x1b\\", "-", "--", ".", "x".repeat(23)];
const optsList = [undefined, {}, { hard: true }, { wordWrap: false }, { trim: false }, { hard: true, trim: false }, { wordWrap: false, trim: false },
	{ ambiguousIsNarrow: false }, { hard: true, wordWrap: false }, { hard: 1, wordWrap: 0, trim: null }, { hard: true, ambiguousIsNarrow: false, trim: false }];
const cols = [1, 2, 3, 4, 5, 7, 8, 10, 13, 20, 40, 0, -3, 2.9, NaN, Infinity, "6"];
const enc = (x) => x === undefined ? { u: 1 } : Number.isNaN(x) ? { nan: 1 } : x === Infinity ? { inf: 1 } : x;
const cases = [];
for (let k = 0; k < 40000; k++) {
	let s = ""; const n = 1 + rnd(12);
	for (let i = 0; i < n; i++) s += W[rnd(W.length)] + (rnd(3) ? " " : "");
	const c = cols[rnd(cols.length)], o = optsList[rnd(optsList.length)];
	cases.push({ s, c: enc(c), o: enc(o), out: Bun.wrapAnsi(s, c, o) });
}
await Bun.write(process.argv[2] + "/wrap.cases.json", JSON.stringify(cases));
console.log("wrap cases:", cases.length);
