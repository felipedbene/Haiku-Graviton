// Shared, deterministic case list for Bun.ant.CellSegmenter. The SAME module is
// run against the real runtime (gen-cellseg-vectors.mjs, preloaded into the
// Claude Code 2.1.314 binary) and against compat/lib/cellseg.mjs
// (check-cellseg.mjs), so both sides execute identical calls. A case is a
// session: one constructor call, then a sequence of segment/paint/setCell calls
// on that instance, with the instance's tables recorded after every step.

let st = 0xce115e6dn;
const rnd = (n) => { st ^= st << 13n; st &= (1n << 64n) - 1n; st ^= st >> 7n; st ^= st << 17n; st &= (1n << 64n) - 1n; return Number(st % BigInt(n)); };
const pick = (a) => a[rnd(a.length)];

// What the TUI passes (chunk-x65c4var.js ps()) and the substitute set b6e.
export const BUNDLE_SCREEN = { widthMask: 3, narrow: 0, wide: 1, spacerTail: 2, spacerHead: 3,
	emptyCharIndex: 0, spacerCharIndex: 1, emptyWord: 0, tabWidth: 8 };
export const BUNDLE_SUBST = [[1564, 1564], [8234, 8238], [8294, 8297]];

const TEXT = ["a", "b", "Z", " ", "  ", "~", "\t", "\n", "\r", "\x00", "\x01", "\x07", "\x7f", "\x80", "\x85", "\x9c", " ",
	"§", "­", "©", "®", "±", "·", "é", "é", "́", "̀", "⃣", "​", "‌", "‍", "⁠", "﻿",
	"︎", "️", "#", "1", "1️⃣", "ᄀ", "ᅡ", "ᆨ", "가", "각", "あ", "中", "文", "日本", "Ａ", "…", "█", "─", "▶",
	"\u{1f600}", "\u{1f44d}", "\u{1f3fd}", "\u{1f44d}\u{1f3fd}", "\u{1f468}‍\u{1f469}‍\u{1f467}", "\u{1f1fa}", "\u{1f1fa}\u{1f1f8}",
	"\u{1f3f3}️‍\u{1f308}", "❤", "❤️", "☺︎", "\u{1fa89}", "\u{e0067}", "\ud800", "\udc00", "􏿿",
	"Α", "Ж", "҃", "؀", "ः", "ि", "क", "्", "क्ष", "゙", "ﾞ",
	"؜", "‪", "‫", "‬", "‭", "‮", "⁦", "⁧", "⁨", "⁩", "‎", "‏",
	"א", "ב", "שלום", "مرحبا", "ع", "١٢", "123", "(", ")", "[", "]", "<", ">", "\u{10ffff}", "\u{e0001}", "\u{1d400}"];
const ESC = ["\x1b[31m", "\x1b[39m", "\x1b[1m", "\x1b[2m", "\x1b[3m", "\x1b[4m", "\x1b[5m", "\x1b[7m", "\x1b[8m", "\x1b[9m",
	"\x1b[21m", "\x1b[22m", "\x1b[23m", "\x1b[24m", "\x1b[27m", "\x1b[29m", "\x1b[49m", "\x1b[0m", "\x1b[m", "\x1b[;m", "\x1b[00m",
	"\x1b[1;31m", "\x1b[0;32m", "\x1b[31;0m", "\x1b[1;2;3;4;5m", "\x1b[38;5;196m", "\x1b[48;5;21m", "\x1b[38;2;255;0;10m",
	"\x1b[48;2;1;2;3m", "\x1b[58;5;3m", "\x1b[59m", "\x1b[38:2::1:2:3m", "\x1b[4:3m", "\x1b[38;5m", "\x1b[38m", "\x1b[38;2;1m",
	"\x1b[999m", "\x1b[53m", "\x1b[55m", "\x1b[73m", "\x1b[75m", "\x1b[90m", "\x1b[97m", "\x1b[100m", "\x1b[107m", "\x1b[51m", "\x1b[54m",
	"\x1b[6m", "\x1b[25m", "\x1b[28m", "\x1b[20m", "\x9b32m", "\x9b0m", "\x1b[1;38;5;9;48;2;0;0;0m",
	"\x1b]8;;https://a.example/x\x07", "\x1b]8;;\x07", "\x1b]8;id=7;https://b.example\x1b\\", "\x1b]8;;\x1b\\", "\x9d8;;u3\x9c",
	"\x1b]8;;http://c\x1b\\", "\x1b]8;x=1:id=2;file:///tmp/f\x07", "\x1b]8;;\x9c", "\x1b]8;;bad\x18", "\x1b]8;\x07", "\x1b]8\x07",
	"\x1b]0;title\x07", "\x1b]2;t\x1b\\", "\x1bP1$r\x1b\\", "\x1b(B", "\x1b7", "\x1bc", "\x1b[2K", "\x1b[1A", "\x1b[?25l", "\x1b[5;3H",
	"\x1b", "\x1b[", "\x1b]", "\x90q\x9c", "\x1b[3\x18", "\x1b]x\x1a", "\x1b\x1b[m", "\x1b%G", "\x1b ́"];

function line() {
	if (rnd(40) === 0) {	// a long line
		let s = "";
		const n = 50 + rnd(400);
		for (let i = 0; i < n; i++) s += rnd(3) === 0 ? pick(ESC) : pick(TEXT);
		return s;
	}
	let s = "";
	const n = rnd(14);
	for (let i = 0; i < n; i++) s += rnd(3) === 0 ? pick(ESC) : pick(TEXT);
	return s;
}

const SCREENS = [
	BUNDLE_SCREEN,
	{ ...BUNDLE_SCREEN, emptyWord: (3 << 17) | (4 << 2), emptyCharIndex: 5, spacerCharIndex: 9 },
	{ ...BUNDLE_SCREEN, tabWidth: 4 },
	{ ...BUNDLE_SCREEN, narrow: 2, wide: 3, spacerTail: 0, spacerHead: 1 },
	{ widthMask: 7, narrow: 4, wide: 5, spacerTail: 6, spacerHead: 7, emptyCharIndex: 2, spacerCharIndex: 3, emptyWord: 8, tabWidth: 3 },
];

function preScreen(w, h, sc) {	// a row-major screen buffer with a random mix of cell kinds
	const a = [];
	for (let i = 0; i < w * h; i++) {
		const k = rnd(6);
		if (k === 0 || k === 5) a.push(sc.emptyCharIndex, sc.emptyWord | sc.narrow);
		else if (k === 1) a.push(500 + rnd(50), (rnd(40) << 17) | (rnd(4) << 2) | sc.narrow);
		else if (k === 2 && (i + 1) % w !== 0) { a.push(600 + rnd(50), (rnd(40) << 17) | sc.wide, sc.spacerCharIndex, sc.emptyWord | sc.spacerTail); i++; }
		else if (k === 3) a.push(sc.emptyCharIndex, sc.emptyWord | sc.spacerHead);
		else a.push(rnd(1000), rnd(0x7fffffff) - rnd(4));	// junk
	}
	return a;
}

export function buildCases() {
	const cases = [];
	for (let k = 0; k < 4000; k++) {
		const v = rnd(20);
		const opts = { ambiguousIsNarrow: true, substitute: BUNDLE_SUBST, screen: BUNDLE_SCREEN };
		if (v === 0) opts.ambiguousIsNarrow = false;
		else if (v === 1) delete opts.ambiguousIsNarrow;
		else if (v === 2) opts.replacement = pick(["?", "XY", "\u{1f600}", "é", "中", "ab", "\t", " "]);
		else if (v === 3) opts.substitute = pick([[], [[0x80, 0x10ffff]], [[0x5d0, 0x5ea]], [[0x200b, 0x200d], [0x1f600, 0x1f600]], [[0x300, 0x36f]]]);
		else if (v === 4) delete opts.substitute;
		else if (v === 5) opts.screen = pick(SCREENS);
		else if (v === 6) delete opts.screen;
		const sc = opts.screen ?? BUNDLE_SCREEN;
		const ops = [];
		const nops = 1 + rnd(6);
		for (let i = 0; i < nops; i++) {
			const s = line();
			const tight = rnd(8) === 0;
			const cap = tight ? [rnd(2 * s.length + 4), rnd(12)] : [2 * s.length + 8, 2 * s.length + 8];
			ops.push({ op: "seg", s, cap, re: rnd(6) === 0 });
			if (!tight && rnd(3) > 0) {
				const w = 1 + rnd(14), h = 1 + rnd(3);
				const count = rnd(10) === 0 ? pick(["n-1", "n+1", 0]) : "n";
				const order = rnd(12) === 0 ? pick(["rev", "dup", "oob", "short", "id"]) : null;
				ops.push({ op: "paint", w, h, pre: preScreen(w, h, sc), x: rnd(w + 6) - 3, y: rnd(h + 2) - (rnd(8) === 0 ? 1 : 0),
					count, order, words: rnd(5) === 0 ? "junk" : "clean" });
			}
			if (rnd(5) === 0) {
				const w = 1 + rnd(8), h = 1 + rnd(2);
				ops.push({ op: "set", w, h, pre: preScreen(w, h, sc), x: rnd(w + 3) - 1, y: rnd(h + 1),
					ci: rnd(3000), word: (rnd(40) << 17) | (rnd(8) << 2) | rnd(8) });
			}
		}
		cases.push({ opts, ops });
	}
	return cases;
}

// Argument-validation and edge cases, each on a fresh instance.
export const EDGE = [
	["ctor-none", (C) => new C()],
	["ctor-call", (C) => C({})],
	["ctor-null", (C) => new C(null)],
	["ctor-num", (C) => new C(5)],
	["ctor-str", (C) => new C("x")],
	["sub-pair1", (C) => new C({ substitute: [[200]] })],
	["sub-num", (C) => new C({ substitute: 5 })],
	["sub-str-in", (C) => new C({ substitute: [[200, "a"]] })],
	["sub-lo", (C) => new C({ substitute: [[5, 300]] })],
	["sub-hi", (C) => new C({ substitute: [[300, 0x110000]] })],
	["sub-rev", (C) => new C({ substitute: [[300, 200]] })],
	["sub-float", (C) => new C({ substitute: [[200.5, 300]] })],
	["sub-neg", (C) => new C({ substitute: [[-1, 300]] })],
	["sub-obj", (C) => new C({ substitute: [{ 0: 200, 1: 300, length: 2 }] })],
	["sub-null", (C) => new C({ substitute: null }).graphemes.length],
	["sub-undef", (C) => new C({ substitute: undefined }).graphemes.length],
	["sub-surr", (C) => new C({ substitute: [[0xd800, 0xdfff]] }).graphemes.length],
	["sub-triple", (C) => new C({ substitute: [[200, 300, 400]] })],
	["sub-strnum", (C) => new C({ substitute: [["200", "300"]] })],
	["repl-empty", (C) => new C({ replacement: "" })],
	["repl-num", (C) => new C({ replacement: 5 })],
	["repl-null", (C) => new C({ replacement: null }).graphemes.slice(-1)],
	["repl-two", (C) => new C({ replacement: "ab" }).graphemes.slice(-1)],
	["amb-str", (C) => { const s = new C({ ambiguousIsNarrow: "" }); const c = new Int32Array(8); s.segment("α", c, new Int32Array(8), false); return Array.from(c); }],
	["amb-0", (C) => { const s = new C({ ambiguousIsNarrow: 0 }); const c = new Int32Array(8); s.segment("α", c, new Int32Array(8), false); return Array.from(c); }],
	["amb-null", (C) => { const s = new C({ ambiguousIsNarrow: null }); const c = new Int32Array(8); s.segment("α", c, new Int32Array(8), false); return Array.from(c); }],
	["screen-num", (C) => new C({ screen: 5 })],
	["screen-null", (C) => new C({ screen: null }).graphemes.length],
	["screen-empty", (C) => new C({ screen: {} }).graphemes.length],
	["screen-str", (C) => new C({ screen: { widthMask: "3" } }).graphemes.length],
	["screen-neg", (C) => new C({ screen: { ...BUNDLE_SCREEN, tabWidth: -1 } })],
	["screen-tab0", (C) => new C({ screen: { ...BUNDLE_SCREEN, tabWidth: 0 } })],
	["screen-float", (C) => new C({ screen: { ...BUNDLE_SCREEN, tabWidth: 2.5 } })],
	["screen-big", (C) => new C({ screen: { ...BUNDLE_SCREEN, emptyWord: 2 ** 32 } })],
	["screen-negword", (C) => new C({ screen: { ...BUNDLE_SCREEN, emptyWord: -1 } }).graphemes.length],
	["screen-mask0", (C) => new C({ screen: { ...BUNDLE_SCREEN, widthMask: 0 } })],
	["tables-identity", (C) => { const s = new C({}); const g = s.graphemes, k = s.sgrKeys; s.segment("\x1b[31m\u{1f600}\x1b]8;;u\x07x", new Int32Array(16), new Int32Array(16), false);
		return [g === s.graphemes, k === s.sgrKeys, s.uris === s.uris, g.length, Array.isArray(g), Object.isFrozen(g), g.at(-1), k.at(-1), s.uris.at(-1)]; }],
	["tables-mutate", (C) => { const s = new C({}); s.graphemes.push("zz"); s.sgrKeys.length = 0; const c = new Int32Array(8);
		const n = s.segment("\u{1f600}\x1b[1mq", c, new Int32Array(8), false); return [n, Array.from(c), s.graphemes.slice(-3), s.sgrKeys]; }],
	["seg-overlap", (C) => { const b = new Int32Array(64); return new C({}).segment("abc", b, b, false); }],
	["seg-overlap-view", (C) => { const b = new ArrayBuffer(256); return new C({}).segment("abc", new Int32Array(b, 0, 16), new Int32Array(b, 32, 16), false); }],
	["seg-adjacent-view", (C) => { const b = new ArrayBuffer(256); return new C({}).segment("abc", new Int32Array(b, 0, 16), new Int32Array(b, 64, 16), false); }],
	["seg-array", (C) => new C({}).segment("abc", [], new Int32Array(4), false)],
	["seg-u32", (C) => new C({}).segment("abc", new Uint32Array(8), new Int32Array(8), false)],
	["seg-runs-u32", (C) => new C({}).segment("abc", new Int32Array(8), new Uint32Array(8), false)],
	["seg-shared", (C) => new C({}).segment("abc", new Int32Array(new SharedArrayBuffer(32)), new Int32Array(8), false)],
	["seg-num", (C) => new C({}).segment(5, new Int32Array(8), new Int32Array(8), false)],
	["seg-strobj", (C) => new C({}).segment(new String("ab"), new Int32Array(8), new Int32Array(8), false)],
	["seg-noflag", (C) => { const c = new Int32Array(8); return [new C({}).segment("ab", c, new Int32Array(8)), Array.from(c)]; }],
	["seg-flag-str", (C) => { const c = new Int32Array(8); return [new C({}).segment("ab", c, new Int32Array(8), "yes"), Array.from(c)]; }],
	["seg-noargs", (C) => new C({}).segment()],
	["seg-empty-arrays", (C) => new C({}).segment("", new Int32Array(0), new Int32Array(0), false)],
	["seg-empty-arrays-x", (C) => new C({}).segment("x", new Int32Array(0), new Int32Array(0), false)],
	["seg-runs0", (C) => new C({}).segment("x", new Int32Array(8), new Int32Array(0), false)],
	["seg-runs1", (C) => new C({}).segment("x", new Int32Array(8), new Int32Array(1), false)],
	["seg-onlysgr", (C) => { const r = new Int32Array(8).fill(-7); return [new C({}).segment("\x1b[31m", new Int32Array(8), r, false), Array.from(r)]; }],
	["seg-detached", (C) => { const b = new ArrayBuffer(32); const c = new Int32Array(b); structuredClone(b, { transfer: [b] }); return new C({}).segment("x", c, new Int32Array(8), false); }],
	["seg-resizable", (C) => new C({}).segment("x", new Int32Array(new ArrayBuffer(32, { maxByteLength: 64 })), new Int32Array(8), false)],
	["seg-unaligned", (C) => new C({}).segment("x", new Int32Array(new ArrayBuffer(36), 4, 8), new Int32Array(8), false)],
	["seg-this", (C) => C.prototype.segment.call({}, "x", new Int32Array(8), new Int32Array(8), false)],
	["graphemes-this", (C) => Object.getOwnPropertyDescriptor(C.prototype, "graphemes").get.call({})],
	["seg-many-runs", (C) => { const s = new C({}); let t = ""; for (let i = 0; i < 300; i++) t += (i & 1 ? "\x1b[31m" : "\x1b[32m") + "x"; const r = new Int32Array(1000); const c = new Int32Array(1000); return [s.segment(t, c, r, false), s.sgrKeys.length, r[598], r[599], c[599]]; }],
	["seg-big-neg", (C) => new C({}).segment("x".repeat(5000), new Int32Array(10), new Int32Array(10), false)],
	["paint-noscreen", (C) => { const s = new C({}); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); const scr = new Int32Array(20); const r = s.paint(scr, 10, 0, 0, c, n, undefined, new Int32Array(200).fill(7), new Int32Array(4)); return [r, Array.from(scr)]; }],
	["set-noscreen", (C) => { const scr = new Int32Array(20); const r = new C({}).setCell(scr, 10, 2, 0, 5, 9 << 17); return [r, Array.from(scr)]; }],
	["paint-overlap", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const b = new Int32Array(64); const n = s.segment("ab", b.subarray(0, 8), new Int32Array(8), false); return s.paint(b, 4, 0, 1, b.subarray(0, 8), n, undefined, new Int32Array(200), new Int32Array(4)); }],
	["paint-overlap-cm", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const b = new Int32Array(400); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); return s.paint(b.subarray(0, 20), 10, 0, 0, c, n, undefined, b.subarray(10, 210), new Int32Array(4)); }],
	["paint-count-exceeds", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); s.segment("ab", c, new Int32Array(8), false); return s.paint(new Int32Array(20), 10, 0, 0, c, 5, undefined, new Int32Array(200), new Int32Array(4)); }],
	["paint-count-neg", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); s.segment("ab", c, new Int32Array(8), false); return s.paint(new Int32Array(20), 10, 0, 0, c, -1, undefined, new Int32Array(200), new Int32Array(4)); }],
	["paint-count-float", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); s.segment("ab", c, new Int32Array(8), false); const scr = new Int32Array(20); return [s.paint(scr, 10, 0, 0, c, 1.5, undefined, new Int32Array(200), new Int32Array(4)), Array.from(scr)]; }],
	["paint-charmap-short", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); const scr = new Int32Array(20); return [s.paint(scr, 10, 0, 0, c, n, undefined, new Int32Array(10), new Int32Array(4)), Array.from(scr)]; }],
	["paint-words-short", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("a\x1b[31mb", c, new Int32Array(8), false); const scr = new Int32Array(20); return [s.paint(scr, 10, 0, 0, c, n, undefined, new Int32Array(200), new Int32Array(1)), Array.from(scr)]; }],
	["paint-words-empty", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); const scr = new Int32Array(20); return [s.paint(scr, 10, 0, 0, c, n, undefined, new Int32Array(200), new Int32Array(0)), Array.from(scr)]; }],
	["paint-width0", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); const scr = new Int32Array(20); return [s.paint(scr, 0, 0, 0, c, n, undefined, new Int32Array(200), new Int32Array(4)), Array.from(scr)]; }],
	["paint-width-neg", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); return s.paint(new Int32Array(20), -3, 0, 0, c, n, undefined, new Int32Array(200), new Int32Array(4)); }],
	["paint-width-big", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); const scr = new Int32Array(20); return [s.paint(scr, 100, 0, 0, c, n, undefined, new Int32Array(200), new Int32Array(4)), Array.from(scr)]; }],
	["paint-x-float", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); const scr = new Int32Array(20); return [s.paint(scr, 10, 1.7, 0, c, n, undefined, new Int32Array(200), new Int32Array(4)), Array.from(scr)]; }],
	["paint-x-huge", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); return s.paint(new Int32Array(20), 10, 2 ** 31, 0, c, n, undefined, new Int32Array(200), new Int32Array(4)); }],
	["paint-x-1e6", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); return s.paint(new Int32Array(20), 10, 1048570, 0, c, n, undefined, new Int32Array(200), new Int32Array(4)); }],
	["paint-x-neg-big", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); return s.paint(new Int32Array(20), 10, -100, 0, c, n, undefined, new Int32Array(200), new Int32Array(4)); }],
	["paint-order-num", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); return s.paint(new Int32Array(20), 10, 0, 0, c, n, 5, new Int32Array(200), new Int32Array(4)); }],
	["paint-order-null", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); const scr = new Int32Array(20); return [s.paint(scr, 10, 0, 0, c, n, null, new Int32Array(200), new Int32Array(4)), Array.from(scr)]; }],
	["paint-screen-odd", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("abc", c, new Int32Array(8), false); const scr = new Int32Array(7); return [s.paint(scr, 2, 0, 1, c, n, undefined, new Int32Array(200), new Int32Array(4)), Array.from(scr)]; }],
	["paint-screen-arr", (C) => { const s = new C({ screen: BUNDLE_SCREEN }); const c = new Int32Array(8); const n = s.segment("ab", c, new Int32Array(8), false); return s.paint([], 10, 0, 0, c, n, undefined, new Int32Array(200), new Int32Array(4)); }],
	["paint-args", (C) => new C({ screen: BUNDLE_SCREEN }).paint()],
	["set-args", (C) => new C({ screen: BUNDLE_SCREEN }).setCell()],
	["set-neg", (C) => { const scr = new Int32Array(20); return [new C({ screen: BUNDLE_SCREEN }).setCell(scr, 10, -1, 0, 5, 0), Array.from(scr)]; }],
	["set-y-neg", (C) => { const scr = new Int32Array(20); return [new C({ screen: BUNDLE_SCREEN }).setCell(scr, 10, 1, -1, 5, 0), Array.from(scr)]; }],
	["set-float", (C) => { const scr = new Int32Array(20); return [new C({ screen: BUNDLE_SCREEN }).setCell(scr, 10, 1.5, 0, 5.5, 7.9), Array.from(scr)]; }],
	["set-width0", (C) => { const scr = new Int32Array(20); return [new C({ screen: BUNDLE_SCREEN }).setCell(scr, 0, 0, 0, 5, 0), Array.from(scr)]; }],
	["set-word-neg", (C) => { const scr = new Int32Array(20); return [new C({ screen: BUNDLE_SCREEN }).setCell(scr, 10, 1, 0, -5, -1), Array.from(scr)]; }],
	["set-word-big", (C) => { const scr = new Int32Array(20); return [new C({ screen: BUNDLE_SCREEN }).setCell(scr, 10, 1, 0, 2 ** 31, 2 ** 32 + 5), Array.from(scr)]; }],
	["set-ci-str", (C) => { const scr = new Int32Array(20); return [new C({ screen: BUNDLE_SCREEN }).setCell(scr, 10, 1, 0, "7", "9"), Array.from(scr)]; }],
	["set-u32", (C) => new C({ screen: BUNDLE_SCREEN }).setCell(new Uint32Array(20), 10, 1, 0, 5, 0)],
	["proto", (C) => [Object.getOwnPropertyNames(C.prototype).sort(), C.name, C.length, Object.prototype.toString.call(new C({}))]],
];

const toPlain = (v) => (ArrayBuffer.isView(v) ? Array.from(v) : v);
export function errInfo(e) {
	return { err: e?.constructor?.name, msg: String(e?.message), code: e?.code };
}

function orderFor(kind, n) {
	if (kind === "rev") return Int32Array.from({ length: n }, (_, i) => n - 1 - i);
	if (kind === "dup") return Int32Array.from({ length: n }, () => 0);
	if (kind === "oob") return Int32Array.from({ length: n }, (_, i) => (i === n - 1 ? n + 3 : i));
	if (kind === "short") return Int32Array.from({ length: Math.max(0, n - 1) }, (_, i) => i);
	return Int32Array.from({ length: n }, (_, i) => i);
}

// Run one session against constructor C; returns a JSON-able record.
export function runCase(C, c) {
	const out = [];
	let s;
	try { s = new C(c.opts); } catch (e) { return [errInfo(e)]; }
	let seen = [0, 0, 0, 0];
	const tables = () => {
		const t = [s.graphemes, s.sgrKeys, s.sgrCloseKeys, s.uris];
		const d = t.map((a, i) => (a.length < seen[i] ? { reset: a.slice() } : a.slice(seen[i])));
		seen = t.map((a) => a.length);
		return d;
	};
	out.push(tables());
	let cells = new Int32Array(0), runs = new Int32Array(0), n = 0;
	for (const op of c.ops) {
		try {
			if (op.op === "seg") {
				cells = new Int32Array(op.cap[0]).fill(-7);
				runs = new Int32Array(op.cap[1]).fill(-7);
				n = s.segment(op.s, cells, runs, op.re);
				out.push({ n, cells: Array.from(cells), runs: Array.from(runs), t: tables() });
			} else if (op.op === "paint") {
				const scr = Int32Array.from(op.pre);
				const cm = new Int32Array(s.graphemes.length);
				for (let i = 0; i < cm.length; i++) cm[i] = 1000 + 3 * i;
				let maxRun = 0;
				for (let i = 0; i < Math.max(0, n); i++) maxRun = Math.max(maxRun, cells[2 * i + 1] >>> 10);
				const words = new Int32Array(maxRun + 1);
				for (let i = 0; i <= maxRun; i++) words[i] = op.words === "junk" ? (i * 2654435761) | 0 : ((i + 1) * 3) << 17;
				const count = op.count === "n" ? n : op.count === "n-1" ? Math.max(0, n - 1) : op.count === "n+1" ? n + 1 : op.count;
				const order = op.order ? orderFor(op.order, count) : undefined;
				const r = s.paint(scr, op.w, op.x, op.y, cells, count, order, cm, words);
				out.push({ r, scr: Array.from(scr) });
			} else if (op.op === "set") {
				const scr = Int32Array.from(op.pre);
				const r = s.setCell(scr, op.w, op.x, op.y, op.ci, op.word);
				out.push({ r, scr: Array.from(scr) });
			}
		} catch (e) {
			out.push(errInfo(e));
		}
	}
	return out;
}

export function runEdge(C) {
	return EDGE.map(([name, f]) => {
		try { return [name, JSON.parse(JSON.stringify(toPlain(f(C)) ?? null))]; } catch (e) { return [name, errInfo(e)]; }
	});
}
