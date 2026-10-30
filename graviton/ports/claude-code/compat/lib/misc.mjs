// Small Bun APIs the bundle calls, implemented on Node with Bun 1.4.4's contracts
// (probed on stock Bun; see compat/test/check-misc.mjs for the diff test).
import { createHash } from "node:crypto";
import { zstdDecompressSync as zDecSync, zstdDecompress as zDec } from "node:zlib";
import { readFileSync, statSync, writeFileSync, unlinkSync, openSync, fstatSync, createReadStream, existsSync } from "node:fs";
import { Readable } from "node:stream";
import { resolve, join } from "node:path";
import { tmpdir } from "node:os";
import v8 from "node:v8";
import vm from "node:vm";
import { toBytes } from "./hash.mjs";

// Bun.sleepSync(ms): blocks the thread.
const SLEEP = new Int32Array(new SharedArrayBuffer(4));
export function sleepSync(ms) {
	const n = Number(ms);
	if (!(n > 0)) return;
	Atomics.wait(SLEEP, 0, 0, n);
}

// Bun.gc(force): collect, return the heap size. V8 exposes gc() only behind
// --expose-gc; setting the flag at runtime and reading `gc` from a fresh context
// is V8's supported way to obtain it without a command-line flag.
let v8gc = null;
export function gc(force) {
	if (!v8gc) {
		v8.setFlagsFromString("--expose-gc");
		v8gc = vm.runInNewContext("gc");
	}
	v8gc(force ? undefined : { type: "minor" });
	return v8.getHeapStatistics().used_heap_size;
}

// Bun.SHA256: hasher class with a static one-shot hash(input, encoding?).
const digestOut = (buf, enc) => (enc === undefined ? new Uint8Array(buf) : buf.toString(enc));
export class SHA256 {
	static byteLength = 32;
	#h = createHash("sha256");
	update(data) { this.#h.update(toBytes(data)); return this; }
	digest(enc) { return digestOut(this.#h.digest(), enc); }
	static hash(input, enc) { return digestOut(createHash("sha256").update(toBytes(input)).digest(), enc); }
}

// zstd: Node's zlib.zstd* (Buffer in, Buffer out), like Bun's.
export const zstdDecompressSync = (data) => zDecSync(toBytes(data));
export const zstdDecompress = (data) => new Promise((ok, fail) => zDec(toBytes(data), (e, r) => (e ? fail(e) : ok(r))));

// Bun.generateHeapSnapshot("v8", "arraybuffer"): a V8 .heapsnapshot as an
// ArrayBuffer. Bun's other formats (its JSC object form) do not exist in V8.
export function generateHeapSnapshot(format, encoding) {
	if (format !== "v8") throw new TypeError(`generateHeapSnapshot: format ${String(format)} is not available on V8`);
	const file = join(tmpdir(), `cc-heap-${process.pid}-${Date.now()}.heapsnapshot`);
	v8.writeHeapSnapshot(file);
	try {
		const b = readFileSync(file);
		if (encoding === "arraybuffer") return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
		return b.toString("utf8");
	} finally {
		unlinkSync(file);
	}
}

// Bun.file / Bun.stdin: a lazily-read file reference (Bun's BunFile is a Blob).
// The members implemented are the ones whose semantics are plain file reads;
// Bun.spawn recognises a BunFile as a stdio target (opens the path for writing).
export class BunFile {
	#path; #fd;
	constructor(pathOrFd, type = "") {
		if (typeof pathOrFd === "number") this.#fd = pathOrFd;
		else this.#path = resolve(String(pathOrFd instanceof URL ? pathOrFd.pathname : pathOrFd));
		this.type = type;
	}
	get name() { return this.#path ?? this.#fd; }
	get fd() { return this.#fd; }
	get path() { return this.#path; }
	get size() {
		try { return this.#fd !== undefined ? fstatSync(this.#fd).size : statSync(this.#path).size; } catch { return 0; }
	}
	async exists() { return this.#path !== undefined ? existsSync(this.#path) : true; }
	#readAll() { return readFileSync(this.#fd !== undefined ? this.#fd : this.#path); }
	async bytes() { return new Uint8Array(this.#readAll()); }
	async arrayBuffer() { const b = this.#readAll(); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); }
	async text() { return this.#readAll().toString("utf8"); }
	async json() { return JSON.parse(await this.text()); }
	stream() {
		if (this.#fd === 0) return Readable.toWeb(process.stdin);
		return Readable.toWeb(createReadStream(null, { fd: this.#fd !== undefined ? this.#fd : openSync(this.#path, "r") }));
	}
}
export const file = (p, opts) => new BunFile(p, opts?.type ?? "");
export const stdin = new BunFile(0);

// Bun.main: the absolute path of the entry module.
export const main = resolve(process.argv[1] ?? "");
