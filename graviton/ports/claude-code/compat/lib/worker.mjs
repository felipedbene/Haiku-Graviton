// Bun's global `Worker` (the Web Worker API) on Node's worker_threads.
//
// The bundle uses it in two places: a blob-URL CommonJS script that writes stdout on
// a thread (chunk-2hxx68dq.js writeOnAThread) and the function-hooks host, an ESM file
// with `workerData` (chunk-zy2k3hny.js). Contract measured on stock Bun 1.4.4 with
// compat/test/worker (see check.mjs):
//  - Worker is an EventTarget; onmessage/onerror handlers; MessageEvent/ErrorEvent;
//    postMessage(data, transfer), terminate(), ref(), unref().
//  - Inside: `self === globalThis`, global onmessage/addEventListener/postMessage/close,
//    `require` in a blob script, `Bun` defined, worker_threads' workerData untouched.
//  - An uncaught error is reported as an ErrorEvent on the Worker and the worker keeps
//    running (Node would end the thread); terminate() stops all delivery at once.
// Everything else Bun's Worker accepts (preload, smol, env, argv, relative paths, ...)
// throws instead of being ignored.
import { Worker as NodeWorker, MessageChannel } from "node:worker_threads";
import { resolveObjectURL } from "node:buffer";
import { isAbsolute } from "node:path";
import { pathToFileURL } from "node:url";

const BOOT = new URL("./worker-boot.mjs", import.meta.url);
const OPTIONS = new Set(["name", "workerData", "type"]);

export class ErrorEvent extends Event {
	#message; #error;
	constructor(type, init = {}) {
		super(type, init);
		this.#message = String(init.message ?? "");
		this.#error = init.error;
	}
	get message() { return this.#message; }
	get error() { return this.#error; }
	get filename() { return ""; }
	get lineno() { return 0; }
	get colno() { return 0; }
}

const notSupported = (what) => new TypeError(`Worker: ${what} is not supported by the Node compat layer`);

export class Worker extends EventTarget {
	#w; #done = false;
	onmessage = null;
	onerror = null;
	onmessageerror = null;

	constructor(url, options) {
		super();
		if (arguments.length === 0) throw new TypeError("Worker constructor requires a URL");
		if (typeof url !== "string" && !(url instanceof URL)) throw new TypeError("Worker URL must be a string or URL");
		const opts = options ?? {};
		for (const k of Object.keys(opts)) if (!OPTIONS.has(k)) throw notSupported(`option "${k}"`);
		if (opts.type !== undefined && opts.type !== "module" && opts.type !== "classic") throw notSupported(`type "${opts.type}"`);
		const spec = String(url);
		let init;
		if (spec.startsWith("blob:")) {
			const blob = resolveObjectURL(spec);
			if (blob === undefined) throw new TypeError(`Worker: ${spec} is not a live object URL`);
			init = { kind: "blob", blob, url: spec };
		} else if (spec.startsWith("file:")) {
			init = { kind: "module", url: spec };
		} else if (isAbsolute(spec)) {
			init = { kind: "module", url: pathToFileURL(spec).href };
		} else {
			throw notSupported(`a relative or non-file URL (${spec})`);
		}
		const ctl = new MessageChannel();
		const w = new NodeWorker(BOOT, { name: opts.name, workerData: opts.workerData });
		this.#w = w;
		// The first message is the boot's (read synchronously before the target runs);
		// user messages queue behind it untouched.
		w.postMessage({ __ccWorkerInit: { ...init, ctl: ctl.port2 } }, [ctl.port2]);
		w.on("message", (data) => this.#deliver(new MessageEvent("message", { data })));
		w.on("messageerror", (error) => this.#deliver(new MessageEvent("messageerror", { data: error })));
		w.on("error", (error) => this.#fail(error));
		w.on("exit", () => { this.#done = true; ctl.port1.close(); });
		ctl.port1.on("message", (m) => {
			if (m.type === "error") this.#fail(m.error, m.message);
		});
		ctl.port1.unref();
	}

	#deliver(ev) {
		if (this.#done) return;
		const h = ev.type === "message" ? this.onmessage : this.onmessageerror;
		if (typeof h === "function") h.call(this, ev);
		this.dispatchEvent(ev);
	}

	#fail(error, message) {
		if (this.#done) return;
		const ev = new ErrorEvent("error", { error, message: message ?? (error instanceof Error ? `${error.name}: ${error.message}` : String(error)) });
		if (typeof this.onerror === "function") this.onerror.call(this, ev);
		this.dispatchEvent(ev);
	}

	postMessage(data, transfer) {
		if (this.#done) return;
		this.#w.postMessage(data, Array.isArray(transfer) ? transfer : transfer?.transfer);
	}

	terminate() {
		if (this.#done) return;
		this.#done = true;
		this.#w.terminate();
	}

	ref() { this.#w.ref(); return this; }
	unref() { this.#w.unref(); return this; }
	get threadId() { return this.#w.threadId; }
}
