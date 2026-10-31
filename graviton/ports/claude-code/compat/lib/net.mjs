// Bun.serve, Bun.listen and Bun.connect (Bun 1.4.4) on Node's http / https / net.
//
// Bun sources consulted, tag bun-v1.4.4 (rev 38afee6c84fc):
//   src/runtime/server/{mod.rs,server_body.rs,ServerConfig.rs,RequestContext.rs,server.classes.ts}
//   src/runtime/socket/{Listener.rs,socket_body.rs,Handlers.rs,SocketConfig.bind.ts,sockets.classes.ts}
//   src/runtime/dns_jsc/{dns.rs,cares_jsc.rs}, src/cares_sys/c_ares.rs, src/sys/Error.rs
//   src/bun_core/fmt.rs (URLFormatter), src/js/node/http.ts (maxHeaderSize)
//   packages/bun-usockets/src/bsd.c (listen address choice)
//   packages/bun-uws/src/{HttpContext.h,HttpResponse.h,HttpResponseData.h} (idle timeouts)
// Behaviour was then pinned down against stock Bun with compat/test/net-scenarios.js.
//
// Only what Claude Code's bundle reaches is implemented; everything else either
// is absent or throws an explicit "not implemented by the compat layer" error,
// never a silent stand-in. See the end of this header for the absent members.
//
// Two places reach into documented-but-underscored Node internals, because Bun's
// contract cannot be met through the public API:
//   * Bun.serve / Bun.listen bind synchronously and throw synchronously (callers
//     read server.port on the next line and catch EADDRINUSE). Node's listen() is
//     asynchronous even for IP literals, so binding goes through
//     net.Server.prototype._listen2 (the hook cluster uses, kept "to avoid
//     breaking code that wraps this method"), and the bind error, which Node
//     only emits on the next tick, is taken from the public
//     "tracing:net.server.listen:error" diagnostics channel that Node publishes
//     synchronously from the same frame.
//   * Bun's socket.write() returns how many bytes the kernel accepted right now
//     and buffers nothing (the relay in the bundle keeps its own queue and waits
//     for drain). Node's write() buffers everything. So writes go straight to the
//     socket's fd with fs.writeSync; when the kernel is full, exactly one more
//     byte is handed to Node's writer, whose completion is the "writable again"
//     signal that becomes the drain callback. That byte is counted as written.
//
// Absent on purpose (the bundle never touches them):
//   Server: fetch(), upgrade(), publish(), subscriberCount(), reload(), timeout(),
//     closeIdleConnections(), address, id, pendingWebSockets, [Symbol.dispose];
//     options routes/static/websocket/unix/http2/http3/baseURI throw.
//   Listener: reload(), fd, unix, getsockname(), [Symbol.dispose].
//   Socket: flush(), shutdown(), close(), timeout(), setNoDelay(), setKeepAlive(),
//     reload(), upgradeTLS(), every TLS getter, bytesWritten, listener, fd,
//     [Symbol.dispose]; tls / unix / fd options throw.
import http from "node:http";
import https from "node:https";
import net from "node:net";
import os from "node:os";
import util from "node:util";
import dc from "node:diagnostics_channel";
import { writeSync } from "node:fs";
import { execFileSync } from "node:child_process";

// ---- errors shaped like Bun's --------------------------------------------------

// Bun's throw_invalid_arguments: a TypeError whose code is not an own enumerable
// key (Object.keys() of it is empty).
function invalidArgs(message) {
	const e = new TypeError(message);
	Object.defineProperty(e, "code", { value: "ERR_INVALID_ARG_TYPE", enumerable: false, writable: true, configurable: true });
	return e;
}
function outOfRange(message) {
	const e = new RangeError(message);
	Object.defineProperty(e, "code", { value: "ERR_OUT_OF_RANGE", enumerable: false, writable: true, configurable: true });
	return e;
}
function notImplemented(what) {
	return new Error(`${what} is not implemented by the Bun compat layer (compat/lib/net.mjs)`);
}
const nativeErrno = (code) => os.constants.errno[code];
// libuv's error number for a code: what Node puts in err.errno and what Bun
// reports for sys errors ("Node reports libuv's codes in err.errno").
function uvErrno(code, fallback) {
	if (typeof fallback === "number") return fallback;
	const n = nativeErrno(code);
	return typeof n === "number" ? (n > 0 ? -n : n) : undefined;
}
// bun_sys::Error::to_system_error: "<CODE>: <libuv label>, <syscall>".
function sysError(code, errno, syscall) {
	let label = "Unknown Error";
	try { if (typeof errno === "number") label = util.getSystemErrorMessage(errno); } catch {}
	const e = new Error(`${code}: ${label}, ${syscall}`);
	e.code = code;
	e.errno = errno;
	e.syscall = syscall;
	return e;
}

// ---- synchronous bind ------------------------------------------------------------

const listenErrors = new WeakMap();
dc.subscribe("tracing:net.server.listen:error", (msg) => { if (msg && msg.server) listenErrors.set(msg.server, msg.error); });
// libuv's uv_tcp_flags, the `flags` argument of _listen2 (TCPConstants is internal).
const UV_TCP_IPV6ONLY = 1, UV_TCP_REUSEPORT = 2;

// Node emits every bind failure a second time on the next tick; the copy we
// already threw synchronously must not become an uncaught 'error'. Accept-time
// errors (EMFILE...) also land here: uSockets drops those and keeps listening,
// and so does this.
function guardServerErrors(server) {
	server.on("error", () => {});
}

function bindOnce(server, ip, port, flags) {
	server._listen2(ip, port, net.isIP(ip), 511, undefined, flags);
	if (server._handle) return null;
	const err = listenErrors.get(server);
	listenErrors.delete(server);
	if (!err) throw new Error("compat net: bind failed but Node did not publish the error on net.server.listen");
	return err;
}

// Haiku's IPv6 sockets are always IPv6-only: getsockopt(IPV6_V6ONLY) is
// hard-wired to 1 and setsockopt(IPV6_V6ONLY, 0) fails with EINVAL
// (src/add-ons/kernel/network/protocols/ipv6/ipv6.cpp, ipv6_getsockopt /
// ipv6_setsockopt). libuv's bind always sets the option, to 0 unless asked for
// UV_TCP_IPV6ONLY, so on Haiku every IPv6 bind fails with EINVAL. uSockets
// ignores that setsockopt's result, so Bun would bind. Asking for v6-only there
// is what the socket is anyway, so it changes nothing but the failed call.
const IPV6_ALWAYS_ONLY = process.platform === "haiku";

// Bind `server` to `ip`:`port` now. SO_REUSEPORT is asked for like uSockets does:
// where the platform has no load-balancing SO_REUSEPORT, libuv answers ENOTSUP,
// and uSockets (which only sets it #ifdef SO_REUSEPORT) binds without it.
function bindSync(server, ip, port, { reusePort = false, ipv6Only = false } = {}) {
	let flags = 0;
	if ((ipv6Only || IPV6_ALWAYS_ONLY) && net.isIP(ip) === 6) flags |= UV_TCP_IPV6ONLY;
	if (reusePort) flags |= UV_TCP_REUSEPORT;
	let err = bindOnce(server, ip, port, flags);
	if (err && reusePort && err.code === "ENOTSUP") err = bindOnce(server, ip, port, flags & ~UV_TCP_REUSEPORT);
	return err;
}

// Names (not IP literals) are resolved synchronously, as uSockets' getaddrinfo
// does, by asking a child Node for dns.lookup's answer (same libc call, same
// order). Only operator-configured names reach this; every bundle call site
// passes an IP literal.
function resolveSync(hostname) {
	const env = { ...process.env };
	delete env.NODE_OPTIONS;
	try {
		const out = execFileSync(process.execPath, ["-e", "require('dns').lookup(process.argv[1],{all:true},(e,a)=>{process.stdout.write(JSON.stringify(e?{code:e.code}:a))})", hostname], { env, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"], timeout: 30000 });
		const r = JSON.parse(out);
		return Array.isArray(r) ? r : [];
	} catch { return []; }
}

// uSockets' bsd_create_listen_socket: every IPv6 candidate first, then IPv4.
function listenCandidates(hostname) {
	if (net.isIP(hostname)) return [hostname];
	const all = resolveSync(hostname);
	return [...all.filter((a) => a.family === 6), ...all.filter((a) => a.family === 4)].map((a) => a.address);
}

function bindHost(server, hostname, port, opts) {
	const cands = listenCandidates(hostname);
	let err = null;
	for (const ip of cands) {
		err = bindSync(server, ip, port, opts);
		if (!err) return null;
	}
	return err ?? { code: undefined, noAddress: true };
}

// ---- the 4-second idle ticker (uSockets timeouts) ---------------------------------
// uSockets checks socket timeouts once per 4 s tick; us_socket_timeout(s, n) arms
// the socket to expire on tick now + ((n + 3) >> 2). Bun.serve's idleTimeout is in
// those units, so a 1 s timeout fires 0-4 s later and 10 s fires 8-12 s later.
// The sweep timer runs while some HTTP context exists and restarts when the first
// one is created after none (uSockets links/unlinks it with the contexts), which
// fixes the phase of the ticks relative to the server's creation.
let tick = 0, ticker = null, contexts = 0;
const armed = new Set();
function contextUp() {
	if (contexts++ === 0) {
		ticker = setInterval(() => {
			tick++;
			for (const e of armed) if (e.deadline === tick) { armed.delete(e); e.deadline = null; e.onTimeout(); }
		}, 4000);
		ticker.unref();
	}
}
function contextDown() {
	if (--contexts === 0) { clearInterval(ticker); ticker = null; }
}
function arm(entry, seconds) {
	if (!seconds) { entry.deadline = null; armed.delete(entry); return; }
	entry.deadline = tick + ((seconds + 3) >> 2);
	armed.add(entry);
}
function disarm(entry) { entry.deadline = null; armed.delete(entry); }

// ---- node:http maxHeaderSize ------------------------------------------------------
// In Bun, `http.maxHeaderSize = n` sets the process-wide request header limit that
// Bun.serve's parser uses, for servers that already exist too. Node's property is
// getter-only, so the bundle's assignment (`Hbe.maxHeaderSize=262144` next to the
// gateway's Bun.serve) throws in strict-mode ESM. installHttpMaxHeaderSize makes it
// a real accessor feeding the servers created here.
let maxHeaderSize = http.maxHeaderSize;
const liveHttpServers = new Set();
export function installHttpMaxHeaderSize(httpModule = http) {
	Object.defineProperty(httpModule, "maxHeaderSize", {
		configurable: true, enumerable: true,
		get() { return maxHeaderSize; },
		set(v) {
			if (!Number.isInteger(v) || v < 0) throw invalidArgs(`The "maxHeaderSize" argument must be a non-negative integer. Received ${util.inspect(v)}`);
			maxHeaderSize = v;
			// Node reads server.maxHeaderSize for each new connection.
			for (const s of liveHttpServers) s.maxHeaderSize = v;
		},
	});
}

// ---- Bun.serve ---------------------------------------------------------------------

const WELCOME = "Welcome to Bun! To get started, return a Response object.";
const PROD_ERROR = "Something went wrong!";
const BODYLESS_METHODS = new Set(["GET", "HEAD"]);
const isNoBodyStatus = (s) => s === 204 || s === 304 || (s >= 100 && s < 200);
const requestInfo = new WeakMap();
const connOf = new WeakMap();

// A Request whose url is the bare request target: Bun hands the fetch handler
// "/p?q=1" for an HTTP/1.0 request without Host, which Node's Request cannot hold.
class RelativeRequest extends Request {
	#target;
	constructor(target, init) { super("http://localhost" + target, init); this.#target = target; }
	get url() { return this.#target; }
}

function formatUrl(ssl, hostname, port) {
	let host = hostname ?? "localhost";
	if (hostname && hostname[0] !== "[" && net.isIP(hostname) === 6) host = `[${hostname}]`;
	const optional = (ssl && port === 443) || (!ssl && port === 80);
	return `${ssl ? "https" : "http"}://${host}${optional ? "" : ":" + port}/`;
}

function rawReply(socket, statusLine) {
	// uWS answers these straight from the parser: status line, no headers, close.
	if (socket.destroyed) return;
	socket.end(`HTTP/1.1 ${statusLine}\r\n\r\n`);
	socket.destroySoon?.();
}

function escapeHtml(s) {
	return String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
}

function serveOptionError(arg) {
	if (arg === null || typeof arg !== "object") return invalidArgs("Bun.serve expects an object");
	return null;
}

export function serve(options) {
	const bad = serveOptionError(options);
	if (bad) throw bad;
	return new BunServer(options);
}

class BunServer {
	#node; #ssl; #dev; #fetch; #onError; #hostname; #configPort; #idle; #maxBody;
	#listening = false; #pending = 0; #stopping = false; #terminated = false;
	#unrefd = false; #urlCache = null; #closed = null; #conns = new Set(); #reqs = new Set(); #context = false;

	constructor(o) {
		const env = process.env;
		let dev = env.NODE_ENV !== "production";
		let reusePort = env.NODE_UNIQUE_ID !== undefined;
		if (o.development !== undefined) {
			dev = typeof o.development === "object" && o.development !== null ? true : !!o.development;
			reusePort = !dev;
		}
		for (const k of ["routes", "static", "websocket", "webSocket", "unix", "baseURI", "app"]) {
			if (o[k] !== undefined && o[k] !== null && o[k] !== false) throw notImplemented(`Bun.serve option "${k}"`);
		}
		if (o.http2 || o.http3) throw notImplemented("Bun.serve http2/http3");
		if (o.http1 === false) throw notImplemented("Bun.serve http1:false");

		let idle = 10;
		if (o.idleTimeout !== undefined && o.idleTimeout !== null) {
			if (typeof o.idleTimeout !== "number" || !Number.isInteger(o.idleTimeout)) throw invalidArgs("Bun.serve expects idleTimeout to be an integer");
			const v = Math.max(0, o.idleTimeout);
			if (v > 255) throw invalidArgs("Bun.serve expects idleTimeout to be 255 or less");
			idle = v;
		}

		let port = 3000;
		for (const k of ["BUN_PORT", "PORT", "NODE_PORT"]) {
			const v = env[k];
			if (v !== undefined && /^\d+$/.test(v) && +v <= 65535) { port = +v; break; }
		}
		if (o.port !== undefined && o.port !== null) {
			const n = Number(o.port);
			if (!Number.isFinite(n) || n % 1 !== 0) throw outOfRange(`The value of "options.port" is out of range. It must be an integer. Received ${util.inspect(o.port)}`);
			if (n < 0 || n > 65535) throw outOfRange(`The value of "options.port" is out of range. It must be >= 0 and <= 65535. Received ${util.inspect(o.port)}`);
			port = n;
		}

		const host = typeof o.hostname === "string" ? o.hostname : typeof o.host === "string" ? o.host : undefined;
		// Bun's default is every interface (uSockets binds "::" / 0.0.0.0). Every
		// bundle call site names its address, so an omitted one is refused rather
		// than silently opening the server to the network.
		if (!host) throw notImplemented("Bun.serve without an explicit hostname (Bun would bind every interface)");
		if (o.reusePort !== undefined) reusePort = !!o.reusePort;
		const ipv6Only = !!o.ipv6Only;

		if (o.maxRequestBodySize !== undefined && o.maxRequestBodySize !== null && o.maxRequestBodySize !== 0 && typeof o.maxRequestBodySize === "number") this.#maxBody = Math.max(0, Math.trunc(o.maxRequestBodySize));
		else this.#maxBody = 128 * 1024 * 1024;

		if (o.error !== undefined && o.error !== null && o.error !== false && o.error !== 0 && o.error !== "") {
			if (typeof o.error !== "function") throw invalidArgs("Expected error to be a function");
			this.#onError = o.error;
		}
		if (o.fetch !== undefined && o.fetch !== null && o.fetch !== false && o.fetch !== 0 && o.fetch !== "") {
			if (typeof o.fetch !== "function") throw invalidArgs("Expected fetch() to be a function");
			this.#fetch = o.fetch;
		} else {
			throw invalidArgs("Bun.serve() needs either:\n\n  - A routes object:\n     routes: {\n       \"/path\": {\n         GET: (req) => new Response(\"Hello\")\n       }\n     }\n\n  - Or a fetch handler:\n     fetch: (req) => {\n       return new Response(\"Hello\")\n     }\n\nLearn more at https://bun.com/docs/api/http");
		}

		const nodeOpts = {
			maxHeaderSize, requireHostHeader: false, requestTimeout: 0, headersTimeout: 0, keepAliveTimeout: 0,
		};
		const tls = o.tls;
		const tlsOn = tls && !(Array.isArray(tls) && tls.length === 0);
		if (tlsOn) {
			const t = Array.isArray(tls) ? tls[0] : tls;
			if (Array.isArray(tls) && tls.length > 1) throw notImplemented("Bun.serve tls SNI arrays");
			for (const k of Object.keys(t)) if (!["cert", "key", "ca", "passphrase", "requestCert", "rejectUnauthorized"].includes(k)) throw notImplemented(`Bun.serve tls option "${k}"`);
			for (const k of ["cert", "key", "ca"]) {
				const v = t[k];
				const ok = (x) => typeof x === "string" || Buffer.isBuffer(x) || x instanceof Uint8Array;
				if (v !== undefined && !(ok(v) || (Array.isArray(v) && v.every(ok)))) throw notImplemented(`Bun.serve tls.${k} of this type (only strings and buffers)`);
			}
			Object.assign(nodeOpts, { cert: t.cert, key: t.key, ca: t.ca, passphrase: t.passphrase, requestCert: !!t.requestCert, rejectUnauthorized: t.rejectUnauthorized ?? false });
		}
		this.#ssl = !!tlsOn;
		this.#dev = dev;
		this.#idle = idle;
		this.#hostname = host;
		this.#configPort = port;

		const node = this.#ssl ? https.createServer(nodeOpts) : http.createServer(nodeOpts);
		node.timeout = 0;
		guardServerErrors(node);
		node.on("request", (req, res) => this.#onRequest(req, res));
		node.on(this.#ssl ? "secureConnection" : "connection", (sock) => this.#onConnection(sock));
		// Node's own 400/431 answers for malformed requests already match uWS's
		// ("HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close").
		const err = bindHost(node, host, port, { reusePort, ipv6Only });
		if (err) {
			if (err.code && err.code !== "EADDRINUSE") {
				// Linux Bun reports the real errno for anything but EADDRINUSE/EACCES.
				if (err.code === "EACCES") {
					const e = new Error(`permission denied ${host}:${port}`);
					e.code = "EACCES"; e.errno = 0; e.syscall = "listen";
					throw e;
				}
				throw sysError(err.code, uvErrno(err.code, err.errno), "listen");
			}
			const e = new Error(`Failed to start server. Is port ${port} in use?`);
			e.code = "EADDRINUSE"; e.errno = 0; e.syscall = "listen";
			throw e;
		}
		this.#node = node;
		this.#listening = true;
		liveHttpServers.add(node);
		contextUp();
		this.#context = true;
	}

	get port() { return this.#listening ? this.#node.address().port : this.#configPort; }
	get hostname() { return this.#hostname; }
	// Bun caches url on first read (server.classes.ts: cache: true).
	get url() { return this.#urlCache ??= new URL(formatUrl(this.#ssl, this.#hostname, this.port)); }
	get protocol() { return this.#ssl ? "https" : "http"; }
	get development() { return this.#dev; }
	get pendingRequests() { return this.#pending; }

	requestIP(request) {
		if (arguments.length < 1) throw invalidArgs("Missing Request object");
		if (!(request instanceof Request)) throw invalidArgs("Expected Request object");
		// Like Bun, the answer comes from the request's own connection.
		return requestInfo.get(request) ?? null;
	}

	ref() {
		if (this.#unrefd && !this.#isClosed()) {
			this.#unrefd = false;
			this.#node.ref();
			for (const c of this.#conns) c.sock.ref();
		}
		return this;
	}
	// In Bun one poll ref covers the listener and every connection, so an unref'd
	// server lets the process exit with keep-alive connections still open.
	unref() {
		this.#unrefd = true;
		this.#node.unref();
		for (const c of this.#conns) c.sock.unref();
		return this;
	}

	stop(abrupt) {
		const force = abrupt === true;
		const p = this.#closedPromise();
		if (this.#listening || (force && !this.#terminated)) this.#stop(force);
		return p;
	}

	#isClosed() { return !this.#listening && this.#pending === 0 && this.#conns.size === 0; }
	#closedPromise() {
		if (this.#isClosed()) return Promise.resolve(undefined);
		if (!this.#closed) {
			let resolve;
			const promise = new Promise((r) => { resolve = r; });
			this.#closed = { promise, resolve };
		}
		return this.#closed.promise;
	}
	#maybeClosed() {
		if (this.#closed && this.#isClosed()) {
			const c = this.#closed;
			this.#closed = null;
			// Bun resolves it from a scheduled task, after the closing work.
			setImmediate(() => c.resolve(undefined));
		}
		if (this.#isClosed() && this.#node) {
			liveHttpServers.delete(this.#node);
			if (this.#context) { this.#context = false; contextDown(); }
		}
	}
	#stop(force) {
		if (this.#listening) {
			this.#listening = false;
			this.#stopping = true;
			this.#node.close();
		}
		if (force) {
			this.#terminated = true;
			// Every in-flight request is aborted before stop() returns.
			for (const r of [...this.#reqs]) r.abort();
			for (const c of [...this.#conns]) c.sock.destroy();
		} else {
			// Idle keep-alive connections close now; busy ones after their response.
			for (const c of [...this.#conns]) if (c.active === 0) c.sock.destroy();
		}
		this.#maybeClosed();
	}

	#onConnection(sock) {
		if (connOf.has(sock)) return;
		const conn = { sock, active: 0, idleSec: 10, deadline: null, onTimeout: () => sock.destroy() };
		connOf.set(sock, conn);
		this.#conns.add(conn);
		if (this.#unrefd) sock.unref();
		// uWS: "Any connected socket should timeout until it has a request" (10 s).
		arm(conn, conn.idleSec);
		sock.on("close", () => {
			disarm(conn);
			this.#conns.delete(conn);
			this.#maybeClosed();
		});
	}

	#onRequest(req, res) {
		const sock = req.socket;
		if (!connOf.has(sock)) this.#onConnection(sock);
		const conn = connOf.get(sock);
		// uWS refuses an HTTP/1.1 request without Host before any handler runs.
		if (req.httpVersion === "1.1" && req.headers.host === undefined) {
			sock.end("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
			return;
		}
		const method = req.method;
		const te = req.headers["transfer-encoding"];
		const cl = req.headers["content-length"];
		const mayHaveBody = !BODYLESS_METHODS.has(method);
		if (mayHaveBody && cl !== undefined && Number(cl) > this.#maxBody) {
			rawReply(sock, "413 Request Entity Too Large");
			return;
		}

		this.#pending++;
		conn.active++;
		conn.idleSec = this.#idle;
		arm(conn, conn.idleSec);
		const ac = new AbortController();
		const ctx = {
			req, res, conn, method, ended: false, calledError: false, headSent: false,
			abort: () => { if (!ctx.ended) { ctx.ended = true; ac.abort(); } },
		};
		this.#reqs.add(ctx);
		let finished = false;
		const done = () => {
			if (finished) return;
			finished = true;
			this.#pending--;
			conn.active--;
			this.#reqs.delete(ctx);
			this.#maybeClosed();
		};
		res.on("close", () => {
			// The response did not complete: the client went away or stop(true) ran.
			if (!res.writableFinished) ctx.abort();
			done();
		});
		res.on("finish", () => {
			ctx.ended = true;
			if (sock.destroyed) return;
			arm(conn, conn.idleSec);
			// After a graceful stop, a busy connection closes once its work is done.
			if (this.#stopping && conn.active <= 1) sock.destroySoon();
		});

		// The request body, as Bun exposes it: null without one, otherwise a stream
		// that fails with Bun's error when it outgrows maxRequestBodySize.
		let body = null;
		if (mayHaveBody && (te !== undefined || (cl !== undefined && Number(cl) > 0))) {
			let total = 0, perTimeout = 0;
			const max = this.#maxBody;
			body = new ReadableStream({
				start: (c) => {
					req.on("data", (chunk) => {
						total += chunk.length;
						if (total > max) {
							req.removeAllListeners("data");
							req.resume();
							try { c.error(new Error("Request body exceeded maxRequestBodySize")); } catch {}
							if (!res.headersSent && !ctx.ended) {
								ctx.ended = true;
								rawReply(sock, "413 Payload Too Large");
							}
							return;
						}
						// uWS re-arms the idle timeout only at 16 KiB/s of body throughput.
						perTimeout += chunk.length;
						if (perTimeout >= 16384 * conn.idleSec) { perTimeout = 0; arm(conn, conn.idleSec); }
						c.enqueue(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.length));
						if (c.desiredSize !== null && c.desiredSize <= 0) req.pause();
					});
					req.on("end", () => {
						// uWS: the last body chunk disarms the idle timeout.
						disarm(conn);
						try { c.close(); } catch {}
					});
					req.on("aborted", () => { try { c.error(new DOMException("The operation was aborted.", "AbortError")); } catch {} });
				},
				pull: () => { req.resume(); },
				cancel: () => { req.removeAllListeners("data"); req.resume(); },
			}, { highWaterMark: 65536, size: (ch) => ch.byteLength });
		}

		const host = req.headers.host;
		const headers = new Headers();
		const raw = req.rawHeaders;
		for (let i = 0; i < raw.length; i += 2) headers.append(raw[i], raw[i + 1]);
		const init = { method, headers, signal: ac.signal };
		if (body) { init.body = body; init.duplex = "half"; }
		let request;
		try {
			request = host !== undefined
				? new Request(`${this.#ssl ? "https" : "http"}://${host}${req.url}`, init)
				: new RelativeRequest(req.url, init);
		} catch {
			// A request Node parsed but WHATWG Request rejects (e.g. CONNECT, a
			// malformed Host); uWS would hand it to fetch, Node's Request cannot.
			ctx.ended = true;
			sock.end("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
			return;
		}
		if (sock.remoteAddress) requestInfo.set(request, { address: sock.remoteAddress, family: sock.remoteFamily, port: sock.remotePort });
		ctx.request = request;

		let result;
		try {
			result = this.#fetch.call(this, request, this);
		} catch (e) {
			this.#handleError(e, ctx);
			return;
		}
		if (result !== null && typeof result === "object" && typeof result.then === "function") {
			result.then((v) => this.#render(v, ctx), (e) => this.#handleError(e, ctx));
		} else {
			this.#render(result, ctx);
		}
	}

	#render(value, ctx) {
		if (ctx.ended || ctx.res.destroyed) { discardBody(value); return; }
		if (!(value instanceof Response)) {
			let shown;
			try { shown = util.inspect(value); } catch { shown = null; }
			process.stderr.write(shown === null ? "error: Expected a Response object\n" : `error: Expected a Response object, but received '${shown}'\n`);
			this.#renderMissing(ctx);
			return;
		}
		if (!(value.status >= 100 && value.status <= 999)) {
			this.#handleError(new Error(`Cannot send a Response with status ${value.status}. HTTP status codes must be between 100 and 999 (Response.error() returns status 0).`), ctx);
			return;
		}
		writeResponse(ctx, value, this.#stopping).catch((e) => {
			// Nothing was sent yet (e.g. Node refused a header): answer like a throw.
			if (!ctx.res.headersSent) this.#finishError(e, ctx);
			else ctx.res.destroy();
		});
	}

	#renderMissing(ctx) {
		if (!this.#dev) { writeFixed(ctx, 204, [], null, this.#stopping); return; }
		writeFixed(ctx, 200, ["content-type", "text/plain;charset=utf-8"], Buffer.from(WELCOME), this.#stopping);
	}

	#handleError(err, ctx) {
		if (ctx.ended || ctx.res.destroyed || ctx.res.headersSent) return;
		if (this.#onError && !ctx.calledError) {
			ctx.calledError = true;
			let r;
			try { r = this.#onError.call(this, err); } catch (e) { r = e; }
			if (ctx.ended || ctx.res.destroyed) { discardBody(r); return; }
			if (r instanceof Error) return this.#finishError(r, ctx);
			if (r !== null && typeof r === "object" && typeof r.then === "function") {
				r.then((v) => {
					if (v instanceof Response && v.status >= 100 && v.status <= 999) this.#render(v, ctx);
					else this.#finishError(err, ctx);
				}, (e) => this.#finishError(e, ctx));
				return;
			}
			if (r instanceof Response && r.status >= 100 && r.status <= 999) return this.#render(r, ctx);
		}
		this.#finishError(err, ctx);
	}

	#finishError(err, ctx) {
		if (ctx.ended || ctx.res.destroyed || ctx.res.headersSent) return;
		// Bun reports the error on stderr in both modes; its own formatting (source
		// excerpt, colours) is not reproduced, Node's inspect is used instead.
		let path = ctx.req.url;
		try { path = new URL(ctx.req.url, "http://x").pathname; } catch {}
		process.stderr.write(`error: ${util.inspect(err)}\n`);
		if (this.#dev) {
			process.stderr.write(`${ctx.method} - ${path} failed\n`);
			const page = `<!DOCTYPE html><html><head><meta charset="utf-8"><title>${escapeHtml(ctx.method + " - " + path + " failed")}</title></head><body><h1>${escapeHtml(ctx.method + " - " + path + " failed")}</h1><pre>${escapeHtml(err instanceof Error ? err.stack ?? String(err) : util.inspect(err))}</pre></body></html>`;
			writeFixed(ctx, 500, ["content-type", "text/html;charset=utf-8"], Buffer.from(page), this.#stopping);
			return;
		}
		writeFixed(ctx, 500, ["content-type", "text/plain"], Buffer.from(PROD_ERROR), this.#stopping);
	}
}

function discardBody(v) {
	try { if (v instanceof Response && v.body && !v.bodyUsed) v.body.cancel().catch(() => {}); } catch {}
}

// Common header writing: no Connection / Keep-Alive header ever (uWS sends none;
// removeHeader("connection") makes Node omit it while still honouring
// keep-alive), no Date on a HEAD response that would have had a body (Bun writes
// those through end_without_body, which skips Date), and a stopping server
// closes the connection after the response.
function startHead(ctx, status, flat, stopping) {
	const { res, method } = ctx;
	res.removeHeader("connection");
	if (method === "HEAD" && !isNoBodyStatus(status)) res.sendDate = false;
	if (stopping) res.shouldKeepAlive = false;
	res.writeHead(status, flat);
	ctx.headSent = true;
}

function writeFixed(ctx, status, flat, buf, stopping) {
	const { res, method } = ctx;
	if (res.destroyed || res.headersSent) return;
	const h = [...flat];
	if (!isNoBodyStatus(status)) h.push("content-length", String(buf ? buf.length : 0));
	startHead(ctx, status, h, stopping);
	if (method === "HEAD" || isNoBodyStatus(status) || !buf) res.end();
	else res.end(buf);
	ctx.ended = true;
}

// Header order on the wire. Bun writes a Response's headers from WebKit's
// HTTPHeaderMap: every Set-Cookie first, then the "common" names (WebKit's
// HTTPHeaderNames.in) in insertion order, then all other names in insertion
// order; uWS appends Content-Length and Date. Node's Headers iterates sorted
// (as the Fetch spec says), so the insertion order is read from the object
// undici's Headers formats for util.inspect, captured by swapping
// util.formatWithOptions for the duration of that synchronous call.
const WEBKIT_COMMON_HEADERS = new Set([
	"accept", "accept-charset", "accept-language", "accept-encoding", "accept-ranges",
	"access-control-allow-credentials", "access-control-allow-headers", "access-control-allow-methods",
	"access-control-allow-origin", "access-control-expose-headers", "access-control-max-age",
	"access-control-request-headers", "access-control-request-method", "age", "authorization", "cache-control",
	"connection", "content-disposition", "content-encoding", "content-language", "content-length",
	"content-location", "content-security-policy", "content-security-policy-report-only", "content-type",
	"content-range", "cookie", "cookie2", "cross-origin-embedder-policy",
	"cross-origin-embedder-policy-report-only", "cross-origin-opener-policy",
	"cross-origin-opener-policy-report-only", "cross-origin-resource-policy", "date", "dnt", "default-style",
	"etag", "expect", "expires", "host", "if-match", "if-modified-since", "if-none-match", "if-range",
	"if-unmodified-since", "keep-alive", "last-event-id", "last-modified", "link", "location", "origin",
	"ping-from", "ping-to", "purpose", "pragma", "proxy-authorization", "proxy-connection", "range", "referer",
	"referrer-policy", "refresh", "report-to", "sec-fetch-dest", "sec-fetch-mode", "sec-websocket-accept",
	"sec-websocket-extensions", "sec-websocket-key", "sec-websocket-protocol", "sec-websocket-version",
	"server-timing", "service-worker", "service-worker-allowed", "service-worker-navigation-preload",
	"set-cookie", "set-cookie2", "sourcemap", "te", "timing-allow-origin", "trailer", "transfer-encoding",
	"upgrade", "upgrade-insecure-requests", "user-agent", "vary", "via", "x-content-type-options",
	"x-dns-prefetch-control", "x-frame-options", "x-sourcemap", "x-xss-protection", "x-temp-tablet",
	"icy-metaint", "icy-metadata"
]);
function insertionOrder(headers) {
	const custom = headers[util.inspect.custom];
	if (typeof custom !== "function") return null;
	const orig = util.formatWithOptions;
	let entries = null;
	util.formatWithOptions = (o, v) => { entries = v; return ""; };
	try { custom.call(headers, 0, {}, util.inspect); } catch { entries = null; } finally { util.formatWithOptions = orig; }
	if (!entries || typeof entries !== "object") return null;
	return Object.keys(entries).map((k) => k.toLowerCase());
}
// Verified once against a known Headers object; when this Node's undici does
// not expose insertion order this way, the sorted iteration order is used and
// the net diff test shows it.
export const headerOrderFromInsertion = (() => {
	const h = new Headers([["x-b", "1"], ["content-type", "t"], ["a-z", "2"]]);
	const o = insertionOrder(h);
	return !!o && o.join() === "x-b,content-type,a-z";
})();
function orderedHeaderPairs(headers) {
	const sorted = [];
	for (const [k, v] of headers) if (k !== "set-cookie") sorted.push([k, v]);
	const out = headers.getSetCookie().map((c) => ["set-cookie", c]);
	let names = headerOrderFromInsertion ? insertionOrder(headers) : null;
	if (!names || names.filter((n) => n !== "set-cookie").sort().join() !== sorted.map(([k]) => k).join()) names = sorted.map(([k]) => k);
	const value = new Map(sorted);
	for (const n of names) if (n !== "set-cookie" && WEBKIT_COMMON_HEADERS.has(n)) out.push([n, value.get(n)]);
	for (const n of names) if (n !== "set-cookie" && !WEBKIT_COMMON_HEADERS.has(n)) out.push([n, value.get(n)]);
	return out;
}

const CUT = Symbol("cut");
async function writeResponse(ctx, response, stopping) {
	const { res, method, conn } = ctx;
	const status = response.status;
	const pairs = orderedHeaderPairs(response.headers);
	const userCL = pairs.find(([k]) => k === "content-length")?.[1] ?? null;
	const flatWithout = (drop) => { const f = []; for (const [k, v] of pairs) if (!drop.includes(k)) f.push(k, v); return f; };
	let flat = flatWithout(["content-length", "transfer-encoding"]);
	const body = response.body;
	if (isNoBodyStatus(status) || body === null) {
		if (body) body.cancel().catch(() => {});
		if (!isNoBodyStatus(status)) flat.push("content-length", "0");
		startHead(ctx, status, flat, stopping);
		res.end();
		return;
	}
	// Bun sends Content-Length when the whole body is already there when the
	// response is rendered (strings, buffers, blobs, a stream that was closed with
	// its chunks queued, a small fetch() body already received) and chunked
	// encoding otherwise. "Already there" here: readable within two turns of the
	// event loop's check phase, the time a Node Blob stream needs to hand over
	// its bytes (a string or buffer body needs none, a closed stream one).
	const reader = body.getReader();
	const chunks = [];
	let size = 0, done = false, pendingRead = null;
	const imm = new Promise((r) => setImmediate(() => setImmediate(() => r(CUT))));
	for (;;) {
		const p = reader.read();
		const r = await Promise.race([p, imm]);
		if (r === CUT) { pendingRead = p; break; }
		if (r.done) { done = true; break; }
		const c = toU8(r.value);
		chunks.push(c);
		size += c.length;
	}
	if (ctx.ended || res.destroyed) { reader.cancel().catch(() => {}); return; }
	if (done) {
		flat.push("content-length", String(size));
		startHead(ctx, status, flat, stopping);
		if (method === "HEAD") res.end();
		else res.end(chunks.length === 1 ? chunks[0] : Buffer.concat(chunks));
		return;
	}
	// A Content-Length the handler set is kept, in its place, for a streamed body.
	if (userCL !== null) flat = flatWithout(["transfer-encoding"]);
	else flat.push("transfer-encoding", "chunked");
	startHead(ctx, status, flat, stopping);
	if (method === "HEAD") { reader.cancel().catch(() => {}); res.end(); return; }
	const closed = new Promise((r) => res.once("close", () => r(CUT)));
	const write = async (c) => {
		if (!res.write(c)) {
			// uWS hangs the idle timeout while the client is not reading.
			disarm(conn);
			const r = await Promise.race([new Promise((ok) => res.once("drain", ok)), closed]);
			if (r === CUT) return false;
		}
		arm(conn, conn.idleSec);
		return true;
	};
	try {
		for (const c of chunks) if (!(await write(c))) { reader.cancel().catch(() => {}); return; }
		let p = pendingRead;
		for (;;) {
			const r = await Promise.race([p, closed]);
			if (r === CUT) { reader.cancel().catch(() => {}); return; }
			if (r.done) break;
			if (!(await write(toU8(r.value)))) { reader.cancel().catch(() => {}); return; }
			p = reader.read();
		}
		res.end();
	} catch {
		// A body stream that errors mid-response: Bun closes the connection.
		res.destroy();
	}
}

function toU8(v) {
	if (v instanceof Uint8Array) return v;
	if (ArrayBuffer.isView(v)) return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
	if (v instanceof ArrayBuffer) return new Uint8Array(v);
	if (typeof v === "string") return Buffer.from(v);
	throw new TypeError("Response body stream chunks must be Uint8Array");
}

// ---- Bun.listen / Bun.connect -----------------------------------------------------

const HANDLER_NAMES = [["open", "onOpen"], ["close", "onClose"], ["data", "onData"], ["drain", "onWritable"], ["timeout", "onTimeout"], ["connectError", "onConnectError"], ["end", "onEnd"], ["error", "onError"], ["handshake", "onHandshake"]];
function parseHandlers(socket) {
	const h = {};
	for (const [k, name] of HANDLER_NAMES) {
		const v = socket[k];
		if (v === undefined || v === null) continue;
		if (typeof v !== "function") throw invalidArgs(`Expected "${name}" callback to be a function`);
		h[k] = v;
	}
	if (!h.data && !h.drain) throw invalidArgs('Expected at least "data" or "drain" callback');
	const bt = socket.binaryType ?? "buffer";
	if (!["buffer", "arraybuffer", "uint8array"].includes(bt)) throw invalidArgs(`binaryType must be "buffer", "arraybuffer" or "uint8array"`);
	h.binaryType = bt;
	return h;
}
function parseSocketOptions(opts, what) {
	if (opts === null || typeof opts !== "object") throw invalidArgs("Expected object");
	if (opts.socket === undefined || opts.socket === null) throw invalidArgs("SocketOptions.socket is required");
	if (typeof opts.socket !== "object") throw invalidArgs("SocketOptions.socket must be an object");
	if (opts.tls) throw notImplemented(`${what} with tls`);
	if (opts.unix) throw notImplemented(`${what} on a unix socket`);
	if (opts.fd !== undefined) throw notImplemented(`${what} on an fd`);
	const handlers = parseHandlers(opts.socket);
	const hostname = typeof opts.hostname === "string" && opts.hostname ? opts.hostname : typeof opts.host === "string" && opts.host ? opts.host : null;
	const port = opts.port === undefined || opts.port === null ? null : Number(opts.port);
	if (port === null) throw notImplemented(`${what} without a port (unix path form)`);
	if (!Number.isInteger(port) || port < 0 || port > 65535) throw outOfRange(`The value of "port" is out of range. It must be >= 0 and <= 65535. Received ${util.inspect(opts.port)}`);
	if (!hostname) throw notImplemented(`${what} without an explicit hostname`);
	return { handlers, hostname, port, allowHalfOpen: !!opts.allowHalfOpen };
}

// Route an exception from a socket handler like Bun's call_error_handler: to the
// error handler, or with none, to the process' uncaught-exception path.
function reportHandlerError(sock, err) {
	const onError = sock._handlers.error;
	if (!onError) { process.nextTick(() => { throw err; }); return; }
	try { onError.call(sock, sock, err); } catch (e) { process.nextTick(() => { throw e; }); }
}
function callHandler(sock, name, ...args) {
	const f = sock._handlers[name];
	if (!f) return;
	try { f.call(sock, sock, ...args); } catch (e) { reportHandlerError(sock, e); }
}

function toWriteBytes(data, a, b) {
	let buf;
	if (typeof data === "string") {
		const enc = typeof a === "string" ? a : typeof b === "string" ? b : "utf8";
		return Buffer.from(data, enc);
	}
	if (Buffer.isBuffer(data)) buf = data;
	else if (ArrayBuffer.isView(data)) buf = Buffer.from(data.buffer, data.byteOffset, data.byteLength);
	else if (data instanceof ArrayBuffer || data instanceof SharedArrayBuffer) buf = Buffer.from(data);
	else if (typeof Blob !== "undefined" && data instanceof Blob) throw notImplemented("socket.write(Blob)");
	else throw invalidArgs(`The "data" argument must be of type string, buffer, or blob. Received ${util.inspect(data)}`);
	const off = typeof a === "number" ? a : 0;
	const len = typeof b === "number" ? b : buf.length - off;
	if (off < 0 || off > buf.length) throw outOfRange(`The value of "byteOffset" is out of range. It must be >= 0 and <= ${buf.length}. Received ${off}`);
	if (len < 0 || len > buf.length - off) throw outOfRange(`The value of "byteLength" is out of range. It must be >= 0 and <= ${buf.length - off}. Received ${len}`);
	return buf.subarray(off, off + len);
}

const RAW_WRITE_RETRY = new Set(["EAGAIN", "EWOULDBLOCK"]);

class BunSocket {
	#s; #detached = false; #closeFired = false; #pendingNode = 0; #endAfterFlush = false;
	#readErr = null; #cache = Object.create(null); #listener;
	data = undefined;

	constructor(s, handlers, listener, wire = true) {
		this.#s = s;
		this.#listener = listener;
		Object.defineProperty(this, "_handlers", { value: handlers, writable: true, enumerable: false });
		if (!wire) { this.#detached = true; this.#closeFired = true; return; }
		s.on("data", (d) => {
			if (this.#detached) return;
			const bt = this._handlers.binaryType;
			const v = bt === "buffer" ? d : bt === "uint8array" ? new Uint8Array(d.buffer, d.byteOffset, d.length) : d.buffer.slice(d.byteOffset, d.byteOffset + d.length);
			callHandler(this, "data", v);
		});
		s.on("end", () => {
			if (this.#detached) return;
			callHandler(this, "end");
			// allowHalfOpen:false (the default): a FIN from the peer ends the socket,
			// whether or not an end handler ran.
			if (!this._allowHalfOpen && !this.#detached) this.#closeNow(false);
		});
		s.on("error", (e) => {
			if (this.#detached) return;
			if (e.syscall === "write") {
				// A failed write of the byte held by Node: Bun's on_writable delivers
				// the fatal send error to the error handler, then closes.
				reportHandlerError(this, sysError(e.code, uvErrno(e.code, e.errno), "write"));
				return;
			}
			this.#readErr = e;
		});
		s.on("close", () => {
			if (this.#closeFired) return;
			this.#detached = true;
			const e = this.#readErr;
			this.#fireClose(e ? sysError(e.code, uvErrno(e.code, e.errno), e.syscall || "read") : undefined);
		});
	}

	_setAllowHalfOpen(v) { Object.defineProperty(this, "_allowHalfOpen", { value: v, writable: true, enumerable: false }); }

	#fireClose(err) {
		if (this.#closeFired) return;
		this.#closeFired = true;
		this.#listener?._forget(this);
		callHandler(this, "close", err);
	}
	// uSockets close: the socket detaches and its close handler runs before the
	// call that closed it returns. A clean close still delivers what the kernel
	// holds; terminate() resets.
	#closeNow(reset) {
		if (this.#detached) return;
		this.#detached = true;
		if (reset) this.#s.resetAndDestroy();
		else this.#s.destroy();
		this.#fireClose(undefined);
	}

	get readyState() {
		if (this.#detached) return -1;
		if (this.#s.writableEnded) return -2;
		return 1;
	}
	#cached(k, f) {
		if (k in this.#cache) return this.#cache[k];
		if (this.#detached) return undefined;
		return (this.#cache[k] = f());
	}
	get remoteAddress() { return this.#cached("remoteAddress", () => this.#s.remoteAddress); }
	get remoteFamily() { return this.#cached("remoteFamily", () => this.#s.remoteFamily); }
	get localAddress() { return this.#cached("localAddress", () => this.#s.localAddress); }
	get localFamily() { return this.#cached("localFamily", () => this.#s.localAddress && (net.isIP(this.#s.localAddress) === 6 ? "IPv6" : "IPv4")); }
	get localPort() { return this.#cached("localPort", () => this.#s.localPort); }
	get remotePort() { return this.#detached ? undefined : this.#s.remotePort; }

	write(data, a, b) {
		if (this.#detached) return -1;
		if (data === undefined) return 0;
		const bytes = toWriteBytes(data, a, b);
		if (this.#s.writableEnded) return -1;
		if (bytes.length === 0) return 0;
		// The kernel was full at the last write and has not drained yet.
		if (this.#pendingNode > 0) return 0;
		const fd = this.#s._handle?.fd;
		if (typeof fd !== "number" || fd < 0) throw new Error("compat net: socket has no file descriptor to write to");
		let n;
		try {
			n = writeSync(fd, bytes, 0, bytes.length);
		} catch (e) {
			if (RAW_WRITE_RETRY.has(e.code)) n = 0;
			// A fatal send (peer gone): Bun returns the negative errno, clamped to -1.
			else return -1;
		}
		if (n < bytes.length) {
			this.#pendingNode++;
			this.#s.write(bytes.subarray(n, n + 1), (err) => {
				this.#pendingNode--;
				if (err || this.#detached || this.#pendingNode > 0) return;
				if (this.#endAfterFlush) { this.#endAfterFlush = false; this.#closeNow(false); return; }
				callHandler(this, "drain");
			});
			n += 1;
		}
		return n;
	}

	end(data, a, b) {
		if (this.#detached) return -1;
		let n = 0;
		if (data !== undefined) {
			n = this.write(data, a, b);
			if (n < 0) return -1;
		}
		if (this.#pendingNode > 0) this.#endAfterFlush = true;
		else this.#closeNow(false);
		return n;
	}

	terminate() { this.#closeNow(true); }

	pause() { if (!this.#detached) this.#s.pause(); }
	resume() { if (!this.#detached) this.#s.resume(); }
	ref() { if (!this.#detached) this.#s.ref(); }
	unref() { if (!this.#detached) this.#s.unref(); }
	_forceClose() { this.#closeNow(false); }
}

class BunListener {
	#node; #hostname; #port; #handlers; #sockets = new Set(); #halfOpen;
	data;
	constructor(node, hostname, port, handlers, data, halfOpen) {
		this.#node = node; this.#hostname = hostname; this.#port = port; this.#handlers = handlers; this.data = data; this.#halfOpen = halfOpen;
		node.on("connection", (s) => this.#accept(s));
	}
	#accept(s) {
		const sock = new BunSocket(s, this.#handlers, this);
		sock._setAllowHalfOpen(this.#halfOpen);
		this.#sockets.add(sock);
		callHandler(sock, "open");
	}
	_forget(sock) { this.#sockets.delete(sock); }
	get port() { return this.#port; }
	get hostname() { return this.#hostname; }
	stop(force) {
		if (this.#node.listening) this.#node.close();
		if (force === true) for (const s of [...this.#sockets]) s._forceClose();
	}
	// The listener's ref covers the listening socket only: an accepted socket
	// keeps the process alive on its own (Listener.rs: "accepted sockets each
	// have their own" poll ref), unlike Bun.serve's connections.
	ref() { this.#node.ref(); }
	unref() { this.#node.unref(); }
}

export function listen(opts) {
	const { handlers, hostname, port, allowHalfOpen } = parseSocketOptions(opts, "Bun.listen");
	const node = net.createServer({ allowHalfOpen: true, pauseOnConnect: !!opts.pauseOnConnect });
	guardServerErrors(node);
	const err = bindHost(node, hostname, port, { reusePort: !!opts.reusePort, ipv6Only: !!opts.ipv6Only });
	if (err) {
		// Listener.rs: "Failed to listen at <host>" plus syscall/errno/address/port/code.
		const e = new Error(`Failed to listen at ${hostname}`);
		if (err.code) {
			let errno = nativeErrno(err.code);
			if (err.code === "ENAMETOOLONG") errno = nativeErrno("EINVAL");
			e.syscall = "listen";
			e.errno = errno;
			e.address = hostname;
			e.port = port;
			e.code = err.code === "ENAMETOOLONG" ? "EINVAL" : err.code;
		}
		throw e;
	}
	return new BunListener(node, hostname, node.address().port, handlers, opts.data, allowHalfOpen);
}

// Bun.connect's failures (socket_body.rs connect_failed): connect errors keep their
// code only for a short allowlist, everything else is ECONNREFUSED; resolver
// failures become c-ares-style codes ("getaddrinfo ENOTFOUND host").
const CONNECT_KEEP = new Set(["ENOENT", "ENOTSOCK", "EACCES", "EINVAL", "ECONNRESET", "EADDRINUSE", "EADDRNOTAVAIL", "ECONNABORTED", "ECANCELED"]);
const EAI_TO_ARES = { ENOTFOUND: ["ENOTFOUND", 4], EAI_NONAME: ["ENOTFOUND", 4], EAI_NODATA: ["ENOTFOUND", 4], EAI_AGAIN: ["EAI_AGAIN", -3001], EAI_FAIL: ["EBADRESP", 10], EAI_FAMILY: ["EBADFAMILY", 9], EAI_ADDRFAMILY: ["EBADFAMILY", 9], EAI_MEMORY: ["ENOMEM", 15], EAI_SERVICE: ["ESERVICE", 25], EAI_SYSTEM: ["ESERVFAIL", 3], EAI_BADFLAGS: ["EBADFLAGS", 18], EAI_SOCKTYPE: ["ECONNREFUSED", 11], EAI_CANCELED: ["ECANCELLED", 24] };
function connectError(e, hostname) {
	const first = e && e.code ? e : e && Array.isArray(e.errors) && e.errors[0] ? e.errors[0] : e;
	const code = first && first.code;
	if (first && first.syscall === "getaddrinfo") {
		const [c, errno] = EAI_TO_ARES[code] ?? ["ENOTIMP", 5];
		const err = new Error(`getaddrinfo ${c} ${hostname}`);
		err.code = c; err.errno = errno; err.syscall = "getaddrinfo"; err.hostname = hostname;
		return err;
	}
	const c = CONNECT_KEEP.has(code) ? code : "ECONNREFUSED";
	const err = new Error("Failed to connect");
	err.code = c;
	// libuv's number for the code (-111 on Linux). Not -errno: Haiku's errno
	// constants are already negative, and negating them gave a positive errno.
	err.errno = uvErrno(c);
	err.syscall = "connect";
	return err;
}

// Bun's resolver answers localhost names itself with [::1, 127.0.0.1] (Chrome's
// ServeLocalhost) instead of asking getaddrinfo; everything else goes to the system.
function isLocalhostName(h) {
	const s = h.replace(/\.$/, "").toLowerCase();
	return s === "localhost" || s.endsWith(".localhost");
}
function localhostLookup(hostname, options, cb) {
	const all = [{ address: "::1", family: 6 }, { address: "127.0.0.1", family: 4 }];
	const fam = typeof options === "number" ? options : options?.family;
	const list = fam === 4 || fam === 6 ? all.filter((a) => a.family === fam) : all;
	if (options && options.all) cb(null, list);
	else cb(null, list[0].address, list[0].family);
}

export function connect(opts) {
	const { handlers, hostname, port, allowHalfOpen } = parseSocketOptions(opts, "Bun.connect");
	const host = hostname.startsWith("[") && hostname.endsWith("]") ? hostname.slice(1, -1) : hostname;
	let settle;
	const promise = new Promise((resolve, reject) => { settle = { resolve, reject }; });
	const s = net.connect({
		host, port, allowHalfOpen: true, autoSelectFamily: true,
		...(isLocalhostName(host) && !net.isIP(host) ? { lookup: localhostLookup } : {}),
	});
	let opened = false;
	const onConnectFail = (e) => {
		if (opened) return;
		const err = connectError(e, hostname);
		if (handlers.connectError) {
			// connectError gets a socket object that never opened (Bun's detached wrapper).
			const shell = new BunSocket(s, handlers, null, false);
			shell._setAllowHalfOpen(allowHalfOpen);
			let r;
			try { r = handlers.connectError.call(shell, shell, err); } catch (x) { r = x; }
			// The error is handled by connectError; the promise still rejects, as handled.
			promise.catch(() => {});
			settle.reject(r instanceof Error ? r : err);
			return;
		}
		settle.reject(err);
	};
	s.once("error", onConnectFail);
	s.once("connect", () => {
		opened = true;
		s.removeListener("error", onConnectFail);
		const sock = new BunSocket(s, handlers, null);
		sock._setAllowHalfOpen(allowHalfOpen);
		sock.data = opts.data;
		// Bun resolves the promise first, then runs open; awaiting code resumes after.
		settle.resolve(sock);
		callHandler(sock, "open");
	});
	return promise;
}
