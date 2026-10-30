// analyze-requires.mjs <relocated-tree> <install-prefix> <depsdir> > requires.json
//
// Finds every __ccRequire("<prefix>/chunk.js") call that runs while its module is
// being EVALUATED (top level, or inside a function invoked on the spot), as opposed
// to inside a function that runs later. Node's require(esm) refuses to load a module
// whose static imports lead back into a module that is still evaluating
// (ERR_REQUIRE_CYCLE_MODULE); Bun's import.meta.require tolerates that. Only an
// evaluation-time require can hit it, so those are the sites relocate-bundle.py
// rule R4 has to turn into static imports -- and only the cycle-capable ones (the
// target statically reaches the requiring module), so laziness elsewhere is kept.
//
// Uses acorn (pinned in deps.lock, fetched by fetch-deps.sh into <depsdir>):
// minified code defeats brace counting.
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const [root, prefix, deps] = process.argv.slice(2);
const acorn = await import(pathToFileURL(join(deps, "acorn/dist/acorn.mjs")).href);
const P = prefix + "/";
// Only Bun "js" loader modules are code; LOADERS.json lists the rest by file.
const L = JSON.parse(readFileSync(join(root, "LOADERS.json"), "utf8"));
const notCode = new Set([...L.text, ...L.file, ...L.napi].map((p) => p.slice(P.length)));
const files = readdirSync(root).filter((f) => (f.endsWith(".js") || f === "cli") && !notCode.has(f));
const isFn = (n) => n && (n.type === "FunctionExpression" || n.type === "ArrowFunctionExpression"
	|| n.type === "FunctionDeclaration");

const staticEdges = new Map();
const evalRequires = [];	// {module, target, start, end}
let laterRequires = 0;
for (const f of files) {
	const src = readFileSync(join(root, f), "utf8");
	let ast;
	try {
		ast = acorn.parse(src, { ecmaVersion: "latest", sourceType: "module", allowHashBang: true });
	} catch (e) {
		throw new Error(`${f}: ${e.message}`);
	}
	const deps = new Set();
	for (const n of ast.body) {
		if ((n.type === "ImportDeclaration" || n.type === "ExportNamedDeclaration"
			|| n.type === "ExportAllDeclaration") && n.source?.value?.startsWith(P))
			deps.add(n.source.value.slice(P.length));
	}
	staticEdges.set(f, deps);
	// Walk with an "evaluated now" flag. Function bodies are deferred, except a
	// function that is itself the callee of a call evaluated now (an IIFE).
	const walk = (node, now) => {
		if (!node || typeof node.type !== "string") return;
		if (node.type === "CallExpression" && node.callee.type === "Identifier"
			&& node.callee.name === "__ccRequire" && node.arguments[0]?.type === "Literal"
			&& typeof node.arguments[0].value === "string" && node.arguments[0].value.startsWith(P)) {
			const target = node.arguments[0].value.slice(P.length);
			if (now) evalRequires.push({ module: f, target, start: node.start, end: node.end });
			else laterRequires++;
		}
		for (const key of Object.keys(node)) {
			if (key === "type" || key === "start" || key === "end") continue;
			const v = node[key];
			const visit = (c) => {
				if (!c || typeof c.type !== "string") return;
				let childNow = now;
				if (isFn(c)) childNow = now && node.type === "CallExpression" && key === "callee";
				// Class bodies: methods/getters are deferred, static blocks/fields run now.
				if (c.type === "MethodDefinition" || c.type === "Property" && (c.kind === "get"
					|| c.kind === "set" || c.method)) childNow = false;
				walk(c, childNow);
			};
			Array.isArray(v) ? v.forEach(visit) : visit(v);
		}
	};
	walk(ast, true);
}
const reach = (start) => {
	const seen = new Set([start]), stack = [start];
	while (stack.length) for (const d of staticEdges.get(stack.pop()) ?? []) if (!seen.has(d)) { seen.add(d); stack.push(d); }
	return seen;
};
const cyc = evalRequires.filter((r) => reach(r.target).has(r.module));
// Indirect: the target statically reaches some module that statically reaches the
// requirer -- an ancestor that can still be on the evaluation stack. A superset of
// what can actually cycle (the ancestor may well have finished), reported so the
// diff tests can watch these sites; R4 converts only the direct ones.
const ancestors = new Map();
for (const f of files) for (const d of reach(f)) {
	if (!ancestors.has(d)) ancestors.set(d, new Set());
	ancestors.get(d).add(f);
}
const indirect = evalRequires.filter((r) => !cyc.includes(r)
	&& [...reach(r.target)].some((m) => m !== r.module && ancestors.get(r.module)?.has(m)));
process.stdout.write(JSON.stringify({
	modules: files.length, eval_time_requires: evalRequires.length, deferred_requires: laterRequires,
	r4_sites: cyc, indirect_cycle_capable: indirect,
	other_eval_time: evalRequires.filter((r) => !cyc.includes(r) && !indirect.includes(r)),
}, null, 1) + "\n");
