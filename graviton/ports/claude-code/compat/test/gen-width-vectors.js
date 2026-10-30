// Run under STOCK Bun. Writes width.vectors.bin (every scalar value, 3 option
// combos) and width.cases.jsonl (sequence/ANSI cases). Deterministic.
const out = process.argv[2];
// 1. Every Unicode scalar value alone: [default, ambiguousIsNarrow:false, countAnsi].
const N = 0x110000, buf = new Uint8Array(N * 3);
for (let cp = 0; cp < N; cp++) {
	if (cp >= 0xd800 && cp <= 0xdfff) continue;
	const s = String.fromCodePoint(cp);
	buf[cp * 3] = Bun.stringWidth(s);
	buf[cp * 3 + 1] = Bun.stringWidth(s, { ambiguousIsNarrow: false });
	buf[cp * 3 + 2] = Bun.stringWidth(s, { countAnsiEscapeCodes: true });
}
await Bun.write(out + "/width.vectors.bin", buf);
// 2. Sequences built from an adversarial alphabet.
let st = 0x9e3779b9n;
const rnd = (n) => { st ^= st << 13n; st &= (1n << 64n) - 1n; st ^= st >> 7n; st ^= st << 17n; st &= (1n << 64n) - 1n; return Number(st % BigInt(n)); };
const A = ["a", "Z", " ", "\t", "\n", "\r", "\x00", "\x7f", "\u0085", " ", "§", "­", "©", "®", "é", "é",
	"́", "̀", "؀", "ः", "ि", "क", "्", "ष", "त", "‌", "‍", "︎", "️", "⃣",
	"#", "1", "ᄀ", "ᅡ", "ᆨ", "가", "각", "あ", "中", "Ａ", "…", "█", "─", "▶",
	"\u{1f600}", "\u{1f44d}", "\u{1f3fd}", "\u{1f468}", "\u{1f469}", "\u{1f467}", "\u{1f1fa}", "\u{1f1f8}", "\u{1f3f3}", "❤", "☺",
	"\u{1fa89}", "\u{e0067}", "\u{e007f}", "\ud800", "\udc00", "Α", "Ж", "҂", "҃", "԰", "゙", "゚",
	"\x1b[31m", "\x1b[0m", "\x1b[1;38;2;255;0;0m", "\x1b]8;;http://x\x07", "\x1b]8;;\x1b\\", "\x1b]0;title\x9c", "\x1bP1$r\x1b\\",
	"\x1b(B", "\x1b7", "\x1bc", "\x1b", "\x1b[", "\x1b]", "\x9b31m", "\x9d0;t\x07", "\x90q\x9c", "\x1b[3\x18", "\x1b]x\x1a", "\x1b\x1b[m",
	"\x1b%G", "\x1b ́", "\x9c", "\x1b\x9c"];
const lines = [];
const rec = (s) => lines.push(JSON.stringify({ s, d: Bun.stringWidth(s), w: Bun.stringWidth(s, { ambiguousIsNarrow: false }),
	c: Bun.stringWidth(s, { countAnsiEscapeCodes: true }), cw: Bun.stringWidth(s, { countAnsiEscapeCodes: true, ambiguousIsNarrow: false }) }));
for (let i = 0; i < A.length; i++) for (let j = 0; j < A.length; j++) rec(A[i] + A[j]);
for (let k = 0; k < 60000; k++) { let s = ""; const n = 1 + rnd(12); for (let i = 0; i < n; i++) s += A[rnd(A.length)]; rec(s); }
for (const s of ["", "abc", "x".repeat(5000), "中".repeat(700) + "́", "a؀b", "؀\u{1f600}", "\u{1f468}‍\u{1f469}‍\u{1f467}",
	"\u{1f3f3}️‍\u{1f308}", "1️⃣", "\u{1f1fa}\u{1f1f8}\u{1f1fa}", "क्ष", "각"]) rec(s);
await Bun.write(out + "/width.cases.jsonl", lines.join("\n") + "\n");
// Non-string inputs and option edge cases.
const misc = { undef: Bun.stringWidth(undefined), num: Bun.stringWidth(12345), nul: Bun.stringWidth(null), obj: Bun.stringWidth({}),
	optStr0: Bun.stringWidth("§", { ambiguousIsNarrow: "" }), optStrF: Bun.stringWidth("§", { ambiguousIsNarrow: "false" }),
	opt0: Bun.stringWidth("§", { ambiguousIsNarrow: 0 }), optNull: Bun.stringWidth("§", { ambiguousIsNarrow: null }),
	protoPolluted: (() => { Object.prototype.ambiguousIsNarrow = false; const r = Bun.stringWidth("§", {}); delete Object.prototype.ambiguousIsNarrow; return r; })(),
	inherited: Bun.stringWidth("§", Object.create({ ambiguousIsNarrow: false })) };
await Bun.write(out + "/width.misc.json", JSON.stringify(misc));
console.log("codepoints:", N, "cases:", lines.length, "misc:", JSON.stringify(misc));
