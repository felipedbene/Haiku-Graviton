// Run under STOCK Bun: writes the reference vectors compat/lib/hash.mjs must match.
// Inputs are deterministic (xorshift PRNG), so the file regenerates identically.
let s = 0x2545f491n;
const rnd = () => { s ^= s << 13n; s &= (1n << 64n) - 1n; s ^= s >> 7n; s ^= s << 17n; s &= (1n << 64n) - 1n; return Number(s & 255n); };
const bytes = (n) => Uint8Array.from({ length: n }, rnd);
const hex = (u) => Buffer.from(u).toString("hex");
const lens = [...Array(301).keys(), 383, 384, 385, 1000, 4095, 4096, 4097, 65537];
const seeds = [undefined, 0, 1, 0xffffffff, 2 ** 32, 2 ** 53 - 1, -1, 1.75, -2.5, 0n, 1n, 2n ** 63n, 2n ** 64n - 1n, 2n ** 64n, -1n];
const str = (v) => typeof v === "bigint" ? { bigint: v.toString() } : v === undefined ? { undef: true } : v;
const out = { bun: Bun.version, revision: Bun.revision, cases: [] };
for (const n of lens) {
	const b = bytes(n);
	for (const seed of (n <= 64 || n % 97 === 0 ? seeds : [undefined, 7n])) {
		out.cases.push({ kind: "bytes", hex: hex(b), seed: str(seed),
			wyhash: Bun.hash(b, seed).toString(), xxHash64: Bun.hash.xxHash64(b, seed).toString() });
	}
	out.cases.push({ kind: "bytes", hex: hex(b), crc32: Bun.hash.crc32(b) });
}
for (const t of ["", "a", "é", "日本語", "👍🏽", "a\u0000b", "x".repeat(100), "\ud800", "mixed ✓ ascii ü 𝄞"]) {
	out.cases.push({ kind: "string", text: t, wyhash: Bun.hash(t).toString(),
		wyhash_seeded: Bun.hash(t, Bun.hash("seed")).toString(), xxHash64: Bun.hash.xxHash64(t).toString(), crc32: Bun.hash.crc32(t) });
}
// Input kinds: ArrayBuffer, DataView, Uint16Array over the same bytes, Buffer subarray.
const b = bytes(37), u16 = new Uint16Array(b.buffer.slice(0, 36)), sub = Buffer.from(bytes(80)).subarray(5, 70);
out.cases.push({ kind: "views", hex: hex(b), ab: Bun.hash(b.buffer).toString(), dv: Bun.hash(new DataView(b.buffer)).toString(),
	u16hex: hex(new Uint8Array(u16.buffer)), u16: Bun.hash(u16).toString(), subhex: hex(sub), sub: Bun.hash(sub).toString() });
await Bun.write(process.argv[2], JSON.stringify(out));
console.log("cases:", out.cases.length, "bun", Bun.version, Bun.revision);
