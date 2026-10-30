// Bun.YAML.parse -- port of Bun bun-v1.4.4 (rev 38afee6c84fc):
//   src/parsers/yaml.rs           Parser<Utf8> (scan, scan_plain_scalar,
//                                 ScalarResolverCtx::try_resolve_number, block
//                                 and flow collections, anchors/aliases, merge
//                                 keys, ParseResultError::add_to_log messages)
//   src/runtime/api/YAMLObject.rs parse / parse_to_values / ParserCtx::to_js
//                                 (Expr -> JS values, PendingProperty)
//   src/runtime/api.rs            with_text_format_source
//   src/bun_core/fmt.rs           parse_unsigned (radix 0), parse_double
//   src/js_parser_jsc/expr_jsc.rs utf8_bytes_to_js, with bun_core
//                                 string/immutable.rs write_wtf8_as_utf16le and
//                                 unicode.rs decode_wtf8_rune_t (scalar loop)
// The parser runs over UTF-8 bytes, held here as a binary string (one char per
// byte), because Bun's Parser<Utf8> classifies bytes, not code points: every
// index, length and comparison below is in bytes, exactly as in Rust. Strings
// are decoded back to UTF-16 only when they become JS values.
// Positions only feed error locations, which the thrown message does not
// contain, so the port keeps them only where the parser itself reads them.

import { Buffer, isUtf8 } from "node:buffer";
import { Worker, isMainThread, workerData, MessageChannel, receiveMessageOnPort } from "node:worker_threads";

const BLOCK_OUT = 0, BLOCK_IN = 1, FLOW_IN = 2, FLOW_KEY = 3;
const STRIP = 0, CLIP = 1, KEEP = 2;
const TAG_NONE = 0, TAG_NONSPECIFIC = 1, TAG_BOOL = 2, TAG_INT = 3, TAG_FLOAT = 4, TAG_NULL = 5, TAG_STR = 6,
	TAG_VERBATIM = 7, TAG_UNKNOWN = 8;
const T_EOF = 0, T_SEQ_ENTRY = 1, T_MAP_KEY = 2, T_MAP_VALUE = 3, T_COLLECT = 4, T_SEQ_START = 5, T_SEQ_END = 6,
	T_MAP_START = 7, T_MAP_END = 8, T_ANCHOR = 9, T_ALIAS = 10, T_TAG = 11, T_DIRECTIVE = 12, T_RESERVED = 13,
	T_DOC_START = 14, T_DOC_END = 15, T_SCALAR = 16;
const S_PLAIN = 0, S_BLOCK = 1, S_QUOTED = 2;
const FC_POSITIVE = 0, FC_NEGATIVE = 1, FC_DOT = 2, FC_OTHER = 3;
const K_SEQ_ENTRY = 0, K_MAP_EXPLICIT_KEY = 1, K_MAP_VALUE = 2;
const ESC_X = 2, ESC_LOWER_U = 4, ESC_UPPER_U = 8;
const BODY_FLOW_SEQUENCE = 0, BODY_FLOW_MAPPING = 1, BODY_BLOCK_SEQUENCE = 2, BODY_BLOCK_MAPPING = 3,
	BODY_EXPLICIT_KEY_MAPPING = 4;

// Expr: the subset of bun_ast the YAML parser builds. Collections are objects
// so that aliases share them by identity, as E::Array/E::Object pointers do.
const E_NULL = 0, E_BOOL = 1, E_NUM = 2, E_STR = 3, E_ARR = 4, E_OBJ = 5;
const NULL_EXPR = Object.freeze({ t: E_NULL });
const boolExpr = (v) => ({ t: E_BOOL, v });
const numExpr = (v) => ({ t: E_NUM, v });
const strExpr = (s) => ({ t: E_STR, s });
const isCollection = (e) => e.t === E_ARR || e.t === E_OBJ;

const MAX_MERGED_PROPERTIES = 1024 * 1024;
const MAX_ALIAS_EXPANSION = 16 * 1024 * 1024;
// Bun's StackCheck (parse_node's `stack_check.remaining()`) measures native
// stack bytes, so how deep a document may nest depends on which construct
// recurses. Measured on stock Bun 1.4.4 (linux-arm64), deepest accepted, stable
// to +-1 between runs: flow sequence 4723, flow mapping 4515, compact block
// sequence 4085, block mapping 3364 levels. Here each parse_node call charges a
// cost for its call site (FRAME_*), fitted so that those four limits come out
// exactly; mixed nestings add up as native frames do. The Rust alias-depth
// accounting (an alias is charged its anchor's stack) runs on these units.
// Node's default stack runs out first, near 1000 levels: such a parse is
// re-run on a worker thread with a stack big enough (parseDeep).
// Errors abort the whole parse (yaml.rs never recovers from one), so the
// context/indent stacks are restored on success paths only, without
// try/finally frames.
const STACK_UNITS = 4722000;
const FRAME_FLOW_SEQUENCE = 1000, FRAME_FLOW_MAPPING = 1046, FRAME_BLOCK_SEQUENCE = 1156, FRAME_BLOCK_MAPPING = 1404;
const DEEP_STACK_MB = 32;

// ParseResultError::add_to_log texts.
const MESSAGES = {
	UnexpectedEof: "Unexpected EOF",
	UnexpectedToken: "Unexpected token",
	UnexpectedCharacter: "Unexpected character",
	TabIndentation: "Tab characters cannot be used as indentation",
	InvalidDirective: "Invalid directive",
	UnresolvedTagHandle: "Unresolved tag handle",
	UnresolvedAlias: "Unresolved alias",
	MultilineImplicitKey: "Multiline implicit key",
	MultipleAnchors: "Multiple anchors",
	MultipleTags: "Multiple tags",
	UnexpectedDocumentStart: "Unexpected document start",
	UnexpectedDocumentEnd: "Unexpected document end",
	MultipleYamlDirectives: "Multiple YAML directives",
	ExcessiveAliasing: "Excessive aliasing",
	CyclicAlias: "Cyclic aliases are only supported by Bun.YAML.parse",
	CyclicMerge: "Merge key cannot reference an enclosing node",
};

// A ParseError value (Rust `Err(ParseError::X)`), unwound with throw.
class ParseError {
	constructor(kind) { this.kind = kind; }
}
const fail = (kind) => { throw new ParseError(kind); };

// chars:: classification over UTF-8 code units.
const isDecDigit = (c) => c >= 0x30 && c <= 0x39;
const isHexDigit = (c) => (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66);
const isWordChar = (c) => (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) || c === 0x2d;
// EncodingKind::Utf8 arm: every byte >= 0x80 counts, below that only printable ASCII.
const isNsChar = (c) => (c >= 0x21 && c <= 0x7e) || (c >= 0x80 && c <= 0xff);
const isFlowIndicator = (c) => c === 0x2c || c === 0x5b || c === 0x5d || c === 0x7b || c === 0x7d;
const isAnchorChar = (c) => isNsChar(c) && !isFlowIndicator(c);
const isWsOrBreak = (c) => c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d;
const hexValue = (c) => (c >= 0x30 && c <= 0x39 ? c - 0x30 : c >= 0x61 && c <= 0x66 ? c - 0x57 : c >= 0x41 && c <= 0x46 ? c - 0x37 : -1);

// bun_core::fmt::parse_unsigned::<u64>(s, 0): optional 0x/0o/0b prefix
// (either case), `_` separators except leading/trailing, u64 range.
function parseUnsignedRadix0(s) {
	let radix = 10, d = s;
	if (s.length >= 2 && s.charCodeAt(0) === 0x30) {
		const c = s.charCodeAt(1);
		if (c === 0x78 || c === 0x58) { d = s.slice(2); radix = 16; }
		else if (c === 0x6f || c === 0x4f) { d = s.slice(2); radix = 8; }
		else if (c === 0x62 || c === 0x42) { d = s.slice(2); radix = 2; }
	}
	if (d.length === 0 || d.charCodeAt(0) === 0x5f || d.charCodeAt(d.length - 1) === 0x5f) return null;
	let acc = 0n;
	const R = BigInt(radix);
	for (let i = 0; i < d.length; i++) {
		const c = d.charCodeAt(i);
		if (c === 0x5f) continue;
		let v;
		if (c >= 0x30 && c <= 0x39) v = c - 0x30;
		else if (c >= 0x61 && c <= 0x7a) v = c - 0x61 + 10;
		else if (c >= 0x41 && c <= 0x5a) v = c - 0x41 + 10;
		else return null;
		if (v >= radix) return null;
		acc = acc * R + BigInt(v);
	}
	if (acc > 0xffffffffffffffffn) return null;
	// `unsigned as f64`: round-to-nearest, as Number(BigInt) does.
	return Number(acc);
}

// [10.2.1.4] core schema number shape over the lexed slice; see the Rust doc.
function isCoreSchemaNumber(s, firstChar) {
	let i = 0;
	const len = s.length;
	let sawLeadingDot = firstChar === FC_DOT;
	if (!sawLeadingDot && i < len && s.charCodeAt(i) === 0x2e) { i++; sawLeadingDot = true; }
	if (sawLeadingDot) {
		if (i >= len || !isDecDigit(s.charCodeAt(i))) return false;
		while (i < len && isDecDigit(s.charCodeAt(i))) i++;
	} else {
		if (i >= len || !isDecDigit(s.charCodeAt(i))) return false;
		while (i < len && isDecDigit(s.charCodeAt(i))) i++;
		if (i < len && s.charCodeAt(i) === 0x2e) {
			i++;
			while (i < len && isDecDigit(s.charCodeAt(i))) i++;
		}
	}
	if (i < len && (s.charCodeAt(i) === 0x65 || s.charCodeAt(i) === 0x45)) {
		i++;
		if (i < len && (s.charCodeAt(i) === 0x2b || s.charCodeAt(i) === 0x2d)) i++;
		if (i >= len || !isDecDigit(s.charCodeAt(i))) return false;
		while (i < len && isDecDigit(s.charCodeAt(i))) i++;
	}
	return i === len;
}

// NodeTag::resolve_null
const resolveNull = (tag) => (tag === TAG_NONSPECIFIC || tag === TAG_STR ? strExpr("") : NULL_EXPR);

// yaml_merge_key_expr_eql: by value for scalars (f64 `==`, so NaN never
// matches and 0 == -0), by identity for collections. The hash only buckets.
function mergeKeyEql(l, r) {
	if (l.t !== r.t) return false;
	switch (l.t) {
		case E_NULL: return true;
		case E_BOOL: return l.v === r.v;
		case E_NUM: return l.v === r.v;
		case E_STR: return l.s === r.s;
		default: return l === r;
	}
}
let collectionIds = new WeakMap(), nextCollectionId = 0;
function mergeKeyHash(e) {
	switch (e.t) {
		case E_NULL: return "n";
		case E_BOOL: return e.v ? "t" : "f";
		case E_NUM: return "#" + (e.v === 0 ? 0 : e.v);
		case E_STR: return "s" + e.s;
		default: {
			let id = collectionIds.get(e);
			if (id === undefined) collectionIds.set(e, (id = nextCollectionId++));
			return "@" + id;
		}
	}
}

// MappingProps: the property list of a mapping, with the merge-key index.
class MappingProps {
	constructor() {
		this.list = [];
		this.mergeIndex = new Map();
		this.mergeIndexed = 0;
	}
	append(key, value) { this.list.push([key, value]); }
	merge(mergeProps, parser) {
		while (this.mergeIndexed < this.list.length) {
			const idx = this.mergeIndexed;
			const h = mergeKeyHash(this.list[idx][0]);
			let b = this.mergeIndex.get(h);
			if (!b) this.mergeIndex.set(h, (b = []));
			b.push(idx);
			this.mergeIndexed++;
		}
		// Reverse order: of duplicate keys in the source the last one wins, and
		// the merged keys land in reverse source order (as stock Bun shows).
		next: for (let m = mergeProps.length - 1; m >= 0; m--) {
			const mergeProp = mergeProps[m];
			const h = mergeKeyHash(mergeProp[0]);
			const candidates = this.mergeIndex.get(h);
			if (candidates) {
				for (const idx of candidates) if (mergeKeyEql(this.list[idx][0], mergeProp[0])) continue next;
			}
			if (parser.mergePropsBudget === 0) fail("OutOfMemory");
			parser.mergePropsBudget--;
			this.list.push([mergeProp[0], mergeProp[1]]);
			let b = this.mergeIndex.get(h);
			if (!b) this.mergeIndex.set(h, (b = []));
			b.push(this.list.length - 1);
			this.mergeIndexed = this.list.length;
		}
	}
}

// ScalarResolverCtx with its StringBuilder: `str` is the built text, `ws` the
// pending whitespace that only lands if more content follows (drain).
class ScalarCtx {
	constructor(p, tag) {
		this.str = "";
		this.ws = "";
		this.resolved = false;
		this.scalar = null;
		this.tag = tag;
		this.resolvedScalarLen = 0;
		this.start = p.pos;
		this.line = p.line;
		this.lineIndent = p.lineIndent;
		this.multiline = false;
	}
	drain() {
		if (this.ws.length) { this.str += this.ws; this.ws = ""; }
	}
	done() {
		let data;
		if (this.scalar !== null && this.str.length === this.resolvedScalarLen) data = this.scalar;
		else data = strExpr(this.str);
		return { k: T_SCALAR, start: this.start, indent: this.lineIndent, line: this.line,
			scalar: { data, style: S_PLAIN, multiline: this.multiline } };
	}
	checkAppend(p) {
		if (this.str.length === 0) {
			this.lineIndent = p.lineIndent;
			this.line = p.line;
		} else if (this.line !== p.line) {
			this.multiline = true;
		}
	}
	appendSource(p, ch) {
		this.checkAppend(p);
		this.drain();
		this.str += ch;
	}
	appendSourceWhitespace(ch) { this.ws += ch; }
	appendUnknownSourceSlice(p, off, end) {
		for (let i = off; i < end; i++) {
			const c = p.s.charCodeAt(i);
			if (isWsOrBreak(c)) this.ws += p.s[i];
			else { this.checkAppend(p); this.drain(); this.str += p.s[i]; }
		}
	}
	appendWhitespace(ch) { this.ws += ch; }
	appendWhitespaceNTimes(ch, n) { if (n) this.ws += ch.repeat(n); }
	resolve(scalar, text) {
		this.drain();
		this.str += text;
		this.resolved = true;
		switch (this.tag) {
			case TAG_NONE: this.resolvedScalarLen = this.str.length; this.scalar = scalar; break;
			case TAG_BOOL: if (scalar.t === E_BOOL) { this.resolvedScalarLen = this.str.length; this.scalar = scalar; } break;
			case TAG_INT:
			case TAG_FLOAT: if (scalar.t === E_NUM) { this.resolvedScalarLen = this.str.length; this.scalar = scalar; } break;
			case TAG_NULL: if (scalar.t === E_NULL) { this.resolvedScalarLen = this.str.length; this.scalar = scalar; } break;
			default: break;	// !, !!str, verbatim and unknown tags always make strings
		}
	}

	tryResolveNumber(p, firstChar) {
		switch (firstChar) {
			case FC_DOT: {
				const c = p.next();
				if (c === 0x6e) {	// n
					p.inc(1);
					if (p.startsWith("an")) { this.resolve(numExpr(NaN), "nan"); p.inc(2); return; }
					this.appendSource(p, "n");
					return;
				}
				if (c === 0x4e) {	// N
					p.inc(1);
					if (p.startsWith("aN")) { this.resolve(numExpr(NaN), "NaN"); p.inc(2); return; }
					if (p.startsWith("AN")) { this.resolve(numExpr(NaN), "NAN"); p.inc(2); return; }
					this.appendSource(p, "N");
					return;
				}
				if (c === 0x69) {	// i
					p.inc(1);
					if (p.startsWith("nf")) { this.resolve(numExpr(Infinity), "inf"); p.inc(2); return; }
					this.appendSource(p, "i");
					return;
				}
				if (c === 0x49) {	// I
					p.inc(1);
					if (p.startsWith("nf")) { this.resolve(numExpr(Infinity), "Inf"); p.inc(2); return; }
					if (p.startsWith("NF")) { this.resolve(numExpr(Infinity), "INF"); p.inc(2); return; }
					this.appendSource(p, "I");
					return;
				}
				break;
			}
			case FC_NEGATIVE:
			case FC_POSITIVE: {
				if (p.next() === 0x2e && (p.peek(1) === 0x69 || p.peek(1) === 0x49)) {
					this.appendSource(p, ".");
					p.inc(1);
					const inf = firstChar === FC_NEGATIVE ? -Infinity : Infinity;
					const c = p.next();
					if (c === 0x69) {
						p.inc(1);
						if (p.startsWith("nf")) { this.resolve(numExpr(inf), "inf"); p.inc(2); return; }
						this.appendSource(p, "i");
						return;
					}
					if (c === 0x49) {
						p.inc(1);
						if (p.startsWith("nf")) { this.resolve(numExpr(inf), "Inf"); p.inc(2); return; }
						if (p.startsWith("NF")) { this.resolve(numExpr(inf), "INF"); p.inc(2); return; }
						this.appendSource(p, "I");
						return;
					}
					return;
				}
				break;
			}
			default: break;
		}

		const start = p.pos;
		let decimal = p.next() === 0x2e;
		let x = false, o = false, e = false, minus = false, hex = false;
		// The first body char is at `pos`; step past it (for a sign only when it
		// is a '.') so the loop starts on the second with the flags set.
		if (!(firstChar === FC_NEGATIVE || firstChar === FC_POSITIVE) || decimal) p.inc(1);
		let first = true;
		let c = p.next();
		let valid;
		for (;;) {
			if (c === 0x20 || c === 0x09 || c === 0 || c === 0x0a || c === 0x0d || c === 0x3a) {
				valid = !(first && (firstChar === FC_POSITIVE || firstChar === FC_NEGATIVE));
				break;
			}
			if (c === 0x2c || c === 0x5d || c === 0x7d) {
				const ctx = p.context();
				valid = ctx === FLOW_IN || ctx === FLOW_KEY;
				break;
			}
			if (c === 0x30) {
				const wasFirst = first;
				first = false;
				p.inc(1);
				if (wasFirst) {
					const n = p.next();
					if (n === 0x62 || n === 0x42) { valid = false; break; }
					c = n;
					continue;
				}
				c = p.next();
				continue;
			}
			if (c >= 0x31 && c <= 0x39) { first = false; p.inc(1); c = p.next(); continue; }
			if (c === 0x65 || c === 0x45) {
				first = false;
				if (e) hex = true;
				e = true;
				p.inc(1);
				c = p.next();
				continue;
			}
			if ((c >= 0x61 && c <= 0x64) || c === 0x66 || (c >= 0x41 && c <= 0x44) || c === 0x46) {
				hex = true;
				if (first && (c === 0x62 || c === 0x42)) { valid = false; break; }
				first = false;
				p.inc(1);
				c = p.next();
				continue;
			}
			if (c === 0x78) {
				first = false;
				if (x) { valid = false; break; }
				x = true; p.inc(1); c = p.next(); continue;
			}
			if (c === 0x6f) {
				first = false;
				if (o) { valid = false; break; }
				o = true; p.inc(1); c = p.next(); continue;
			}
			if (c === 0x2e) {
				first = false;
				if (decimal) { valid = false; break; }
				decimal = true; p.inc(1); c = p.next(); continue;
			}
			if (c === 0x2b) {
				first = false;
				if (x) { valid = false; break; }
				p.inc(1); c = p.next(); continue;
			}
			if (c === 0x2d) {
				first = false;
				if (minus) { valid = false; break; }
				minus = true; p.inc(1); c = p.next(); continue;
			}
			valid = false;
			break;
		}
		const end = p.pos;
		this.appendUnknownSourceSlice(p, start, end);
		if (!valid) return;

		const lexed = p.s.slice(start, end);
		let value;
		if (x || o || hex) {
			value = parseUnsignedRadix0(lexed);
			if (value === null) return;
		} else {
			// Shape-checked first: WTF::parseDouble prefix-parses (`1+1` -> 1).
			// On a slice of this shape it is a plain decimal conversion.
			if (!isCoreSchemaNumber(lexed, firstChar)) return;
			value = Number(lexed);
		}
		this.resolved = true;
		if (this.tag === TAG_NONE || this.tag === TAG_FLOAT || this.tag === TAG_INT) {
			this.resolvedScalarLen = this.str.length;
			this.scalar = numExpr(firstChar === FC_NEGATIVE ? -value : value);
		}
	}
}

const scanOpts = (o) => ({
	additionalParentIndent: o && o.additionalParentIndent !== undefined ? o.additionalParentIndent : null,
	tag: o && o.tag !== undefined ? o.tag : TAG_NONE,
	firstScan: !!(o && o.firstScan),
	outsideContext: !!(o && o.outsideContext),
});
const DEFAULT_SCAN = Object.freeze(scanOpts());

const tokenTag = (tok) => (tok && tok.k === T_TAG ? tok.tag : TAG_NONE);

class NodeProps {
	constructor() {
		this.hasAnchor = null;
		this.hasTag = null;
		this.hasMappingAnchor = null;
		this.hasMappingTag = null;
	}
	hasAnchorOrTag() { return this.hasAnchor !== null || this.hasTag !== null; }
	setAnchor(anchor) {
		const prev = this.hasAnchor;
		if (prev !== null) {
			this.hasAnchor = null;
			if (prev.line === anchor.line || this.hasMappingAnchor !== null) fail("MultipleAnchors");
			this.hasMappingAnchor = prev;
		}
		this.hasAnchor = anchor;
	}
	anchorLine() { return this.hasAnchor ? this.hasAnchor.line : null; }
	takeFlowNodeAnchor(line) {
		if (this.hasAnchor !== null && this.hasAnchor.line === line) {
			const a = this.hasAnchor;
			this.hasAnchor = null;
			return a;
		}
		return null;
	}
	takeBlockMappingAnchor(line) { return this.takeImplicitKeyAnchors(line).mappingAnchor; }
	takeImplicitKeyAnchors(line) {
		if (this.hasMappingAnchor !== null) {
			const mappingAnchor = this.hasMappingAnchor;
			this.hasMappingAnchor = null;
			const inner = this.hasAnchor;
			this.hasAnchor = null;
			if (inner !== null && inner.line !== line) fail("MultipleAnchors");
			return { keyAnchor: inner, mappingAnchor };
		}
		if (this.hasAnchor !== null) {
			const mystery = this.hasAnchor;
			this.hasAnchor = null;
			if (mystery.line === line) return { keyAnchor: mystery, mappingAnchor: null };
			return { keyAnchor: null, mappingAnchor: mystery };
		}
		return { keyAnchor: null, mappingAnchor: null };
	}
	setTag(tok) {
		if (this.hasTag !== null) {
			if (this.hasTag.line === tok.line) fail("MultipleTags");
			this.hasMappingTag = this.hasTag;
		}
		this.hasTag = tok;
	}
	tag() { return tokenTag(this.hasTag); }
	tagLine() { return this.hasTag ? this.hasTag.line : null; }
	takeTag() { const t = this.tag(); this.hasTag = null; return t; }
}

class Parser {
	constructor(s) {
		this.s = s;
		this.len = s.length;
		// [206] l-document-prefix ::= c-byte-order-mark? l-comment*
		const start = s.length >= 3 && s.charCodeAt(0) === 0xef && s.charCodeAt(1) === 0xbb && s.charCodeAt(2) === 0xbf ? 3 : 0;
		this.pos = start;
		this.lineStartPos = start;
		this.lineIndent = 0;
		this.tabAfterIndent = false;
		this.line = 1;
		this.token = { k: T_EOF, start: 0, indent: 0, line: 1 };
		this.contexts = [];
		this.blockIndents = [];
		this.explicitDocumentStartLine = null;
		this.anchors = new Map();
		this.openCollections = [];
		this.hasCyclicAlias = false;
		this.tagHandles = new Set();
		this.stackUsed = 0;
		this.leastStackLeft = Infinity;
		this.mergePropsBudget = MAX_MERGED_PROPERTIES;
		this.aliasExpansionBudget = MAX_ALIAS_EXPANSION;
	}

	// ── input cursor ──
	next() { return this.pos < this.len ? this.s.charCodeAt(this.pos) : 0; }
	peek(n) { const p = this.pos + n; return p < this.len ? this.s.charCodeAt(p) : 0; }
	inc(n) { this.pos = Math.min(this.pos + n, this.len); }
	newline() {
		this.lineIndent = 0;
		this.tabAfterIndent = false;
		this.lineStartPos = this.pos + 1;
		this.line++;
	}
	startsWith(lit) { return this.s.startsWith(lit, this.pos); }
	isEof() { return this.pos >= this.len; }
	isAtLineStart() { return this.pos === this.lineStartPos; }
	isChar(c) { return this.pos < this.len && this.s.charCodeAt(this.pos) === c; }
	trySkipChar(c) { if (!this.isChar(c)) fail("UnexpectedCharacter"); this.inc(1); }
	isSWhite() { const c = this.next(); return this.pos < this.len && (c === 0x20 || c === 0x09); }
	isSWhiteAt(n) { const p = this.pos + n; if (p >= this.len) return false; const c = this.s.charCodeAt(p); return c === 0x20 || c === 0x09; }
	isBChar() { const c = this.next(); return this.pos < this.len && (c === 0x0a || c === 0x0d); }
	isBCharOrEof() { if (this.pos >= this.len) return true; const c = this.s.charCodeAt(this.pos); return c === 0x0a || c === 0x0d; }
	isSWhiteOrBCharOrEofAt(n) { const p = this.pos + n; return p < this.len ? isWsOrBreak(this.s.charCodeAt(p)) : true; }
	isSWhiteOrBCharAt(n) { const p = this.pos + n; return p < this.len ? isWsOrBreak(this.s.charCodeAt(p)) : false; }
	skipSWhite() { while (this.isSWhite()) this.inc(1); }
	trySkipSWhite() { if (!this.isSWhite()) fail("UnexpectedCharacter"); this.skipSWhite(); }
	isNsChar() { return this.pos < this.len && isNsChar(this.s.charCodeAt(this.pos)); }
	skipNsChars() { while (this.isNsChar()) this.inc(1); }
	trySkipNsChars() { if (!this.isNsChar()) fail("UnexpectedCharacter"); this.skipNsChars(); }
	isWordChar() { return this.pos < this.len && isWordChar(this.s.charCodeAt(this.pos)); }
	skipNsWordChars() { while (this.isWordChar()) this.inc(1); }
	trySkipNsWordChars() { if (!this.isWordChar()) fail("UnexpectedCharacter"); this.skipNsWordChars(); }
	isDecDigit() { return this.pos < this.len && isDecDigit(this.s.charCodeAt(this.pos)); }
	trySkipNsDecDigits() { if (!this.isDecDigit()) fail("UnexpectedCharacter"); while (this.isDecDigit()) this.inc(1); }
	isAnchorChar() { return this.pos < this.len && isAnchorChar(this.s.charCodeAt(this.pos)); }
	trySkipNsAnchorChars() { if (!this.isAnchorChar()) fail("UnexpectedCharacter"); this.inc(1); while (this.isAnchorChar()) this.inc(1); }
	// chars::is_ns_tag_char over remain(): null, or the length of the char.
	nsTagCharLen() {
		const p = this.pos, rem = this.len - p;
		if (rem <= 0) return null;
		const c0 = this.s.charCodeAt(p);
		switch (c0) {
			case 0x23: case 0x3b: case 0x2f: case 0x3f: case 0x3a: case 0x40: case 0x26: case 0x3d: case 0x2b:
			case 0x24: case 0x5f: case 0x2e: case 0x7e: case 0x2a: case 0x27: case 0x28: case 0x29:
				return 1;
			case 0x21: case 0x2c: case 0x5b: case 0x5d: case 0x7b: case 0x7d:
				return null;
		}
		if (c0 === 0x25 && rem > 2 && isHexDigit(this.s.charCodeAt(p + 1)) && isHexDigit(this.s.charCodeAt(p + 2))) return 3;
		return isWordChar(c0) ? 1 : null;
	}
	skipNsTagChars() { let l; while ((l = this.nsTagCharLen()) !== null) this.inc(l); }
	trySkipNsTagChars() {
		const l = this.nsTagCharLen();
		if (l === null) fail("UnexpectedCharacter");
		this.inc(l);
		this.skipNsTagChars();
	}
	isNsUriChar() {
		const p = this.pos, rem = this.len - p;
		if (rem <= 0) return false;
		const c0 = this.s.charCodeAt(p);
		switch (c0) {
			case 0x23: case 0x3b: case 0x2f: case 0x3f: case 0x3a: case 0x40: case 0x26: case 0x3d: case 0x2b:
			case 0x24: case 0x2c: case 0x5f: case 0x2e: case 0x21: case 0x7e: case 0x2a: case 0x27: case 0x28:
			case 0x29: case 0x5b: case 0x5d:
				return true;
		}
		if (c0 === 0x25 && rem > 2 && isHexDigit(this.s.charCodeAt(p + 1)) && isHexDigit(this.s.charCodeAt(p + 2))) return true;
		return isWordChar(c0);
	}
	skipNsUriChars() { while (this.isNsUriChar()) this.inc(1); }
	// s-l-comments: leaves `pos` on the line break, or at EOF.
	trySkipToNewLine() {
		let whitespace = false;
		if (this.isSWhite()) { whitespace = true; this.skipSWhite(); }
		if (this.isChar(0x23)) {
			if (!whitespace) fail("UnexpectedCharacter");
			this.inc(1);
			while (!this.isBCharOrEof()) {
				if (this.next() === 0) fail("UnexpectedCharacter");
				this.inc(1);
			}
		}
		if (this.pos < this.len && !this.isChar(0x0a) && !this.isChar(0x0d)) fail("UnexpectedCharacter");
	}

	// ── contexts / stack ──
	context() { const l = this.contexts.length; return l ? this.contexts[l - 1] : BLOCK_OUT; }
	setContext(c) { this.contexts.push(c); }
	unsetContext() { this.contexts.pop(); }
	blockIndent() { const l = this.blockIndents.length; return l ? this.blockIndents[l - 1] : null; }
	stackRemaining() { return Math.max(0, STACK_UNITS - this.stackUsed); }
	tokenInit(start) { return { start, indent: this.lineIndent, line: this.line }; }

	parse() {
		this.scan(scanOpts({ firstScan: true }));
		const docs = [];
		// one null document if eof, not zero documents.
		let first = true;
		while (first || this.token.k !== T_EOF) {
			first = false;
			docs.push(this.parseDocument());
		}
		return docs;
	}

	// Returns true for a %YAML directive.
	parseDirective() {
		if (this.token.indent !== 0) fail("InvalidDirective");
		if (this.startsWith("YAML") && this.isSWhiteAt(4)) {
			this.inc(4);
			this.trySkipSWhite();
			this.trySkipNsDecDigits();
			this.trySkipChar(0x2e);
			this.trySkipNsDecDigits();
			this.trySkipToNewLine();
			return true;
		}
		if (this.startsWith("TAG") && this.isSWhiteAt(3)) {
			this.inc(3);
			this.trySkipSWhite();
			this.trySkipChar(0x21);
			if (this.isSWhite()) {	// primary tag handle
				this.skipSWhite();
				this.parseDirectiveTagPrefix();
				this.trySkipToNewLine();
				return false;
			}
			if (this.isChar(0x21)) {	// secondary tag handle
				this.inc(1);
				this.trySkipSWhite();
				this.parseDirectiveTagPrefix();
				this.trySkipToNewLine();
				return false;
			}
			const off = this.pos;	// named tag handle
			this.trySkipNsWordChars();
			const handle = this.s.slice(off, this.pos);
			this.trySkipChar(0x21);
			this.trySkipSWhite();
			this.tagHandles.add(handle);
			this.parseDirectiveTagPrefix();
			this.trySkipToNewLine();
			return false;
		}
		// reserved directive
		this.trySkipNsChars();
		this.skipSWhite();
		while (this.isNsChar()) {
			this.skipNsChars();
			this.skipSWhite();
		}
		this.trySkipToNewLine();
		return false;
	}

	parseDirectiveTagPrefix() {
		if (this.isChar(0x21)) {	// local tag prefix
			this.inc(1);
			this.skipNsUriChars();
			return;
		}
		const l = this.nsTagCharLen();	// global tag prefix
		if (l !== null) {
			this.inc(l);
			this.skipNsUriChars();
			return;
		}
		fail("InvalidDirective");
	}

	parseDocument() {
		this.anchors.clear();
		this.hasCyclicAlias = false;
		this.tagHandles.clear();
		let hasDirectives = false, hasYamlDirective = false;
		while (this.token.k === T_DIRECTIVE) {
			if (this.parseDirective()) {
				if (hasYamlDirective) fail("MultipleYamlDirectives");
				hasYamlDirective = true;
			}
			hasDirectives = true;
			this.scan(DEFAULT_SCAN);
		}
		this.explicitDocumentStartLine = null;
		if (this.token.k === T_DOC_START) {
			this.explicitDocumentStartLine = this.token.line;
			this.scan(DEFAULT_SCAN);
		} else if (hasDirectives) {
			fail("UnexpectedToken");	// directives must end with '---'
		}
		const root = this.parseNode({});
		switch (this.token.k) {
			case T_EOF:
			case T_DOC_START:
				break;
			case T_DOC_END: {
				let documentEndLine = this.token.line;
				this.scan(DEFAULT_SCAN);
				while (this.token.k === T_DOC_END) {	// consume all bare documents
					documentEndLine = this.token.line;
					this.scan(DEFAULT_SCAN);
				}
				if (this.token.line === documentEndLine && this.token.k !== T_EOF) fail("UnexpectedToken");
				break;
			}
			default:
				fail("UnexpectedToken");
		}
		return root;
	}

	// [149] a JSON-style key in flow-in scans its `:` in flow-key.
	maybeSetJsonKey(allowed) {
		if (allowed && this.context() === FLOW_IN) { this.setContext(FLOW_KEY); return true; }
		return false;
	}
	unsetJsonKey(wasSet) { if (wasSet) this.unsetContext(); }

	// [142]/[143] consume `?` and parse the flow explicit-entry key.
	parseFlowExplicitKey() {
		this.scan(DEFAULT_SCAN);
		const k = this.token.k;
		if (k === T_MAP_VALUE || k === T_COLLECT || k === T_MAP_END || k === T_SEQ_END) return NULL_EXPR;
		if (k === T_MAP_KEY) fail("UnexpectedToken");
		let scannedTag = null, scannedAnchor = null;
		for (;;) {
			if (this.token.k === T_ANCHOR && scannedAnchor === null) {
				scannedAnchor = this.pendingAnchor(this.token.name, this.stackRemaining());
			} else if (this.token.k === T_TAG && scannedTag === null) {
				scannedTag = this.token;
			} else {
				break;
			}
			this.scan(scanOpts({ tag: tokenTag(scannedTag) }));
			const t = this.token.k;
			if (t === T_MAP_VALUE || t === T_COLLECT || t === T_MAP_END || t === T_SEQ_END) {
				return this.propsToENode(scannedTag, scannedAnchor);
			}
		}
		this.setContext(FLOW_KEY);
		const key = this.parseNode({ explicitMappingKey: true, scannedTag, scannedAnchor, frame: FRAME_FLOW_MAPPING });
		this.unsetContext();
		return key;
	}

	parseFlowSequenceEntries() {
		const seq = [];
		this.setContext(FLOW_IN);
		this.scan(DEFAULT_SCAN);
		while (this.token.k !== T_SEQ_END) {
			let item;
			if (this.token.k === T_MAP_KEY) {
				// [150] ns-flow-pair ::= '?' s-separate ns-flow-map-explicit-entry
				const key = this.parseFlowExplicitKey();
				let value;
				if (this.token.k === T_MAP_VALUE) {
					this.scan(DEFAULT_SCAN);
					if (this.token.k === T_COLLECT || this.token.k === T_SEQ_END) value = NULL_EXPR;
					else value = this.parseNode({ currentMappingIndent: this.token.indent, frame: FRAME_FLOW_SEQUENCE });
				} else {
					value = NULL_EXPR;
				}
				const props = new MappingProps();
				this.appendEntry(props, key, value);
				item = { t: E_OBJ, props: props.list };
			} else {
				item = this.parseNode({ flowPairAllowed: true, frame: FRAME_FLOW_SEQUENCE });
			}
			seq.push(item);
			if (this.token.k === T_SEQ_END) break;
			if (this.token.k !== T_COLLECT) fail("UnexpectedToken");
			this.scan(DEFAULT_SCAN);
		}
		// The post-`]` scan happens after FLOW_IN is popped.
		this.unsetContext();
		this.scan(DEFAULT_SCAN);
		return seq;
	}

	parseFlowMappingEntries() {
		const props = new MappingProps();
		this.setContext(FLOW_IN);
		this.scan(DEFAULT_SCAN);
		while (this.token.k !== T_MAP_END) {
			let key;
			if (this.token.k === T_MAP_KEY) {
				key = this.parseFlowExplicitKey();
			} else if (this.token.k === T_MAP_VALUE) {
				key = NULL_EXPR;	// [147] e-node key followed by `:`
			} else {
				this.setContext(FLOW_KEY);
				key = this.parseNode({ frame: FRAME_FLOW_MAPPING });
				this.unsetContext();
			}
			const k = this.token.k;
			if (k === T_COLLECT) {
				props.append(key, NULL_EXPR);
				this.scan(DEFAULT_SCAN);
				continue;
			}
			if (k === T_MAP_END) {
				props.append(key, NULL_EXPR);
				continue;
			}
			if (k !== T_MAP_VALUE) fail("UnexpectedToken");
			this.scan(DEFAULT_SCAN);
			if (this.token.k === T_MAP_END || this.token.k === T_COLLECT) {
				props.append(key, NULL_EXPR);
			} else {
				// [147] the value is ns-flow-node (`{a: b: c}` is an error).
				const value = this.parseNode({ currentMappingIndent: this.token.indent, frame: FRAME_FLOW_MAPPING });
				this.appendEntry(props, key, value);
			}
			// [140] after an entry, only `,` or `}`.
			if (this.token.k === T_COLLECT) this.scan(DEFAULT_SCAN);
			else if (this.token.k !== T_MAP_END) fail("UnexpectedToken");
		}
		this.unsetContext();
		this.scan(DEFAULT_SCAN);
		return props.list;
	}

	parseBlockSequenceEntries() {
		const sequenceIndent = this.token.indent;
		// [200] same-line content after `---` can only be a flow node.
		if (this.explicitDocumentStartLine !== null && this.token.line === this.explicitDocumentStartLine) fail("UnexpectedToken");
		this.blockIndents.push(sequenceIndent);
		const seq = [];
		let prevLine = 0;
		while (this.token.k === T_SEQ_ENTRY && this.token.indent === sequenceIndent) {
			if (this.tabAfterIndent) fail("TabIndentation");
			const entryLine = this.token.line;
			const entryStart = this.token.start;
			// only the first entry can be another sequence entry on the same line
			if (seq.length !== 0 && prevLine === entryLine) break;
			prevLine = entryLine;
			this.scan(scanOpts({ additionalParentIndent: sequenceIndent + 1 }));
			seq.push(this.parseBlockIndented(sequenceIndent, entryLine, entryStart + 2, K_SEQ_ENTRY));
		}
		this.blockIndents.pop();
		return seq;
	}

	// [190] c-l-block-map-explicit-key; the current token is the `?`.
	parseBlockExplicitKey(mappingStart, mappingIndent, mappingLine) {
		this.blockIndents.push(mappingIndent);
		this.scan(scanOpts({ additionalParentIndent: mappingIndent + 1 }));
		const key = this.parseBlockIndented(mappingIndent, mappingLine, mappingStart, K_MAP_EXPLICIT_KEY);
		this.blockIndents.pop();
		return key;
	}

	parseBlockMapping(anchor, firstKey, mappingStart, mappingIndent, mappingLine, flowPairAllowed) {
		return this.parseCollection(E_OBJ, anchor, BODY_BLOCK_MAPPING, firstKey, mappingIndent, mappingLine, flowPairAllowed);
	}

	// The current token is the one after `firstKey`.
	parseBlockMappingEntries(firstKey, mappingIndent, mappingLine, flowPairAllowed) {
		if (this.explicitDocumentStartLine !== null && mappingLine === this.explicitDocumentStartLine) fail("UnexpectedToken");
		// Reached from a flow collection (`["a": b]`), the key's column is not a block boundary.
		const pushedBlockIndent = !(this.context() === FLOW_IN || this.context() === FLOW_KEY);
		if (pushedBlockIndent) this.blockIndents.push(mappingIndent);
		const props = new MappingProps();
		let firstEntryEndLine = mappingLine;
		{
			const mappingValueLine = this.token.line;
			const mappingValueStart = this.token.start;
			let value;
			if (this.token.k === T_MAP_KEY) {	// a !!set entry
				if (this.token.line === mappingLine) fail("UnexpectedToken");
				value = NULL_EXPR;
			} else if (this.token.k === T_MAP_VALUE) {
				value: {
					firstEntryEndLine = mappingValueLine;
					// [191] the `:` must be at exactly the `?` indent (block ctx).
					if (mappingValueLine !== mappingLine
						&& !(this.context() === FLOW_IN || this.context() === FLOW_KEY)
						&& (this.token.indent !== mappingIndent || this.tabAfterIndent)) {
						if (this.token.indent < mappingIndent) {	// [189] e-node
							value = NULL_EXPR;
							break value;
						}
						fail(this.tabAfterIndent ? "TabIndentation" : "UnexpectedToken");
					}
					const parentIndent = mappingValueLine !== mappingLine ? mappingIndent + 1 : null;
					this.scan(scanOpts({ additionalParentIndent: parentIndent }));
					value = this.parseBlockIndented(mappingIndent, mappingValueLine, mappingValueStart, K_MAP_VALUE, flowPairAllowed);
				}
			} else {
				value = NULL_EXPR;	// [189] explicit-value is optional
			}
			this.appendEntry(props, firstKey, value);
		}

		if (this.context() === FLOW_IN) {
			if (pushedBlockIndent) this.blockIndents.pop();
			return props.list;
		}

		this.setContext(BLOCK_IN);
		let previousLine = firstEntryEndLine;
		while (!(this.token.k === T_EOF || this.token.k === T_DOC_START || this.token.k === T_DOC_END)
			&& this.token.indent === mappingIndent && this.token.line !== previousLine) {
			// [192]/[195] each entry sits at s-indent(n) (spaces only).
			if (this.tabAfterIndent) fail("TabIndentation");
			const keyLine = this.token.line;
			previousLine = keyLine;
			const explicitKey = this.token.k === T_MAP_KEY;
			const key = this.parseNode({ currentMappingIndent: mappingIndent, frame: FRAME_BLOCK_MAPPING });
			const k = this.token.k;
			if (k === T_EOF) {
				if (explicitKey) { props.append(key, NULL_EXPR); continue; }
				fail("UnexpectedToken");
			} else if (k === T_MAP_VALUE && explicitKey) {
				// [191] l-block-map-explicit-value ::= s-indent(n) ':' ...
				if (this.token.indent !== mappingIndent || this.tabAfterIndent) {
					if (this.token.indent < mappingIndent) { props.append(key, NULL_EXPR); continue; }
					fail(this.tabAfterIndent ? "TabIndentation" : "UnexpectedToken");
				}
			} else if (k === T_MAP_VALUE) {
				if (keyLine !== this.token.line) fail("MultilineImplicitKey");
			} else if (k === T_MAP_KEY) {
				// next !!set entry
			} else {
				if (explicitKey) { props.append(key, NULL_EXPR); continue; }
				fail("UnexpectedToken");
			}

			const mappingValueLine = this.token.line;
			const mappingValueStart = this.token.start;
			if (this.token.k === T_MAP_VALUE) previousLine = mappingValueLine;
			let value;
			if (this.token.k === T_MAP_KEY) {	// a !!set entry
				if (this.token.line === keyLine) fail("UnexpectedToken");
				value = NULL_EXPR;
			} else {
				const parentIndent = mappingValueLine !== keyLine ? mappingIndent + 1 : null;
				this.scan(scanOpts({ additionalParentIndent: parentIndent }));
				value = this.parseBlockIndented(mappingIndent, mappingValueLine, mappingValueStart, K_MAP_VALUE, false);
			}
			this.appendEntry(props, key, value);
		}
		this.unsetContext();
		if (pushedBlockIndent) this.blockIndents.pop();
		return props.list;
	}

	// Appends `key: value`, expanding a `<<` merge key (quoted or not).
	appendEntry(props, key, value) {
		if (key.t === E_STR && key.s === "<<") {
			this.rejectOpenMergeSource(value);
			if (value.t === E_OBJ) {
				props.merge(value.props, this);
				return;
			}
			if (value.t === E_ARR) {
				for (const item of value.items) {
					if (item.t === E_OBJ) {
						this.rejectOpenMergeSource(item);
						props.merge(item.props, this);
					}
				}
				return;
			}
		}
		props.append(key, value);
	}

	rejectOpenMergeSource(node) {
		// An open collection is only reachable through a cyclic alias.
		if (!this.hasCyclicAlias || !this.isOpenCollection(node)) return;
		fail("CyclicMerge");
	}

	// ── anchors ──
	pendingAnchor(name, stackLeft) {
		const a = { name, start: this.token.start, line: this.token.line, stackLeft, outerLeastStackLeft: this.leastStackLeft };
		this.leastStackLeft = stackLeft;
		return a;
	}
	// The stack used since `anchor` was scanned; outer anchors count it too.
	closeAnchor(anchor) {
		const stack = Math.max(0, anchor.stackLeft - this.leastStackLeft);
		this.leastStackLeft = Math.min(this.leastStackLeft, anchor.outerLeastStackLeft);
		return stack;
	}
	dropAnchors(np) {
		if (np.hasAnchor) this.closeAnchor(np.hasAnchor);
		if (np.hasMappingAnchor) this.closeAnchor(np.hasMappingAnchor);
	}
	bindAnchor(anchor, node) {
		const stack = this.closeAnchor(anchor);
		this.anchors.set(anchor.name, [node, stack]);
	}
	// The node exists (empty) while `body` parses its entries, so an alias to
	// it from inside (a cyclic alias) refers to the very node returned.
	// `body` is a BODY_* code rather than a closure to keep the JS stack per
	// nesting level small (see parseDeep).
	parseCollection(kind, anchor, body, a, b, c, d) {
		const node = kind === E_ARR ? { t: E_ARR, items: [] } : { t: E_OBJ, props: [] };
		if (anchor !== null) this.openCollections.push({ anchor: anchor.name, node });
		let r;
		switch (body) {
			case BODY_FLOW_SEQUENCE: r = this.parseFlowSequenceEntries(); break;
			case BODY_FLOW_MAPPING: r = this.parseFlowMappingEntries(); break;
			case BODY_BLOCK_SEQUENCE: r = this.parseBlockSequenceEntries(); break;
			case BODY_BLOCK_MAPPING: r = this.parseBlockMappingEntries(a, b, c, d); break;
			case BODY_EXPLICIT_KEY_MAPPING: {
				// a = mapping_start, b = mapping_indent, c = mapping_line, d = flow_pair_allowed
				const key = this.parseBlockExplicitKey(a, b, c);
				r = this.parseBlockMappingEntries(key, b, c, d);
				break;
			}
		}
		if (kind === E_ARR) node.items = r; else node.props = r;
		if (anchor === null) return node;
		this.openCollections.pop();
		this.bindAnchor(anchor, node);
		return node;
	}
	resolveAlias(name) {
		const anchored = this.anchors.get(name);
		if (anchored !== undefined) return anchored;
		for (let i = this.openCollections.length - 1; i >= 0; i--) {
			if (this.openCollections[i].anchor === name) {
				// CyclicAliases::Allow (Bun.YAML.parse)
				this.hasCyclicAlias = true;
				return [this.openCollections[i].node, 0];
			}
		}
		fail("UnresolvedAlias");
	}
	isOpenCollection(node) {
		if (!isCollection(node)) return false;
		return this.openCollections.some((o) => o.node === node);
	}

	// [185] s-l+block-indented(n, c); the current token follows the indicator.
	parseBlockIndented(n, indicatorLine, indicatorStart, kind, flowPairAllowed = false) {
		let valueTag = null, valueAnchor = null;
		const inFlow = this.context() === FLOW_IN || this.context() === FLOW_KEY;
		for (;;) {
			if (!inFlow && this.token.line !== indicatorLine) {
				// [196]/[201] a token on a later line at indent <= n (< n for a
				// nested `-` outside BLOCK-IN) belongs to the parent.
				const belongsToParent = this.token.k === T_SEQ_ENTRY && kind !== K_SEQ_ENTRY
					? this.token.indent < n
					: this.token.indent <= n;
				if (belongsToParent) {
					// A single-line plain scalar was resolved under `valueTag`;
					// rescan it tag-neutral for the parent.
					if (valueTag !== null && this.token.k === T_SCALAR && this.token.scalar.style === S_PLAIN && !this.token.scalar.multiline) {
						this.pos = this.token.start;
						this.line = this.token.line;
						this.lineIndent = this.token.indent;
						this.scan(DEFAULT_SCAN);
					}
					return this.propsToENode(valueTag, valueAnchor);
				}
			}
			const k = this.token.k;
			if (k === T_ANCHOR && valueAnchor === null) {
				valueAnchor = this.pendingAnchor(this.token.name, this.stackRemaining());
			} else if (k === T_TAG && valueTag === null) {
				valueTag = this.token;
			} else if ((k === T_SEQ_ENTRY || k === T_MAP_KEY) && this.token.line === indicatorLine
				&& (this.token.indent <= n || this.tabAfterIndent)) {
				// [185] a compact construct needs indent >= n+1 via spaces.
				fail(this.tabAfterIndent ? "TabIndentation" : "UnexpectedToken");
			} else if ((k === T_COLLECT || k === T_SEQ_END) && kind === K_MAP_VALUE && flowPairAllowed
				&& (this.context() === FLOW_IN || this.context() === FLOW_KEY)) {
				// [149] e-node pair value in flow (`"a":,` / `"a":]`).
				return this.propsToENode(valueTag, valueAnchor);
			} else {
				return this.parseNode({
					frame: kind === K_SEQ_ENTRY ? FRAME_BLOCK_SEQUENCE : FRAME_BLOCK_MAPPING,
					currentMappingIndent: n,
					explicitMappingKey: kind === K_MAP_EXPLICIT_KEY,
					scannedTag: valueTag,
					scannedAnchor: valueAnchor,
				});
			}
			this.scan(scanOpts({ tag: tokenTag(valueTag) }));
		}
	}

	// [161] e-scalar with a property's tag resolved and anchor registered.
	propsToENode(tag, anchor) {
		const e = resolveNull(tokenTag(tag));
		if (anchor !== null) this.bindAnchor(anchor, e);
		return e;
	}

	// Charges the budget for every node reachable from `root`, once per path;
	// an edge back onto the current path (a cyclic alias) counts once.
	chargeAliasExpansion(root) {
		if (!isCollection(root)) { this.chargeAliasNode(); return; }
		const onPath = this.hasCyclicAlias ? new Set() : null;
		// Visit::Exit is an EXIT marker on `stack`, its node kept in `exits`
		// (markers pop in the reverse order they were pushed, as do exits).
		const stack = [root];
		const EXIT = Symbol("exit");
		const exits = [];
		while (stack.length) {
			const node = stack.pop();
			if (node === EXIT) { onPath.delete(exits.pop()); continue; }
			this.chargeAliasNode();
			if (onPath !== null && isCollection(node)) {
				if (onPath.has(node)) continue;
				onPath.add(node);
				stack.push(EXIT);
				exits.push(node);
			}
			if (node.t === E_ARR) {
				for (const item of node.items) stack.push(item);
			} else if (node.t === E_OBJ) {
				for (const [k, v] of node.props) { stack.push(k); stack.push(v); }
			}
		}
	}
	chargeAliasNode() {
		if (this.aliasExpansionBudget === 0) fail("ExcessiveAliasing");
		this.aliasExpansionBudget--;
	}

	parseNode(opts) {
		const stackLeft = this.stackRemaining();
		if (stackLeft === 0) fail("StackOverflow");
		this.leastStackLeft = Math.min(this.leastStackLeft, stackLeft);
		const frame = opts.frame || 0;	// the document root is free
		this.stackUsed += frame;
		const r = this.parseNodeInner(opts, stackLeft);
		this.stackUsed -= frame;
		return r;
	}

	parseNodeInner(opts, stackLeft) {
		const currentMappingIndent = opts.currentMappingIndent !== undefined ? opts.currentMappingIndent : null;
		const explicitMappingKey = !!opts.explicitMappingKey;
		const flowPairAllowed = !!opts.flowPairAllowed;
		const np = new NodeProps();
		if (opts.scannedTag) np.setTag(opts.scannedTag);
		if (opts.scannedAnchor) np.setAnchor(opts.scannedAnchor);

		let node;
		node: for (;;) {
			const tok = this.token;
			switch (tok.k) {
				case T_EOF:
				case T_DOC_START:
				case T_DOC_END:
					node = NULL_EXPR;
					break node;

				case T_ANCHOR:
					np.setAnchor(this.pendingAnchor(tok.name, stackLeft));
					this.scan(scanOpts({ tag: np.tag() }));
					continue;

				case T_TAG:
					np.setTag(tok);
					this.scan(scanOpts({ tag: tok.tag }));
					continue;

				case T_ALIAS: {
					const aliasIndent = tok.indent, aliasLine = tok.line;
					const aliasTabAfterIndent = this.tabAfterIndent;
					if (np.hasAnchor && np.hasAnchor.line === aliasLine) fail("UnexpectedToken");
					if (np.hasTag && np.hasTag.line === aliasLine) fail("UnexpectedToken");
					const [copy, stack] = this.resolveAlias(tok.name);
					// Walkers recurse through aliases: charge the target's stack.
					const left = stackLeft - stack;
					if (!(left > 0)) fail("StackOverflow");
					this.leastStackLeft = Math.min(left, this.leastStackLeft);
					this.chargeAliasExpansion(copy);
					this.scan(DEFAULT_SCAN);
					if (this.token.k === T_MAP_VALUE) {
						if (this.token.indent < aliasIndent
							|| (explicitMappingKey && aliasLine !== this.token.line && this.token.indent <= aliasIndent)) {
							node = copy;
							break node;
						}
						if (this.context() === FLOW_KEY) { this.dropAnchors(np); return copy; }
						if (aliasTabAfterIndent && (this.context() === BLOCK_OUT || this.context() === BLOCK_IN)) fail("TabIndentation");
						if (currentMappingIndent !== null && currentMappingIndent === aliasIndent) { this.dropAnchors(np); return copy; }
						if (aliasLine !== this.token.line && !explicitMappingKey) fail("MultilineImplicitKey");
						if (this.context() === FLOW_IN && !flowPairAllowed) { this.dropAnchors(np); return copy; }
						return this.parseBlockMapping(np.takeBlockMappingAnchor(aliasLine), copy, tok.start, aliasIndent, aliasLine, flowPairAllowed);
					}
					node = copy;
					break node;
				}

				case T_SEQ_START:
				case T_MAP_START: {
					const isSeq = tok.k === T_SEQ_START;
					const cStart = tok.start, cIndent = tok.indent, cLine = tok.line;
					const cTabAfterIndent = this.tabAfterIndent;
					const anchor = np.takeFlowNodeAnchor(cLine);
					const jsonKey = this.maybeSetJsonKey(flowPairAllowed);
					const coll = isSeq
						? this.parseCollection(E_ARR, anchor, BODY_FLOW_SEQUENCE)
						: this.parseCollection(E_OBJ, anchor, BODY_FLOW_MAPPING);
					this.unsetJsonKey(jsonKey);
					if (this.token.k === T_MAP_VALUE) {
						if (this.token.indent < cIndent
							|| (explicitMappingKey && cLine !== this.token.line && this.token.indent <= cIndent)) {
							node = coll;
							break node;
						}
						if (this.context() === FLOW_KEY) { node = coll; break node; }
						if (cTabAfterIndent && (this.context() === BLOCK_OUT || this.context() === BLOCK_IN)) fail("TabIndentation");
						if (currentMappingIndent !== null && currentMappingIndent === cIndent) { node = coll; break node; }
						if (cLine !== this.token.line && !explicitMappingKey) fail("MultilineImplicitKey");
						if (this.context() === FLOW_IN && !flowPairAllowed) { node = coll; break node; }
						const map = this.parseBlockMapping(np.takeBlockMappingAnchor(cLine), coll, cStart, cIndent, cLine, flowPairAllowed);
						if (isSeq) return map;
						node = map;
						break node;
					}
					node = coll;
					break node;
				}

				case T_COLLECT:
				case T_SEQ_END:
				case T_MAP_END:
					if (np.hasAnchorOrTag()) { node = NULL_EXPR; break node; }
					fail("UnexpectedToken");
					break;

				case T_SEQ_ENTRY: {
					const al = np.anchorLine();
					if (al !== null && al === tok.line) fail("UnexpectedToken");
					const tl = np.tagLine();
					if (tl !== null && tl === tok.line) fail("UnexpectedToken");
					const anchor = np.hasAnchor;
					np.hasAnchor = null;
					node = this.parseCollection(E_ARR, anchor, BODY_BLOCK_SEQUENCE);
					break node;
				}

				case T_MAP_KEY: {
					if (this.context() === FLOW_IN || this.context() === FLOW_KEY) fail("UnexpectedToken");
					// [195] each `?` sits at s-indent(n) (spaces only).
					if (this.tabAfterIndent) fail("TabIndentation");
					const mStart = tok.start, mIndent = tok.indent, mLine = tok.line;
					// A subsequent key of the enclosing mapping, which takes it from the `:`.
					if (currentMappingIndent !== null && currentMappingIndent === mIndent) {
						return this.parseBlockExplicitKey(mStart, mIndent, mLine);
					}
					// The first key of a new mapping, whose node exists before its key
					// is parsed so the key may alias it.
					const anchor = np.hasAnchor;
					np.hasAnchor = null;
					node = this.parseCollection(E_OBJ, anchor, BODY_EXPLICIT_KEY_MAPPING, mStart, mIndent, mLine, flowPairAllowed);
					break node;
				}

				case T_MAP_VALUE: {
					if (this.context() === FLOW_KEY) { node = NULL_EXPR; break node; }
					if (this.tabAfterIndent && this.context() !== FLOW_IN) fail("TabIndentation");
					if (currentMappingIndent !== null && currentMappingIndent === tok.indent) { node = NULL_EXPR; break node; }
					// [200]/[193] a property on the `:` line is the e-node key's.
					const colonLine = tok.line;
					const keyTag = np.tagLine() === colonLine ? np.takeTag() : TAG_NONE;
					const firstKey = resolveNull(keyTag);
					const anchors = np.takeImplicitKeyAnchors(colonLine);
					if (anchors.keyAnchor !== null) this.bindAnchor(anchors.keyAnchor, firstKey);
					node = this.parseBlockMapping(anchors.mappingAnchor, firstKey, tok.start, tok.indent, colonLine, flowPairAllowed);
					break node;
				}

				case T_SCALAR: {
					const sStart = tok.start, sIndent = tok.indent, sLine = tok.line;
					const sTabAfterIndent = this.tabAfterIndent;
					const scalar = tok.scalar;
					const jsonKey = scalar.style === S_QUOTED ? this.maybeSetJsonKey(flowPairAllowed) : false;
					this.scan(scanOpts({ tag: np.tag(), outsideContext: true }));
					this.unsetJsonKey(jsonKey);
					if (this.token.k === T_MAP_VALUE) {
						// maybe the start of a new mapping with an implicit key
						if (this.token.indent < sIndent
							|| (explicitMappingKey && sLine !== this.token.line && this.token.indent <= sIndent)) {
							node = scalar.data;
							break node;
						}
						// [192] the key is at s-indent(n) (spaces only).
						if (sTabAfterIndent && (this.context() === BLOCK_OUT || this.context() === BLOCK_IN)) fail("TabIndentation");
						if (currentMappingIndent !== null && currentMappingIndent === sIndent) { node = scalar.data; break node; }
						if (this.context() === FLOW_KEY) { node = scalar.data; break node; }
						if (sLine !== this.token.line && !explicitMappingKey) fail("MultilineImplicitKey");
						// [147] flow-map value is ns-flow-node, not a pair.
						if (this.context() === FLOW_IN && !flowPairAllowed) { node = scalar.data; break node; }
						const implicitKey = scalar.data;
						const anchors = np.takeImplicitKeyAnchors(sLine);
						if (anchors.keyAnchor !== null) this.bindAnchor(anchors.keyAnchor, implicitKey);
						return this.parseBlockMapping(anchors.mappingAnchor, implicitKey, sStart, sIndent, sLine, flowPairAllowed);
					}
					node = scalar.data;
					break node;
				}

				case T_DIRECTIVE:
				case T_RESERVED:
					fail("UnexpectedToken");
			}
		}

		// Whatever was not claimed above belongs to `node`.
		const tag = np.tag();
		if (np.hasMappingAnchor !== null) fail("MultipleAnchors");
		if (np.hasMappingTag !== null) fail("MultipleTags");
		const resolved = node.t === E_NULL ? resolveNull(tag) : node;
		if (np.hasAnchor !== null) this.bindAnchor(np.hasAnchor, resolved);
		return resolved;
	}

	foldLines() {
		let total = 0;
		let c = this.next();
		for (;;) {
			if (c === 0x0d) {
				if (this.peek(1) === 0x0a) this.inc(1);
				c = 0x0a;
				continue;
			}
			if (c === 0x0a) {
				total++;
				this.newline();
				this.inc(1);
				c = this.next();
				continue;
			}
			if (c === 0x20) {
				let indent = 1;
				this.inc(1);
				while (this.next() === 0x20) { this.inc(1); indent++; }
				this.lineIndent = indent;
				if (this.next() === 0x09) this.tabAfterIndent = true;
				this.skipSWhite();
				c = this.next();
				continue;
			}
			if (c === 0x09) {
				this.tabAfterIndent = true;
				this.inc(1);
				this.skipSWhite();
				c = this.next();
				continue;
			}
			return total;
		}
	}

	scanPlainScalar(opts) {
		const ctx = new ScalarCtx(this, opts.tag);
		const s = this.s;
		let c = this.next();
		for (;;) {
			switch (c) {
				case 0:
					if (!this.isEof()) fail("UnexpectedCharacter");
					return ctx.done();
				case 0x2d:	// -
					// [203] c-directives-end is line-starting at column 0.
					if (this.isAtLineStart() && this.lineIndent === 0 && this.startsWith("---") && this.isAnyOrEofAt(3)) return ctx.done();
					if (!ctx.resolved && ctx.str.length === 0) {
						ctx.appendSource(this, "-");
						this.inc(1);
						ctx.tryResolveNumber(this, FC_NEGATIVE);
						c = this.next();
						continue;
					}
					ctx.appendSource(this, "-");
					this.inc(1);
					c = this.next();
					continue;
				case 0x2e:	// .
					if (this.isAtLineStart() && this.lineIndent === 0 && this.startsWith("...") && this.isAnyOrEofAt(3)) return ctx.done();
					if (!ctx.resolved && ctx.str.length === 0) {
						const n = this.peek(1);
						if (n === 0x6e || n === 0x4e || n === 0x69 || n === 0x49) {
							ctx.appendSource(this, ".");
							this.inc(1);
							ctx.tryResolveNumber(this, FC_DOT);
						} else {
							ctx.tryResolveNumber(this, FC_OTHER);
						}
						c = this.next();
						continue;
					}
					ctx.appendSource(this, ".");
					this.inc(1);
					c = this.next();
					continue;
				case 0x3a:	// :
					if (this.isSWhiteOrBCharOrEofAt(1)) return ctx.done();
					if (this.context() === FLOW_IN || this.context() === FLOW_KEY) {
						// [130] in flow, `:` before a flow indicator ends the scalar.
						if (isFlowIndicator(this.peek(1))) return ctx.done();
					}
					ctx.appendSource(this, ":");
					this.inc(1);
					c = this.next();
					continue;
				case 0x23:	// #
					if (this.isAtLineStart() || isWsOrBreak(s.charCodeAt(this.pos - 1))) return ctx.done();
					ctx.appendSource(this, "#");
					this.inc(1);
					c = this.next();
					continue;
				case 0x2c: case 0x5b: case 0x5d: case 0x7b: case 0x7d:
					if (this.context() === FLOW_IN || this.context() === FLOW_KEY) return ctx.done();
					ctx.appendSource(this, s[this.pos]);
					this.inc(1);
					c = this.next();
					continue;
				case 0x20: case 0x09:
					ctx.appendSourceWhitespace(s[this.pos]);
					this.inc(1);
					c = this.next();
					continue;
				case 0x0d:
					if (this.peek(1) === 0x0a) this.inc(1);
					c = 0x0a;
					continue;
				case 0x0a: {
					this.newline();
					this.inc(1);
					const lines = this.foldLines();
					const bi = this.blockIndent();
					// end here when the next line is not more indented: a new value.
					if (bi !== null && !(this.lineIndent > bi)) return ctx.done();
					// clear the trailing whitespace before the newline.
					ctx.ws = "";
					if (lines === 0 && !this.isEof()) ctx.appendWhitespace(" ");
					ctx.appendWhitespaceNTimes("\n", lines);
					c = this.next();
					continue;
				}
				default: {
					const ch = s[this.pos];
					if (ctx.resolved || ctx.str.length !== 0) {
						this.inc(1);
						ctx.appendSource(this, ch);
						c = this.next();
						continue;
					}
					// first non-whitespace
					if (c === 0x6e) {	// n
						this.inc(1);
						if (this.startsWith("ull")) { ctx.resolve(NULL_EXPR, "null"); this.inc(3); }
						else ctx.appendSource(this, ch);
					} else if (c === 0x4e) {	// N
						this.inc(1);
						if (this.startsWith("ull")) { ctx.resolve(NULL_EXPR, "Null"); this.inc(3); }
						else if (this.startsWith("ULL")) { ctx.resolve(NULL_EXPR, "NULL"); this.inc(3); }
						else ctx.appendSource(this, ch);
					} else if (c === 0x7e) {	// ~
						this.inc(1);
						ctx.resolve(NULL_EXPR, "~");
					} else if (c === 0x74) {	// t
						this.inc(1);
						if (this.startsWith("rue")) { ctx.resolve(boolExpr(true), "true"); this.inc(3); }
						else ctx.appendSource(this, ch);
					} else if (c === 0x54) {	// T
						this.inc(1);
						if (this.startsWith("rue")) { ctx.resolve(boolExpr(true), "True"); this.inc(3); }
						else if (this.startsWith("RUE")) { ctx.resolve(boolExpr(true), "TRUE"); this.inc(3); }
						else ctx.appendSource(this, ch);
					} else if (c === 0x66) {	// f
						this.inc(1);
						if (this.startsWith("alse")) { ctx.resolve(boolExpr(false), "false"); this.inc(4); }
						else ctx.appendSource(this, ch);
					} else if (c === 0x46) {	// F
						this.inc(1);
						if (this.startsWith("alse")) { ctx.resolve(boolExpr(false), "False"); this.inc(4); }
						else if (this.startsWith("ALSE")) { ctx.resolve(boolExpr(false), "FALSE"); this.inc(4); }
						else ctx.appendSource(this, ch);
					} else if (c === 0x2b) {	// +
						ctx.appendSource(this, "+");
						this.inc(1);
						ctx.tryResolveNumber(this, FC_POSITIVE);
					} else if (c >= 0x30 && c <= 0x39) {
						ctx.tryResolveNumber(this, FC_OTHER);
					} else {
						this.inc(1);
						ctx.appendSource(this, ch);
					}
					c = this.next();
					continue;
				}
			}
		}
	}

	isAnyOrEofAt(n) {	// is_any_or_eof_at(" \t\n\r", n)
		const p = this.pos + n;
		return p < this.len ? isWsOrBreak(this.s.charCodeAt(p)) : true;
	}

	// Returns [indentIndicator (0 = auto), chomp].
	scanBlockHeader() {
		let indentIndicator = null, chomp = null;
		let c = this.next();
		for (;;) {
			if (c === 0) {
				if (!this.isEof()) fail("UnexpectedCharacter");
				return [indentIndicator ?? 0, chomp ?? CLIP];
			}
			if (c >= 0x31 && c <= 0x39) {
				if (indentIndicator !== null) fail("UnexpectedCharacter");
				indentIndicator = c - 0x30;
				this.inc(1);
				c = this.next();
				continue;
			}
			if (c === 0x2d || c === 0x2b) {
				if (chomp !== null) fail("UnexpectedCharacter");
				chomp = c === 0x2d ? STRIP : KEEP;
				this.inc(1);
				c = this.next();
				continue;
			}
			if (c === 0x20 || c === 0x09) {
				this.inc(1);
				this.skipSWhite();
				if (this.next() === 0x23) {
					this.inc(1);
					while (!this.isBCharOrEof()) {
						if (this.next() === 0) fail("UnexpectedCharacter");
						this.inc(1);
					}
				}
				c = this.next();
				continue;
			}
			if (c === 0x0d) {
				if (this.peek(1) === 0x0a) this.inc(1);
				c = 0x0a;
				continue;
			}
			if (c === 0x0a) {
				// the first newline is always excluded from a literal
				this.newline();
				this.inc(1);
				if (this.next() === 0x09) fail("TabIndentation");
				return [indentIndicator ?? 0, chomp ?? CLIP];
			}
			fail("UnexpectedCharacter");
		}
	}

	scanAutoIndentedLiteralScalar(indentIndicator, chomp, folded, start, line) {
		const s = this.s;
		const explicitIndent = indentIndicator === 0 ? null : (this.blockIndent() ?? 0) + indentIndicator;
		const ctx = {
			text: "",
			leadingNewlines: 0,
			contentIndent: explicitIndent ?? 0,
			maxLeadingIndent: 0,
			prevMoreIndented: false,
			curMoreIndented: false,
		};
		const done = () => {
			// [165] an EOF right after the last content line counts as its break.
			if (ctx.text.length !== 0 && ctx.leadingNewlines === 0) ctx.leadingNewlines = 1;
			if (chomp === KEEP) ctx.text += "\n".repeat(ctx.leadingNewlines);
			else if (chomp === CLIP && ctx.text.length !== 0) ctx.text += "\n";
			return { k: T_SCALAR, start, indent: ctx.contentIndent, line, scalar: { data: strExpr(ctx.text), style: S_BLOCK, multiline: false } };
		};
		const append = (ch) => {
			if (ctx.text.length === 0) {
				if (explicitIndent === null && ctx.contentIndent < ctx.maxLeadingIndent) fail("UnexpectedCharacter");
				ctx.text += "\n".repeat(ctx.leadingNewlines) + ch;
				ctx.leadingNewlines = 0;
				ctx.prevMoreIndented = ctx.curMoreIndented;
				return;
			}
			if (ctx.leadingNewlines === 0) { ctx.text += ch; return; }
			// First content after line breaks: [73]/[74] breaks next to
			// more-indented lines are not folded.
			if (folded && !ctx.prevMoreIndented && !ctx.curMoreIndented) {
				ctx.text += ctx.leadingNewlines === 1 ? " " : "\n".repeat(ctx.leadingNewlines - 1);
			} else {
				ctx.text += "\n".repeat(ctx.leadingNewlines);
			}
			ctx.text += ch;
			ctx.leadingNewlines = 0;
			ctx.prevMoreIndented = ctx.curMoreIndented;
		};

		// Phase 1: find content_indent and the first content char.
		let consumedIndentThisLine = false;
		let contentIndent, first;
		phase1: for (;;) {
			const c = this.next();
			switch (c) {
				case 0:
					if (!this.isEof()) fail("UnexpectedCharacter");
					if (consumedIndentThisLine) ctx.leadingNewlines++;
					if (explicitIndent === null) ctx.contentIndent = this.lineIndent;
					return done();
				case 0x0d:
				case 0x0a:
					if (c === 0x0d && this.peek(1) === 0x0a) this.inc(1);
					this.newline();
					this.inc(1);
					if (this.next() === 0x09) fail("TabIndentation");
					ctx.leadingNewlines++;
					consumedIndentThisLine = false;
					continue;
				case 0x20: {
					let indent = 1;
					this.inc(1);
					if (explicitIndent !== null) {
						while (indent < explicitIndent && this.next() === 0x20) { indent++; this.inc(1); }
						this.lineIndent = indent;
						if (!(indent < explicitIndent)) consumedIndentThisLine = true;
						const n = this.next();
						if (n === 0 || n === 0x0a || n === 0x0d) continue;
						contentIndent = explicitIndent;
						first = n;
						break phase1;
					}
					consumedIndentThisLine = true;
					while (this.next() === 0x20) { indent++; this.inc(1); }
					if (ctx.maxLeadingIndent < indent) ctx.maxLeadingIndent = indent;
					this.lineIndent = indent;
					continue;
				}
				default:
					contentIndent = explicitIndent !== null ? explicitIndent : this.lineIndent;
					first = c;
					break phase1;
			}
		}
		ctx.contentIndent = contentIndent;
		ctx.curMoreIndented = first === 0x20 || first === 0x09;
		if (first === 0x09) this.tabAfterIndent = true;

		// A body line is indented >= content_indent and > the parent block's indent.
		const bi = this.blockIndent();
		const minIndent = bi !== null ? Math.max(contentIndent, bi + 1) : contentIndent;

		// Phase 2: the body.
		let c = first;
		for (;;) {
			if (c === 0) {
				if (!this.isEof()) fail("UnexpectedCharacter");
				return done();
			}
			if (c === 0x0d) {
				if (this.peek(1) === 0x0a) this.inc(1);
				c = 0x0a;
				continue;
			}
			if (c === 0x0a) {
				ctx.leadingNewlines++;
				this.newline();
				this.inc(1);
				for (;;) {
					const nc = this.next();
					if (nc === 0x0d || nc === 0x0a) {
						if (nc === 0x0d && this.peek(1) === 0x0a) this.inc(1);
						ctx.leadingNewlines++;
						this.newline();
						this.inc(1);
						if (this.next() === 0x09) fail("TabIndentation");
						continue;
					}
					if (nc === 0x20) {
						let indent = 0;
						while (indent < ctx.contentIndent && this.next() === 0x20) { indent++; this.inc(1); }
						this.lineIndent = indent;
						const n2 = this.next();
						ctx.curMoreIndented = n2 === 0x20 || n2 === 0x09;
						if (n2 === 0x09) this.tabAfterIndent = true;
						c = n2;
						break;
					}
					ctx.curMoreIndented = nc === 0x09;
					if (nc === 0x09) this.tabAfterIndent = true;
					c = nc;
					break;
				}
				continue;
			}
			if (c === 0x2d) {
				if (this.isAtLineStart() && this.lineIndent === 0 && this.startsWith("---") && this.isAnyOrEofAt(3)) return done();
				if (this.lineIndent < minIndent) return done();
				append("-");
				this.inc(1);
				c = this.next();
				continue;
			}
			if (c === 0x2e) {
				if (this.isAtLineStart() && this.lineIndent === 0 && this.startsWith("...") && this.isAnyOrEofAt(3)) return done();
				if (this.lineIndent < minIndent) return done();
				append(".");
				this.inc(1);
				c = this.next();
				continue;
			}
			if (this.lineIndent < minIndent) return done();
			append(s[this.pos]);
			this.inc(1);
			c = this.next();
		}
	}

	scanSingleQuotedScalar() {
		const start = this.pos, scalarLine = this.line, scalarIndent = this.lineIndent;
		let text = "";
		for (;;) {
			const c = this.next();
			switch (c) {
				case 0: fail("UnexpectedCharacter"); break;
				case 0x2e:
					if (this.isAtLineStart() && this.startsWith("...") && this.isSWhiteOrBCharAt(3)) fail("UnexpectedDocumentEnd");
					text += ".";
					this.inc(1);
					break;
				case 0x2d:
					if (this.isAtLineStart() && this.startsWith("---") && this.isSWhiteOrBCharAt(3)) fail("UnexpectedDocumentStart");
					text += "-";
					this.inc(1);
					break;
				case 0x0d:
				case 0x0a: {
					// A CR is one break by itself here, so CRLF folds to "\n".
					this.newline();
					this.inc(1);
					const lines = this.foldLines();
					text += lines === 0 ? " " : "\n".repeat(lines);
					const bi = this.blockIndent();
					if (bi !== null && this.lineIndent <= bi) fail("UnexpectedCharacter");
					break;
				}
				case 0x20:
				case 0x09: {
					const off = this.pos;
					this.inc(1);
					this.skipSWhite();
					if (!this.isBChar()) text += this.s.slice(off, this.pos);
					break;
				}
				case 0x27:
					this.inc(1);
					if (this.next() === 0x27) { text += "'"; this.inc(1); break; }
					return { k: T_SCALAR, start, indent: scalarIndent, line: scalarLine, scalar: { data: strExpr(text), style: S_QUOTED, multiline: false } };
				default:
					text += this.s[this.pos];
					this.inc(1);
			}
		}
	}

	scanDoubleQuotedScalar() {
		const start = this.pos, scalarLine = this.line, scalarIndent = this.lineIndent;
		let text = "";
		for (;;) {
			const c = this.next();
			switch (c) {
				case 0: fail("UnexpectedCharacter"); break;
				case 0x2e:
					if (this.isAtLineStart() && this.startsWith("...") && this.isSWhiteOrBCharAt(3)) fail("UnexpectedDocumentEnd");
					text += ".";
					this.inc(1);
					break;
				case 0x2d:
					if (this.isAtLineStart() && this.startsWith("---") && this.isSWhiteOrBCharAt(3)) fail("UnexpectedDocumentStart");
					text += "-";
					this.inc(1);
					break;
				case 0x0d:
				case 0x0a: {
					this.newline();
					this.inc(1);
					const lines = this.foldLines();
					text += lines === 0 ? " " : "\n".repeat(lines);
					const bi = this.blockIndent();
					if (bi !== null && this.lineIndent <= bi) fail("UnexpectedCharacter");
					break;
				}
				case 0x20:
				case 0x09: {
					const off = this.pos;
					this.inc(1);
					this.skipSWhite();
					if (!this.isBChar()) text += this.s.slice(off, this.pos);
					break;
				}
				case 0x22:
					this.inc(1);
					return { k: T_SCALAR, start, indent: scalarIndent, line: scalarLine, scalar: { data: strExpr(text), style: S_QUOTED, multiline: false } };
				case 0x5c: {
					this.inc(1);
					const e = this.next();
					if (e === 0x0d || e === 0x0a) {	// escaped line break
						this.newline();
						this.inc(1);
						const lines = this.foldLines();
						const bi = this.blockIndent();
						if (bi !== null && this.lineIndent <= bi) fail("UnexpectedCharacter");
						text += "\n".repeat(lines);
						this.skipSWhite();
						break;
					}
					switch (e) {
						case 0x20: text += " "; break;
						case 0x09: text += "\t"; break;
						case 0x30: text += "\0"; break;
						case 0x61: text += "\x07"; break;
						case 0x62: text += "\b"; break;
						case 0x74: text += "\t"; break;
						case 0x6e: text += "\n"; break;
						case 0x76: text += "\v"; break;
						case 0x66: text += "\f"; break;
						case 0x72: text += "\r"; break;
						case 0x65: text += "\x1b"; break;
						case 0x22: text += "\""; break;
						case 0x2f: text += "/"; break;
						case 0x5c: text += "\\"; break;
						case 0x4e: text += "\xc2\x85"; break;	// UTF-8 of U+0085
						case 0x5f: text += "\xc2\xa0"; break;	// U+00A0
						case 0x4c: text += "\xe2\x80\xa8"; break;	// U+2028
						case 0x50: text += "\xe2\x80\xa9"; break;	// U+2029
						case 0x78: text += this.decodeHexCodePoint(ESC_X); break;
						case 0x75: text += this.decodeHexCodePoint(ESC_LOWER_U); break;
						case 0x55: text += this.decodeHexCodePoint(ESC_UPPER_U); break;
						default: fail("UnexpectedCharacter");
					}
					this.inc(1);
					break;
				}
				default:
					text += this.s[this.pos];
					this.inc(1);
			}
		}
	}

	readHexDigits(count) {
		let value = 0;
		for (let i = 0; i < count; i++) {
			this.inc(1);
			const d = hexValue(this.next());
			if (d < 0) fail("UnexpectedCharacter");
			value = value * 16 + d;	// at most 8 digits: exact, as the Rust u32
		}
		return value;
	}

	// Returns the code point as UTF-8 bytes (binary string).
	decodeHexCodePoint(escape) {
		let cp = this.readHexDigits(escape);
		if (cp > 0x10ffff) fail("UnexpectedCharacter");
		// JSON's `\uD8xx\uDCxx` pair is accepted; lone surrogates are errors.
		if (cp >= 0xd800 && cp <= 0xdfff) {
			if (escape !== ESC_LOWER_U || !(cp >= 0xd800 && cp <= 0xdbff) || this.peek(1) !== 0x5c || this.peek(2) !== 0x75) {
				fail("UnexpectedCharacter");
			}
			this.inc(2);
			const low = this.readHexDigits(ESC_LOWER_U);
			if (!(low >= 0xdc00 && low <= 0xdfff)) fail("UnexpectedCharacter");
			cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
		}
		return Buffer.from(String.fromCodePoint(cp), "utf8").toString("latin1");
	}

	// c-ns-tag-property; at the '!'.
	scanTagProperty() {
		const start = this.pos;
		const tagToken = (tag) => ({ k: T_TAG, start, indent: this.lineIndent, line: this.line, tag });
		this.inc(1);
		const c = this.next();
		if (c === 0 || c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) {
			return tagToken(TAG_NONSPECIFIC);	// c-non-specific-tag
		}
		if (c === 0x3c) {	// c-verbatim-tag
			this.inc(1);
			if (this.next() === 0x21) {
				this.inc(1);
				this.skipNsUriChars();
			} else {
				const l = this.nsTagCharLen();
				if (l === null) fail("UnexpectedCharacter");
				this.inc(l);
				this.skipNsUriChars();
			}
			this.trySkipChar(0x3e);
			return tagToken(TAG_VERBATIM);
		}
		if (c === 0x21) {	// c-ns-shorthand-tag / secondary tag handle
			this.inc(1);
			const off = this.pos;
			this.trySkipNsTagChars();
			const n = this.next();
			if (n === 0 || n === 0x20 || n === 0x09 || n === 0x0d || n === 0x0a) {
				// s-separate
			} else if (isFlowIndicator(n)) {
				if (this.context() === BLOCK_OUT || this.context() === BLOCK_IN) fail("UnexpectedCharacter");
			} else {
				fail("UnexpectedCharacter");
			}
			return tagToken(this.shorthandToTag(this.s.slice(off, this.pos)));
		}
		// c-ns-shorthand-tag / named tag handle
		const off = this.pos;
		this.trySkipNsWordChars();
		const handle = this.s.slice(off, this.pos);
		if (this.next() === 0x21) {
			this.inc(1);
			if (!this.tagHandles.has(handle)) {
				this.pos = off;
				fail("UnresolvedTagHandle");
			}
			this.trySkipNsTagChars();
			return tagToken(TAG_UNKNOWN);
		}
		// primary
		this.skipNsTagChars();
		return tagToken(this.shorthandToTag(this.s.slice(off, this.pos)));
	}

	shorthandToTag(s) {
		switch (s) {
			case "bool": return TAG_BOOL;
			case "int": return TAG_INT;
			case "float": return TAG_FLOAT;
			case "null": return TAG_NULL;
			case "str": return TAG_STR;
			default: return TAG_UNKNOWN;
		}
	}

	scan(opts) {
		let countIndentation = opts.firstScan || opts.additionalParentIndent !== null;
		// Still in leading whitespace, where a tab taints tab_after_indent.
		let inIndentPosition = countIndentation;
		if (inIndentPosition) this.tabAfterIndent = false;
		let additionalParentIndent = opts.additionalParentIndent;
		const previousTokenLine = this.token.line;
		const s = this.s;
		let token;
		next: for (;;) {
			const c = this.next();
			const start = this.pos;
			switch (c) {
				case 0:
					// [1] c-printable excludes U+0000; NUL is also the EOF sentinel.
					if (!this.isEof()) fail("UnexpectedCharacter");
					token = { k: T_EOF, ...this.tokenInit(start) };
					break next;
				case 0x2d: {	// -
					if (this.isAtLineStart() && this.lineIndent === 0 && this.startsWith("---") && this.isSWhiteOrBCharOrEofAt(3)) {
						this.inc(3);
						token = { k: T_DOC_START, ...this.tokenInit(start) };
						break next;
					}
					const n = this.peek(1);
					if (n === 0 || n === 0x0a || n === 0x0d || n === 0x20 || n === 0x09) {
						this.inc(1);
						if (this.context() === FLOW_IN || this.context() === FLOW_KEY) fail("UnexpectedToken");
						token = { k: T_SEQ_ENTRY, ...this.tokenInit(start) };
						break next;
					}
					if (isFlowIndicator(n) && (this.context() === FLOW_IN || this.context() === FLOW_KEY)) {
						this.inc(1);
						fail("UnexpectedToken");
					}
					token = this.scanPlainScalar(opts);
					break next;
				}
				case 0x2e:	// .
					if (this.isAtLineStart() && this.lineIndent === 0 && this.startsWith("...") && this.isSWhiteOrBCharOrEofAt(3)) {
						this.inc(3);
						token = { k: T_DOC_END, ...this.tokenInit(start) };
						break next;
					}
					token = this.scanPlainScalar(opts);
					break next;
				case 0x3f: {	// ?
					const n = this.peek(1);
					if (n === 0 || n === 0x20 || n === 0x09 || n === 0x0a || n === 0x0d
						|| (isFlowIndicator(n) && (this.context() === FLOW_IN || this.context() === FLOW_KEY))) {
						this.inc(1);
						token = { k: T_MAP_KEY, ...this.tokenInit(start) };
						break next;
					}
					token = this.scanPlainScalar(opts);
					break next;
				}
				case 0x3a: {	// :
					const n = this.peek(1);
					if (n === 0 || n === 0x20 || n === 0x09 || n === 0x0a || n === 0x0d
						|| (isFlowIndicator(n) && (this.context() === FLOW_IN || this.context() === FLOW_KEY))
						|| (!isFlowIndicator(n) && this.context() === FLOW_KEY)) {
						this.inc(1);
						token = { k: T_MAP_VALUE, ...this.tokenInit(start) };
						break next;
					}
					token = this.scanPlainScalar(opts);
					break next;
				}
				case 0x2c:	// ,
					if (this.context() === FLOW_IN || this.context() === FLOW_KEY) {
						this.inc(1);
						token = { k: T_COLLECT, ...this.tokenInit(start) };
						break next;
					}
					token = this.scanPlainScalar(opts);
					break next;
				case 0x5b: this.inc(1); token = { k: T_SEQ_START, ...this.tokenInit(start) }; break next;
				case 0x5d: this.inc(1); token = { k: T_SEQ_END, ...this.tokenInit(start) }; break next;
				case 0x7b: this.inc(1); token = { k: T_MAP_START, ...this.tokenInit(start) }; break next;
				case 0x7d: this.inc(1); token = { k: T_MAP_END, ...this.tokenInit(start) }; break next;
				case 0x23:	// #
					if (!this.isAtLineStart() && !isWsOrBreak(s.charCodeAt(start - 1))) fail("UnexpectedCharacter");
					this.inc(1);
					while (!this.isBCharOrEof()) {
						if (this.next() === 0) fail("UnexpectedCharacter");
						this.inc(1);
					}
					continue;
				case 0x26:	// &
				case 0x2a: {	// *
					this.inc(1);
					const off = this.pos;
					this.trySkipNsAnchorChars();
					const t = { k: c === 0x26 ? T_ANCHOR : T_ALIAS, start, indent: this.lineIndent, line: this.line, name: s.slice(off, this.pos) };
					const n = this.next();
					if (n === 0 || n === 0x20 || n === 0x09 || n === 0x0a || n === 0x0d
						|| (isFlowIndicator(n) && (this.context() === FLOW_IN || this.context() === FLOW_KEY))) {
						token = t;
						break next;
					}
					fail("UnexpectedCharacter");
					break;
				}
				case 0x21:	// !
					token = this.scanTagProperty();
					break next;
				case 0x7c:	// |
				case 0x3e:	// >
					if (this.context() === BLOCK_OUT || this.context() === BLOCK_IN) {
						const indicatorIndent = this.lineIndent;
						this.inc(1);
						const lstart = this.pos, lline = this.line;
						const [ind, chomp] = this.scanBlockHeader();
						token = this.scanAutoIndentedLiteralScalar(ind, chomp, c === 0x3e, lstart, lline);
						// Token.indent of a block scalar is the indicator's s-indent.
						token.indent = indicatorIndent;
						break next;
					}
					fail("UnexpectedToken");
					break;
				case 0x27:	// '
					this.inc(1);
					token = this.scanSingleQuotedScalar();
					break next;
				case 0x22:	// "
					this.inc(1);
					token = this.scanDoubleQuotedScalar();
					break next;
				case 0x25:	// %
					this.inc(1);
					token = { k: T_DIRECTIVE, ...this.tokenInit(start) };
					break next;
				case 0x40:	// @
				case 0x60:	// `
					this.inc(1);
					this.token = { k: T_RESERVED, ...this.tokenInit(start) };
					fail("UnexpectedToken");
					break;
				case 0x0d:
				case 0x0a:
					if (c === 0x0d && this.peek(1) === 0x0a) this.inc(1);
					countIndentation = true;
					inIndentPosition = true;
					additionalParentIndent = null;
					this.newline();
					this.inc(1);
					continue;
				case 0x20: {
					let total = 1;
					this.inc(1);
					while (this.next() === 0x20) { this.inc(1); total++; }
					if (countIndentation) this.lineIndent = total + (additionalParentIndent ?? 0);
					countIndentation = false;
					continue;
				}
				case 0x09:
					if (countIndentation && additionalParentIndent === null && this.context() === BLOCK_IN) fail("TabIndentation");
					if (inIndentPosition) {
						// [63] s-indent is spaces only: taint, and make the token's
						// indent what the spaces reached.
						this.tabAfterIndent = true;
						additionalParentIndent = null;
					}
					countIndentation = false;
					this.inc(1);
					continue;
				default:
					token = this.scanPlainScalar(opts);
					break next;
			}
		}
		this.token = token;
		const ctx = this.context();
		if (ctx === FLOW_IN || ctx === FLOW_KEY) {
			const bi = this.blockIndent();
			if (bi !== null && !opts.outsideContext && this.token.line !== previousTokenLine
				&& this.token.indent <= bi && this.token.k !== T_SEQ_END && this.token.k !== T_MAP_END) {
				fail("UnexpectedToken");
			}
		}
	}
}

// ── YAMLObject.rs ──

// JSC's texts for the ToString/ToPrimitive TypeErrors that V8 words differently.
function jscTypeError(e) {
	if (e instanceof TypeError) {
		if (e.message === "Cannot convert object to primitive value") return new TypeError("No default value");
		if (e.message === "Cannot convert a Symbol value to a string") return new TypeError("Cannot convert a symbol to a string");
	}
	return e;
}
function toStringJSC(v) {
	try {
		return String(v === undefined ? "undefined" : typeof v === "symbol" ? `${v}` : v);
	} catch (e) {
		throw jscTypeError(e);
	}
}

// utf8_bytes_to_js: valid UTF-8 decodes as usual (the simdutf path); anything
// else goes through Bun's scalar WTF-8 loop, where an invalid or truncated
// sequence costs one U+FFFD per byte and an encoded surrogate is kept as is.
function decode(b) {
	if (!/[\x80-\xff]/.test(b)) return b;
	const buf = Buffer.from(b, "latin1");
	if (isUtf8(buf)) return buf.toString("utf8");
	const units = [];
	const n = buf.length;
	for (let i = 0; i < n;) {
		const c0 = buf[i];
		if (c0 < 0x80) { units.push(c0); i++; continue; }
		const width = c0 >= 0xc0 && c0 <= 0xdf ? 2 : c0 >= 0xe0 && c0 <= 0xef ? 3 : c0 >= 0xf0 && c0 <= 0xf7 ? 4 : 1;
		if (width === 1) { units.push(0xfffd); i++; continue; }
		// Missing trailing bytes read as 0, which fails the continuation test.
		const s1 = i + 1 < n ? buf[i + 1] : 0, s2 = width > 2 && i + 2 < n ? buf[i + 2] : 0, s3 = width > 3 && i + 3 < n ? buf[i + 3] : 0;
		let cp = -1;
		if ((s1 & 0xc0) === 0x80) {
			if (width === 2) {
				cp = ((c0 & 0x1f) << 6) | (s1 & 0x3f);
				if (cp < 0x80) cp = -1;
			} else if ((s2 & 0xc0) === 0x80) {
				if (width === 3) {
					cp = ((c0 & 0x0f) << 12) | ((s1 & 0x3f) << 6) | (s2 & 0x3f);
					if (cp < 0x800) cp = -1;
				} else if ((s3 & 0xc0) === 0x80) {
					cp = ((c0 & 0x07) << 18) | ((s1 & 0x3f) << 12) | ((s2 & 0x3f) << 6) | (s3 & 0x3f);
					if (cp < 0x10000 || cp > 0x10ffff) cp = -1;
				}
			}
		}
		if (cp < 0) { units.push(0xfffd); i++; continue; }
		if (cp < 0x10000) units.push(cp);
		else units.push(0xd800 + ((cp - 0x10000) >> 10), 0xdc00 + ((cp - 0x10000) & 0x3ff));
		i += width;
	}
	let out = "";
	for (let i = 0; i < units.length; i += 4096) out += String.fromCharCode.apply(null, units.slice(i, i + 4096));
	return out;
}

const define = (obj, key, value) =>
	Object.defineProperty(obj, key, { value, writable: true, enumerable: true, configurable: true });

// ParserCtx::to_js: collections are materialised once by identity, so shared
// and cyclic aliases come out as shared and cyclic JS objects. A property
// whose key is a collection, and every later one of that mapping, waits until
// the whole tree exists (PendingProperty), then the key is ToString'd.
function toJS(root) {
	const seen = new Map();
	const pending = [];
	const conv = (e) => {
		switch (e.t) {
			case E_NULL: return null;
			case E_BOOL: return e.v;
			case E_NUM: return e.v;
			case E_STR: return decode(e.s);
			case E_ARR: {
				let a = seen.get(e);
				if (a !== undefined) return a;
				a = [];
				seen.set(e, a);
				for (const item of e.items) a.push(conv(item));
				return a;
			}
			case E_OBJ: {
				let o = seen.get(e);
				if (o !== undefined) return o;
				o = {};
				seen.set(e, o);
				let isPending = false;
				for (const [k, v] of e.props) {
					const key = conv(k);
					const value = conv(v);
					isPending ||= key !== null && typeof key === "object";
					if (isPending) { pending.push([o, key, value]); continue; }
					define(o, String(key), value);
				}
				return o;
			}
		}
	};
	const result = conv(root);
	for (const [o, key, value] of pending) define(o, toStringJSC(key), value);
	return result;
}

// JsBytes: buffers and views are taken as bytes, anything else is ToString'd.
function inputBytes(input) {
	if (input instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer)) {
		return Buffer.from(input).toString("latin1");
	}
	if (ArrayBuffer.isView(input)) return Buffer.from(input.buffer, input.byteOffset, input.byteLength).toString("latin1");
	// A lone surrogate encodes as U+FFFD, as in Bun.
	return Buffer.from(toStringJSC(input), "utf8").toString("latin1");
}

const stackOverflow = () => new RangeError("Maximum call stack size exceeded.");
const isNodeStackOverflow = (e) => e instanceof RangeError && e.message === "Maximum call stack size exceeded";
// Thrown out of parseBytes when Node's stack, not the modelled one, ran out.
const NEEDS_DEEPER_STACK = Symbol("yaml-parse: needs a deeper stack");

function parseBytes(bytes, deep) {
	const p = new Parser(bytes);
	let docs;
	try {
		docs = p.parse();
	} catch (e) {
		if (e instanceof ParseError) {
			if (e.kind === "StackOverflow") throw stackOverflow();
			// AllocError (the merged-property budget): JsError::OutOfMemory.
			if (e.kind === "OutOfMemory") throw new RangeError("Out of memory");
			// UnexpectedCharacter at the end of the input reads as an EOF.
			const kind = e.kind === "UnexpectedCharacter" && p.pos >= p.len ? "UnexpectedEof" : e.kind;
			throw new SyntaxError("YAML Parse error: " + MESSAGES[kind]);
		}
		if (isNodeStackOverflow(e)) throw deep ? stackOverflow() : NEEDS_DEEPER_STACK;
		throw e;
	}
	// multi-document streams are converted into arrays
	const root = docs.length === 1 ? docs[0] : { t: E_ARR, items: docs };
	try {
		return toJS(root);
	} catch (e) {
		if (isNodeStackOverflow(e)) throw deep ? stackOverflow() : NEEDS_DEEPER_STACK;
		throw jscTypeError(e);
	}
}

// ── the deep-stack worker ──
// The result crosses back as a flat list of collections (refs as {r: index}),
// built and rebuilt without recursion, so even its structured clone is shallow.
// Identity, cycles, key order, -0 and NaN all survive.
const WORKER_MARK = "bun-yaml-parse-deep";
const ERRORS = { SyntaxError, RangeError, TypeError, Error };

function flatten(value) {
	const ids = new Map(), nodes = [];
	const ref = (v) => {
		if (v === null || typeof v !== "object") return v;
		let id = ids.get(v);
		if (id === undefined) { id = nodes.length; ids.set(v, id); nodes.push(v); }
		return { r: id };
	};
	const root = ref(value);
	const flat = [];
	for (let i = 0; i < nodes.length; i++) {
		const v = nodes[i];
		const f = [Array.isArray(v) ? 0 : 1];
		if (f[0] === 0) for (let j = 0; j < v.length; j++) f.push(ref(v[j]));
		else for (const k of Object.keys(v)) f.push(k, ref(v[k]));
		flat.push(f);
	}
	return { root, flat };
}

function unflatten({ root, flat }) {
	const objs = flat.map((f) => (f[0] === 0 ? [] : {}));
	const val = (x) => (x !== null && typeof x === "object" ? objs[x.r] : x);
	for (let i = 0; i < flat.length; i++) {
		const f = flat[i], o = objs[i];
		if (f[0] === 0) for (let j = 1; j < f.length; j++) o.push(val(f[j]));
		else for (let j = 1; j < f.length; j += 2) define(o, f[j], val(f[j + 1]));
	}
	return val(root);
}

function parseDeep(bytes) {
	const signal = new Int32Array(new SharedArrayBuffer(4));
	const { port1, port2 } = new MessageChannel();
	const worker = new Worker(new URL(import.meta.url), {
		workerData: { [WORKER_MARK]: true, bytes, signal, port: port2 },
		transferList: [port2],
		resourceLimits: { stackSizeMb: DEEP_STACK_MB },
	});
	worker.unref();
	// The worker posts its one message, then flips `signal`.
	Atomics.wait(signal, 0, 0);
	const m = receiveMessageOnPort(port1);
	port1.close();
	worker.terminate();
	if (!m) throw new Error("yaml-parse: deep-stack worker sent no result");
	const r = m.message;
	if ("err" in r) throw new (ERRORS[r.err[0]] || Error)(r.err[1]);
	return unflatten(r.ok);
}

function runWorker({ bytes, signal, port }) {
	let msg;
	try {
		msg = { ok: flatten(parseBytes(bytes, true)) };
	} catch (e) {
		msg = { err: [e && e.constructor ? e.constructor.name : "Error", e && e.message] };
	}
	port.postMessage(msg);
	Atomics.store(signal, 0, 1);
	Atomics.notify(signal, 0);
}

export function parse(input) {
	// The argument is ToString'd once, here, as by the JsBytes binding.
	const bytes = inputBytes(input);
	try {
		return parseBytes(bytes, false);
	} catch (e) {
		if (e === NEEDS_DEEPER_STACK) return parseDeep(bytes);
		throw e;
	}
}

if (!isMainThread && workerData && workerData[WORKER_MARK]) runWorker(workerData);
