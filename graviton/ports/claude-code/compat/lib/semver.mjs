// Bun.semver.{order, satisfies} -- port of Bun bun-v1.4.4 src/semver (Rust):
// Version.rs (parse, parse_exact, order_without_build, Tag), SemverQuery.rs
// (parse_with, Token::to_range, Group/List/Query), SemverRange.rs (Range,
// Comparator) and src/semver_jsc/SemverObject.rs (the JS entry points).
// Inputs are byte strings (ASCII-only, checked by the entry points); version
// numbers are u64, so BigInt here.

const U64_MAX = (1n << 64n) - 1n;
const W_NONE = 0, W_MAJOR = 1, W_MINOR = 2, W_PATCH = 3;
const OP_UNSET = 0, OP_EQL = 1, OP_LT = 3, OP_LTE = 4, OP_GT = 5, OP_GTE = 6;
const T_NONE = 0, T_GT = 1, T_GTE = 2, T_LT = 3, T_LTE = 4, T_VERSION = 5, T_TILDA = 6, T_CARET = 7;
const isDigit = (c) => c >= 0x30 && c <= 0x39;
const isAlpha = (c) => (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a);
const isWs = (c) => c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d || c === 0x0b || c === 0x0c;

// bun_core::parse_unsigned::<u64>(s, 10): null for empty, non-digit, overflow.
function parseU64(s) {
	if (s.length === 0) return null;
	for (let i = 0; i < s.length; i++) if (!isDigit(s.charCodeAt(i))) return null;
	const v = BigInt(s);
	return v > U64_MAX ? null : v;
}

// Tag strings: `pre`/`build` are {s, set}. Tag::eql compares pre hashes;
// measured against stock Bun, an explicitly empty prerelease ("1.0.0-") equals
// the default one, so equality is by content (`set` is informational only).
const NOSTR = Object.freeze({ s: "", set: false });
const ext = (s) => ({ s, set: true });
const tag = (pre = NOSTR, build = NOSTR) => ({ pre, build });
const ZERO_PRE = tag(ext("0"), NOSTR);
const DEFAULT_TAG = tag();
const hasPre = (t) => t.pre.s.length > 0;
const hasBuild = (t) => t.build.s.length > 0;
const tagEql = (a, b) => a.pre.s === b.pre.s;
const strOrder = (a, b) => (a < b ? -1 : a > b ? 1 : 0);	// byte order (ASCII)

function orderPre(a, b) {
	const L = a.pre.s.split("."), R = b.pre.s.split(".");
	for (let i = 0; ; i++) {
		const l = L[i], r = R[i];
		if (l === undefined && r === undefined) return 0;
		if (r === undefined) return 1;
		if (l === undefined) return -1;
		const lu = parseU64(l), ru = parseU64(r);
		if (lu !== null && ru === null) return -1;
		if (lu === null && ru !== null) return 1;
		if (lu === null && ru === null) { const o = strOrder(l, r); if (o) return o; continue; }
		if (lu < ru) return -1;
		if (lu > ru) return 1;
	}
}

function tagOrderWithoutBuild(a, b) {
	if (hasPre(a) && hasPre(b)) return orderPre(a, b);
	return strOrder(a.pre.s, b.pre.s);
}

const version = (major = 0n, minor = 0n, patch = 0n, t = DEFAULT_TAG) => ({ major, minor, patch, tag: t });
const isZero = (v) => v.major === 0n && v.minor === 0n && v.patch === 0n;
const versionEql = (a, b) => a.major === b.major && a.minor === b.minor && a.patch === b.patch && tagEql(b.tag, a.tag);

function orderWithoutTag(l, r) {
	for (const k of ["major", "minor", "patch"]) {
		if (l[k] < r[k]) return -1;
		if (l[k] > r[k]) return 1;
	}
	if (hasPre(l.tag)) { if (!hasPre(r.tag)) return -1; }
	else if (hasPre(r.tag)) return 1;
	return 0;
}

function orderWithoutBuild(l, r) {
	const o = orderWithoutTag(l, r);
	return o !== 0 ? o : tagOrderWithoutBuild(l.tag, r.tag);
}

// Tag::parse_with_pre_count over s (a suffix starting at the '-'/'+'/letter).
function parseTag(s, initialPreCount) {
	let build = 0, pre = initialPreCount;
	for (let k = 0; k < s.length; k++) {
		const c = s.charCodeAt(k);
		if (c === 0x20) break;
		if (c === 0x2b) build++;
		else if (c === 0x2d) pre++;
	}
	const result = { tag: tag(), len: 0 };
	if (build === 0 && pre === 0) return result;
	const S_NONE = 0, S_PRE = 1, S_BUILD = 2;
	let state = S_NONE, start = 0, i = 0;
	let preStr = NOSTR, buildStr = NOSTR;
	let stopped = false;
	while (i < s.length) {
		const c = s.charCodeAt(i);
		if (c === 0x2b) {
			if (state === S_PRE || (state === S_NONE && initialPreCount > 0)) preStr = ext(s.slice(start, i));
			if (state !== S_BUILD) { state = S_BUILD; start = i + 1; }
		} else if (c === 0x2d) {
			if (state === S_NONE) { state = S_PRE; start = i + 1; }
		} else if (isAlpha(c) || isDigit(c) || c === 0x2e) {
			/* tag character */
		} else {
			if (state === S_PRE) { preStr = ext(s.slice(start, i)); state = S_NONE; }
			else if (state === S_BUILD) { buildStr = ext(s.slice(start, i)); state = S_NONE; }
			stopped = true;
			break;
		}
		i++;
	}
	if (!stopped && state === S_NONE && initialPreCount > 0) { state = S_PRE; start = 0; }
	if (stopped && state === S_NONE && initialPreCount > 0) { state = S_PRE; start = 0; }
	if (state === S_PRE) preStr = ext(s.slice(start, i));
	else if (state === S_BUILD) buildStr = ext(s.slice(start, i));
	result.tag = tag(preStr, buildStr);
	result.len = i;
	return result;
}

// Version::parse over the byte string s. Returns ParseResult.
export function parseVersion(s) {
	const r = { wildcard: W_NONE, valid: true, overflow: false, major: null, minor: null, patch: null, tag: DEFAULT_TAG, len: 0 };
	let part = 0;
	if (s.length === 0) { r.valid = false; return r; }
	let isDone = false;
	let i = s.length;
	for (let c = 0; c < s.length; c++) {
		const ch = s.charCodeAt(c);
		if (isWs(ch) || ch === 0x76 /* v */ || ch === 0x3d /* = */) continue;
		i = c;
		break;
	}
	if (i === s.length) { r.valid = false; r.wildcard = W_MAJOR; r.len = i; return r; }
	while (i < s.length) {
		if (isDone) break;
		const ch = s.charCodeAt(i);
		if (ch === 0x20) { isDone = true; break; }
		if (ch === 0x7c || ch === 0x5e || ch === 0x23 || ch === 0x26 || ch === 0x25 || ch === 0x21) {
			isDone = true; i = i > 0 ? i - 1 : 0; break;
		}
		if (isDigit(ch)) {
			const ps = i;
			i++;
			while (i < s.length && isDigit(s.charCodeAt(i))) i++;
			if (part < 3) {
				const n = parseU64(s.slice(ps, i));
				if (n === null) {
					r.valid = false;
					if (r.wildcard === W_NONE) { r.overflow = true; isDone = true; break; }
				}
				if (part === 0) r.major = n; else if (part === 1) r.minor = n; else r.patch = n;
				part++;
			}
			if (i < s.length && s.charCodeAt(i) === 0x2e && part !== 3) i++;
			continue;
		}
		if (ch === 0x2e) { r.valid = false; isDone = true; break; }
		if (ch === 0x2d || ch === 0x2b) {
			const fewest = ch === 0x2b ? 1 : 2;
			if (part < fewest && r.wildcard === W_NONE) { r.valid = false; isDone = true; break; }
			const t = parseTag(s.slice(i), 0);
			r.tag = t.tag;
			i += t.len;
			break;
		}
		if (ch === 0x78 || ch === 0x2a || ch === 0x58) {
			if (i > 0 && isDigit(s.charCodeAt(i - 1))) r.valid = false;
			i++;
			while (i < s.length && [0x78, 0x2a, 0x58].includes(s.charCodeAt(i))) i++;
			if (i < s.length && s.charCodeAt(i) === 0x2e) i++;
			if (r.wildcard === W_NONE) {
				if (part === 0) { r.wildcard = W_MAJOR; part = 1; }
				else if (part === 1) { r.wildcard = W_MINOR; part = 2; }
				else if (part === 2) { r.wildcard = W_PATCH; part = 3; }
			}
			continue;
		}
		if (r.wildcard === W_NONE && part >= 2 && isAlpha(ch)) {
			const t = parseTag(s.slice(i), 1);
			r.tag = t.tag;
			i += t.len;
			isDone = true;
			break;
		}
		r.valid = false;
		isDone = true;
		break;
	}
	if (r.wildcard === W_NONE) {
		if (part === 0) { r.valid = false; r.wildcard = W_MAJOR; }
		else if (part === 1) r.wildcard = W_MINOR;
		else if (part === 2) r.wildcard = W_PATCH;
	}
	r.len = i;
	return r;
}

const pmin = (r) => version(r.major ?? 0n, r.minor ?? 0n, r.patch ?? 0n, r.tag);
const pmax = (r) => version(r.major ?? U64_MAX, r.minor ?? U64_MAX, r.patch ?? U64_MAX, r.tag);

function trimWs(s) {
	let a = 0, b = s.length;
	while (a < b && isWs(s.charCodeAt(a))) a++;
	while (b > a && isWs(s.charCodeAt(b - 1))) b--;
	return s.slice(a, b);
}

function parseExact(s) {
	const t = trimWs(s);
	const p = parseVersion(t);
	return p.valid && p.wildcard === W_NONE && p.len === t.length ? pmin(p) : null;
}

// --- ranges ------------------------------------------------------------------
const cmp = (op = OP_UNSET, v = version()) => ({ op, version: v });
const rng = (left = cmp(), right = cmp()) => ({ left, right });
const nullSet = () => cmp(OP_LT, version(0n, 0n, 0n, ZERO_PRE));
const ltNextMajor = (M) => M + 1n <= U64_MAX ? cmp(OP_LT, version(M + 1n, 0n, 0n, ZERO_PRE)) : cmp(OP_LTE, version(U64_MAX, U64_MAX, U64_MAX));
const ltNextMinor = (M, m) => m + 1n <= U64_MAX ? cmp(OP_LT, version(M, m + 1n, 0n, ZERO_PRE)) : cmp(OP_LTE, version(M, U64_MAX, U64_MAX));
const ltNextPatch = (M, m, p) => p + 1n <= U64_MAX ? cmp(OP_LT, version(M, m, p + 1n, ZERO_PRE)) : cmp(OP_LTE, version(M, m, U64_MAX));
const gteNextMajor = (M, t) => M + 1n <= U64_MAX ? cmp(OP_GTE, version(M + 1n, 0n, 0n, t)) : nullSet();
const gteNextMinor = (M, m, t) => m + 1n <= U64_MAX ? cmp(OP_GTE, version(M, m + 1n, 0n, t)) : gteNextMajor(M, t);

function initWildcard(v, wc, floor) {
	if (wc === W_NONE) return rng(cmp(OP_EQL, v));
	if (wc === W_MAJOR) return rng(cmp(OP_GTE, version(0n, 0n, 0n, floor)));
	if (wc === W_MINOR) return rng(ltNextMajor(v.major), cmp(OP_GTE, version(v.major, 0n, 0n, floor)));
	return rng(ltNextMinor(v.major, v.minor), cmp(OP_GTE, version(v.major, v.minor, 0n, floor)));
}

function comparatorSatisfies(c, v) {
	const o = orderWithoutBuild(v, c.version);
	if (o === 0) return c.op === OP_LTE || c.op === OP_GTE || c.op === OP_EQL;
	if (o === 1) return c.op === OP_GT || c.op === OP_GTE;
	if (c.op === OP_GTE) return isZero(c.version) && !hasPre(c.version.tag);
	return c.op === OP_LT || c.op === OP_LTE;
}

function rangeSatisfies(r, v, pre) {
	const hasLeft = r.left.op !== OP_UNSET, hasRight = r.right.op !== OP_UNSET;
	if (!hasLeft) return true;
	if (pre) {
		pre.matched = pre.matched
			|| (hasPre(r.left.version.tag) && v.patch === r.left.version.patch && v.minor === r.left.version.minor && v.major === r.left.version.major)
			|| (hasRight && hasPre(r.right.version.tag) && v.patch === r.right.version.patch && v.minor === r.right.version.minor && v.major === r.right.version.major);
	}
	if (!comparatorSatisfies(r.left, v)) return false;
	if (hasRight && !comparatorSatisfies(r.right, v)) return false;
	return true;
}

function toRange(tokTag, tokWild, p, floor) {
	const wildcardIsWritten = p.valid;
	if (tokTag === T_CARET) {
		let r = rng();
		done: {
			if (p.major === null) { if (wildcardIsWritten) r = initWildcard(pmin(p), W_MAJOR, floor); break done; }
			const major = p.major;
			r.left = cmp(OP_GTE, version(major, 0n, 0n, floor));
			if (p.minor !== null) {
				r.left.version.minor = p.minor;
				if (p.patch !== null) {
					r.left.version.patch = p.patch;
					r.left.version.tag = p.tag;
					if (major === 0n) {
						if (!hasPre(p.tag)) r.left.version.tag = tag(floor.pre, r.left.version.tag.build);
						r.right = p.minor === 0n ? ltNextPatch(0n, 0n, p.patch) : ltNextMinor(0n, p.minor);
						break done;
					}
				} else if (major === 0n) {
					r.right = ltNextMinor(0n, p.minor);
					break done;
				}
			}
			r.right = ltNextMajor(major);
		}
		return r;
	}
	if (tokTag === T_TILDA) {
		let r = rng();
		done: {
			if (p.major === null) { if (wildcardIsWritten) r = initWildcard(pmin(p), W_MAJOR, floor); break done; }
			const major = p.major;
			r.left = cmp(OP_GTE, version(major));
			if (p.minor !== null) {
				r.left.version.minor = p.minor;
				if (p.patch !== null) { r.left.version.patch = p.patch; r.left.version.tag = p.tag; }
				r.right = ltNextMinor(major, p.minor);
				break done;
			}
			r.right = ltNextMajor(major);
		}
		return r;
	}
	if (tokTag === T_VERSION) {
		if (tokWild !== W_NONE) return initWildcard(pmin(p), tokWild, floor);
		return rng(cmp(OP_EQL, pmin(p)));
	}
	const op = tokTag === T_GT ? OP_GT : tokTag === T_GTE ? OP_GTE : tokTag === T_LT ? OP_LT : OP_LTE;
	const major = p.major ?? 0n, minor = p.minor ?? 0n;
	const firstOfPartial = (t) => cmp(op, version(major, minor, 0n, t));
	let left;
	if (tokWild === W_NONE) left = cmp(op, pmin(p));
	else if ((op === OP_GT || op === OP_LT) && tokWild === W_MAJOR && wildcardIsWritten) left = nullSet();
	else if (tokWild === W_MAJOR) return initWildcard(pmin(p), W_MAJOR, floor);
	else if (op === OP_GTE) left = firstOfPartial(floor);
	else if (op === OP_LT) left = firstOfPartial(ZERO_PRE);
	else if (op === OP_GT && tokWild === W_MINOR) left = gteNextMajor(major, floor);
	else if (op === OP_GT && tokWild === W_PATCH) left = gteNextMinor(major, minor, floor);
	else if (tokWild === W_MINOR) left = ltNextMajor(major);
	else left = ltNextMinor(major, minor);
	return rng(left);
}

// Group = [List...] (OR); List = [Range...] (AND).
function parseRange(input, floor = DEFAULT_TAG) {
	const group = { lists: [[rng()]] };
	const orRange = (r) => {
		const g = group.lists;
		if (g.length === 1 && g[0].length === 1 && g[0][0].left.op === OP_UNSET) { g[0][0] = r; return; }
		g.push([r]);
	};
	const andRange = (r) => {
		const l = group.lists[group.lists.length - 1];
		if (l.length === 1 && l[0].left.op === OP_UNSET && l[0].right.op === OP_UNSET) { l[0] = r; return; }
		l.push(r);
	};
	const nullGroup = () => ({ lists: [[rng(nullSet())]] });
	const lwsLen = (k) => { let n = 0; while (k + n < input.length && isWs(input.charCodeAt(k + n))) n++; return n; };
	let i = 0, tokTag = T_NONE, tokWild = W_NONE, isOr = false, afterComparator = false;
	while (i < input.length) {
		let skipRound = false, noOperator = false;
		const c = input.charCodeAt(i);
		const skipSpaces = () => { while (i < input.length && input.charCodeAt(i) === 0x20) i++; };
		if (c === 0x3e || c === 0x3c) {
			i++; skipSpaces();
			const orEq = i < input.length && input.charCodeAt(i) === 0x3d;
			if (orEq) i++;
			tokTag = c === 0x3e ? (orEq ? T_GTE : T_GT) : (orEq ? T_LTE : T_LT);
			skipSpaces();
		} else if (c === 0x3d || c === 0x76) {
			tokTag = T_VERSION; noOperator = true; i++; skipSpaces();
		} else if (c === 0x7e) {
			tokTag = T_TILDA; i++;
			if (i < input.length && input.charCodeAt(i) === 0x3e) i++;
			skipSpaces();
		} else if (c === 0x5e) {
			tokTag = T_CARET; i++; skipSpaces();
		} else if (isDigit(c) || c === 0x58 || c === 0x78 || c === 0x2a) {
			tokTag = T_VERSION; noOperator = true;
		} else if (c === 0x7c) {
			i++;
			while (i < input.length && input.charCodeAt(i) === 0x7c) i++;
			skipSpaces();
			isOr = true; tokTag = T_NONE; skipRound = true;
		} else if (c === 0x2d) {
			i++; skipSpaces();
		} else if (c === 0x20) {
			i++; skipSpaces(); skipRound = true;
		} else {
			i++; tokTag = T_NONE; afterComparator = false;
			while (i < input.length && input.charCodeAt(i) !== 0x20 && input.charCodeAt(i) !== 0x7c) i++;
			skipRound = true;
		}
		if (skipRound) continue;
		const p = parseVersion(input.slice(i));
		if (p.overflow) return nullGroup();
		tokWild = p.wildcard;
		i += p.len;
		const rollback = i;
		const maybeHyphen = i < input.length && (input.charCodeAt(i) === 0x20 || input.charCodeAt(i) === 0x2d);
		let hyphenate = false;
		if (maybeHyphen) {
			hyphenate = (() => {
				i += lwsLen(i);
				if (!(i < input.length && input.charCodeAt(i) === 0x2d)) return false;
				i++;
				i += lwsLen(i);
				if (i === input.length) return false;
				if (input.charCodeAt(i) === 0x76 || input.charCodeAt(i) === 0x3d) i++;
				if (i === input.length) return false;
				i += lwsLen(i);
				if (i === input.length) return false;
				const d = input.charCodeAt(i);
				return isDigit(d) || d === 0x58 || d === 0x78 || d === 0x2a;
			})();
		}
		if (!hyphenate) i = rollback;
		if (!hyphenate) i += 1;
		const valid = p.valid && !hyphenate;
		if (noOperator && !(afterComparator && valid)) isOr = true;
		let r;
		if (hyphenate) {
			const p2 = parseVersion(input.slice(i));
			if (p2.overflow) return nullGroup();
			const v2 = pmin(p2);
			i += p2.len + 1;
			let v1 = pmin(p);
			if (!hasPre(v1.tag)) v1 = version(v1.major, v1.minor, v1.patch, tag(floor.pre, v1.tag.build));
			let right;
			if (p2.wildcard === W_MAJOR) right = cmp();
			else if (p2.wildcard === W_MINOR) right = ltNextMajor(v2.major);
			else if (p2.wildcard === W_PATCH) right = ltNextMinor(v2.major, v2.minor);
			else if (hasPre(floor) && !hasPre(v2.tag)) right = ltNextPatch(v2.major, v2.minor, v2.patch);
			else right = cmp(OP_LTE, v2);
			r = rng(cmp(OP_GTE, v1), right);
		} else if (tokTag === T_NONE) {
			tokWild = W_NONE;
			continue;
		} else r = toRange(tokTag, tokWild, p, floor);
		if (isOr) orRange(r); else andRange(r);
		isOr = false;
		tokWild = W_NONE;
		afterComparator = valid && rollback < input.length && input.charCodeAt(rollback) === 0x20;
	}
	return group;
}

function groupExactVersion(g) {
	if (g.lists.length !== 1) return null;
	const l = g.lists[0];
	if (l.length !== 1) return null;
	const r = l[0];
	return r.left.op === OP_EQL && r.right.op === OP_UNSET ? r.left.version : null;
}

function groupSatisfies(g, v) {
	if (hasPre(v.tag)) {
		for (const list of g.lists) {
			const pre = { matched: false };
			let ok = true;
			for (const r of list) if (!rangeSatisfies(r, v, pre)) { ok = false; break; }
			if (ok && pre.matched) return true;
		}
		return false;
	}
	for (const list of g.lists) {
		let ok = true;
		for (const r of list) if (!rangeSatisfies(r, v, null)) { ok = false; break; }
		if (ok) return true;
	}
	return false;
}

// JS strings reach Bun's Rust as UTF-8; the entry points reject non-ASCII first.
const isAllAscii = (s) => { for (let i = 0; i < s.length; i++) if (s.charCodeAt(i) > 0x7f) return false; return true; };

export function order(a, b) {
	const l = String(a), r = String(b);
	if (!isAllAscii(l) || !isAllAscii(r)) return 0;
	const lp = parseVersion(l), rp = parseVersion(r);
	for (const [res, inp] of [[lp, l], [rp, r]])
		if (!res.valid) throw new Error(`Invalid SemVer: ${inp}\n`);
	return orderWithoutBuild(pmax(lp), pmax(rp));
}

export function satisfies(v, range) {
	const vs = String(v), rs = String(range);
	if (!isAllAscii(vs) || !isAllAscii(rs)) return false;
	const pv = parseExact(vs);
	if (!pv) return false;
	const g = parseRange(rs);
	const exact = groupExactVersion(g);
	if (exact) return versionEql(pv, exact);
	return groupSatisfies(g, pv);
}
