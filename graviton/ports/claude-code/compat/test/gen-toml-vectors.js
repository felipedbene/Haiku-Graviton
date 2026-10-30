// Run under STOCK Bun: reference outputs for Bun.TOML.parse, deterministic.
import { edgeCases, byteCases, configFiles, coerceCases, depthCases, makeFuzzer, encode, encodeFlat, encodeError } from "./toml-corpus.js";

const DT = ["Instant", "PlainDateTime", "PlainDate", "PlainTime"];
const dtName = (v) => { for (const n of DT) if (v instanceof Temporal[n]) return n; return null; };
const run = (f, enc = encode) => { try { return { v: enc(f(), dtName) }; } catch (e) { return { e: encodeError(e) }; } };
const P = (x) => Bun.TOML.parse(x);

const out = { edge: [], bytes: [], config: [], coerce: [], depth: [], fuzz: [], fuzzshort: [], mutate: [] };
for (const s of edgeCases) out.edge.push([s, run(() => P(s))]);
for (const b of byteCases) out.bytes.push([Buffer.from(b).toString("base64"), run(() => P(Buffer.from(b)))]);
for (const s of configFiles) out.config.push([s, run(() => P(s))]);
for (const [name, mk] of coerceCases) out.coerce.push([name, run(() => (mk === null ? Bun.TOML.parse() : P(mk())))]);
for (const [name, s] of depthCases) out.depth.push([name, run(() => P(s), encodeFlat)]);

// argv[3]: an optional other fuzz seed, for extra runs beyond the default.
const fz = makeFuzzer(process.argv[3] ? BigInt(process.argv[3]) : 0x70e1c0de);
for (let k = 0; k < 40000; k++) { const s = fz.document(); out.fuzz.push([s, run(() => P(s))]); }
// One or two lines per document: most survive, so values and keys get
// exercised on the success path, not only up to the first error.
for (let k = 0; k < 40000; k++) { const s = fz.document(2); out.fuzzshort.push([s, run(() => P(s))]); }
// Mutants of valid documents only, so mutations land in real structure.
const valid = [...out.fuzz.filter(([, r]) => r.v).map(([s]) => s), ...out.fuzzshort.filter(([, r]) => r.v).map(([s]) => s), ...edgeCases.filter((s, i) => out.edge[i][1].v), ...configFiles];
for (let k = 0; k < 40000; k++) { const s = fz.mutate(valid[fz.rnd(valid.length)]); out.mutate.push([s, run(() => P(s))]); }

await Bun.write(process.argv[2] + "/toml.cases.json", JSON.stringify(out));
for (const [k, v] of Object.entries(out)) console.log(`${k}: ${v.length} (${v.filter(([, r]) => r.v).length} parse, ${v.filter(([, r]) => r.e).length} throw)`);
