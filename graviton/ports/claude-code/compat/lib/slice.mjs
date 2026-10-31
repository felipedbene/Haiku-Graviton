// Bun.sliceAnsi -- port of Bun bun-v1.4.4 src/jsc/bindings/sliceAnsi.cpp.
// Structure (ASCII fast path, leave-one-behind bulk step, scan horizon, pending
// ANSI, speculative ellipsis zone) is kept as in the C++ because several of those
// optimizations are observable (e.g. the bulk step flushes a pending cluster).
import { fusedClassify, widthFromFused, visibleUTF16Width, graphemeBreak } from "./width.mjs";
import { isEscapeCharacter } from "./ansi.mjs";
import { SgrStyleState, applySgrToState, parseSgrParams, shouldIncludeSgrAfterEnd } from "./sgr.mjs";

const SIZE_MAX = Number.MAX_SAFE_INTEGER;
export const CHARACTER = 0, SGR = 1, HYPERLINK = 2, CONTROL = 3;

const codepointWidth = (cp, amb) => (cp > 0x10ffff ? 1 : widthFromFused(fusedClassify(cp), amb));
const isEmojiPresentation = (cp) => cp <= 0x10ffff && (fusedClassify(cp) & 0x80) !== 0;

export class GraphemeWidthState {
	constructor() { this.count = 0; }
	reset(cp, amb) {
		this.firstCp = cp; this.lastCp = cp; this.count = 1;
		this.keycap = cp === 0x20e3;
		this.regionalIndicator = cp >= 0x1f1e6 && cp <= 0x1f1ff;
		this.skinTone = cp >= 0x1f3fb && cp <= 0x1f3ff;
		this.zwj = cp === 0x200d;
		this.vs15 = false; this.vs16 = false;
		const w = codepointWidth(cp, amb);
		this.baseWidth = w; this.nonEmojiWidth = w;
		this.emojiBase = isEmojiPresentation(cp);
	}
	add(cp, amb) {
		this.lastCp = cp;
		if (this.count < 255) this.count++;
		this.keycap ||= cp === 0x20e3;
		this.regionalIndicator ||= cp >= 0x1f1e6 && cp <= 0x1f1ff;
		this.skinTone ||= cp >= 0x1f3fb && cp <= 0x1f3ff;
		this.zwj ||= cp === 0x200d;
		this.vs15 ||= cp === 0xfe0e;
		this.vs16 ||= cp === 0xfe0f;
		const w = codepointWidth(cp, amb);
		if (w > 0) this.nonEmojiWidth = Math.min(this.nonEmojiWidth + w, 1023);
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

// U16_NEXT: a lone surrogate decodes as itself, one unit long.
export function decodeUTF16(s, p, end) {
	const u = s.charCodeAt(p);
	if (u >= 0xd800 && u <= 0xdbff && p + 1 < end) {
		const v = s.charCodeAt(p + 1);
		if (v >= 0xdc00 && v <= 0xdfff) return [((u - 0xd800) << 10) + (v - 0xdc00) + 0x10000, 2];
	}
	return [u, 1];
}

// findEscapeCharacter: first of 0x1b 0x90 0x98 0x9b 0x9c 0x9d 0x9e 0x9f, or -1.
function findEscape(s, p, end) {
	for (let i = p; i < end; i++) {
		const c = s.charCodeAt(i);
		if (isEscapeCharacter(c) || c === 0x9c) return i;
	}
	return -1;
}

function firstNonAsciiPrintable(s, p, end) {
	for (let i = p; i < end; i++) {
		const c = s.charCodeAt(i);
		if (c < 0x20 || c > 0x7e) return i - p;
	}
	return end - p;
}

function parseCsi(s, start, end) {
	let it = start;
	let canonical = true;
	const c0 = s.charCodeAt(it);
	if (c0 === 0x1b) {
		if (end - it < 2 || s.charCodeAt(it + 1) !== 0x5b) return null;
		it += 2;
	} else if (c0 === 0x9b) it += 1;
	else return null;
	while (it < end) {
		const c = s.charCodeAt(it);
		if (c >= 0x40 && c <= 0x7e) return { end: it + 1, isSgr: c === 0x6d && canonical };
		if (c >= 0x30 && c <= 0x3f) {
			if (!(c >= 0x30 && c <= 0x39) && c !== 0x3b && c !== 0x3a) canonical = false;
			it++; continue;
		}
		if (c >= 0x20 && c <= 0x2f) { canonical = false; it++; continue; }
		if (c === 0x18 || c === 0x1a || c === 0x9c) return { end: it + 1, isSgr: false };
		if (c === 0x1b) return { end: it, isSgr: false };
		canonical = false;
		it++;
	}
	return { end, isSgr: false };
}

function parseEscapeSequence(s, start, end) {
	let it = start + 1;
	if (it === end) return end;
	const c = s.charCodeAt(it);
	if (c === 0x1b) return it;
	if (c === 0x5b || c === 0x5d || c === 0x50 || c === 0x58 || c === 0x5e || c === 0x5f) return null;
	if (c >= 0x20 && c <= 0x2f) {
		++it;
		if (it === end) return end;
		if (s.charCodeAt(it) === 0x1b) return it;
		return it + 1;
	}
	if (c >= 0x30 && c <= 0x7e) return it + 1;
	return null;
}

function parseHyperlink(s, start, end) {
	let it = start;
	let escOsc = false;
	const c0 = s.charCodeAt(it);
	if (c0 === 0x1b && end - it >= 4 && s.charCodeAt(it + 1) === 0x5d && s.charCodeAt(it + 2) === 0x38 && s.charCodeAt(it + 3) === 0x3b) {
		escOsc = true; it += 4;
	} else if (c0 === 0x9d && end - it >= 3 && s.charCodeAt(it + 1) === 0x38 && s.charCodeAt(it + 2) === 0x3b) it += 3;
	else return null;
	let p = it;
	while (p < end && s.charCodeAt(p) !== 0x3b) {
		const c = s.charCodeAt(p);
		if (c === 0x07 || c === 0x9c || c === 0x1b || c === 0x18 || c === 0x1a) return null;
		p++;
	}
	if (p >= end) return null;
	const uriStart = p + 1;
	const closePrefix = escOsc ? "\x1b]8;;" : "\x9d8;;";
	for (p = uriStart; p < end; p++) {
		const c = s.charCodeAt(p);
		if (c === 0x07) return { end: p + 1, isOpen: p > uriStart, code: [start, p + 1], closePrefix, terminator: "\x07" };
		if (c === 0x1b && p + 1 < end && s.charCodeAt(p + 1) === 0x5c)
			return { end: p + 2, isOpen: p > uriStart, code: [start, p + 2], closePrefix, terminator: "\x1b\\" };
		if (c === 0x9c) return { end: p + 1, isOpen: p > uriStart, code: [start, p + 1], closePrefix, terminator: "\x9c" };
		if (c === 0x1b || c === 0x18 || c === 0x1a) return null;
	}
	return null;
}

function parseControlString(s, start, end) {
	let it = start;
	const c = s.charCodeAt(it);
	let supportsBel = false;
	if (c === 0x1b) {
		if (end - it < 2) return null;
		const n = s.charCodeAt(it + 1);
		if (n === 0x5d) { it += 2; supportsBel = true; }
		else if (n === 0x50 || n === 0x58 || n === 0x5e || n === 0x5f) it += 2;
		else if (n === 0x5c) return it + 2;
		else return null;
	} else if (c === 0x9d) { it += 1; supportsBel = true; }
	else if (c === 0x90 || c === 0x98 || c === 0x9e || c === 0x9f) it += 1;
	else if (c === 0x9c) return it + 1;
	else return null;
	while (it < end) {
		const u = s.charCodeAt(it);
		if (supportsBel && u === 0x07) return it + 1;
		if (u === 0x1b) {
			if (it + 1 < end && s.charCodeAt(it + 1) === 0x5c) return it + 2;
			return it;
		}
		if (u === 0x18 || u === 0x1a) return it + 1;
		if (u === 0x9c) return it + 1;
		it++;
	}
	return null;	// unterminated: NOT consumed (unlike stringWidth/stripANSI)
}

// tryParseAnsi -> {end, type, hl?} or null.
export function tryParseAnsi(s, start, end) {
	const c = s.charCodeAt(start);
	if (c === 0x1b || c === 0x9d) {
		const h = parseHyperlink(s, start, end);
		if (h) return { end: h.end, type: HYPERLINK, hl: h };
	}
	if (c === 0x1b || c === 0x9d || c === 0x90 || c === 0x98 || c === 0x9e || c === 0x9f || c === 0x9c) {
		const e = parseControlString(s, start, end);
		if (e !== null) return { end: e, type: CONTROL };
	}
	if (c === 0x1b || c === 0x9b) {
		const r = parseCsi(s, start, end);
		if (r) return { end: r.end, type: r.isSgr ? SGR : CONTROL };
	}
	if (c === 0x1b) {
		const e = parseEscapeSequence(s, start, end);
		if (e !== null) return { end: e, type: CONTROL };
	}
	return null;
}

function resolveSliceBounds(startD, endD, totalW) {
	let from = startD < 0 ? totalW + startD : startD;
	let to = endD < 0 ? totalW + endD : endD;
	if (from < 0) from = 0;
	if (to > totalW) to = totalW;
	if (!(to > from)) return { start: 0, end: 0, cutStart: false, cutEnd: false, empty: true };
	const st = Math.trunc(from), en = Math.trunc(to);
	return { start: st, end: en, cutStart: st > 0, cutEnd: en < totalW, empty: false };
}

// Shared CR/LF handling in front of the table-driven break.
export function clusterBreak(st, prevCp, cp) {
	if (!st.hasPrev) return true;
	if (prevCp === 0x0d && cp === 0x0a) return false;
	if (prevCp === 0x0d || prevCp === 0x0a || cp === 0x0d || cp === 0x0a) { st.breakState = 0; return true; }
	const [brk, ns] = graphemeBreak(prevCp, cp, st.breakState);
	st.breakState = ns;
	return brk;
}

function computeTotalWidth(s, asciiPrefix, amb) {
	const end = s.length;
	let totalW = asciiPrefix > 0 ? asciiPrefix - 1 : 0;
	const st = { hasPrev: false, breakState: 0 };
	let prevCp = 0;
	const gs = new GraphemeWidthState();
	if (asciiPrefix > 0) {
		prevCp = s.charCodeAt(asciiPrefix - 1);
		st.hasPrev = true;
		gs.reset(prevCp, amb);
	}
	let p = asciiPrefix;
	while (p < end) {
		const c = s.charCodeAt(p);
		if (isEscapeCharacter(c) || c === 0x9c) {
			const t = tryParseAnsi(s, p, end);
			if (t) { p = t.end; continue; }
		}
		const [cp, len] = decodeUTF16(s, p, end);
		if (clusterBreak(st, prevCp, cp)) {
			if (st.hasPrev) totalW += gs.width();
			gs.reset(cp, amb);
		} else gs.add(cp, amb);
		prevCp = cp;
		st.hasPrev = true;
		p += len;
	}
	if (st.hasPrev) totalW += gs.width();
	return totalW;
}

function emitSliceStreaming(s, asciiPrefix, start, end, ellipsis, ellipsisWidth, cutStartForEllipsis,
	cutEndKnown, cutEndHint, amb) {
	const dataEnd = s.length;
	const endUnbounded = end === SIZE_MAX;
	let result = "";
	let active = new SgrStyleState();
	let activeHyperlink = false, activeHlClosePrefix = "", activeHlTerminator = "", activeHlCode = "";
	let position = 0, include = false;
	let prevVisCp = 0;
	const st = { hasPrev: false, breakState: 0 };
	const gs = new GraphemeWidthState();
	let pending = [];	// {a, b, type, hlOpen, hl}

	const flushPending = (filterCloseOnly) => {
		for (const pa of pending) {
			let emit = false;
			if (pa.type === SGR) {
				if (filterCloseOnly) {
					const ps = s.charCodeAt(pa.a) === 0x9b ? pa.a + 1 : pa.a + 2;
					const params = parseSgrParams(s, ps, pa.b - 1);
					if (params.overflow || params.hasColon) continue;
					if (!shouldIncludeSgrAfterEnd(params, active)) continue;
				}
				applySgrToState(active, s, pa.a, pa.b);
				emit = true;
			} else if (pa.type === HYPERLINK) {
				const isClose = !pa.hlOpen;
				if (filterCloseOnly && (!isClose || !activeHyperlink)) continue;
				if (pa.hlOpen) {
					activeHyperlink = true;
					activeHlCode = s.slice(pa.hl.code[0], pa.hl.code[1]);
					activeHlClosePrefix = pa.hl.closePrefix;
					activeHlTerminator = pa.hl.terminator;
				} else activeHyperlink = false;
				emit = true;
			} else if (pa.type === CONTROL) {
				if (!filterCloseOnly) emit = true;
			}
			if (emit) result += s.slice(pa.a, pa.b);
		}
		pending = [];
	};

	let needStartEllipsis = false, needEndEllipsis = false, ellipsisEndBudget = 0;
	const startBeforeBudget = start;
	if (ellipsisWidth > 0) {
		if (cutStartForEllipsis && ellipsisWidth < (endUnbounded ? SIZE_MAX - start : end - start)) {
			needStartEllipsis = true;
			start += ellipsisWidth;
		}
		if (cutEndKnown && cutEndHint && ellipsisWidth < end - start) {
			needEndEllipsis = true;
			end -= ellipsisWidth;
		} else if (!cutEndKnown && !endUnbounded && ellipsisWidth < end - start) {
			needEndEllipsis = true;
			ellipsisEndBudget = ellipsisWidth;
			end -= ellipsisWidth;
		}
		if (cutEndKnown && (cutStartForEllipsis || cutEndHint) && !needStartEllipsis && !needEndEllipsis)
			return ellipsis;
	}
	let inSpecZone = false, specZoneMark = 0, specStyles = null;
	let specHyperlink = false, specHlClosePrefix = "", specHlTerminator = "";
	const enterSpecZone = () => {
		if (inSpecZone) return;
		inSpecZone = true;
		specZoneMark = result.length;
		specStyles = active.clone();
		specHyperlink = activeHyperlink;
		specHlClosePrefix = activeHlClosePrefix;
		specHlTerminator = activeHlTerminator;
	};
	const specEnd = ellipsisEndBudget > 0 ? end + ellipsisEndBudget : end;

	{
		const ffEnd = asciiPrefix > 0 ? asciiPrefix - 1 : 0;
		position = Math.min(start, ffEnd);
	}
	let p = position;
	let sawCutEnd = false, pastSpecEnd = false;

	const beginInclude = () => {
		include = true;
		result += active.openCodes();
		if (needStartEllipsis) result += ellipsis;
		if (activeHyperlink) result += activeHlCode;
	};

	// Returns false to stop (cut reached).
	const processVisibleCp = (cp, len) => {
		const shouldBreak = clusterBreak(st, prevVisCp, cp);
		if (shouldBreak) {
			if (st.hasPrev) position += gs.width();
			if (!endUnbounded && position >= specEnd) {
				if (ellipsisWidth === 0 || position > specEnd || codepointWidth(cp, amb) > 0) {
					sawCutEnd = true;
					flushPending(true);
					return false;
				}
				pastSpecEnd = true;
				gs.reset(cp, amb);
				prevVisCp = cp;
				st.hasPrev = true;
				p += len;
				return true;
			}
			if (!include && position >= start) beginInclude();
			if (include) {
				flushPending(false);
				if (!endUnbounded && position >= end && ellipsisEndBudget > 0) enterSpecZone();
				result += s.slice(p, p + len);
			} else pending = [];
			gs.reset(cp, amb);
		} else {
			if (include && !pastSpecEnd) {
				flushPending(false);
				result += s.slice(p, p + len);
			} else if (!include) pending = [];
			gs.add(cp, amb);
		}
		prevVisCp = cp;
		st.hasPrev = true;
		p += len;
		return true;
	};

	walk: while (p < dataEnd) {
		let scanEnd;
		if (endUnbounded) scanEnd = dataEnd;
		else {
			const budget = (specEnd > position ? specEnd - position : 0) + 4;
			scanEnd = dataEnd - p <= budget ? dataEnd : p + budget;
		}
		const nextEsc = findEscape(s, p, scanEnd);
		const runEnd = nextEsc >= 0 ? nextEsc : scanEnd;
		{
			const asciiLen = firstNonAsciiPrintable(s, p, runEnd);
			let bulkN = asciiLen > 1 ? asciiLen - 1 : 0;
			if (bulkN > 0) {
				if (st.hasPrev) {
					position += gs.width();
					st.hasPrev = false;
					if (!endUnbounded && position >= specEnd) {
						sawCutEnd = true;
						flushPending(true);
						break walk;
					}
				}
				if (!include && position < start) {
					const skipN = Math.min(start - position, bulkN);
					p += skipN; position += skipN; bulkN -= skipN;
				}
				if (bulkN > 0 && !include && position >= start) beginInclude();
				if (bulkN > 0 && include) {
					flushPending(false);
					const emitN = endUnbounded ? bulkN : Math.min(specEnd > position ? specEnd - position : 0, bulkN);
					if (emitN > 0) {
						if (ellipsisEndBudget > 0) {
							const toMain = position < end ? Math.min(end - position, emitN) : 0;
							if (toMain > 0) result += s.slice(p, p + toMain);
							if (emitN > toMain) {
								enterSpecZone();
								result += s.slice(p + toMain, p + emitN);
							}
						} else result += s.slice(p, p + emitN);
						p += emitN; position += emitN; bulkN -= emitN;
					}
					if (!endUnbounded && position >= specEnd) {
						sawCutEnd = true;
						break walk;
					}
				}
				p += bulkN;
				position += bulkN;
			}
		}
		while (p < runEnd) {
			const [cp, len] = decodeUTF16(s, p, dataEnd);
			if (!processVisibleCp(cp, len)) break walk;
		}
		if (p >= dataEnd) break;
		const c = s.charCodeAt(p);
		if (isEscapeCharacter(c) || c === 0x9c) {
			const t = tryParseAnsi(s, p, dataEnd);
			if (t) {
				if (!include) {
					if (t.type === SGR) applySgrToState(active, s, p, t.end);
					else if (t.type === HYPERLINK) {
						if (t.hl.isOpen) {
							activeHyperlink = true;
							activeHlCode = s.slice(t.hl.code[0], t.hl.code[1]);
							activeHlClosePrefix = t.hl.closePrefix;
							activeHlTerminator = t.hl.terminator;
						} else activeHyperlink = false;
					}
				} else pending.push({ a: p, b: t.end, type: t.type, hlOpen: t.hl?.isOpen ?? false, hl: t.hl });
				p = t.end;
				continue;
			}
		}
		{
			const [cp, len] = decodeUTF16(s, p, dataEnd);
			if (!processVisibleCp(cp, len)) break walk;
		}
	}

	const reachedEOF = !sawCutEnd;
	const lastClusterCol = position;
	if (reachedEOF) {
		if (st.hasPrev) position += gs.width();
		if (!endUnbounded && position > specEnd) sawCutEnd = true;
	}
	if (ellipsisWidth > 0 && (!include || (!needStartEllipsis && !needEndEllipsis))
		&& (sawCutEnd || (cutStartForEllipsis && (position > startBeforeBudget || (st.hasPrev && lastClusterCol >= startBeforeBudget)))))
		return ellipsis;
	if (!include) return "";
	let discardedZone = false;
	if (ellipsisEndBudget > 0) {
		if (sawCutEnd) {
			if (inSpecZone) {
				result = result.slice(0, specZoneMark);
				active = specStyles;
				activeHyperlink = specHyperlink;
				activeHlClosePrefix = specHlClosePrefix;
				activeHlTerminator = specHlTerminator;
				discardedZone = true;
			}
		} else needEndEllipsis = false;
	}
	if (reachedEOF && !discardedZone) flushPending(!endUnbounded && position >= specEnd);
	if (activeHyperlink) result += activeHlClosePrefix + activeHlTerminator;
	if (needEndEllipsis) result += ellipsis;
	result += active.closeCodes();
	return result;
}

// Returns null for "reuse the input string".
function sliceAnsiImpl(s, startD, endD, ellipsis, ellipsisWidth, amb) {
	if (s.length === 0) return "";
	if (startD === 0 && !Number.isFinite(endD) && endD > 0 && ellipsisWidth === 0) return null;
	let prefixScanLen = s.length;
	if (startD >= 0 && endD >= 0 && Number.isFinite(endD)) {
		const cap = endD + 2;
		if (cap < s.length) prefixScanLen = Math.trunc(cap);
	}
	const asciiPrefix = firstNonAsciiPrintable(s, 0, prefixScanLen);
	const wholeStringAscii = prefixScanLen === s.length && asciiPrefix === s.length;
	const sliceInsidePrefix = startD >= 0 && endD >= 0 && endD < asciiPrefix;
	if (wholeStringAscii || sliceInsidePrefix) {
		const totalW = wholeStringAscii ? s.length : asciiPrefix;
		const b = resolveSliceBounds(startD, endD, totalW);
		if (b.empty) return "";
		const cutEnd = wholeStringAscii ? b.cutEnd : true;
		if (!b.cutStart && !cutEnd) return null;
		let st = b.start, en = b.end;
		if (ellipsisWidth > 0) {
			const doStart = b.cutStart && ellipsisWidth < en - st;
			if (doStart) st += ellipsisWidth;
			const doEnd = cutEnd && ellipsisWidth < en - st;
			if (doEnd) en -= ellipsisWidth;
			if (!doStart && !doEnd) return ellipsis;
			return (doStart ? ellipsis : "") + s.slice(st, en) + (doEnd ? ellipsis : "");
		}
		return s.slice(st, en);
	}
	let start, end, cutEndKnown, cutEndHint;
	if (startD >= 0 && !(endD < 0)) {
		if (!Number.isFinite(startD) || startD > s.length * 2) return "";
		start = startD;
		if (!Number.isFinite(endD) || endD > s.length * 2) {
			end = SIZE_MAX; cutEndKnown = true; cutEndHint = false;
		} else {
			end = endD;
			if (end <= start) return "";
			cutEndKnown = false; cutEndHint = false;
		}
	} else {
		const totalW = computeTotalWidth(s, asciiPrefix, amb);
		const b = resolveSliceBounds(startD, endD, totalW);
		if (b.empty) return "";
		start = b.start;
		end = b.cutEnd ? b.end : SIZE_MAX;
		cutEndKnown = true;
		cutEndHint = b.cutEnd;
	}
	return emitSliceStreaming(s, asciiPrefix, start, end, ellipsis, ellipsisWidth, start > 0, cutEndKnown, cutEndHint, amb);
}

// ECMAScript ToIntegerOrInfinity.
function toIntegerOrInfinity(v) {
	const n = Number(v);
	if (Number.isNaN(n) || n === 0) return 0;
	if (!Number.isFinite(n)) return n;
	return Math.trunc(n);
}

export function sliceAnsi(input, startV, endV, arg4, arg5) {
	const s = String(input);
	if (s.length === 0) return "";
	const startD = startV === undefined ? 0 : toIntegerOrInfinity(startV);
	const endD = endV === undefined ? Infinity : toIntegerOrInfinity(endV);
	let ellipsis = null, amb = false;
	if (typeof arg4 === "string") {
		ellipsis = arg4;
		if (typeof arg5 === "boolean") amb = !arg5;
	} else if (typeof arg4 === "boolean") amb = !arg4;
	else if (arg4 !== null && (typeof arg4 === "object" || typeof arg4 === "function")) {
		const e = arg4.ellipsis;
		if (typeof e === "string") ellipsis = e;
		const a = arg4.ambiguousIsNarrow;
		if (a !== undefined) amb = !a;
	} else if (arg4 === undefined) {
		if (typeof arg5 === "boolean") amb = !arg5;
	}
	const ell = ellipsis ?? "";
	const ellipsisWidth = ell ? visibleUTF16Width(ell, true, amb) : 0;
	const r = sliceAnsiImpl(s, startD, endD, ell, ellipsisWidth, amb);
	return r === null ? s : r;
}
