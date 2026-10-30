// Bun.spawn(cmd, options) / Bun.spawn({cmd, ...options}) on Node's child_process,
// following Bun bun-v1.4.4 (rev 38afee6c84fc):
//   src/runtime/api/bun/js_bun_spawn_bindings.rs  option parsing, argv/env/PATH, error shapes
//   src/runtime/api/bun/spawn/stdio.rs            Stdio::extract / extract_blob / as_spawn_option
//   src/spawn_sys/spawn_process.rs                child-side file actions (open flags, /dev/null, extra fds)
//   src/runtime/api/bun/subprocess.rs             Subprocess getters, kill, ref/unref, exited, asyncDispose
//   src/runtime/api/bun/subprocess/Readable.rs    stdout/stderr getter values
//   src/runtime/api/bun/subprocess/Writable.rs    stdin getter values
//   src/runtime/api/BunObject.classes.ts          the Subprocess prototype surface
//   src/sys_jsc/signal_code_jsc.rs                SignalCode.fromJS
//   src/jsc/JSValue.rs (get_truthy), src/jsc/JSArrayIterator.rs, src/jsc/JSGlobalObject.rs
//     (validate_integer_range), src/jsc/bindings/c-bindings.cpp (BUN_DEFAULT_PATH_FOR_SPAWN)
// Behaviour was then pinned by diff-testing against stock Bun 1.4.4 on Linux arm64
// (compat/test/spawn-scenarios.mjs). What Node cannot express is rejected with an
// explicit error naming the feature (UNSUPPORTED below), never approximated.
import { ChildProcess } from "node:child_process";
import { openSync, closeSync, statSync, accessSync, constants as FS } from "node:fs";
import { constants as OS } from "node:os";
import { resolve as resolvePath } from "node:path";
import { getSystemErrorMap, inspect } from "node:util";
import { BunFile } from "./misc.mjs";
import { bunWhich } from "./which.mjs";

// Bun inherits the env loader's map, which is fixed at startup: process.env
// writes made later (even to PATH) are not seen by Bun.spawn's default env or its
// executable lookup (verified on stock Bun). The analogue here is the env as it
// stands when this module is first evaluated, i.e. at preload time.
const STARTUP_ENV = Object.entries(process.env).map(([k, v]) => `${k}=${v}`);
const STARTUP_PATH = process.env.PATH ?? "";
// Bun resolves bare names against its top_level_dir (the start-up cwd) when no
// cwd option is given, like Bun.which.
const TOP_LEVEL_DIR = process.cwd();
// _PATH_DEFPATH on Linux/FreeBSD/macOS, Bun's own fallback elsewhere (Haiku included).
const DEFAULT_PATH = process.platform === "darwin" ? "/usr/bin:/bin:/usr/sbin:/sbin" : "/usr/bin:/bin";
const MAX_PATH_BYTES = process.platform === "haiku" ? 1024 : 4096;
const IS_LINUX = process.platform === "linux" || process.platform === "android";

// ---- errors, shaped like Bun's ----
const UV = new Map();
const UV_MESSAGE = new Map();
for (const [n, [name, msg]] of getSystemErrorMap()) {
	if (!UV.has(name)) UV.set(name, n);
	UV_MESSAGE.set(n, msg);
}
const coded = (E, code, msg) => Object.assign(new E(msg), { code });
const invalidArgs = (msg) => coded(TypeError, "ERR_INVALID_ARG_TYPE", msg);
const invalidValue = (msg) => coded(TypeError, "ERR_INVALID_ARG_VALUE", msg);
const outOfRange = (field, it, got) => coded(RangeError, "ERR_OUT_OF_RANGE", `The value of "${field}" is out of range. It must be ${it}. Received ${got}`);
// SystemError.toErrorInstance: "<CODE>: <description>, <syscall>[ '<path>']" with
// code, errno (libuv-negative), syscall and path as properties.
function systemError(code, errno, syscall, path) {
	const desc = UV_MESSAGE.get(errno) ?? code;
	const e = new Error(`${code}: ${desc}, ${syscall}${path === undefined ? "" : ` '${path}'`}`);
	e.code = code;
	e.errno = errno;
	e.syscall = syscall;
	if (path !== undefined) e.path = path;
	return e;
}
function commandNotFound(command) {
	const e = new Error(`Executable not found in $PATH: "${command}"`);
	e.code = "ENOENT";
	e.errno = UV.get("ENOENT");
	e.path = command;
	return e;
}
// Features of Bun.spawn that have no Node child_process equivalent. Thrown, so a
// caller that depends on them fails loudly instead of getting something else.
const UNSUPPORTED = (what) => new Error(`Bun.spawn: ${what} is not supported by the Node compat layer`);
// bun_fmt::quote: a JSON-style double-quoted string.
const quote = (s) => JSON.stringify(s);
// JSValue.toBunString is ECMAScript ToString; JSC's wording for the one case that throws.
function toStr(v) {
	if (typeof v === "symbol") throw new TypeError("Cannot convert a symbol to a string");
	return `${v}`;
}
const isObject = (v) => (typeof v === "object" && v !== null) || typeof v === "function";
// JSValue.get_truthy: undefined, null and "" read as absent.
const truthy = (o, k) => { const v = o[k]; return v === undefined || v === null || v === "" ? undefined : v; };
const bytes = (s) => Buffer.byteLength(s, "utf8");

// Node's ERR_INVALID_ARG_TYPE "Received ..." tail, which Bun reuses for `signal`.
function received(v) {
	if (v === null || v === undefined) return ` Received ${v}`;
	if (typeof v === "function") return ` Received function ${v.name}`;
	if (typeof v === "object") {
		if (v.constructor?.name) return ` Received an instance of ${v.constructor.name}`;
		return ` Received ${inspect(v, { depth: -1 })}`;
	}
	let shown = inspect(v, { colors: false });
	if (shown.length > 28) shown = `${shown.slice(0, 25)}...`;
	return ` Received type ${typeof v} (${shown})`;
}

// JSGlobalObject.validate_integer_range.
function integerInRange(value, field, min, max, dflt) {
	if (value === undefined) return dflt;
	const lo = Math.max(min, Number.MIN_SAFE_INTEGER), hi = Math.min(max, Number.MAX_SAFE_INTEGER);
	if (typeof value !== "number") throw invalidArgs(`The "${field}" property must be of type number. Received ${value === null ? "null" : typeof value}`);
	if (Number.isNaN(value)) return dflt;
	if (Math.floor(value) !== value) throw invalidArgs(`The "${field}" property must be of type integer. Received number`);
	if (value < lo || value > hi) throw outOfRange(field, `>= ${min} and <= ${max}`, String(value));
	return value;
}

// ---- signals (SignalCode) ----
// Canonical name per number: the first os.constants.signals entry for it, which
// is Bun's name set on Linux (SIGABRT over SIGIOT, SIGIO over SIGPOLL).
const SIG_NAME = new Map();
for (const [name, n] of Object.entries(OS.signals)) if (n >= 1 && n <= 31 && !SIG_NAME.has(n)) SIG_NAME.set(n, name);
const SIG_BY_NAME = new Map([...SIG_NAME].map(([n, name]) => [name, n]));
const SIG_LIST = [...SIG_NAME.keys()].sort((a, b) => a - b).map((n) => `'${SIG_NAME.get(n)}'`);
const SIG_ENUM_MSG = `signal must be one of ${SIG_LIST.slice(0, -1).join(", ")} or ${SIG_LIST.at(-1)}`;
const SIGTERM = OS.signals.SIGTERM;
function signalFromJS(arg) {
	if (typeof arg === "number") {
		if (Number.isNaN(arg)) return SIGTERM;
		if (!Number.isFinite(arg) || Math.trunc(arg) !== arg) throw invalidArgs("Unknown signal");
		if (arg < 0) throw invalidArgs("Invalid signal: must be >= 0");
		if (arg > 31) throw invalidArgs("Invalid signal: must be < 32");
		return arg;
	}
	if (typeof arg === "string") {
		if (arg.length === 0) return SIGTERM;
		const n = SIG_BY_NAME.get(arg);
		if (n === undefined) throw invalidArgs(SIG_ENUM_MSG);
		return n;
	}
	if (arg !== undefined && arg !== null) throw invalidArgs("Invalid signal: must be a string or an integer");
	return SIGTERM;
}

// ---- stdio (Stdio::extract) ----
// kinds: inherit | ignore | pipe | fd{fd} | path{path} | buffer{data}
const INHERIT = { kind: "inherit" }, IGNORE = { kind: "ignore" }, PIPE = { kind: "pipe" };
function isArrayBufferLike(v) {
	return v instanceof ArrayBuffer || ArrayBuffer.isView(v) || (typeof SharedArrayBuffer === "function" && v instanceof SharedArrayBuffer);
}
function extractStdio(i, value, current) {
	if (value === undefined) return current;
	if (value === null) return IGNORE;
	if (typeof value === "string") {
		if (value === "inherit") return INHERIT;
		if (value === "ignore") return IGNORE;
		if (value === "pipe" || value === "overlapped") {
			if (i >= 3) throw UNSUPPORTED(`stdio[${i}] "pipe" (an extra pipe handed out as a raw fd)`);
			return PIPE;
		}
		if (value === "socket-fd") {
			if (i < 3) throw invalidArgs("stdio: 'socket-fd' is only supported at indices >= 3");
			throw UNSUPPORTED(`stdio[${i}] "socket-fd"`);
		}
		if (value === "ipc") throw UNSUPPORTED(`stdio[${i}] "ipc"`);
		throw invalidArgs("stdio must be an array of 'inherit', 'pipe', 'ignore', Bun.file(pathOrFd), number, or null");
	}
	if (typeof value === "number") {
		const fd = value | 0;
		if (fd < 0) throw invalidArgs("file descriptor must be a positive integer");
		if (fd >= 2147483647) throw invalidArgs(`file descriptor must be a valid integer, received: ${String(value)}`);
		if (fd === 0) {
			if (i === 1 || i === 2) throw invalidArgs("stdin cannot be used for stdout or stderr");
			return INHERIT;
		}
		if (fd === 1 || fd === 2) {
			if (i === 0) throw invalidArgs("stdout and stderr cannot be used for stdin");
			if (i === fd) return INHERIT;
		}
		return { kind: "fd", fd };
	}
	if (value instanceof BunFile) {
		// A file-backed Blob: a path is opened by the child, an fd is dup'd into it.
		if (value.fd !== undefined) return value.fd === i && i < 3 ? INHERIT : { kind: "fd", fd: value.fd };
		return { kind: "path", path: value.path };
	}
	if (value instanceof Blob) {
		if (i === 1 || i === 2) throw invalidArgs("Blobs are immutable, and cannot be used for stdout/stderr");
		if (value.size === 0) return IGNORE;
		if (i !== 0) throw invalidArgs(`Blob cannot be used for stdio[${i}] yet`);
		return { kind: "buffer", data: value };
	}
	if (value instanceof Request || value instanceof Response) throw UNSUPPORTED(`a ${value.constructor.name} body as stdio[${i}]`);
	if (value instanceof ReadableStream) throw UNSUPPORTED(`a ReadableStream as stdio[${i}]`);
	if (isArrayBufferLike(value)) {
		const view = ArrayBuffer.isView(value) ? new Uint8Array(value.buffer, value.byteOffset, value.byteLength) : new Uint8Array(value);
		if (view.byteLength === 0) return IGNORE;
		if (i === 1 || i === 2) throw invalidArgs("ArrayBufferView cannot be used for stdout/stderr yet");
		if (i !== 0) throw UNSUPPORTED(`an ArrayBuffer as stdio[${i}]`);
		return { kind: "buffer", data: new Uint8Array(view) };
	}
	throw invalidArgs("stdio must be an array of 'inherit', 'ignore', or null");
}

// ---- argv (get_argv / get_argv0) ----
function arrayLike(v) {
	// JSArrayIterator: the length property, then index access (holes read undefined).
	const len = Math.min(Math.max(Math.trunc(Number(v?.length)) || 0, 0), 2 ** 32 - 1);
	return { len, at: (i) => v[i] };
}
function getArgv(cmdValue, path, cwd, argv0) {
	if (cmdValue === undefined || cmdValue === null) throw invalidArgs("cmd must be an array of strings");
	const it = arrayLike(cmdValue);
	if (it.len === 0) throw invalidArgs("cmd must not be empty");
	if (it.len > 2 ** 32 - 3) throw invalidArgs("cmd array is too large");
	const arg0 = toStr(it.at(0));
	if (arg0.includes("\0")) throw invalidValue(`The argument 'args[0]' must be a string without null bytes. Received ${quote(arg0)}`);
	// execvpe's rule: a name with a slash is used as given, relative to the child's cwd.
	const pathToUse = arg0.includes("/") ? "" : path !== "" ? path : DEFAULT_PATH;
	let file = arg0;
	if (pathToUse !== "") {
		const found = bytes(arg0) >= MAX_PATH_BYTES ? null : bunWhich(arg0, { PATH: pathToUse, cwd });
		if (!found) throw commandNotFound(arg0);
		file = found;
	}
	const args = [argv0 ?? arg0];
	for (let i = 1; i < it.len; i++) {
		const a = toStr(it.at(i));
		if (a.includes("\0")) throw invalidValue(`The argument 'args[${i}]' must be a string without null bytes. Received "${a}"`);
		args.push(a);
	}
	return { file, args };
}

// The child does chdir() before opening path stdio (spawn_process.rs adds the
// chdir file action first), so a bad cwd is the error a caller sees even when a
// stdio path is also bad. Paths are opened here in the parent, so the chdir is
// checked the way the kernel would: existence, directory, search permission.
function checkChdir(cwd) {
	let st;
	try { st = statSync(cwd); } catch (e) { return e; }
	if (!st.isDirectory()) return { code: "ENOTDIR", errno: UV.get("ENOTDIR") };
	try { accessSync(cwd, FS.X_OK); } catch (e) { return e; }
	return null;
}

// ---- the stream and sink objects ----
// Bun's subprocess pipes are ReadableStreams whose prototype also carries
// text/json/bytes/blob (not arrayBuffer). Chunks are plain Uint8Arrays.
class SubprocessReadableStream extends ReadableStream {
	async bytes() {
		const parts = [];
		let n = 0;
		const r = this.getReader();
		for (;;) {
			const { done, value } = await r.read();
			if (done) break;
			parts.push(value);
			n += value.byteLength;
		}
		const out = new Uint8Array(n);
		let o = 0;
		for (const p of parts) { out.set(p, o); o += p.byteLength; }
		return out;
	}
	async text() { return new TextDecoder().decode(await this.bytes()); }
	async json() { return JSON.parse(await this.text()); }
	async blob() { return new Blob([await this.bytes()]); }
}

// The parent side of a pipe, read eagerly into the stream's queue (Bun's
// PipeReader read_all for non-lazy spawns), so a child never blocks on a full
// pipe that nobody has read yet.
function readableFor(sock, maxBuffer, onOverflow, lazy) {
	let ctl, done = false, total = 0, overflowed = false, started = false;
	const stream = new SubprocessReadableStream({
		start(c) { ctl = c; },
		cancel() { done = true; sock.destroy(); },
	});
	// lazy: leave the bytes in the pipe until the getter is read. Node resumes any
	// unconsumed child stream after exit (flushStdio) and drops what it reads; a
	// 'readable' listener makes that resume() a no-op, so the data waits.
	const hold = () => {};
	if (lazy) sock.on("readable", hold);
	const start = () => {
		if (started) return;
		started = true;
		sock.on("data", (b) => {
			total += b.length;
			if (maxBuffer !== undefined && !overflowed && total > maxBuffer) { overflowed = true; onOverflow(); }
			if (!done) ctl.enqueue(new Uint8Array(b));
		});
		sock.on("end", () => { if (!done) { done = true; ctl.close(); } });
		sock.on("error", (e) => { if (!done) { done = true; ctl.error(e); } });
		if (lazy) {
			sock.off("readable", hold);
			// readableListening is cleared on the next tick; resume() after it.
			process.nextTick(() => sock.resume());
		}
	};
	return { stream, start, sock };
}

const toBytes = (chunk) => {
	if (typeof chunk === "string") return Buffer.from(chunk, "utf8");
	if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
	if (chunk instanceof ArrayBuffer || (typeof SharedArrayBuffer === "function" && chunk instanceof SharedArrayBuffer)) return new Uint8Array(chunk);
	throw new TypeError("FileSink.write() expects a string, ArrayBufferView, or ArrayBuffer");
};
// stdin: "pipe" is a FileSink. write() reports the bytes accepted; flush()/end()
// report the bytes written since the previous flush (as Bun's buffered sink does).
class FileSink {
	#sock; #pending = 0; #error = null;
	constructor(sock) {
		this.#sock = sock;
		// A child that closes its stdin makes later writes fail; that surfaces on
		// the next write() rather than as an unhandled 'error' event.
		sock.on("error", (e) => { this.#error = e; });
	}
	write(chunk) {
		if (this.#error) throw this.#error;
		const b = toBytes(chunk);
		this.#sock.write(b);
		this.#pending += b.byteLength;
		return b.byteLength;
	}
	flush() { const n = this.#pending; this.#pending = 0; return n; }
	end(error) {
		const n = this.flush();
		if (error) this.#sock.destroy(error);
		else this.#sock.end();
		return n;
	}
	start() {}
	ref() { this.#sock.ref(); }
	unref() { this.#sock.unref(); }
}

// ---- Subprocess ----
const noop = () => {};
// try_kill for internal callers (maxBuffer): errors are not reported, as in Bun.
const KILL_QUIETLY = Symbol("killQuietly");
class Subprocess {
	#child; #pid; #status = null; #exited = null; #resolveExited = null;
	#stdin; #stdout; #stderr; #extra; #observed = new Set(); #pipes;
	#killSignal; #onExit; #timer = null; #abort = null;

	constructor(child, init) {
		this.#child = child;
		this.#pid = child.pid;
		this.#stdin = init.stdin;
		this.#stdout = init.stdout;
		this.#stderr = init.stderr;
		this.#extra = init.extra;
		this.#pipes = init.pipes;
		this.#killSignal = init.killSignal;
		this.#onExit = init.onExit;
		child.on("exit", (code, signal) => this.#onProcessExit(code, signal));
		if (init.timeout !== undefined) this.#timer = setTimeout(() => this.#tryKill(this.#killSignal), init.timeout);
		if (init.signal) {
			const onAbort = () => this.#tryKill(this.#killSignal);
			init.signal.addEventListener("abort", onAbort, { once: true });
			this.#abort = () => init.signal.removeEventListener("abort", onAbort);
		}
	}

	#onProcessExit(code, signal) {
		this.#status = signal ? { signal: OS.signals[signal] } : { code };
		if (this.#timer) { clearTimeout(this.#timer); this.#timer = null; }
		if (this.#abort) { this.#abort(); this.#abort = null; }
		// The pending promise is handed out once; later reads of .exited get a
		// fresh settled promise (subprocess.rs exited_promise_take_cached).
		if (this.#resolveExited) { this.#resolveExited(this.#exitValue()); this.#resolveExited = null; this.#exited = null; }
		if (this.#onExit) this.#onExit(this, this.exitCode, this.signalCode, undefined);
	}
	#exitValue() { return this.#status.signal !== undefined ? 128 + this.#status.signal : this.#status.code; }
	#tryKill(sig) {
		if (this.#status) return 0;
		const h = this.#child._handle;
		// Node clears _handle in the same callback that emits 'exit'.
		if (!h) return 0;
		const r = h.kill(sig);
		return r === UV.get("ESRCH") ? 0 : r;
	}

	[KILL_QUIETLY](sig) { this.#tryKill(sig); }

	get pid() { return this.#pid; }
	get stdin() { this.#observed.add("stdin"); return this.#stdin; }
	get writable() { return this.stdin; }
	get stdout() { this.#observed.add("stdout"); this.#pipes.stdout?.start(); return this.#stdout; }
	get readable() { return this.stdout; }
	get stderr() { this.#observed.add("stderr"); this.#pipes.stderr?.start(); return this.#stderr; }
	get stdio() { return [null, null, null, ...this.#extra]; }
	get terminal() { return undefined; }
	get connected() { return false; }
	get killed() { return this.#status !== null; }
	get exitCode() { return this.#status && this.#status.signal === undefined ? this.#status.code : null; }
	get signalCode() {
		if (!this.#status || this.#status.signal === undefined) return null;
		return SIG_NAME.get(this.#status.signal) ?? this.#status.signal;
	}
	get exited() {
		if (this.#exited) return this.#exited;
		if (this.#status) return Promise.resolve(this.#exitValue());
		this.#exited = new Promise((r) => { this.#resolveExited = r; });
		return this.#exited;
	}
	kill(signal) {
		const sig = signalFromJS(signal);
		const r = this.#tryKill(sig);
		if (r) throw systemError(UV_NAME(r), r, "kill");
		return undefined;
	}
	// Keep-alive follows Bun's js_ref/js_unref: the process, plus each stdio pipe
	// whose getter has not been read yet.
	ref() {
		this.#child.ref();
		for (const k of ["stdin", "stdout", "stderr"]) if (!this.#observed.has(k)) this.#pipes[k]?.sock.ref();
	}
	unref() {
		this.#child.unref();
		for (const k of ["stdin", "stdout", "stderr"]) if (!this.#observed.has(k)) this.#pipes[k]?.sock.unref();
	}
	resourceUsage() {
		if (!this.#status) return undefined;
		throw UNSUPPORTED("resourceUsage() (Node does not report a child's rusage)");
	}
	send() {
		throw coded(Error, "ERR_IPC_CHANNEL_CLOSED", "Subprocess.send() can only be used if an IPC channel is open.");
	}
	disconnect() { return undefined; }
	[Symbol.asyncDispose]() {
		if (this.#status) return undefined;
		for (const k of ["stdin", "stdout", "stderr"]) this.#pipes[k]?.sock.unref();
		const r = this.#tryKill(this.#killSignal);
		if (r) throw systemError(UV_NAME(r), r, "kill");
		return this.exited;
	}
	get [Symbol.toStringTag]() { return "Subprocess"; }
}
// Bun's Subprocess has no constructor of its own (noConstructor in the class
// definition), so `proc.constructor` resolves to Object.
delete Subprocess.prototype.constructor;
const UV_NAME = (n) => getSystemErrorMap().get(n)?.[0] ?? `E${-n}`;

// ---- Bun.spawn ----
export function spawn(cmdOrOptions, maybeOptions) {
	let cmdValue, args;
	if (cmdOrOptions === undefined || cmdOrOptions === null) throw invalidArgs("cmd must be an array");
	if (Array.isArray(cmdOrOptions)) {
		cmdValue = cmdOrOptions;
		args = maybeOptions;
	} else if (!isObject(cmdOrOptions)) {
		throw invalidArgs("cmd must be an array");
	} else {
		cmdValue = truthy(cmdOrOptions, "cmd");
		if (cmdValue === undefined) throw invalidArgs("cmd must be an array");
		args = cmdOrOptions;
	}

	let cwd = TOP_LEVEL_DIR, userCwd;
	let argv0;
	let path = STARTUP_PATH;
	let envPairs = null;
	const stdio = [IGNORE, PIPE, INHERIT];
	const extra = [];
	let lazy = false, detached = false, uid, gid, timeout, killSignal = SIGTERM, maxBuffer;
	let onExit, signal, windowsHide = false, windowsVerbatimArguments = false;
	let argv;

	if (isObject(args)) {
		const a0 = truthy(args, "argv0");
		if (a0 !== undefined) {
			const s = toStr(a0);
			if (s.length > 0) {
				if (s.includes("\0")) throw invalidValue(`The property 'options.argv0' must be a string without null bytes. Received ${quote(s)}`);
				argv0 = s;
			}
		}
		const c = truthy(args, "cwd");
		if (c !== undefined) {
			const s = toStr(c);
			if (s.length > 0) {
				if (s.includes("\0")) throw invalidValue(`The property 'options.cwd' must be a string without null bytes. Received ${quote(s)}`);
				cwd = userCwd = s;
			}
		}

		const ipc = truthy(args, "ipc");
		if (typeof ipc === "function") throw UNSUPPORTED("the ipc option");

		const sig = truthy(args, "signal");
		if (sig !== undefined) {
			if (!(sig instanceof AbortSignal)) throw invalidArgs(`The "signal" argument must be of type AbortSignal.${received(sig)}`);
			if (sig.aborted) {
				const e = new Error("The operation was aborted", { cause: sig.reason });
				e.name = "AbortError";
				e.code = "ABORT_ERR";
				throw e;
			}
			signal = sig;
		}
		const od = truthy(args, "onDisconnect");
		if (od !== undefined && typeof od !== "function") throw invalidArgs("onDisconnect must be a function or undefined");
		const oe = truthy(args, "onExit");
		if (oe !== undefined) {
			if (typeof oe !== "function") throw invalidArgs("onExit must be a function or undefined");
			onExit = oe;
		}

		const env = truthy(args, "env");
		if (env !== undefined) {
			if (!isObject(env)) throw invalidArgs("env must be an object");
			// An explicit env replaces the inherited one, and its PATH (or none: then
			// the platform default) is the one argv[0] is looked up in.
			envPairs = [];
			path = "";
			for (const key of Object.keys(env)) {
				const value = env[key];
				if (value === undefined) continue;
				const v = toStr(value);
				if (key.includes("\0")) throw invalidValue(`The property 'options.env['${key}']' must be a string without null bytes. Received "${key}"`);
				if (v.includes("\0")) throw invalidValue(`The property 'options.env['${key}']' must be a string without null bytes. Received "${v}"`);
				const line = `${key}=${v}`;
				const eq = line.indexOf("=");
				if (line.slice(0, eq) === "PATH") path = line.slice(eq + 1);
				envPairs.push(line);
			}
		}

		argv = getArgv(cmdValue, path, cwd, argv0);

		const sv = args.stdio;
		if (sv !== undefined) {
			if (sv !== null) {
				if (!Array.isArray(sv)) throw invalidArgs("stdio must be an array");
				const it = arrayLike(sv);
				let i = 0;
				for (; i < it.len && i < 3; i++) stdio[i] = extractStdio(i, it.at(i), stdio[i]);
				for (; i < it.len; i++) extra.push(extractStdio(i, it.at(i), IGNORE));
			}
		} else {
			if (args.stdin !== undefined) stdio[0] = extractStdio(0, args.stdin, stdio[0]);
			if (args.stderr !== undefined) stdio[2] = extractStdio(2, args.stderr, stdio[2]);
			if (args.stdout !== undefined) stdio[1] = extractStdio(1, args.stdout, stdio[1]);
		}

		if (typeof args.lazy === "boolean") lazy = args.lazy;
		if (typeof args.detached === "boolean") detached = args.detached;
		if (args.uid !== undefined && args.uid !== null) uid = integerInRange(args.uid, "uid", -(2 ** 31), 2 ** 31 - 1, 0);
		if (args.gid !== undefined && args.gid !== null) gid = integerInRange(args.gid, "gid", -(2 ** 31), 2 ** 31 - 1, 0);
		// Bun honours cgroup only where cgroups exist (Linux), ignoring it elsewhere
		// like windowsHide on POSIX. Node cannot start a child inside a cgroup.
		if (args.cgroup !== undefined && args.cgroup !== null && IS_LINUX) throw UNSUPPORTED("the cgroup option");
		if (process.platform === "win32") {
			if (typeof args.windowsHide === "boolean") windowsHide = args.windowsHide;
			if (typeof args.windowsVerbatimArguments === "boolean") windowsVerbatimArguments = args.windowsVerbatimArguments;
		}

		const t = args.timeout;
		if (t !== undefined && t !== null && !(t === Infinity)) {
			if (typeof t === "number" && Number.isNaN(t)) throw outOfRange("timeout", "a non-negative integer", "NaN");
			const n = integerInRange(t, "timeout", 0, Number.MAX_SAFE_INTEGER, 0);
			// The u64 is narrowed as Bun does: (u32) & 0x7FFFFFFF.
			if (n > 0) timeout = Number(BigInt.asUintN(32, BigInt(n)) & 0x7fffffffn);
		}
		if (args.killSignal !== undefined) {
			killSignal = signalFromJS(args.killSignal);
			if (killSignal === 0) throw coded(TypeError, "ERR_UNKNOWN_SIGNAL", "Unknown signal: 0");
		}
		const mb = args.maxBuffer;
		if (typeof mb === "number" && Number.isFinite(mb)) {
			const v = Math.trunc(mb);
			const piped = stdio.some((s) => s.kind === "pipe" || s.kind === "buffer");
			if (v > 0 && piped) maxBuffer = v;
		}

		const term = truthy(args, "terminal");
		if (term !== undefined) {
			// spawn_maybe_sync: a Terminal instance is attached, any other object is
			// parsed as Terminal options and a PTY is opened for the child, anything
			// else is rejected. Compat has no Bun.Terminal and Node has no PTY, so
			// the object case is refused outright rather than run without a tty.
			if (!isObject(term)) throw invalidArgs("terminal must be a Terminal object or options object");
			throw UNSUPPORTED("the terminal option (Bun.Terminal / PTY)");
		}
	} else {
		argv = getArgv(cmdValue, path, cwd, argv0);
	}

	// Path stdio is opened in the child with O_CREAT|0664 (stdin read-only,
	// stdout/stderr write-only, extra slots read-write), never truncated. Opening
	// it here instead, the failure is reported in the shape Bun gives a failed
	// file action: the errno, syscall posix_spawn, path = argv[0].
	const opened = [];
	const hasPath = stdio.some((s) => s.kind === "path") || extra.some((s) => s.kind === "path");
	const fail = (e) => {
		for (const fd of opened) closeSync(fd);
		return systemError(e.code, e.errno, "posix_spawn", argv.args[0]);
	};
	if (hasPath && userCwd !== undefined) {
		const e = checkChdir(userCwd);
		if (e) throw fail(e);
	}
	const base = userCwd !== undefined ? resolvePath(userCwd) : process.cwd();
	const openPath = (s, flags) => {
		try { const fd = openSync(resolvePath(base, s.path), flags | FS.O_CREAT, 0o664); opened.push(fd); return fd; } catch (e) { throw fail(e); }
	};
	const nodeStdio = stdio.map((s, i) => {
		switch (s.kind) {
			case "inherit": return "inherit";
			case "ignore": return "ignore";
			case "pipe": case "buffer": return "pipe";
			case "fd": return s.fd;
			case "path": return openPath(s, i === 0 ? FS.O_RDONLY : FS.O_WRONLY);
		}
	});
	for (const s of extra) {
		switch (s.kind) {
			case "inherit": nodeStdio.push("inherit"); break;
			case "ignore": nodeStdio.push("ignore"); break;	// left closed in the child, as libuv does
			case "fd": nodeStdio.push(s.fd); break;
			case "path": nodeStdio.push(openPath(s, FS.O_RDWR)); break;
		}
	}

	const child = new ChildProcess();
	// Spawn failures are thrown synchronously below; Node's deferred 'error'
	// emission for the same failure is consumed here so it is not reported twice.
	child.on("error", noop);
	let err = 0;
	try {
		err = child.spawn({
			file: argv.file,
			args: argv.args,
			cwd: userCwd,
			envPairs: envPairs ?? [...STARTUP_ENV],
			stdio: nodeStdio,
			detached,
			windowsHide,
			windowsVerbatimArguments,
			uid,
			gid,
		}) ?? 0;
	} catch (e) {
		if (typeof e?.errno !== "number") throw e;
		err = e.errno;
	} finally {
		for (const fd of opened) closeSync(fd);
	}
	if (err) {
		for (const s of [child.stdin, child.stdout, child.stderr]) s?.destroy();
		throw systemError(UV_NAME(err), err, "posix_spawn", argv.args[0]);
	}

	// The value each getter returns (Readable/Writable to_js): a stream or sink for
	// pipes, the caller's fd number for fd targets, undefined otherwise.
	let sub;
	const pipes = {};
	const valueOut = (i, sock) => {
		const s = stdio[i];
		if (s.kind === "fd") return s.fd;
		if (s.kind !== "pipe") return undefined;
		// maxBuffer overflow ends the child with killSignal (Subprocess::on_max_buffer).
		const r = readableFor(sock, maxBuffer, () => sub[KILL_QUIETLY](killSignal), lazy);
		pipes[i === 1 ? "stdout" : "stderr"] = r;
		if (!lazy) r.start();
		return r.stream;
	};
	let stdinValue;
	if (stdio[0].kind === "pipe") { stdinValue = new FileSink(child.stdin); pipes.stdin = { sock: child.stdin }; }
	else if (stdio[0].kind === "fd") stdinValue = stdio[0].fd;
	else if (stdio[0].kind === "buffer") {
		const sock = child.stdin;
		pipes.stdin = { sock };
		// A child that exits without reading its input is not an error for Bun's
		// buffered stdin writer; the write simply ends.
		sock.on("error", noop);
		const data = stdio[0].data;
		if (data instanceof Blob) data.arrayBuffer().then((b) => sock.end(new Uint8Array(b)), (e) => sock.destroy(e));
		else sock.end(data);
	}
	const stdoutValue = valueOut(1, child.stdout);
	const stderrValue = valueOut(2, child.stderr);
	sub = new Subprocess(child, {
		stdin: stdinValue,
		stdout: stdoutValue,
		stderr: stderrValue,
		extra: extra.map((s) => (s.kind === "fd" ? s.fd : null)),
		pipes,
		killSignal,
		onExit,
		timeout,
		signal,
	});
	return sub;
}
