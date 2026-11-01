#!/usr/bin/env python3
"""drop-napi.py <tree>: remove the bundle's native addons (Bun "napi" loader files) from
the tree (rule R5). They are linux-arm64 ELF shared objects (audio-capture.node,
clipboard-napi.node) that can never load on Haiku, and the port ships no Linux binaries.
Both call sites already treat a missing addon as "unavailable" (LIMITATIONS.md). Asserts
the expected set, records what was removed in RELOCATION.json, and refuses a second run.
"""
import json, os, sys

EXPECTED = {"audio-capture.node", "clipboard-napi.node"}


def die(msg):
	sys.exit("drop-napi: " + msg)


tree = sys.argv[1] if len(sys.argv) == 2 else die("usage: drop-napi.py <tree>")
relp = os.path.join(tree, "RELOCATION.json")
rel = json.load(open(relp))
if rel["rule_totals"].get("R5"):
	die("already applied to this tree")
prefix = rel["prefix"].rstrip("/") + "/"
napi = json.load(open(os.path.join(tree, "LOADERS.json")))["napi"]
names = {p[len(prefix):] for p in napi}
if names != EXPECTED:
	die("napi set changed -- re-review: %s" % sorted(names))
for n in sorted(names):
	p = os.path.join(tree, n)
	with open(p, "rb") as f:
		if f.read(4) != b"\x7fELF":
			die("%s is not an ELF object -- re-review" % n)
	os.unlink(p)
	rel["files"].pop(n, None)
	print("R5 removed %s (linux ELF addon)" % n)
rel["rule_totals"]["R5"] = len(names)
rel.setdefault("rule_sites", {})["R5"] = [[n, 1] for n in sorted(names)]
with open(relp, "w") as fh:
	json.dump(rel, fh, indent=1)
