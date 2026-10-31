// Shared by check-spawn.mjs and check-net.mjs: compares one scenario's
// observations recorded on a reference platform (stock Bun on Linux) with the
// same scenario run elsewhere (Node + compat on Haiku).
//
// On the same platform the comparison is exact JSON equality, nothing else.
// Across platforms, exactly these named differences are accepted, each one
// checked against the two platforms' own tables (recorded next to the
// observations, see META_KEY) and reported, never dropped:
//
//   errno-value    the same error `code`, and each side's `errno` is its own
//                  platform's number for that code in the same convention:
//                  libuv's (negative: -111 on Linux, the raw negative value on
//                  Haiku, whose errno constants are already negative) or the
//                  native errno (98 on Linux, -2147454954 on Haiku). A value in
//                  neither convention, e.g. a sign flipped on one side, is a
//                  mismatch.
//   signal-table   Bun's "signal must be one of ..." message: each side must
//                  list its own platform's signals 1..31 in number order. The
//                  numbering differs (Haiku SIGBUS is 30, Linux 7).
//   <scenario rules> SCENARIO_RULES below: one scenario's measured platform
//                  property (e.g. no SO_REUSEPORT sharing on Haiku).
//   unobservable   the other side recorded an "unobservable-on-<platform>"
//                  marker with a reason: the platform keeps no such value. The
//                  reference value is shown; the rest of the scenario must match.
export const META_KEY = "__platform";

export function splitRecording(entries) {
	const map = new Map();
	let meta = null;
	for (const [name, obs] of entries) {
		if (name === META_KEY) meta = obs;
		else map.set(name, obs);
	}
	if (!meta) throw new Error(`recording has no ${META_KEY} entry (re-record it with the current harness)`);
	return { meta, map };
}

const isObj = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const unobsKey = (v) => (isObj(v) ? Object.keys(v).find((k) => k.startsWith("unobservable-on-")) : undefined);

// The conventions `value` is in for `code` on that platform. On Haiku both are
// the same number (the errno constants are negative, and libuv uses them as is).
function errnoConventions(meta, code, value) {
	const n = meta.errno?.[code];
	const out = new Set();
	if (typeof n !== "number") return out;
	if (value === (n > 0 ? -n : n)) out.add("libuv");
	if (value === n) out.add("native");
	return out;
}
const sharedConvention = (ma, ca, va, mb, cb, vb) => {
	const a = errnoConventions(ma, ca, va), b = errnoConventions(mb, cb, vb);
	return [...a].find((c) => b.has(c)) ?? null;
};

// "signal must be one of 'SIGHUP', ..., or 'SIGSYS'": the names, or null.
function signalList(s) {
	const m = /^signal must be one of (.*) or '([A-Z0-9]+)'$/.exec(s);
	if (!m) return null;
	const names = [...m[1].matchAll(/'([A-Z0-9]+)'(?:, |$)/g)].map((x) => x[1]);
	if (names.join("', '") !== m[1].replace(/^'|'$/g, "")) return null;
	return [...names, m[2]];
}
function isOwnSignalTable(meta, names) {
	const nums = names.map((n) => meta.signals?.[n]);
	if (nums.some((n) => typeof n !== "number" || n < 1 || n > 31)) return false;
	for (let i = 1; i < nums.length; i++) if (nums[i] <= nums[i - 1]) return false;
	const distinct = new Set(Object.values(meta.signals).filter((n) => n >= 1 && n <= 31));
	return nums.length === distinct.size;
}

// Differences that belong to one scenario, each a measured property of the
// platform (the evidence is cited in LIMITATIONS.md under the same name). A rule
// applies only to its scenario and platform pair, and only when its check, which
// pins both sides' exact values, passes; anything else is still a mismatch.
const same = (x, y) => JSON.stringify(x) === JSON.stringify(y);
const without = (o, keys) => Object.fromEntries(Object.entries(o).filter(([k]) => !keys.includes(k)));
export const SCENARIO_RULES = [
	{
		name: "exec-directory-errno", scenario: "directory as binary", ref: "linux", got: "haiku",
		reason: "exec of a directory: Linux execve says EACCES; Haiku's libroot exec test (runtime_loader open_executable) says EISDIR",
		check(a, b, ma, mb) {
			if (a.code !== "EACCES" || b.code !== "EISDIR") return false;
			if (!sharedConvention(ma, a.code, a.errno, mb, b.code, b.errno)) return false;
			const tail = (m) => m.slice(m.indexOf(", "));
			return a.message.startsWith("EACCES: ") && b.message.startsWith("EISDIR: ") && tail(a.message) === tail(b.message)
				&& same(without(a, ["code", "errno", "message"]), without(b, ["code", "errno", "message"]));
		},
	},
	{
		name: "no-reuseport-sharing", scenario: "serve: development:false sets SO_REUSEPORT (second bind succeeds)", ref: "linux", got: "haiku",
		reason: "Haiku TCP has no SO_REUSEPORT port sharing (libuv: ENOTSUP; the kernel's bind ignores the option), so the second bind is EADDRINUSE, the same error as with reusePort:false",
		check: (a, b) => a.prodTwice === "bound" && same(a.explicitNoReuse, b.explicitNoReuse) && same(b.prodTwice, b.explicitNoReuse),
	},
	{
		name: "localhost-resolution", scenario: "serve: hostname localhost", ref: "linux", got: "haiku",
		reason: "the hosts files differ: Ubuntu maps localhost to 127.0.0.1 only (::1 is ip6-localhost), Haiku to ::1 and 127.0.0.1; the listener takes the first IPv6 answer, else the first IPv4 one",
		check(a, b) {
			const expect = (r) => (r.resolved.includes("::1") ? { v4: "ECONNREFUSED", v6: "connected" } : { v4: "connected", v6: "ECONNREFUSED" });
			const ok = (r) => Array.isArray(r.resolved) && r.resolved.length > 0 && r.v4 === expect(r).v4 && r.v6 === expect(r).v6;
			return ok(a) && ok(b) && same(without(a, ["v4", "v6", "resolved"]), without(b, ["v4", "v6", "resolved"]));
		},
	},
];

// Known platform defects: still mismatches (the test fails until they are
// fixed), but labelled so the output names the defect instead of a compat bug.
export const KNOWN_DEFECTS = {
	"bad interpreter": "Haiku: exec of a script whose #! interpreter is missing succeeds, and the child dies in runtime_loader (exit 3) instead of execve failing with ENOENT",
};

// Returns { same: true } for exact equality, otherwise { same: false, ok, notes }
// where ok means every difference is one of the named ones above.
export function compare(ref, got, refMeta, gotMeta, scenario) {
	if (JSON.stringify(ref) === JSON.stringify(got)) return { same: true };
	if (refMeta.platform === gotMeta.platform) return { same: false, ok: false, notes: [] };
	const rule = SCENARIO_RULES.find((r) => r.scenario === scenario && r.ref === refMeta.platform && r.got === gotMeta.platform);
	if (rule && rule.check(ref, got, refMeta, gotMeta)) return { same: false, ok: true, notes: [{ kind: rule.name, path: "", reason: rule.reason, ref, got }] };
	const notes = [];
	let ok = true;
	const walk = (a, b, path) => {
		const uk = unobsKey(b);
		if (uk) { notes.push({ kind: "unobservable", path, reason: b[uk], got: b, ref: a }); return; }
		if (Array.isArray(a) && Array.isArray(b)) {
			if (a.length !== b.length) { ok = false; notes.push({ kind: "MISMATCH", path, ref: a, got: b }); return; }
			a.forEach((x, i) => walk(x, b[i], `${path}[${i}]`));
			return;
		}
		if (isObj(a) && isObj(b)) {
			const ka = Object.keys(a), kb = Object.keys(b);
			if (ka.join() !== kb.join()) { ok = false; notes.push({ kind: "MISMATCH", path, ref: a, got: b }); return; }
			for (const k of ka) {
				if (k === "errno" && typeof a.code === "string" && a.code === b.code && typeof a.errno === "number" && typeof b.errno === "number" && a.errno !== b.errno) {
					const c = sharedConvention(refMeta, a.code, a.errno, gotMeta, b.code, b.errno);
					if (c) notes.push({ kind: "errno-value", path: `${path}.errno`, code: a.code, convention: c, ref: a.errno, got: b.errno });
					else { ok = false; notes.push({ kind: "MISMATCH", path: `${path}.errno`, code: a.code, ref: a.errno, refConventions: [...errnoConventions(refMeta, a.code, a.errno)], got: b.errno, gotConventions: [...errnoConventions(gotMeta, b.code, b.errno)] }); }
					continue;
				}
				walk(a[k], b[k], `${path}.${k}`);
			}
			return;
		}
		if (typeof a === "string" && typeof b === "string" && a !== b) {
			const la = signalList(a), lb = signalList(b);
			if (la && lb && isOwnSignalTable(refMeta, la) && isOwnSignalTable(gotMeta, lb)) { notes.push({ kind: "signal-table", path, ref: a, got: b }); return; }
		}
		if (JSON.stringify(a) !== JSON.stringify(b)) { ok = false; notes.push({ kind: "MISMATCH", path, ref: a, got: b }); }
	};
	walk(ref, got, "");
	return { same: false, ok, notes };
}

export function printNotes(notes, indent = "    ") {
	for (const n of notes) {
		const { kind, path, ...rest } = n;
		console.log(`${indent}${kind} at ${path || "(root)"}: ${JSON.stringify(rest)}`);
	}
}
