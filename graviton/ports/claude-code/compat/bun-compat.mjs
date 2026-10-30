// bun-compat.mjs -- the runtime surface the relocated Claude Code bundle expects
// from Bun, implemented on Node. Preloaded with `node --import`.
//
// Rules this file follows (see PATCHES.md): every member is a real implementation
// diff-tested against stock Bun 1.4.4; anything not implemented is ABSENT, never
// a stub, so feature-detecting call sites see the truth and unguarded ones fail
// loudly. Each implemented API is listed in PATCHES.md.

import { registerHooks } from "node:module";
import { readFileSync } from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";
import { dirname, join } from "node:path";

const HERE = dirname(fileURLToPath(import.meta.url));

// --- Bun "text" loader -------------------------------------------------------
// Bun loads files marked `text` in the module graph as their contents: import
// and require both yield the string. Node has no such loader, so a load hook
// serves exactly the files relocate-bundle.py recorded as text (by file, not by
// extension: some .mjs files in the graph are text, not code). CommonJS source
// is used because require() then returns the string itself -- what Bun returns
// -- and an ESM import of CommonJS gets that same string as its default.
const LOADERS = JSON.parse(readFileSync(join(HERE, "LOADERS.json"), "utf8"));
const TEXT = new Set(LOADERS.text.map((p) => pathToFileURL(p).href));

registerHooks({
	load(url, context, nextLoad) {
		if (TEXT.has(url)) {
			const text = readFileSync(fileURLToPath(url), "utf8");
			return { format: "commonjs", source: `module.exports=${JSON.stringify(text)};`,
				shortCircuit: true };
		}
		return nextLoad(url, context);
	},
});

// --- globalThis.Bun ----------------------------------------------------------
// Only members with a real, diff-tested implementation are defined. Absent
// members stay absent so feature detection at the call sites sees the truth.
import { makeHash } from "./lib/hash.mjs";
import { stringWidth } from "./lib/width.mjs";
import { stripANSI } from "./lib/strip.mjs";
import { sliceAnsi } from "./lib/slice.mjs";
import { wrapAnsi } from "./lib/wrap.mjs";
import { order as semverOrder, satisfies as semverSatisfies } from "./lib/semver.mjs";
import { deepEquals } from "./lib/deepequals.mjs";
import { bunWhich } from "./lib/which.mjs";
import * as misc from "./lib/misc.mjs";

const Bun = {
	hash: makeHash(),			// vectors: hash.vectors.json, 3353/3353
	stringWidth,				// vectors: width.*, 3608650/3608650
	stripANSI,				// vectors: strip.cases.json, 20008/20008
	sliceAnsi,				// vectors: slice.cases.json, 40900/40900
	wrapAnsi,				// vectors: wrap.cases.json, 40000/40000
	semver: { order: semverOrder, satisfies: semverSatisfies },	// semver.cases.json, 100009/100009
	deepEquals,				// deepequals.cases.json, 354/354
	which: bunWhich,			// which.cases.json, 38/38
	sleepSync: misc.sleepSync,
	gc: misc.gc,
	SHA256: misc.SHA256,
	zstdDecompress: misc.zstdDecompress,
	zstdDecompressSync: misc.zstdDecompressSync,
	generateHeapSnapshot: misc.generateHeapSnapshot,
	file: misc.file,
	stdin: misc.stdin,
	main: misc.main,
	// `unsafe` is a namespace in Bun. The bundle calls Bun.unsafe.setJITPolicy?.()
	// on a timer guarded only by `typeof Bun`, so the namespace must exist; its one
	// member used, a JavaScriptCore JIT knob, has no V8 counterpart and is absent.
	unsafe: {},
};
// Diagnostics only: CLAUDE_COMPAT_TRACE=<file> records every read of a member
// this module does NOT define (behaviour unchanged: the read still yields
// undefined), so a test run shows which absent APIs a real flow reaches.
import { appendFileSync } from "node:fs";
let exposed = Bun;
const TRACE = process.env.CLAUDE_COMPAT_TRACE;
if (TRACE) {
	const seen = new Set();
	const note = (path) => {
		if (seen.has(path)) return;
		seen.add(path);
		const where = (new Error().stack || "").split("\n").slice(3, 5).map((l) => l.trim()).join(" <- ");
		appendFileSync(TRACE, `${process.pid} absent Bun.${path} @ ${where}\n`);
	};
	const wrap = (obj, prefix) => new Proxy(obj, {
		get(t, k, r) {
			if (typeof k === "symbol") return Reflect.get(t, k, r);
			if (!(k in t)) { note(prefix + k); return undefined; }
			const v = Reflect.get(t, k, r);
			return v && typeof v === "object" && prefix === "" ? wrap(v, k + ".") : v;
		},
		has(t, k) {
			if (typeof k !== "symbol" && !(k in t)) note(prefix + k + " (in)");
			return k in t;
		},
	});
	exposed = wrap(Bun, "");
}
Object.defineProperty(globalThis, "Bun", { value: exposed, writable: false, enumerable: false, configurable: false });
