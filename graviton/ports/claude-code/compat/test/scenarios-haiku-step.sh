# Step script for tools/haiku-compat-run: the Bun.spawn and Bun.serve/listen/
# connect diff tests on Haiku, against stock Bun recorded on Linux by
# spawn-diff-step.sh / net-diff-step.sh. Needs REC_URL (a GET for the recordings
# tarball) and REC_SHA set before it. Runs Node + the shipped compat/lib;
# loopback and port 0 only, and the TCP listeners are listed before and after.
N=/boot/system/bin/node
$N --version
REC=/boot/home/cc-scen/rec; rm -rf $REC; mkdir -p $REC
/boot/system/bin/curl -sSfo $REC/rec.tgz "$REC_URL" || exit 1
echo "$REC_SHA  $REC/rec.tgz" | sha256sum -c - || exit 1
tar xzf $REC/rec.tgz -C $REC && sha256sum $REC/spawn-bun.json $REC/net.cases.json
echo "== TCP listeners before"; netstat -n -t -l
cd compat/test
echo "######## spawn"
export CC_SPAWN_PRESET=orig CC_SPAWN_PRESET2=orig2
timeout 600 $N --import ./spawn-preload.mjs spawn-scenarios.mjs $REC/spawn-node-haiku.json /tmp/cc-scen-spawn-work; echo node-rc=$?
$N check-spawn.mjs $REC/spawn-bun.json $REC/spawn-node-haiku.json; echo check-rc=$?
rm -rf /tmp/cc-scen-spawn-work
echo "######## net"
T=/tmp/cc-scen-tls; rm -rf $T; mkdir -p $T
openssl req -x509 -newkey rsa:2048 -nodes -keyout $T/key.pem -out $T/cert.pem -days 2 -subj /CN=localhost 2>/dev/null && echo tls-ok
NET_TLS_DIR=$T timeout 900 $N check-net.mjs $REC 2> $REC/net-node-haiku.stderr; echo "check rc=$?"
echo "stderr lines: $(wc -l < $REC/net-node-haiku.stderr)"
rm -rf $T
echo "== TCP listeners after"; netstat -n -t -l
echo "== node teams left"; ps | grep -E '^/boot/system/bin/node|^/bin/sleep|sleep ' | grep -v grep; echo "grep rc=$? (1 = none)"
