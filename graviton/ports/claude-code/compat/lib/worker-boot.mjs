// Thread entry for compat/lib/worker.mjs: give the thread Bun's Web Worker global
// scope, load the compat layer, then run the target (a blob script or an ESM file).
import { parentPort, receiveMessageOnPort } from "node:worker_threads";
import { createRequire } from "node:module";
import vm from "node:vm";

// The init message was posted before this thread started, so it is already queued.
const first = receiveMessageOnPort(parentPort);
const init = first?.message?.__ccWorkerInit;
if (init === undefined) throw new Error("compat worker: missing init message");
const ctl = init.ctl;

// Global scope: self, postMessage, close, onmessage, add/removeEventListener.
const scope = new EventTarget();
let onmessage = null;
Object.defineProperties(globalThis, {
	self: { value: globalThis, writable: true, configurable: true },
	onmessage: { get: () => onmessage, set: (f) => { onmessage = f; }, configurable: true, enumerable: true },
	postMessage: { value: (data, transfer) => parentPort.postMessage(data, Array.isArray(transfer) ? transfer : transfer?.transfer), writable: true, configurable: true },
	close: { value: () => { parentPort.close(); process.exit(0); }, writable: true, configurable: true },
	addEventListener: { value: scope.addEventListener.bind(scope), writable: true, configurable: true },
	removeEventListener: { value: scope.removeEventListener.bind(scope), writable: true, configurable: true },
	dispatchEvent: { value: scope.dispatchEvent.bind(scope), writable: true, configurable: true },
});

// Uncaught errors are reported to the Worker object and the thread keeps running.
const report = (error) => {
	let clone;
	try { clone = structuredClone(error); } catch { clone = String(error); }
	ctl.postMessage({ type: "error", error: clone, message: error instanceof Error ? `${error.name}: ${error.message}` : String(error) });
};
process.on("uncaughtException", report);
process.on("unhandledRejection", report);

await import("../bun-compat.mjs");

try {
	if (init.kind === "blob") {
		const text = await init.blob.text();
		let fn;
		try {
			fn = new vm.Script(`(function (exports, require, module, __filename, __dirname) {${text}\n})`, { filename: init.url }).runInThisContext();
		} catch (e) {
			if (!(e instanceof SyntaxError)) throw e;
			// Not a script: a module (Bun accepts either in a blob).
			fn = null;
			await import("data:text/javascript;base64," + Buffer.from(text).toString("base64"));
		}
		if (fn) {
			const module = { exports: {} };
			fn.call(module.exports, module.exports, createRequire(process.cwd() + "/"), module, init.url, process.cwd());
		}
	} else {
		await import(init.url);
	}
} catch (e) {
	report(e);
}

parentPort.on("message", (data) => {
	const ev = new MessageEvent("message", { data });
	if (typeof onmessage === "function") onmessage.call(globalThis, ev);
	scope.dispatchEvent(ev);
});
