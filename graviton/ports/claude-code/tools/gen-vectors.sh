#!/bin/bash
# gen-vectors.sh <outdir>: record, under stock Bun 1.4.4, every platform-independent
# compat vector suite (pure functions of their input), with the seeds the diff tests used.
# tools/check-vectors.sh replays them under Node on any host, Haiku included.
set -euo pipefail
# YAML_PARSE_CORPUS: the yaml-parse-corpus.json built by build-yaml-parse-corpus.mjs
# (not committed: third-party text). Required.
here=$(cd "$(dirname "$0")/.." && pwd); out=$1
: "${YAML_PARSE_CORPUS:?set YAML_PARSE_CORPUS to yaml-parse-corpus.json}"
cp "$YAML_PARSE_CORPUS" "$here/compat/test/yaml-parse-corpus.json"
rm -rf "$out" && mkdir -p "$out"
cd "$here/compat/test"
bun --version > "$out/BUN_VERSION"
bun gen-hash-vectors.js "$out/hash.vectors.json"
for s in width slice wrap semver deepequals yaml-stringify; do bun gen-$s-vectors.js "$out"; done
bun gen-toml-vectors.js "$out/toml-default"
for seed in 1 0xC0FFEE 987654321 0xDEADBEEF12345; do bun gen-toml-vectors.js "$out/toml-$seed" "$seed"; done
for seed in 0 11 22 33 44 55; do
	mkdir -p "$out/yaml-parse-$seed"
	if [ "$seed" = 0 ]; then bun gen-yaml-parse-vectors.js "$out/yaml-parse-$seed"; else bun gen-yaml-parse-vectors.js "$out/yaml-parse-$seed" "$seed"; fi > "$out/yaml-parse-$seed/gen.log"
done
(cd "$out" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS)
echo "gen-vectors: $(wc -l < "$out/SHA256SUMS") files in $out"
