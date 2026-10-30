// Shared by Bun and Node: builds the same value pairs in either runtime.
export function corpus() {
	const P = [];
	const add = (name, a, b) => P.push([name, a, b]);
	const sym = Symbol.for("k"), sym2 = Symbol("local");
	// primitives
	for (const [a, b] of [[1, 1], [1, "1"], [0, -0], [NaN, NaN], [null, undefined], [null, null], [undefined, undefined], ["a", "a"], [1n, 1n], [1n, 1],
		[true, true], [sym, sym], [Symbol("x"), Symbol("x")], [Infinity, Infinity]]) add("prim", a, b);
	// plain objects
	add("obj same", { a: 1, b: [1, 2] }, { a: 1, b: [1, 2] });
	add("obj order", { a: 1, b: 2 }, { b: 2, a: 1 });
	add("obj undefined prop L", { a: 1, b: undefined }, { a: 1 });
	add("obj undefined prop R", { a: 1 }, { a: 1, b: undefined });
	add("obj extra", { a: 1 }, { a: 1, b: 2 });
	add("obj missing", { a: 1, b: 2 }, { a: 1 });
	add("obj both undef diff keys", { a: undefined }, { b: undefined });
	add("obj null proto", Object.create(null), {});
	add("obj null proto vals", Object.assign(Object.create(null), { x: 1 }), { x: 1 });
	class X { constructor() { this.v = 1; } }
	class Y { constructor() { this.v = 1; } }
	add("class vs plain", new X(), { v: 1 });
	add("class X vs Y", new X(), new Y());
	add("class same", new X(), new X());
	add("proto enumerable", Object.create({ inh: 1 }), {});
	add("proto enumerable both", Object.create({ inh: 1 }), Object.create({ inh: 2 }));
	add("index keys obj", { 0: "a", 1: "b" }, { 0: "a", 1: "b" });
	add("index keys vs array", { 0: "a", length: 1 }, ["a"]);
	add("getter", { get g() { return 1; } }, { g: 1 });
	add("getter both", { get g() { return 1; } }, { get g() { return 1; } });
	add("nonenum", Object.defineProperty({}, "h", { value: 1, enumerable: false }), {});
	add("nonenum R", {}, Object.defineProperty({}, "h", { value: 1, enumerable: false }));
	add("symbol key", { [sym]: 1 }, { [sym]: 1 });
	add("symbol key diff", { [sym]: 1 }, { [sym]: 2 });
	add("symbol key missing", { [sym]: 1 }, {});
	add("local symbol", { [sym2]: 1 }, { [sym2]: 1 });
	add("__proto__ own", JSON.parse('{"__proto__":1}'), JSON.parse('{"__proto__":1}'));
	add("nested", { a: { b: { c: [1, { d: 2 }] } } }, { a: { b: { c: [1, { d: 2 }] } } });
	add("nested diff", { a: { b: { c: [1, { d: 2 }] } } }, { a: { b: { c: [1, { d: 3 }] } } });
	const cyc1 = { a: 1 }; cyc1.self = cyc1; const cyc2 = { a: 1 }; cyc2.self = cyc2;
	add("cycle", cyc1, cyc2);
	const cyc3 = { a: 1 }; cyc3.self = { a: 1, self: cyc3 };
	add("cycle unrolled", cyc1, cyc3);
	// arrays
	add("arr", [1, 2, 3], [1, 2, 3]);
	add("arr len", [1, 2], [1, 2, undefined]);
	add("arr len R", [1, 2, undefined], [1, 2]);
	add("arr hole vs undef", [1, , 3], [1, undefined, 3]);
	add("arr hole vs hole", [1, , 3], [1, , 3]);
	add("arr hole vs val", [1, , 3], [1, 2, 3]);
	const ap = [1, 2]; ap.extra = "x";
	add("arr extra prop", ap, [1, 2]);
	const as = [1]; as[sym] = 1;
	add("arr symbol prop", as, [1]);
	add("arr getter", Object.defineProperty([1, 2], 1, { get() { return 2; } }), [1, 2]);
	add("arr vs obj", [], {});
	add("sparse long", (() => { const a = []; a[5] = 1; return a; })(), (() => { const a = []; a[5] = 1; return a; })());
	// specials
	add("date", new Date(0), new Date(0));
	add("date diff", new Date(0), new Date(1));
	add("date invalid", new Date(NaN), new Date(NaN));
	add("date vs obj", new Date(0), {});
	add("regexp", /a/g, /a/g);
	add("regexp flags", /a/g, /a/i);
	add("regexp src", /a/, /b/);
	const re = /a/; re.lastIndex = 3;
	add("regexp lastIndex", re, /a/);
	add("map", new Map([[1, { x: 1 }]]), new Map([[1, { x: 1 }]]));
	add("map diff val", new Map([[1, { x: 1 }]]), new Map([[1, { x: 2 }]]));
	add("map obj keys", new Map([[{ k: 1 }, 1]]), new Map([[{ k: 1 }, 1]]));
	add("map undefined val", new Map([[1, undefined]]), new Map([[2, undefined]]));
	add("map size", new Map([[1, 1]]), new Map());
	add("set", new Set([1, 2]), new Set([2, 1]));
	add("set obj", new Set([{ a: 1 }]), new Set([{ a: 1 }]));
	add("set diff", new Set([1]), new Set([2]));
	add("map vs set", new Map(), new Set());
	add("u8", new Uint8Array([1, 2]), new Uint8Array([1, 2]));
	add("u8 diff", new Uint8Array([1, 2]), new Uint8Array([1, 3]));
	add("u8 vs i8", new Uint8Array([1]), new Int8Array([1]));
	add("f64 zero", new Float64Array([0]), new Float64Array([-0]));
	add("f64 nan", new Float64Array([NaN]), new Float64Array([NaN]));
	add("f32 zero", new Float32Array([0]), new Float32Array([-0]));
	add("u8 empty", new Uint8Array(0), new Uint8Array(0));
	const tp = new Uint8Array([1]); tp.extra = 1;
	add("u8 extra prop", tp, new Uint8Array([1]));
	add("u8 vs buffer", new Uint8Array([1]), Buffer.from([1]));
	add("buffer", Buffer.from("ab"), Buffer.from("ab"));
	add("ab", new Uint8Array([1, 2]).buffer, new Uint8Array([1, 2]).buffer);
	add("ab diff", new Uint8Array([1, 2]).buffer, new Uint8Array([1, 3]).buffer);
	add("dataview", new DataView(new ArrayBuffer(2)), new DataView(new ArrayBuffer(2)));
	add("error", new Error("x"), new Error("x"));
	add("error msg", new Error("x"), new Error("y"));
	add("error type", new Error("x"), new TypeError("x"));
	add("error cause", new Error("x", { cause: 1 }), new Error("x", { cause: 1 }));
	add("error cause diff", new Error("x", { cause: 1 }), new Error("x", { cause: 2 }));
	add("error cause missing", new Error("x", { cause: undefined }), new Error("x"));
	const e1 = new Error("x"); e1.code = "E"; const e2 = new Error("x"); e2.code = "E";
	add("error props", e1, e2);
	const e3 = new Error("x"); e3.code = "F";
	add("error props diff", e1, e3);
	class MyErr extends Error {}
	add("error subclass", new MyErr("x"), new Error("x"));
	add("string obj", new String("a"), new String("a"));
	add("string obj diff", new String("a"), new String("b"));
	add("string obj vs prim", new String("a"), "a");
	add("number obj", new Number(1), new Number(1));
	add("number obj -0", new Number(0), new Number(-0));
	add("bool obj", new Boolean(true), new Boolean(true));
	add("number vs bool obj", new Number(1), new Boolean(true));
	add("symbol obj", Object(sym), Object(sym));
	add("bigint obj", Object(1n), Object(1n));
	add("bigint obj diff", Object(1n), Object(2n));
	add("fn", function f() {}, function f() {});
	const f = () => 1;
	add("fn same", f, f);
	add("obj with fn", { f }, { f });
	add("global", globalThis, globalThis);
	add("promise", Promise.resolve(1), Promise.resolve(1));
	add("weakmap", new WeakMap(), new WeakMap());
	add("arguments", (function () { return arguments; })(1, 2), (function () { return arguments; })(1, 2));
	add("args vs array", (function () { return arguments; })(1, 2), [1, 2]);
	// JSON-like settings (the bundle's real use)
	const settings = () => JSON.parse('{"permissions":{"allow":["Bash(git:*)","Read"],"deny":[]},"env":{"A":"1"},"hooks":{"PreToolUse":[{"matcher":"Bash","hooks":[{"type":"command","command":"x"}]}]},"model":null,"n":1.5}');
	add("settings", settings(), settings());
	add("settings sc", settings(), structuredClone(settings()));
	const s2 = settings(); s2.permissions.allow.push("Write");
	add("settings diff", settings(), s2);
	const s3 = settings(); delete s3.env;
	add("settings delete", settings(), s3);
	const s4 = settings(); s4.extra = undefined;
	add("settings undefined", settings(), s4);
	// deleted-key object (dictionary mode in JSC)
	const del = { a: 1, b: 2, c: 3 }; delete del.b;
	add("deleted key", del, { a: 1, c: 3 });
	const many = {}; for (let i = 0; i < 200; i++) many["k" + i] = i; for (let i = 0; i < 150; i++) delete many["k" + i];
	const many2 = {}; for (let i = 150; i < 200; i++) many2["k" + i] = i;
	add("many deletes", many, many2);
	return P;
}
