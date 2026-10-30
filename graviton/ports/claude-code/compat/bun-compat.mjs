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

const Bun = {
	hash: makeHash(),			// vectors: hash.vectors.json, 3353/3353
	stringWidth,				// vectors: width.*, 3608650/3608650
	stripANSI,				// vectors: strip.cases.json, 20008/20008
	sliceAnsi,				// vectors: slice.cases.json, 40900/40900
	wrapAnsi,				// vectors: wrap.cases.json, 40000/40000
	// `unsafe` is a namespace in Bun. The bundle calls Bun.unsafe.setJITPolicy?.()
	// on a timer guarded only by `typeof Bun`, so the namespace must exist; its one
	// member used, a JavaScriptCore JIT knob, has no V8 counterpart and is absent.
	unsafe: {},
};
Object.defineProperty(globalThis, "Bun", { value: Bun, writable: false, enumerable: false, configurable: false });
