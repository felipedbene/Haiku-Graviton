#!/bin/bash
# diff harness: run each case under A (stock bun) and B (node + compat), compare out/err/rc.
# Exits 1 if any case differs. TREE, NODE and WORK are overridable so parallel
# workers on one box don't share a tree or scratch dir.
set -u
P=${TREE:-/opt/claude-code/2.1.314}; N=${NODE:-/opt/node24/bin/node}; W=${WORK:-/tmp}
run() { local side=$1 of=$2; shift 2; local h=$W/cchome-$side; rm -rf $h; mkdir -p $h
	if [ $side = A ]; then (cd $W && HOME=$h timeout 60 bun --zero-fill-buffers $P/cli "$@" > $of.out 2> $of.err < /dev/null; echo $? > $of.rc)
	else (cd $W && HOME=$h timeout 60 $N --zero-fill-buffers --import $P/bun-compat.mjs $P/cli "$@" > $of.out 2> $of.err < /dev/null; echo $? > $of.rc); fi
	sed -i "s#$W/cchome-$side#\$HOME#g" $of.out $of.err; }
D=$W/ccdiff; rm -rf $D; mkdir -p $D; same=0; dif=0
while IFS= read -r line; do [ -z "$line" ] && continue; t=$(echo "$line" | tr ' /' '__'); eval "set -- $line"
	run A $D/A-$t "$@"; run B $D/B-$t "$@"
	if cmp -s $D/A-$t.out $D/B-$t.out && cmp -s $D/A-$t.rc $D/B-$t.rc && cmp -s $D/A-$t.err $D/B-$t.err; then same=$((same+1)); echo "SAME  [$line] rc=$(cat $D/A-$t.rc)"
	else dif=$((dif+1)); echo "DIFF  [$line] rcA=$(cat $D/A-$t.rc) rcB=$(cat $D/B-$t.rc)"; diff $D/A-$t.out $D/B-$t.out | head -${DIFFN:-6}; diff $D/A-$t.err $D/B-$t.err | head -${DIFFN:-12}; fi
done < ${CASES:-/opt/cc/cases.txt}
echo "summary: same=$same diff=$dif"; [ $dif -eq 0 ]
