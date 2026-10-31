# Step script for tools/dev-linux-run (REMOTE_DIR=/opt/cc/w-scen): the Bun.spawn diff test.
# Runs the shared scenarios under stock bun, then node + compat, then compares.
# The recordings stay in $O for the cross-platform run (scenarios-haiku-step.sh).
cd compat/test
# HOME is unset under SSM; "env spread" reports it, so every host sets one.
export HOME=${HOME:-/root}
export CC_SPAWN_PRESET=orig CC_SPAWN_PRESET2=orig2
O=${REC_DIR:-/opt/cc/w-scen.rec}; mkdir -p $O
bun --version; /opt/node24/bin/node --version
timeout 400 bun spawn-scenarios.mjs $O/spawn-bun.json /opt/cc/w-scen-work; echo bun-rc=$?
timeout 400 /opt/node24/bin/node --import ./spawn-preload.mjs spawn-scenarios.mjs $O/spawn-node.json /opt/cc/w-scen-work; echo node-rc=$?
/opt/node24/bin/node check-spawn.mjs $O/spawn-bun.json $O/spawn-node.json; echo check-rc=$?
rm -rf /opt/cc/w-scen-work
