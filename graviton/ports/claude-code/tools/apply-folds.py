#!/usr/bin/env python3
"""Rule P4: restore every constant-folded process.platform / process.arch.

  apply-folds.py <relocated-tree> <folds.json> [--expect N] [--acorn DIR [--node NODE]]

folds.json is find-folds.py output for the same Claude Code build (its linux-arm64
source_sha256 must equal the tree's). Each site's literal becomes the runtime
expression find-folds.py recorded: `"linux"` -> `process.platform`,
`"Platform: linux-arm64"` -> `("Platform: "+process.platform+"-"+process.arch)`,
template text `linux:` -> `${process.platform}:`, `.arm64` -> `[process.arch]`.

Every site is found by its context snippet, which must occur exactly once in its
module, and the text at the recorded offset inside it must be the recorded original;
the site total must equal EXPECTED for this version, so a new upstream version stops
the build until find-folds.py has been re-run and the sites re-reviewed.

P4 subsumes relocate-bundle.py's P1 (switch("linux")) and P2 (switch("arm64")):
those 10 discriminants are P4 sites too. If RELOCATION.json shows P1/P2 already
applied, their sites are verified (the rewritten form occurs exactly once) and
skipped, never applied twice; if the relocation no longer runs P1/P2, P4 applies them.

With --acorn, every rewritten module is parsed with acorn afterwards (via node).
Edits in place; appends the result to RELOCATION.json.
"""
import hashlib, json, os, subprocess, sys, tempfile

EXPECTED = 126		# 2.1.314: find-folds.py over linux-arm64 / darwin-arm64 / linux-x64
VERSION = "2.1.314"

PARSEJS = r"""
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
const [acdir, ...files] = process.argv.slice(2);
const acorn = await import(pathToFileURL(join(acdir, "dist/acorn.mjs")).href);
let bad = 0;
for (const f of files) {
	try { acorn.parse(readFileSync(f, "utf8"), { ecmaVersion: "latest", sourceType: "module", allowHashBang: true }); }
	catch (e) { bad++; console.log("PARSE-FAIL " + f + ": " + e.message); }
}
console.log("parsed " + files.length + " modules, " + bad + " failed");
process.exit(bad ? 1 : 0);
"""


def die(msg):
	sys.exit("apply-folds: " + msg)


def main():
	args = sys.argv[1:]
	opts = {"--expect": None, "--acorn": None, "--node": "node"}
	for k in list(opts):
		if k in args:
			i = args.index(k)
			opts[k] = args[i + 1]
			del args[i:i + 2]
	if len(args) != 2:
		die("usage: apply-folds.py <tree> <folds.json> [--expect N] [--acorn DIR [--node NODE]]")
	tree, folds = args[0], json.load(open(args[1], encoding="utf-8"))
	expected = int(opts["--expect"]) if opts["--expect"] else EXPECTED
	relp = os.path.join(tree, "RELOCATION.json")
	rel = json.load(open(relp))
	summ = folds["summary"]
	if summ["inputs"]["la"]["source_sha256"] != rel["source_sha256"]:
		die("folds.json is for linux-arm64 %s, the tree for %s" % (
			summ["inputs"]["la"]["source_sha256"], rel["source_sha256"]))
	if opts["--expect"] is None and summ["version"] != VERSION:
		die("folds.json is for %s; EXPECTED is for %s -- re-review, then pass --expect" % (
			summ["version"], VERSION))
	sites = folds["sites"]
	if len(sites) != expected:
		die("expected %d fold sites, folds.json has %d -- re-review" % (expected, len(sites)))
	if rel["rule_totals"].get("P4"):
		die("P4 already applied to this tree")
	applied_rules = {r for r in ("P1", "P2") if rel["rule_totals"].get(r)}

	by_mod = {}
	for s in sites:
		by_mod.setdefault(s["module"], []).append(s)
	done, skipped, changed = [], [], []
	for mod in sorted(by_mod):
		path = os.path.join(tree, mod)
		src = open(path, encoding="utf-8").read()
		edits = []
		for s in by_mod[mod]:
			snip, k = s["snippet"], s["snippet_offset"]
			orig, repl = s["original"], s["replacement"]
			if snip[k:k + len(orig)] != orig:
				die("%s@%d: snippet does not hold %r at its offset" % (mod, s["offset"], orig))
			if s["relocate_rule"] in applied_rules:
				# already rewritten by relocate-bundle.py: verify, do not apply again
				want = snip[:k] + repl + snip[k + len(orig):]
				n = src.count(want)
				if n != 1:
					die("%s@%d: %s site: rewritten form occurs %d times, expected 1" % (
						mod, s["offset"], s["relocate_rule"], n))
				skipped.append([mod, s["offset"], s["relocate_rule"]])
				continue
			n = src.count(snip)
			if n != 1:
				die("%s@%d: snippet occurs %d times, expected exactly 1: %r" % (mod, s["offset"], n, snip))
			at = src.index(snip) + k
			edits.append((at, at + len(orig), repl, s))
		edits.sort(key=lambda e: e[0])
		for a, b in zip(edits, edits[1:]):
			if a[1] > b[0]:
				die("%s: overlapping sites at %d and %d" % (mod, a[0], b[0]))
		for at, end, repl, s in reversed(edits):
			src = src[:at] + repl + src[end:]
			done.append([mod, s["offset"], s["kind"], s["position"], s["expression"]])
		if edits:
			with open(path, "w", encoding="utf-8") as fh:
				fh.write(src)
			rel["files"][mod] = hashlib.sha256(src.encode()).hexdigest()
			changed.append(path)
	if len(done) + len(skipped) != expected:
		die("accounted for %d + %d sites, expected %d" % (len(done), len(skipped), expected))
	if opts["--acorn"]:
		with tempfile.NamedTemporaryFile("w", suffix=".mjs", delete=False) as fh:
			fh.write(PARSEJS)
			js = fh.name
		try:
			r = subprocess.run([opts["--node"], js, os.path.abspath(opts["--acorn"])] + changed)
		finally:
			os.unlink(js)
		if r.returncode:
			die("a rewritten module no longer parses")
	rel["rule_totals"]["P4"] = len(done)
	rel["rule_sites"]["P4"] = sorted(done)
	rel["rule_totals"]["P4_skipped"] = len(skipped)
	rel["rule_sites"]["P4_skipped"] = sorted(skipped)
	with open(relp, "w") as fh:
		json.dump(rel, fh, indent=1)
	print("P4 applied %d sites in %d modules; skipped %d already rewritten by %s" % (
		len(done), len(changed), len(skipped), ",".join(sorted(applied_rules)) or "-"))


if __name__ == "__main__":
	main()
