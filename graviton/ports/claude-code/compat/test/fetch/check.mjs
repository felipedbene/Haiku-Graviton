// check.mjs <bun.json> <node.json>: compare scenario results by name.
//
// "refuse: ..." scenarios: Bun's result is reference only; Node must have thrown
// or rejected with the compat layer's "not supported by the Node compat layer".
//
// Everything else must be identical, except two named differences, each printed
// as KNOWN with both values, and each listed in LIMITATIONS.md:
//   * error objects: Bun rejects with TypeError{code: <failure code>}; Node's fetch
//     with TypeError("fetch failed", {cause: {code}}). Compared: both TypeErrors
//     and the code (Bun's own code vs Node's cause.code). One scenario names Bun
//     codes that Node's TLS stack spells differently (CODE_ALIAS).
//   * Connection header on a request that will not reuse its connection: Bun
//     sends none, Node's HTTP client sends "close" (only where CONN_CLOSE says).
import { readFileSync } from "node:fs";
const [a, b] = process.argv.slice(2).map((f) => new Map(JSON.parse(readFileSync(f, "utf8"))));
const CODE_ALIAS = {
	"mTLS: client cert accepted, required": { UND_ERR_SOCKET: "EPROTO", ERR_OSSL_BAD_DECRYPT: "FailedToOpenSocket" },
};
const CONN_CLOSE = new Set([
	"keepalive:false: a connection per request", "FetchSession keepAlive:false",
	"tls serverName + host header + checkServerIdentity (gateway pattern)", "tls servername (lowercase) is accepted",
	"tls checkServerIdentity returning undefined skips the name check",
]);
const known = [];
const isErr = (x) => x && typeof x === "object" && x.isError === true && typeof x.name === "string";
function norm(k, x, y, path) {
	if (isErr(x) && isErr(y) && x.name === "TypeError" && y.name === "TypeError" && "code" in x && !("cause" in x)
		&& y.message === "fetch failed" && y.cause && !("code" in y)) {
		const alias = (CODE_ALIAS[k] || {})[y.cause.code];
		known.push(`${path}: bun ${x.code} "${x.message}" | node cause ${y.cause.code} "${y.cause.message}"${alias ? " (alias)" : ""}`);
		return [{ name: "TypeError", code: x.code }, { name: "TypeError", code: alias || y.cause.code }];
	}
	if (x === undefined && y === "close" && CONN_CLOSE.has(k)) {
		known.push(`${path}: Connection header bun (none) | node close`);
		return ["close", "close"];
	}
	if (x && y && typeof x === "object" && typeof y === "object" && Array.isArray(x) === Array.isArray(y)) {
		const ox = Array.isArray(x) ? [] : {}, oy = Array.isArray(y) ? [] : {};
		for (const key of new Set([...Object.keys(x), ...Object.keys(y)])) {
			const [p, q] = norm(k, x[key], y[key], path + "." + key);
			if (key in x || p !== undefined) ox[key] = p;
			if (key in y || q !== undefined) oy[key] = q;
		}
		return [ox, oy];
	}
	return [x, y];
}
// Prints each differing leaf as "path: bun | node".
function leaves(x, y, path) {
	if (JSON.stringify(x) === JSON.stringify(y)) return;
	if (x && y && typeof x === "object" && typeof y === "object" && Array.isArray(x) === Array.isArray(y)) {
		for (const k of new Set([...Object.keys(x), ...Object.keys(y)])) leaves(x[k], y[k], path + "." + k);
		return;
	}
	console.log(`  ${path || "."}: bun ${JSON.stringify(x)} | node ${JSON.stringify(y)}`);
}
let same = 0, diff = 0, refused = 0, withKnown = 0;
for (const [k, va] of a) {
	const vb = b.get(k);
	if (k.startsWith("refuse: ")) {
		const s = JSON.stringify(vb) || "";
		if (/not supported by the Node compat layer/.test(s)) { refused++; console.log("REFUSED", k); }
		else { diff++; console.log("DIFF", k, "\n  bun: ", JSON.stringify(va), "\n  node:", s); }
		continue;
	}
	if (JSON.stringify(va) === JSON.stringify(vb)) { same++; console.log("SAME", k); continue; }
	const before = known.length;
	const [na, nb] = norm(k, va, vb, "");
	if (JSON.stringify(na) === JSON.stringify(nb)) {
		withKnown++;
		console.log("SAME*", k);
		for (const l of known.slice(before)) console.log("  KNOWN", l);
	} else { known.length = before; diff++; console.log("DIFF", k); leaves(va, vb, ""); }
}
for (const k of b.keys()) if (!a.has(k)) { diff++; console.log("EXTRA", k); }
console.log(`fetch: ${same} identical + ${withKnown} identical but for named differences / ${refused} refused as required / ${diff} mismatch (${a.size} scenarios)`);
process.exit(diff ? 1 : 0);
