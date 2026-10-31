# Step script for tools/dev-linux-run (REMOTE_DIR=/opt/cc/w-fetch): the fetch diff test.
# Builds a tree (fetch-deps + build-tree), records the scenarios under stock bun,
# checks them under node + the tree's compat preload, compares, and verifies that
# no listener is left behind. ONLY=<regex> limits the scenarios.
set -u
W=/opt/cc/w-fetch N=/opt/node24/bin/node
bash tools/fetch-deps.sh $W/deps > $W/deps.log 2>&1 || { cat $W/deps.log; exit 1; }
grep undici $W/deps.log
rm -rf $W/tree && bash tools/build-tree.sh /opt/cc/v2.1.314/package/claude $W/tree $W/tree $N $W/deps > $W/build.log 2>&1 || { tail -20 $W/build.log; exit 1; }
tail -1 $W/build.log; ls $W/tree/node_modules
cd compat/test/fetch
WK=$(mktemp -d /tmp/ccfetch.XXXXXX); mkdir -p $WK/sock
bash gen-certs.sh $WK/certs > /dev/null
ss -ltn | awk 'NR>1{print $4}' | sort > $WK/ss-before
export FIXTURE_NODE=$N
timeout 600 bun run.js $WK/bun.json $WK/certs $WK/sock "${ONLY:-}"; echo bun-rc=$?
timeout 600 $N --import $W/tree/bun-compat.mjs run.js $WK/node.json $WK/certs $WK/sock "${ONLY:-}"; echo node-rc=$?
$N check.mjs $WK/bun.json $WK/node.json; echo check-rc=$?
ss -ltn | awk 'NR>1{print $4}' | sort > $WK/ss-after
echo "listeners left: $(comm -13 $WK/ss-before $WK/ss-after | wc -l)"; comm -13 $WK/ss-before $WK/ss-after
echo "--- ss -ltnp"; ss -ltnp
mkdir -p $W.out && cp $WK/bun.json $WK/node.json $W.out/
rm -rf $WK
