// Run INSIDE the real Claude Code 2.1.314 binary (its runtime is
// @anthropic-ai/bun-internal, the only one with Bun.ant.CellSegmenter):
//   CELLSEG_OUT=<dir> BUN_OPTIONS="--preload <this file>" claude --version
// Bun applies BUN_OPTIONS to standalone executables too, so this file runs in
// the binary's own runtime before the bundle's entry; it writes the reference
// outputs and exits, so Claude Code itself never starts. Deterministic.
import { writeFileSync } from "node:fs";
import { buildCases, runCase, runEdge } from "./cellseg-cases.mjs";
const out = process.env.CELLSEG_OUT;
const C = globalThis.Bun?.ant?.CellSegmenter;
if (!out || typeof C !== "function") { console.error("gen-cellseg-vectors: need CELLSEG_OUT and Bun.ant.CellSegmenter"); process.exit(2); }
const cases = buildCases();
writeFileSync(out + "/cellseg.cases.jsonl", cases.map((c) => JSON.stringify(runCase(C, c))).join("\n") + "\n");
writeFileSync(out + "/cellseg.edge.json", JSON.stringify(runEdge(C)));
console.log("cellseg vectors: sessions", cases.length, "edge", runEdge(C).length, "runtime", process.versions.bun, typeof Bun.ant);
process.exit(0);
