// Run under STOCK Bun: reference outputs for Bun.YAML.stringify over the shared corpus.
import { run, runDivergent, maxDepth } from "./yaml-stringify-corpus.js";
const stringify = (v, r, s) => Bun.YAML.stringify(v, r, s);
const out = { cases: run(stringify, 2000), divergent: runDivergent(stringify), maxDepth: maxDepth(stringify) };
await Bun.write(process.argv[2] + "/yaml-stringify.cases.json", JSON.stringify(out));
console.log("YAML.stringify cases:", out.cases.length, "divergent:", out.divergent.length, "maxDepth:", JSON.stringify(out.maxDepth));
