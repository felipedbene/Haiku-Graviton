// Bun 1.4.4's fetch() extensions and Bun.FetchSession, on Node's own fetch.
//
// Bun sources consulted, tag bun-v1.4.4 (rev 38afee6c84fc):
//   src/runtime/webcore/fetch.rs (option extraction, order and errors)
//   src/runtime/webcore/fetch/{FetchSession.rs,FetchSession.classes.ts,FetchTasklet.rs,compress_body.rs}
//   src/runtime/socket/SSLConfig.bind.ts (the tls dictionary)
//   src/http/lib.rs (ProxySettings, CONNECT and absolute-form writers, idle timeout)
//   src/dotenv/{env_loader.rs,no_proxy.rs} (proxy environment, NO_PROXY matching)
// Behaviour was then pinned down against stock Bun with compat/test/fetch/.
//
// Node's fetch accepts Bun's extra init keys and silently ignores them, so a
// `proxy` would be bypassed and a `unix` socket or `tls.ca` ignored with no
// error. This module wraps globalThis.fetch: a request that carries a Bun option
// (or that Bun would send through an environment proxy) gets an undici
// dispatcher that implements it; any other request goes to Node's fetch
// untouched. Responses are always Node's own global Response, so instanceof
// checks hold.
//
// The dispatcher classes come from the npm "undici" package installed next to
// this file (tools/deps.lock pins the version whose major matches Node 24's
// bundled undici, so Node's fetch drives them through the same handler API).
// It is loaded on first use, not at startup.
//
// Options Bun accepts that this layer cannot reproduce throw "<what> is not
// supported by the Node compat layer" (a rejection, as Bun's own option errors
// are); none is ever ignored. LIMITATIONS.md lists them and every measured
// difference that remains.
import { createRequire } from "node:module";
import net from "node:net";
import tls from "node:tls";
import { readFileSync } from "node:fs";
import { createHash, randomBytes } from "node:crypto";
import { isAbsolute, join } from "node:path";

const require = createRequire(import.meta.url);
let undici;
const U = () => (undici ??= require("undici"));

// ---- errors -------------------------------------------------------------------

function codeError(Ctor, code, message) {
	const e = new Ctor(message);
	Object.defineProperty(e, "code", { value: code, enumerable: false, writable: true, configurable: true });
	return e;
}
const invalidType = (m) => codeError(TypeError, "ERR_INVALID_ARG_TYPE", m);
const invalidValue = (m) => codeError(TypeError, "ERR_INVALID_ARG_VALUE", m);
const notSupported = (what) => codeError(TypeError, "ERR_NOT_SUPPORTED_BY_NODE_COMPAT", `fetch: ${what} is not supported by the Node compat layer`);
function received(v) {
	if (v === null) return "Received null";
	if (v === undefined) return "Received undefined";
	if (typeof v === "function") return `Received function ${v.name || "<anonymous>"}`;
	if (typeof v === "object") return `Received an instance of ${(v.constructor && v.constructor.name) || "Object"}`;
	return `Received type ${typeof v} (${typeof v === "string" ? `'${v}'` : String(v)})`;
}
const invalidProxy = (v) => invalidType(`The "proxy" argument must be a string, a URL, an object with a "url", or false. ${received(v)}`);

// Errors this layer raises inside a request (a refused CONNECT, a value returned
// by tls.checkServerIdentity) reach the caller as Bun delivers them: as the
// rejection itself, not as the cause of Node's TypeError("fetch failed").
const OWN = new WeakSet();
const own = (e) => { if (e instanceof Object) OWN.add(e); return e; };
function unwrap(e) {
	for (let x = e, i = 0; x instanceof Object && i < 4; x = x.cause, i++) if (OWN.has(x)) return x;
	return e;
}

// ---- proxy environment (env_loader.rs), read live: Bun's process.env writes reach it ----

const env = process.env;
const emptyish = (v) => v === "" || v === '""' || v === "''";
function lowerThenUpper(lo, up) {
	const p = env[lo];
	return p !== undefined && p !== "" ? p : env[up];
}
function schemeProxy(isHttp) {
	const v = isHttp ? lowerThenUpper("http_proxy", "HTTP_PROXY") : lowerThenUpper("https_proxy", "HTTPS_PROXY");
	return v === undefined || emptyish(v) ? undefined : v;
}
// all_proxy often names a SOCKS proxy, which Bun's client cannot speak: only a
// value without a scheme, or with http:/https:, is used.
function allProxy() {
	const v = lowerThenUpper("all_proxy", "ALL_PROXY");
	if (v === undefined || emptyish(v)) return undefined;
	const m = /^([^:/?#]*):\/\//.exec(v);
	return !m || /^https?$/i.test(m[1]) ? v : undefined;
}
function noProxyList() {
	const r = (n) => { const v = env[n]; return v === undefined || emptyish(v) ? undefined : v; };
	return r("no_proxy") ?? r("NO_PROXY") ?? "";
}
function envProxySettings() {
	let http = schemeProxy(true), https = schemeProxy(false);
	if (!(http && https)) { const all = allProxy(); http ??= all; https ??= all; }
	return http || https ? { http: http || "", https: https || "", noProxy: noProxyList() } : null;
}
const envHasProxy = () => envProxySettings() !== null;

// ---- NO_PROXY matching (no_proxy.rs) -------------------------------------------------

function ipBytes(text) {	// strict dotted quad or IPv6; ::ffff:a.b.c.d folds to IPv4
	if (net.isIPv4(text)) return { v4: true, b: text.split(".").map(Number) };
	if (!net.isIPv6(text) || text.includes("%")) return null;
	let s = text, tail = [];
	const lastColon = s.lastIndexOf(":");
	if (s.slice(lastColon + 1).includes(".")) {
		tail = s.slice(lastColon + 1).split(".").map(Number);
		s = s.slice(0, lastColon + 1) + "0:0";
	}
	const [hd, tl] = s.includes("::") ? s.split("::") : [s, null];
	const head = hd ? hd.split(":") : [];
	const rest = tl === null ? [] : tl ? tl.split(":") : [];
	const groups = tl === null ? head : [...head, ...Array(8 - head.length - rest.length).fill("0"), ...rest];
	const b = [];
	for (const g of groups) { const n = parseInt(g, 16); b.push(n >> 8, n & 255); }
	if (tail.length) b.splice(12, 4, ...tail);
	if (b.slice(0, 10).every((x) => x === 0) && b[10] === 255 && b[11] === 255) return { v4: true, b: b.slice(12) };
	return { v4: false, b };
}
const stripBrackets = (h) => (h.startsWith("[") && h.endsWith("]") ? h.slice(1, -1) : h);
const stripDot = (h) => (h.endsWith(".") ? h.slice(0, -1) : h);
const digits = (t) => (/^[0-9]+$/.test(t) ? Number(t) : null);
function splitPort(entry) {
	if (entry.startsWith("[")) {
		const c = entry.indexOf("]");
		if (c >= 0) { const rest = entry.slice(c + 1); return [entry.slice(1, c), rest.startsWith(":") ? rest.slice(1) : null]; }
		return [entry, null];
	}
	const n = entry.split(":").length - 1;
	if (n === 1) { const c = entry.indexOf(":"); return [entry.slice(0, c), entry.slice(c + 1)]; }
	return [entry, null];
}
function cidrContains(entry, slash, host) {
	const written = ipBytes(stripBrackets(entry.slice(0, slash)));
	let bits = digits(entry.slice(slash + 1));
	if (!written || bits === null || bits > 255) return false;
	if (!net.isIPv4(stripBrackets(entry.slice(0, slash))) && written.v4) {
		if (bits < 96) return false;
		bits -= 96;
	}
	if (written.v4 !== host.v4 || bits > written.b.length * 8) return false;
	const whole = bits >> 3, rem = bits & 7;
	for (let i = 0; i < whole; i++) if (written.b[i] !== host.b[i]) return false;
	return rem === 0 || ((written.b[whole] ^ host.b[whole]) >> (8 - rem)) === 0;
}
export function noProxyMatches(list, hostname, port) {
	const host = stripDot(stripBrackets(hostname));
	if (!host) return false;
	const entries = list.split(/[, \t\r\n]+/).filter(Boolean);
	if (!entries.length) return false;
	const hostIp = ipBytes(host);
	const lc = host.toLowerCase();
	for (const entry of entries) {
		if (entry === "*") return true;
		const slash = entry.indexOf("/");
		if (slash >= 0) { if (hostIp && cidrContains(entry, slash, hostIp)) return true; continue; }
		let [eh, ep] = splitPort(entry);
		if (ep !== null && digits(ep) !== port) continue;
		eh = eh.startsWith("*.") ? eh.slice(2) : eh.startsWith(".") ? eh.slice(1) : eh;
		eh = stripDot(eh);
		if (!eh) continue;
		if (hostIp) {
			const ei = ipBytes(eh);
			if (ei && ei.v4 === hostIp.v4 && ei.b.every((x, i) => x === hostIp.b[i])) return true;
			continue;
		}
		const e = eh.toLowerCase();
		if (lc === e || (lc.length > e.length && lc.endsWith(e) && lc[lc.length - e.length - 1] === ".")) return true;
	}
	return false;
}

// ---- proxy policy (FetchSession.rs parse_proxy, lib.rs ProxySettings) --------------------

function proxyHref(v) {
	const s = v instanceof URL ? v.href : typeof v === "string" ? v : null;
	if (s === null || !URL.canParse(s)) throw invalidValue("fetch() proxy URL is invalid");
	return new URL(s).href;
}
// -> undefined (absent), "unusable", { direct: true } or { href, headers, respectNoProxy }
function parseProxy(v) {
	if (typeof v === "boolean") { if (v) throw invalidProxy(v); return { direct: true }; }
	if (v instanceof URL || (typeof v === "string" && v.length > 0)) return { href: proxyHref(v), headers: null, respectNoProxy: true };
	if (v === undefined || v === null || typeof v === "string") return undefined;
	if (typeof v !== "object") return "unusable";
	const url = v.url;
	if (url === undefined || url === null) return "unusable";
	const href = proxyHref(url);
	let headers = null;
	if (v.headers !== undefined && v.headers !== null) headers = new Headers(v.headers);
	const r = v.respectNoProxy;
	return { href, headers, respectNoProxy: typeof r === "boolean" ? r : true };
}
const portOf = (u) => (u.port ? Number(u.port) : u.protocol === "https:" ? 443 : 80);
function resolveProxy(settings, url) {
	const href = url.protocol === "http:" ? settings.http : settings.https;
	if (!href || noProxyMatches(settings.noProxy, url.hostname, portOf(url))) return null;
	return href;
}
// A proxy value with no scheme is an HTTP proxy, as curl reads HTTP_PROXY.
function proxyURL(href) {
	const u = new URL(/^[a-z][a-z0-9+.-]*:\/\//i.test(href) ? href : "http://" + href);
	if (u.protocol !== "http:" && u.protocol !== "https:") throw notSupported(`a ${u.protocol} proxy`);
	return u;
}

// ---- tls (SSLConfig.bind.ts) -------------------------------------------------------------

// Keys whose non-default values cannot be given to Node's TLS stack with Bun's meaning.
const TLS_REFUSED = {
	dhParamsFile: null, lowMemoryMode: false, requestCert: false, secureOptions: 0, minVersion: 0, maxVersion: 0,
	ALPNProtocols: null, clientRenegotiationLimit: 0, clientRenegotiationWindow: 0, crl: null,
	allowPartialTrustChain: false, sessionTimeout: 0, sigalgs: null, ecdhCurve: null,
};
function fileValue(name, v) {
	if (v === undefined || v === null) return undefined;
	const one = (x) => {
		if (typeof x === "string" || Buffer.isBuffer(x)) return x;
		if (ArrayBuffer.isView(x)) return Buffer.from(x.buffer, x.byteOffset, x.byteLength);
		if (x instanceof ArrayBuffer) return Buffer.from(x);
		if (typeof Blob !== "undefined" && x instanceof Blob) throw notSupported(`a Blob or Bun.file() as tls.${name}`);
		throw invalidType(`TLSOptions.${name} must be a string, a BufferSource, a Blob, or an array of those`);
	};
	return Array.isArray(v) ? v.map(one) : one(v);
}
const digests = new WeakMap();
function feed(h, v) {
	if (v === undefined) h.update("u");
	else if (typeof v === "string") { h.update(`s${v.length}:`); h.update(v); }
	else if (Array.isArray(v) || Buffer.isBuffer(v)) {
		let d = digests.get(v);
		if (d === undefined) {
			const hh = createHash("sha256");
			if (Array.isArray(v)) { hh.update(`a${v.length}:`); for (const x of v) feed(hh, x); } else hh.update(v);
			digests.set(v, (d = hh.digest("hex")));
		}
		h.update(d);
	} else h.update(`v${String(v)}`);
}
// -> { opts: node tls connect options, key: pool key, check: user checkServerIdentity }
function parseTLS(t) {
	let check;
	const csi = t.checkServerIdentity;
	if (csi !== undefined) {
		if (typeof csi === "function") check = csi;
		else if (csi !== null) throw invalidType(`The "tls.checkServerIdentity" property must be of type function. Received ${typeof csi}`);
	}
	for (const [k, d] of Object.entries(TLS_REFUSED)) {
		const v = t[k];
		if (v !== undefined && v !== null && v !== d && !(d === 0 && Number(v) === 0)) throw notSupported(`tls.${k}`);
	}
	const opts = {};
	const ru = t.rejectUnauthorized;
	if (ru !== undefined && ru !== null) {
		if (typeof ru !== "boolean") throw invalidType("TLSOptions.rejectUnauthorized must be a boolean");
		opts.rejectUnauthorized = ru;
	}
	const sn = t.serverName ?? t.servername;
	if (sn !== undefined && sn !== null) {
		const s = String(sn);
		if (s.includes("\0")) throw invalidValue('"serverName" must not contain null bytes');
		opts.servername = s;
	}
	for (const k of ["passphrase", "ciphers"]) if (t[k] !== undefined && t[k] !== null) opts[k] = String(t[k]);
	for (const k of ["ca", "cert", "key"]) {
		const v = fileValue(k, t[k]);
		const f = t[k + "File"];
		if (f !== undefined && f !== null) {
			if (v !== undefined) throw notSupported(`tls.${k} together with tls.${k}File`);
			opts[k] = readFileSync(String(f));
		} else if (v !== undefined) opts[k] = v;
	}
	const h = createHash("sha256");
	for (const k of ["ca", "cert", "key", "passphrase", "ciphers", "servername", "rejectUnauthorized"]) { h.update(k); feed(h, opts[k]); }
	return { opts, key: h.digest("hex"), check };
}
// Node calls checkServerIdentity only after the chain verified, as Bun does. A
// returned (or thrown) error rejects the fetch with that error itself.
function wrapCheck(check) {
	return (host, cert) => {
		let r;
		try { r = check(host, cert); } catch (e) { return own(e); }
		return r instanceof Error ? own(r) : r;
	};
}

// ---- idle timeout (lib.rs) -----------------------------------------------------------------

// Bun pads an idle timeout by one timer-wheel period so it never fires early.
function normalizeIdle(raw) {
	if (raw === 0) return 0;
	raw = Math.min(raw, 238 * 60);
	return raw + 4 > 240 ? (Math.ceil(raw / 60) + 1) * 60 : raw + 4;
}
const IDLE_ENV = env.BUN_CONFIG_HTTP_IDLE_TIMEOUT;
const IDLE_ENV_N = IDLE_ENV !== undefined && /^[0-9]+$/.test(IDLE_ENV.trim()) ? Number(IDLE_ENV.trim()) : undefined;
const DEFAULT_IDLE_S = normalizeIdle(IDLE_ENV_N ?? 300);

// ---- dispatchers -----------------------------------------------------------------------------

// Pools: one map for plain fetch() calls, one per FetchSession, as Bun keys its
// keep-alive pool by session. Values are undici Agents keyed by every setting
// that makes a connection unshareable.
const GLOBAL_POOL = new Map();
function pooled(pool, key, make) {
	let a = pool.get(key);
	if (!a) pool.set(key, (a = make()));
	return a;
}

// Bun's CONNECT request, byte for byte: Host and Proxy-Connection unless the
// caller's proxy headers name them, Proxy-Authorization from the URL's
// credentials unless they name it, then every proxy header.
function proxyAuth(p) {
	if (!p.username && !p.password) return null;
	return "Basic " + Buffer.from(`${decodeURIComponent(p.username)}:${decodeURIComponent(p.password)}`).toString("base64");
}
function connectRequest(host, port, p, headers) {
	const has = (n) => headers !== null && headers.has(n);
	let s = `CONNECT ${host}:${port} HTTP/1.1\r\n`;
	if (!has("host")) s += `Host: ${host}:${port}\r\n`;
	if (!has("proxy-connection")) s += "Proxy-Connection: Keep-Alive\r\n";
	const auth = proxyAuth(p);
	if (auth && !has("proxy-authorization")) s += `Proxy-Authorization: ${auth}\r\n`;
	if (headers) for (const [k, v] of headers) s += `${k}: ${v}\r\n`;
	return s + "\r\n";
}
function tunnelError(status, statusText, pairs) {
	const e = codeError(Error, "ERR_PROXY_TUNNEL", `Proxy refused to open a tunnel: ${status} ${statusText}`);
	e.status = status;
	e.statusText = statusText;
	e.headers = new Headers(pairs);
	return own(e);
}
// Bun's idle timeout rejects with the DOMException AbortSignal.timeout() uses.
function timeoutError() {
	return own(new DOMException("The operation timed out.", "TimeoutError"));
}
const UNDICI_TIMEOUTS = new Set(["UND_ERR_HEADERS_TIMEOUT", "UND_ERR_BODY_TIMEOUT", "UND_ERR_CONNECT_TIMEOUT"]);
function connectToProxy(p, plan) {
	const port = Number(p.port) || (p.protocol === "https:" ? 443 : 80);
	const host = stripBrackets(p.hostname);
	if (p.protocol === "https:") {
		return tls.connect({ ...plan.tls.opts, servername: net.isIP(host) ? undefined : host, host, port, ALPNProtocols: ["http/1.1"] });
	}
	return net.connect({ host, port });
}
// undici `connect` function for https targets behind a proxy: CONNECT, then TLS
// to the origin inside the tunnel with the request's tls options.
function tunnelConnector(p, proxyHeaders, plan) {
	return (c, cb) => {
		const port = c.port || (c.protocol === "https:" ? 443 : 80);
		let done = false, timer;
		const sock = connectToProxy(p, plan);
		const finish = (err, s) => {
			if (done) return;
			done = true;
			clearTimeout(timer);
			if (err) sock.destroy();
			cb(err, s);
		};
		if (plan.idleMs) timer = setTimeout(() => finish(timeoutError()), plan.idleMs);
		sock.once(p.protocol === "https:" ? "secureConnect" : "connect", () => sock.write(connectRequest(c.hostname, port, p, proxyHeaders)));
		sock.on("error", (e) => finish(e));
		sock.once("close", () => finish(own(codeError(Error, "ECONNRESET", "The proxy closed the connection before answering CONNECT"))));
		let buf = Buffer.alloc(0);
		const onData = (d) => {
			buf = Buffer.concat([buf, d]);
			const end = buf.indexOf("\r\n\r\n");
			if (end < 0) { if (buf.length > 65536) finish(tunnelError(0, "unreadable reply", [])); return; }
			sock.removeListener("data", onData);
			sock.pause();
			const lines = buf.subarray(0, end).toString("latin1").split("\r\n");
			const m = /^HTTP\/1\.[01] (\d{3})(?: (.*))?$/.exec(lines[0]);
			const pairs = lines.slice(1).map((l) => { const i = l.indexOf(":"); return [l.slice(0, i).trim(), l.slice(i + 1).trim()]; }).filter(([k]) => k);
			if (!m) return finish(tunnelError(0, "unreadable reply", []));
			const status = Number(m[1]);
			if (status < 200 || status > 299) return finish(tunnelError(status, m[2] || "", pairs));
			const rest = buf.subarray(end + 4);
			if (rest.length) sock.unshift(rest);
			sock.removeAllListeners("close");
			if (c.protocol !== "https:") { sock.resume(); return finish(null, sock); }
			const name = c.servername || plan.tls.opts.servername || (net.isIP(stripBrackets(c.hostname)) ? undefined : c.hostname);
			const ts = tls.connect({ ...plan.tls.opts, ...(plan.check && { checkServerIdentity: plan.check }), socket: sock, servername: name, ALPNProtocols: ["http/1.1"] });
			ts.once("secureConnect", () => finish(null, ts));
			ts.once("error", (e) => finish(e));
		};
		sock.on("data", onData);
	};
}
function agentFor(plan, kind, p, proxyHeaders) {
	const make = () => {
		if (kind === "tunnel") return new (U().Agent)({ connect: tunnelConnector(p, proxyHeaders, plan) });
		// "forward" reaches an http: origin through the proxy, so its only TLS
		// (an https: proxy) is to the proxy: the origin's name check is not for it.
		const connect = { ...plan.tls.opts, timeout: plan.idleMs };
		if (kind === "forward") delete connect.servername;
		else if (plan.check) connect.checkServerIdentity = plan.check;
		if (kind === "direct" && plan.unix) connect.socketPath = plan.unix;
		return new (U().Agent)({ connect });
	};
	// A per-request checkServerIdentity closure has no identity a pool could key
	// on: Bun bypasses the pool for it, and so does this (the agent is closed when
	// the response ends).
	if (plan.ownCheck) return { agent: make(), oneShot: true };
	const key = JSON.stringify([kind, plan.tls.key, plan.unix, plan.idleMs, p ? p.href : "", proxyHeaders ? [...proxyHeaders] : null, !!plan.check]);
	return { agent: pooled(plan.pool, key, make), oneShot: false };
}
function flatHeaders(h) {
	if (!h) return [];
	if (Array.isArray(h)) return Array.isArray(h[0]) ? h.flat() : h.slice();
	return Object.entries(h).flat();
}
// Bun's absolute-form request to a proxy for an http: target: Proxy-Connection,
// then Proxy-Authorization (URL credentials, unless the proxy headers name it),
// then the proxy headers, then the request's own.
function forwardOpts(opts, url, p, proxyHeaders) {
	const extra = ["Proxy-Connection", "Keep-Alive"];
	const auth = proxyAuth(p);
	if (auth && !(proxyHeaders && proxyHeaders.has("proxy-authorization"))) extra.push("Proxy-Authorization", auth);
	if (proxyHeaders) for (const [k, v] of proxyHeaders) extra.push(k, v);
	const mine = flatHeaders(opts.headers);
	if (!mine.some((k, i) => i % 2 === 0 && String(k).toLowerCase() === "host")) mine.push("host", url.host);
	return { ...opts, origin: p.origin, path: url.origin + opts.path, headers: [...extra, ...mine] };
}
// Two things Node's fetch would do to a response that Bun does not are kept
// from it by renaming on the way in, and undone by restore():
//   * decompress:false: the Content-Encoding header is renamed, so Node's fetch
//     finds nothing to decode;
//   * a 407 (a forwarding proxy asking for credentials): Node's fetch turns any
//     407 into a network error, Bun returns it; it travels as 299 plus a marker.
const MARK = randomBytes(6).toString("hex");
const RAW_CE = "x-compat-raw-content-encoding-" + MARK, RAW_CE_BUF = Buffer.from(RAW_CE);
const WAS_407 = "x-compat-status-407-" + MARK, WAS_407_BUF = Buffer.from(WAS_407);
// Wraps fetch's (legacy-API) dispatch handler for one hop.
function wrapHandler(handler, plan, agent) {
	const w = Object.create(handler);
	w.onHeaders = function (status, raw, resume, statusText) {
		if (!plan.decompress) for (let i = 0; i < raw.length; i += 2) if (String(raw[i]).toLowerCase() === "content-encoding") raw[i] = RAW_CE_BUF;
		if (status === 407) { raw.push(WAS_407_BUF, Buffer.from("1")); status = 299; }
		return handler.onHeaders.call(this, status, raw, resume, statusText);
	};
	// A one-shot agent (per-request checkServerIdentity) closes with its response.
	const done = () => { if (agent) agent.close().catch(() => {}); };
	w.onComplete = function (...a) { done(); return handler.onComplete.apply(this, a); };
	w.onError = function (err) {
		done();
		if (err && UNDICI_TIMEOUTS.has(err.code)) err = timeoutError();
		return handler.onError.call(this, err);
	};
	return w;
}
function routeFor(plan) {
	return {
		dispatch(opts, handler) {
			try {
				const url = new URL(opts.origin);
				opts = { ...opts, headersTimeout: plan.idleMs, bodyTimeout: plan.idleMs };
				// Node's fetch drops a caller's Host header; Bun sends it (the gateway's
				// pinned-address fetch relies on that). Only on the hop to the URL's own origin.
				if (plan.host !== null && url.origin === plan.origin) {
					const hs = flatHeaders(opts.headers).filter((x, i, a) => (i % 2 === 0 ? String(x).toLowerCase() !== "host" : String(a[i - 1]).toLowerCase() !== "host"));
					opts.headers = ["host", plan.host, ...hs];
				}
				let pick;
				let href = null, proxyHeaders = null;
				if (plan.proxy) {
					href = resolveProxy(plan.proxy, url);
					proxyHeaders = plan.proxy.headers;
				}
				if (href) {
					const p = proxyURL(href);
					if (url.protocol === "http:") {
						pick = agentFor(plan, "forward", p, null);
						opts = forwardOpts(opts, url, p, proxyHeaders);
					} else pick = agentFor(plan, "tunnel", p, proxyHeaders);
				} else pick = agentFor(plan, "direct", null, null);
				// Bun does not reuse a connection it will not pool, or one keepalive:false asked for.
				if (!plan.keepAlive || pick.oneShot) opts.reset = true;
				return pick.agent.dispatch(opts, wrapHandler(handler, plan, pick.oneShot ? pick.agent : null));
			} catch (e) {
				handler.onError(own(e));
				return false;
			}
		},
	};
}

// ---- option extraction (fetch.rs order) ----------------------------------------------------------

const BUN_KEYS = ["proxy", "unix", "tls", "timeout", "keepalive", "decompress", "session", "verbose", "s3", "compress", "maxRedirects", "protocol"];
const toInt32 = (n) => n | 0;
// -> null when nothing Bun-specific applies to this request, else the plan.
function planFor(init, bound, host) {
	const o = init !== null && typeof init === "object" ? init : null;
	const g = (k) => (o ? o[k] : undefined);
	let decompress = true;
	{ const v = g("decompress"); if (typeof v === "boolean") decompress = v; else if (typeof v === "number") decompress = toInt32(v) !== 0; }
	{
		const v = g("compress");
		if (v !== undefined && v !== null && v !== false) {
			if (typeof v === "string" && !["gzip", "deflate", "br", "zstd"].includes(v)) throw invalidType(`fetch: 'compress' must be "gzip", "deflate", "br", or "zstd"`);
			throw notSupported("the 'compress' option");
		}
	}
	{
		const v = g("maxRedirects");
		if (v !== undefined && v !== null) {
			if (typeof v !== "number" || !(v >= 0) || v % 1 !== 0) throw invalidType("fetch: 'maxRedirects' must be a non-negative integer");
			throw notSupported("the 'maxRedirects' option");
		}
	}
	let session = bound;
	if (!session) {
		const v = g("session");
		if (v !== undefined && v !== null) {
			if (!SESSIONS.has(v)) throw invalidType("fetch: 'session' must be a Bun.FetchSession");
			session = v;
		}
	}
	const ss = session ? SESSIONS.get(session) : null;
	let tlsP = null, requestHasTLS = false;
	{ const v = g("tls"); if (v !== null && typeof v === "object") { requestHasTLS = true; tlsP = parseTLS(v); } }
	if (ss && !requestHasTLS) tlsP = ss.tls;
	let unix = "";
	{ const v = g("unix"); if (typeof v === "string" && v.length) unix = v.startsWith("\0") || isAbsolute(v) ? v : join(process.cwd(), v); }
	const requestHasUnix = unix !== "";
	{
		const v = g("protocol");
		if (typeof v === "string") {
			if (v === "http2" || v === "h2" || v === "http3" || v === "h3") throw notSupported(`protocol "${v}"`);
			if (v !== "http1.1" && v !== "h1") throw invalidType(`fetch: 'protocol' must be "http2", "h2", "http3", "h3", "http1.1", or "h1"`);
		}
	}
	let idleS = DEFAULT_IDLE_S;
	{
		const v = g("timeout");
		if (typeof v === "boolean") { if (!v) idleS = 0; }
		else if (typeof v === "number") {
			if (Number.isFinite(v) && v > 0) idleS = normalizeIdle(Math.min(Math.ceil(v / 1000), 0xffffffff));
			if (!Number.isFinite(v) || toInt32(v) === 0) idleS = 0;
		}
	}
	let keepAlive = !(ss && !ss.keepAlive);
	{ const v = g("keepalive"); if (typeof v === "boolean") keepAlive = v; else if (typeof v === "number") keepAlive = toInt32(v) !== 0; }
	{
		const v = g("verbose");
		if (v === true || v === "curl") throw notSupported("the 'verbose' option");
	}
	let proxyOpt;
	{ const v = g("proxy"); if (o && "proxy" in o) { const r = parseProxy(v); if (r !== "unusable") proxyOpt = r; } }
	if (ss) {
		if (!requestHasUnix && !(proxyOpt && proxyOpt.href)) unix = ss.unix;
		if (proxyOpt === undefined && ss.proxy && !(ss.proxy.href && requestHasUnix)) proxyOpt = ss.proxy;
	}
	if (proxyOpt && proxyOpt.href && unix) throw invalidValue("fetch() cannot use a proxy with a unix socket.");
	// Bun's ProxyPolicy: Direct, Explicit (NO_PROXY per hop unless respectNoProxy
	// is false) or Env (captured now, resolved per hop; none over a unix socket).
	let proxy = null;
	if (proxyOpt && proxyOpt.href) proxy = { http: proxyOpt.href, https: proxyOpt.href, noProxy: proxyOpt.respectNoProxy ? noProxyList() : "", headers: proxyOpt.headers };
	else if (!(proxyOpt && proxyOpt.direct) && !unix) { const e = envProxySettings(); if (e) proxy = { ...e, headers: null }; }

	const special = !!(o && BUN_KEYS.some((k) => o[k] !== undefined));
	if (!special && !session && !proxy && host === null && IDLE_ENV_N === undefined) return null;
	const check = tlsP && tlsP.check ? wrapCheck(tlsP.check) : (ss && ss.check) || null;
	return {
		tls: tlsP || { opts: {}, key: "" },
		check,
		ownCheck: !!(tlsP && tlsP.check && tlsP !== (ss && ss.tls)),
		unix, proxy, keepAlive, decompress, idleMs: idleS * 1000, host, origin: null,
		pool: ss ? ss.pool : GLOBAL_POOL,
	};
}

const STD_KEYS = ["method", "headers", "body", "referrer", "referrerPolicy", "mode", "credentials", "cache", "redirect", "integrity", "signal", "window", "priority", "duplex"];
function nodeInit(init) {
	if (init === null || typeof init !== "object") return {};
	const p = Object.getPrototypeOf(init);
	let out;
	if (p === Object.prototype || p === null) { out = { ...init }; for (const k of BUN_KEYS) delete out[k]; }
	else { out = {}; for (const k of STD_KEYS) if (init[k] !== undefined) out[k] = init[k]; }
	if (out.dispatcher !== undefined) throw notSupported("a 'dispatcher' together with Bun fetch options");
	return out;
}
// An idle timeout that fires while the body is read reaches the reader as
// Node's TypeError("terminated") around this layer's TimeoutError; Bun's reader
// gets the TimeoutError. Only responses with an idle timer armed are rewrapped.
function mapBodyErrors(body) {
	const reader = body.getReader();
	return new ReadableStream({
		async pull(c) {
			try { const { done, value } = await reader.read(); if (done) c.close(); else c.enqueue(value); } catch (e) { c.error(unwrap(e)); }
		},
		cancel(r) { return reader.cancel(r); },
	}, { highWaterMark: 0 });
}
function restore(res, plan) {
	const ce = res.headers.has(RAW_CE), was407 = res.headers.has(WAS_407), wrap = plan.idleMs > 0 && res.body !== null;
	if (!ce && !was407 && !wrap) return res;
	const h = new Headers();
	for (const [k, v] of res.headers) if (k !== "set-cookie" && k !== WAS_407) h.append(k === RAW_CE ? "content-encoding" : k, v);
	for (const c of res.headers.getSetCookie()) h.append("set-cookie", c);
	const status = was407 ? 407 : res.status;
	const nullBody = [101, 103, 204, 205, 304].includes(status);
	const out = new Response(nullBody ? null : wrap ? mapBodyErrors(res.body) : res.body, { status, statusText: res.statusText, headers: h });
	Object.defineProperty(out, "url", { value: res.url, enumerable: false, configurable: true });
	Object.defineProperty(out, "redirected", { value: res.redirected, enumerable: false, configurable: true });
	return out;
}
// The caller's Host header, if any (init headers win over a Request's, as in fetch()).
function hostOf(input, init) {
	const h = init !== null && typeof init === "object" ? init.headers : undefined;
	if (h !== undefined && h !== null) {
		if (h instanceof Headers) return h.get("host");
		if (typeof h[Symbol.iterator] === "function") {
			for (const pair of h) if (pair && String(pair[0]).toLowerCase() === "host") return String(pair[1]);
		} else if (typeof h === "object") {
			for (const k of Object.keys(h)) if (k.toLowerCase() === "host") return String(h[k]);
		}
		return null;
	}
	return input instanceof Request ? input.headers.get("host") : null;
}

// ---- fetch ----------------------------------------------------------------------------------

let nodeFetch;
function bunFetch(input, init, bound) {
	let plan;
	try {
		if (input instanceof URL || typeof input === "string") {
			if (/^s3:/i.test(String(input))) throw notSupported("an s3:// URL");
		}
		const host = hostOf(input, init);
		plan = planFor(init, bound, host);
		if (plan === null) {
			if (init !== null && typeof init === "object" && init.keepalive !== undefined) init = nodeInit(init);
			return nodeFetch(input, init);
		}
		const ni = nodeInit(init);
		ni.dispatcher = routeFor(plan);
		if (plan.host !== null) plan.origin = new URL(input instanceof Request ? input.url : String(input)).origin;
		return nodeFetch(input, ni).then((r) => restore(r, plan), (e) => { throw unwrap(e); });
	} catch (e) {
		return Promise.reject(e);
	}
}

// ---- Bun.FetchSession ----------------------------------------------------------------------------

const SESSIONS = new WeakMap();
const sessionFetch = new WeakMap();
function stateOf(self) {
	const s = self !== null && typeof self === "object" ? SESSIONS.get(self) : undefined;
	if (!s) throw codeError(TypeError, "ERR_INVALID_THIS", `Expected this to be instanceof FetchSession, but r${received(self).slice(1)}`);
	return s;
}
function FetchSession(options) {
	if (!new.target) throw codeError(TypeError, "ERR_ILLEGAL_CONSTRUCTOR", "FetchSession constructor cannot be invoked without 'new'");
	const st = { tls: null, check: null, proxy: null, unix: "", keepAlive: true, pool: new Map() };
	if (options !== undefined && options !== null) {
		if (typeof options !== "object") throw invalidType("FetchSession: options must be an object");
		const t = options.tls;
		if (t !== null && typeof t === "object") {
			st.tls = parseTLS(t);
			if (st.tls.check) st.check = wrapCheck(st.tls.check);
		} else if (t !== undefined && t !== null) throw invalidType("FetchSession: 'tls' must be an object");
		if ("proxy" in options) {
			const r = parseProxy(options.proxy);
			if (r === "unusable") throw invalidProxy(options.proxy);
			if (r !== undefined) st.proxy = r;
		}
		const u = options.unix;
		if (typeof u === "string" && u.length) st.unix = u.startsWith("\0") || isAbsolute(u) ? u : join(process.cwd(), u);
		else if (u !== undefined && u !== null) throw invalidType("FetchSession: 'unix' must be a non-empty string");
		if (st.unix && st.proxy && st.proxy.href) throw invalidType("FetchSession: cannot use a proxy with a unix socket");
		const ka = options.keepAlive;
		if (typeof ka === "boolean") st.keepAlive = ka;
		else if (ka !== null && typeof ka === "object") {
			for (const k of ["idleTimeout", "maxIdleSockets"]) {
				const v = ka[k];
				if (v === undefined) continue;
				if (k === "idleTimeout" && !(typeof v === "number" && Number.isFinite(v) && v > 0)) throw invalidType("FetchSession: 'keepAlive.idleTimeout' must be a positive number");
				throw notSupported(`FetchSession keepAlive.${k}`);
			}
		} else if (ka !== undefined && ka !== null) throw invalidType("FetchSession: 'keepAlive' must be a boolean or an object");
	}
	SESSIONS.set(this, st);
}
// Closes the session's idle connections; requests in flight finish, and the
// session stays usable (its next request opens a new connection).
function close() {
	const st = stateOf(this);
	for (const a of st.pool.values()) a.close().catch(() => {});
	st.pool.clear();
}
Object.defineProperty(FetchSession, "length", { value: 0 });
Object.defineProperty(FetchSession.prototype, "fetch", {
	enumerable: true, configurable: true,
	get() {
		stateOf(this);
		let f = sessionFetch.get(this);
		if (!f) {
			const self = this;
			f = function fetch(input) { return bunFetch(input, arguments[1], self); }.bind(this);
			sessionFetch.set(this, f);
		}
		return f;
	},
});
Object.defineProperty(FetchSession.prototype, "close", { value: close, enumerable: true, writable: true, configurable: true });
Object.defineProperty(FetchSession.prototype, Symbol.dispose, { value: function dispose() { return close.call(this); }, enumerable: false, writable: true, configurable: true });

export { FetchSession };

// Replaces globalThis.fetch with the Bun-option-aware wrapper, keeping Node's
// name and length.
export function installFetch(g = globalThis) {
	nodeFetch = g.fetch;
	const fetch = function fetch(input, init) { return bunFetch(input, init, null); };
	Object.defineProperty(fetch, "length", { value: nodeFetch.length });
	Object.defineProperty(g, "fetch", { value: fetch, writable: true, configurable: true, enumerable: true });
}
