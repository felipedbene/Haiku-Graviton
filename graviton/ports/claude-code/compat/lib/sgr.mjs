// SGR style tracking shared by sliceAnsi and wrapAnsi. Port of Bun bun-v1.4.4
// ANSIHelpers.h (sgrCloseCode, isSgrEndCode) and the SgrStyleState / SgrParams /
// applySgrToState / shouldIncludeSgrAfterEnd code in sliceAnsi.cpp.

export function sgrCloseCode(c) {
	switch (c) {
	case 1: case 2: return 22;
	case 3: case 20: return 23;
	case 4: case 21: return 24;
	case 5: case 6: return 25;
	case 7: return 27;
	case 8: return 28;
	case 9: return 29;
	case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38:
	case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97: return 39;
	case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48:
	case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107: return 49;
	case 51: case 52: return 54;
	case 53: return 55;
	case 58: return 59;
	case 73: case 74: return 75;
	default: return 0;
	}
}

const END_CODES = new Set([0, 22, 23, 24, 25, 27, 28, 29, 39, 49, 54, 55, 59, 75]);
export const isSgrEndCode = (c) => END_CODES.has(c);

export function sgrSlot(openCode) {
	const close = sgrCloseCode(openCode);
	return close === 22 || close === 23 || close === 24 || close === 54 ? openCode : close;
}

export const makeSgrCode = (isC1, code) => (isC1 ? "\x9b" : "\x1b[") + code + "m";
export const makeSgrCodeMulti = (isC1, codes) => (isC1 ? "\x9b" : "\x1b[") + codes.join(";") + "m";

export class SgrStyleState {
	constructor(entries = []) { this.entries = entries; }
	clone() { return new SgrStyleState(this.entries.map((e) => ({ ...e }))); }
	applyReset() { this.entries = []; }
	applyEnd(endCode) { this.entries = this.entries.filter((e) => e.endCode !== endCode); }
	applyStart(slot, openCode, endCode) {
		this.entries = this.entries.filter((e) => e.slot !== slot);
		this.entries.push({ slot, endCode, openCode });
	}
	openCodes() { return this.entries.map((e) => e.openCode).join(""); }
	closeCodes() {
		let out = "";
		const n = this.entries.length;
		for (let i = n; i > 0; --i) {
			const end = this.entries[i - 1].endCode;
			let already = false;
			for (let j = n; j > i; --j) if (this.entries[j - 1].endCode === end) { already = true; break; }
			if (!already) out += end;
		}
		return out;
	}
	isEmpty() { return this.entries.length === 0; }
}

// parseSgrParams over s[ps, pe): digits accumulate (clamped), ';'/':' separate,
// anything else stops. An empty parameter is 0, so count >= 1.
export function parseSgrParams(s, ps, pe) {
	const out = { data: [], overflow: false, hasColon: false };
	let current = 0;
	for (let p = ps; p < pe; p++) {
		const c = s.charCodeAt(p);
		if (c >= 0x30 && c <= 0x39) {
			if (current < 100000) current = current * 10 + (c - 0x30);
		} else if (c === 0x3b || c === 0x3a) {
			if (c === 0x3a) out.hasColon = true;
			if (out.data.length >= 32) { out.overflow = true; return out; }
			out.data.push(current);
			current = 0;
		} else break;
	}
	if (out.data.length >= 32) { out.overflow = true; return out; }
	out.data.push(current);
	return out;
}

// applySgrToState for the sequence s[a, b) (ESC[...m or C1 CSI ...m).
export function applySgrToState(state, s, a, b) {
	const isC1 = s.charCodeAt(a) === 0x9b;
	const params = parseSgrParams(s, isC1 ? a + 1 : a + 2, b - 1);
	const P = params.data;
	if (params.hasColon || params.overflow) {
		const first = P[0];
		const close = sgrCloseCode(first);
		state.applyStart(sgrSlot(first), s.slice(a, b), close ? makeSgrCode(false, close) : "\x1b[0m");
		return;
	}
	let i = 0;
	while (i < P.length) {
		const code = P[i];
		if (code === 0) { state.applyReset(); i++; continue; }
		if (code === 38 || code === 48 || code === 58) {
			const slot = sgrSlot(code), endStr = makeSgrCode(false, sgrCloseCode(code));
			if (i + 1 < P.length) {
				const t = P[i + 1];
				if (t === 5 && i + 2 < P.length) {
					state.applyStart(slot, makeSgrCodeMulti(isC1, [code, 5, P[i + 2]]), endStr);
					i += 3; continue;
				}
				if (t === 2 && i + 4 < P.length) {
					state.applyStart(slot, makeSgrCodeMulti(isC1, [code, 2, P[i + 2], P[i + 3], P[i + 4]]), endStr);
					i += 5; continue;
				}
			}
			state.applyStart(slot, makeSgrCode(isC1, code), endStr);
			i++; continue;
		}
		if (isSgrEndCode(code)) { state.applyEnd(makeSgrCode(false, code)); i++; continue; }
		const close = sgrCloseCode(code);
		state.applyStart(sgrSlot(code), makeSgrCode(isC1, code), close ? makeSgrCode(false, close) : "\x1b[0m");
		i++;
	}
}

export function shouldIncludeSgrAfterEnd(params, active) {
	let hasStart = false, hasClose = false;
	const P = params.data;
	for (let i = 0; i < P.length; ++i) {
		const code = P[i];
		if (code === 0) { if (!active.isEmpty()) hasClose = true; continue; }
		if (isSgrEndCode(code)) {
			const endStr = makeSgrCode(false, code);
			if (active.entries.some((e) => e.endCode === endStr)) hasClose = true;
			continue;
		}
		if (code === 38 || code === 48 || code === 58) {
			hasStart = true;
			if (i + 1 < P.length) {
				const t = P[i + 1];
				if (t === 5 && i + 2 < P.length) i += 2;
				else if (t === 2 && i + 4 < P.length) i += 4;
			}
			continue;
		}
		hasStart = true;
	}
	return hasClose && !hasStart;
}
