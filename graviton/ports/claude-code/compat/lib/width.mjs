// Bun.stringWidth -- port of Bun bun-v1.4.4 src/jsc/bindings/stringWidth.cpp
// (UTF-16 path, including the bulk-run semantics of highway_visible_utf16_width,
// which flushes the pending grapheme cluster -- observable, so it is kept).
// Data: width-tables.mjs, converted value for value from Bun's generated header.
import { kGraphemeBreakStage1 as S1, kGraphemeBreakStage2 as S2, kGraphemeBreakStage3 as S3 }
	from "./width-tables.mjs";
import { isEscapeCharacter, consumeANSI } from "./ansi.mjs";

const CLASS_MASK = 0x1f, EMOJI_BIT = 0x80;
export const fusedClassify = (cp) => S3[S2[S1[cp >> 8] + (cp & 0xff)]];
export function widthFromFused(packed, ambiguousAsWide) {
	const w = (packed >> 5) & 3;
	return w === 3 ? (ambiguousAsWide ? 2 : 1) : w;
}

// GraphemeBreakClass ordinals (must match the table) and break states.
const G = { Other: 0, Prepend: 1, RI: 2, SpacingMark: 3, L: 4, V: 5, T: 6, LV: 7, LVT: 8, ZWJ: 9,
	ZWNJ: 10, ExtPict: 11, EModBase: 12, EMod: 13, InCBExtend: 14, InCBLinker: 15, InCBConsonant: 16 };
const S = { Default: 0, RI: 1, ExtPict: 2, InCBConsonant: 3, InCBLinker: 4 };
const isInCBExtend = (g) => g === G.InCBExtend || g === G.ZWJ;
const isExtend = (g) => g === G.ZWNJ || g === G.InCBExtend || g === G.InCBLinker;
const isExtPict = (g) => g === G.ExtPict || g === G.EModBase;

// computeGraphemeBreakNoControl, transcribed. Returns [shouldBreak, newState].
function computeBreak(g1, g2, state) {
	switch (state) {
	case S.RI:
		if (g1 !== G.RI || g2 !== G.RI) state = S.Default;
		break;
	case S.ExtPict: {
		const ok = (g) => g === G.InCBExtend || g === G.InCBLinker || g === G.ZWNJ || g === G.ZWJ
			|| g === G.ExtPict || g === G.EModBase || g === G.EMod;
		if (!ok(g1)) state = S.Default;
		if (!ok(g2)) state = S.Default;
		break;
	}
	case S.InCBConsonant:
	case S.InCBLinker: {
		const ok = (g) => g === G.InCBConsonant || g === G.InCBLinker || g === G.InCBExtend || g === G.ZWJ;
		if (!ok(g1)) state = S.Default;
		if (!ok(g2)) state = S.Default;
		break;
	}
	}
	if (g1 === G.L && (g2 === G.L || g2 === G.V || g2 === G.LV || g2 === G.LVT)) return [false, state];
	if ((g1 === G.LV || g1 === G.V) && (g2 === G.V || g2 === G.T)) return [false, state];
	if ((g1 === G.LVT || g1 === G.T) && g2 === G.T) return [false, state];
	if (g2 === G.SpacingMark) return [false, state];
	if (g1 === G.Prepend) return [false, state];
	if (g1 === G.InCBConsonant) {
		if (isInCBExtend(g2)) return [false, S.InCBConsonant];
		if (g2 === G.InCBLinker) return [false, S.InCBLinker];
	} else if (state === S.InCBConsonant) {
		if (g2 === G.InCBLinker) return [false, S.InCBLinker];
		if (isInCBExtend(g2)) return [false, state];
		state = S.Default;
	} else if (state === S.InCBLinker) {
		if (g2 === G.InCBLinker || isInCBExtend(g2)) return [false, state];
		if (g2 === G.InCBConsonant) return [false, S.Default];
		state = S.Default;
	}
	if (isExtPict(g1)) {
		if (isExtend(g2) || g2 === G.ZWJ) return [false, S.ExtPict];
		if (g1 === G.EModBase && g2 === G.EMod) return [false, S.ExtPict];
	} else if (state === S.ExtPict) {
		if ((isExtend(g1) || g1 === G.EMod) && (isExtend(g2) || g2 === G.ZWJ)) return [false, state];
		if (g1 === G.ZWJ && isExtPict(g2)) return [false, S.Default];
		state = S.Default;
	}
	if (g1 === G.RI && g2 === G.RI) {
		if (state === S.Default) return [false, S.RI];
		return [true, S.Default];
	}
	if (isExtend(g2) || g2 === G.ZWJ) return [false, state];
	return [true, state];
}

// Precomputed decision table, same key/value layout as the C++.
const DECISIONS = new Uint8Array(1 << 13);
for (let st = 0; st < 5; st++)
	for (let a = 0; a < 17; a++)
		for (let b = 0; b < 17; b++) {
			const [brk, ns] = computeBreak(a, b, st);
			DECISIONS[st | (a << 3) | (b << 8)] = (brk ? 1 : 0) | (ns << 1);
		}

// Bun__graphemeBreak(cp1, cp2, &state): table lookup on both codepoints' classes.
// Returns [shouldBreak, newState]. Out-of-range input is a boundary.
export function graphemeBreak(cp1, cp2, state) {
	if (cp1 > 0x10ffff || cp2 > 0x10ffff) return [true, state];
	const v = DECISIONS[state | ((fusedClassify(cp1) & CLASS_MASK) << 3) | ((fusedClassify(cp2) & CLASS_MASK) << 8)];
	return [(v & 1) === 1, v >> 1];
}

class GraphemeState {
	constructor() { this.count = 0; }
	reset(cp, packed, amb) {
		this.firstCp = cp;
		if (cp < 0x80) {
			const w = cp >= 0x20 && cp < 0x7f ? 1 : 0;
			this.count = 1; this.baseWidth = w; this.nonEmojiWidth = w;
			this.emojiBase = this.keycap = this.regionalIndicator = this.skinTone = this.zwj = this.vs15 = this.vs16 = false;
			return;
		}
		const w = widthFromFused(packed, amb);
		this.count = 1; this.baseWidth = w; this.nonEmojiWidth = w;
		this.emojiBase = (packed & EMOJI_BIT) !== 0;
		this.keycap = cp === 0x20e3;
		this.regionalIndicator = cp >= 0x1f1e6 && cp <= 0x1f1ff;
		this.skinTone = cp >= 0x1f3fb && cp <= 0x1f3ff;
		this.zwj = cp === 0x200d;
		this.vs15 = false; this.vs16 = false;
	}
	add(cp, packed, amb) {
		if (this.count < 255) this.count++;
		this.keycap ||= cp === 0x20e3;
		this.regionalIndicator ||= cp >= 0x1f1e6 && cp <= 0x1f1ff;
		this.skinTone ||= cp >= 0x1f3fb && cp <= 0x1f3ff;
		this.zwj ||= cp === 0x200d;
		this.vs15 ||= cp === 0xfe0e;
		this.vs16 ||= cp === 0xfe0f;
		this.nonEmojiWidth = Math.min(this.nonEmojiWidth + widthFromFused(packed, amb), 1023);
	}
	width() {
		if (this.count === 0) return 0;
		if (this.regionalIndicator && this.count >= 2) return 2;
		if (this.keycap) return 2;
		if (this.regionalIndicator) return 1;
		if (this.emojiBase && (this.skinTone || this.zwj)) return 2;
		if (this.vs15 || this.vs16) {
			if (this.baseWidth === 2 || (this.vs16 && (this.emojiBase || this.firstCp === 0xa9 || this.firstCp === 0xae))) return 2;
			return this.baseWidth;
		}
		return this.nonEmojiWidth;
	}
}

// ClassifyBulkUTF16Unit: units that are always their own cluster, fixed width.
function bulkWidth(u) {
	if ((u >= 0x20 && u <= 0x7e) || (u >= 0xa0 && u <= 0x2ff && u !== 0xa9 && u !== 0xad && u !== 0xae)
		|| (u >= 0x370 && u <= 0x482) || (u >= 0x48a && u <= 0x52f)) return 1;
	if ((u >= 0x3041 && u <= 0x3096) || (u >= 0x309b && u <= 0x30ff) || (u >= 0x3400 && u <= 0x4dbf)
		|| (u >= 0x4e00 && u <= 0x9fff) || (u >= 0xac00 && u <= 0xd7a3) || (u >= 0xff01 && u <= 0xff60)) return 2;
	return 0;
}
const isSurrogate = (u) => u >= 0xd800 && u <= 0xdfff;
const isLead = (u) => u >= 0xd800 && u <= 0xdbff;
const isTrail = (u) => u >= 0xdc00 && u <= 0xdfff;

// UTF16WidthAccumulator, transcribed.
class Accumulator {
	constructor(amb) {
		this.amb = amb; this.len = 0; this.g = new GraphemeState();
		this.prevClass = G.Other; this.breakState = S.Default; this.hasPrev = false;
	}
	addCodepoint(cp) {
		const packed = fusedClassify(cp), cls = packed & CLASS_MASK;
		if (!this.hasPrev) this.g.reset(cp, packed, this.amb);
		else {
			const v = DECISIONS[this.breakState | (this.prevClass << 3) | (cls << 8)];
			this.breakState = v >> 1;
			if (v & 1) { this.len += this.g.width(); this.g.reset(cp, packed, this.amb); }
			else this.g.add(cp, packed, this.amb);
		}
		this.hasPrev = true;
		this.prevClass = cls;
	}
	seedFromBulkRun(cp, packed) {
		if (this.g.count > 0) this.len += this.g.width();
		this.g.reset(cp, packed, this.amb);
		this.hasPrev = true;
		this.prevClass = packed & CLASS_MASK;
		this.breakState = S.Default;
	}
	// Consume s[i..end) (or up to the next escape introducer when stopAtEscape).
	addRun(s, i, end, stopAtEscape) {
		const begin = i;
		for (;;) {
			if (!this.amb && i < end) {
				const u0 = s.charCodeAt(i);
				if (u0 >= 0x20 && !isSurrogate(u0)) {
					let j = i, w = 0, bw;
					while (j < end && (bw = bulkWidth(s.charCodeAt(j)))) { w += bw; j++; }
					if (j > i) {
						const lastCp = s.charCodeAt(j - 1), lp = fusedClassify(lastCp);
						this.seedFromBulkRun(lastCp, lp);
						this.len += w - widthFromFused(lp, this.amb);
						i = j;
						continue;
					}
				}
			}
			if (i >= end || (stopAtEscape && isEscapeCharacter(s.charCodeAt(i)))) break;
			let idx = 0;
			if (s.charCodeAt(i) <= 0x7f) {
				let bound = end;
				if (stopAtEscape) {
					const e = s.indexOf("\x1b", i);
					bound = e === -1 || e > end ? end : e;
				}
				while (i + idx < bound && s.charCodeAt(i + idx) <= 0x7f) idx++;
			}
			if (idx > 0) {
				const lastCp = s.charCodeAt(i + idx - 1), lp = fusedClassify(lastCp);
				this.seedFromBulkRun(lastCp, lp);
				for (let k = i; k < i + idx - 1; k++) {
					const c = s.charCodeAt(k);
					if (c >= 0x20 && c < 0x7f) this.len++;
				}
				i += idx;
				continue;
			}
			const u = s.charCodeAt(i);
			if (isLead(u)) {
				if (i + 1 < end && isTrail(s.charCodeAt(i + 1))) {
					this.addCodepoint(((u - 0xd800) << 10) + (s.charCodeAt(i + 1) - 0xdc00) + 0x10000);
					i += 2;
				} else i += 1;	// lone lead / truncated pair: skipped, zero width
			} else if (isTrail(u)) i += 1;
			else { this.addCodepoint(u); i += 1; }
		}
		return i - begin;
	}
	finish() { return this.len + this.g.width(); }
}

export function visibleUTF16Width(s, excludeAnsi, amb) {
	const acc = new Accumulator(amb);
	const end = s.length;
	if (!excludeAnsi) {
		acc.addRun(s, 0, end, false);
		return acc.finish();
	}
	let p = 0;
	while (p < end) {
		if (isEscapeCharacter(s.charCodeAt(p))) { p = consumeANSI(s, p, end); continue; }
		p += acc.addRun(s, p, end, true);
	}
	return acc.finish();
}

// Bun's getIfPropertyExistsPrototypePollutionMitigation: own or inherited, but the
// lookup stops before Object.prototype.
export function optionLookup(o, key) {
	for (let p = o; p !== null && p !== Object.prototype; p = Object.getPrototypeOf(p))
		if (Object.prototype.hasOwnProperty.call(p, key)) return { found: true, value: o[key] };
	return { found: false };
}

// getTruthy option semantics (undefined/null/falsy strings keep the default).
function truthyOption(o, key, dflt) {
	const r = optionLookup(o, key);
	if (!r.found) return dflt;
	const v = r.value;
	if (v === undefined || v === null) return dflt;
	if (typeof v === "string" && !v) return dflt;
	return Boolean(v);
}

export function stringWidth(input, options) {
	if (input === undefined) return 0;
	const s = String(input);
	if (s.length === 0) return 0;
	let countAnsi = false, ambiguousIsNarrow = true;
	if (options !== null && (typeof options === "object" || typeof options === "function")) {
		countAnsi = truthyOption(options, "countAnsiEscapeCodes", false);
		ambiguousIsNarrow = truthyOption(options, "ambiguousIsNarrow", true);
		// perCodePoint selects Bun's ICU (node GetColumnWidth) algorithm, which this
		// port does not implement; refuse rather than silently measure differently.
		if (truthyOption(options, "perCodePoint", false))
			throw new TypeError("Bun.stringWidth({perCodePoint}) is not implemented by claude-code compat");
	}
	return visibleUTF16Width(s, !countAnsi, !ambiguousIsNarrow);
}
