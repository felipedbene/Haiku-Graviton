// Bun.YAML.stringify -- port of Bun bun-v1.4.4 src/runtime/api/YAMLObject.rs
// (stringify, Stringifier, Space, AnchorAlias, string_needs_quotes,
// string_is_number, can_use_prop_name_as_anchor), plus the JSC glue it leans
// on: src/jsc/bindings/bindings.cpp (JSC__JSValue__unwrapBoxedPrimitive,
// Bun__JSArray__getContiguousVector), src/jsc/JSArrayIterator.rs,
// src/jsc/JSPropertyIterator.rs + bindings/JSPropertyIterator.cpp and
// src/jsc/JSType.rs (is_function, is_array).
// src/format/yaml/{printer,mod,scalar}.rs are NOT this: they are the Prettier
// YAML formatter behind `bun format`; YAML.stringify never reaches them.
//
// Strings are walked by UTF-16 code unit, as BunString::char_at does, so lone
// surrogates pass through unchanged. No replacer, no toJSON: Bun has neither.

import { types } from "node:util";

const STACK_OVERFLOW_V8 = "Maximum call stack size exceeded";
const STACK_OVERFLOW_JSC = "Maximum call stack size exceeded.";

// JSValue::is_function is a JSType check (JSFunction | InternalFunction), not
// isCallable: a callable Proxy is ProxyObjectType, so it is walked as an object
// (and prints `{}`), not skipped like a function.
const isFunction = (v) => typeof v === "function" && !types.isProxy(v);
// JSType::is_array is Array | DerivedArray; a Proxy of an array is a
// ProxyObject and goes through the property iterator ("0": ..., "1": ...).
const isArray = (v) => !types.isProxy(v) && Array.isArray(v);
const isObject = (v) => v !== null && (typeof v === "object" || typeof v === "function");
const isSkipped = (v) => v === undefined || typeof v === "symbol" || isFunction(v);

// JSC__JSValue__unwrapBoxedPrimitive. Number and String wrappers go through
// toNumber / toString, which run user valueOf / toString / @@toPrimitive (and
// throw on a BigInt result); Boolean and BigInt wrappers read the internal
// slot. Symbol wrappers stay objects.
function unwrapBoxedPrimitive(v) {
	if (!isObject(v)) return v;
	if (types.isNumberObject(v)) return +v;
	if (types.isStringObject(v)) return String(v);
	if (types.isBooleanObject(v)) return Boolean.prototype.valueOf.call(v);
	if (types.isBigIntObject(v)) return BigInt.prototype.valueOf.call(v);
	return v;
}

// JSValue::is_int32. JSC boxes a double that holds an int32 value as an int32
// when the value is made by jsNumber(double), but arithmetic in the LLInt/JIT
// and Double-shaped arrays keep a boxed double: for those Bun prints +0 where
// this prints 0. Node exposes no such representation, so every +0 is int32
// here (all other int32-valued doubles print identically either way).
const isInt32 = (n) => (n | 0) === n && !(n === 0 && 1 / n < 0);

// Bun__JSPropertyIterator__getNameAndValue: the names are a snapshot of the own
// enumerable string keys (getOwnPropertyNames, DontEnumPropertiesMode::Exclude,
// i.e. Object.keys); each value is then a full [[Get]]-style slot lookup, so a
// name deleted by an earlier getter is skipped entirely (not printed as
// undefined), while one shadowed further up the prototype chain is found there.
// A Proxy anywhere on the chain answers through its get trap, receiver intact.
const ABSENT = Object.freeze({});
function getPropertySlotValue(object, name) {
	for (let o = object; o !== null; o = Reflect.getPrototypeOf(o)) {
		if (types.isProxy(o)) return Reflect.get(o, name, object);
		const d = Reflect.getOwnPropertyDescriptor(o, name);
		if (d === undefined) continue;
		if ("value" in d) return d.value;
		return d.get === undefined ? undefined : d.get.call(object);
	}
	return ABSENT;
}

// Space::init: clamp on the float (min(10, ToIntegerOrInfinity)); NaN, < 1 and
// "" are minified, as is every non-number non-string.
const SPACE_MINIFIED = 0, SPACE_NUMBER = 1, SPACE_STR = 2;
function initSpace(spaceValue) {
	const space = unwrapBoxedPrimitive(spaceValue);
	if (typeof space === "number") {
		if (Number.isNaN(space) || space < 1) return { kind: SPACE_MINIFIED };
		return { kind: SPACE_NUMBER, n: space > 10 ? 10 : Math.trunc(space) };
	}
	if (typeof space === "string") {
		if (space.length === 0) return { kind: SPACE_MINIFIED };
		// newline() repeats space_str.trunc(10): the first 10 code units.
		return { kind: SPACE_STR, s: space.slice(0, 10) };
	}
	return { kind: SPACE_MINIFIED };
}

// AnchorAliasName / ValueOrigin.
const NAME_ROOT = 0, NAME_ITEM = 1, NAME_PROP = 2;
const ORIGIN_ROOT = { kind: NAME_ROOT }, ORIGIN_ITEM = { kind: NAME_ITEM };
const anchorInit = (origin) => ({ anchored: false, used: false, kind: origin.kind, propName: origin.propName, counter: 0 });

class Stringifier {
	constructor(spaceValue) {
		this.out = "";
		this.indent = 0;
		// Columns added by the `- ` prefixes of the enclosing sequence items.
		this.itemOffset = 0;
		// Every object visited by findAnchorsAndAliases, by identity.
		this.knownCollections = new Map();
		this.arrayItemCounter = 0;
		// Always rename anchors named "root" to avoid colliding with the root
		// anchor/alias.
		this.propNames = new Map([["root", 0]]);
		this.space = initSpace(spaceValue);
	}

	findAnchorsAndAliases(value, origin) {
		const unwrapped = unwrapBoxedPrimitive(value);
		if (unwrapped === null) return;
		if (typeof unwrapped === "number") return;
		if (typeof unwrapped === "bigint") throw new Error("YAML.stringify cannot serialize BigInt");
		if (typeof unwrapped === "boolean") return;
		if (typeof unwrapped === "string") return;

		const existing = this.knownCollections.get(unwrapped);
		if (existing !== undefined) {
			// Seen before: this becomes an alias, so the anchor is confirmed
			// used and gets its counter now, in first-reuse order.
			if (existing.used) return;
			existing.used = true;
			switch (existing.kind) {
				case NAME_ROOT:
					break;	// only one possible
				case NAME_ITEM:
					existing.counter = this.arrayItemCounter++;
					break;
				case NAME_PROP: {
					// Unsafe names use generated `value<counter>` anchors, keyed
					// on "value" so the counter is shared with literal "value"
					// properties.
					const key = canUsePropNameAsAnchor(existing.propName) ? existing.propName : "value";
					const prev = this.propNames.get(key);
					const n = prev === undefined ? 0 : prev + 1;
					this.propNames.set(key, n);
					existing.counter = n;
					break;
				}
			}
			return;
		}

		this.knownCollections.set(unwrapped, anchorInit(origin));

		if (isArray(unwrapped)) {
			const len = unwrapped.length;
			for (let i = 0; i < len; i++) {
				const item = unwrapped[i];
				if (isSkipped(item)) continue;
				this.findAnchorsAndAliases(item, ORIGIN_ITEM);
			}
			return;
		}

		const names = Object.keys(unwrapped);
		for (let i = 0; i < names.length; i++) {
			const v = getPropertySlotValue(unwrapped, names[i]);
			if (v === ABSENT) continue;
			if (isSkipped(v)) continue;
			this.findAnchorsAndAliases(v, { kind: NAME_PROP, propName: names[i] });
		}
	}

	stringify(value) {
		this.stringifyUnwrapped(unwrapBoxedPrimitive(value), false);
	}

	// `unwrapped` has been through unwrapBoxedPrimitive. With `afterKey` the
	// value follows a block-style mapping key and writes its own separator
	// after the `:`.
	stringifyUnwrapped(unwrapped, afterKey) {
		if (afterKey && !isObject(unwrapped)) this.out += " ";

		if (unwrapped === null) {
			this.out += "null";
			return;
		}

		if (typeof unwrapped === "number") {
			if (isInt32(unwrapped)) {
				this.out += String(unwrapped);
				return;
			}
			const num = unwrapped;
			if (num === -Infinity) this.out += "-.inf";
			else if (num === Infinity) this.out += ".inf";
			else if (Number.isNaN(num)) this.out += ".nan";
			else if (num === 0) this.out += "-0";	// +0 is int32 above
			// WTF::StringBuilder::append(double) is ECMAScript Number::toString.
			else this.out += String(num);
			return;
		}

		if (typeof unwrapped === "bigint") throw new Error("YAML.stringify cannot serialize BigInt");

		if (typeof unwrapped === "boolean") {
			this.out += unwrapped ? "true" : "false";
			return;
		}

		if (typeof unwrapped === "string") {
			this.appendString(unwrapped);
			return;
		}

		const known = this.knownCollections.get(unwrapped);
		const anchor = known !== undefined && known.used ? known : null;

		// An anchor or alias starts on the line after the key.
		if (anchor !== null && afterKey) {
			afterKey = false;
			this.newline();
		}

		if (anchor !== null) {
			this.out += anchor.anchored ? "*" : "&";
			switch (anchor.kind) {
				case NAME_ROOT:
					this.out += "root";
					break;
				case NAME_ITEM:
					this.out += "item" + anchor.counter;
					break;
				case NAME_PROP:
					if (!canUsePropNameAsAnchor(anchor.propName)) {
						this.out += "value" + anchor.counter;
					} else {
						this.out += anchor.propName;
						if (anchor.counter !== 0) this.out += anchor.counter;
					}
					break;
			}
			if (anchor.anchored) return;
			anchor.anchored = true;
			if (this.space.kind === SPACE_MINIFIED) this.out += " ";
			else this.newline();
		}

		if (isArray(unwrapped)) {
			// JSArrayIterator: the length is read once; a hole reads as
			// undefined (the fast path) or through [[Get]] (the slow path),
			// which agree unless the prototype has indexed properties, and
			// then JSC leaves the fast path anyway.
			const len = unwrapped.length;

			if (len === 0) {
				if (afterKey) this.out += " ";
				this.out += "[]";
				return;
			}

			if (this.space.kind === SPACE_MINIFIED) {
				this.out += "[";
				let first = true;
				for (let i = 0; i < len; i++) {
					const item = unwrapped[i];
					if (isSkipped(item)) continue;
					if (!first) this.out += ",";
					first = false;
					this.stringify(item);
				}
				this.out += "]";
			} else {
				let first = true;
				for (let i = 0; i < len; i++) {
					const item = unwrapped[i];
					if (isSkipped(item)) continue;
					if (!first || afterKey) this.newline();
					first = false;
					this.out += "- ";
					// No newline is needed here for any value.
					this.itemOffset += 2;
					this.stringify(item);
					this.itemOffset -= 2;
				}
				if (first) {
					if (afterKey) this.out += " ";
					this.out += "[]";
				}
			}
			return;
		}

		const names = Object.keys(unwrapped);

		if (names.length === 0) {
			if (afterKey) this.out += " ";
			this.out += "{}";
			return;
		}

		if (this.space.kind === SPACE_MINIFIED) {
			this.out += "{";
			let first = true;
			for (let i = 0; i < names.length; i++) {
				const value = getPropertySlotValue(unwrapped, names[i]);
				if (value === ABSENT) continue;
				if (isSkipped(value)) continue;
				if (!first) this.out += ",";
				first = false;
				this.appendString(names[i]);
				this.out += ": ";
				this.stringify(value);
			}
			this.out += "}";
		} else {
			let first = true;
			for (let i = 0; i < names.length; i++) {
				const value = getPropertySlotValue(unwrapped, names[i]);
				if (value === ABSENT) continue;
				if (isSkipped(value)) continue;
				if (!first || afterKey) this.newline();
				first = false;
				this.appendString(names[i]);
				this.out += ":";
				this.indent += 1;
				this.stringifyUnwrapped(unwrapBoxedPrimitive(value), true);
				this.indent -= 1;
			}
			if (first) {
				if (afterKey) this.out += " ";
				this.out += "{}";
			}
		}
	}

	newline() {
		switch (this.space.kind) {
			case SPACE_MINIFIED:
				return;
			case SPACE_NUMBER:
				this.out += "\n" + " ".repeat(this.indent * this.space.n);
				break;
			case SPACE_STR:
				this.out += "\n" + this.space.s.repeat(this.indent);
				break;
		}
		this.out += " ".repeat(this.itemOffset);
	}

	appendDoubleQuotedString(str) {
		let s = "\"";
		for (let i = 0; i < str.length; i++) {
			const c = str.charCodeAt(i);
			const esc = c < 0x80 ? ASCII_ESCAPES[c] : c === 0x85 ? "\\N" : c === 0xa0 ? "\\_" : c === 0x2028 ? "\\L" : c === 0x2029 ? "\\P" : undefined;
			s += esc === undefined ? str[i] : esc;
		}
		this.out += s + "\"";
	}

	appendString(str) {
		if (stringNeedsQuotes(str)) this.appendDoubleQuotedString(str);
		else this.out += str;
	}
}

// append_double_quoted_string's table for code units below 0x80; undefined
// means the unit is copied as is.
const ASCII_ESCAPES = (() => {
	const t = new Array(0x80).fill(undefined);
	const hex = (c) => "\\x" + c.toString(16).padStart(2, "0");
	for (let c = 0; c < 0x20; c++) t[c] = hex(c);
	t[0x00] = "\\0";
	t[0x07] = "\\a";	// bell
	t[0x08] = "\\b";	// backspace
	t[0x09] = "\\t";
	t[0x0a] = "\\n";
	t[0x0b] = "\\v";
	t[0x0c] = "\\f";
	t[0x0d] = "\\r";
	t[0x1b] = "\\e";	// escape
	t[0x22] = "\\\"";
	t[0x5c] = "\\\\";
	t[0x7f] = "\\x7f";	// delete
	return t;
})();

// Can this property name be emitted verbatim as an anchor/alias name? Anchor
// names can't be quoted or escaped, so only unambiguously safe characters are
// reused; anything else falls back to a generated `value<counter>` name.
function canUsePropNameAsAnchor(str) {
	if (str.length === 0) return false;
	for (let i = 0; i < str.length; i++) {
		const c = str.charCodeAt(i);
		const ok = (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a)
			|| c === 0x2d || c === 0x2e || c === 0x5f;
		if (!ok) return false;
	}
	return !matchesGeneratedAnchorName(str);
}

// `value0`, `item12`, `root1`, ...: names that could duplicate a generated
// anchor name. The bare prefix itself does not match.
function matchesGeneratedAnchorName(str) {
	nextPrefix: for (const prefix of ["value", "item", "root"]) {
		if (str.length <= prefix.length) continue;
		for (let i = 0; i < prefix.length; i++) {
			if (str.charCodeAt(i) !== prefix.charCodeAt(i)) continue nextPrefix;
		}
		for (let i = prefix.length; i < str.length; i++) {
			const c = str.charCodeAt(i);
			if (!(c >= 0x30 && c <= 0x39)) continue nextPrefix;
		}
		return true;
	}
	return false;
}

// Exact, case-sensitive (BunString::eq_ascii): "tRuE" stays plain.
const KEYWORDS = new Set([
	"true", "True", "TRUE", "false", "False", "FALSE", "yes", "Yes", "YES", "no",
	"No", "NO", "on", "On", "ON", "off", "Off", "OFF", "n", "N", "y", "Y", "null",
	"Null", "NULL", "~", ".inf", ".Inf", ".INF", ".nan", ".NaN", ".NAN",
]);

function stringNeedsQuotes(str) {
	const len = str.length;
	if (len === 0) return true;

	switch (str.charCodeAt(len - 1)) {
		// whitespace characters
		case 0x20: case 0x09: case 0x0a: case 0x0d:
		// a trailing colon can be misinterpreted as a mapping indicator
		// (oven-sh/bun#25439)
		case 0x3a:
			return true;
	}

	switch (str.charCodeAt(0)) {
		// starting with an indicator character requires quotes
		case 0x26: case 0x2a: case 0x3f: case 0x7c: case 0x2d: case 0x3c: case 0x3e:
		case 0x21: case 0x25: case 0x40: case 0x3a: case 0x2c: case 0x5b: case 0x5d:
		case 0x7b: case 0x7d: case 0x23: case 0x27: case 0x22: case 0x60:
		// starting with whitespace requires quotes
		case 0x20: case 0x09: case 0x0a: case 0x0d:
		// the parser drops a leading byte order mark
		case 0xfeff:
			return true;
	}

	if (KEYWORDS.has(str)) return true;

	let i = 0;
	while (i < len) {
		const c = str.charCodeAt(i);
		switch (c) {
			// flow indicators always need quotes
			case 0x7b: case 0x7d: case 0x5b: case 0x5d: case 0x2c:
				return true;

			case 0x3a:	// ':'
				if (i + 1 < len) {
					switch (str.charCodeAt(i + 1)) {
						case 0x20: case 0x09: case 0x0a: case 0x0d:
							return true;
					}
				}
				i += 1;
				break;

			case 0x23: case 0x60: case 0x27:	// '#', '`', '\''
				return true;

			case 0x2d:	// '-': a `---` document marker
			case 0x2e:	// '.': a `...` document marker
				if (i + 2 < len && str.charCodeAt(i + 1) === c && str.charCodeAt(i + 2) === c) {
					if (i + 3 >= len) return true;
					switch (str.charCodeAt(i + 3)) {
						case 0x20: case 0x09: case 0x0d: case 0x0a:
						case 0x5b: case 0x5d: case 0x7b: case 0x7d: case 0x2c:
							return true;
					}
				}
				if (i === 0 && stringIsNumber(str)) return true;
				i += 1;
				break;

			case 0x30: case 0x31: case 0x32: case 0x33: case 0x34:
			case 0x35: case 0x36: case 0x37: case 0x38: case 0x39:
			// A leading '+' followed by digits/dot parses as a positive number.
			case 0x2b:
				if (i === 0 && stringIsNumber(str)) return true;
				i += 1;
				break;

			default:
				if (c <= 0x1f || c === 0x22 || c === 0x7f || c === 0x85 || c === 0xa0 || c === 0x2028 || c === 0x2029) return true;
				i += 1;
		}
	}
	return false;
}

// True when `str` would be parsed back as a number by YAML.parse (mirrors the
// parser's try_resolve_number / is_core_schema_number, erring towards quoting).
const BASE_DEC = 0, BASE_HEX = 1, BASE_OCT = 2;
function stringIsNumber(str) {
	const len = str.length;
	if (len === 0) return false;

	let i = 0;

	// Optional leading sign.
	const first = str.charCodeAt(0);
	if (first === 0x2b || first === 0x2d) {
		i = 1;
		if (i >= len) return false;	// bare "+" / "-" isn't a number
		// Signed special floats: "+.inf", "-.Inf", ... (".nan" can't be signed).
		if (str.charCodeAt(i) === 0x2e && isInfSuffix(str, i)) return true;
	}

	// Hex / octal base prefix.
	let base = BASE_DEC;
	if (i + 1 < len && str.charCodeAt(i) === 0x30) {
		switch (str.charCodeAt(i + 1)) {
			case 0x78: case 0x58:	// 'x' | 'X'
				base = BASE_HEX;
				i += 2;
				if (i >= len) return false;	// "0x" alone isn't hex
				break;
			case 0x6f: case 0x4f:	// 'o' | 'O'
				base = BASE_OCT;
				i += 2;
				if (i >= len) return false;	// "0o" alone isn't octal
				break;
		}
	}

	// Scan the rest, with the minimal state the parser uses to decide validity.
	let sawDot = false, sawExp = false, sawMinusAfterSign = false;
	for (; i < len; i++) {
		const c = str.charCodeAt(i);
		if (c >= 0x30 && c <= 0x39) continue;
		if ((c >= 0x61 && c <= 0x64) || c === 0x66 || (c >= 0x41 && c <= 0x44) || c === 0x46) {
			// hex digits are only valid in hex
			if (base !== BASE_HEX) return false;
			continue;
		}
		switch (c) {
			case 0x65: case 0x45:	// 'e' | 'E'; in hex just a digit
				if (base === BASE_DEC) {
					if (sawExp) return false;
					sawExp = true;
				}
				break;
			case 0x2e:
				if (sawDot || base !== BASE_DEC) return false;
				sawDot = true;
				break;
			case 0x2b:	// accepted unless in hex
				if (base === BASE_HEX) return false;
				break;
			case 0x2d:	// at most one '-' after the leading sign
				if (sawMinusAfterSign) return false;
				sawMinusAfterSign = true;
				break;
			default:
				return false;
		}
	}
	return true;
}

// The three units after the `.` at `i` spell "inf", "Inf" or "INF".
// Over-matches "+.infX", which is harmless for the quoting decision.
function isInfSuffix(str, i) {
	if (i + 4 > str.length) return false;
	const t = str.slice(i + 1, i + 4);
	return t === "inf" || t === "Inf" || t === "INF";
}

export function stringify(value, replacer, space) {
	if (value === undefined || typeof value === "symbol" || isFunction(value)) return undefined;

	if (replacer !== undefined && replacer !== null) {
		throw new Error("YAML.stringify does not support the replacer argument");
	}

	try {
		const stringifier = new Stringifier(space);
		stringifier.findAnchorsAndAliases(value, ORIGIN_ROOT);
		stringifier.stringify(value);
		return stringifier.out;
	} catch (e) {
		// Bun's StackCheck throws JSC's stack-overflow RangeError (with its
		// trailing period); here the same condition surfaces as V8's. The depth
		// at which each runtime gives up differs (see the test notes).
		if (e instanceof RangeError && e.message === STACK_OVERFLOW_V8) throw new RangeError(STACK_OVERFLOW_JSC);
		throw e;
	}
}
