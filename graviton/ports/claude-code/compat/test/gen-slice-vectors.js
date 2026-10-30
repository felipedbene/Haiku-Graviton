// Run under STOCK Bun: reference outputs for Bun.sliceAnsi (and stripANSI), deterministic.
let st = 0x51ce5eedn;
const rnd = (n) => { st ^= st << 13n; st &= (1n << 64n) - 1n; st ^= st >> 7n; st ^= st << 17n; st &= (1n << 64n) - 1n; return Number(st % BigInt(n)); };
const A = ["a", "b", "Z", " ", "\t", "\n", "\r", "\r\n", "é", "é", "́", "中", "文", "あ", "Ａ", "§", "±", "©", "®", "­", "​",
	"\u{1f600}", "\u{1f44d}\u{1f3fd}", "\u{1f468}‍\u{1f469}‍\u{1f467}", "\u{1f1fa}\u{1f1f8}", "\u{1f1fa}", "1️⃣", "❤️", "☺︎", "؀", "क्ष", "각", "각",
	"\ud800", "\udc00", "\x1b[31m", "\x1b[39m", "\x1b[1m", "\x1b[22m", "\x1b[0m", "\x1b[m", "\x1b[38;5;196m", "\x1b[48;2;1;2;3m", "\x1b[38:2::1:2:3m",
	"\x1b[4;31m", "\x1b[24m", "\x1b[7m", "\x1b[99m", "\x9b32m", "\x1b]8;;http://e.x\x07", "\x1b]8;;\x07", "\x1b]8;id=1;http://y\x1b\\", "\x1b]8;;\x1b\\",
	"\x9d8;;u\x9c", "\x1b]0;title\x07", "\x1bP1$r\x1b\\", "\x1b(B", "\x1b7", "\x1b[2K", "\x1b[?25l", "\x1b", "\x1b[", "\x9c", "\x90q", "\x1b]x", "\x1b[3\x18"];
const cases = [];
const opts = [[], [undefined, undefined, "…"], [undefined, undefined, "..."], [undefined, undefined, true], [undefined, undefined, false],
	[undefined, undefined, { ellipsis: "…", ambiguousIsNarrow: false }], [undefined, undefined, "\x1b[2m…\x1b[22m"], [undefined, undefined, "…", false],
	[undefined, undefined, undefined, false], [undefined, undefined, "中"]];
const ranges = [[0, 1], [0, 3], [1, 4], [2, 2], [0, 10], [3, undefined], [undefined, 5], [-3, undefined], [-5, -1], [0, -2], [1, Infinity],
	[0.7, 4.2], [NaN, 3], [-Infinity, 2], [5, 1], [0, 0], [100, 200], ["2", "6"]];
const run = (s, r, o) => {
	const args = [s, r[0], r[1]]; if (o.length > 2) args.push(o[2]); if (o.length > 3) args.push(o[3]);
	cases.push({ s, a: [r[0] === undefined ? { u: 1 } : Number.isNaN(r[0]) ? { nan: 1 } : r[0] === Infinity ? { inf: 1 } : r[0] === -Infinity ? { ninf: 1 } : r[0],
		r[1] === undefined ? { u: 1 } : Number.isNaN(r[1]) ? { nan: 1 } : r[1] === Infinity ? { inf: 1 } : r[1] === -Infinity ? { ninf: 1 } : r[1]],
		o: o.slice(2).map((x) => x === undefined ? { u: 1 } : x), out: Bun.sliceAnsi(...args) });
};
for (let k = 0; k < 30000; k++) {
	let s = ""; const n = 1 + rnd(14); for (let i = 0; i < n; i++) s += A[rnd(A.length)];
	run(s, ranges[rnd(ranges.length)], opts[rnd(opts.length)]);
	if (k % 3 === 0) { const a = rnd(12), b = a + rnd(12); run(s, [a, b], opts[rnd(opts.length)]); }
}
for (const s of ["hello world", "x".repeat(300), "\x1b[31m" + "y".repeat(200) + "\x1b[39m", "中".repeat(100), "ab́cd"])
	for (const r of ranges) for (const o of opts) run(s, r, o);
const strip = [];
for (let k = 0; k < 20000; k++) { let s = ""; const n = 1 + rnd(10); for (let i = 0; i < n; i++) s += A[rnd(A.length)]; strip.push({ s, out: Bun.stripANSI(s) }); }
for (const s of ["", 12, null, undefined, "\x1b[31mx\x1b[0m", "\x1b]8;;u\x1b\\l\x1b]8;;\x1b\\", "\x9c", "a\x1b"]) strip.push({ s: s === undefined ? { u: 1 } : s, out: Bun.stripANSI(s) });
await Bun.write(process.argv[2] + "/slice.cases.json", JSON.stringify(cases));
await Bun.write(process.argv[2] + "/strip.cases.json", JSON.stringify(strip));
console.log("slice cases:", cases.length, "strip cases:", strip.length);
