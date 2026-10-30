#!/usr/bin/env python3
"""Rule R4: turn evaluation-time, cycle-capable __ccRequire() calls into static imports.

  apply-r4.py <relocated-tree> <requires.json> [expected-count]

requires.json is analyze-requires.mjs output for the same tree. For each
`r4_sites` entry (module M requires chunk B while M is evaluating, and B
statically imports M back), the call `__ccRequire("<prefix>/B")` in M becomes a
reference to `import*as __ccR4_<n> from"<prefix>/B"`. A static import cycle is
ordinary ESM, which Node and Bun both evaluate; Node's require(esm) refuses to
enter a cycle with a module that is still evaluating (ERR_REQUIRE_CYCLE_MODULE),
Bun's import.meta.require does not -- this is the one place that difference bites.

Edits in place and appends the sites to RELOCATION.json. The call text must occur
exactly once in its module (so no offset arithmetic across JS/Python string
models), and the site count must equal the expected count for this version.
"""
import hashlib, json, os, sys

EXPECTED = 1	# 2.1.314: chunk-zy2k3hny.js -> chunk-vgzpjjhb.js


def die(msg):
	sys.exit("apply-r4: " + msg)


def main():
	if len(sys.argv) not in (3, 4):
		die("usage: apply-r4.py <tree> <requires.json> [expected-count]")
	tree, req = sys.argv[1], json.load(open(sys.argv[2]))
	expected = int(sys.argv[3]) if len(sys.argv) == 4 else EXPECTED
	rel = json.load(open(os.path.join(tree, "RELOCATION.json")))
	prefix = rel["prefix"]
	sites = req["r4_sites"]
	if len(sites) != expected:
		die("expected %d R4 sites, analysis found %d -- re-review" % (expected, len(sites)))
	if rel["rule_totals"].get("R4"):
		die("R4 already applied to this tree")
	done = []
	for n, s in enumerate(sites):
		path = os.path.join(tree, s["module"])
		src = open(path, encoding="utf-8").read()
		call = '__ccRequire("%s/%s")' % (prefix, s["target"])
		if src.count(call) != 1:
			die("%s: %r occurs %d times, expected exactly 1" % (s["module"], call, src.count(call)))
		ident = "__ccR4_%d" % n
		src = src.replace(call, ident)
		# After the R2 prelude line, so the module's own header stays first.
		head, nl, body = src.partition("\n")
		if "__ccCreateRequire" not in head:
			die("%s: no R2 prelude on line 1" % s["module"])
		src = head + nl + 'import*as %s from"%s/%s";\n' % (ident, prefix, s["target"]) + body
		with open(path, "w", encoding="utf-8") as fh:
			fh.write(src)
		rel["files"][s["module"]] = hashlib.sha256(src.encode()).hexdigest()
		done.append([s["module"], s["target"]])
	rel["rule_totals"]["R4"] = len(done)
	rel["rule_sites"]["R4"] = done
	with open(os.path.join(tree, "RELOCATION.json"), "w") as fh:
		json.dump(rel, fh, indent=1)
	print("R4 applied:", done)


if __name__ == "__main__":
	main()
