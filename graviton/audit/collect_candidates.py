#!/usr/bin/env python3
"""Deterministic Collect+Cluster for the overnight bug-fix run.

Reads FINDINGS.md, keeps active-surface bug-shaped findings, CLUSTERS near-duplicates
(same file+checker, lines within a window) into one work item, and RANKS them so
FP-prone cppcheck value-flow checks on vendored/BSD code sink below distinct/novel
findings. Emits a JSON array suitable for the overnight workflow's `args.findings`.

Usage: python3 collect_candidates.py [FINDINGS.md] > candidates.json
Deterministic (Principle 1): no LLM, no network, stable ordering.
"""
import re, sys, json

FINDINGS = sys.argv[1] if len(sys.argv) > 1 else "/local/home/benfelip/Haiku-Graviton/FINDINGS.md"
CLUSTER_WINDOW = 12  # lines: same file+checker within this span collapse into one item

ACTIVE = {"ena", "network-stack", "bfs", "app_server-remote", "kernel/arm64"}

# bug-shaped ids worth a deep pass (mirrors PRIORITY.md triage)
KEEP = {
 "uninitvar","uninitMemberVar","uninitStructMember","nullPointer","integerOverflow",
 "arrayIndexOutOfBounds","bufferAccessOutOfBounds","negativeIndex","shiftTooManyBitsSigned",
 "shiftNegative","memleak","memleakOnRealloc","resourceLeak","deallocuse","doubleFree",
 "unsignedLessThanZero","unknownEvaluationOrder","knownConditionTrueFalse","invalidFunctionArg",
 "nullPointerRedundantCheck","danglingLifetime","selfInitialization","missingReturn",
 "duplicateCondition","duplicateExpression","redundantAssignment",
 "bugprone-assignment-in-if-condition","bugprone-sizeof-expression","bugprone-suspicious-enum-usage",
 "bugprone-suspicious-string-compare","bugprone-incorrect-roundings","bugprone-branch-clone",
 "bugprone-implicit-widening-of-multiplication-result","bugprone-macro-parentheses",
 "cert-err33-c","cert-err34-c",
}
# cppcheck value-flow checks with a high false-positive rate on this tree (shakedown 2026-08-27:
# radix.c uninitvar/nullPointer were all FP). Down-ranked, not dropped.
FP_PRONE = {"uninitvar","nullPointer","nullPointerRedundantCheck","uninitStructMember"}
# vendored / BSD-derived paths where the above idioms are usually intentional upstream shape.
VENDORED_HINTS = ("ena-com/", "/stack/radix.c", "/libnet", "/ppp/")

# subsystem exercised-surface weight (higher = worked harder in prod)
SUB_W = {"ena": 5, "network-stack": 4, "bfs": 3, "app_server-remote": 2, "kernel/arm64": 2}

def subsystem_of(path):
    if "drivers/network/ether/ena" in path: return "ena"
    if path.startswith("src/add-ons/kernel/network"): return "network-stack"
    if "file_systems/bfs" in path: return "bfs"
    if "/drawing/interface/remote" in path: return "app_server-remote"
    if path.startswith("src/system/kernel/arch/arm64"): return "kernel/arm64"
    return None

row_re = re.compile(
    r'\| \d+ \| `([^`]+):(\d+)` \| (cppcheck|clang-tidy) \| ([^ ]+) \| (\w+) \| \*\*(\w+)\*\* \| ([\w/-]+) \| (.*?) \|$')

raw = []
for line in open(FINDINGS, encoding="utf-8", errors="replace"):
    m = row_re.match(line.strip())
    if not m:
        continue
    f, ln, tool, cid, tsev, guess, sub, msg = m.groups()
    if sub not in ACTIVE or cid not in KEEP:
        continue
    raw.append({"file": f, "line": int(ln), "checker": cid, "tool": tool,
                "subsystem": sub, "description": msg})

# ---- cluster: same (file, checker), lines within CLUSTER_WINDOW ----
raw.sort(key=lambda r: (r["file"], r["checker"], r["line"]))
clusters = []
for r in raw:
    c = clusters[-1] if clusters else None
    if (c and c["file"] == r["file"] and c["checker"] == r["checker"]
            and r["line"] - c["_lastline"] <= CLUSTER_WINDOW):
        c["sibling_lines"].append(r["line"]); c["_lastline"] = r["line"]
    else:
        clusters.append({"file": r["file"], "line": r["line"], "checker": r["checker"],
                         "tool": r["tool"], "subsystem": r["subsystem"],
                         "description": r["description"], "sibling_lines": [r["line"]],
                         "_lastline": r["line"]})

# ---- rank: subsystem weight, minus FP-prone/vendored penalties ----
for c in clusters:
    vendored = any(h in c["file"] for h in VENDORED_HINTS)
    fp_prone = c["checker"] in FP_PRONE
    score = SUB_W.get(c["subsystem"], 1) * 10
    if fp_prone: score -= 12          # value-flow checks sink below distinct findings
    if vendored: score -= 6           # intentional-idiom risk
    if len(c["sibling_lines"]) >= 3: score += 1  # recurring smell, slight bump
    c["priority_score"] = score
    c["fp_prone"] = fp_prone
    c["vendored"] = vendored
    del c["_lastline"]

clusters.sort(key=lambda c: (-c["priority_score"], c["file"], c["line"]))

# summary to stderr, JSON array to stdout
from collections import Counter
bysub = Counter(c["subsystem"] for c in clusters)
sys.stderr.write(f"raw findings: {len(raw)}  ->  clusters: {len(clusters)}\n")
sys.stderr.write(f"by subsystem: {dict(bysub)}\n")
sys.stderr.write(f"fp-prone clusters (down-ranked): {sum(c['fp_prone'] for c in clusters)}\n")
json.dump(clusters, sys.stdout, indent=1)
