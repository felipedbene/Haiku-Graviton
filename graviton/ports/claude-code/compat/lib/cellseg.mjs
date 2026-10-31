// Bun.ant.CellSegmenter -- the terminal-cell segmenter Claude Code's TUI renderer
// needs (chunk-pmgbk1rv.js d7n; consumers chunk-x65c4var.js class pf / w1 / xg,
// chunk-zy2k3hny.js UBt). It is native code in Anthropic's internal Bun
// (@anthropic-ai/bun-internal) with no public source. This is a contract port:
// every behaviour here was measured against the real runtime of the Claude Code
// 2.1.314 binary (test/gen-cellseg-vectors.mjs runs inside it) and is diff-tested
// by test/check-cellseg.mjs. The text side reuses the Bun ports already in this
// directory: ANSI recognition and grapheme clustering from slice.mjs (Bun's
// sliceAnsi), cluster widths from its GraphemeWidthState (Bun's stringWidth
// tables), SGR state from sgr.mjs.
//
// Contract (see PATCHES.md C23 for the full derivation):
//   new CellSegmenter({ambiguousIsNarrow, substitute, replacement, screen})
//   .graphemes / .sgrKeys / .sgrCloseKeys / .uris  live, append-only arrays
//   .segment(line, cells, runs, reordered) -> cell count, or -(capacity needed)
//       cells[2i] = grapheme index, cells[2i+1] = width | TAB | SUBST | run << 10
//       runs[2r]  = sgrKeys index,  runs[2r+1]  = uris index
//   .paint(screen, screenWidth, x, y, cells, count, order, charMap, words)
//   .setCell(screen, screenWidth, x, y, charIndex, word)
//       -> endColumn + damageStart * 2^20 + damageEnd * 2^36 (empty: 65535, 0)
import { createRequire } from "node:module";
import { GraphemeWidthState, decodeUTF16, clusterBreak } from "./slice.mjs";
import { fusedClassify } from "./width.mjs";
import { bidiClass } from "./bidi-classes.mjs";
import { isEscapeCharacter } from "./ansi.mjs";
import { SgrStyleState, applySgrToState, parseSgrParams, sgrCloseCode, sgrSlot, isSgrEndCode } from "./sgr.mjs";

// Cluster width. Bun's grapheme width rules (slice.mjs GraphemeWidthState, the
// stringWidth tables) with two measured differences: leading Prepend code
// points (U+0600 ...) do not become the cluster's base, the next code point
// does (they still count toward the cluster length); and when a variation
// selector decides the base's width, the widths of the cluster's other code
// points are still added ("\u2764\ufe0f\uff9e" is 3, stringWidth says 2).
const isPrepend = (cp) => (fusedClassify(cp) & 0x1f) === 1;
function clusterCpWidth(cps, amb) {
	let i = 0;
	while (i < cps.length && isPrepend(cps[i])) i++;
	if (i === cps.length) i = 0;
	const g = new GraphemeWidthState();
	g.reset(cps[i], amb);
	for (let j = i + 1; j < cps.length; j++) g.add(cps[j], amb);
	if (i > 0) g.count = Math.min(255, g.count + i);
	const w = g.width();
	if ((g.vs15 || g.vs16) && !g.regionalIndicator && !g.keycap && !(g.emojiBase && (g.skinTone || g.zwj)))
		return w + (g.nonEmojiWidth - g.baseWidth);
	return w;
}

// The segmenter's SGR state: Bun's sliceAnsi rules (sgr.mjs) with three
// measured differences. A sequence holding a parameter with no known meaning
// (999, 10, ...) is kept whole, as one entry, like a colon sequence. Re-applying
// a code a slot already holds leaves the slot where it is, except for such
// whole-sequence entries, which move last. Codes are always spelled ESC [.
const isKnownSgr = (c) => c === 0 || c === 38 || c === 48 || c === 58 || isSgrEndCode(c) || sgrCloseCode(c) !== 0;
function allKnown(P) {	// skipping the arguments of 38/48/58;5;n and ;2;r;g;b
	for (let i = 0; i < P.length; i++) {
		const c = P[i];
		if (!isKnownSgr(c)) return false;
		if ((c === 38 || c === 48 || c === 58) && i + 1 < P.length) {
			if (P[i + 1] === 5 && i + 2 < P.length) i += 2;
			else if (P[i + 1] === 2 && i + 4 < P.length) i += 4;
		}
	}
	return true;
}
class CellStyleState extends SgrStyleState {
	applyStart(slot, openCode, endCode, whole) {
		if (openCode.charCodeAt(0) === 0x9b) openCode = "\x1b[" + openCode.slice(1);
		const i = this.entries.findIndex((e) => e.slot === slot);
		if (!whole && i >= 0 && this.entries[i].openCode === openCode && !this.entries[i].whole) return;
		this.entries = this.entries.filter((e) => e.slot !== slot);
		this.entries.push({ slot, endCode, openCode, whole });
	}
	apply(s, a, b) {	// the CSI ... m at s[a, b)
		const isC1 = s.charCodeAt(a) === 0x9b;
		const params = parseSgrParams(s, isC1 ? a + 1 : a + 2, b - 1);
		if (params.hasColon || params.overflow || !allKnown(params.data)) {
			const first = params.data[0], close = sgrCloseCode(first);
			this.applyStart(sgrSlot(first), "\x1b[" + s.slice(isC1 ? a + 1 : a + 2, b), close ? "\x1b[" + close + "m" : "\x1b[0m", true);
			return;
		}
		applySgrToState(this, s, a, b);
	}
}

const TAB_FLAG = 256, SUBST_FLAG = 512, RUN_SHIFT = 10, MAX_RUNS = 4194304;
const NO_DAMAGE_START = 65535;
const P20 = 1048576, P36 = 68719476736;

function err(Ctor, code, msg) {
	const e = new Ctor(msg);
	e.code = code;
	return e;
}
const typeName = (v) => (v === null ? "null" : typeof v);

// "non-shared, fixed-length, 4-byte-aligned Int32Array"
function isPlainInt32Array(v) {
	if (!ArrayBuffer.isView(v) || Object.prototype.toString.call(v) !== "[object Int32Array]") return false;
	const b = v.buffer;
	if (typeof SharedArrayBuffer !== "undefined" && b instanceof SharedArrayBuffer) return false;
	if (b.resizable) return false;
	return true;
}
function checkInt32Array(v, name) {
	if (!isPlainInt32Array(v))
		throw err(TypeError, "ERR_INVALID_ARG_TYPE", `Expected ${name} to be a non-shared, fixed-length, 4-byte-aligned Int32Array for 'CellSegmenter'.`);
}
function overlaps(a, b) {
	if (a.buffer !== b.buffer || a.byteLength === 0 || b.byteLength === 0) return false;
	return a.byteOffset < b.byteOffset + b.byteLength && b.byteOffset < a.byteOffset + a.byteLength;
}

function checkInteger(v, name, lo, hi) {
	if (typeof v !== "number") throw err(TypeError, "ERR_INVALID_ARG_TYPE", `The "${name}" property must be of type number. Received ${typeName(v)}`);
	if (!Number.isInteger(v)) throw err(TypeError, "ERR_INVALID_ARG_TYPE", `The "${name}" property must be of type integer. Received number`);
	if (v < lo || v > hi) throw err(RangeError, "ERR_OUT_OF_RANGE", `The value of "${name}" is out of range. It must be >= ${lo} and <= ${hi}. Received ${v}`);
	return v;
}

const SCREEN_FIELDS = ["widthMask", "narrow", "wide", "spacerTail", "spacerHead", "emptyCharIndex", "spacerCharIndex", "emptyWord", "tabWidth"];
const SCREEN_DEFAULTS = { widthMask: 3, narrow: 0, wide: 1, spacerTail: 2, spacerHead: 3,
	emptyCharIndex: 0, spacerCharIndex: 1, emptyWord: 0, tabWidth: 8 };

// The initial grapheme table: " ", "", then printable ASCII, then TAB and the
// replacement. Indices 0 and 1 line up with the screen's empty/spacer chars.
function initialGraphemes(replacement) {
	const g = [" ", ""];
	for (let c = 0x21; c <= 0x7e; c++) g.push(String.fromCharCode(c));
	g.push("\t");
	if (!g.includes(replacement)) g.push(replacement);	// interned: no duplicate
	return g;
}

const INTERNAL = Symbol("CellSegmenter");
const state = (self, method) => {
	const st = self !== null && typeof self === "object" ? self[INTERNAL] : undefined;
	if (st === undefined)
		throw err(TypeError, "ERR_INVALID_THIS", `Expected this to be instanceof CellSegmenter, but received ${self === null || typeof self !== "object" ? typeName(self) : "an instance of " + (self.constructor?.name ?? "Object")}`);
	return st;
};

class Table {	// interned strings; `view` is the live array handed to JS
	constructor(initial) {
		this.items = initial.slice();
		this.index = new Map(initial.map((s, i) => [s, i]));
		this.view = initial.slice();
	}
	intern(s) {
		let i = this.index.get(s);
		if (i === undefined) {
			i = this.items.length;
			this.items.push(s);
			this.index.set(s, i);
			this.sync();
		}
		return i;
	}
	set(i, s) { this.items[i] = s; this.sync(); }
	// The JS array is extended as entries are interned (callers keep the array
	// itself, chunk-x65c4var.js pf caches it): every entry the array does not
	// have yet is appended. Measured: entries a caller pushed itself are kept
	// and not overwritten; a caller-truncated array is refilled from its length.
	sync() {
		const v = this.view;
		for (let i = v.length; i < this.items.length; i++) v.push(this.items[i]);
		return v;
	}
}

export function CellSegmenter(options) {
	if (!new.target) throw err(TypeError, "ERR_ILLEGAL_CONSTRUCTOR", "CellSegmenter constructor cannot be invoked without 'new'");
	if (options !== undefined && options !== null && typeof options !== "object" && typeof options !== "function")
		throw err(TypeError, "ERR_INVALID_ARG_TYPE", "Expected options to be a object for 'CellSegmenter'.");
	const o = options ?? {};
	const amb = o.ambiguousIsNarrow;
	const ambiguousIsNarrow = amb === undefined || amb === null ? true : Boolean(amb);
	const substitute = [];
	const sub = o.substitute;
	if (sub !== undefined && sub !== null) {
		if (!Array.isArray(sub))
			throw err(TypeError, "ERR_INVALID_ARG_TYPE", "Expected options.substitute to be a list of [first, last] pairs for 'CellSegmenter'.");
		for (const pair of sub) {
			if (!Array.isArray(pair) || pair.length < 2)
				throw err(TypeError, "ERR_INVALID_ARG_TYPE", "Expected options.substitute[] to be a [first, last] pair for 'CellSegmenter'.");
			const first = checkInteger(pair[0], "substitute[][0]", 128, 0x10ffff);
			const last = checkInteger(pair[1], "substitute[][1]", first, 0x10ffff);
			substitute.push([first, last]);
		}
	}
	let replacement = "�";
	const rep = o.replacement;
	if (rep !== undefined && rep !== null) {
		if (typeof rep !== "string") throw err(TypeError, "ERR_INVALID_ARG_TYPE", "Expected options.replacement to be a string for 'CellSegmenter'.");
		if (rep.length === 0) throw err(TypeError, "ERR_INVALID_ARG_TYPE", "CellSegmenter: options.replacement must not be empty");
		replacement = rep;
	}
	const screen = { ...SCREEN_DEFAULTS };
	const sc = o.screen;
	if (sc !== null && typeof sc === "object") {
		for (const k of SCREEN_FIELDS) {
			const v = sc[k];
			if (v === undefined) continue;
			screen[k] = checkInteger(v, k, -9007199254740991, 9007199254740991);
			if (v < -2147483648 || v > 2147483647)
				throw err(RangeError, "ERR_OUT_OF_RANGE", `The value of "${k}" is out of range. It must be >= -9007199254740991 and <= 9007199254740991. Received ${v}`);
		}
	}
	const graphemes = new Table(initialGraphemes(replacement));
	Object.defineProperty(this, INTERNAL, { value: {
		ambiguousIsNarrow, substitute, replacement, screen,
		replacementWidth: Math.max(1, clusterWidth(replacement, !ambiguousIsNarrow)),	// measured: "\t" is 1
		graphemes, sgrKeys: new Table([""]), sgrCloseKeys: new Table([""]), uris: new Table([""]),
		keyIndex: new Map([["", 0]]),
	} });
}

function clusterWidth(s, amb) {	// width of a string taken as one cluster
	const g = new GraphemeWidthState();
	for (let p = 0; p < s.length;) {
		const [cp, len] = decodeUTF16(s, p, s.length);
		if (p === 0) g.reset(cp, amb); else g.add(cp, amb);
		p += len;
	}
	return g.width();
}

function isSubstituted(st, cp) {
	for (const [a, b] of st.substitute) if (cp >= a && cp <= b) return true;
	return false;
}

// The style key for the current SGR state: open codes joined by NUL, and the
// matching close codes at the same positions.
function styleKey(st, entries) {
	if (entries.length === 0) return 0;
	const open = entries.map((e) => e.openCode).join("\x00");
	let k = st.keyIndex.get(open);
	if (k === undefined) {
		k = st.sgrKeys.intern(open);
		st.sgrCloseKeys.set(k, entries.map((e) => e.endCode).join("\x00"));
		st.keyIndex.set(open, k);
	}
	return k;
}

// Escape sequences, with the extents of Bun's consumeANSI (ansi.mjs: what
// stringWidth skips, unterminated sequences run to the end of the line), split
// into the individual sequences that machine passes through. Returns
// {end, kind, a, b}: kind "sgr" with the CSI at [a, end), or "osc" with the
// payload [a, b) and a proper terminator (BEL or ST), or "other".
function scanEscape(s, p, end) {
	let c = s.charCodeAt(p), q = p + 1;
	if (c === 0x1b) {
		if (q >= end) return { end: q, kind: "other" };
		const n = s.charCodeAt(q);
		if (n === 0x5b) { c = 0x9b; q++; }
		else if (n === 0x5d) { c = 0x9d; q++; }
		else if (n === 0x50 || n === 0x58 || n === 0x5e || n === 0x5f) { c = 0x90; q++; }
		else if (n === 0x1b) return { end: q, kind: "other" };	// ESC ESC: the first is absorbed
		else if (n === 0x18 || n === 0x1a || n === 0x9c) return { end: q + 1, kind: "other" };
		else if (n >= 0x20 && n <= 0x2f) {	// ESC intermediate final
			q++;
			if (q < end && s.charCodeAt(q) !== 0x1b) q++;
			return { end: q, kind: "other" };
		} else if (n >= 0x30 && n <= 0x7e) return { end: q + 1, kind: "other" };
		else return { end: q, kind: "other" };	// ESC alone; the next unit is text
	}
	if (c === 0x9b) {
		const a = q;
		for (; q < end; q++) {
			const u = s.charCodeAt(q);
			if (u >= 0x40 && u <= 0x7e) {
				let sgr = u === 0x6d;
				for (let i = a; sgr && i < q; i++) { const v = s.charCodeAt(i); if (!((v >= 0x30 && v <= 0x39) || v === 0x3b || v === 0x3a)) sgr = false; }
				return { end: q + 1, kind: sgr ? "sgr" : "other", a: p, b: q + 1 };
			}
			if (u === 0x1b) return { end: q, kind: "other" };
			if (u === 0x18 || u === 0x1a || u === 0x9c) return { end: q + 1, kind: "other" };
		}
		return { end, kind: "other" };
	}
	// OSC (BEL or ST ends it) or DCS/SOS/PM/APC (ST only)
	const osc = c === 0x9d, a = q;
	for (; q < end; q++) {
		const u = s.charCodeAt(q);
		if (osc && u === 0x07) return { end: q + 1, kind: osc ? "osc" : "other", a, b: q };
		if (u === 0x9c) return { end: q + 1, kind: osc ? "osc" : "other", a, b: q };
		if (u === 0x1b) {
			if (q + 1 < end && s.charCodeAt(q + 1) === 0x5c) return { end: q + 2, kind: osc ? "osc" : "other", a, b: q };
			return { end: q, kind: "other" };
		}
		if (u === 0x18 || u === 0x1a) return { end: q + 1, kind: "other" };
	}
	return { end, kind: "other" };
}

// OSC 8 ; params ; URI -> the URI ("" closes the link), or undefined.
function hyperlinkTarget(s, a, b) {
	if (b - a < 2 || s.charCodeAt(a) !== 0x38 || s.charCodeAt(a + 1) !== 0x3b) return undefined;
	const semi = s.indexOf(";", a + 2);
	if (semi < 0 || semi >= b) return undefined;
	return s.slice(semi + 1, b);
}

const isControl = (cp) => cp < 0x20 || (cp >= 0x7f && cp <= 0x9f);

// One line -> clusters [{text, firstCp, cps, width, key, uri, kind}] in logical
// order. Controls, TAB and substituted code points stand alone.
function clusters(st, line) {
	const out = [];
	const amb = !st.ambiguousIsNarrow;
	const sgr = new CellStyleState();
	let uri = 0, dirty = false, key = 0;
	const bs = { hasPrev: false, breakState: 0 };
	let prevCp = 0, cur = null;
	const flush = () => { if (cur) { cur.width = clusterCpWidth(cur.list, amb); out.push(cur); cur = null; } };
	// A cluster takes the style in effect at its first code point. The style's
	// key is interned when a cluster starts, even one too narrow to become a
	// cell, but not for a control character.
	const start = (cp, control) => {
		if (dirty && !control) { key = styleKey(st, sgr.entries); dirty = false; }
		cur = { text: "", cps: 0, firstCp: cp, key, uri, list: [] };
	};
	const end = line.length;
	let p = 0;
	while (p < end) {
		const c = line.charCodeAt(p);
		if (isEscapeCharacter(c)) {
			const t = scanEscape(line, p, end);
			if (t.kind === "sgr") { sgr.apply(line, t.a, t.b); dirty = true; }
			else if (t.kind === "osc") {
				const target = hyperlinkTarget(line, t.a, t.b);
				if (target !== undefined) uri = target === "" ? 0 : st.uris.intern(target);
			}
			p = t.end;
			continue;
		}
		const [cp, len] = decodeUTF16(line, p, end);
		if (cp >= 0xd800 && cp <= 0xdfff) { p += len; continue; }	// lone surrogate: skipped
		if (isControl(cp) || isSubstituted(st, cp)) {
			flush();
			bs.hasPrev = false; bs.breakState = 0;
			start(cp, isControl(cp) && cp !== 9);
			cur.text = line.slice(p, p + len); cur.cps = 1;
			cur.kind = cp === 9 ? "tab" : isControl(cp) ? "control" : "subst";
			out.push(cur); cur = null;
			p += len;
			continue;
		}
		if (clusterBreak(bs, prevCp, cp)) {
			flush();
			start(cp);
		}
		cur.text += line.slice(p, p + len);
		cur.cps++;
		cur.list.push(cp);
		prevCp = cp;
		bs.hasPrev = true;
		p += len;
	}
	flush();
	return out;
}

// Bidi reordering (segment(..., reordered=true), used when the terminal does
// not reorder itself: Windows Terminal, VS Code). Levels come from the Unicode
// Bidirectional Algorithm over the line's text with escapes and control
// characters removed and substituted code points as U+FFFD, paragraph
// direction from the first strong character; a cell takes the level of its
// cluster's first code unit. Cells are then reordered by rule L2 (reverse every
// maximal run at level >= L, for L from the highest level down to 1). Run
// indices stay those of logical order.
let bidi;
// bidi-js (npm, MIT; pinned in tools/deps.lock, installed next to ws): a
// UAX #9 implementation, loaded on the first reordered line.
function loadBidi() {
	const factory = createRequire(import.meta.url)("bidi-js");
	return (factory.default ?? factory)();
}
// bidi-js walks UTF-16 code units (a supplementary code point would be
// classified by its surrogate halves) and carries older Unicode data (U+1FA89
// is L there, ON in Unicode 17). So every code point is classified from
// bidi-classes.mjs (DerivedBidiClass 17.0.0, the version of the width tables)
// and, unless bidi-js already agrees, analysed as a one-unit BMP stand-in of
// that class. Offsets are therefore per code point.
const STAND_IN = { L: "a", R: "\u05d0", AL: "\u0627", EN: "0", ES: "+", ET: "#", AN: "\u0660", CS: ",", NSM: "\u0300",
	BN: "\u200b", B: "\u2029", S: "\t", WS: " ", ON: "!", LRE: "\u202a", LRO: "\u202d", RLE: "\u202b", RLO: "\u202e",
	PDF: "\u202c", LRI: "\u2066", RLI: "\u2067", FSI: "\u2068", PDI: "\u2069" };
function bidiText(s) {
	let out = "";
	for (const ch of s) {
		const c = bidiClass(ch.codePointAt(0));
		out += ch.length === 1 && bidi.getBidiCharTypeName(ch) === c ? ch : STAND_IN[c];
	}
	return out;
}
// The text one cluster contributes: none for a control, U+FFFD for a
// substituted code point, only the leading Prepend marks for a cluster that
// starts with them (measured: "\u0600\u0639" analyses as AN, not AL),
// else the cluster.
function bidiPart(c) {
	if (c.kind === "control") return "";
	if (c.kind === "subst") return "\ufffd";
	if (isPrepend(c.firstCp)) {
		let n = 0;
		while (n < c.list.length && isPrepend(c.list[n])) n++;
		return String.fromCodePoint(...c.list.slice(0, n));
	}
	return c.text;
}
// Lines without a strong right-to-left letter are left alone (RLM and ALM do
// not count).
function hasRtlLetter(s) {
	for (const ch of s) {
		const t = bidiClass(ch.codePointAt(0));
		if ((t === "R" || t === "AL") && ch !== "\u200f" && ch !== "\u061c") return true;
	}
	return false;
}
function bidiLevels(list) {	// -> level per cluster (controls: undefined), or null
	const raw = list.map(bidiPart);
	if (!hasRtlLetter(raw.join(""))) return null;
	bidi ??= loadBidi();
	const parts = raw.map(bidiText);
	const T = parts.join("");
	const res = bidi.getEmbeddingLevels(T, "auto");
	const levels = Array.from(res.levels);
	// A boundary neutral takes the level of the next character that is not
	// one (the paragraph level at the end); bidi-js gives it the previous one's.
	const para = res.paragraphs[0]?.level ?? 0;
	for (let i = T.length - 1, next = para; i >= 0; i--) {
		if (bidiClass(T.charCodeAt(i)) === "BN") levels[i] = next;
		else next = levels[i];
	}
	const out = [];
	let p = 0;
	for (let i = 0; i < list.length; i++) { out.push(parts[i] ? levels[p] : undefined); p += parts[i].length; }
	return out;
}
function reorderByLevels(items, lv) {	// UAX #9 L2, in place
	let max = 0;
	for (const l of lv) if (l > max) max = l;
	for (let L = max; L >= 1; L--) {
		let x = 0;
		while (x < items.length) {
			if (lv[x] >= L) {
				let y = x + 1;
				while (y < items.length && lv[y] >= L) y++;
				for (let a = x, b = y - 1; a < b; a++, b--) { [items[a], items[b]] = [items[b], items[a]]; [lv[a], lv[b]] = [lv[b], lv[a]]; }
				x = y;
			} else x++;
		}
	}
}

CellSegmenter.prototype.segment = function segment(line, cells, runs, reordered) {
	const st = state(this, "segment");
	if (typeof line !== "string") throw err(TypeError, "ERR_INVALID_ARG_TYPE", "Expected line to be a string for 'segment'.");
	checkInt32Array(cells, "cells");
	checkInt32Array(runs, "runs");
	if (overlaps(cells, runs)) throw err(TypeError, "ERR_INVALID_ARG_TYPE", "segment: cells and runs must not overlap");
	// Cells and runs are written as they are produced. The first one that does
	// not fit stops all writing and interning; the rest of the line is only
	// counted, and the caller gets -count back (a run always has a cell, so
	// count is enough for both arrays).
	const cellCap = cells.length >> 1, runCap = runs.length >> 1;
	let count = 0, nruns = 0, lastKey = 0, lastUri = 0, full = false;
	const list = clusters(st, line);
	const levels = reordered ? bidiLevels(list) : null;
	const pending = levels ? [] : null;	// reordered: [g, info, level], written at the end
	for (let i = 0; i < list.length; i++) {
		const cl = list[i];
		let text, info;
		if (cl.kind === "tab") { text = "\t"; info = TAB_FLAG; }
		else if (cl.kind === "subst") { text = st.replacement; info = st.replacementWidth | SUBST_FLAG; }
		else if (cl.kind === "control") continue;
		else {
			if (cl.width === 0) continue;
			text = cl.text;
			info = cl.width;
		}
		if (full) { count++; continue; }
		const g = st.graphemes.intern(text);
		if (nruns === 0 || lastKey !== cl.key || lastUri !== cl.uri) {
			if (nruns >= MAX_RUNS) throw err(RangeError, "ERR_OUT_OF_RANGE", `CellSegmenter.segment: the line produces more than ${MAX_RUNS} style runs`);
			if (nruns >= runCap) { full = true; count++; continue; }
			runs[2 * nruns] = cl.key; runs[2 * nruns + 1] = cl.uri;
			lastKey = cl.key; lastUri = cl.uri;
			nruns++;
		}
		if (count >= cellCap) { full = true; count++; continue; }
		if (pending) pending.push([g, info | ((nruns - 1) << RUN_SHIFT), i]);
		else { cells[2 * count] = g; cells[2 * count + 1] = info | ((nruns - 1) << RUN_SHIFT); }
		count++;
	}
	if (pending && full) {	// measured: the cells that fit, in logical order
		for (let j = 0; j < pending.length; j++) { cells[2 * j] = pending[j][0]; cells[2 * j + 1] = pending[j][1]; }
	} else if (pending) {
		// L2 over every cluster that has a level (cells and zero-width ones
		// alike: a dropped cluster still separates runs), then keep the cells.
		const idx = [], lv = [];
		for (let i = 0; i < list.length; i++) if (levels[i] !== undefined) { idx.push(i); lv.push(levels[i]); }
		reorderByLevels(idx, lv);
		const pos = new Map(idx.map((v, k) => [v, k]));
		pending.sort((x, y) => pos.get(x[2]) - pos.get(y[2]));
		for (let j = 0; j < pending.length; j++) { cells[2 * j] = pending[j][0]; cells[2 * j + 1] = pending[j][1]; }
	}
	return full ? -count : count;
};

// --- screen writes -----------------------------------------------------------
// A screen is width*height cells of [charIndex, word]; word & widthMask is the
// cell kind (narrow / wide / spacerTail / spacerHead).
class Writer {
	constructor(sc, screen, width, y) {
		this.sc = sc; this.screen = screen; this.width = width;
		const rows = width > 0 ? Math.floor(screen.length / 2 / width) : 0;
		this.ok = y >= 0 && y < rows && width > 0;
		this.base = y * width;
		this.lo = NO_DAMAGE_START; this.hi = 0;
	}
	touch(x) { if (x < this.lo) this.lo = x; if (x + 1 > this.hi) this.hi = x + 1; }
	kind(x) { return this.screen[2 * (this.base + x) + 1] & this.sc.widthMask; }
	raw(x, ch, word) {
		const i = 2 * (this.base + x);
		this.screen[i] = ch; this.screen[i + 1] = word;
		this.touch(x);
	}
	clear(x) { this.raw(x, this.sc.emptyCharIndex, this.sc.emptyWord); }
	// One cell, with the cleanup a wide character's halves need.
	put(x, ch, word) {
		if (!this.ok || x < 0 || x >= this.width) return;
		const sc = this.sc, kind = word & sc.widthMask, old = this.kind(x);
		if (old === sc.wide && kind !== sc.wide && x + 1 < this.width && this.kind(x + 1) === sc.spacerTail) this.clear(x + 1);
		if (old === sc.spacerTail && kind !== sc.spacerTail && x > 0 && this.kind(x - 1) === sc.wide) this.clear(x - 1);
		this.raw(x, ch, word);
		if (kind === sc.wide && x + 1 < this.width) {
			if (this.kind(x + 1) === sc.wide && x + 2 < this.width && this.kind(x + 2) === sc.spacerTail) this.clear(x + 2);
			this.raw(x + 1, sc.spacerCharIndex, sc.emptyWord | sc.spacerTail);
		}
	}
	result(endCol) {
		const lo = this.lo, hi = this.hi;
		return Math.max(0, Math.min(endCol, P20 - 1)) + (hi > 0 ? lo * P20 + hi * P36 : NO_DAMAGE_START * P20);
	}
}

const toInt32 = (v) => Number(v) | 0;

CellSegmenter.prototype.paint = function paint(screen, screenWidth, x, y, cells, count, order, charMap, words) {
	const st = state(this, "paint");
	checkInt32Array(screen, "screen");
	screenWidth = checkInteger(screenWidth, "screenWidth", 0, 9007199254740991);
	x = toInt32(x); y = toInt32(y);
	checkInt32Array(cells, "cells");
	count = checkInteger(count, "count", 0, 9007199254740991);
	if (order !== undefined && order !== null) checkInt32Array(order, "order");
	else order = undefined;
	checkInt32Array(charMap, "charMap");
	checkInt32Array(words, "words");
	if (count > cells.length >> 1) throw err(TypeError, "ERR_INVALID_ARG_TYPE", "paint: count exceeds cells");
	if (order && count > order.length) throw err(TypeError, "ERR_INVALID_ARG_TYPE", "paint: count exceeds order");
	if (overlaps(screen, cells) || overlaps(screen, charMap) || overlaps(screen, words) || (order && overlaps(screen, order)))
		throw err(TypeError, "ERR_INVALID_ARG_TYPE", "paint: screen must not overlap the input arrays");
	const sc = st.screen;
	const w = new Writer(sc, screen, screenWidth, y);
	let col = x;
	for (let k = 0; k < count; k++) {
		let i = k;
		if (order) {
			i = order[k];
			if (i < 0 || i >= count) throw err(TypeError, "ERR_INVALID_ARG_TYPE", "paint: order index out of range");
		}
		const g = cells[2 * i], info = cells[2 * i + 1];
		const run = info >>> RUN_SHIFT;
		const ch = g >= 0 && g < charMap.length ? charMap[g] : 0;
		const word = run < words.length ? words[run] : 0;
		if (info & TAB_FLAG) {
			const tw = sc.tabWidth;
			// to the next tab stop, but never past the right edge
			// (C remainder: from a negative column the stop is tw - col % tw away)
			const n = Math.max(0, Math.min(col + tw - (col % tw), screenWidth) - col);
			for (let j = 0; j < n; j++) { w.put(col, sc.emptyCharIndex, sc.emptyWord | sc.narrow); col++; }
			continue;
		}
		const cw = info & 255;
		if (cw >= 2 && col + cw > screenWidth) {
			w.put(col, sc.emptyCharIndex, sc.emptyWord | sc.spacerHead);
			col++;
			continue;
		}
		const styled = word & ~sc.widthMask;
		w.put(col, ch, styled | (cw >= 2 ? sc.wide : sc.narrow));
		// a cluster wider than 2 gets more tail cells, carrying its style
		for (let j = 2; j < cw; j++) w.put(col + j, sc.spacerCharIndex, styled | sc.spacerTail);
		col += cw >= 2 ? cw : 1;
	}
	return w.result(col);
};

CellSegmenter.prototype.setCell = function setCell(screen, screenWidth, x, y, charIndex, word) {
	const st = state(this, "setCell");
	checkInt32Array(screen, "screen");
	screenWidth = toInt32(screenWidth); x = toInt32(x); y = toInt32(y);
	charIndex = toInt32(charIndex); word = toInt32(word);
	const w = new Writer(st.screen, screen, screenWidth, y);
	w.put(x, charIndex, word);
	return w.result(x + 1);
};

for (const name of ["graphemes", "sgrKeys", "sgrCloseKeys", "uris"]) {
	Object.defineProperty(CellSegmenter.prototype, name, {
		get() {
			const st = this !== null && typeof this === "object" ? this[INTERNAL] : undefined;
			if (st === undefined) throw new TypeError(`The CellSegmenter.${name} getter can only be used on instances of CellSegmenter`);
			return st[name].sync();
		},
		enumerable: false, configurable: true,
	});
}
Object.defineProperty(CellSegmenter.prototype, Symbol.toStringTag, { value: "CellSegmenter", configurable: true });
for (const name of ["segment", "paint", "setCell"]) Object.defineProperty(CellSegmenter.prototype, name, { enumerable: false });
Object.defineProperty(CellSegmenter, "length", { value: 0 });
