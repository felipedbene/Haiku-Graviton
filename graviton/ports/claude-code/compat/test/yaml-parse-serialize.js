// Canonical text of a Bun.YAML.parse result, shared by the generator (stock
// Bun) and the checker (Node). It is strict on purpose: own key order, -0,
// NaN/Infinity, holes, prototypes, property attributes, and object identity
// (an object met again prints as #<n>, so shared and cyclic aliases must be
// shared and cyclic on both sides). Thrown values print as `!<Ctor>: <message>`.

export function serialize(v) {
	// Iterative (an explicit work stack of values and literal text), so that
	// thousands of nesting levels serialize on Node's default stack too.
	const ids = new Map();
	const out = [];
	const work = [{ v }];
	const prim = (x) => {
		if (x === null) return "null";
		switch (typeof x) {
			case "undefined": return "undef";
			case "boolean": return String(x);
			case "number": return Object.is(x, -0) ? "-0" : String(x);
			case "string": return JSON.stringify(x);
			case "object": return null;
			default: return "?" + typeof x;
		}
	};
	while (work.length) {
		const w = work.pop();
		if (typeof w === "string") { out.push(w); continue; }
		const x = w.v;
		const p = prim(x);
		if (p !== null) { out.push(p); continue; }
		if (ids.has(x)) { out.push("#" + ids.get(x)); continue; }
		const id = ids.size;
		ids.set(x, id);
		const tasks = [];
		if (Array.isArray(x)) {
			if (Object.getPrototypeOf(x) !== Array.prototype) { out.push("!proto-array"); continue; }
			tasks.push("@" + id + "[");
			for (let i = 0; i < x.length; i++) {
				if (i) tasks.push(",");
				tasks.push(i in x ? { v: x[i] } : "hole");
			}
			const extra = Reflect.ownKeys(x).filter((k) => k !== "length" && !(typeof k === "string" && String(k >>> 0) === k && (k >>> 0) < x.length));
			tasks.push("]" + (extra.length ? "+" + JSON.stringify(extra.map(String)) : ""));
		} else {
			const proto = Object.getPrototypeOf(x);
			tasks.push("@" + id + (proto === Object.prototype ? "" : "<proto>") + "{");
			let first = true;
			for (const k of Reflect.ownKeys(x)) {
				const d = Object.getOwnPropertyDescriptor(x, k);
				const attrs = ("value" in d ? "" : "accessor") + (d.writable ? "" : "ro") + (d.enumerable ? "" : "hidden") + (d.configurable ? "" : "fixed");
				tasks.push((first ? "" : ",") + JSON.stringify(String(k)) + (attrs ? "<" + attrs + ">" : "") + ":", { v: d.value });
				first = false;
			}
			tasks.push("}");
		}
		for (let i = tasks.length - 1; i >= 0; i--) work.push(tasks[i]);
	}
	return out.join("");
}

export function outcome(f) {
	try {
		return serialize(f());
	} catch (e) {
		return "!" + (e && e.constructor ? e.constructor.name : typeof e) + ": " + (e && e.message);
	}
}

// Input descriptors in the vectors file: a string is the input itself;
// {b: base64, as} is bytes as Buffer/Uint8Array/ArrayBuffer/DataView; {v} is a
// non-string JS value from the fixed table below.
export const SPECIAL = {
	undefined: undefined,
	null: null,
	num123: 123,
	numFrac: -1.5e-7,
	true: true,
	emptyObject: {},
	toStringObject: { toString() { return "a: 1\nb: [x, y]"; } },
	array: ["a: 1", "b"],
	symbol: Symbol("x"),
	bigint: 12345678901234567890n,
};

export function materialize(d) {
	if (typeof d === "string") return d;
	if ("v" in d) return SPECIAL[d.v];
	const buf = Buffer.from(d.b, "base64");
	switch (d.as) {
		case "Buffer": return buf;
		case "Uint8Array": return new Uint8Array(buf);
		case "ArrayBuffer": return new Uint8Array(buf).buffer;
		case "DataView": { const u = new Uint8Array(buf.length + 3); u.set(buf, 2); return new DataView(u.buffer, 2, buf.length); }
		case "Uint16Array": { const u = new Uint8Array(buf.length + (buf.length & 1)); u.set(buf); return new Uint16Array(u.buffer); }
	}
	throw new Error("bad descriptor");
}
