// Run under Node: compare compat/lib/toml.mjs against gen-toml-vectors.js output.
import { readFileSync } from "node:fs";
import { Buffer } from "node:buffer";
import { parse, parseWith, dateTimeString } from "../lib/toml.mjs";
import { coerceCases, depthCases, encode, encodeFlat, encodeError, stringifyDateTimes } from "./toml-corpus.js";

const V = JSON.parse(readFileSync(process.argv[2] + "/toml.cases.json", "utf8"));

// parseWith hands date/times to this hook; the marker carries Bun's type name
// and the string our parse() returns, so one comparison checks both.
class DT { constructor(kind, s) { this.kind = kind; this.s = s; } toString() { return this.s; } }
const dtName = (v) => (v instanceof DT ? v.kind : null);
const hooked = (x) => parseWith(x, (text, kind) => new DT(kind, dateTimeString(text, kind)));
const run = (f, enc = encode) => { try { return { v: enc(f(), dtName) }; } catch (e) { return { e: encodeError(e) }; } };
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const hasDT = (r) => JSON.stringify(r).includes('"T\\":') || JSON.stringify(r).includes('"T":');
const strMode = (r) => (r.v !== undefined && typeof r.v === "object" ? { v: stringifyDateTimes(r.v) } : r);

// Behaviours that cannot match on Node, each listed with why. A listed case
// that stops differing is reported too, so this list cannot go stale silently.
const KNOWN = {
	"coerce:blob": "Bun reads a Blob synchronously; Node cannot, so the port throws a TypeError instead",
};
// Bun runs out of native stack (RangeError "Maximum call stack size
// exceeded.") on very deep documents: in parse_value's recursion for nested
// values, in expr_to_js's for nested tables. Where depends on the native stack,
// not on a fixed limit. The port's parse has no recursion and accepts them.
const DEEP = "Bun overflows its native stack at this depth; the port has no recursion and parses it";
for (const n of ["inline-20000", "array-30000", "inline-30000", "array-50000", "inline-50000", "array-100000", "inline-100000", "header-100000", "dotted-100000"])
	KNOWN["depth:" + n] = DEEP;

const coerceInput = new Map(coerceCases);
const depthInput = new Map(depthCases);
let bad = 0, known = 0, dtVectors = 0;
for (const kind of ["edge", "bytes", "config", "coerce", "depth", "fuzz", "fuzzshort", "mutate"]) {
	let ok = 0, kBad = 0;
	for (const [inp, exp] of V[kind]) {
		const id = `${kind}:${inp}`;
		let thunk;
		if (kind === "bytes") thunk = (p) => p(Buffer.from(inp, "base64"));
		else if (kind === "coerce") { const mk = coerceInput.get(inp); thunk = (p) => (mk === null ? p() : p(mk())); }
		else if (kind === "depth") thunk = (p) => p(depthInput.get(inp));
		else thunk = (p) => p(inp);
		const enc = kind === "depth" ? encodeFlat : encode;
		const got = run(() => thunk(hooked), enc);
		const gotStr = run(() => thunk(parse), enc);
		const match = same(got, exp) && same(gotStr, strMode(exp));
		if (hasDT(exp)) dtVectors++;
		const label = kind === "coerce" || kind === "depth" ? id : `${kind}:${JSON.stringify(inp).slice(0, 160)}`;
		if (KNOWN[id] !== undefined) {
			if (match) { bad++; kBad++; console.log("KNOWN-DIFFERENCE NOW MATCHES", label); }
			else { known++; console.log("KNOWN", label, "--", KNOWN[id], "\n  got", JSON.stringify(got).slice(0, 200), "\n  exp", JSON.stringify(exp).slice(0, 200)); }
			continue;
		}
		if (match) ok++;
		else { bad++; kBad++; if (kBad <= 15) console.log("MISMATCH", label, "\n  got", JSON.stringify(got).slice(0, 400), "\n  str", JSON.stringify(gotStr).slice(0, 200), "\n  exp", JSON.stringify(exp).slice(0, 400)); }
	}
	console.log(`toml.${kind}: ${ok} match / ${kBad} mismatch`);
}
console.log(`toml: ${dtVectors} vectors contain date/times (Temporal objects in Bun; strings equal to their String() from parse(), type-checked through parseWith)`);
console.log(`toml: ${known} known difference(s), listed above`);
process.exit(bad ? 1 : 0);
