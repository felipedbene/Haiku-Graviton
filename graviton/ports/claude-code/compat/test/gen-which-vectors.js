import { CASES } from "./which-cases.js";
const out = CASES.map((a) => { try { return Bun.which(...a); } catch (e) { return "THROW " + e.message; } });
await Bun.write(process.argv[2] + "/which.cases.json", JSON.stringify(out));
console.log("which cases:", out.length);
