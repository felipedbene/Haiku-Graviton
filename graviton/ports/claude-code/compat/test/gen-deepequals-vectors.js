import { corpus } from "./deepequals-corpus.js";
const out = corpus().map(([n, a, b]) => {
	const r = (f) => { try { return f(); } catch (e) { return "THROW " + e.message; } };
	return [n, r(() => Bun.deepEquals(a, b)), r(() => Bun.deepEquals(a, b, true)), r(() => Bun.deepEquals(a, b, true, true))];
});
await Bun.write(process.argv[2] + "/deepequals.cases.json", JSON.stringify(out));
console.log("deepEquals pairs:", out.length);
