// Run under STOCK Bun: reference outcomes of Bun.YAML.parse, deterministic.
//   bun gen-yaml-parse-vectors.js <out-dir> [seed]
// Inputs: hand-written edge cases, the real-world corpus (yaml-parse-corpus.json,
// see build-yaml-parse-corpus.mjs), a grammar-driven YAML generator, and byte
// mutations of all of those. Writes <out-dir>/yaml-parse.cases.json as
// {category: [[input-descriptor, outcome], ...]}.
import { readFileSync } from "node:fs";
import { outcome, materialize } from "./yaml-parse-serialize.js";

// argv[3]: optional fuzz seed (the grammar and mutation streams follow it).
let st = (process.argv[3] ? Number(process.argv[3]) : 0x9e3779b9) | 0;
const rnd = (n) => { st ^= st << 13; st ^= st >>> 17; st ^= st << 5; return ((st >>> 0) % n); };
const pick = (a) => a[rnd(a.length)];
const chance = (k) => rnd(k) === 0;
// `clean` steers the grammar away from its deliberate breakage, for a stream of
// mostly valid documents whose values (not errors) get compared.
let clean = false;
const dirty = (k) => !clean && chance(k);

// ── scalars ──
const NUMBERS = ["0", "1", "-1", "+1", "01", "007", "0x1F", "0xff", "0XFF", "0x", "0o17", "0o8", "0O17", "0b101", "0B1", "1_000",
	"1.5", "-1.5", "+1.5", ".5", "-.5", "+.5", "5.", "1e3", "1E3", "1e+3", "1e-3", "1e", "1e+", "1.2.3", "1+1", "1-1", "--1", "-",
	"+", ".", "..", "1x5", "1o5", "0x1e", "0x1.5", "0x1-2", "1ee5", "1e5e", "12:30", "12:30:45", "2001-12-14", "2001-12-14t21:59:43.10-05:00",
	".inf", ".Inf", ".INF", "-.inf", "+.inf", "-.Inf", "+.INF", ".iNf", "-.infx", ".nan", ".NaN", ".NAN", ".nAn", "-.nan", "+.nan",
	"18446744073709551615", "18446744073709551616", "0xFFFFFFFFFFFFFFFF", "0x10000000000000000", "9007199254740993", "1e400", "-1e400",
	"1e-400", "-0", "+0", "0.0", "-0.0", "00", "0e0", "1,000", "1 000", "1#x", "1 #x", "0o", "0o0", "0x0", "0xg", "1.e5", ".e5", "-.e5",
	"1a", "a1", "0b", "0a", "0e", "0e1", "0f", "1d", "1b", "12e3f"];
const KEYWORDS = ["null", "Null", "NULL", "nULL", "~", "~~", "true", "True", "TRUE", "tRUE", "false", "False", "FALSE", "fALSE", "yes", "no",
	"on", "off", "y", "n", "nullx", "truex", "null x", "true false", "", "<<", "=", "!", "-a", "?a", ":a", "a:b", "a::b", "http://x.y/z?q=1#f"];
const WORDS = ["name", "description", "tools", "model", "a", "b", "c", "key", "value", "x", "y", "foo bar", "with space ", "é", "日本",
	"emoji😀", "tab\there", "colon: x", "hash #x", "hash#x", "dash - x", "-dash", "q'uote", "q\"uote", "back\\slash", "per%cent", "at@", "back`tick",
	"a,b", "a[b]", "a{b}", "*star", "&amp", "|pipe", ">gt", "!bang", "%pct", "@at", "`bt"];
const ESCAPES = ["\\n", "\\t", "\\\\", "\\\"", "\\/", "\\0", "\\a", "\\b", "\\v", "\\f", "\\r", "\\e", "\\ ", "\\\t", "\\N", "\\_", "\\L", "\\P",
	"\\x41", "\\xe9", "\\xff", "\\x4", "\\xZZ", "\\u00e9", "\\u263A", "\\uD83D\\uDE00", "\\ud83d\\ude00", "\\uD83D", "\\uDE00", "\\uD83Dx", "\\uD83D\\u0041",
	"\\U0001F600", "\\U00110000", "\\UFFFFFFFF", "\\U0000D800", "\\q", "\\'", "\\\n", "\\\r\n", "\\\n  ", "\\"];

function plainScalar() {
	switch (rnd(4)) {
		case 0: return pick(NUMBERS);
		case 1: return pick(KEYWORDS);
		case 2: return pick(WORDS);
		default: {
			let s = "";
			const n = 1 + rnd(8);
			for (let i = 0; i < n; i++) s += pick(["0", "1", "9", ".", "-", "+", "e", "E", "x", "o", "b", "a", "f", "n", "i", "N", "I", "_", ":", " ", "#", "~", "t", "r", "u"]);
			return s;
		}
	}
}
function singleQuoted() {
	let s = "'";
	const n = rnd(5);
	for (let i = 0; i < n; i++) s += pick(["a", "''", " ", "  ", "\n", "\n\n", "\n  ", "\r\n", "\t", "é", "\\n", "\"", "#", ": ", "---", "..."]);
	return s + (dirty(30) ? "" : "'");
}
function doubleQuoted() {
	let s = "\"";
	const n = rnd(5);
	for (let i = 0; i < n; i++) s += chance(2) ? pick(clean ? ESCAPES.slice(0, 26) : ESCAPES) : pick(["a", " ", "  ", "\n", "\n\n", "\n  ", "\r\n", "\t", "é", "'", "#", ": ", "\x01"]);
	return s + (dirty(30) ? "" : "\"");
}
function blockScalar(indent) {
	let h = pick(["|", ">"]);
	if (chance(2)) h += pick(["-", "+", "1", "2", "3", "9", "-2", "2-", "+1", "1+", "--", "12", "0", "x"]);
	if (chance(6)) h += pick([" # comment", "  #c", "#bad", " \t"]);
	const base = indent + (chance(8) ? 0 : 1 + rnd(3));
	let s = h + "\n";
	const n = rnd(5);
	for (let i = 0; i < n; i++) {
		const r = rnd(10);
		if (r === 0) s += "\n";
		else if (r === 1) s += " ".repeat(base + 1 + rnd(3)) + "more\n";
		else if (r === 2) s += " ".repeat(rnd(base + 2)) + "\n";
		else if (r === 3) s += " ".repeat(base) + "\tt\n";
		else if (r === 4) s += " ".repeat(Math.max(0, base - 1)) + "less\n";
		else s += " ".repeat(base) + pick(["line", "a b", "# not comment", "- x", "key: v", "---", "...", "é"]) + (chance(5) ? "  " : "") + "\n";
	}
	return s.replace(/\n$/, chance(3) ? "" : "\n");
}

// ── nodes ──
let anchorsSoFar = [];
function props() {
	let p = "";
	if (chance(6)) { const a = pick(["a", "b", "c", "x1", "a-b", "é"]); anchorsSoFar.push(a); p += "&" + a + " "; }
	if (chance(6)) p += clean ? pick(["!!str", "!!int", "!!float", "!!bool", "!!null", "!", "!foo", "!<tag:x,2000:y>"]) + " " : pick(["!!str", "!!int", "!!float", "!!bool", "!!null", "!", "!foo", "!e!bar", "!<tag:x,2000:y>", "!<!local>", "!!map", "!!seq", "!str", "!!binary", "!!"]) + " ";
	return p;
}
function inlineNode(depth) {
	const r = rnd(12);
	if (r === 0 && anchorsSoFar.length) return "*" + pick(anchorsSoFar);
	if (r === 1 && !clean) return "*" + pick(["zz", "a", "b"]);
	if (r < 4 && depth < 4) return props() + flow(depth + 1);
	if (r === 4) return props() + singleQuoted();
	if (r === 5) return props() + doubleQuoted();
	return props() + plainScalar();
}
function flow(depth) {
	const seq = chance(2);
	const n = rnd(4);
	const sp = () => pick(["", " ", "  ", "\n", "\n  ", "\t", " # c\n "]);
	let s = seq ? "[" : "{";
	for (let i = 0; i < n; i++) {
		if (i) s += sp() + "," + sp();
		if (seq) {
			if (chance(8)) s += "? " + inlineNode(depth) + (chance(2) ? ": " + inlineNode(depth) : "");
			else if (chance(6)) s += inlineNode(depth) + ": " + inlineNode(depth);
			else s += inlineNode(depth);
		} else {
			if (chance(8)) s += "? " + inlineNode(depth);
			else s += inlineNode(depth) + pick([": ", ":", " : ", ":\n "]) + (chance(8) ? "" : inlineNode(depth));
		}
	}
	if (chance(10)) s += ",";
	return s + (clean ? " " : sp()) + (dirty(25) ? "" : seq ? "]" : "}");
}
function key() {
	switch (rnd(10)) {
		case 0: return "<<";
		case 1: return singleQuoted();
		case 2: return doubleQuoted();
		case 3: return "[" + plainScalar() + ", " + plainScalar() + "]";
		case 4: return "{" + plainScalar() + ": 1}";
		case 5: return pick(["__proto__", "constructor", "toString", "0", "1", "10", "2", "-1", "1.5", "length", "\"__proto__\""]);
		default: return props() + plainScalar();
	}
}
function block(depth, indent) {
	const ind = " ".repeat(indent);
	let s = "";
	const n = 1 + rnd(4);
	if (chance(2) || depth >= 4) {	// mapping
		for (let i = 0; i < n; i++) {
			if (chance(12)) { s += ind + "? " + inlineNode(depth) + "\n" + (chance(2) ? ind + ": " + inlineNode(depth) + "\n" : ""); continue; }
			const k = key();
			const r = rnd(10);
			if (r < 4 || depth >= 4) s += ind + k + ":" + pick([" ", "  ", "\t", " "]) + inlineNode(depth) + (chance(8) ? " # trailing" : "") + "\n";
			else if (r < 6) s += ind + k + ": " + blockScalar(indent);
			else if (r < 7) s += ind + k + ":\n";
			else s += ind + k + ":" + (chance(6) ? " " + props().trim() : "") + "\n" + block(depth + 1, indent + pick([0, 1, 2, 2, 4]));
			if (!s.endsWith("\n")) s += "\n";
		}
	} else {	// sequence
		for (let i = 0; i < n; i++) {
			const r = rnd(10);
			if (r < 5) s += ind + "- " + inlineNode(depth) + "\n";
			else if (r < 6) s += ind + "- " + blockScalar(indent + 2);
			else if (r < 7) s += ind + "-\n";
			else if (r < 8) s += ind + "- " + key() + ": " + inlineNode(depth) + "\n" + ind + "  " + key() + ": " + inlineNode(depth) + "\n";
			else s += ind + "-" + (chance(4) ? " " + props().trim() : "") + "\n" + block(depth + 1, indent + pick([0, 1, 2, 2]));
			if (!s.endsWith("\n")) s += "\n";
		}
	}
	return s;
}
function documentStream() {
	anchorsSoFar = [];
	let s = "";
	if (chance(20)) s += "\ufeff";
	if (dirty(10)) s += pick(["%YAML 1.2\n", "%YAML 1.1\n", "%TAG ! tag:x,2000:\n", "%TAG !e! tag:e,2000:\n", "%FOO bar baz\n", "%YAML 1.2\n%YAML 1.2\n", "%TAG !e! !local-\n", "  %YAML 1.2\n"]);
	if (chance(4)) s += pick(["---\n", "--- ", "--- # c\n", "---\t"]);
	if (chance(5)) s += "# leading comment\n";
	const r = rnd(6);
	if (r === 0) s += inlineNode(0) + "\n";
	else s += block(0, 0);
	if (chance(5)) s += pick(["...\n", "---\n" + block(0, 0), "...\n---\n" + inlineNode(0), "--- " + inlineNode(0) + "\n", "... # end\n", "...\n...\n", "... x\n"]);
	if (chance(6)) s = s.replace(/\n/g, "\r\n");
	if (dirty(15)) s = s.replace(/\n/g, "\r");
	if (chance(10)) s = s.trimEnd();
	return s;
}

// ── mutations ──
const INSERTS = [" ", "  ", "\t", "\n", "\r\n", "\r", ":", ": ", "-", "- ", "?", "? ", "#", " #", "&a ", "*a", "!", "!!str ", "|", ">", "'", "\"",
	"[", "]", "{", "}", ",", "\\", "...", "---", "%", "@", "`", "<<: ", "x", "1", ".", "\0", "é", "\ufeff", "\x7f", "\x01", "~", "0x", "-.", "\u2028"];
function mutate(s) {
	const k = 1 + rnd(3);
	for (let i = 0; i < k; i++) {
		const at = rnd(s.length + 1);
		switch (rnd(6)) {
			case 0: s = s.slice(0, at) + s.slice(at + 1 + rnd(3)); break;
			case 1: case 2: s = s.slice(0, at) + pick(INSERTS) + s.slice(at); break;
			case 3: s = s.slice(0, at) + pick(INSERTS) + s.slice(at + 1); break;
			case 4: {	// re-indent a line
				const lines = s.split("\n");
				const l = rnd(lines.length);
				lines[l] = chance(2) ? " ".repeat(1 + rnd(3)) + lines[l] : lines[l].replace(/^ {1,2}/, "");
				s = lines.join("\n");
				break;
			}
			default: {	// duplicate or drop a line
				const lines = s.split("\n");
				const l = rnd(lines.length);
				if (chance(2)) lines.splice(l, 0, lines[l]); else lines.splice(l, 1);
				s = lines.join("\n");
			}
		}
	}
	return s;
}

// ── fixed edge cases ──
const b64 = (bytes, as = "Buffer") => ({ b: Buffer.from(bytes).toString("base64"), as });
const EDGE = [
	{ v: "undefined" }, { v: "null" }, { v: "num123" }, { v: "numFrac" }, { v: "true" }, { v: "emptyObject" }, { v: "toStringObject" },
	{ v: "array" }, { v: "symbol" }, { v: "bigint" },
	b64(Buffer.from("a: 1\nb: [1, 2]\n")), b64(Buffer.from("a: 1"), "Uint8Array"), b64(Buffer.from("- x"), "ArrayBuffer"),
	b64(Buffer.from("k: v"), "DataView"), b64(Buffer.from("ab: 1\n"), "Uint16Array"), b64([0, 0], "ArrayBuffer"), b64([]),
	b64([0x61, 0x3a, 0x20, 0xff, 0xfe]), b64([0x61, 0x3a, 0x20, 0xc3]), b64([0x61, 0x3a, 0x20, 0xe2, 0x82]), b64([0x61, 0x3a, 0x20, 0xe2, 0x82, 0x41]),
	b64([0x61, 0x3a, 0x20, 0xf0, 0x9f, 0x98]), b64([0x61, 0x3a, 0x20, 0xc0, 0xaf]), b64([0x61, 0x3a, 0x20, 0xed, 0xa0, 0x80]),
	b64([0x61, 0x3a, 0x20, 0xf4, 0x90, 0x80, 0x80]), b64([0x61, 0x3a, 0x20, 0xf5, 0x80]), b64([0x80, 0x3a, 0x20, 0x31]),
	b64([0x26, 0x80, 0x20, 0x31, 0x0a, 0x2a, 0x80]), b64([0x22, 0xff, 0x22]), b64([0x27, 0xc3, 0x27]), b64([0x7c, 0x0a, 0x20, 0xc3, 0xa9, 0xff]),
	b64([0xef, 0xbb, 0xbf, 0x61, 0x3a, 0x20, 0x31]), b64([0xef, 0xbb, 0x61]),
	"a: \ud800", "a: \udfff x", "\ud83d\ude00: \ud83d", "\ufeff\ufeffa: 1", "a: 1\ufeff", "a\0b", "\0", "a: \"\0\"", "# \0",
	"? {toString: 1}\n: x", "? {valueOf: 1}\n: x", "? {toString: 1, valueOf: 2}\n: x", "? [[{toString: 1}]]\n: x", "? []\n: x", "? {}\n: x",
	"? [1, [2, 3]]\n: x\ny: 2", "? [a, b]\n: 1\n? [a, b]\n: 2", "? {a: 1}\n: x\nb: 2\n? [c]\n: 3", "&a [*a]", "&a {b: *a}", "&a [1, *a, [*a]]",
	"&a {? *a : 1}", "&a [{? *a : 1}]", "&a {<<: *a}", "&a [{<<: *a}]", "&a {b: {<<: *a}}", "&a {<<: [*a]}", "a: &x {b: *x, <<: *x}",
	"__proto__: 1\nx: 2", "__proto__: {a: 1}", "\"__proto__\": [1]", "constructor: 1\ntoString: 2\nvalueOf: 3\nhasOwnProperty: 4",
	"2: a\n1: b\nx: c\n0: d\n-1: e\n1.5: f\n4294967295: g\n4294967294: h\n01: i", "a: 1\na: 2\nb: 3\na: 4", "null: 1\n~: 2\ntrue: 3\n1e3: 4\n.nan: 5\n0x10: 6\n-0: 7",
	"<<: {a: 1, b: 2}\nc: 3", "c: 3\n<<: {a: 1, b: 2, a: 5}", "a: 1\n<<: {a: 2}", "<<: [{a: 1}, {a: 2, b: 3}, 5, [x]]\n", "<<: 5", "<<: [1, 2]", "\"<<\": {a: 1}",
	"'<<': {a: 1}", "<<: *u", "x: &a {p: 1, q: 2}\ny:\n  <<: *a\n  q: 3\n  <<: {r: 4}", "<<: {.nan: 1, .nan: 2, 0: 3, -0: 4, null: 5, ~: 6}",
	"? .nan\n: 1\n? .nan\n: 2", "!!str <<: {a: 1}", "<<: !!str {a: 1}", "base: &b {x: 1}\nd1: {<<: *b, <<: *b}",
	"", " ", "\n", "\n\n", "#", "# c", "---", "--- ", "...", "---\n...", "--- a\n--- b\n--- c", "a\n---\nb\n...\n---\nc", "...\n...\na", "--- |\n  x\n--- >\n  y",
	"%YAML 1.2\n---\nx", "%YAML 1.2\n%YAML 1.2\n---\nx", "%YAML 1.2", "%TAG !e! tag:e:\n---\n!e!x 1", "!e!x 1", "%FOO\n---\n1", " %YAML 1.2\n---\n1", "%YAML1.2\n---\n1",
	"%YAML 1.2 # c\n---\n1", "%YAML 1.2#c\n---\n1", "%TAG ! !foo-\n---\n!bar x", "%TAG !! tag:x:\n---\n!!int 5", "%TAG !e!", "%TAG !e! x\n%TAG !e! y\n---\n!e!a 1",
	"!!str", "!!int 5", "!!int 0x1F", "!!int 5.5", "!!int true", "!!float 5", "!!float .inf", "!!bool true", "!!bool 1", "!!null null", "!!null ~", "!!null x",
	"!!str 5", "!!str null", "! 5", "!foo 5", "!<tag:x> 5", "!<!x> 5", "!<> 5", "!<tag:x 5", "!!str\n- a", "a: !!str", "a: !\nb: 1", "!!str : x", "! : x",
	"&a !!str : x\n", "!!map {a: 1}", "!!seq [1]", "!!int [1]", "!!null [1]", "&a &b x", "&a\n&b x", "&a\n&b\n&c x", "!!str !!int x", "!!str\n!!int x",
	"&a\n!!str\nk: v", "&m\n&k k: v", "&m\n&k\nk: v", "!t1\n!t2 k: v", "a: &x 1\nb: *x", "a: *x\nb: &x 1", "&a a: *a", "*a", "* a", "&", "&a", "&a\n", "*a: 1",
	"&a x\n*a : y", "&a [1]\n---\n*a", "- &a 1\n- *a\n- &a 2\n- *a",
	"a: [1", "\"abc", "'abc", "a: 'x\n...\n'", "a: \"x\n---\n\"", "- a\nb: 1", "a: 1\n- b", "a:\n- b\n- c", "a:\n  - b\n -c", "[a, b]: c", "{a: b}: c",
	"[a\n,b]", "a: [b\nc]", "a:\n  [b\nc]", "a: {b\n: c}", "{a: b: c}", "[a: b: c]", "[a: b, c: d]", "{? a}", "[? a]", "[? a : b]", "{? : x}", "[?]", "{?}",
	"[a, ? b]", "[\"a\": b]", "[\"a\":b]", "[\"a\":\nb]", "[a:b]", "{a:b}", "{\"a\":b}", "{a:\"b\"}", "[\"a\":]", "[\"a\":, b]", "{a: , b: }", "{: x}", "[: x]",
	"[,]", "[a,,b]", "{,}", "[a, b,]", "{a: 1,}", "[- a]", "{- a}", "[a - b]", "[-a]", "[a]b", "[a] b", "{a: 1} # c", "{a: 1}x", "]", "}", ",", "a, b",
	"a: |\n  x\n  y\n", "a: |-\n  x\n\n", "a: |+\n  x\n\n", "a: >\n  x\n  y\n\n  z\n", "a: >\n  x\n    more\n  y\n", "a: |2\n   x\n", "a: |1\n  x", "|\n\tx",
	"|\n  \tx", "a: |\n   x\n  y", "a: |\n  x\n   y", "|", "|\n", "|-", "|+\n\n", ">\n\n\n", "|x\n a", "|-+\n a", "|12\n a", "| # c\n  x", "|#c\n x", ">2-\n   x\n",
	"- |\n  x\n- >\n  y", "a: |\n  x\nb: 1", "a: >-\n  trimmed\n", ">\n  folded\n  text\n\n  para\n", ">\n a\n\n\n b", "|\n  ---\n  ...", "|\n---\n", "|\n  a\n---",
	"a: 'it''s'", "a: 'x\n  y\n\n  z'", "a: '  lead'", "a: 'trail  '", "a: 'x\r\n  y'", "a: \"x\r\n  y\"", "a: \"\\\r\n  y\"", "a: \"x\\\n   y\"",
	"a: \"\\u00e9\\x41\\U0001F600\\uD83D\\uDE00\"", "a: \"\\uD83D\"", "a: \"\\uDE00\"", "a: \"\\U00110000\"", "a: \"\\q\"", "a: \"\\0\\a\\b\\t\\n\\v\\f\\r\\e\\ \\\"\\/\\\\\\N\\_\\L\\P\"",
	"a: \"\\x\"", "a: \"\\xg0\"", "a: \"\\u12\"", "a: \"tab\there\"", "\"a\nb\": c", "'a\nb': c", "\"multi\n  line\"", "  \"x\"\n  : y",
	"0x1F", "0o17", "-0", ".NaN", "1e400", "0b11", "12:30", "2001-12-14", "c: yes", "~", "- -1\n- +1\n- -.5\n- +.inf\n- -.Inf\n- -0x10\n- +0o7",
	"a:\tb", "a:\t\tb", "a: \tb", "\ta: b", "a:\n\tb: c", "a:\n  \tb: c", "- \ta", "-\ta", "? \ta\n: b", "a\t: b", "[\ta]", "a: [\n\tb]", "{\ta: b}",
	"a:\n  b: 1\n\tc: 2", "a: 1 # c\n# d\nb: 2", "a: 1# c", "a: x#y", "a: x #y", "#a: 1\nb: 2", "a: 1\n  # indented comment\nb: 2",
	"a:\n  b:\n    c:\n      d: 1\n  e: 2\nf: 3", "a:\n- 1\n- 2\nb:\n  - 3", "- - - x", "- - a\n  - b\n- c", "- a: 1\n  b: 2\n- c: 3", "-\n  a: 1",
	"? a\n? b\nc: 1", "? - a\n  - b\n: - c", "? a\n: b\n? c\n: d", "?\n: x", ": x", ":", "? a\n:", "a:\n: b", "a: b\n: c", "- ? a\n  : b",
	"plain\n  continued\n  lines", "a: plain\n  continued", "a: plain\n\n  para", "a: x\n b", "a:\n  x\n y", "key with spaces: v", "a b c: d",
	"a: b: c", "a: - b", "- a - b", "a: [b] c", "\"a\" b: c", "a: 'b' c", "a: \"b\" c", "a:b: c", "a :b", "a : b", "a:  b  ", "a: b   # c   ",
	"x:\r\n  y: 1\r\n  z: 2\r\n", "a: 1\rb: 2", "a: |\r\n  x\r\n  y\r\n", "a: >\r\n  x\r\n\r\n  y\r\n", "\"a\r\nb\"", "'a\r\n\r\nb'",
	"@x", "`x", "a: @x", "a: `x", "- @", "a: %x", "%", "a: %", "a: !", "a: &", "a: *", "a: |x", "a: >x", "a: 'x", "a: \"x",
	"[".repeat(100) + "]".repeat(100), "- ".repeat(100) + "x", "{a: ".repeat(100) + "1" + "}".repeat(100),
	"[".repeat(500) + "]".repeat(500), "- ".repeat(500) + "x", "{a: ".repeat(500) + "1" + "}".repeat(500),
];

// Merge budget (MAX_MERGED_PROPERTIES, 2^20) and alias expansion (2^24 nodes).
const bigAnchor = "a: &a {" + Array.from({ length: 1100 }, (_, i) => "k" + i + ": " + i).join(", ") + "}\n";
EDGE.push(bigAnchor + Array.from({ length: 960 }, (_, i) => "m" + i + ": {<<: *a}").join("\n"));
EDGE.push(bigAnchor + Array.from({ length: 940 }, (_, i) => "m" + i + ": {<<: *a}").join("\n"));
EDGE.push("a: &a [x,x,x,x,x,x,x,x,x,x]\nb: &b [*a,*a,*a,*a,*a,*a,*a,*a,*a,*a]\nc: &c [*b,*b,*b,*b,*b,*b,*b,*b,*b,*b]\nd: &d [*c,*c,*c,*c,*c,*c,*c,*c,*c,*c]\ne: &e [*d,*d,*d,*d,*d,*d,*d,*d,*d,*d]\nf: &f [*e,*e,*e,*e,*e,*e,*e,*e,*e,*e]\ng: &g [*f,*f,*f,*f,*f,*f,*f,*f,*f,*f]\nh: &h [*g,*g,*g,*g,*g,*g,*g,*g,*g,*g]\n");
EDGE.push("a: &a [x,x,x,x,x,x,x,x,x,x]\nb: &b [*a,*a,*a,*a,*a,*a,*a,*a,*a,*a]\nc: &c [*b,*b,*b,*b,*b,*b,*b,*b,*b,*b]\nd: &d [*c,*c,*c,*c,*c,*c,*c,*c,*c,*c]\ne: &e [*d,*d,*d,*d,*d,*d,*d,*d,*d,*d]\n");
EDGE.push("a: &a [" + "[".repeat(300) + "]".repeat(300) + "]\nb: &b [" + "[".repeat(300) + "*a" + "]".repeat(300) + "]\nc: [*b, *b]");

// Nesting beyond the depth Node's default stack carries this port to; Bun's
// own limit (~4000-4700 levels) is not stable between runs.
const DEPTH = [];
for (const n of [800, 1000, 1500, 2000, 3000, 6000]) {
	DEPTH.push("[".repeat(n) + "]".repeat(n), "- ".repeat(n) + "x", "{a: ".repeat(n) + "1" + "}".repeat(n));
}
DEPTH.push("a: &a " + "[".repeat(700) + "]".repeat(700) + "\n" + Array.from({ length: 8 }, (_, i) => "b" + i + ": &b" + i + " " + "[".repeat(500) + (i ? "*b" + (i - 1) : "*a") + "]".repeat(500)).join("\n"));

// Around Bun's limits (deepest accepted: flow sequence 4723, flow mapping 4515,
// compact sequence 4085, block mapping 3364; Bun itself wavers by one level
// there, so these stay 3 off), and a mixed nesting around where adding the
// per-construct costs puts its limit (~2308 `[{a: ` pairs).
const DEPTH_BAND = [];
const blockMap = (n) => Array.from({ length: n }, (_, i) => " ".repeat(i) + "a:").join("\n") + " 1";
for (const [g, t] of [[(n) => "[".repeat(n) + "]".repeat(n), 4723], [(n) => "{a: ".repeat(n) + "1" + "}".repeat(n), 4515],
	[(n) => "- ".repeat(n) + "x", 4085], [blockMap, 3364]]) DEPTH_BAND.push(g(t - 3), g(t + 3));
for (const n of [2200, 2290, 2300, 2316, 2330, 2400]) DEPTH_BAND.push("[{a: ".repeat(n) + "1" + "}]".repeat(n));

const corpus = JSON.parse(readFileSync(new URL("./yaml-parse-corpus.json", import.meta.url), "utf8"));
const grammar = [];
for (let i = 0; i < 30000; i++) grammar.push(documentStream());
clean = true;
const grammarClean = [];
for (let i = 0; i < 20000; i++) grammarClean.push(documentStream());
clean = false;
const seeds = [...EDGE.filter((x) => typeof x === "string" && x.length < 2000), ...corpus.frontmatter, ...corpus.yamlTestSuite, ...grammar.slice(0, 5000)];
const mutations = [];
for (let i = 0; i < 40000; i++) mutations.push(mutate(pick(seeds)));
const frontmatterMut = [];
for (let i = 0; i < 5000; i++) frontmatterMut.push(mutate(pick(corpus.frontmatter)));

const cats = {
	edge: EDGE,
	depth: DEPTH,
	depthBand: DEPTH_BAND,
	frontmatter: corpus.frontmatter,
	markdownHead: corpus.markdownHead,
	yamlTestSuite: corpus.yamlTestSuite,
	grammar,
	grammarClean,
	mutation: mutations,
	frontmatterMutation: frontmatterMut,
};
const out = {};
for (const [name, inputs] of Object.entries(cats)) {
	out[name] = inputs.map((d) => [d, outcome(() => Bun.YAML.parse(materialize(d)))]);
	const errs = out[name].filter(([, r]) => r.startsWith("!")).length;
	console.log(`${name}: ${inputs.length} vectors (${errs} throw)`);
}
await Bun.write(process.argv[2] + "/yaml-parse.cases.json", JSON.stringify(out));
