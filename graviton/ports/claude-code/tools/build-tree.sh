#!/bin/bash
# build-tree.sh <claude-binary> <outdir> <install-prefix> <node> <depsdir>
#
# <depsdir> holds fetch-deps.sh output (integrity-verified ws, undici + acorn).
#
# The whole deterministic pass, in order: extract the Bun module graph, relocate
# (R1-R3, P1-P3), analyze evaluation-time requires, apply R4, restore the
# build-time platform/arch folds (P4, sites in folds-<version>.json), add haiku cases to the platform-name
# maps (P5-P7), then install the
# runtime support files (compat preload, the pinned ws and undici packages, node-fetch).
# Each step asserts its own counts; any change in upstream shape stops the build.
set -euo pipefail
bin=$1 out=$2 prefix=$3 node=$4 deps=$5
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 -I "$here/extract-bun-bundle.py" "$bin" "$work/bunfs"
python3 -I "$here/relocate-bundle.py" "$work/bunfs" "$out" "$prefix"
"$node" "$here/analyze-requires.mjs" "$out" "$prefix" "$deps" > "$work/requires.json"
python3 -I "$here/apply-r4.py" "$out" "$work/requires.json"
python3 -I "$here/apply-folds.py" "$out" "$here/folds-2.1.314.json" --acorn "$deps/acorn" --node "$node"
python3 -I "$here/apply-haiku-names.py" "$out"
cp "$work/requires.json" "$out/REQUIRES.json"
cp "$here/../compat/bun-compat.mjs" "$here/../compat/claude.mjs" "$out/"
mkdir -p "$out/lib" && cp "$here/../compat/lib/"*.mjs "$out/lib/"
mkdir -p "$out/node_modules"
cp -R "$here/../compat/node_modules/node-fetch" "$out/node_modules/"
cp -R "$deps/ws" "$out/node_modules/ws"
cp -R "$deps/undici" "$out/node_modules/undici"
# Node's fetch drives the npm undici dispatchers through its bundled undici's
# handler API, which is stable within a major: refuse a tree whose majors differ.
bundled=$("$node" -p 'process.versions.undici')
pinned=$("$node" -p 'require(process.argv[1]).version' "$deps/undici/package.json")
[ "${bundled%%.*}" = "${pinned%%.*}" ] || { echo "build-tree: node bundles undici $bundled, deps pin $pinned (majors differ)" >&2; exit 1; }
echo "build-tree: $out ready (prefix $prefix)"
