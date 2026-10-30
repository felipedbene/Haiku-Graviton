// Run under Node: compare compat/lib/hash.mjs against the stock-Bun vectors.
import { readFileSync } from "node:fs";
import { wyhash, xxHash64, crc32 } from "../lib/hash.mjs";
const V = JSON.parse(readFileSync(process.argv[2], "utf8"));
const seed = (v) => v && v.undef ? undefined : v && v.bigint !== undefined ? BigInt(v.bigint) : v;
const hb = (h) => Buffer.from(h, "hex");
let ok = 0, bad = 0;
const eq = (what, got, exp) => { if (String(got) === String(exp)) ok++; else { bad++; if (bad <= 10) console.log("MISMATCH", what, "got", String(got), "exp", String(exp)); } };
for (const c of V.cases) {
	if (c.kind === "bytes" && c.wyhash !== undefined) {
		const b = hb(c.hex), s = seed(c.seed);
		eq(`wyhash len=${b.length} seed=${JSON.stringify(c.seed)}`, wyhash(b, s), c.wyhash);
		eq(`xxh64 len=${b.length} seed=${JSON.stringify(c.seed)}`, xxHash64(b, s), c.xxHash64);
	} else if (c.kind === "bytes") eq(`crc32 len=${c.hex.length / 2}`, crc32(hb(c.hex)), c.crc32);
	else if (c.kind === "string") {
		eq(`wyhash str ${JSON.stringify(c.text)}`, wyhash(c.text), c.wyhash);
		eq(`wyhash seeded str ${JSON.stringify(c.text)}`, wyhash(c.text, wyhash("seed")), c.wyhash_seeded);
		eq(`xxh64 str ${JSON.stringify(c.text)}`, xxHash64(c.text), c.xxHash64);
		eq(`crc32 str ${JSON.stringify(c.text)}`, crc32(c.text), c.crc32);
	} else if (c.kind === "views") {
		const b = hb(c.hex);
		eq("ArrayBuffer", wyhash(b.buffer.slice(b.byteOffset, b.byteOffset + b.length)), c.ab);
		eq("DataView", wyhash(new DataView(b.buffer, b.byteOffset, b.length)), c.dv);
		const u = hb(c.u16hex); eq("Uint16Array", wyhash(new Uint16Array(u.buffer.slice(u.byteOffset, u.byteOffset + u.length))), c.u16);
		eq("Buffer subarray", wyhash(hb(c.subhex)), c.sub);
	}
}
console.log(`hash vectors (bun ${V.bun} ${V.revision}): ${ok} match, ${bad} mismatch`);
process.exit(bad ? 1 : 0);
