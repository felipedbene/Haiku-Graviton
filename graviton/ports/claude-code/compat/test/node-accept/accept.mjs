// Node 24 acceptance on the target: each check prints PASS/FAIL with the observed value.
import zlib from "node:zlib";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { Worker } from "node:worker_threads";
let fail = 0;
const check = (name, got, want) => {
	const ok = JSON.stringify(got) === JSON.stringify(want);
	if (!ok) fail++;
	console.log(`${ok ? "PASS" : "FAIL"} ${name}: ${JSON.stringify(got)}${ok ? "" : " (want " + JSON.stringify(want) + ")"}`);
};
console.log("versions", JSON.stringify({ node: process.version, v8: process.versions.v8, icu: process.versions.icu, unicode: process.versions.unicode, undici: process.versions.undici, uv: process.versions.uv, openssl: process.versions.openssl, platform: process.platform, arch: process.arch }));
const seg = (s) => [...new Intl.Segmenter("en", { granularity: "grapheme" }).segment(s)].map((x) => x.segment);
check("Segmenter ZWJ family", seg("👨‍👩‍👧‍👦").length, 1);
check("Segmenter flags", seg("🇩🇪🇯🇵").length, 2);
check("Segmenter combining", seg("éä").length, 2);
check("Segmenter Hangul jamo", seg("각").length, 1);
check("Segmenter Devanagari", seg("क्षि").length, 1);
check("Segmenter skin tone", seg("👍🏽x").length, 2);
check("Segmenter words de", [...new Intl.Segmenter("de", { granularity: "word" }).segment("Grüße, Welt!")].filter((s) => s.isWordLike).map((s) => s.segment), ["Grüße", "Welt"]);
check("NumberFormat de-DE", new Intl.NumberFormat("de-DE").format(1234567.891), "1.234.567,891");
check("NumberFormat de-DE EUR", new Intl.NumberFormat("de-DE", { style: "currency", currency: "EUR" }).format(1234.5), "1.234,50 €");
check("NumberFormat ja-JP", new Intl.NumberFormat("ja-JP", { style: "currency", currency: "JPY" }).format(5000), "￥5,000");
check("DisplayNames fr region", new Intl.DisplayNames(["fr"], { type: "region" }).of("US"), "États-Unis");
check("DateTimeFormat de", new Intl.DateTimeFormat("de-DE", { timeZone: "UTC", dateStyle: "full" }).format(new Date(Date.UTC(2026, 9, 31))), "Samstag, 31. Oktober 2026");
check("Collator de", ["z", "ä", "a"].sort(new Intl.Collator("de").compare), ["a", "ä", "z"]);
check("full ICU locales", Intl.NumberFormat.supportedLocalesOf(["fr", "zh-Hant", "hi", "ar"]).length, 4);
const raw = Buffer.from("zstd round trip ".repeat(1000));
const c = zlib.zstdCompressSync(raw);
check("zstd round trip", zlib.zstdDecompressSync(c).equals(raw), true);
check("zstd compresses", c.length < raw.length / 10, true);
check("process.execve present", typeof process.execve, "function");
check("worker_threads", await new Promise((ok) => { const w = new Worker("require('worker_threads').parentPort.postMessage(6*7)", { eval: true }); w.once("message", (m) => { ok(m); w.terminate(); }); }), 42);
const d = fs.mkdtempSync(path.join(os.tmpdir(), "acc-"));
// fs.watch is reported as observed, never skipped: a throw is a FAIL with its code.
const watchOnce = (target, poke) => new Promise((ok) => {
	let w;
	try {
		w = fs.watch(target, (ev, f) => { clearTimeout(t); w.close(); ok(`${ev}:${f}`); });
	} catch (e) {
		ok(`THREW ${e.code}`);
		return;
	}
	const t = setTimeout(() => { w.close(); ok("no event within 5s"); }, 5000);
	setTimeout(poke, 300);
});
const wDir = await watchOnce(d, () => fs.writeFileSync(path.join(d, "x.txt"), "1"));
check("fs.watch dir", wDir.startsWith("rename:") || wDir.startsWith("change:"), true);
console.log(`INFO fs.watch dir: ${wDir}`);
const wf = path.join(d, "f.txt"); fs.writeFileSync(wf, "a");
const wFile = await watchOnce(wf, () => fs.appendFileSync(wf, "b"));
check("fs.watch file", wFile.startsWith("change:"), true);
console.log(`INFO fs.watch file: ${wFile}`);
const polled = await new Promise((ok) => {
	const t = setTimeout(() => { fs.unwatchFile(wf); ok("no change within 6s"); }, 6000);
	fs.watchFile(wf, { interval: 200 }, (cur, prev) => { clearTimeout(t); fs.unwatchFile(wf); ok(cur.size - prev.size); });
	setTimeout(() => fs.appendFileSync(wf, "cc"), 500);
});
check("fs.watchFile (stat polling)", polled, 2);
const r = await fetch("https://registry.npmjs.org/ws/8.22.0").then((x) => x.json()).then((j) => j.dist.integrity, (e) => "ERR " + (e.cause?.code || e.message));
check("https fetch + TLS roots", r, "sha512-Ydggc987+RO0AnWtZ/7Wq9FtNvcrL1b/RO0ud9mWjUPgDrsAAwQSF51sm2hm1XofbU/4jkpGEsLFsZZxU+1DOg==");
console.log(`node-accept: ${fail === 0 ? "ALL PASS" : fail + " FAIL"}`);
process.exit(fail ? 1 : 0);
