#!/bin/bash
# check-folds.sh <extract-dir> <folds.json> <acorn-dir> [node]
#
# Exercises apply-folds.py's other path: a tree where relocate-bundle.py did NOT run
# P1/P2 (the state once the integrator drops them in favour of P4). Applies every
# fold site to a copy of the raw linux-arm64 extraction, then checks that all sites
# were applied (none skipped), that no switch("linux") / switch("arm64") remains,
# that every rewritten module parses, and that a second run refuses to double-apply.
set -euo pipefail
ex=$1 folds=$2 acorn=$3 node=${4:-node}
here=$(cd "$(dirname "$0")/../../tools" && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
cp -R "$ex/root" "$t/tree"
sha=$(python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["source_sha256"])' "$ex/manifest.json")
printf '{"source_sha256":"%s","prefix":"/x","rule_totals":{},"rule_sites":{},"files":{}}' "$sha" > "$t/tree/RELOCATION.json"
python3 -I "$here/apply-folds.py" "$t/tree" "$folds" --acorn "$acorn" --node "$node"
python3 -I -c 'import json,sys;t=json.load(open(sys.argv[1]))["rule_totals"];assert t["P4_skipped"]==0,t;print("P4 =",t["P4"])' "$t/tree/RELOCATION.json"
left=$( (cat "$t/tree"/*.js | grep -o 'switch("\(linux\|arm64\)")' || true) | wc -l)
[ "$left" -eq 0 ] || { echo "check-folds: $left folded switch discriminants left" >&2; exit 1; }
if python3 -I "$here/apply-folds.py" "$t/tree" "$folds" 2>/dev/null; then
	echo "check-folds: a second apply was not refused" >&2; exit 1
fi
echo "check-folds: OK"
