# Step script for tools/dev-linux-run (REMOTE_DIR=/opt/cc/w-scen): the Bun.serve /
# listen / connect diff test. Records stock Bun, then runs the same scenarios
# under Node with compat/lib/net.mjs and compares. Loopback and port 0 only; the
# listeners left by bun or node afterwards are listed (none expected).
cd compat/test
O=${REC_DIR:-/opt/cc/w-scen.rec}; mkdir -p $O
T=/opt/cc/w-scen.tls; rm -rf $T; mkdir -p $T
openssl req -x509 -newkey rsa:2048 -nodes -keyout $T/key.pem -out $T/cert.pem -days 2 -subj /CN=localhost 2>/dev/null && echo tls-ok
NET_TLS_DIR=$T timeout 600 bun gen-net-vectors.js $O > $O/net-bun.stdout 2> $O/net-bun.stderr; echo "gen rc=$?"; cat $O/net-bun.stdout
NET_TLS_DIR=$T timeout 600 /opt/node24/bin/node check-net.mjs $O 2> $O/net-node.stderr; echo "check rc=$?"
echo "=== listeners left by bun or node after the run:"; ss -ltnp | grep -E '"(bun|node)"'; echo "grep rc=$? (1 = none)"
rm -rf $T
