#!/bin/bash
# tui-compare.sh <dir-A> <dir-B>: compare tools/tui-diff.sh captures taken on two hosts
# (TUI_ONLY=A on one, TUI_ONLY=B on the other) snapshot by snapshot, byte for byte.
# An A snapshot that never settled is reported, not compared. Exits 1 on any FAIL.
set -u
A=$1 B=$2; pass=0 fail=0 unsettled=0 missing=0
for nf in "$A"/*.n; do
	name=$(basename "$nf" .n); n=$(cat "$nf")
	[ -f "$B/$name.n" ] || { echo "MISSING $name on B"; missing=$((missing + 1)); continue; }
	for k in $(seq 1 "$n"); do
		a=$A/$name.$k.A b=$B/$name.$k.B
		if [ -f "$a.unsettled" ]; then unsettled=$((unsettled + 1)); echo "UNSETTLED $name#$k (A)"; continue; fi
		if [ -f "$b.unsettled" ]; then fail=$((fail + 1)); echo "FAIL $name#$k (B never settled)"; continue; fi
		if cmp -s "$a" "$b"; then pass=$((pass + 1)); echo "PASS $name#$k ($(wc -c < "$a") bytes)"
		else fail=$((fail + 1)); echo "FAIL $name#$k"; diff "$a" "$b" | head -${DIFFN:-8} | cut -c1-200 | cat -v; fi
	done
done
echo "tui-compare: pass=$pass fail=$fail unsettled=$unsettled missing=$missing"
[ $fail -eq 0 ] && [ $unsettled -eq 0 ] && [ $missing -eq 0 ]
