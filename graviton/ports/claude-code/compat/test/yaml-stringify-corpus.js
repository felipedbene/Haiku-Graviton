// Shared by Bun and Node: builds the same YAML.stringify inputs in either
// runtime and runs every (value, replacer, space) combination through a given
// stringify. Every value is built fresh per call (getters below mutate their
// objects), and getters / traps / valueOf append to `log`, so the order in
// which the stringifier observes the value is compared too.
//
// Number constraint, applied by construction: JSC prints a zero held as a boxed
// double as "+0" (see divergent() and yaml-stringify.mjs isInt32), so zeros
// here are int32 literals and never share an array with a non-int32 number.

const KEYWORDS = ["true", "True", "TRUE", "false", "False", "FALSE", "yes", "Yes", "YES", "no", "No", "NO", "on", "On", "ON",
	"off", "Off", "OFF", "n", "N", "y", "Y", "null", "Null", "NULL", "~", ".inf", ".Inf", ".INF", ".nan", ".NaN", ".NAN"];
const STRINGS = [
	"", " ", "a", "a ", " a", "a:", "a: b", "a:b", "a:\tb", "a:\nb", "a:\rb", ":a", "a #b", "a#b", "#", "-", "--", "---", "--- a", "---a", "---[", "---,",
	"...", "... ", "....", "...a", "...}", "a...", "a...b", "a..b", "a---", "a--- b", "a---b", "a---{", "-a", "a-b", "a.b", ".", "..", "x...\t",
	"-1", "+1", "1", "01", "007", "0x1F", "0X1f", "0x", "0xG", "0o7", "0O7", "0o", "0o8", "1e5", "1E5", "1e", "1e+5", "1e-5", "1e5e5", "1.2.3", "1-2", "1--2", "1+5",
	"+-1", "--1", "-1-2", "0b1", "1,000", "1_000", "1a", "a1", "12:30", "1:2", "0.", ".5", "5.", "-.5", "+.5", "1.5e3", "-1.5E-3", "0xe", "0x1e5", "0x1.5",
	"+.inf", "-.inf", "+.Inf", "-.INF", "+.InF", "-.nan", "+.infx", "+", "+a", "+.", "-.", "1 2", "Infinity", "-Infinity", "NaN", "nan", "inf",
	"tRuE", "yEs", "nULL", "~a", "a~", "null ", " null", "TRUE!", "on ", "y:", "\u0000", "a\u0000b", "\u0001", "\u0006", "\u0007", "\u0008", "\t", "\n", "\u000b",
	"\u000c", "\r", "\u000e", "\u001a", "\u001b", "\u001f", "\u007f", "a\u0085b", "\u0085", "a\u00a0b", "\u00a0", "a\u2028b", "a\u2029b", "\ufeff", "\ufeffa", "a\ufeff",
	"é", "Ünïcödé", "漢字", "😀", "a😀b", "\ud800", "\udc00x", "x\ud800", "a\"b", "\"", "a'b", "'", "a`b", "`", "a\\b", "\\", "multi\nline", "\ttab", "tab\t", "a\tb",
	"a\rb", "@x", "%x", "!x", "&x", "*x", "?x", "|x", ">x", "<x", "[x", "]x", "{x", "}x", ",x", "a,b", "a[b", "a]b", "a{b", "a}b", "a?b", "a&b", "a*b", "a!b",
	"a|b", "a>b", "a<b", "a%b", "a@b", "x: y: z", "http://example.com/a?b=c#d", "C:\\path", "a b c", "a  b", "key", "root", "value", "item", "value0", "item1",
	"root2", "value01", "item-1", "valuex", "root.1", "\u00ff\u0100", "\u2027\u202a", "\uffff", "\u0080\u0084\u0086\u009f",
];
for (const k of KEYWORDS) STRINGS.push(k);
STRINGS.push("x".repeat(5000), "ab:".repeat(300) + "c", "\u00e9".repeat(1000));

// No 0 here: this array is Double-shaped in JSC, so a 0 read from it is a
// boxed double ("+0"). The int32 zero cases are literals in corpus().
const NUMBERS = [-0, 1, -1, 7, 42, 2147483647, -2147483648, 2147483648, -2147483649, 4294967295, 4294967296, 1.5, -1.5, 0.1, 0.3, 1e21, 1e-7, 1e20,
	123456789.123, 1e300, -1e300, 5e-324, 1.7976931348623157e308, 9007199254740991, 9007199254740992, 2 ** 64, NaN, Infinity, -Infinity, 1 / 3, -2.5e-8, 100];

function logged(log, tag, target, keys) {
	const o = {};
	for (const k of keys) Object.defineProperty(o, k, { enumerable: true, configurable: true, get() { log.push(tag + "." + k); return target[k]; } });
	return o;
}

function tracedProxy(log, target, tag) {
	return new Proxy(target, {
		ownKeys(t) { log.push(tag + " ownKeys"); return Reflect.ownKeys(t); },
		getOwnPropertyDescriptor(t, k) { log.push(tag + " gopd " + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
		get(t, k, r) { log.push(tag + " get " + String(k)); return Reflect.get(t, k, r); },
		has(t, k) { log.push(tag + " has " + String(k)); return Reflect.has(t, k); },
		getPrototypeOf(t) { log.push(tag + " getPrototypeOf"); return Reflect.getPrototypeOf(t); },
	});
}

// [name, (log) => value]
export function corpus() {
	const C = [];
	const add = (name, f) => C.push([name, f]);
	// primitives and roots that print nothing
	for (const v of [null, true, false]) add("prim " + String(v), () => v);
	add("num 0", () => 0);
	for (const n of NUMBERS) add("num " + (Object.is(n, -0) ? "-0" : String(n)), () => n);
	add("undefined", () => undefined);
	add("symbol", () => Symbol("s"));
	add("function", () => function f() {});
	add("arrow", () => () => 1);
	add("class", () => class K {});
	add("bigint root", () => 10n);
	add("Array ctor root", () => Array);
	for (const s of STRINGS) add("str " + JSON.stringify(s.length > 40 ? s.slice(0, 40) + "…" + s.length : s), () => s);
	// every string as a key and as a value inside a container
	add("all strings as keys", () => { const o = {}; STRINGS.forEach((s, i) => { o[s] = i; }); return o; });
	add("all strings as values", () => STRINGS.slice());
	add("all strings nested", () => STRINGS.map((s) => ({ [s]: [s, { k: s }] })));
	add("all numbers", () => NUMBERS.concat(["x"]));
	add("numbers in obj", () => { const o = {}; NUMBERS.forEach((n, i) => { o["k" + i] = n; }); return o; });
	add("int array with 0", () => [0, 1, 2, -5, 0]);
	add("zero in obj", () => ({ a: 0, b: [0], c: { d: 0 } }));
	add("-0 alone in array", () => [-0]);
	// structure / after_key layout
	const S = {
		"empty obj": () => ({}), "empty arr": () => [], "nested empty": () => ({ a: {}, b: [], c: [[]], d: [{}], e: { f: {} } }),
		"arr in obj": () => ({ a: [1, 2, 3] }), "obj in arr": () => [{ a: 1, b: 2 }, { c: 3 }], "arr in arr": () => [[1, 2], [3, [4, [5]]], []],
		"deep obj": () => ({ a: { b: { c: { d: { e: 1 } } } } }), "mixed": () => ({ a: [{ b: [{ c: 1, d: [2, { e: 3 }] }] }], f: "g", h: null }),
		"arr of empties": () => [{}, [], {}, []], "obj after key arr": () => ({ k: [{ x: 1 }, [1, 2], "s"] }),
		"only undefined arr": () => [undefined, undefined], "only undefined obj": () => ({ a: undefined, b: () => 1, c: Symbol("c") }),
		"nested only undefined": () => ({ a: [undefined], b: { c: undefined }, d: [[undefined]] }),
		"skips in arr": () => [1, undefined, () => 2, Symbol("x"), 3, null], "skips in obj": () => ({ a: 1, b: undefined, c: () => 2, d: Symbol("x"), e: null }),
		"holes": () => [1, , 3, , ], "sparse big": () => { const a = []; a[5] = "x"; a[20] = { y: 1 }; return a; },
		"only holes": () => new Array(4), "obj with arr only undefined after key": () => ({ a: [undefined], b: [] }),
		"index keys": () => ({ 2: "a", 1: "b", b: 1, a: 2, "-1": 3, "1.5": 4, "01": 5, 4294967294: 6, 4294967295: 7 }),
		"__proto__ own": () => JSON.parse('{"__proto__": {"x": 1}, "y": 2}'), "null proto": () => Object.assign(Object.create(null), { a: 1, b: { c: 2 } }),
		"proto props not own": () => Object.assign(Object.create({ inh: 1, get g() { return 2; } }), { own: 3 }),
		"nonenum": () => Object.defineProperty({ a: 1 }, "h", { value: 2, enumerable: false }), "symbol keys": () => ({ [Symbol("s")]: 1, a: 2, [Symbol.for("t")]: 3 }),
		"class instance": () => { class P { constructor() { this.x = 1; this.y = [2]; } m() {} } return new P(); },
		"array subclass": () => { class A extends Array {} const a = new A(); a.push(1, "b", { c: 3 }); return a; },
		"array extra props": () => Object.assign([1, 2], { foo: "bar" }), "arguments": () => (function () { return arguments; })(1, "a", [2]),
		"date": () => new Date(0), "date in obj": () => ({ d: new Date(86400000) }), "map": () => new Map([[1, 2]]), "set": () => new Set([1]), "regexp": () => /x/gi,
		"error": () => new Error("boom"), "typeerror in arr": () => [new TypeError("t")], "u8": () => new Uint8Array([1, 2, 3]), "i32": () => new Int32Array([5, -7]),
		"u8 in obj": () => ({ b: new Uint8Array([0, 255]) }), "arraybuffer": () => new ArrayBuffer(4), "dataview": () => new DataView(new ArrayBuffer(2)),
		"promise": () => Promise.resolve(1), "weakmap": () => new WeakMap(), "math": () => Math, "json": () => JSON,
		"functions in obj": () => ({ a: Array, b: Map, c: Function.prototype, d: (() => 1).bind(null), e: async () => 1, f: class {}, g: function* () {}, h: async function* () {}, i: Object, z: 1 }),
		"functions in arr": () => [Array, Function.prototype, (() => 1).bind(null), class {}, 1],
		"toJSON ignored": () => ({ toJSON() { return "nope"; }, a: 1 }), "toJSON root date-like": () => ({ toJSON: () => 1 }),
		"boxed": () => [new Number(3), new Number(-0), new Number(0), new Number(1.5), new Number(NaN), new String("yes"), new String(""), new String("a b"), new Boolean(true), new Boolean(false)],
		"boxed in obj": () => ({ n: new Number(7), s: new String("x:"), b: new Boolean(false), e: new String("") }),
		"boxed root num": () => new Number(42), "boxed root str": () => new String("no"), "boxed root bool": () => new Boolean(true),
		"boxed symbol": () => Object(Symbol("q")), "boxed symbol in obj": () => ({ s: Object(Symbol("q")), t: [Object(Symbol("r"))] }),
		"boxed bigint": () => Object(5n), "bigint nested": () => ({ a: [1, { b: 2n }] }), "bigint in arr": () => [1, 2, 3n],
		"long key": () => ({ ["k".repeat(300)]: "v".repeat(300) }), "many keys": () => { const o = {}; for (let i = 0; i < 300; i++) o["key" + i] = i % 7 ? i : [i]; return o; },
		"deep 1000 arr": () => { let a = [1]; for (let i = 0; i < 1000; i++) a = [a]; return a; },
		"deep 1000 obj": () => { let a = { x: 1 }; for (let i = 0; i < 1000; i++) a = { x: a }; return a; },
		"deep 1e6 arr": () => { let a = []; for (let i = 0; i < 1e6; i++) a = [a]; return a; },
		"deep 1e6 obj": () => { let a = {}; for (let i = 0; i < 1e6; i++) a = { a }; return a; },
	};
	for (const [k, f] of Object.entries(S)) add(k, f);

	// anchors and aliases
	const A = {
		"shared in arr": () => { const o = { a: 1 }; return [o, o, o]; },
		"shared arr in arr": () => { const a = [1, 2]; return [a, [a], a]; },
		"shared across props": () => { const o = { x: [1] }; return { p: o, q: o, r: { s: o } }; },
		"self cycle": () => { const a = { x: 1 }; a.self = a; return a; },
		"arr self cycle": () => { const a = [1]; a.push(a); return a; },
		"nested cycle": () => { const a = { b: { c: {} } }; a.b.c.back = a.b; return a; },
		"mutual": () => { const a = { n: "a" }, b = { n: "b" }; a.b = b; b.a = a; return [a, b]; },
		"prop names": () => {
			const names = ["root", "value", "value0", "item1", "root2", "a b", "", "é", "x-y.z_1", "item", "valuex", "-", ".", "0", "value", "Value"];
			const o = {}; const shared = names.map((n) => ({ n }));
			names.forEach((n, i) => { o[n] = shared[i]; });
			o.again = names.map((_, i) => shared[i]);
			return o;
		},
		"same prop name many": () => { const r = []; for (let i = 0; i < 12; i++) { const s = { i }; r.push({ k: s }, { k: s }); } return r; },
		"unsafe names counter": () => { const s1 = {}, s2 = [], s3 = { z: 1 }; return [{ "a b": s1 }, { "a:b": s2 }, { value: s3 }, s1, s2, s3, { value: s1 }]; },
		"literal value then unsafe": () => { const s1 = [1], s2 = [2]; return { value: s1, x: s1, "%": s2, y: s2 }; },
		"root named prop": () => { const s = { a: 1 }; return { root: s, r2: s }; },
		"root and prop root": () => { const r = { k: 1 }; const s = { a: 1 }; r.root = s; r.copy = s; r.me = r; return r; },
		"shared empty": () => { const e = {}, a = []; return { e1: e, e2: e, a1: a, a2: a, l: [e, a, e, a] }; },
		"shared in arr after key": () => { const o = { x: 1 }, a = [1]; return { k: [o, a], m: o, n: a }; },
		"alias of item in nested": () => { const o = { deep: [1] }; return [[o], [[o]], { q: o }]; },
		"item counter order": () => { const a = { a: 1 }, b = { b: 1 }, c = { c: 1 }; return [a, b, c, c, b, a]; },
		"anchor root arr": () => { const a = [1, 2]; a.push({ back: a }); return a; },
		"shared boxed": () => { const n = new Number(1); return [n, n]; },
		"shared string not anchor": () => { const s = "same"; return [s, s, { s, t: s }]; },
		"shared fn skipped": () => { const f = () => 1; return { a: f, b: f, c: [f] }; },
		"shared deep chain": () => { const leaf = { leaf: true }; const mid = { leaf, l2: leaf }; return { mid, m2: mid, leaf }; },
		"anchor under item offset": () => { const s = { a: { b: 1 } }; return [[s, [s]], { x: [s] }]; },
	};
	for (const [k, f] of Object.entries(A)) add("anchor " + k, f);

	// observation order: getters, traps, valueOf
	const O = {
		"getter log": (log) => logged(log, "g", { a: 1, b: [2], c: { d: 3 } }, ["a", "b", "c"]),
		"getter fresh object": (log) => ({ get o() { log.push("o"); return { fresh: 1 }; }, get p() { log.push("p"); return [1]; } }),
		"getter shared": (log) => { const s = { s: 1 }; return { get a() { log.push("a"); return s; }, b: s }; },
		"getter deletes later": (log) => ({ get a() { log.push("a"); delete this.b; return 1; }, b: 2, c: 3 }),
		"getter adds": (log) => ({ get a() { log.push("a"); this.z = 9; return 1; }, b: 2 }),
		"getter shadow proto": (log) => { const proto = { get b() { log.push("proto b"); return "from proto"; } }; const o = Object.create(proto); o.a = { get z() { log.push("z"); return 1; } }; Object.defineProperty(o, "b", { enumerable: true, configurable: true, get() { log.push("own b"); delete o.b; return 2; } }); o.c = 3; return o; },
		"getter delete reveals proto": (log) => { const proto = { b: "proto" }; const o = Object.create(proto); Object.defineProperty(o, "a", { enumerable: true, get() { log.push("a"); delete o.b; return 1; } }); o.b = "own"; return o; },
		"setter only": () => Object.defineProperty({}, "s", { enumerable: true, set(v) {} }),
		"getter throws": (log) => ({ a: 1, get b() { log.push("b"); throw new TypeError("getter boom"); } }),
		"getter throws non-error": () => ({ get b() { throw "str"; } }),
		"getter returns bigint": (log) => ({ get a() { log.push("a"); return 1n; } }),
		"bigint after getter": (log) => ({ get a() { log.push("a"); return 1; }, b: 2n, get c() { log.push("c"); return 3; } }),
		"index getter arr": (log) => { const a = [1, 2]; Object.defineProperty(a, 1, { enumerable: true, get() { log.push("i1"); return "two"; } }); return a; },
		"arr shrinks": (log) => { const a = [1, 2, 3, 4]; Object.defineProperty(a, 0, { enumerable: true, configurable: true, writable: true, value: { get x() { log.push("x"); a.length = 2; return 1; } } }); return a; },
		"proxy plain": (log) => tracedProxy(log, { a: 1, b: { c: 2 } }, "P"),
		"proxy arr": (log) => tracedProxy(log, [1, [2]], "PA"),
		"proxy callable": (log) => tracedProxy(log, function f() {}, "PF"),
		"proxy callable with props": () => { const f = function () {}; f.a = 1; return new Proxy(f, {}); },
		"proxy in proto": (log) => { const o = Object.create(tracedProxy(log, { inh: 1 }, "PP")); o.own = 2; return o; },
		"proxy nonenum hidden": (log) => new Proxy({ a: 1, b: 2 }, { getOwnPropertyDescriptor(t, k) { log.push("gopd " + String(k)); const d = Reflect.getOwnPropertyDescriptor(t, k); if (k === "b") d.enumerable = false; return d; } }),
		"proxy get lies": (log) => new Proxy({ a: 1, b: 2 }, { get(t, k) { log.push("get " + String(k)); return k === "a" ? undefined : "lie"; } }),
		"proxy shared": (log) => { const p = tracedProxy(log, { z: 1 }, "S"); return [p, p]; },
		"boxed valueOf log": (log) => { const n = new Number(5); n.valueOf = () => { log.push("valueOf"); return 6; }; const s = new String("q"); s.toString = () => { log.push("toString"); return "yes"; }; return { n, s, arr: [n, s] }; },
		"boxed toPrimitive": (log) => { const n = new Number(5); n[Symbol.toPrimitive] = (h) => { log.push("prim " + h); return 9; }; const s = new String("q"); s[Symbol.toPrimitive] = (h) => { log.push("prim " + h); return "r"; }; return [n, s]; },
		"boxed valueOf throws": () => { const n = new Number(5); n.valueOf = () => { throw new RangeError("vo"); }; return { n }; },
		"boxed toString returns obj": (log) => { const s = new String("q"); s.toString = () => { log.push("ts"); return {}; }; s.valueOf = () => { log.push("vo"); return "fallback"; }; return [s]; },
		"boxed number valueOf string": (log) => { const n = new Number(5); n.valueOf = () => { log.push("vo"); return "12"; }; return [n]; },
		"boxed root valueOf": (log) => { const n = new Number(5); n.valueOf = () => { log.push("vo"); return 6; }; return n; },
		"boxed valueOf mutates arr": (log) => { const a = [0, 1, 2]; const n = new Number(1); n.valueOf = () => { log.push("vo"); a.length = 1; return 2; }; a[0] = n; return a; },
		"proto has getter for hole": () => { const a = [1, , 3]; Object.setPrototypeOf(a, Object.create(Array.prototype, { 1: { get() { return "proto"; } } })); return a; },
	};
	for (const [k, f] of Object.entries(O)) add("observe " + k, f);
	return C;
}

// [name, (log) => space]
export function spaces() {
	return [
		["absent", undefined], ["null", null], ["0", 0], ["1", 1], ["2", 2], ["4", 4], ["10", 10], ["11", 11], ["100", 100], ["-1", -1], ["2.7", 2.7], ["0.9", 0.9],
		["NaN", NaN], ["Infinity", Infinity], ["-Infinity", -Infinity], ["\"\"", ""], ["tab", "\t"], ["--", "--"], ["space", " "], ["long16", "0123456789ABCDEF"],
		["surrogate split", "abcdefghi\u{1F600}"], ["unicode", "é漢\u{1F600}"], ["new Number(3)", "NUMOBJ"], ["new String(ab)", "STROBJ"], ["true", true], ["{}", "OBJ"], ["[2]", "ARR"],
		["valueOf space", "VOSPACE"], ["13 chars", "0123456789ABC"],
	].map(([n, v]) => [n, (log) => v === "NUMOBJ" ? new Number(3) : v === "STROBJ" ? new String("ab") : v === "OBJ" ? {} : v === "ARR" ? [2]
		: v === "VOSPACE" ? (() => { const s = new Number(1); s.valueOf = () => { log.push("space valueOf"); return 3; }; return s; })() : v]);
}

export const replacers = () => [["absent", undefined, false], ["null", null, true], ["fn", (k, v) => v, true], ["arr", ["a"], true]];

// Deterministic xorshift32. `| 0` keeps every derived number int32 in JSC.
function rng(seed) {
	let s = (seed * 2654435761 + 0x9e3779b9) | 0;
	if (s === 0) s = 1;
	return (n) => { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; return ((s >>> 0) % n) | 0; };
}

const FUZZ_KEYS = ["a", "b", "key", "root", "value", "value0", "item", "item2", "x y", "", "é", "1", "01", "-1", "yes", "null", "a:", "a: b", "#c", "k-1",
	"k.2", "k_3", "\t", "[x]", "😀", "very long key name here", "on", "0x10", "~", "---"];
const FUZZ_STRINGS = STRINGS.filter((s) => s.length < 50);
const FUZZ_DOUBLES = [1.5, -2.25, 0.1, 1e21, 1e-7, 3.14159, -0, NaN, Infinity, -Infinity, 2147483648, -2147483649, 5e-324, 1e300];

// One random value; seeds are independent so a single case regenerates alone.
export function fuzz(seed) {
	const r = rng(seed);
	const pool = [];
	const leaf = () => {
		switch (r(20)) {
			case 0: return null;
			case 1: return r(2) === 0;
			case 2: case 3: return r(1000) - 500;
			case 4: return r(3) === 0 ? 0 : r(2147483647);
			case 5: return FUZZ_DOUBLES[r(FUZZ_DOUBLES.length)];
			case 6: { const k = r(2000000) - 1000000; if (k === 0) return 7; return k / 1000; }
			case 7: case 8: case 9: case 10: return FUZZ_STRINGS[r(FUZZ_STRINGS.length)];
			case 11: { let s = ""; const n = r(12); const alpha = "ab :#-.,'\"\\\n\t{}[]&*!|>%@`?~01e+x\u0085\u00a0\u2028\ufeff\u00e9\ud800\ud83d\ude00"; for (let i = 0; i < n; i++) s += alpha[r(alpha.length)]; return s; }
			case 12: return undefined;
			case 13: return r(2) ? Symbol("s") : () => 1;
			case 14: return [new Number(r(100)), new String(FUZZ_STRINGS[r(FUZZ_STRINGS.length)]), new Boolean(r(2) === 0)][r(3)];
			case 15: return [new Date(0), new Map(), Object(Symbol("b")), new Proxy(() => 1, {}), /r/, new Uint8Array([r(256), 7])][r(6)];
			case 16: return r(50) === 0 ? 5n : "s" + r(100);
			default: return pool.length ? pool[r(pool.length)] : "p";
		}
	};
	const node = (depth) => {
		if (depth >= 4 || r(3) === 0) return leaf();
		if (r(2) === 0) {
			const items = [];
			const n = r(6);
			for (let i = 0; i < n; i++) items.push(r(8) === 0 ? undefined : node(depth + 1));
			// No +0 next to a non-int32 number: the array would be Double-shaped
			// in JSC and print "+0" (see divergent()).
			const nonInt = items.some((v) => typeof v === "number" && ((v | 0) !== v || Object.is(v, -0)));
			const arr = [];
			pool.push(arr);
			for (const v of items) arr.push(nonInt && v === 0 && !Object.is(v, -0) ? 1 : v);
			if (r(10) === 0) arr.length += 2;
			return arr;
		}
		const obj = r(15) === 0 ? Object.create(null) : {};
		pool.push(obj);
		const n = r(6);
		for (let i = 0; i < n; i++) obj[FUZZ_KEYS[r(FUZZ_KEYS.length)]] = r(10) === 0 && pool.length ? pool[r(pool.length)] : node(depth + 1);
		return obj;
	};
	return node(0);
}

// Inputs where stock Bun's output depends on JSC value representation or engine
// error text, which Node does not expose. Reported, not counted.
export function divergent() {
	let h = 0.5;
	const dz = () => h - h;	// a computed zero: a boxed double in JSC
	return [
		["computed zero root", () => dz()],
		["computed zero in obj", () => ({ a: dz() })],
		["literal zero in Double-shaped array", () => [1.5, 0]],
		["zero pushed after double", () => { const a = [1.5]; a.push(0); return a; }],
		["zero before double then string", () => { const a = [0]; a.push(1.5); a.push("s"); return a; }],
		["Float64Array zero", () => new Float64Array([0, 1.5])],
		["-0 then 0", () => [-0, 0]],
		["engine TypeError: boxed valueOf returns BigInt", () => { const n = new Number(1); n.valueOf = () => 1n; return [n]; }],
		["engine TypeError: revoked proxy", () => { const p = Proxy.revocable({}, {}); p.revoke(); return { p: p.proxy }; }],
		["engine TypeError: String toString returns symbol", () => { const s = new String("a"); s.toString = () => Symbol("x"); s.valueOf = () => Symbol("y"); return [s]; }],
	];
}

const result = (f) => {
	try {
		const v = f();
		return typeof v === "string" ? { v } : { t: typeof v };
	} catch (e) {
		return { e: [e && e.constructor ? e.constructor.name : typeof e, e instanceof Error ? e.message : String(e)] };
	}
};

// Runs every combination through `stringify`; returns [[name, result, log]].
export function run(stringify, fuzzCount) {
	const out = [];
	const R = replacers();
	for (const [name, make] of corpus()) {
		for (const [rn, rep] of R) {
			for (const [sn, makeSpace] of spaces()) {
				const log = [];
				const value = make(log);
				const space = makeSpace(log);
				out.push([`${name} | replacer=${rn} | space=${sn}`, result(() => stringify(value, rep, space)), log]);
			}
		}
	}
	const FS = [["absent", () => undefined], ["0", () => 0], ["2", () => 2], ["4", () => 4], ["tab", () => "\t"], ["--", () => "--"], ["11", () => 11], ["-1", () => -1], ["13 chars", () => "0123456789ABC"]];
	for (let seed = 0; seed < fuzzCount; seed++) {
		for (const [rn, rep] of R) {
			for (const [sn, ms] of FS) out.push([`fuzz ${seed} | replacer=${rn} | space=${sn}`, result(() => stringify(fuzz(seed), rep, ms())), []]);
		}
	}
	return out;
}

export function runDivergent(stringify) {
	return divergent().map(([name, make]) => [name, result(() => stringify(make(), null, 2)), result(() => stringify(make()))]);
}

// Deepest nesting each runtime stringifies (minified and spaced), by bisection.
export function maxDepth(stringify) {
	const ok = (d, sp) => { let a = []; for (let i = 0; i < d; i++) a = [a]; try { stringify(a, null, sp); return true; } catch (e) { if (e instanceof RangeError) return false; throw e; } };
	const res = {};
	for (const sp of [0, 2]) {
		let lo = 1, hi = 1 << 22;
		while (lo + 1 < hi) { const mid = (lo + hi) >>> 1; if (ok(mid, sp)) lo = mid; else hi = mid; }
		res["space=" + sp] = lo;
	}
	return res;
}
