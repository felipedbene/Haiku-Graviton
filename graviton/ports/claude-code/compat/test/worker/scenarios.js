// Worker scenarios run identically under stock Bun 1.4.4 (recording) and under
// Node 24 + compat (checking). Each returns JSON-able observations; nothing is
// normalised except what is noted per scenario.
const { writeFileSync, mkdtempSync } = require("fs");
const { join } = require("path");
const os = require("os");
const dir = mkdtempSync(join(os.tmpdir(), "wk-"));
const file = (name, src) => { const p = join(dir, name); writeFileSync(p, src); return p; };
const blob = (src) => URL.createObjectURL(new Blob([src], { type: "application/javascript" }));
const reply = (w, msg, transfer) => new Promise((ok, fail) => {
	const t = setTimeout(() => fail(new Error("timeout")), 5000);
	w.onmessage = (e) => { clearTimeout(t); ok(e); };
	w.onerror = (e) => { clearTimeout(t); ok({ error: true, type: e.type, message: e.message, ctor: e.constructor.name }); };
	if (msg !== undefined) w.postMessage(msg, transfer);
});
const evt = (e) => e.error ? e : { type: e.type, ctor: e.constructor.name, data: e.data };

const S = {};
S["globals"] = async () => ({
	typeofWorker: typeof Worker,
	isEventTarget: Worker.prototype instanceof EventTarget,
	members: ["postMessage", "terminate", "ref", "unref", "addEventListener", "removeEventListener"].map((k) => [k, typeof Worker.prototype[k]]),
});
S["blob CommonJS script with require and onmessage assignment"] = async () => {
	const w = new Worker(blob(`
const { writeSync } = require('fs')
onmessage = ({ data }) => { postMessage({ echo: data, writeSync: typeof writeSync, self: typeof self, selfIsGlobal: self === globalThis, bun: typeof Bun, name: typeof name === "string" ? name : null }) }
`), { name: "probe" });
	const e = await reply(w, { a: 1, b: [1, 2] });
	w.terminate();
	return evt(e);
};
S["file ESM worker, workerData, self.onmessage"] = async () => {
	const p = file("esm.mjs", `
import { workerData, isMainThread, threadId } from "worker_threads";
self.onmessage = (e) => postMessage({ got: e.data, workerData, isMainThread, threadIdType: typeof threadId });
`);
	const w = new Worker(new URL("file://" + p), { name: "x", workerData: { stamp: 7, list: [1, 2] } });
	const e = await reply(w, "hi");
	w.terminate();
	return evt(e);
};
S["string path to ESM file"] = async () => {
	const p = file("plain.mjs", `postMessage({ ok: true, meta: typeof import.meta.url })`);
	const w = new Worker(p);
	const e = await reply(w);
	w.terminate();
	return evt(e);
};
S["MessagePort transfer and port.onmessage"] = async () => {
	const w = new Worker(blob(`
onmessage = (e) => { const port = e.data.port; port.onmessage = (m) => port.postMessage({ back: m.data * 2 }); postMessage("ready") }
`));
	const { port1, port2 } = new MessageChannel();
	await reply(w, { port: port2 }, [port2]);
	const r = await new Promise((ok) => { port1.onmessage = (m) => ok({ data: m.data, ctor: m.constructor.name }); port1.postMessage(21); });
	port1.close(); w.terminate();
	return r;
};
S["SharedArrayBuffer + Atomics across threads"] = async () => {
	const sab = new SharedArrayBuffer(16), a = new Int32Array(sab);
	const w = new Worker(blob(`
onmessage = ({ data }) => { const a = new Int32Array(data); Atomics.store(a, 1, 42); Atomics.store(a, 0, 1); Atomics.notify(a, 0) }
`));
	w.postMessage(sab);
	const r = Atomics.wait(a, 0, 0, 5000);
	w.terminate();
	return { wait: r, value: Atomics.load(a, 1) };
};
S["Uint8Array slice clones by value"] = async () => {
	const w = new Worker(blob(`onmessage = ({ data }) => postMessage({ ctor: data.constructor.name, len: data.length, first: data[0], last: data[data.length - 1] })`));
	const e = await reply(w, Uint8Array.prototype.slice.call(new Uint8Array([9, 8, 7, 6, 5]), 1, 4));
	w.terminate();
	return evt(e);
};
S["addEventListener('message')"] = async () => {
	const w = new Worker(blob(`addEventListener("message", (e) => postMessage(["via listener", e.data]))`));
	const r = await new Promise((ok) => { w.addEventListener("message", (e) => ok({ data: e.data, type: e.type })); w.postMessage(3); });
	w.terminate();
	return r;
};
S["uncaught throw in worker -> error event"] = async () => {
	const w = new Worker(blob(`onmessage = () => { throw new TypeError("boom here") }`));
	const e = await reply(w, 1);
	w.terminate();
	return e;
};
S["syntax error in blob -> error event"] = async () => {
	const w = new Worker(blob(`this is not js (`));
	const e = await reply(w);
	w.terminate();
	return { error: e.error === true, type: e.type, ctor: e.ctor };
};
S["self.close() stops the worker"] = async () => {
	const w = new Worker(blob(`onmessage = (e) => { postMessage("bye"); close(); }`));
	const first = (await reply(w, 1)).data;
	let more = false;
	w.onmessage = () => { more = true; };
	w.postMessage(2);
	await new Promise((r) => setTimeout(r, 300));
	return { first, more };
};
S["terminate stops delivery"] = async () => {
	const w = new Worker(blob(`onmessage = (e) => setTimeout(() => postMessage("late"), 200)`));
	let got = false;
	w.onmessage = () => { got = true; };
	w.postMessage(1);
	w.terminate();
	await new Promise((r) => setTimeout(r, 500));
	return { got };
};
S["constructor errors"] = async () => {
	const r = {};
	for (const [k, f] of [["no args", () => new Worker()], ["number", () => new Worker(5)]]) {
		try { f(); r[k] = "no throw"; } catch (e) { r[k] = [e.constructor.name]; }
	}
	return r;
};
module.exports = { S, dir };
