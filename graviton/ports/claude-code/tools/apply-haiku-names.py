#!/usr/bin/env python3
"""apply-haiku-names.py <tree>: give the bundle's platform-name maps a real "haiku" case.

Runs after apply-folds.py (P4): once process.platform is live, these maps see
"haiku" and, without a case for it, call Haiku "unknown" (P5) or "Linux" (P7), or
return the raw lowercase value where every other OS gets a display name (P6).
Each rule is an exact, asserted substitution (IDs match PATCHES.md); a count other
than the expected one stops the build.
"""
import hashlib, json, os, sys

RULES = [
	# P5: the main host's getPlatform() (chunk-8dvz2fjk.js); "unknown" otherwise.
	("P5", "chunk-8dvz2fjk.js",
		'this.platform=e!==void 0&&yMt(e)?"wsl":"linux"}else this.platform="unknown"',
		'this.platform=e!==void 0&&yMt(e)?"wsl":"linux"}else if(this.sources.platform==="haiku")'
		'this.platform="haiku";else this.platform="unknown"'),
	# P6: G1e, platform -> display name (chunk-8dvz2fjk.js).
	("P6", "chunk-8dvz2fjk.js",
		'case"linux":return"Linux";default:return e}',
		'case"linux":return"Linux";case"haiku":return"Haiku";default:return e}'),
	# P7: nTo, platform -> display name compared with a peer's osPlatform
	# (chunk-yt5m1rse.js); every non-darwin/win32 value became "Linux".
	("P7", "chunk-yt5m1rse.js",
		':e==="win32"?"Windows":"Linux"}',
		':e==="win32"?"Windows":e==="haiku"?"Haiku":"Linux"}'),
]


def die(msg):
	sys.exit("apply-haiku-names: " + msg)


def main():
	if len(sys.argv) != 2:
		die("usage: apply-haiku-names.py <tree>")
	tree = sys.argv[1]
	relp = os.path.join(tree, "RELOCATION.json")
	rel = json.load(open(relp))
	if not rel["rule_totals"].get("P4"):
		die("run apply-folds.py (P4) first")
	if any(rel["rule_totals"].get(r) for r, *_ in RULES):
		die("already applied to this tree")
	for rule, mod, old, new in RULES:
		p = os.path.join(tree, mod)
		s = open(p, encoding="utf-8").read()
		n = s.count(old)
		if n != 1:
			die("%s: expected 1 site in %s, found %d -- re-review" % (rule, mod, n))
		s = s.replace(old, new)
		open(p, "w", encoding="utf-8").write(s)
		rel["files"][mod] = hashlib.sha256(s.encode()).hexdigest()
		rel["rule_totals"][rule] = 1
		rel.setdefault("rule_sites", {})[rule] = [[mod, 1]]
		print("%s applied in %s" % (rule, mod))
	with open(relp, "w") as fh:
		json.dump(rel, fh, indent=1)


main()
