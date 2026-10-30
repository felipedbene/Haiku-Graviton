// Runs the shared net scenarios under stock Bun with the real Bun.serve /
// Bun.listen / Bun.connect and records the observations.
import { runAll } from "./net-scenarios.js";
const api = { serve: (o) => Bun.serve(o), listen: (o) => Bun.listen(o), connect: (o) => Bun.connect(o) };
const out = await runAll(api, { tlsDir: process.env.NET_TLS_DIR || null, childArgs: [] });
await Bun.write(process.argv[2] + "/net.cases.json", JSON.stringify(out, null, 1));
console.log("net scenarios:", out.length);
process.exit(0);
