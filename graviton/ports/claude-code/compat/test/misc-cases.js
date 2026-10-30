// Shared by Bun and Node: observations of the small Bun APIs (misc.mjs).
export async function observe(B) {
	const o = {};
	const T = async (k, f) => { try { const v = await f(); o[k] = v instanceof Uint8Array ? { u8: Buffer.from(v).toString("hex") } : v; } catch (e) { o[k] = { throw: e.constructor.name }; } };
	for (const [i, x] of [["s", "abc"], ["empty", ""], ["u8", new Uint8Array([1, 2, 3])], ["ab", new Uint8Array([9]).buffer], ["utf8", "héllo 👍"], ["dv", new DataView(new Uint8Array([5, 6]).buffer)]]) {
		await T(`sha.${i}.b64`, () => B.SHA256.hash(x, "base64"));
		await T(`sha.${i}.hex`, () => B.SHA256.hash(x, "hex"));
		await T(`sha.${i}.raw`, () => B.SHA256.hash(x));
	}
	await T("sha.inc", () => new B.SHA256().update("ab").update(new Uint8Array([99])).digest("hex"));
	await T("sha.byteLength", () => B.SHA256.byteLength);
	const z = Buffer.from("KLUv/SAKUQAAaGVsbG8genN0ZA==", "base64");	// zstd("hello zstd"), from Bun.zstdCompressSync
	await T("zstd.sync", () => B.zstdDecompressSync(z).toString());
	await T("zstd.sync.isBuffer", () => Buffer.isBuffer(B.zstdDecompressSync(z)));
	await T("zstd.async", async () => (await B.zstdDecompress(z)).toString());
	await T("zstd.u8", () => B.zstdDecompressSync(new Uint8Array(z)).toString());
	await T("zstd.bad", () => B.zstdDecompressSync(Buffer.from("nope")));
	await T("gc.type", () => typeof B.gc(true));
	await T("gc.positive", () => B.gc() > 0);
	await T("sleep", () => { const t = performance.now(); B.sleepSync(20); const d = performance.now() - t; return d >= 19 && d < 200; });
	await T("sleep0", () => B.sleepSync(0));
	await T("heap", () => { const s = B.generateHeapSnapshot("v8", "arraybuffer"); return [s instanceof ArrayBuffer, JSON.parse(new TextDecoder().decode(s)).snapshot.meta.node_fields.length > 3]; });
	await T("main.abs", () => B.main.startsWith("/"));
	await T("file.text", async () => { await import("node:fs").then((fs) => fs.writeFileSync("/tmp/cc-misc-f.txt", "data\n")); return [B.file("/tmp/cc-misc-f.txt").name, B.file("/tmp/cc-misc-f.txt").size, await B.file("/tmp/cc-misc-f.txt").text(), await B.file("/tmp/cc-misc-f.txt").exists(), await B.file("/tmp/cc-misc-nope").exists()]; });
	await T("file.json", async () => { (await import("node:fs")).writeFileSync("/tmp/cc-misc-j.json", '{"a":[1]}'); return await B.file("/tmp/cc-misc-j.json").json(); });
	await T("file.bytes", async () => Buffer.from(await B.file("/tmp/cc-misc-f.txt").bytes()).toString());
	await T("file.stream", async () => await new Response(B.file("/tmp/cc-misc-f.txt").stream()).text());
	await T("stdin.text", async () => await new Response(B.stdin.stream()).text());
	return o;
}
