// Bun.wrapAnsi -- port of Bun bun-v1.4.4 src/jsc/bindings/wrapAnsi.cpp.
// Rows are arrays of string pieces joined on demand; widths use the same
// stringWidth (Bun__visibleWidthExcludeANSI_utf16) and per-codepoint table.
import { fusedClassify, widthFromFused, visibleUTF16Width, graphemeBreak } from "./width.mjs";
import { isEscapeCharacter, consumeANSI } from "./ansi.mjs";
import { sgrCloseCode } from "./sgr.mjs";

const codepointWidth = (cp, amb) => (cp > 0x10ffff ? 1 : widthFromFused(fusedClassify(cp), amb));

function decodeUTF16(s, p, end) {
	const u = s.charCodeAt(p);
	if (u >= 0xd800 && u <= 0xdbff && p + 1 < end) {
		const v = s.charCodeAt(p + 1);
		if (v >= 0xdc00 && v <= 0xdfff) return [((u - 0xd800) << 10) + (v - 0xdc00) + 0x10000, 2];
	}
	return [u, 1];
}

function findEscape(s, p, end) {
	for (let i = p; i < end; i++) {
		const c = s.charCodeAt(i);
		if (isEscapeCharacter(c) || c === 0x9c) return i;
	}
	return -1;
}

const strWidth = (s, narrow) => (s.length === 0 ? 0 : visibleUTF16Width(s, true, !narrow));

function skipLeadingAnsi(s, a, b) {
	return a < b && isEscapeCharacter(s.charCodeAt(a)) ? consumeANSI(s, a, b) : a;
}

function wordStartsNewCluster(s, a, b) {
	a = skipLeadingAnsi(s, a, b);
	if (a >= b) return true;
	const [cp] = decodeUTF16(s, a, b);
	if (cp < 0x80) return true;
	return graphemeBreak(0x20, cp, 0)[0];
}

function wordSeamIsAscii(rowTail, s, a, b) {
	a = skipLeadingAnsi(s, a, b);
	if (a >= b) return true;
	return rowTail < 0x80 && s.charCodeAt(a) < 0x80;
}

// The separator space ending the word at `start`; escapes are opaque.
function findWordSeparator(s, start, end) {
	const nextSpace = (from) => { const i = s.indexOf(" ", from); return i === -1 || i >= end ? end : i; };
	let space = nextSpace(start);
	for (let it = start; ;) {
		const esc = findEscape(s, it, space);
		if (esc < 0) return space;
		const after = consumeANSI(s, esc, end);
		it = after > esc ? after : esc + 1;
		if (it > space) space = nextSpace(it);
	}
}

class Row {
	constructor() { this.d = ""; this.trimScanOffset = 0; this.leadingTrimComplete = false; }
	width(narrow) { return strWidth(this.d, narrow); }
	trimLeadingSpaces() {
		if (this.leadingTrimComplete) return 0;
		const d = this.d, size = d.length;
		let read = this.trimScanOffset, removed = 0;
		let out = d.slice(0, this.trimScanOffset);
		let changed = false;
		while (read < size) {
			const c = d.charCodeAt(read);
			if (isEscapeCharacter(c)) {
				const e = consumeANSI(d, read, size);
				out += d.slice(read, e);
				read = e;
			} else if (c === 0x20 || c === 0x09) {
				if (c === 0x20) removed++;
				read++;
				changed = true;
			} else {
				this.leadingTrimComplete = true;
				break;
			}
		}
		const write = out.length;
		if (changed) this.d = out + d.slice(read);
		this.trimScanOffset = write;
		return removed;
	}
}

function wrapWord(rows, s, a, b, columns, opt) {
	let vis = rows[rows.length - 1].width(opt.narrow);
	let it = a;
	while (it < b) {
		if (isEscapeCharacter(s.charCodeAt(it))) {
			const e = consumeANSI(s, it, b);
			rows[rows.length - 1].d += s.slice(it, e);
			it = e;
			continue;
		}
		const [cp, len] = decodeUTF16(s, it, b);
		const w = codepointWidth(cp, !opt.narrow);
		if (vis + w > columns) { rows.push(new Row()); vis = 0; }
		rows[rows.length - 1].d += s.slice(it, it + len);
		vis += w;
		it += len;
		if (vis === columns && it < b) { rows.push(new Row()); vis = 0; }
	}
	if (vis === 0 && rows[rows.length - 1].d.length > 0 && rows.length > 1) {
		const last = rows.pop();
		rows[rows.length - 1].d += last.d;
	}
}

function trimRowTrailingSpaces(row, narrow) {
	const d = row.d, end = d.length;
	let lastVisibleEnd = 0;
	for (let ws = 0; ;) {
		const we = findWordSeparator(d, ws, end);
		if (strWidth(d.slice(ws, we), narrow) > 0) lastVisibleEnd = we;
		if (we === end) break;
		ws = we + 1;
	}
	if (lastVisibleEnd === end) return;
	let out = d.slice(0, lastVisibleEnd);
	for (let it = lastVisibleEnd; it !== end;) {
		if (isEscapeCharacter(d.charCodeAt(it))) {
			const e = consumeANSI(d, it, end);
			out += d.slice(it, e);
			it = e;
		} else {
			if (d.charCodeAt(it) !== 0x20) out += d[it];
			++it;
		}
	}
	row.d = out;
}

// Digits then 'm', as a single code (uint32 arithmetic, like the C++).
function parseSgrCode(s, a, b) {
	let code = 0;
	for (let it = a; it < b; ++it) {
		const c = s.charCodeAt(it);
		if (c >= 0x30 && c <= 0x39) code = (code * 10 + (c - 0x30)) >>> 0;
		else if (c === 0x6d) return code;
		else break;
	}
	return null;
}

// trackEscape: classify the sequence at `seq`, update state, return its end.
function trackEscape(s, seq, end, st) {
	let body, isCsi = false, isOsc = false;
	const c0 = s.charCodeAt(seq);
	if (c0 === 0x1b) {
		if (end - seq < 2) return end;
		const next = s.charCodeAt(seq + 1);
		body = seq + 2;
		if (next === 0x5b) isCsi = true;
		else if (next === 0x5d) isOsc = true;
		else if (next === 0x50 || next === 0x58 || next === 0x5e || next === 0x5f) { /* control string */ }
		else if (next >= 0x20 && next <= 0x2f) return end - seq >= 3 && s.charCodeAt(body) !== 0x1b ? seq + 3 : body;
		else if (next >= 0x30 && next <= 0x7e) return body;
		else return seq + 1;
	} else if (c0 === 0x9b) { isCsi = true; body = seq + 1; }
	else if (c0 === 0x9d) { isOsc = true; body = seq + 1; }
	else if (c0 === 0x90 || c0 === 0x98 || c0 === 0x9e || c0 === 0x9f) body = seq + 1;
	else return seq + 1;

	if (isCsi) {
		let seqEnd = end;
		for (let it = body; it < end; ++it) {
			const c = s.charCodeAt(it);
			if (c === 0x1b) { seqEnd = it; break; }
			if ((c >= 0x40 && c <= 0x7e) || c === 0x18 || c === 0x1a || c === 0x9c) { seqEnd = it + 1; break; }
		}
		const code = parseSgrCode(s, body, seqEnd);
		if (code !== null) st.escapeCode = code === 39 || code === 0 ? null : code;
		return seqEnd;
	}
	let seqEnd = end, payloadEnd = end, terminated = false;
	for (let it = body; it < end; ++it) {
		const c = s.charCodeAt(it);
		if (c === 0x1b) {
			payloadEnd = it;
			terminated = it + 1 < end && s.charCodeAt(it + 1) === 0x5c;
			seqEnd = terminated ? it + 2 : it;
			break;
		}
		if ((isOsc && c === 0x07) || c === 0x9c) { payloadEnd = it; seqEnd = it + 1; terminated = true; break; }
		if (c === 0x18 || c === 0x1a) { payloadEnd = it; seqEnd = it + 1; break; }
	}
	if (!isOsc || !terminated || payloadEnd - body < 2 || s.charCodeAt(body) !== 0x38 || s.charCodeAt(body + 1) !== 0x3b) return seqEnd;
	const params = body + 2;
	let uri = params;
	while (uri < payloadEnd && s.charCodeAt(uri) !== 0x3b) ++uri;
	if (uri === payloadEnd) return seqEnd;
	++uri;
	st.link = uri === payloadEnd ? null : s.slice(params, payloadEnd);
	return seqEnd;
}

function joinRows(rows) {
	const st = { escapeCode: null, link: null };
	let out = "";
	for (let r = 0; r < rows.length; ++r) {
		if (r > 0) {
			out += "\n";
			if (st.escapeCode !== null && sgrCloseCode(st.escapeCode)) out += "\x1b[" + st.escapeCode + "m";
			if (st.link !== null) out += "\x1b]8;" + st.link + "\x07";
		}
		const d = rows[r].d, end = d.length;
		let it = 0;
		while (it !== end) {
			const esc = findEscape(d, it, end);
			if (esc < 0) { out += d.slice(it); break; }
			const e = trackEscape(d, esc, end, st);
			out += d.slice(it, e);
			it = e;
		}
		if (r + 1 < rows.length) {
			if (st.link !== null) out += "\x1b]8;;\x07";
			if (st.escapeCode !== null) {
				const close = sgrCloseCode(st.escapeCode);
				if (close) out += "\x1b[" + close + "m";
			}
		}
	}
	return out;
}

function processLine(s, ls, le, columns, opt) {
	const rows = [];
	if (opt.trim) {
		let a = ls, b = le;
		while (a < b && (s.charCodeAt(a) === 0x20 || s.charCodeAt(a) === 0x09)) a++;
		while (b > a && (s.charCodeAt(b - 1) === 0x20 || s.charCodeAt(b - 1) === 0x09)) b--;
		if (a >= b) return rows;
	}
	rows.push(new Row());
	let lastRowWidth = 0, dirty = false;
	const placeWord = (ws, we, idx) => {
		if (opt.trim) {
			const removed = rows[rows.length - 1].trimLeadingSpaces();
			if (!dirty) lastRowWidth = removed < lastRowWidth ? lastRowWidth - removed : 0;
		}
		if (dirty) { lastRowWidth = rows[rows.length - 1].width(opt.narrow); dirty = false; }
		let rowLength = lastRowWidth, spacePrecedes = true, rowTail = 0x20;
		if (idx !== 0) {
			if (rowLength >= columns && (!opt.wordWrap || !opt.trim)) { rows.push(new Row()); rowLength = 0; }
			if (rowLength > 0 || !opt.trim) { rows[rows.length - 1].d += " "; rowLength++; }
			else if (rows[rows.length - 1].d.length > 0) {
				spacePrecedes = false;
				const d = rows[rows.length - 1].d;
				rowTail = d.charCodeAt(d.length - 1);
			}
		}
		const wordLen = strWidth(s.slice(ws, we), opt.narrow);
		if (opt.hard && wordLen > columns) {
			const remaining = columns > rowLength ? columns - rowLength : 0;
			const thisLine = 1 + (wordLen > remaining ? Math.floor((wordLen - remaining - 1) / columns) : 0);
			const nextLine = wordLen > 0 ? Math.floor((wordLen - 1) / columns) : 0;
			if (nextLine < thisLine) rows.push(new Row());
			dirty = true;
			wrapWord(rows, s, ws, we, columns, opt);
			return;
		}
		if (rowLength + wordLen > columns && rowLength > 0 && wordLen > 0) {
			if (!opt.wordWrap && rowLength < columns) { dirty = true; wrapWord(rows, s, ws, we, columns, opt); return; }
			rows.push(new Row());
			rowLength = 0;
		}
		if (rowLength + wordLen > columns && !opt.wordWrap) { dirty = true; wrapWord(rows, s, ws, we, columns, opt); return; }
		rows[rows.length - 1].d += s.slice(ws, we);
		if (spacePrecedes ? wordStartsNewCluster(s, ws, we) : wordSeamIsAscii(rowTail, s, ws, we)) lastRowWidth = rowLength + wordLen;
		else dirty = true;
	};
	let ws = ls;
	for (let idx = 0; ; ++idx) {
		const we = findWordSeparator(s, ws, le);
		placeWord(ws, we, idx);
		if (we === le) break;
		ws = we + 1;
	}
	if (opt.trim) for (const row of rows) trimRowTrailingSpaces(row, opt.narrow);
	return rows;
}

function toIntegerOrInfinity(v) {
	const n = Number(v);
	if (Number.isNaN(n) || n === 0) return 0;
	if (!Number.isFinite(n)) return n;
	return Math.trunc(n);
}

export function wrapAnsi(input, columnsV, options) {
	const s = String(input);
	if (s.length === 0) return "";
	let columns = 0;
	if (columnsV !== undefined) {
		const c = toIntegerOrInfinity(columnsV);
		if (c > 0 && Number.isFinite(c)) columns = c;
	}
	const opt = { hard: false, wordWrap: true, trim: true, narrow: true };
	if (options !== null && (typeof options === "object" || typeof options === "function")) {
		opt.hard = Boolean(options.hard);
		opt.wordWrap = options.wordWrap !== false;
		opt.trim = options.trim !== false;
		if (options.ambiguousIsNarrow !== undefined) opt.narrow = Boolean(options.ambiguousIsNarrow);
	}
	if (columns === 0) return s;
	let out = "", ls = 0, first = true;
	const end = s.length;
	for (;;) {
		let br = -1;
		for (let k = ls; k < end; k++) {
			const c = s.charCodeAt(k);
			if (c === 0x0a || c === 0x0d) { br = k; break; }
		}
		const le = br === -1 ? end : br;
		if (!first) out += "\n";
		first = false;
		const rows = processLine(s, ls, le, columns, opt);
		if (rows.length) out += joinRows(rows);
		if (le === end) break;
		ls = s.charCodeAt(le) === 0x0d && le + 1 !== end && s.charCodeAt(le + 1) === 0x0a ? le + 2 : le + 1;
	}
	return out;
}
