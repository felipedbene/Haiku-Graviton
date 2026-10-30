#!/usr/bin/env python3
"""Deterministic rewrite of an extracted Claude Code Bun bundle into an installable tree.

  relocate-bundle.py <extracted-bunfs-dir> <outdir> <install-prefix> [--expect counts.json]

<extracted-bunfs-dir> is extract-bun-bundle.py's output (it mirrors /$bunfs/).
<install-prefix> is the absolute path the tree will live at on the target
(e.g. /boot/system/lib/claude-code/2.1.314). <outdir> receives the tree.

Every rule is a literal, exact-substring rewrite whose match count is asserted
against the expected counts for this version. A different count means upstream
changed the code shape, and the run stops so each site can be re-reviewed: a
rule must never silently match more, or fewer, sites than PATCHES.md describes.
Rules (IDs match PATCHES.md):

  R1  "/$bunfs/root/"            -> "<install-prefix>/"      module specifiers + asset/addon paths
  R2  import.meta.require        -> __ccRequire (+ per-module createRequire prelude)
  R3  import.meta.dir (not ...name) -> import.meta.dirname
  P1  switch("linux")            -> switch(process.platform)  (9 build-time-folded sites)
  P2  switch("arm64")            -> switch(process.arch)      (1 folded site)
  P3  platform helper: add case"haiku":return"haiku"         (1 site)

Inputs are read-only; <outdir> must not exist.
"""
import hashlib, json, os, re, shutil, sys

BUNFS = "/$bunfs/root/"
PRELUDE = ('import{createRequire as __ccCreateRequire}from"node:module";'
	'const __ccRequire=__ccCreateRequire(import.meta.url);\n')
HELPER_OLD = 'case"linux":return"linux";case"win32":return"windows";default:return"unknown"'
HELPER_NEW = ('case"linux":return"linux";case"win32":return"windows";'
	'case"haiku":return"haiku";default:return"unknown"')

# Expected per-rule totals for 2.1.314; override with --expect for another version.
EXPECT = {"R1": 250476, "R2": 547, "R3": 9, "P1": 9, "P2": 1, "P3": 1}


def die(msg):
	sys.exit("relocate-bundle: " + msg)


def main():
	args = sys.argv[1:]
	expect = dict(EXPECT)
	if "--expect" in args:
		i = args.index("--expect")
		expect.update(json.load(open(args[i + 1])))
		del args[i:i + 2]
	if len(args) != 3:
		die("usage: relocate-bundle.py <bunfs-dir> <outdir> <install-prefix> [--expect f]")
	src, out, prefix = args
	if not prefix.startswith("/") or prefix.endswith("/"):
		die("install prefix must be absolute without a trailing slash")
	manifest = json.load(open(os.path.join(src, "manifest.json")))
	if os.path.exists(out):
		die(out + " exists")
	os.makedirs(out)
	totals = {k: 0 for k in EXPECT}
	sites = {k: [] for k in EXPECT}
	files = []
	# Bun loader ids seen in this graph: 1 js, 5 file (import yields the path),
	# 10 napi, 13 text (import/require yields the contents as a string). Node has
	# no text loader, so the runtime compat hook needs the exact text set -- by
	# file, not by extension: two .mjs files here are text, not code.
	loaders = {"text": [], "file": [], "napi": []}
	names = {5: "file", 10: "napi", 13: "text"}
	for m in manifest["modules"]:
		rel = m["name"][len(BUNFS):]
		ip, op = os.path.join(src, "root", rel), os.path.join(out, rel)
		os.makedirs(os.path.dirname(op), exist_ok=True)
		if m["loader"] != 1:		# assets, text, addons: copied byte-for-byte
			if m["loader"] not in names:
				die("%s: unhandled Bun loader %d" % (rel, m["loader"]))
			loaders[names[m["loader"]]].append(prefix + "/" + rel)
			shutil.copyfile(ip, op)
			files.append((rel, hashlib.sha256(open(op, "rb").read()).hexdigest()))
			continue
		s = open(ip, encoding="utf-8").read()

		def sub(rule, old, new, text):
			n = text.count(old)
			if n:
				totals[rule] += n
				sites[rule].append([rel, n])
			return text.replace(old, new)

		s = sub("R1", BUNFS, prefix + "/", s)
		n = s.count("import.meta.require")
		if n:
			s = sub("R2", "import.meta.require", "__ccRequire", s)
			s = PRELUDE + s
		# R3: `import.meta.dir` only where it is not already `import.meta.dirname`.
		k = len(re.findall(r"import\.meta\.dir(?!name)", s))
		if k:
			totals["R3"] += k
			sites["R3"].append([rel, k])
			s = re.sub(r"import\.meta\.dir(?!name)", "import.meta.dirname", s)
		s = sub("P1", 'switch("linux")', "switch(process.platform)", s)
		s = sub("P2", 'switch("arm64")', "switch(process.arch)", s)
		s = sub("P3", HELPER_OLD, HELPER_NEW, s)
		# The rewritten file must not still reference the in-binary filesystem.
		if BUNFS in s:
			die(rel + ": /$bunfs/root/ survived R1")
		with open(op, "w", encoding="utf-8") as fh:
			fh.write(s)
		files.append((rel, hashlib.sha256(s.encode()).hexdigest()))
	bad = [f"{k}: expected {expect[k]}, matched {totals[k]}" for k in EXPECT
		if expect[k] is not None and totals[k] != expect[k]]
	if bad:
		shutil.rmtree(out)
		die("rule counts changed -- re-review the sites before relocating:\n  " + "\n  ".join(bad))
	# All JS chunks are ESM (module_format 1); the extensionless entry `cli` too.
	with open(os.path.join(out, "package.json"), "w") as fh:
		json.dump({"type": "module", "private": True}, fh)
	with open(os.path.join(out, "LOADERS.json"), "w") as fh:
		json.dump({k: sorted(v) for k, v in loaders.items()}, fh, indent=1)
	report = {"source_sha256": manifest["source_sha256"], "prefix": prefix,
		"rule_totals": totals, "rule_sites": sites,
		"files": {r: h for r, h in sorted(files)}}
	with open(os.path.join(out, "RELOCATION.json"), "w") as fh:
		json.dump(report, fh, indent=1)
	print("relocated %d files to %s (prefix %s)" % (len(files), out, prefix))
	print("rule totals:", json.dumps(totals))


if __name__ == "__main__":
	main()
