// Run under STOCK Bun: reference outputs for Bun.semver.order/satisfies, deterministic.
let st = 0x5e3e7e11n;
const rnd = (n) => { st ^= st << 13n; st &= (1n << 64n) - 1n; st ^= st >> 7n; st ^= st << 17n; st &= (1n << 64n) - 1n; return Number(st % BigInt(n)); };
const pick = (a) => a[rnd(a.length)];
const num = () => pick(["0", "1", "2", "3", "10", "01", "99", "314", "18446744073709551615", "18446744073709551616", "9007199254740993"]);
const pre = () => pick(["alpha", "beta", "rc", "0", "1", "alpha.1", "beta.2", "rc.10", "rc.9", "x.y", "0a", "a-b", "", "1.2.3", "canary.67e7966.0"]);
const ver = () => {
	let s = pick(["", "", "v", "=", " ", "v ", "=v"]) + num();
	const parts = rnd(4);
	if (parts > 0) s += "." + pick([num(), num(), "x", "X", "*"]);
	if (parts > 1) s += "." + pick([num(), num(), "x", "*"]);
	if (parts > 2 && rnd(4) === 0) s += "." + num();
	if (rnd(3) === 0) s += "-" + pre();
	if (rnd(5) === 0) s += "+" + pick(["build", "b.1", "001", "x-y"]);
	if (rnd(12) === 0) s += pick(["rc1", "beta", "x", " ", "  ", "|", "^", "#"]);
	return s;
};
const range = () => {
	const one = () => {
		const r = rnd(10);
		if (r < 2) return pick(["^", "~", "~>", ">", ">=", "<", "<=", "=", "", "> ", "< =", "v"]) + ver();
		if (r < 3) return ver() + pick([" - ", "-", " -", "- "]) + ver();
		if (r < 4) return pick(["*", "x", "X", "", "latest", "next", "1.x", "1.2.x", ">x", "<*", "boop", "=>16.0.0", "1.2.3.4", "--foo"]);
		return pick(["^", "~", ">=", "<", ""]) + ver();
	};
	let s = one();
	const n = rnd(4);
	for (let i = 0; i < n; i++) s += pick([" ", "  ", " || ", "||", " | ", " && "]) + one();
	return s;
};
const T = (f) => { try { return { v: f() }; } catch (e) { return { e: e.message }; } };
const out = { order: [], satisfies: [] };
for (let k = 0; k < 40000; k++) { const a = ver(), b = ver(); out.order.push([a, b, T(() => Bun.semver.order(a, b))]); }
for (let k = 0; k < 60000; k++) { const v = rnd(4) ? ver() : pick(["1.2.3", "1.0.0-alpha", "0.0.0", "2.1.314", "1.2.3-beta.1", " 1.2.3 ", "1.2", "v1.2.3"]), r = range(); out.satisfies.push([v, r, T(() => Bun.semver.satisfies(v, r))]); }
for (const [a, b] of [["é", "1.0.0"], ["1.0.0", "1.0.0-"], ["1.0.0-", "1.0.0"]]) out.order.push([a, b, T(() => Bun.semver.order(a, b))]);
for (const [v, r] of [["1.0.0-", "1.0.0"], ["1.0.0", "1.0.0-"], ["é", "*"], ["1.0.0", "é"], ["1.2.3", ""], ["", ""]]) out.satisfies.push([v, r, T(() => Bun.semver.satisfies(v, r))]);
await Bun.write(process.argv[2] + "/semver.cases.json", JSON.stringify(out));
console.log("order:", out.order.length, "satisfies:", out.satisfies.length);
