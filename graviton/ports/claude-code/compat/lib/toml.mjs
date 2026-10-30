// Bun.TOML.parse -- port of Bun bun-v1.4.4 src/parsers/toml.rs (Scanner,
// Parser; the Tokenizer only serves TOML::tokens, not Bun.TOML.parse, but its
// non-recursive keyval walk is the model for parseKeyval here),
// src/runtime/api/TOMLObject.rs + TOMLObject.bind.ts (argument coercion, the
// thrown SyntaxError), src/runtime/api.rs with_text_format_source (the 2 GiB
// RangeError) and src/js_parser_jsc/expr_jsc.rs (Expr -> JS: keys defined as
// own properties, datetimes via Temporal.*.from).
//
// The scanner works on the UTF-8 bytes, as Bun's does, so every check happens
// in the same order and the first error raised is the one Bun logs first (Bun
// aborts on the first error, so throwing at once is equivalent). Bun builds an
// Expr tree and converts it afterwards; here JS values are built directly,
// which is the same tree minus the second walk.
//
// Differences that cannot be removed on Node, all deliberate:
// - Bun returns date/time values as Temporal.Instant / PlainDateTime /
//   PlainDate / PlainTime objects. Node 24 has no Temporal, so `parse`
//   returns the string Bun's object stringifies to (String(v) and
//   JSON.stringify(v) agree with Bun; typeof does not).
//   `parseWith(input, onDateTime)` hands each one to the caller instead.
// - A Blob input is refused (Node cannot read one synchronously).
// - Bun throws RangeError "Maximum call stack size exceeded." on documents
//   nested deeper than its native stack allows (around 10k-20k inline
//   tables, 20k-30k arrays, 50k-100k tables); this parse has no recursion.

import { Buffer, isUtf8 } from "node:buffer";

const MAX_SAFE_INTEGER = (1n << 53n) - 1n;
const I64_MAX = (1n << 63n) - 1n;
const I64_MIN_MAG = 1n << 63n;
const MAX_PARSEABLE_LEN = 2147483647;	// i32::MAX: Bun's Loc is an i32

const BARE_CR = "Bare carriage return is not allowed; use \\r\\n or \\n";
const UNDERSCORE_IN_NUMBER = "Underscores in numbers must be surrounded by digits";
const LOSSY_INT = "Integer cannot be losslessly represented as a JavaScript number; it must be within +/-(2^53 - 1)";

// E::TomlDateTimeKind, named after the Temporal type Bun materializes.
const DT_OFFSET = "Instant", DT_LOCAL_DT = "PlainDateTime", DT_DATE = "PlainDate", DT_TIME = "PlainTime";

// Meta.kind: how a table or array came to exist (see toml.rs `Kind`).
const K_HEADER = 0, K_HEADER_IMPLICIT = 1, K_DOTTED = 2, K_ARRAY_ELEM = 3, K_INLINE = 4, K_AOT = 5, K_STATIC = 6;

// Value token tags (toml.rs `ValueData`).
const V_STRING = 0, V_NUMBER = 1, V_DATETIME = 2, V_BOOL = 3, V_ARRAY = 4, V_INLINE = 5;

const isDigit = (c) => c >= 0x30 && c <= 0x39;
const isAlpha = (c) => (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a);
const isAlnum = (c) => isDigit(c) || isAlpha(c);
const isHex = (c) => isDigit(c) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66);
const hexVal = (c) => (isDigit(c) ? c - 0x30 : (c | 0x20) - 0x61 + 10);
const isBareKeyChar = (c) => isAlnum(c) || c === 0x2d || c === 0x5f;

// TOMLObject::parse: a failed parse throws the first log message this way.
const syntaxError = (msg) => new SyntaxError("TOML Parse error: " + msg);

function codedError(Ctor, message, code) {
	const e = new Ctor(message);
	Object.defineProperty(e, "code", { value: code, writable: true, configurable: true, enumerable: false });
	return e;
}

// ── scanner ─────────────────────────────────────────────────────────────────

class Scanner {
	constructor(src) {
		this.src = src;	// Buffer of UTF-8 bytes
		this.pos = 0;
	}

	// ── error helpers ──────────────────────────────────────────────────────

	err(msg) { return syntaxError(msg); }

	// `{before} '{key}'{after}`; redaction is off for Bun.TOML.parse.
	errKeyed(before, key, after) { return syntaxError(`${before} '${key}'${after}`); }

	errChar(pos, what) {
		if (pos >= this.src.length) return syntaxError(`${what} end of file`);
		const c = this.src[pos];
		if (c >= 0x21 && c <= 0x7e) return syntaxError(`${what} '${String.fromCharCode(c)}'`);
		return syntaxError(`${what} (0x${c.toString(16).toUpperCase().padStart(2, "0")})`);
	}

	// A bare word in value position is almost always an unquoted string.
	errUnquotedString(pos) {
		let end = pos;
		while (end < this.src.length && isBareKeyChar(this.peekAt(end)) && end - pos < 64) end++;
		if (end === pos) return syntaxError("Strings must be quoted");
		return syntaxError(`Strings must be quoted: "${this.src.toString("latin1", pos, end)}"`);
	}

	// ── byte cursor ────────────────────────────────────────────────────────

	peek() { return this.pos < this.src.length ? this.src[this.pos] : 0; }
	peekAt(p) { return p >= 0 && p < this.src.length ? this.src[p] : 0; }
	atEof() { return this.pos >= this.src.length; }
	startsWith(p, s) {
		if (p + s.length > this.src.length) return false;
		for (let i = 0; i < s.length; i++) if (this.src[p + i] !== s.charCodeAt(i)) return false;
		return true;
	}

	skipWs() { for (let c = this.peek(); c === 0x20 || c === 0x09; c = this.peek()) this.pos++; }

	expectNewline() {
		const c = this.peek();
		if (c === 0x0a) { this.pos++; return; }
		if (c === 0x0d) {
			if (this.peekAt(this.pos + 1) === 0x0a) { this.pos += 2; return; }
			throw this.err(BARE_CR);
		}
		throw this.errChar(this.pos, "Expected a newline but found");
	}

	skipComment() {
		this.pos++;
		for (;;) {
			const c = this.peek();
			if (c === 0 && this.atEof()) return;
			if (c === 0x0a) return;
			if (c === 0x0d) {
				if (this.peekAt(this.pos + 1) === 0x0a) return;
				throw this.err(BARE_CR);
			}
			if (c === 0x09) { this.pos++; continue; }
			if (c < 0x20 || c === 0x7f) throw this.errChar(this.pos, "Control character is not allowed in a comment:");
			this.pos++;
		}
	}

	skipWsCommentNewline() {
		for (;;) {
			const c = this.peek();
			if (c === 0x20 || c === 0x09) this.pos++;
			else if (c === 0x0a || c === 0x0d) this.expectNewline();
			else if (c === 0x23) this.skipComment();
			else return;
		}
	}

	// ── document setup ─────────────────────────────────────────────────────

	initDocument() {
		// simdutf validation in Bun; node:buffer isUtf8 is the same check.
		if (!isUtf8(this.src)) throw this.err("Invalid UTF-8 byte sequence");
		if (this.startsWith(0, "\xEF\xBB\xBF")) this.pos = 3;
		return this.pos;
	}

	// ── scan modes ─────────────────────────────────────────────────────────

	// Returns null (Eof), {aot} (TableOpen) or a key segment string.
	scanLineStart() {
		this.skipWsCommentNewline();
		if (this.atEof()) return null;
		if (this.peek() === 0x5b) {
			this.pos++;
			// `array-table-open = %x5B.5B`: the second bracket is adjacent.
			const aot = this.peek() === 0x5b;
			if (aot) this.pos++;
			return { aot };
		}
		return this.scanKeySegment();
	}

	// One key segment (bare, basic-quoted, or literal-quoted), decoded.
	scanKeySegment() {
		const c = this.peek();
		if (c === 0x22) return this.scanBasicString(false).text;
		if (c === 0x27) return this.scanLiteralString(false).text;
		if (isBareKeyChar(c)) {
			const start = this.pos;
			while (isBareKeyChar(this.peek())) this.pos++;
			return this.src.toString("latin1", start, this.pos);
		}
		throw this.errChar(this.pos, "Expected a key but found");
	}

	scanKeyAfterSep() { this.skipWs(); return this.scanKeySegment(); }

	// true for `.`, false for `=`.
	scanKeyvalSep() {
		this.skipWs();
		const c = this.peek();
		if (c === 0x2e) { this.pos++; return true; }
		if (c === 0x3d) { this.pos++; return false; }
		throw this.errChar(this.pos, "Expected '=' after a key but found");
	}

	// true for `.`, false for the close; `]]` must be adjacent.
	scanHeaderSep(aot) {
		this.skipWs();
		const c = this.peek();
		if (c === 0x2e) { this.pos++; return true; }
		if (c === 0x5d) {
			this.pos++;
			if (aot) {
				if (this.peek() !== 0x5d) throw this.errChar(this.pos, "Expected ']]' to close an array-of-tables header but found");
				this.pos++;
			}
			return false;
		}
		throw aot
			? this.errChar(this.pos, "Expected ']]' to close an array-of-tables header but found")
			: this.errChar(this.pos, "Expected ']' to close a table header but found");
	}

	// Returns a key string, CLOSE or EOF (TOML 1.1 multi-line inline tables).
	scanInlineKey() {
		this.skipWsCommentNewline();
		if (this.peek() === 0x7d) { this.pos++; return CLOSE; }
		if (this.atEof()) return EOF;
		return this.scanKeySegment();
	}

	// `keyval-sep = ws %x3D ws`: spaces/tabs only, then exactly one value.
	scanValueRequired() {
		this.skipWs();
		const c = this.peek();
		if (c === 0x0a || c === 0x0d) throw this.err("Missing value after '='; values must be on the same line");
		if (c === 0 && this.atEof()) throw this.err("Missing value after '='");
		return this.scanValueToken();
	}

	// Returns a value token, CLOSE or EOF.
	scanArrayItem() {
		this.skipWsCommentNewline();
		if (this.peek() === 0x5d) { this.pos++; return CLOSE; }
		if (this.atEof()) return EOF;
		return this.scanValueToken();
	}

	// true for `,`, false for the closer.
	scanListSep(close, what) {
		this.skipWsCommentNewline();
		const c = this.peek();
		if (c === 0x2c) { this.pos++; return true; }
		if (c === close) { this.pos++; return false; }
		throw this.errChar(this.pos, what);
	}

	scanLineEnd(after) {
		this.skipWs();
		if (this.peek() === 0x23) this.skipComment();
		if (this.atEof()) return;
		const c = this.peek();
		if (c === 0x0a || c === 0x0d) return this.expectNewline();
		throw this.err(`Expected a newline or end of file after ${after}`);
	}

	// ── value scanning ─────────────────────────────────────────────────────

	scanValueToken() {
		const pos = this.pos;
		const c = this.peek();
		if (c === 0x22) {
			const s = this.scanBasicString(this.startsWith(this.pos, '"""'));
			return { t: V_STRING, v: s.text };
		}
		if (c === 0x27) {
			const s = this.scanLiteralString(this.startsWith(this.pos, "'''"));
			return { t: V_STRING, v: s.text };
		}
		if (c === 0x74) { this.expectKeyword("true"); return { t: V_BOOL, v: true }; }
		if (c === 0x66) { this.expectKeyword("false"); return { t: V_BOOL, v: false }; }
		if (c === 0x5b) { this.pos++; return { t: V_ARRAY, v: null }; }
		if (c === 0x7b) { this.pos++; return { t: V_INLINE, v: null }; }
		if (c === 0x69 || c === 0x6e || c === 0x2b || c === 0x2d || isDigit(c)) return this.scanNumberOrDatetime();
		if (isAlpha(c)) throw this.errUnquotedString(pos);
		throw this.errChar(pos, "Expected a value but found");
	}

	expectKeyword(word) {
		const pos = this.pos;
		// `truex` and `tru` are both unquoted strings.
		if (this.startsWith(this.pos, word) && !isBareKeyChar(this.peekAt(this.pos + word.length))) {
			this.pos += word.length;
			return;
		}
		throw this.errUnquotedString(pos);
	}

	// ── numbers and date/times ─────────────────────────────────────────────

	scanNumberOrDatetime() {
		// Date/times start with an unsigned digit run: `DDDD-` or `DD:`.
		if (isDigit(this.peek())) {
			const d1 = this.digitRunLen(this.pos);
			if (d1 === 4 && this.peekAt(this.pos + 4) === 0x2d) {
				const dt = this.scanDatetimeFromDate();
				this.expectValueTerminator();
				return { t: V_DATETIME, v: dt.text, kind: dt.kind };
			}
			if (d1 === 2 && this.peekAt(this.pos + 2) === 0x3a) {
				const start = this.pos;
				this.scanTimeDigits();
				this.expectValueTerminator();
				return { t: V_DATETIME, v: this.src.toString("latin1", start, this.pos), kind: DT_TIME };
			}
		}
		return this.scanNumber();
	}

	digitRunLen(start) {
		let i = start;
		while (isDigit(this.peekAt(i))) i++;
		return i - start;
	}

	readDigits(n, what) {
		let value = 0;
		for (let i = 0; i < n; i++) {
			const c = this.peek();
			if (!isDigit(c)) throw this.err(what);
			value = value * 10 + (c - 0x30);
			this.pos++;
		}
		return value;
	}

	scanDatetimeFromDate() {
		const start = this.pos;
		const year = this.readDigits(4, "Invalid date: expected a 4-digit year");
		if (this.peek() !== 0x2d) throw this.err("Invalid date: expected '-' after the year");
		this.pos++;
		const month = this.readDigits(2, "Invalid date: expected a 2-digit month");
		if (this.peek() !== 0x2d) throw this.err("Invalid date: expected '-' after the month");
		this.pos++;
		const day = this.readDigits(2, "Invalid date: expected a 2-digit day");

		if (month < 1 || month > 12) throw this.err("Invalid date: month must be between 01 and 12");
		const leap = year % 4 === 0 && (year % 100 !== 0 || year % 400 === 0);
		let maxDay;
		switch (month) {
			case 1: case 3: case 5: case 7: case 8: case 10: case 12: maxDay = 31; break;
			case 4: case 6: case 9: case 11: maxDay = 30; break;
			default: maxDay = leap ? 29 : 28;
		}
		if (day < 1 || day > maxDay) throw this.err("Invalid date: day is out of range for the month");

		// Optional time part: 'T'/'t', or a space when a time clearly follows.
		let hasTime = false;
		const c = this.peek();
		if (c === 0x54 || c === 0x74) { this.pos++; hasTime = true; }
		else if (c === 0x20 && isDigit(this.peekAt(this.pos + 1)) && isDigit(this.peekAt(this.pos + 2)) && this.peekAt(this.pos + 3) === 0x3a) {
			this.pos++;
			hasTime = true;
		}
		if (!hasTime) return { text: this.src.toString("latin1", start, this.pos), kind: DT_DATE };

		this.scanTimeDigits();
		let hasOffset = false;
		const o = this.peek();
		if (o === 0x5a || o === 0x7a) { this.pos++; hasOffset = true; }
		else if (o === 0x2b || o === 0x2d) {
			this.pos++;
			const hour = this.readDigits(2, "Invalid date-time offset: expected 2-digit hours");
			if (this.peek() !== 0x3a) throw this.err("Invalid date-time offset: expected ':' between hours and minutes");
			this.pos++;
			const minute = this.readDigits(2, "Invalid date-time offset: expected 2-digit minutes");
			if (hour > 23) throw this.err("Invalid date-time offset: hours must be between 00 and 23");
			if (minute > 59) throw this.err("Invalid date-time offset: minutes must be between 00 and 59");
			hasOffset = true;
		}
		return { text: this.src.toString("latin1", start, this.pos), kind: hasOffset ? DT_OFFSET : DT_LOCAL_DT };
	}

	// `HH:MM[:SS[.frac]]` -- seconds are optional in TOML 1.1.
	scanTimeDigits() {
		const hour = this.readDigits(2, "Invalid time: expected 2-digit hours");
		if (this.peek() !== 0x3a) throw this.err("Invalid time: expected ':' after hours");
		this.pos++;
		const minute = this.readDigits(2, "Invalid time: expected 2-digit minutes");
		if (hour > 23) throw this.err("Invalid time: hours must be between 00 and 23");
		if (minute > 59) throw this.err("Invalid time: minutes must be between 00 and 59");
		if (this.peek() === 0x3a) {
			this.pos++;
			const second = this.readDigits(2, "Invalid time: expected 2-digit seconds");
			// 60 covers leap seconds, per RFC 3339.
			if (second > 60) throw this.err("Invalid time: seconds must be between 00 and 60");
			if (this.peek() === 0x2e) {
				this.pos++;
				if (!isDigit(this.peek())) throw this.err("Invalid time: expected at least one digit of fractional seconds");
				while (isDigit(this.peek())) this.pos++;
			}
		}
	}

	// `self.pos.wrapping_sub(1)` at 0 reads past the end, i.e. 0: peekAt(-1) is 0.
	checkUnderscore(digitP) {
		if (!digitP(this.peekAt(this.pos - 1)) || !digitP(this.peekAt(this.pos + 1))) throw this.err(UNDERSCORE_IN_NUMBER);
		this.pos++;
	}

	scanDecimalDigits() {
		for (;;) {
			const c = this.peek();
			if (isDigit(c)) this.pos++;
			else if (c === 0x5f) this.checkUnderscore(isDigit);
			else return;
		}
	}

	scanNumber() {
		const start = this.pos;
		let negative = false;
		const s = this.peek();
		if (s === 0x2d) { this.pos++; negative = true; }
		else if (s === 0x2b) this.pos++;

		// inf / nan, optionally signed; `infinity` / `nanoseconds` are words.
		if (this.startsWith(this.pos, "inf") && !isBareKeyChar(this.peekAt(this.pos + 3))) {
			this.pos += 3;
			this.expectValueTerminator();
			return { t: V_NUMBER, v: negative ? -Infinity : Infinity };
		}
		if (this.startsWith(this.pos, "nan") && !isBareKeyChar(this.peekAt(this.pos + 3))) {
			this.pos += 3;
			this.expectValueTerminator();
			return { t: V_NUMBER, v: NaN };
		}

		// Radix-prefixed integers (unsigned only).
		const p1 = this.peekAt(this.pos + 1);
		if (this.peek() === 0x30 && (p1 === 0x78 || p1 === 0x6f || p1 === 0x62)) {
			if (negative || this.src[start] === 0x2b) throw this.err("A sign is not allowed on hexadecimal, octal, or binary integers");
			return this.scanRadixInteger();
		}

		if (!isDigit(this.peek())) {
			if (start === this.pos && isAlpha(this.peek())) throw this.errUnquotedString(start);
			throw this.errChar(this.pos, "Expected a number but found");
		}

		// Integer part. Bun accumulates a u64 magnitude with overflow flag;
		// the digit text as a BigInt gives the same comparisons below.
		const intStart = this.pos;
		let digits = 0, mag = "";
		for (;;) {
			const c = this.peek();
			if (isDigit(c)) { digits++; mag += String.fromCharCode(c); this.pos++; }
			else if (c === 0x5f) this.checkUnderscore(isDigit);
			else break;
		}
		if (digits > 1 && this.src[intStart] === 0x30) throw this.err("Leading zeros are not allowed in numbers");

		let isFloat = false;
		if (this.peek() === 0x2e) {
			isFloat = true;
			this.pos++;
			if (!isDigit(this.peek())) throw this.err("A decimal point must be followed by at least one digit");
			this.scanDecimalDigits();
		}
		const e = this.peek();
		if (e === 0x65 || e === 0x45) {
			isFloat = true;
			this.pos++;
			const sg = this.peek();
			if (sg === 0x2b || sg === 0x2d) this.pos++;
			if (!isDigit(this.peek())) throw this.err("An exponent must contain at least one digit");
			this.scanDecimalDigits();
		}

		this.expectValueTerminator();

		if (isFloat) {
			// WTF::parseDouble over the literal minus underscores. The scanner
			// admits only `[+-]digits[.digits][e[+-]digits]`, on which it and
			// Number() are both correctly-rounded full-match parses.
			const raw = this.src.toString("latin1", start, this.pos).replaceAll("_", "");
			return { t: V_NUMBER, v: Number(raw) };
		}

		const magnitude = BigInt(mag);
		const signedLimit = negative ? I64_MIN_MAG : I64_MAX;
		if (magnitude > signedLimit) throw this.err("Integer is outside the 64-bit signed range");
		if (magnitude > MAX_SAFE_INTEGER) throw this.err(LOSSY_INT);
		// -(0 as i64) as f64 is +0: `-0` the integer is not negative zero.
		const n = Number(magnitude);
		return { t: V_NUMBER, v: negative ? 0 - n : n };
	}

	scanRadixInteger() {
		const rc = this.peekAt(this.pos + 1);
		const radix = rc === 0x78 ? 16 : rc === 0x6f ? 8 : 2;
		this.pos += 2;
		const digitP = radix === 16 ? isHex : radix === 8 ? (c) => c >= 0x30 && c <= 0x37 : (c) => c === 0x30 || c === 0x31;
		if (!digitP(this.peek())) throw this.err("Expected at least one digit after the radix prefix");

		let value = 0n;
		const R = BigInt(radix);
		for (;;) {
			const c = this.peek();
			if (digitP(c)) { value = value * R + BigInt(hexVal(c)); this.pos++; }
			else if (c === 0x5f) this.checkUnderscore(digitP);
			else if (isAlnum(c)) throw this.errChar(this.pos, "Invalid digit in number:");
			else break;
		}

		this.expectValueTerminator();
		// u64 overflow and > i64::MAX give the same error in Bun.
		if (value > I64_MAX) throw this.err("Integer is outside the 64-bit signed range");
		if (value > MAX_SAFE_INTEGER) throw this.err(LOSSY_INT);
		return { t: V_NUMBER, v: Number(value) };
	}

	expectValueTerminator() {
		const c = this.peek();
		if (c === 0 && this.atEof()) return;
		if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d || c === 0x2c || c === 0x5d || c === 0x7d || c === 0x23) return;
		throw this.errChar(this.pos, "Unexpected character after a value:");
	}

	// ── strings ────────────────────────────────────────────────────────────

	// Runs of 3-5 close a multi-line string; longer runs are an error.
	quoteRunClose(quote) {
		let run = 0;
		while (this.peekAt(this.pos + run) === quote) run++;
		if (run > 5) throw this.err("Too many quotes at the end of a multi-line string");
		return run;
	}

	// Returns {text}. Bun borrows the source until decoding diverges from it
	// (escape, CRLF, line-ending backslash); `buf` mirrors that copy-on-write.
	scanBasicString(multiline) {
		this.pos += multiline ? 3 : 1;
		if (multiline) {
			// A newline immediately after the opening delimiter is trimmed.
			const c = this.peek();
			if (c === 0x0a) this.pos++;
			else if (c === 0x0d && this.peekAt(this.pos + 1) === 0x0a) this.pos += 2;
		}
		const start = this.pos;
		let buf = null;
		const src = this.src;
		for (;;) {
			if (this.atEof()) throw this.err("Unterminated string");
			const c = src[this.pos];
			if (c === 0x22) {
				if (!multiline) {
					const text = buf ? decode(buf) : src.toString("utf8", start, this.pos);
					this.pos++;
					return { text };
				}
				const run = this.quoteRunClose(0x22);
				if (run >= 3) {
					const extra = run - 3;
					let text;
					if (buf) { for (let i = 0; i < extra; i++) buf.push(0x22); text = decode(buf); }
					else text = src.toString("utf8", start, this.pos + extra);
					this.pos += run;
					return { text };
				}
				if (buf) for (let i = 0; i < run; i++) buf.push(0x22);
				this.pos += run;
			} else if (c === 0x5c) {
				// Line-ending backslash (multi-line only): trim all whitespace
				// up to the next non-whitespace character.
				if (multiline) {
					let i = this.pos + 1;
					while (this.peekAt(i) === 0x20 || this.peekAt(i) === 0x09) i++;
					const n = this.peekAt(i);
					if (n === 0x0a || (n === 0x0d && this.peekAt(i + 1) === 0x0a)) {
						if (!buf) buf = Array.from(src.subarray(start, this.pos));
						this.pos = i;
						for (;;) {
							const w = this.peek();
							if (w === 0x20 || w === 0x09 || w === 0x0a) this.pos++;
							else if (w === 0x0d && this.peekAt(this.pos + 1) === 0x0a) this.pos += 2;
							else break;
						}
						continue;
					}
				}
				if (!buf) buf = Array.from(src.subarray(start, this.pos));
				this.scanEscape(buf);
			} else if (c === 0x0a) {
				if (!multiline) throw this.err("Unterminated string; newlines must be escaped in basic strings");
				if (buf) buf.push(0x0a);
				this.pos++;
			} else if (c === 0x0d) {
				if (this.peekAt(this.pos + 1) !== 0x0a) throw this.err(BARE_CR);
				if (!multiline) throw this.err("Unterminated string; newlines must be escaped in basic strings");
				// CRLF normalizes to LF in multi-line strings.
				if (!buf) buf = Array.from(src.subarray(start, this.pos));
				buf.push(0x0a);
				this.pos += 2;
			} else if (c === 0x09) {
				if (buf) buf.push(0x09);
				this.pos++;
			} else if (c < 0x20 || c === 0x7f) {
				throw this.errChar(this.pos, "Control character must be escaped in a string:");
			} else {
				if (buf) buf.push(c);
				this.pos++;
			}
		}
	}

	scanEscape(buf) {
		this.pos++;
		const c = this.peek();
		this.pos++;
		switch (c) {
			case 0x62: buf.push(0x08); break;	// \b
			case 0x74: buf.push(0x09); break;	// \t
			case 0x6e: buf.push(0x0a); break;	// \n
			case 0x66: buf.push(0x0c); break;	// \f
			case 0x72: buf.push(0x0d); break;	// \r
			case 0x22: buf.push(0x22); break;
			case 0x5c: buf.push(0x5c); break;
			case 0x65: buf.push(0x1b); break;	// \e (TOML 1.1)
			case 0x78: appendScalar(buf, this.readHexCodepoint("hex escape", 2)); break;	// \x (TOML 1.1)
			case 0x75: appendScalar(buf, this.readHexCodepoint("Unicode escape", 4)); break;
			case 0x55: appendScalar(buf, this.readHexCodepoint("Unicode escape", 8)); break;
			default:
				// `0 if self.at_eof()` is tested after the cursor moved past the
				// escaped byte, so a NUL that is the last byte also counts.
				if (c === 0 && this.atEof()) throw this.err("Unterminated escape sequence");
				this.pos--;
				throw this.errChar(this.pos, "Invalid escape sequence:");
		}
	}

	readHexCodepoint(what, digits) {
		let value = 0;
		for (let i = 0; i < digits; i++) {
			const c = this.peek();
			if (!isHex(c)) throw this.err(`A ${what} must be followed by exactly ${digits} hex digits`);
			value = value * 16 + hexVal(c);
			this.pos++;
		}
		return value;
	}

	scanLiteralString(multiline) {
		this.pos += multiline ? 3 : 1;
		if (multiline) {
			const c = this.peek();
			if (c === 0x0a) this.pos++;
			else if (c === 0x0d && this.peekAt(this.pos + 1) === 0x0a) this.pos += 2;
		}
		const start = this.pos;
		let buf = null;
		const src = this.src;
		for (;;) {
			if (this.atEof()) throw this.err("Unterminated string");
			const c = src[this.pos];
			if (c === 0x27) {
				if (!multiline) {
					const text = buf ? decode(buf) : src.toString("utf8", start, this.pos);
					this.pos++;
					return { text };
				}
				const run = this.quoteRunClose(0x27);
				if (run >= 3) {
					const extra = run - 3;
					let text;
					if (buf) { for (let i = 0; i < extra; i++) buf.push(0x27); text = decode(buf); }
					else text = src.toString("utf8", start, this.pos + extra);
					this.pos += run;
					return { text };
				}
				if (buf) for (let i = 0; i < run; i++) buf.push(0x27);
				this.pos += run;
			} else if (c === 0x0a) {
				if (!multiline) throw this.err("Unterminated string; literal strings cannot contain newlines");
				if (buf) buf.push(0x0a);
				this.pos++;
			} else if (c === 0x0d) {
				if (this.peekAt(this.pos + 1) !== 0x0a) throw this.err(BARE_CR);
				if (!multiline) throw this.err("Unterminated string; literal strings cannot contain newlines");
				if (!buf) buf = Array.from(src.subarray(start, this.pos));
				buf.push(0x0a);
				this.pos += 2;
			} else if (c === 0x09) {
				if (buf) buf.push(0x09);
				this.pos++;
			} else if (c < 0x20 || c === 0x7f) {
				throw this.errChar(this.pos, "Control character is not allowed in a literal string:");
			} else {
				if (buf) buf.push(c);
				this.pos++;
			}
		}
	}
}

const CLOSE = Symbol("close"), EOF = Symbol("eof");
const decode = (bytes) => Buffer.from(bytes).toString("utf8");

// char::from_u32 + encode_utf8: surrogates and > 0x10FFFF are not scalars.
function appendScalar(buf, cp) {
	if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) throw syntaxError("Escaped code point must be a Unicode scalar value");
	if (cp < 0x80) buf.push(cp);
	else if (cp < 0x800) buf.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
	else if (cp < 0x10000) buf.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
	else buf.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
}

// ── date/time values ────────────────────────────────────────────────────────
//
// truncate_fractional_seconds: TOML says excess precision "should be
// truncated, not rounded"; Temporal takes at most 9 digits.
function truncateFractionalSeconds(text) {
	const dot = text.indexOf(".");
	if (dot < 0) return text;
	let end = dot + 1;
	while (end < text.length && isDigit(text.charCodeAt(end))) end++;
	if (end - (dot + 1) <= 9) return text;
	return text.slice(0, dot + 10) + text.slice(end);
}

// What String(Temporal.<kind>.from(text)) gives in Bun for the literals the
// scanner admits: seconds default to 00, a leap second 60 is constrained to
// 59, the fraction loses trailing zeros, an Instant is converted to UTC, and
// a year outside 0000-9999 is written with a sign and six digits.
const pad = (n, w) => String(n).padStart(w, "0");
const fmtYear = (y) => (y >= 0 && y <= 9999 ? pad(y, 4) : (y < 0 ? "-" : "+") + pad(Math.abs(y), 6));
function fmtTime(h, m, s, frac) {
	const f = frac.replace(/0+$/, "");
	return `${pad(h, 2)}:${pad(m, 2)}:${pad(s, 2)}${f ? "." + f : ""}`;
}
// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's algorithm).
function daysFromCivil(y, m, d) {
	y -= m <= 2 ? 1 : 0;
	const era = Math.floor(y / 400);
	const yoe = y - era * 400;
	const doy = Math.floor((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
	const doe = yoe * 365 + Math.floor(yoe / 4) - Math.floor(yoe / 100) + doy;
	return era * 146097 + doe - 719468;
}
function civilFromDays(z) {
	z += 719468;
	const era = Math.floor(z / 146097);
	const doe = z - era * 146097;
	const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365);
	const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100));
	const mp = Math.floor((5 * doy + 2) / 153);
	const d = doy - Math.floor((153 * mp + 2) / 5) + 1;
	const m = mp + (mp < 10 ? 3 : -9);
	return [era * 400 + yoe + (m <= 2 ? 1 : 0), m, d];
}
function parseTime(t) {	// HH:MM[:SS[.frac]] at the start of t; returns [h, m, s, frac, rest]
	const h = +t.slice(0, 2), m = +t.slice(3, 5);
	let s = 0, frac = "", i = 5;
	if (t[5] === ":") {
		s = Math.min(+t.slice(6, 8), 59);
		i = 8;
		if (t[8] === ".") { let j = 9; while (j < t.length && isDigit(t.charCodeAt(j))) j++; frac = t.slice(9, j); i = j; }
	}
	return [h, m, s, frac, t.slice(i)];
}

export function dateTimeString(text, kind) {
	if (kind === DT_TIME) {
		const [h, m, s, frac] = parseTime(text);
		return fmtTime(h, m, s, frac);
	}
	const y = +text.slice(0, 4), mo = +text.slice(5, 7), d = +text.slice(8, 10);
	if (kind === DT_DATE) return `${fmtYear(y)}-${pad(mo, 2)}-${pad(d, 2)}`;
	const [h, m, s, frac, rest] = parseTime(text.slice(11));
	if (kind === DT_LOCAL_DT) return `${fmtYear(y)}-${pad(mo, 2)}-${pad(d, 2)}T${fmtTime(h, m, s, frac)}`;
	let off = 0;
	if (rest[0] === "+" || rest[0] === "-") off = (rest[0] === "-" ? -1 : 1) * (+rest.slice(1, 3) * 3600 + +rest.slice(4, 6) * 60);
	const secs = daysFromCivil(y, mo, d) * 86400 + h * 3600 + m * 60 + s - off;
	const days = Math.floor(secs / 86400), sod = secs - days * 86400;
	const [Y, M, D] = civilFromDays(days);
	return `${fmtYear(Y)}-${pad(M, 2)}-${pad(D, 2)}T${fmtTime(Math.floor(sod / 3600), Math.floor(sod / 60) % 60, sod % 60, frac)}Z`;
}

// ── parser ──────────────────────────────────────────────────────────────────

class Parser {
	constructor(src, onDateTime) {
		this.scanner = new Scanner(src);
		this.onDateTime = onDateTime;
		// Keyed by the JS object/array itself, as Bun keys by E::Object address.
		this.tmeta = new Map();
		this.ameta = new Map();
		this.block = 0;
	}

	parseRoot() {
		const sc = this.scanner;
		sc.initDocument();
		const root = {};
		let current = root;
		for (;;) {
			const ls = sc.scanLineStart();
			if (ls === null) return root;
			if (typeof ls === "object") {
				current = this.parseTableHeader(root, ls.aot);
				sc.scanLineEnd("a table header");
			} else {
				this.parseKeyval(current, ls);
				sc.scanLineEnd("a key/value pair");
			}
		}
	}

	parseTableHeader(root, aot) {
		const sc = this.scanner;
		const path = [sc.scanKeyAfterSep()];
		while (sc.scanHeaderSep(aot)) path.push(sc.scanKeyAfterSep());
		this.block++;
		return this.navigateHeader(root, path, aot);
	}

	navigateHeader(root, path, isAot) {
		const sc = this.scanner;
		let cur = root;
		for (let i = 0; i < path.length; i++) {
			const seg = path[i];
			const last = i + 1 === path.length;
			if (!Object.hasOwn(cur, seg)) {
				if (last && isAot) {
					const array = this.newArray(K_AOT);
					const elem = this.appendAotElem(array);
					this.insertKey(cur, seg, array);
					cur = elem;
				} else {
					const t = this.newTable(last ? K_HEADER : K_HEADER_IMPLICIT);
					this.insertKey(cur, seg, t);
					cur = t;
				}
				continue;
			}
			const found = cur[seg];
			const tm = this.tableMeta(found);
			if (tm) {
				if (last) {
					if (isAot) throw sc.errKeyed("Cannot redefine table", seg, " as an array of tables");
					if (tm.kind === K_HEADER_IMPLICIT) {
						this.tmeta.set(found, { kind: K_HEADER, block: this.block });
						cur = found;
					} else if (tm.kind === K_INLINE) {
						throw sc.errKeyed("Cannot redefine inline table", seg, "");
					} else {
						throw sc.errKeyed("Cannot redefine table", seg, "");
					}
				} else {
					if (tm.kind === K_INLINE) throw sc.errKeyed("Cannot extend inline table", seg, "");
					cur = found;
				}
			} else if (Array.isArray(found)) {
				const am = this.ameta.get(found);
				if (am.kind !== K_AOT) throw sc.errKeyed("Cannot extend array", seg, "");
				if (last) {
					if (!isAot) throw sc.errKeyed("Cannot redefine array of tables", seg, " as a table");
					cur = this.appendAotElem(found);
				} else {
					// Descend into the most recent element.
					cur = found[found.length - 1];
				}
			} else {
				throw sc.errKeyed("Cannot redefine key", seg, last && isAot ? " as an array of tables" : " as a table");
			}
		}
		return cur;
	}

	// Path of `key = ...` after its first segment, through the `=`.
	readKeyPath(first) {
		const sc = this.scanner;
		const path = [first];
		while (sc.scanKeyvalSep()) path.push(sc.scanKeyAfterSep());
		return path;
	}

	// parse_keyval + parse_value + parse_array + parse_inline_table, which
	// recurse once per nesting level in toml.rs. V8's stack runs out at a few
	// thousand levels where Bun's lasts past ten thousand, so the recursion is
	// unrolled onto an explicit stack the way toml.rs's own Tokenizer::keyval
	// walks the same grammar. Scanner calls, table creation, block changes
	// and assignments happen in the recursive order: a value is assigned once
	// it is complete, and an inline table restores the block before that.
	parseKeyval(table, first) {
		const sc = this.scanner;
		const open = [];	// {arr, dest} or {tbl, outerBlock, dest}
		// Where the value being parsed goes: {arr} or {table, path}.
		let dest = { table, path: this.readKeyPath(first) };
		let token = sc.scanValueRequired();
		next: for (;;) {
			let value;
			if (token.t === V_ARRAY) {
				const arr = this.newArray(K_STATIC);
				open.push({ arr, dest });
				const item = sc.scanArrayItem();
				if (item === EOF) throw sc.err("Unterminated array; expected ']'");
				if (item !== CLOSE) { dest = { arr }; token = item; continue next; }
				open.pop();
				value = arr;
			} else if (token.t === V_INLINE) {
				// Its own definition block: dotted keys inside cannot extend
				// outer tables and vice versa.
				const outerBlock = this.block;
				this.block++;
				const tbl = this.newTable(K_DOTTED);
				open.push({ tbl, outerBlock, dest });
				const k = sc.scanInlineKey();
				if (k === EOF) throw sc.err("Unterminated inline table; expected '}'");
				if (k !== CLOSE) { dest = { table: tbl, path: this.readKeyPath(k) }; token = sc.scanValueRequired(); continue next; }
				this.closeInline(open.pop());
				value = tbl;
			} else {
				value = token.t === V_DATETIME ? this.onDateTime(truncateFractionalSeconds(token.v), token.kind) : token.v;
			}
			// Deliver complete values outwards until a container wants more.
			for (;;) {
				if (dest.arr) dest.arr.push(value);
				else this.assignPath(dest.table, dest.path, value);
				if (open.length === 0) return;
				const top = open[open.length - 1];
				if (top.arr) {
					if (sc.scanListSep(0x5d, "Expected ',' or ']' in an array but found")) {
						const item = sc.scanArrayItem();
						if (item === EOF) throw sc.err("Unterminated array; expected ']'");
						if (item !== CLOSE) { dest = { arr: top.arr }; token = item; continue next; }
					}
					value = top.arr;
				} else {
					// A trailing comma is allowed; a second comma is not (scanInlineKey).
					if (sc.scanListSep(0x7d, "Expected ',' or '}' in an inline table but found")) {
						const k = sc.scanInlineKey();
						if (k === EOF) throw sc.err("Unterminated inline table; expected '}'");
						if (k !== CLOSE) { dest = { table: top.tbl, path: this.readKeyPath(k) }; token = sc.scanValueRequired(); continue next; }
					}
					this.closeInline(top);
					value = top.tbl;
				}
				open.pop();
				dest = top.dest;
			}
		}
	}

	// Inline tables are closed: nothing may extend them later.
	closeInline(frame) {
		this.tmeta.set(frame.tbl, { kind: K_INLINE, block: this.block });
		this.block = frame.outerBlock;
	}

	assignPath(table, path, value) {
		const sc = this.scanner;
		let cur = table;
		for (let i = 0; i < path.length - 1; i++) {
			const seg = path[i];
			if (!Object.hasOwn(cur, seg)) {
				const t = this.newTable(K_DOTTED);
				this.insertKey(cur, seg, t);
				cur = t;
				continue;
			}
			const found = cur[seg];
			const tm = this.tableMeta(found);
			if (!tm) throw sc.errKeyed("Cannot redefine key", seg, "");
			if (!(tm.kind === K_DOTTED && tm.block === this.block)) throw sc.errKeyed("Cannot extend table", seg, " with a dotted key");
			cur = found;
		}
		this.insertKey(cur, path[path.length - 1], value);
	}

	// ── table bookkeeping ──────────────────────────────────────────────────

	tableMeta(v) { return v !== null && typeof v === "object" ? this.tmeta.get(v) : undefined; }

	newTable(kind) {
		const t = {};
		this.tmeta.set(t, { kind, block: this.block });
		return t;
	}

	newArray(kind) {
		const a = [];
		this.ameta.set(a, { kind, block: this.block });
		return a;
	}

	appendAotElem(array) {
		const elem = this.newTable(K_ARRAY_ELEM);
		array.push(elem);
		return elem;
	}

	insertKey(obj, key, value) {
		if (Object.hasOwn(obj, key)) throw this.scanner.errKeyed("Cannot redefine key", key, "");
		// expr_to_js defines own properties: `__proto__` is a key, not the
		// prototype. Every other key on a plain object assigns the same way.
		if (key === "__proto__") Object.defineProperty(obj, key, { value, writable: true, enumerable: true, configurable: true });
		else obj[key] = value;
	}
}

// ── JS binding (TOMLObject.bind.ts: b.bytes from string/buffer/blob,
// otherwise toString, refusing undefined/null; copied) ───────────────────────

// Returns a Buffer over the input's bytes. Bun copies ("shared: copied");
// nothing can run between here and the end of the parse, so only a
// SharedArrayBuffer (writable by other threads) needs the copy.
function inputBytes(input) {
	if (input === undefined || input === null)
		throw codedError(TypeError, "input must be a string, ArrayBuffer, ArrayBufferView or Blob", "ERR_INVALID_ARG_TYPE");
	if (typeof input === "string") return Buffer.from(input, "utf8");	// lone surrogates -> U+FFFD, as Bun
	let view = null;
	if (input instanceof ArrayBuffer || (typeof SharedArrayBuffer === "function" && input instanceof SharedArrayBuffer))
		view = new Uint8Array(input);
	else if (ArrayBuffer.isView(input))
		view = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
	if (view !== null) {
		if (typeof SharedArrayBuffer === "function" && view.buffer instanceof SharedArrayBuffer) return Buffer.from(view);
		return Buffer.from(view.buffer, view.byteOffset, view.byteLength);
	}
	// Bun reads a Blob's bytes synchronously; Node can only read one
	// asynchronously, so this is refused explicitly rather than stringified.
	if (typeof Blob === "function" && input instanceof Blob)
		throw new TypeError("Bun.TOML.parse: a Blob cannot be read synchronously on Node; pass await blob.text()");
	if (typeof input === "symbol") throw new TypeError("Cannot convert a symbol to a string");	// JSC's wording
	return Buffer.from(`${input}`, "utf8");
}

export function parseWith(input, onDateTime) {
	const src = inputBytes(input);
	// with_text_format_source: Bun's source positions are i32.
	if (src.length > MAX_PARSEABLE_LEN)
		throw codedError(RangeError, `The value of "input.byteLength" is out of range. It must be <= ${MAX_PARSEABLE_LEN}. Received ${src.length}`, "ERR_OUT_OF_RANGE");
	return new Parser(src, onDateTime).parseRoot();
}

export function parse(input) {
	return parseWith(input, dateTimeString);
}
