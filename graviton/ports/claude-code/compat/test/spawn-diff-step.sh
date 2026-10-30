# Step script for tools/dev-linux-run (REMOTE_DIR=/opt/cc/w-spawn): the Bun.spawn diff test.
# Runs the shared scenarios under stock bun, then node + compat, then compares.
cd compat/test
export CC_SPAWN_PRESET=orig CC_SPAWN_PRESET2=orig2
timeout 400 bun spawn-scenarios.mjs /opt/cc/w-spawn/bun.json /opt/cc/w-spawn-work; echo bun-rc=$?
timeout 400 /opt/node24/bin/node --import ./spawn-preload.mjs spawn-scenarios.mjs /opt/cc/w-spawn/node.json /opt/cc/w-spawn-work; echo node-rc=$?
/opt/node24/bin/node check-spawn.mjs /opt/cc/w-spawn/bun.json /opt/cc/w-spawn/node.json; echo check-rc=$?
rm -rf /opt/cc/w-spawn-work
