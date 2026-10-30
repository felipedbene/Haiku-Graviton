// ANSI escape recognizer shared by stringWidth / stripANSI / sliceAnsi / wrapAnsi.
// Port of Bun bun-v1.4.4 src/jsc/bindings/ANSIHelpers.h (isEscapeCharacter,
// consumeANSI with UTF-16/Latin-1 code-unit semantics: one unit = one codepoint).
// Indices are UTF-16 code-unit offsets into a JS string.

export function isEscapeCharacter(c) {
	return c === 0x1b || c === 0x9b || c === 0x9d || c === 0x90 || c === 0x98 || c === 0x9e || c === 0x9f;
}

const START = 0, GOT_ESC = 1, IGNORE_NEXT = 2, IN_CSI = 3, IN_OSC = 4, IN_OSC_GOT_ESC = 5,
	NEED_ST = 6, NEED_ST_GOT_ESC = 7;

// Returns the index just past the escape sequence starting at `start` (or `end`
// when it is unterminated). `s[start]` is expected to be an introducer; if it is
// not, `start` is returned (no progress), exactly like the C++.
export function consumeANSI(s, start, end) {
	let state = START;
	for (let it = start; it < end; it++) {
		const c = s.charCodeAt(it);
		switch (state) {
		case START:
			if (c === 0x1b) state = GOT_ESC;
			else if (c === 0x9b) state = IN_CSI;
			else if (c === 0x9d) state = IN_OSC;
			else if (c === 0x90 || c === 0x98 || c === 0x9e || c === 0x9f) state = NEED_ST;
			else return it;		// includes 0xc2: only special on the UTF-8 path
			break;
		case IN_OSC_GOT_ESC:
		case NEED_ST_GOT_ESC:
			if (c === 0x5c) {	// '\' : ESC \ is the string terminator
				state = START;
				break;
			}
			state = GOT_ESC;
			// fallthrough: this unit is the one following that ESC
		case GOT_ESC: // eslint-disable-line no-fallthrough
			if (c === 0x1b) break;
			if (c === 0x18 || c === 0x1a) { state = START; break; }
			if (c === 0x5b) { state = IN_CSI; break; }			// [
			if (c === 0x5d) { state = IN_OSC; break; }			// ]
			if (c === 0x50 || c === 0x58 || c === 0x5e || c === 0x5f) { state = NEED_ST; break; }	// P X ^ _
			if (c >= 0x20 && c <= 0x2f) { state = IGNORE_NEXT; break; }
			if (c >= 0x30 && c <= 0x7e) { state = START; break; }
			if (c === 0x9c) { state = START; break; }
			return it;
		case IGNORE_NEXT:
			if (c === 0x1b) { state = GOT_ESC; break; }
			state = START;
			break;
		case IN_CSI: {
			// First unit in [0x40, 0x7E] or one of ESC, CAN, SUB, C1 ST.
			let t = it;
			for (; t < end; t++) {
				const u = s.charCodeAt(t);
				if ((u >= 0x40 && u <= 0x7e) || u === 0x1b || u === 0x18 || u === 0x1a || u === 0x9c) break;
			}
			if (t === end) return end;
			it = t;
			state = s.charCodeAt(t) === 0x1b ? GOT_ESC : START;
			break;
		}
		case IN_OSC:
		case NEED_ST: {
			const osc = state === IN_OSC;
			let t = it;
			for (; t < end; t++) {
				const u = s.charCodeAt(t);
				if (u === 0x1b || u === 0x9c || u === 0x18 || u === 0x1a || (osc && u === 0x07)) break;
			}
			if (t === end) return end;
			it = t;
			if (s.charCodeAt(t) === 0x1b) {
				state = osc ? IN_OSC_GOT_ESC : NEED_ST_GOT_ESC;
				break;
			}
			state = START;
			break;
		}
		}
	}
	return end;
}
