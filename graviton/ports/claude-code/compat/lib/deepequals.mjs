// Bun.deepEquals(a, b, strict?, skipPrototype?) -- port of Bun bun-v1.4.4
// src/jsc/bindings/bindings.cpp Bun__deepEquals<isStrict, false, false, skip>
// (no asymmetric matchers, no node prototype checks) with specialObjectsDequal /
// specialObjectsDequalSlow. JSC cell types are recovered with node:util/types.
//
// JSC picks a structure "fast path" for ordinary objects whose structure has no
// indexed properties, no accessors, no own __proto__ and is not an uncacheable
// dictionary; it compares own enumerable keys only. Otherwise the slow path
// walks getPropertyNames(), which includes enumerable keys inherited from the
// prototype chain. The uncacheable-dictionary state is invisible to JS, so an
// object that JSC demoted to a dictionary (heavy key deletion) is compared with
// the fast path here -- the two differ only when its prototype chain carries
// enumerable properties. Recorded in LIMITATIONS.md.
import { types } from "node:util";

const EMPTY = Symbol("empty");
const has = Object.prototype.hasOwnProperty;
const isIndexKey = (k) => typeof k === "string" && k !== "" && String(k >>> 0) === k && k !== "4294967295";
const TYPED = ["Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
	"Float16Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"];
const FLOAT = new Set(["Float16Array", "Float32Array", "Float64Array"]);
const tagOf = (o) => Object.prototype.toString.call(o).slice(8, -1);

function cellType(o) {
	if (typeof o === "function") return "Function";
	if (types.isProxy(o)) return "Proxy";
	if (Array.isArray(o)) return "Array";
	if (types.isTypedArray(o)) {
		for (const t of TYPED) if (globalThis[t] && o instanceof globalThis[t]) return t;
		const t = Object.getPrototypeOf(Object.getPrototypeOf(o))?.constructor && tagOf(o);
		return TYPED.includes(t) ? t : "TypedArray";
	}
	if (types.isAnyArrayBuffer(o)) return "ArrayBuffer";
	if (types.isDataView(o)) return "DataView";
	if (types.isDate(o)) return "Date";
	if (types.isRegExp(o)) return "RegExp";
	if (types.isNativeError(o)) return "Error";
	if (types.isMap(o)) return "Map";
	if (types.isSet(o)) return "Set";
	if (types.isStringObject(o)) return "StringObject";
	if (types.isNumberObject(o)) return "NumberObject";
	if (types.isBooleanObject(o)) return "BooleanObject";
	if (types.isSymbolObject(o)) return "SymbolObject";
	if (types.isBigIntObject(o)) return "BigIntObject";
	if (o === globalThis) return "Global";
	return "Object";
}

// JSObject::calculatedClassName: the constructor's name, else the toString tag.
function className(o) {
	try {
		const c = o.constructor;
		if (typeof c === "function" && typeof c.name === "string" && c.name) return c.name;
	} catch {}
	return tagOf(o);
}

function ownEnumerableKeys(o) {
	return Reflect.ownKeys(o).filter((k) => Object.getOwnPropertyDescriptor(o, k)?.enumerable);
}

// getPropertyNames(DontEnum excluded): own enumerable keys, then enumerable keys
// up the prototype chain not already seen (for-in order, symbols included).
function propertyNames(o) {
	const out = [], seen = new Set();
	for (let p = o; p !== null; p = Object.getPrototypeOf(p)) {
		for (const k of Reflect.ownKeys(p)) {
			if (seen.has(k)) continue;
			seen.add(k);
			const d = Object.getOwnPropertyDescriptor(p, k);
			if (d && d.enumerable) out.push(k);
		}
	}
	return out;
}

const getIfExists = (o, k) => (k in o ? o[k] : EMPTY);

function canFastEnumerate(o) {
	if (cellType(o) !== "Object") return false;
	for (const k of Reflect.ownKeys(o)) {
		if (isIndexKey(k)) return false;
		if (k === "__proto__") return false;
		const d = Object.getOwnPropertyDescriptor(o, k);
		if (d && (d.get || d.set)) return false;
	}
	return true;
}

// getIndexWithoutAccessors: an own data element's value, else empty.
function indexWithoutAccessors(o, i) {
	const d = Object.getOwnPropertyDescriptor(o, String(i));
	return d && !d.get && !d.set ? d.value : EMPTY;
}

function make(isStrict, skipProto) {
	const deep = (v1, v2, stack, addToStack) => {
		if (v1 !== EMPTY && v2 !== EMPTY && Object.is(v1, v2)) return true;
		if (v1 === EMPTY || v2 === EMPTY) return (v1 === EMPTY) === (v2 === EMPTY);
		const isObj = (x) => x !== null && (typeof x === "object" || typeof x === "function");
		if (!isObj(v1) || !isObj(v2)) return false;
		for (const [a, b] of stack) {
			const f = a === v1, s = b === v2;
			if (f) return s;
			if (s) return false;
		}
		if (addToStack) stack.push([v1, v2]);
		try {
			let r = special(v1, v2, stack);
			if (r !== undefined) return r;
			r = special(v2, v1, stack);
			if (r !== undefined) return r;
			return objectsEqual(v1, v2, stack);
		} finally {
			if (addToStack) stack.pop();
		}
	};

	const special = (c1, c2, stack) => {
		const t1 = cellType(c1), t2 = cellType(c2);
		switch (t1) {
		case "Set": {
			if (t2 !== "Set") return false;
			if (c1.size !== c2.size) return false;
			for (const k1 of c1) {
				if (c2.has(k1)) continue;
				let found = false;
				for (const k2 of c2) if (deep(k1, k2, stack, false)) { found = true; break; }
				if (!found) return false;
			}
			return true;
		}
		case "Map": {
			if (t2 !== "Map") return false;
			if (c1.size !== c2.size) return false;
			for (const [k1, v1] of c1) {
				let v2 = c2.get(k1);
				if (v2 === undefined) {
					let found = false;
					for (const [k2, vv] of c2) { v2 = vv; if (deep(k1, k2, stack, false)) { found = true; break; } }
					if (!found) return false;
				}
				if (!deep(v1, v2, stack, false)) return false;
			}
			return true;
		}
		case "ArrayBuffer": {
			if (t2 !== "ArrayBuffer") return false;
			if (c1.byteLength !== c2.byteLength) return false;
			if (types.isSharedArrayBuffer(c1) !== types.isSharedArrayBuffer(c2)) return false;
			if (c1.detached || c2.detached) throw new TypeError("Cannot perform Construct on a detached ArrayBuffer");
			const a = new Uint8Array(c1), b = new Uint8Array(c2);
			for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
			return true;
		}
		case "DataView":
			return undefined;		// compared as a plain object (checkPrototypes off)
		case "Date":
			if (t2 !== "Date") return false;
			return c1.getTime() === c2.getTime();	// NaN != NaN, like internalNumber ==
		case "RegExp":
			if (t2 !== "RegExp") return false;
			return c1.source === c2.source && c1.flags === c2.flags;	// RegExpKey (flags, pattern)
		case "Error": {
			if (t2 !== "Error") return false;
			if (errorType(c1) !== errorType(c2)) return false;	// ErrorType enum only (not subclasses)
			if (String(c1.name) !== String(c2.name)) return false;
			if (String(c1.message) !== String(c2.message)) return false;
			if (isStrict) {
				if (("cause" in c1) !== ("cause" in c2)) return false;
			}
			if (!deep(c1.cause, c2.cause, stack, true)) return false;
			const a1 = propertyNames(c1), a2 = propertyNames(c2);
			if (isStrict && a1.length !== a2.length) return false;
			let i;
			for (i = 0; i < a1.length; i++) {
				const k = a1[i];
				if (k === "stack") continue;
				const p1 = c1[k], p2 = getIfExists(c2, k);
				if (!isStrict && p1 === undefined && p2 === EMPTY) continue;
				if (p2 === EMPTY) return false;
				if (!deep(p1, p2, stack, true)) return false;
			}
			for (; i < a2.length; i++) {
				const k = a2[i];
				if (k === "stack") continue;
				if (getIfExists(c2, k) !== undefined) return false;
			}
			return true;
		}
		case "StringObject": {
			if (t2 !== "StringObject") return false;
			if (!skipProto && className(c1) !== className(c2)) return false;
			if (String(c1) !== String(c2)) return false;
			if (isStrict && (extraOwn(c1, String(c1).length) || extraOwn(c2, String(c2).length))) return undefined;
			return true;
		}
		case "Function":
			return false;
		case "Global":
			return t1 === t2;
		case "NumberObject":
		case "BooleanObject":
			if (t1 !== t2) return false;
			if (!Object.is(c1.valueOf(), c2.valueOf())) return false;
			return undefined;
		default:
			if (TYPED.includes(t1)) {
				if (!TYPED.includes(t2) || t1 !== t2) return false;
				const extra = isStrict && (extraOwn(c1, c1.length) || extraOwn(c2, c2.length));
				if (c1.byteLength !== c2.byteLength) return false;
				if (c1.byteLength === 0) return extra ? undefined : true;
				if (c1.buffer === c2.buffer && c1.byteOffset === c2.byteOffset) return extra ? undefined : true;
				let eq = true;
				if (!isStrict && FLOAT.has(t1)) {
					for (let i = 0; i < c1.length; i++) if (c1[i] !== c2[i]) { eq = false; break; }
				} else {
					const a = new Uint8Array(c1.buffer, c1.byteOffset, c1.byteLength);
					const b = new Uint8Array(c2.buffer, c2.byteOffset, c2.byteLength);
					for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) { eq = false; break; }
				}
				if (!eq) return false;
				return extra ? undefined : true;
			}
		}
		if (t1 === "SymbolObject" || t1 === "BigIntObject") {
			if (t1 !== t2) return false;
			if (!Object.is(c1.valueOf(), c2.valueOf())) return false;
		}
		return undefined;
	};

	// hasExtraOwnProperties: named properties, or indexed storage beyond the contents.
	const extraOwn = (o, len) => Reflect.ownKeys(o).some((k) => !(isIndexKey(k) && Number(k) < len));
	const errorType = (e) => {
		for (const C of [EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError, AggregateError])
			if (e instanceof C) return C.name;
		return "Error";
	};

	const objectsEqual = (o1, o2, stack) => {
		const a1Arr = Array.isArray(o1), a2Arr = Array.isArray(o2);
		if (a1Arr !== a2Arr) return false;
		if (a1Arr && a2Arr && !(types.isProxy(o1) || types.isProxy(o2))) {
			const l1 = o1.length, l2 = o2.length;
			if (isStrict && l1 !== l2) return false;
			let i = 0;
			for (; i < l1; i++) {
				const left = indexWithoutAccessors(o1, i), right = indexWithoutAccessors(o2, i);
				if (isStrict) {
					if (left === EMPTY && right === EMPTY) continue;
					if (left === EMPTY || right === EMPTY) return false;
				} else if ((left === EMPTY || right === EMPTY) && (left === undefined || right === undefined)) continue;
				if (!deep(left, right, stack, true)) return false;
			}
			for (; i < l2; i++) {
				const right = indexWithoutAccessors(o2, i);
				if (right === EMPTY || right === undefined) continue;
				return false;
			}
			const s1 = Reflect.ownKeys(o1).filter((k) => typeof k === "symbol" && Object.getOwnPropertyDescriptor(o1, k).enumerable);
			const s2 = Reflect.ownKeys(o2).filter((k) => typeof k === "symbol" && Object.getOwnPropertyDescriptor(o2, k).enumerable);
			if (isStrict && s1.length !== s2.length) return false;
			for (const k of s1) {
				const p1 = o1[k], p2 = getIfExists(o2, k);
				if (!isStrict && p1 === undefined && p2 === EMPTY) continue;
				if (p2 === EMPTY) return false;
				if (!deep(p1, p2, stack, true)) return false;
			}
			return true;
		}
		if (isStrict && !skipProto && className(o1) !== className(o2)) return false;
		if (canFastEnumerate(o1) && canFastEnumerate(o2)) {
			const k1 = ownEnumerableKeys(o1);
			const pairs = [];
			for (const k of k1) {
				const left = o1[k];
				let right = EMPTY;
				if (isStrict) {
					const d = Object.getOwnPropertyDescriptor(o2, k);
					if (d && d.enumerable) right = d.value;
				} else if (has.call(o2, k)) right = Object.getOwnPropertyDescriptor(o2, k).value;
				if (!isStrict && left === undefined && right === EMPTY) continue;
				if (right === EMPTY) return false;
				pairs.push([left, right]);
			}
			let remain = k1.length;
			for (const k of ownEnumerableKeys(o2)) {
				if (!isStrict && o2[k] === undefined) continue;
				if (!has.call(o1, k)) return false;
				if (remain === 0) return false;
				remain--;
			}
			for (const [l, r] of pairs) {
				if (l === r || Object.is(l, r)) continue;
				if (!deep(l, r, stack, true)) return false;
			}
			return true;
		}
		const a1 = propertyNames(o1), a2 = propertyNames(o2);
		if (isStrict && a1.length !== a2.length) return false;
		let i;
		for (i = 0; i < a1.length; i++) {
			const k = a1[i];
			const p1 = o1[k], p2 = getIfExists(o2, k);
			if (!isStrict && p1 === undefined && p2 === EMPTY) continue;
			if (p2 === EMPTY) return false;
			if (!deep(p1, p2, stack, true)) return false;
		}
		for (; i < a2.length; i++) if (getIfExists(o2, a2[i]) !== undefined) return false;
		return true;
	};
	return (a, b) => deep(a, b, [], true);
}

const LOOSE = make(false, false), STRICT = make(true, false), STRICT_SKIP = make(true, true);

export function deepEquals(...args) {
	if (args.length < 2) throw new TypeError("Expected 2 values to compare");
	const [a, b, strict, skipPrototype] = args;
	if (strict === true) return skipPrototype === true ? STRICT_SKIP(a, b) : STRICT(a, b);
	return LOOSE(a, b);
}
