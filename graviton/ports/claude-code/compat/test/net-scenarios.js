// Shared by Bun and Node: every scenario drives api.serve / api.listen /
// api.connect with real clients (raw node:net / node:tls sockets speaking
// HTTP/1.x by hand, the runtime's own fetch and WebSocket) and returns what a
// caller can observe. gen-net-vectors.js runs this under stock Bun with the
// real Bun.* functions; check-net.mjs runs it under Node with compat/lib/net.mjs
// and compares the JSON.
//
// Network rules: loopback only (127.0.0.1 / ::1 / "localhost"), port 0 only,
// every server stopped in a finally. The one "fixed" port ever used is one a
// scenario just got from the kernel and released, to provoke EADDRINUSE or
// ECONNREFUSED on loopback.
//
// Normalised (run-specific) values: ephemeral port numbers become "PORT" in
// strings and "port>0" style booleans in fields; the Date response header is
// dropped (its value is the wall clock). Nothing else is normalised.
import net from "node:net";
import tls from "node:tls";
import http from "node:http";
import { createHash } from "node:crypto";
import { spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import dns from "node:dns/promises";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function deferred() {
	let resolve, reject;
	const p = new Promise((a, b) => { resolve = a; reject = b; });
	return { p, resolve, reject };
}
async function within(p, ms, label) {
	let t;
	try {
		return await Promise.race([p, new Promise((_, j) => { t = setTimeout(() => j(new Error("timeout: " + label)), ms); })]);
	} finally { clearTimeout(t); }
}
async function waitFor(cond, ms, label) {
	const end = Date.now() + ms;
	while (!cond()) {
		if (Date.now() > end) throw new Error("waitFor timeout: " + label);
		await sleep(5);
	}
}
const P = (s, ...ports) => {
	let out = String(s);
	for (const p of ports) if (p) out = out.split(String(p)).join("PORT");
	return out;
};
function errShape(e, ...ports) {
	if (!(e instanceof Object)) return { notAnObject: String(e) };
	const keys = Object.keys(e).sort();
	const o = { isError: e instanceof Error, name: e.name, message: P(e.message, ...ports), keys };
	for (const k of ["code", "errno", "syscall", "address", "hostname"]) if (k in e) o[k] = e[k];
	if ("port" in e) o.port = typeof e.port === "number" && e.port > 0 ? "port>0" : e.port;
	return o;
}

// ---- a minimal HTTP/1.x client over a raw socket -----------------------------
function parseResponse(buf, head, eof) {
	const he = buf.indexOf("\r\n\r\n");
	if (he < 0) return null;
	const lines = buf.subarray(0, he).toString("latin1").split("\r\n");
	const m = /^(HTTP\/1\.[01]) (\d{3}) ?(.*)$/.exec(lines[0]);
	if (!m) return { bad: lines[0] };
	const headers = lines.slice(1).map((l) => { const i = l.indexOf(":"); return [l.slice(0, i).toLowerCase(), l.slice(i + 1).trim()]; });
	const get = (k) => headers.find((h) => h[0] === k)?.[1];
	const status = +m[2];
	const rest = buf.subarray(he + 4);
	const base = { version: m[1], status, reason: m[3], headers };
	if (head || status === 204 || status === 304 || (status >= 100 && status < 200)) return { ...base, framing: "none", body: Buffer.alloc(0), complete: true, extra: rest.length };
	const te = get("transfer-encoding");
	if (te && /chunked/i.test(te)) {
		const parts = []; let off = 0;
		for (;;) {
			const le = rest.indexOf("\r\n", off);
			if (le < 0) return eof ? { ...base, framing: "chunked", body: Buffer.concat(parts), complete: false } : null;
			const n = parseInt(rest.subarray(off, le).toString("latin1"), 16);
			if (n === 0) {
				if (rest.length < le + 4) return eof ? { ...base, framing: "chunked", body: Buffer.concat(parts), complete: false } : null;
				return { ...base, framing: "chunked", chunks: parts.length, body: Buffer.concat(parts), complete: true, extra: rest.length - (le + 4) };
			}
			if (rest.length < le + 2 + n + 2) return eof ? { ...base, framing: "chunked", body: Buffer.concat(parts), complete: false } : null;
			parts.push(rest.subarray(le + 2, le + 2 + n));
			off = le + 2 + n + 2;
		}
	}
	const cl = get("content-length");
	if (cl !== undefined) {
		const n = +cl;
		if (rest.length < n) return eof ? { ...base, framing: "content-length", body: rest, complete: false } : null;
		return { ...base, framing: "content-length", body: rest.subarray(0, n), complete: true, extra: rest.length - n };
	}
	return eof ? { ...base, framing: "eof", body: rest, complete: true } : null;
}

// Sends `raw` and resolves once one full response was parsed or the socket ended.
// `res.sock` stays open (keep-alive tests use it); callers destroy it.
function httpRaw({ port, host = "127.0.0.1", raw, head = false, useTls = false }) {
	return new Promise((resolve) => {
		const sock = useTls
			? tls.connect({ port, host, rejectUnauthorized: false, servername: "localhost" })
			: net.connect({ port, host });
		let buf = Buffer.alloc(0), done = false, err = null;
		const closed = deferred();
		const finish = (r) => { if (done) return; done = true; resolve({ res: r, sock, closed: closed.p, err: () => err }); };
		sock.on(useTls ? "secureConnect" : "connect", () => sock.write(raw));
		sock.on("data", (d) => {
			buf = Buffer.concat([buf, d]);
			const r = parseResponse(buf, head, false);
			if (r && (r.complete || r.bad)) finish(r);
		});
		sock.on("error", (e) => { err = e.code || e.message; });
		sock.on("close", () => {
			closed.resolve(true);
			if (!done) finish(buf.length ? parseResponse(buf, head, true) ?? { partialHead: buf.length } : { noResponse: true, err });
		});
	});
}
function summ(r, ...ports) {
	if (!r || !r.status) return r ? { ...r, body: undefined } : r;
	return {
		version: r.version, status: r.status, reason: r.reason,
		headers: r.headers.filter(([k]) => k !== "date").map(([k, v]) => [k, P(v, ...ports)]),
		hasDate: r.headers.some(([k]) => k === "date"),
		framing: r.framing, complete: r.complete,
		body: r.body.length <= 512 ? P(r.body.toString("utf8"), ...ports) : { len: r.body.length, sha: createHash("sha256").update(r.body).digest("hex") },
	};
}
const GET = (path = "/", extra = "", host = "127.0.0.1") => `GET ${path} HTTP/1.1\r\nHost: ${host}\r\n${extra}\r\n`;
const GETC = (path = "/", extra = "") => GET(path, "Connection: close\r\n" + extra);

function tryConnect(port, host = "127.0.0.1") {
	return new Promise((resolve) => {
		const s = net.connect({ port, host });
		s.on("connect", () => { s.destroy(); resolve("connected"); });
		s.on("error", (e) => resolve(e.code));
	});
}
// A kernel-assigned loopback port that is free again by the time it is returned.
function freePort(host = "127.0.0.1") {
	return new Promise((resolve, reject) => {
		const s = net.createServer();
		s.on("error", reject);
		s.listen({ port: 0, host }, () => { const p = s.address().port; s.close(() => resolve(p)); });
	});
}
// A plain node:net peer for the Bun.connect scenarios. An IPv6 peer is bound
// v6-only: a ::1 socket never accepts IPv4 anyway, so this changes nothing on
// Linux, and it is the only way Node binds IPv6 on Haiku, whose IPv6 sockets
// are always v6-only and refuse IPV6_V6ONLY=0 (libuv sets it to 0 by default).
function peerServer(onConn, host = "127.0.0.1", port = 0) {
	return new Promise((resolve, reject) => {
		const s = net.createServer(onConn);
		s.on("error", reject);
		s.listen({ port, host, ...(net.isIP(host) === 6 ? { ipv6Only: true } : {}) }, () => resolve(s));
	});
}
const closeServer = (s) => new Promise((r) => s.close(() => r()));

function track(list, v) { list.push(v); return v; }
function stopAll(list) {
	for (const s of list.splice(0)) {
		try { if (typeof s.stop === "function") s.stop(true); else if (typeof s.close === "function") s.close(); else if (typeof s.destroy === "function") s.destroy(); } catch {}
	}
}

// ---- scenarios ----------------------------------------------------------------
export function scenarios(api, env) {
	const S = [];
	const add = (name, fn) => S.push([name, fn]);
	const H = "127.0.0.1";

	add("serve: shape and members", async (t) => {
		const s = track(t, api.serve({ hostname: H, port: 0, fetch: () => new Response("x") }));
		const port = s.port;
		return {
			portType: typeof port, portPositive: port > 0, port3000: port === 3000,
			hostname: s.hostname, url: P(s.url.href, port), urlIsURL: s.url instanceof URL, protocol: s.protocol,
			development: s.development, pendingRequests: s.pendingRequests,
			fns: ["stop", "unref", "ref", "requestIP"].map((k) => [k, typeof s[k]]),
			unrefReturnsSelf: s.unref() === s, refReturnsSelf: s.ref() === s,
		};
	});

	add("serve: GET round trip, request shape, second argument", async (t) => {
		let seen;
		const s = track(t, api.serve({
			hostname: H, port: 0,
			fetch(req, server) {
				seen = {
					same: server === s, thisIsServer: this === s, method: req.method, url: P(req.url, s.port), isRequest: req instanceof Request,
					xa: req.headers.get("x-a"), xm: req.headers.get("x-m"), host: P(req.headers.get("host"), s.port),
					pending: server.pendingRequests, signal: typeof req.signal, aborted: req.signal.aborted,
					ip: (() => { const i = server.requestIP(req); return i && { address: i.address, family: i.family, port: i.port > 0, keys: Object.keys(i).sort() }; })(),
					bodyUsed: req.bodyUsed, body: req.body,
				};
				return new Response("hello", { status: 201, statusText: "Made", headers: { "x-out": "1", "content-type": "text/plain" } });
			},
		}));
		const r = await httpRaw({ port: s.port, raw: GETC("/a/b?c=d&e", `X-A: one\r\nX-M: 1\r\nX-M: 2\r\n`, `127.0.0.1:${s.port}`) });
		r.sock.destroy();
		return { seen, res: summ(r.res, s.port), after: s.pendingRequests };
	});

	add("serve: framing and header order of Response kinds", async (t) => {
		const up = track(t, api.serve({ hostname: H, port: 0, fetch: () => new Response("upstream-body", { headers: { "content-type": "text/plain", "x-up": "1" } }) }));
		const s = track(t, api.serve({
			hostname: H, port: 0,
			fetch(req) {
				const u = new URL(req.url).pathname;
				if (u === "/str") return new Response("text body", { headers: { "x-z": "1", "content-type": "text/plain" } });
				if (u === "/json") return new Response('{"a":1}', { headers: { etag: '"x"', "content-type": "application/json" } });
				if (u === "/null") return new Response(null, { status: 200 });
				if (u === "/304") return new Response(null, { status: 304, headers: { etag: '"x"' } });
				if (u === "/204") return new Response(null, { status: 204 });
				if (u === "/bytes") return new Response(new Uint8Array([1, 2, 3]), { headers: { "content-type": "application/octet-stream" } });
				if (u === "/blob") return new Response(new Blob(["blobby"], { type: "text/x-blob" }));
				if (u === "/stream-now") return new Response(new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode("ab")); c.enqueue(new TextEncoder().encode("cd")); c.close(); } }), { headers: { "content-type": "text/plain" } });
				if (u === "/stream-slow") return new Response(new ReadableStream({ async pull(c) { await sleep(30); c.enqueue(new TextEncoder().encode("z" + (this.n = (this.n ?? 0) + 1))); if (this.n === 3) c.close(); } }), { headers: { "content-type": "text/plain" } });
				if (u === "/cl") return new Response("12345", { headers: { "content-length": "5", "content-type": "text/plain" } });
				if (u === "/stream-partial") return new Response(new ReadableStream({ start(c) { c.enqueue(new TextEncoder().encode("ab")); }, async pull(c) { await sleep(30); c.enqueue(new TextEncoder().encode("cd")); c.close(); } }), { headers: { "content-type": "text/plain" } });
				if (u === "/stream-cl") return new Response(new ReadableStream({ async pull(c) { await sleep(30); c.enqueue(new TextEncoder().encode("wxyz")); c.close(); } }), { headers: { "content-type": "text/plain", "content-length": "4" } });
				if (u === "/proxy") return fetch(`http://127.0.0.1:${up.port}/up`);
				if (u === "/proxy-body") return fetch(`http://127.0.0.1:${up.port}/up`).then((r) => new Response(r.body, { status: r.status, headers: { "content-type": "text/plain" } }));
				if (u === "/cookies") { const h = new Headers(); h.append("set-cookie", "a=1"); h.append("set-cookie", "b=2"); h.set("content-type", "text/plain"); return new Response("c", { headers: h }); }
				return new Response("nf", { status: 404, headers: { "content-type": "text/plain" } });
			},
		}));
		const out = {};
		for (const p of ["/str", "/json", "/null", "/304", "/204", "/bytes", "/blob", "/stream-now", "/stream-slow", "/stream-partial", "/stream-cl", "/proxy", "/proxy-body", "/cl", "/cookies", "/missing"]) {
			const r = await httpRaw({ port: s.port, raw: GETC(p) });
			r.sock.destroy();
			out[p] = summ(r.res, s.port);
		}
		for (const p of ["/str", "/cookies", "/json", "/null", "/204", "/stream-slow", "/cl"]) {
			const h = await httpRaw({ port: s.port, raw: `HEAD ${p} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`, head: true });
			h.sock.destroy();
			out["HEAD " + p] = summ(h.res, s.port);
		}
		return out;
	});

	// What new Response("...") and Response.json() put on the wire is decided by
	// the runtime's Response class (Bun adds text/plain;charset=utf-8 when the
	// body is written and none for HEAD; Node's undici sets the header at
	// construction), not by Bun.serve. Kept apart so a difference here is not
	// read as a net.mjs defect.
	add("Response class defaults on the wire (outside net.mjs)", async (t) => {
		const s = track(t, api.serve({ hostname: H, port: 0, fetch(req) { const u = new URL(req.url).pathname; if (u === "/json") return Response.json({ a: 1 }); if (u === "/404") return new Response("nf", { status: 404 }); return new Response("text body"); } }));
		const out = {};
		for (const [m, p] of [["GET", "/str"], ["GET", "/json"], ["GET", "/404"], ["HEAD", "/str"], ["HEAD", "/json"]]) {
			const r = await httpRaw({ port: s.port, raw: `${m} ${p} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`, head: m === "HEAD" });
			r.sock.destroy();
			out[m + " " + p] = summ(r.res, s.port).headers;
		}
		return out;
	});

	add("serve: no Host header (HTTP/1.0) and absolute paths", async (t) => {
		const urls = [];
		const s = track(t, api.serve({ hostname: H, port: 0, fetch(req) { urls.push(P(req.url, s.port)); return new Response("ok", { headers: { "content-type": "text/plain" } }); } }));
		const a = await httpRaw({ port: s.port, raw: "GET /p?q=1 HTTP/1.0\r\n\r\n" });
		a.sock.destroy();
		const b = await httpRaw({ port: s.port, raw: "GET /x HTTP/1.1\r\nHost: example.test:81\r\nConnection: close\r\n\r\n" });
		b.sock.destroy();
		const c = await httpRaw({ port: s.port, raw: "GET /y HTTP/1.1\r\nConnection: close\r\n\r\n" });
		c.sock.destroy();
		return { urls, a: summ(a.res, s.port), b: summ(b.res, s.port), c: summ(c.res, s.port) };
	});

	add("serve: POST bodies (small, 1 MiB chunked, json)", async (t) => {
		const s = track(t, api.serve({
			hostname: H, port: 0,
			async fetch(req) {
				const u = new URL(req.url).pathname;
				if (u === "/json") { const j = await req.json(); return new Response(JSON.stringify({ got: j, ct: req.headers.get("content-type") }), { headers: { "content-type": "application/json" } }); }
				const b = await req.arrayBuffer();
				return new Response(createHash("sha256").update(Buffer.from(b)).digest("hex") + ":" + b.byteLength, { headers: { "content-type": "text/plain" } });
			},
		}));
		const small = await httpRaw({ port: s.port, raw: `POST /s HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello` });
		small.sock.destroy();
		const big = Buffer.alloc(1 << 20, 7);
		let raw = `POST /b HTTP/1.1\r\nHost: x\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n`;
		const chunks = [];
		for (let o = 0; o < big.length; o += 65536) chunks.push(Buffer.concat([Buffer.from((65536).toString(16) + "\r\n"), big.subarray(o, o + 65536), Buffer.from("\r\n")]));
		const bigReq = Buffer.concat([Buffer.from(raw), ...chunks, Buffer.from("0\r\n\r\n")]);
		const bigR = await httpRaw({ port: s.port, raw: bigReq });
		bigR.sock.destroy();
		const js = await httpRaw({ port: s.port, raw: `POST /json HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: 9\r\n\r\n{"k":[1]}` });
		js.sock.destroy();
		return { small: summ(small.res), big: summ(bigR.res), expectBig: createHash("sha256").update(big).digest("hex"), json: summ(js.res) };
	});

	add("serve: runtime fetch() client round trip", async (t) => {
		const s = track(t, api.serve({ hostname: H, port: 0, async fetch(req) { return new Response(req.method + " " + (await req.text()), { headers: { "x-k": "v", "content-type": "text/plain" } }); } }));
		const r = await fetch(`http://127.0.0.1:${s.port}/f`, { method: "POST", body: "payload" });
		const r2 = await fetch(s.url);
		return { status: r.status, xk: r.headers.get("x-k"), body: await r.text(), viaUrl: [r2.status, await r2.text()] };
	});

	add("serve: fetch throws / rejects / returns undefined (default development)", async (t) => {
		const errs = [];
		const s = track(t, api.serve({
			hostname: H, port: 0,
			fetch(req) {
				const u = new URL(req.url).pathname;
				if (u === "/sync") throw new Error("boom-sync");
				if (u === "/async") return Promise.reject(new Error("boom-async"));
				if (u === "/undef") return undefined;
				return new Response("ok");
			},
		}));
		const out = { development: s.development };
		for (const p of ["/sync", "/async", "/undef"]) {
			const r = await httpRaw({ port: s.port, raw: GETC(p) });
			r.sock.destroy();
			const x = summ(r.res, s.port);
			const text = r.res?.body ? r.res.body.toString("utf8") : "";
			// The development error page is Bun's own HTML; only its kind is compared.
			out[p] = x && { status: x.status, ct: x.headers.find((h) => h[0] === "content-type")?.[1], framing: x.framing, bodyIsHtml: /<html/i.test(text), bodyStart: /<html/i.test(text) ? null : text.slice(0, 60) };
		}
		return out;
	});

	add("serve: fetch throws / returns undefined with development:false", async (t) => {
		const s = track(t, api.serve({
			hostname: H, port: 0, development: false,
			fetch(req) {
				const u = new URL(req.url).pathname;
				if (u === "/sync") throw new Error("boom-sync");
				if (u === "/async") return Promise.reject(new Error("boom-async"));
				if (u === "/undef") return undefined;
				return new Response("ok");
			},
		}));
		const out = { development: s.development };
		for (const p of ["/sync", "/async", "/undef"]) {
			const r = await httpRaw({ port: s.port, raw: GETC(p) });
			r.sock.destroy();
			out[p] = summ(r.res, s.port);
		}
		return out;
	});

	add("serve: error handler (sync Response, async Response, throws)", async (t) => {
		const calls = [];
		const s = track(t, api.serve({
			hostname: H, port: 0, development: false,
			fetch(req) { throw new Error("E:" + new URL(req.url).pathname); },
			error(e) {
				calls.push([this === s, e instanceof Error, e.message]);
				if (e.message === "E:/a") return new Response("handled-a", { status: 503, headers: { "content-type": "text/plain" } });
				if (e.message === "E:/b") return (async () => new Response("handled-b", { status: 502, headers: { "content-type": "text/plain" } }))();
				throw new Error("handler-threw");
			},
		}));
		const out = {};
		for (const p of ["/a", "/b", "/c"]) {
			const r = await httpRaw({ port: s.port, raw: GETC(p) });
			r.sock.destroy();
			out[p] = summ(r.res, s.port);
		}
		return { out, calls };
	});

	add("serve: ::1 binding, dual 127.0.0.1 + ::1 on one port (xf)", async (t) => {
		const fetch = (req, srv) => { const i = srv.requestIP(req); return new Response(i.family + " " + i.address, { headers: { "content-type": "text/plain" } }); };
		const a = track(t, api.serve({ hostname: H, port: 0, fetch }));
		const b = track(t, api.serve({ hostname: "::1", port: a.port, fetch }));
		const r4 = await httpRaw({ port: a.port, raw: GETC() }); r4.sock.destroy();
		const r6 = await httpRaw({ port: a.port, host: "::1", raw: GETC() }); r6.sock.destroy();
		return { samePort: a.port === b.port, hostname6: b.hostname, url6: P(b.url.href, b.port), r4: summ(r4.res).body, r6: summ(r6.res).body };
	});

	add("serve: EADDRINUSE error shape", async (t) => {
		const a = track(t, api.serve({ hostname: H, port: 0, fetch: () => new Response("a") }));
		let e1, e2;
		try { track(t, api.serve({ hostname: H, port: a.port, fetch: () => new Response("b") })); e1 = "no-throw"; } catch (e) { e1 = errShape(e, a.port); }
		const n = await peerServer(() => {}, H);
		const np = n.address().port;
		try { track(t, api.serve({ hostname: H, port: np, fetch: () => new Response("b") })); e2 = "no-throw"; } catch (e) { e2 = errShape(e, np); }
		await closeServer(n);
		return { sameServeTwice: e1, nodeHolder: e2 };
	});

	add("serve: development:false sets SO_REUSEPORT (second bind succeeds)", async (t) => {
		const a = track(t, api.serve({ hostname: H, port: 0, development: false, fetch: () => new Response("a") }));
		let r;
		try { track(t, api.serve({ hostname: H, port: a.port, development: false, fetch: () => new Response("b") })); r = "bound"; } catch (e) { r = errShape(e, a.port); }
		let r2;
		try { track(t, api.serve({ hostname: H, port: a.port, development: false, reusePort: false, fetch: () => new Response("b") })); r2 = "bound"; } catch (e) { r2 = errShape(e, a.port); }
		return { prodTwice: r, explicitNoReuse: r2 };
	});

	add("serve: invalid options", async (t) => {
		const out = {};
		const tryS = (k, o) => { try { track(t, api.serve(o)); out[k] = "ok"; } catch (e) { out[k] = errShape(e); } };
		tryS("idleTimeout 256", { hostname: H, port: 0, idleTimeout: 256, fetch: () => new Response("") });
		tryS("idleTimeout 1.5", { hostname: H, port: 0, idleTimeout: 1.5, fetch: () => new Response("") });
		tryS("port -1", { hostname: H, port: -1, fetch: () => new Response("") });
		tryS("port 1.5", { hostname: H, port: 1.5, fetch: () => new Response("") });
		tryS("fetch not fn", { hostname: H, port: 0, fetch: 3 });
		tryS("error not fn", { hostname: H, port: 0, fetch: () => new Response(""), error: 3 });
		return out;
	});

	add("serve: stop(true) — refused afterwards, promise, idempotent", async (t) => {
		const s = track(t, api.serve({ hostname: H, port: 0, fetch: () => new Response("x") }));
		const port = s.port;
		const p = s.stop(true);
		const isP = p instanceof Promise;
		const v = await within(p, 3000, "stop");
		const again = s.stop(true);
		const v2 = await within(again, 3000, "stop2");
		return { isP, v, v2, after: await tryConnect(port), portAfter: s.port === port ? "same" : s.port, pending: s.pendingRequests };
	});

	add("serve: stop(false) drains an in-flight request", async (t) => {
		const gate = deferred(), log = [];
		const s = track(t, api.serve({ hostname: H, port: 0, async fetch(req, srv) { log.push("fetch pending=" + srv.pendingRequests); await gate.p; log.push("released"); return new Response("late", { headers: { "content-type": "text/plain" } }); } }));
		const port = s.port;
		const inflight = httpRaw({ port, raw: GET("/") });
		await waitFor(() => log.length === 1, 3000, "fetch");
		let resolved = false;
		const sp = s.stop(false);
		sp.then(() => { resolved = true; log.push("stop resolved"); });
		log.push("after stop pending=" + s.pendingRequests);
		log.push("connect after stop: " + await tryConnect(port));
		await sleep(50);
		log.push("resolved before release: " + resolved);
		gate.resolve();
		const r = await within(inflight, 3000, "inflight");
		await within(sp, 3000, "drain");
		const closedByServer = await Promise.race([r.closed, sleep(1500).then(() => false)]);
		r.sock.destroy();
		return { log, res: summ(r.res, port), closedByServer, pending: s.pendingRequests };
	});

	add("serve: stop(true) aborts an in-flight request", async (t) => {
		const log = [];
		let sig;
		const s = track(t, api.serve({ hostname: H, port: 0, async fetch(req) { sig = req.signal; sig.addEventListener("abort", () => log.push("abort event")); log.push("fetch"); await sleep(400); log.push("handler done aborted=" + sig.aborted); return new Response("never"); } }));
		const inflight = httpRaw({ port: s.port, raw: GET("/") });
		await waitFor(() => log.length === 1, 3000, "fetch");
		const sp = s.stop(true);
		log.push("after stop");
		const r = await within(inflight, 3000, "inflight");
		await within(sp, 3000, "stop");
		log.push("stop resolved");
		await sleep(500);
		return { log, res: r.res, aborted: sig.aborted, reasonName: sig.reason?.name, pending: s.pendingRequests };
	});

	add("serve: client disconnect aborts req.signal, pendingRequests returns to 0", async (t) => {
		const log = [];
		const s = track(t, api.serve({ hostname: H, port: 0, fetch(req, srv) {
			req.signal.addEventListener("abort", () => log.push("abort pending=" + srv.pendingRequests));
			log.push("fetch pending=" + srv.pendingRequests);
			return new Response(new ReadableStream({ async pull(c) { await sleep(50); c.enqueue(new TextEncoder().encode("tick")); } }), { headers: { "content-type": "text/plain" } });
		} }));
		const sock = net.connect({ port: s.port, host: H });
		await new Promise((r) => sock.on("connect", r));
		let got = 0;
		sock.on("data", (d) => { got += d.length; });
		sock.write(GET("/"));
		await waitFor(() => got > 0, 3000, "first bytes");
		log.push("pending while streaming=" + s.pendingRequests);
		sock.destroy();
		await waitFor(() => log.some((l) => l.startsWith("abort")), 3000, "abort");
		await waitFor(() => s.pendingRequests === 0, 3000, "pending 0");
		log.push("pending end=" + s.pendingRequests);
		return { log };
	});

	add("serve: keep-alive reuse and idleTimeout", async (t) => {
		const out = {};
		for (const idle of [1, 0]) {
			const s = track(t, api.serve({ hostname: H, port: 0, idleTimeout: idle, fetch: () => new Response("k", { headers: { "content-type": "text/plain" } }) }));
			const r = await httpRaw({ port: s.port, raw: GET("/") });
			// a second request on the same connection
			const second = new Promise((resolve) => { let b = Buffer.alloc(0); r.sock.removeAllListeners("data"); r.sock.on("data", (d) => { b = Buffer.concat([b, d]); const x = parseResponse(b, false, false); if (x?.complete) resolve(x.status); }); });
			r.sock.write(GET("/2"));
			const st2 = await within(second, 3000, "second");
			const closedAt = await Promise.race([r.closed.then(() => "closed"), sleep(2600).then(() => "open")]);
			r.sock.destroy();
			out["idle" + idle] = { first: r.res.status, second: st2, after2600ms: closedAt };
			s.stop(true);
		}
		return out;
	});

	add("serve: idle connection and slow request vs idleTimeout (12.5 s)", async (t) => {
		const mk = (idle) => track(t, api.serve({ hostname: H, port: 0, ...(idle === undefined ? {} : { idleTimeout: idle }), development: false, async fetch(req) { if (new URL(req.url).pathname === "/slow") await sleep(2500); return new Response("k", { headers: { "content-type": "text/plain" } }); } }));
		const probe = async (s, path) => {
			const t0 = Date.now();
			const r = await httpRaw({ port: s.port, raw: GET(path) });
			const closed = await Promise.race([r.closed.then(() => Math.round((Date.now() - t0) / 1000)), sleep(12500).then(() => "open")]);
			r.sock.destroy();
			return { status: r.res?.status ?? Object.keys(r.res ?? {}).join(","), closedAfterS: closed };
		};
		const servers = { d: mk(undefined), i0: mk(0), i1: mk(1), i3: mk(3) };
		const jobs = [];
		for (const [k, s] of Object.entries(servers)) { jobs.push(probe(s, "/").then((v) => ["idle " + k, v])); jobs.push(probe(s, "/slow").then((v) => ["slow " + k, v])); }
		return Object.fromEntries(await Promise.all(jobs));
	});

	add("serve: request header size limit and node:http maxHeaderSize", async (t) => {
		const s = track(t, api.serve({ hostname: H, port: 0, fetch: (req) => new Response(String(req.headers.get("x-big")?.length), { headers: { "content-type": "text/plain" } }) }));
		const big = "a".repeat(20000);
		const before = http.maxHeaderSize;
		const r1 = await httpRaw({ port: s.port, raw: GETC("/", `X-Big: ${big}\r\n`) }); r1.sock.destroy();
		let set;
		try { http.maxHeaderSize = 262144; set = http.maxHeaderSize; } catch (e) { set = "THROW " + e.message; }
		const s2 = track(t, api.serve({ hostname: H, port: 0, fetch: (req) => new Response(String(req.headers.get("x-big")?.length), { headers: { "content-type": "text/plain" } }) }));
		const r2 = await httpRaw({ port: s2.port, raw: GETC("/", `X-Big: ${big}\r\n`) }); r2.sock.destroy();
		const r3 = await httpRaw({ port: s.port, raw: GETC("/", `X-Big: ${big}\r\n`) }); r3.sock.destroy();
		try { http.maxHeaderSize = before; } catch {}
		return { before, set, defaultLimit: summ(r1.res), raisedNewServer: summ(r2.res), raisedOldServer: summ(r3.res) };
	});

	add("serve: maxRequestBodySize", async (t) => {
		let called = 0;
		const s = track(t, api.serve({ hostname: H, port: 0, maxRequestBodySize: 10, async fetch(req) { called++; return new Response(String((await req.arrayBuffer()).byteLength), { headers: { "content-type": "text/plain" } }); } }));
		const ok = await httpRaw({ port: s.port, raw: `POST / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 5\r\n\r\nhello` }); ok.sock.destroy();
		const big = await httpRaw({ port: s.port, raw: `POST / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 50\r\n\r\n${"x".repeat(50)}` }); big.sock.destroy();
		const msgs = [];
		const s2 = track(t, api.serve({ hostname: H, port: 0, maxRequestBodySize: 10, development: false, async fetch(req) { try { const b = await req.arrayBuffer(); return new Response("read " + b.byteLength, { headers: { "content-type": "text/plain" } }); } catch (e) { msgs.push([e?.name, e?.message, e?.code]); return new Response("caught", { headers: { "content-type": "text/plain" } }); } } }));
		const ch = await httpRaw({ port: s2.port, raw: `POST / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n32\r\n${"y".repeat(50)}\r\n0\r\n\r\n` }); ch.sock.destroy();
		await sleep(50);
		return { ok: summ(ok.res), big: summ(big.res), called, chunkedOver: summ(ch.res), msgs };
	});

	add("serve: hostname localhost", async (t) => {
		const s = track(t, api.serve({ hostname: "localhost", port: 0, fetch: (req, srv) => new Response(srv.requestIP(req).address, { headers: { "content-type": "text/plain" } }) }));
		const v4 = await tryConnect(s.port, "127.0.0.1"), v6 = await tryConnect(s.port, "::1");
		// The listener goes to the system resolver's first IPv6 answer, else its
		// first IPv4 one (uSockets bsd_create_listen_socket), so what the host's
		// resolver says for localhost is recorded next to it.
		const resolved = (await dns.lookup("localhost", { all: true })).map((a) => a.address);
		return { hostname: s.hostname, url: P(s.url.href, s.port), v4, v6, resolved };
	});

	if (env.tlsDir) add("serve: tls {cert,key}", async (t) => {
		const cert = readFileSync(env.tlsDir + "/cert.pem", "utf8"), key = readFileSync(env.tlsDir + "/key.pem", "utf8");
		const s = track(t, api.serve({ hostname: H, port: 0, idleTimeout: 0, development: false, tls: { cert, key }, fetch: (req, srv) => new Response(P(req.url, srv.port) + " " + srv.requestIP(req).family, { headers: { "content-type": "text/plain" } }) }));
		const r = await httpRaw({ port: s.port, raw: GETC("/sec", "", `localhost:${s.port}`), useTls: true });
		r.sock.destroy();
		const plain = await httpRaw({ port: s.port, raw: GETC("/plain") });
		plain.sock.destroy();
		return { protocol: s.protocol, url: P(s.url.href, s.port), res: summ(r.res, s.port), plainToTls: plain.res && (plain.res.status ?? Object.keys(plain.res)) };
	});

	add("serve: unref'd server lets the process exit (child)", async () => {
		const out = {};
		for (const mode of ["serve-unref-idle", "serve-unref-keepalive", "serve-ref", "listen-unref-conn", "listen-unref-idle"]) out[mode] = await childExit(env, mode);
		return out;
	});

	// ---- Bun.listen ----------------------------------------------------------
	const mkLog = () => { const log = []; return log; };

	add("listen: shape, accept, data type, members, peer end", async (t) => {
		const log = mkLog();
		let members, peerPort;
		const L = track(t, api.listen({
			hostname: H, port: 0,
			socket: {
				open(s) {
					log.push("open");
					members = { data: s.data, readyState: s.readyState, remoteAddress: s.remoteAddress, remoteFamily: s.remoteFamily, localAddress: s.localAddress, localPortIsListener: s.localPort === L.port, remotePort: s.remotePort, fns: ["write", "end", "terminate", "pause", "resume", "ref", "unref"].map((k) => [k, typeof s[k]]) };
					s.data = { tag: 1 };
				},
				data(s, d) { log.push(["data", d?.constructor?.name, Buffer.isBuffer(d), d instanceof Uint8Array, d.toString(), s.data?.tag, this === s]); log.push(["write", s.write("pong:" + d.toString()), s.write(Buffer.from("!")), s.write(new Uint8Array([10]))]); },
				drain(s) { log.push("drain"); },
				end(s) { log.push("end-handler-should-not-exist"); },
				close(s, err) { log.push(["close", typeof err, err?.code, s.readyState]); },
				error(s, e) { log.push(["error", e?.code]); },
			},
		}));
		const shape = { port: L.port > 0, hostname: L.hostname, fns: ["stop", "ref", "unref"].map((k) => [k, typeof L[k]]), data: L.data };
		const c = net.connect({ port: L.port, host: H });
		await new Promise((r) => c.on("connect", r));
		peerPort = c.localPort;
		let got = "";
		c.on("data", (d) => { got += d.toString(); });
		const cclose = new Promise((r) => c.on("close", r));
		await waitFor(() => log.includes("open"), 3000, "open");
		c.write("ping");
		await waitFor(() => got.length >= "pong:ping!\n".length, 3000, "reply");
		c.end();
		await waitFor(() => log.some((l) => Array.isArray(l) && l[0] === "close"), 3000, "close");
		await within(cclose, 3000, "client close");
		members.remotePort = members.remotePort === peerPort;
		return { shape, members, log, got };
	});

	add("listen: open handler that writes immediately; end() ordering", async (t) => {
		const log = mkLog();
		const L = track(t, api.listen({
			hostname: H, port: 0,
			socket: {
				open(s) { log.push(["w", s.write("hi")]); log.push("before end"); log.push(["end", s.end()]); log.push("after end"); log.push(["write after end", s.write("x"), "end again", s.end(), "readyState", s.readyState]); },
				data() {},
				close(s, e) { log.push(["close", typeof e]); },
				error(s, e) { log.push(["error", e?.code]); },
			},
		}));
		const c = net.connect({ port: L.port, host: H });
		let got = "", ended = false;
		c.on("data", (d) => { got += d; });
		c.on("end", () => { ended = true; });
		await within(new Promise((r) => c.on("close", r)), 3000, "client close");
		return { log, got, ended };
	});

	add("listen: end(data) and terminate() as seen by the peer", async (t) => {
		const out = {};
		for (const how of ["end-data", "terminate"]) {
			const log = mkLog();
			const L = track(t, api.listen({
				hostname: H, port: 0,
				socket: {
					data(s, d) {
						log.push("data " + d);
						if (how === "end-data") { log.push(["end", s.end("bye")]); log.push("after end"); }
						else { s.write("partial"); log.push("before terminate"); log.push(["terminate", s.terminate()]); log.push("after terminate"); log.push(["write", s.write("x")]); }
					},
					close(s, e) { log.push(["close", typeof e, e?.code]); },
					error(s, e) { log.push(["error", e?.code]); },
				},
			}));
			const c = net.connect({ port: L.port, host: H });
			let got = "", cerr = null, cend = false;
			c.on("data", (d) => { got += d; });
			c.on("error", (e) => { cerr = e.code; });
			c.on("end", () => { cend = true; });
			await new Promise((r) => c.on("connect", r));
			c.write("go");
			await within(new Promise((r) => c.on("close", r)), 3000, "client close");
			out[how] = { log, got, cerr, cend };
			L.stop(true);
		}
		return out;
	});

	add("listen: peer reset delivers close(err) / error", async (t) => {
		const log = mkLog();
		const L = track(t, api.listen({
			hostname: H, port: 0,
			socket: {
				open() { log.push("open"); },
				data(s, d) { log.push("data " + d); },
				close(s, e) { log.push(["close", typeof e, e && errShape(e)]); },
				error(s, e) { log.push(["error", e?.code]); },
			},
		}));
		const c = net.connect({ port: L.port, host: H });
		await new Promise((r) => c.on("connect", r));
		c.write("x");
		await waitFor(() => log.includes("data x"), 3000, "data");
		c.resetAndDestroy();
		await waitFor(() => log.some((l) => Array.isArray(l) && l[0] === "close"), 3000, "close");
		await sleep(50);
		return { log };
	});

	add("listen: back-pressure, partial write, drain (relay pattern)", async (t) => {
		const total = 24 << 20, chunk = Buffer.alloc(256 << 10);
		for (let i = 0; i < chunk.length; i++) chunk[i] = (i * 31) & 255;
		const st = { partial: 0, zero: 0, negative: 0, drains: 0, sent: 0, maxRet: 0, retOverLen: false, nonInt: false };
		const expect = createHash("sha256");
		const L = track(t, api.listen({
			hostname: H, port: 0,
			socket: {
				open(s) { s.data = { buf: [] }; pump(s); },
				data() {},
				drain(s) { st.drains++; pump(s); },
				close() {},
				error(s, e) { st.err = e?.code; },
			},
		}));
		function pump(s) {
			const q = s.data.buf;
			for (;;) {
				if (q.length === 0) {
					if (st.sent >= total) { s.end(); return; }
					q.push(chunk); expect.update(chunk); st.sent += chunk.length;
				}
				const b = q[0];
				const n = s.write(b);
				if (!Number.isInteger(n)) st.nonInt = true;
				if (n > b.length) st.retOverLen = true;
				if (n < 0) { st.negative++; return; }
				if (n === 0) st.zero++;
				if (n < b.length) { st.partial++; q[0] = b.subarray(n); return; }
				q.shift();
			}
		}
		const c = net.connect({ port: L.port, host: H });
		const got = createHash("sha256");
		let n = 0;
		c.pause();
		c.on("data", (d) => { got.update(d); n += d.length; });
		await new Promise((r) => c.on("connect", r));
		await sleep(400);
		c.resume();
		await within(new Promise((r) => c.on("close", r)), 20000, "transfer");
		return { received: n, total, intact: got.digest("hex") === expect.digest("hex"), sawPartial: st.partial > 0, sawDrain: st.drains > 0, negative: st.negative, retOverLen: st.retOverLen, nonInt: st.nonInt, err: st.err ?? null };
	});

	add("listen: pause() / resume()", async (t) => {
		const log = mkLog();
		let sock;
		const L = track(t, api.listen({ hostname: H, port: 0, socket: { open(s) { sock = s; log.push(["pause", s.pause()]); }, data(s, d) { log.push("data " + d); }, close() { log.push("close"); } } }));
		const c = net.connect({ port: L.port, host: H });
		await new Promise((r) => c.on("connect", r));
		await waitFor(() => sock, 3000, "open");
		c.write("abc");
		await sleep(300);
		log.push("after 300ms");
		log.push(["resume", sock.resume()]);
		await waitFor(() => log.includes("data abc"), 3000, "data after resume");
		c.destroy();
		await waitFor(() => log.includes("close"), 3000, "close");
		return { log };
	});

	add("listen: handler throws -> error(socket, err)", async (t) => {
		const log = mkLog();
		const boom = new Error("data-threw");
		const L = track(t, api.listen({ hostname: H, port: 0, socket: { data(s) { log.push("data"); throw boom; }, error(s, e) { log.push(["error", e === boom, e.message, typeof s.write]); }, close() { log.push("close"); } } }));
		const c = net.connect({ port: L.port, host: H });
		await new Promise((r) => c.on("connect", r));
		c.write("x");
		await waitFor(() => log.length >= 2, 3000, "error");
		await sleep(100);
		log.push("socket still open: " + !c.destroyed);
		c.destroy();
		await waitFor(() => log.includes("close"), 3000, "close");
		return { log };
	});

	add("listen: stop(true) closes accepted sockets; stop() keeps them", async (t) => {
		const out = {};
		for (const force of [true, false]) {
			const log = mkLog();
			let sock;
			const L = track(t, api.listen({ hostname: H, port: 0, socket: { open(s) { sock = s; }, data(s, d) { log.push("data " + d); s.write("echo " + d); }, close(s, e) { log.push(["close", typeof e]); } } }));
			const port = L.port;
			const c = net.connect({ port, host: H });
			let got = "";
			c.on("data", (d) => { got += d; });
			const cclosed = new Promise((r) => c.on("close", () => r("closed")));
			await waitFor(() => sock, 3000, "open");
			log.push("before stop");
			log.push(["stop", force ? L.stop(true) : L.stop()]);
			log.push("after stop");
			log.push("new connect: " + await tryConnect(port));
			if (!force) { c.write("still"); await waitFor(() => got.includes("echo still"), 3000, "echo"); c.end(); }
			log.push("client: " + await within(cclosed, 3000, "cclose"));
			await waitFor(() => log.some((l) => Array.isArray(l) && l[0] === "close"), 3000, "server close");
			log.push(["port after stop", L.port === port, "hostname", L.hostname]);
			out[force ? "force" : "graceful"] = { log, got };
		}
		return out;
	});

	add("listen: EADDRINUSE error shape, ::1, missing handlers", async (t) => {
		const a = track(t, api.listen({ hostname: H, port: 0, socket: { data() {} } }));
		let e1;
		try { track(t, api.listen({ hostname: H, port: a.port, socket: { data() {} } })); e1 = "no-throw"; } catch (e) { e1 = errShape(e, a.port); }
		const b = track(t, api.listen({ hostname: "::1", port: 0, socket: { data() {} } }));
		let e3;
		try { track(t, api.listen({ hostname: H, port: 0, socket: { open() {} } })); e3 = "no-throw"; } catch (e) { e3 = errShape(e); }
		let e4;
		try { track(t, api.listen({ hostname: H, port: 0 })); e4 = "no-throw"; } catch (e) { e4 = errShape(e); }
		return { inUse: e1, v6: { port: b.port > 0, hostname: b.hostname, connect: await tryConnect(b.port, "::1") }, noData: e3, noSocket: e4 };
	});

	add("listen: WebSocket handshake over raw TCP (bufferedAmount probe pattern)", async (t) => {
		const log = mkLog();
		let upgraded = false, acc = Buffer.alloc(0), frameBytes = 0;
		const L = track(t, api.listen({
			hostname: H, port: 0,
			socket: {
				data(w, T) {
					if (upgraded) { frameBytes += T.length; return; }
					acc = Buffer.concat([acc, T]);
					const S = acc.indexOf("\r\n\r\n");
					if (S < 0) return;
					const head = acc.subarray(0, S).toString("latin1");
					log.push("request line: " + head.split("\r\n")[0]);
					const key = /sec-websocket-key: *(\S+)/i.exec(head)?.[1] ?? "";
					const acc2 = createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
					log.push(["101 write", w.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${acc2}\r\n\r\n`) > 0]);
					upgraded = true;
				},
				close() { log.push("server close"); },
			},
		}));
		const ws = new globalThis.WebSocket(`ws://127.0.0.1:${L.port}/`);
		ws.binaryType = "arraybuffer";
		const opened = await within(new Promise((r) => { ws.onopen = () => r("open"); ws.onerror = () => r("error"); ws.onclose = () => r("close"); }), 5000, "ws");
		if (opened === "open") ws.send(new Uint8Array(1000));
		await waitFor(() => frameBytes >= 1000, 3000, "frame");
		log.push("frame bytes>=1000: " + (frameBytes >= 1000));
		L.stop(true);
		await within(new Promise((r) => { if (ws.readyState === 3) r(); ws.onclose = () => r(); }), 3000, "ws close");
		return { opened, log };
	});

	// ---- Bun.connect -----------------------------------------------------------
	add("connect: open/promise ordering, members, data, peer end", async (t) => {
		const log = mkLog();
		let peerSock;
		const peer = track(t, await peerServer((s) => { peerSock = s; s.on("data", (d) => { log.push("peer got " + d); s.end("bye"); }); }));
		const port = peer.address().port;
		const p = api.connect({
			hostname: H, port,
			socket: {
				open(s) { log.push(["open", s.remoteAddress, s.remotePort === port, s.localPort > 0, s.readyState, s.data]); log.push(["write", s.write("hello")]); },
				data(s, d) { log.push(["data", d.constructor.name, Buffer.isBuffer(d), String(d)]); },
				close(s, e) { log.push(["close", typeof e]); },
				error(s, e) { log.push(["error", e?.code]); },
				drain() { log.push("drain"); },
			},
		});
		log.push(["returns promise", p instanceof Promise]);
		const s = await p;
		log.push(["resolved", typeof s.write, s.remoteAddress]);
		await waitFor(() => log.some((l) => Array.isArray(l) && l[0] === "close"), 3000, "close");
		return { log };
	});

	add("connect: data option, binaryType, end() from client", async (t) => {
		const log = mkLog();
		let peerGot = "", peerEnded = false;
		const peer = track(t, await peerServer((s) => { s.on("data", (d) => { peerGot += d; }); s.on("end", () => { peerEnded = true; s.end(); }); }));
		const port = peer.address().port;
		const s = await api.connect({ hostname: H, port, data: { k: 1 }, socket: { binaryType: "uint8array", open(s) { log.push(["open data", JSON.stringify(s.data)]); }, data(s, d) { log.push(["data", d.constructor.name]); }, close() { log.push("close"); } } });
		s.write("abc");
		log.push(["end", s.end()]);
		log.push(["after end readyState", s.readyState, "write", s.write("z")]);
		await waitFor(() => peerEnded && log.includes("close"), 3000, "ended");
		return { log, peerGot };
	});

	add("connect: refused -> promise rejects (no connectError)", async () => {
		const port = await freePort();
		const log = mkLog();
		let e;
		try {
			await api.connect({ hostname: H, port, socket: { data() {}, open() { log.push("open"); }, error(s, er) { log.push(["error", er?.code]); }, close() { log.push("close"); } } });
			e = "resolved";
		} catch (er) { e = errShape(er, port); }
		await sleep(50);
		return { e, log };
	});

	add("connect: refused with connectError handler", async () => {
		const port = await freePort();
		const log = mkLog();
		let e;
		try {
			await api.connect({ hostname: H, port, socket: { data() {}, connectError(s, er) { log.push(["connectError", er?.code, typeof s]); }, error(s, er) { log.push(["error", er?.code]); }, close() { log.push("close"); } } });
			e = "resolved";
		} catch (er) { e = errShape(er, port); }
		await sleep(50);
		return { e, log };
	});

	add("connect: ENOTFOUND", async () => {
		const log = mkLog();
		let e;
		try { await api.connect({ hostname: "no-such-host.invalid", port: 9, socket: { data() {}, error(s, er) { log.push(["error", er?.code]); } } }); e = "resolved"; } catch (er) { e = errShape(er); }
		return { e, log };
	});

	add("connect: E_ ::1 probe (open then end) and localhost resolution", async (t) => {
		const E_ = async (h, p) => { try { return (await api.connect({ hostname: h, port: p, socket: { data() {}, open() {}, error() {}, close() {} } })).end(), true; } catch { return false; } };
		const v6 = track(t, await peerServer((s) => s.on("data", () => {}), "::1"));
		const p6 = v6.address().port;
		const yes = await E_("::1", p6);
		const v4 = track(t, await peerServer((s) => s.resume(), H, p6).catch(() => null));
		let addr = null;
		if (v4) {
			const s = await api.connect({ hostname: "localhost", port: p6, socket: { data() {}, close() {} } });
			addr = s.remoteAddress;
			s.end();
		}
		await closeServer(v6);
		const no = await E_("::1", p6);
		return { yes, no, localhostRemote: addr, dualBound: !!v4 };
	});

	add("connect: Bun.connect to Bun.listen (relay direct-dial shape)", async (t) => {
		const log = mkLog();
		const L = track(t, api.listen({ hostname: H, port: 0, socket: { open(s) { log.push("srv open"); }, data(s, d) { log.push("srv data " + d); s.write("R:" + d); }, close() { log.push("srv close"); } } }));
		const up = await api.connect({ hostname: H, port: L.port, socket: { open(u) { log.push(["cli open", u.remoteAddress]); }, data(u, p) { log.push("cli data " + p); u.end(); }, drain() {}, close() { log.push("cli close"); }, error(u, p) { log.push("cli error " + p?.message); } } });
		log.push(["write", up.write("q")]);
		await waitFor(() => log.includes("cli close") && log.includes("srv close"), 3000, "closes");
		return { log: log.sort((a, b) => JSON.stringify(a) < JSON.stringify(b) ? -1 : 1) };
	});

	return S;
}

// Spawns net-child.mjs in the same runtime, holds a connection open against what
// it serves, and reports whether the child exits on its own within 2.5 s.
async function childExit(env, mode) {
	const child = spawn(process.execPath, [...env.childArgs, new URL("./net-child.mjs", import.meta.url).pathname, mode], { stdio: ["ignore", "pipe", "pipe"] });
	let out = "", errOut = "";
	child.stderr.on("data", (d) => { errOut += d; });
	const exited = new Promise((r) => child.on("exit", (code, sig) => r({ code, sig })));
	const port = await within(new Promise((r, j) => { child.stdout.on("data", (d) => { out += d; const m = /PORT (\d+)/.exec(out); if (m) r(+m[1]); }); child.on("exit", () => j(new Error("child exited early: " + errOut.slice(0, 200)))); }), 5000, "child port");
	let sock = null;
	if (mode === "serve-unref-keepalive" || mode === "serve-ref") {
		const r = await httpRaw({ port, raw: GET("/") });
		sock = r.sock;
	} else if (mode === "listen-unref-conn") {
		sock = net.connect({ port, host: "127.0.0.1" });
		const ok = await new Promise((r) => { sock.on("connect", () => r(true)); sock.on("error", () => r(false)); });
		if (!ok) return { connectFailed: true };
		sock.write("x");
	}
	const res = await Promise.race([exited, sleep(2500).then(() => "still running")]);
	if (res === "still running") { child.kill("SIGKILL"); await exited; }
	sock?.destroy();
	return { exited: res === "still running" ? false : res.code, stderr: errOut.trim().slice(0, 120) };
}

export async function runAll(api, env) {
	const results = [];
	for (const [name, fn] of scenarios(api, env)) {
		const tracked = [];
		let obs;
		try { obs = await within(fn(tracked), 60000, name); } catch (e) { obs = { THREW: String(e && e.message) }; }
		stopAll(tracked);
		await sleep(20);
		results.push([name, JSON.parse(JSON.stringify(obs, (k, v) => (v === undefined ? "__undef" : typeof v === "bigint" ? String(v) : v)))]);
	}
	return results;
}
