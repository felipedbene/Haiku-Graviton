#!/bin/bash
# check-vectors.sh <vecdir> <node>: replay every gen-vectors.sh suite against
# compat/lib under <node>. Prints each checker's summary; exits 1 if any suite fails.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); v=$(cd "$1" && pwd); node=$2
(cd "$v" && sha256sum -c --quiet SHA256SUMS) || { echo "check-vectors: vector files corrupt"; exit 1; }
echo "node $("$node" --version) on $(uname -sm); vectors from bun $(cat "$v/BUN_VERSION")"
cd "$here/compat/test"
fail=0
run() { local name=$1; shift; local o; o=$("$@" 2>&1); local rc=$?
	printf '%s\n' "$o" | grep -v -E '^  |^KNOWN' | tail -n 12 | sed "s/^/[$name] /"
	echo "[$name] rc=$rc"; [ $rc = 0 ] || fail=1; }
run hash "$node" check-hash.mjs "$v/hash.vectors.json"
for s in width slice wrap semver deepequals yaml-stringify; do run $s "$node" check-$s.mjs "$v"; done
for d in "$v"/toml-*; do run "$(basename "$d")" "$node" check-toml.mjs "$d"; done
for d in "$v"/yaml-parse-*; do run "$(basename "$d")" "$node" check-yaml-parse.mjs "$d"; done
echo "check-vectors: $([ $fail = 0 ] && echo ALL PASS || echo FAILURES)"
exit $fail
