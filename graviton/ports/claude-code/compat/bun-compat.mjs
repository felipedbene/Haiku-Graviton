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
