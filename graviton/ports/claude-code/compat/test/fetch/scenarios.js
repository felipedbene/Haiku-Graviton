// fetch scenarios, run identically under stock Bun 1.4.4 (recording) and under
// Node 24 + compat (checking). Each returns JSON-able observations: what the
// caller sees (status, body, error shape) and what the fixture's servers and
// proxies saw (connections, request lines, CONNECT headers).
//
// Normalised (run-specific) values: ephemeral ports become <name> in strings
// (<h>, <hs>, <px>, ...), the unix socket path becomes <sock>, elapsed time
// is reported only as a bucket. Nothing else is normalised here; check.mjs
// documents the one comparison it does not make byte-for-byte.
//
// Scenarios named "refuse: ..." exercise options the Node compat layer cannot
// reproduce; Bun's result is recorded for reference and the checker requires
// the compat layer to throw its "not supported" error instead.
"use strict";
const net = require("net");
const tls = require("tls");
const { readFileSync } = require("fs");
const { join } = require("path");
const { spawn } = require("child_process");

let PORTS, CERTS;
const rd = (f) => readFileSync(join(CERTS, f), "utf8");
const PROXY_ENV = ["http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY"];
for (const k of PROXY_ENV) delete process.env[k];

function norm(s) {
	let out = String(s);
	if (PORTS) {
		out = out.split(PORTS.sock).join("<sock>");
		for (const [k, v] of Object.entries(PORTS)) if (typeof v === "number") out = out.split(String(v)).join(`<${k}>`);
	}
	return out;
}
const U = (srv, path = "/echo", host = "127.0.0.1") => `${srv === "h" || srv === "hk" || srv === "px" ? "http" : "https"}://${host}:${PORTS[srv]}${path}`;

function errShape(e) {
	if (!(e instanceof Object)) return { notAnObject: String(e) };
	const o = { name: e.name, message: norm(e.message), isError: e instanceof Error };
	for (const k of ["code", "status", "statusText"]) if (k in e) o[k] = e[k];
	if (e.headers && typeof e.headers.get === "function") o.proxyAuthenticate = e.headers.get("proxy-authenticate");
	if (e.cause instanceof Object) o.cause = { name: e.cause.name, code: e.cause.code, message: norm(e.cause.message) };
	return o;
}
async function resShape(r, { text = true } = {}) {
	// `type` is left out: Node's fetch says "basic" where Bun says "default", for
	// every response, with or without this layer.
	const o = {
		isResponse: r instanceof Response, status: r.status, redirected: r.redirected, url: norm(r.url),
		contentEncoding: r.headers.get("content-encoding"),
	};
	if (text) {
		const t = norm(await r.text());
		try { o.json = JSON.parse(t); } catch { o.text = t.length > 200 ? t.slice(0, 200) + "..." : t; }
	}
	return o;
}
// Calls fetch-like `f` and reports a synchronous throw, a rejection, or the response.
async function call(f, input, init, opts) {
	let p;
	try { p = f(input, init); } catch (e) { return { syncThrow: errShape(e) }; }
	if (!(p instanceof Promise)) return { notAPromise: typeof p };
	try { return { res: await resShape(await p, opts) }; } catch (e) { return { rejected: errShape(e) }; }
}
const F = (input, init, opts) => call(fetch, input, init, opts);

function ctl(cmd) {
	return new Promise((ok, fail) => {
		const s = net.connect(PORTS.ctl, "127.0.0.1");
		let b = "";
		s.setEncoding("utf8");
		s.on("data", (d) => { b += d; });
		s.on("end", () => ok(JSON.parse(b)));
		s.on("error", fail);
		s.write(cmd + "\n");
	});
}
const reset = () => ctl("reset");
async function seen(withOpen = false) {
	const l = await ctl("stats");
	return {
		conn: l.conn, ...(withOpen && { open: l.open }),
		proxy: l.proxy.map((p) => ({ ...p, url: norm(p.url), headers: p.headers.map(([k, v]) => [k, norm(v)]) })),
	};
}
async function withEnv(vars, fn) {
	const saved = {};
	for (const k of PROXY_ENV) { saved[k] = process.env[k]; delete process.env[k]; }
	for (const [k, v] of Object.entries(vars)) process.env[k] = v;
	try { return await fn(); } finally {
		for (const k of PROXY_ENV) { if (saved[k] === undefined) delete process.env[k]; else process.env[k] = saved[k]; }
	}
}
const pxURL = (name = "px", auth = "") => `http://${auth}127.0.0.1:${PORTS[name]}`;
const caTLS = () => ({ ca: rd("ca.pem") });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const bucket = (ms) => (ms < 1500 ? "<1.5s" : ms < 4500 ? "1.5-4.5s" : ms < 9000 ? "4.5-9s" : ">=9s");

// Runs `src` in a fresh process of this runtime (same preloads), with `env`.
function child(src, env) {
	return new Promise((ok) => {
		const c = spawn(process.execPath, [...process.execArgv, "-e", src], {
			env: { ...process.env, ...env }, stdio: ["ignore", "pipe", "pipe"],
		});
		let out = "", err = "";
		c.stdout.on("data", (d) => { out += d; });
		c.stderr.on("data", (d) => { err += d; });
		c.on("close", (code) => ok({ code, out: norm(out.trim()), err: err ? "stderr:" + err.length : "" }));
	});
}

const S = {};

// ---- surface -----------------------------------------------------------------
S["FetchSession surface"] = async () => {
	const FS = Bun.FetchSession;
	const proto = FS.prototype;
	const d = (k) => { const x = Object.getOwnPropertyDescriptor(proto, k); return x && { get: typeof x.get, value: typeof x.value, enumerable: x.enumerable, writable: x.writable }; };
	const s = new FS();
	const r = {
		typeofFS: typeof FS, name: FS.name, length: FS.length,
		protoNames: Object.getOwnPropertyNames(proto).sort(),
		hasDispose: typeof proto[Symbol.dispose],
		disposeIsClose: proto[Symbol.dispose] === proto.close,
		fetch: d("fetch"), close: d("close"), closeLength: proto.close.length,
		instance: s instanceof FS, fetchCached: s.fetch === s.fetch, fetchType: typeof s.fetch,
		fetchName: s.fetch.name, fetchLength: s.fetch.length, closeReturns: String(s.close()),
	};
	try { FS(); r.callWithoutNew = "no throw"; } catch (e) { r.callWithoutNew = errShape(e); }
	return r;
};
S["FetchSession constructor argument errors"] = async () => {
	const out = {};
	const cases = {
		number: 5, string: "x", tlsString: { tls: "x" }, proxyTrue: { proxy: true }, proxyNumber: { proxy: 7 },
		proxyObjNoUrl: { proxy: {} }, proxyBadUrl: { proxy: "not a url" }, unixEmpty: { unix: "" }, unixNumber: { unix: 3 },
		unixAndProxy: { unix: "/tmp/x.sock", proxy: "http://127.0.0.1:1" }, keepAliveString: { keepAlive: "yes" },
		nullOpts: null, undefinedOpts: undefined, proxyFalse: { proxy: false }, proxyEmpty: { proxy: "" },
	};
	for (const [k, v] of Object.entries(cases)) {
		try { new Bun.FetchSession(v); out[k] = "ok"; } catch (e) { out[k] = errShape(e); }
	}
	return out;
};
S["fetch option argument errors"] = async () => {
	const url = U("h");
	const out = {};
	const cases = {
		proxyTrue: { proxy: true }, proxyNumber: { proxy: 7 }, proxyObjNoUrl: { proxy: {} }, proxyBadUrl: { proxy: "::nope" },
		unixAndProxy: { unix: PORTS.sock, proxy: pxURL() }, sessionNotASession: { session: {} },
		tlsCheckNotFunction: { tls: { checkServerIdentity: 5 } },
	};
	for (const [k, v] of Object.entries(cases)) out[k] = await F(url, v);
	return out;
};

// ---- proxy ---------------------------------------------------------------------
S["proxy string, https target: CONNECT"] = async () => {
	await reset();
	const r = await F(U("hs", "/echo", "localhost"), { proxy: pxURL(), tls: caTLS(), headers: { "x-test": "1" } });
	return { r, seen: await seen() };
};
S["proxy {url, headers}, https target: CONNECT carries the headers"] = async () => {
	await reset();
	const r = await F(U("hs", "/echo", "localhost"), {
		proxy: { url: pxURL(), headers: { "Proxy-Authorization": "Basic dXNlcjpwYXNz", "X-Proxy-Extra": "e1" } }, tls: caTLS(),
	});
	return { r, seen: await seen() };
};
S["proxy {url, headers: Headers}, https target"] = async () => {
	await reset();
	const r = await F(U("hs", "/echo", "localhost"), { proxy: { url: new URL(pxURL()), headers: new Headers({ "Proxy-Authorization": "Bearer tok" }) }, tls: caTLS() });
	return { r, seen: await seen() };
};
S["proxy URL with userinfo: generated Proxy-Authorization"] = async () => {
	await reset();
	const a = await F(U("hs", "/echo", "localhost"), { proxy: pxURL("px", "us%40er:p%3Ass@"), tls: caTLS() });
	const b = await F(U("hs", "/echo", "localhost"), { proxy: { url: pxURL("px", "u:p@"), headers: { "proxy-authorization": "Basic b3ZlcnJpZGU=" } }, tls: caTLS() });
	return { a, b, seen: await seen() };
};
S["proxy string, http target: absolute-form forward"] = async () => {
	await reset();
	const r = await F(U("h", "/echo?q=1"), { proxy: { url: pxURL(), headers: { "Proxy-Authorization": "Basic Zm9yd2FyZA==" } }, headers: { "x-test": "fwd" } });
	return { r, seen: await seen() };
};
S["proxy refuses CONNECT with 407: error shape"] = async () => {
	await reset();
	const r = await F(U("hs", "/echo", "localhost"), { proxy: pxURL("px407"), tls: caTLS() });
	return { r, seen: await seen() };
};
S["proxy answers 407 to a forwarded http request: a response"] = async () => {
	await reset();
	const r = await F(U("h"), { proxy: pxURL("px407") });
	return { r, seen: await seen() };
};
S["proxy refuses an unknown target (403)"] = async () => {
	await reset();
	const r = await F("https://10.255.255.1:8443/echo", { proxy: pxURL(), tls: caTLS() });
	return { r, seen: await seen() };
};
S["proxy URL is https: (no TLS proxy listening): fails"] = async () => {
	const r = await F(U("hs", "/echo", "localhost"), { proxy: `https://127.0.0.1:${PORTS.px}`, tls: caTLS(), signal: AbortSignal.timeout(5000) });
	return { failed: !!(r.rejected || r.syncThrow) };
};
S["https: proxy (TLS to the proxy)"] = async () => {
	await reset();
	const a = await F(U("hs", "/echo", "localhost"), { proxy: `https://localhost:${PORTS.pxs}`, tls: caTLS() });
	const b = await F(U("h"), { proxy: `https://localhost:${PORTS.pxs}`, tls: caTLS() });
	const c = await F(U("hs", "/echo", "localhost"), { proxy: `https://localhost:${PORTS.pxs}` });
	return { a, b, c, seen: await seen() };
};
S["proxy option with redirect: each hop through the proxy"] = async () => {
	await reset();
	const r = await F(U("h", "/redir"), { proxy: pxURL() });
	return { r, seen: await seen() };
};

// ---- environment proxies ---------------------------------------------------------
S["env HTTPS_PROXY set at runtime: plain fetch https goes through it"] = async () => withEnv({ HTTPS_PROXY: pxURL() }, async () => {
	await reset();
	const r = await F(U("hs", "/echo", "localhost"), { tls: caTLS() });
	return { r, seen: await seen() };
});
S["env HTTPS_PROXY: plain fetch without tls option"] = async () => withEnv({ HTTPS_PROXY: pxURL() }, async () => {
	await reset();
	const r = await F(U("hs", "/echo", "localhost"));
	return { r, seen: await seen() };
});
S["env HTTP_PROXY: plain fetch http goes through it"] = async () => withEnv({ HTTP_PROXY: pxURL() }, async () => {
	await reset();
	const r = await F(U("h"));
	return { r, seen: await seen() };
});
S["env HTTPS_PROXY only: http target is direct"] = async () => withEnv({ HTTPS_PROXY: pxURL() }, async () => {
	await reset();
	const r = await F(U("h"));
	return { r, seen: await seen() };
});
S["env lowercase https_proxy wins; empty lowercase falls through"] = async () => {
	const a = await withEnv({ https_proxy: pxURL(), HTTPS_PROXY: pxURL("px407") }, async () => {
		await reset(); const r = await F(U("hs", "/echo", "localhost"), { tls: caTLS() }); return { r, seen: await seen() };
	});
	const b = await withEnv({ https_proxy: "", HTTPS_PROXY: pxURL() }, async () => {
		await reset(); const r = await F(U("hs", "/echo", "localhost"), { tls: caTLS() }); return { r, seen: await seen() };
	});
	const c = await withEnv({ https_proxy: '""', HTTPS_PROXY: pxURL() }, async () => {
		await reset(); const r = await F(U("hs", "/echo", "localhost"), { tls: caTLS() }); return { r, seen: await seen() };
	});
	return { a, b, c };
};
S["env ALL_PROXY fallback"] = async () => {
	const a = await withEnv({ ALL_PROXY: pxURL() }, async () => {
		await reset(); const r = await F(U("h")); return { r, seen: await seen() };
	});
	const b = await withEnv({ all_proxy: `socks5://127.0.0.1:${PORTS.px}` }, async () => {
		await reset(); const r = await F(U("h")); return { r, seen: await seen() };
	});
	const c = await withEnv({ ALL_PROXY: `127.0.0.1:${PORTS.px}` }, async () => {
		await reset(); const r = await F(U("h")); return { r, seen: await seen() };
	});
	return { a, b, c };
};
S["proxy:false with HTTPS_PROXY set: direct"] = async () => withEnv({ HTTPS_PROXY: pxURL() }, async () => {
	await reset();
	const r = await F(U("hs", "/echo", "localhost"), { proxy: false, tls: caTLS() });
	return { r, seen: await seen() };
});
S["proxy:'' and proxy:null with HTTPS_PROXY set: env applies"] = async () => withEnv({ HTTPS_PROXY: pxURL() }, async () => {
	await reset();
	const a = await F(U("hs", "/echo", "localhost"), { proxy: "", tls: caTLS() });
	const b = await F(U("hs", "/echo", "localhost"), { proxy: null, tls: caTLS() });
	return { a, b, seen: await seen() };
});
S["unix with HTTP_PROXY set: env proxy not used"] = async () => withEnv({ HTTP_PROXY: pxURL() }, async () => {
	await reset();
	const r = await F("http://localhost/echo", { unix: PORTS.sock });
	return { r, seen: await seen() };
});
S["NO_PROXY matrix (env proxy)"] = async () => {
	const out = {};
	const targets = { localhost: U("hs", "/echo", "localhost"), ip: U("hs", "/echo", "127.0.0.1") };
	const lists = [
		"localhost", ".localhost", "*.localhost", "LOCALHOST", "localhost.", "127.0.0.1", "127.0.0.0/8", "10.0.0.0/8",
		`localhost:${PORTS.hs}`, "localhost:1", `127.0.0.1:${PORTS.hs}`, "other.example", "*", "a.com, localhost", "a.com localhost",
		"calhost", "::1", "[::ffff:127.0.0.1]", "''",
	];
	for (const l of lists) {
		const row = {};
		for (const [tk, url] of Object.entries(targets)) {
			await withEnv({ HTTPS_PROXY: pxURL(), NO_PROXY: l }, async () => {
				await reset();
				const r = await F(url, { tls: caTLS() });
				const s = await seen();
				row[tk] = (r.res ? "ok" : "fail") + (s.proxy.length ? "+proxied" : "+direct");
			});
		}
		out[norm(l)] = row;
	}
	const lower = await withEnv({ HTTPS_PROXY: pxURL(), no_proxy: "localhost", NO_PROXY: "other" }, async () => {
		await reset(); await F(targets.localhost, { tls: caTLS() }); return (await seen()).proxy.length ? "proxied" : "direct";
	});
	return { out, lowerFirst: lower };
};
S["explicit proxy respects NO_PROXY by default; respectNoProxy:false does not"] = async () => withEnv({ NO_PROXY: "localhost" }, async () => {
	await reset();
	const a = await F(U("hs", "/echo", "localhost"), { proxy: pxURL(), tls: caTLS() });
	const sa = await seen();
	await reset();
	const b = await F(U("hs", "/echo", "localhost"), { proxy: { url: pxURL(), respectNoProxy: false }, tls: caTLS() });
	const sb = await seen();
	await reset();
	const c = await F(U("h", "/echo", "localhost"), { proxy: { url: pxURL(), respectNoProxy: false } });
	return { a, sa, b, sb, c, sc: await seen() };
});
S["env HTTPS_PROXY given at spawn"] = async () => {
	await reset();
	const src = `fetch(${JSON.stringify(U("hs", "/echo", "localhost"))},{tls:{ca:${JSON.stringify(rd("ca.pem"))}}}).then(r=>r.json()).then(j=>console.log(j.server),e=>console.log("err",e.name))`;
	const c = await child(src, { HTTPS_PROXY: pxURL() });
	return { c, seen: await seen() };
};

// ---- TLS -------------------------------------------------------------------------
S["tls ca: custom CA trusted"] = async () => {
	await reset();
	const a = await F(U("hs", "/echo", "localhost"), { tls: caTLS() });
	const b = await F(U("hs", "/echo", "127.0.0.1"), { tls: { ca: Buffer.from(rd("ca.pem")) } });
	const c = await F(U("hs", "/echo", "localhost"), { tls: { ca: [rd("other.pem"), rd("ca.pem")] } });
	return { a, b, c };
};
// Whether a `ca` replaces the default roots or adds to them: a public host
// (outbound only) fetched with only the test CA.
S["tls ca replaces the default roots (public host)"] = async () => {
	const a = await F("https://registry.npmjs.org/", { method: "HEAD", tls: caTLS(), signal: AbortSignal.timeout(15000) }, { text: false });
	const b = await F("https://registry.npmjs.org/", { method: "HEAD", signal: AbortSignal.timeout(15000) }, { text: false });
	return { withTestCA: a.res ? "ok" : "fail", defaultRoots: b.res ? "ok" : "fail" };
};
S["tls: no ca, wrong ca: fails"] = async () => ({
	none: await F(U("hs", "/echo", "localhost")),
	wrong: await F(U("hs", "/echo", "localhost"), { tls: { ca: rd("other.pem") } }),
	emptyTls: await F(U("hs", "/echo", "localhost"), { tls: {} }),
});
S["tls rejectUnauthorized:false"] = async () => ({
	f: await F(U("hs", "/echo", "localhost"), { tls: { rejectUnauthorized: false } }),
	zero: await F(U("hs", "/echo", "localhost"), { tls: { rejectUnauthorized: 0 } }),
});
S["tls hostname mismatch: 127.0.0.1 against a localhost-only cert"] = async () => ({
	plain: await F(U("hsn", "/echo", "127.0.0.1"), { tls: caTLS() }),
	ok: await F(U("hsn", "/echo", "localhost"), { tls: caTLS() }),
});
S["tls serverName + host header + checkServerIdentity (gateway pattern)"] = async () => {
	const calls = [];
	const r = await F(U("hsn", "/echo", "127.0.0.1"), {
		headers: { host: `localhost:${PORTS.hsn}` },
		tls: { ca: rd("ca.pem"), serverName: "localhost", checkServerIdentity: (h, cert) => { calls.push([h, cert.subject && cert.subject.CN, typeof cert.subjectaltname, typeof cert.raw]); return tls.checkServerIdentity("localhost", cert); } },
	});
	return { r, calls };
};
S["tls servername (lowercase) is accepted"] = async () => F(U("hsn", "/echo", "127.0.0.1"), {
	tls: { ca: rd("ca.pem"), servername: "localhost", checkServerIdentity: (h, cert) => tls.checkServerIdentity("localhost", cert) },
});
S["tls checkServerIdentity returning an Error rejects"] = async () => {
	const calls = [];
	const r = await F(U("hs", "/echo", "localhost"), { tls: { ca: rd("ca.pem"), checkServerIdentity: (h) => { calls.push(h); const e = new Error("pinned: no"); e.code = "PIN"; return e; } } });
	return { r, calls };
};
S["tls checkServerIdentity returning undefined skips the name check"] = async () => {
	const calls = [];
	const r = await F(U("hsn", "/echo", "127.0.0.1"), { tls: { ca: rd("ca.pem"), checkServerIdentity: (h) => { calls.push(h); } } });
	return { r, calls };
};
S["mTLS: client cert accepted, required"] = async () => ({
	cert: await F(U("mt", "/echo", "localhost"), { tls: { ca: rd("ca.pem"), cert: rd("cli.pem"), key: rd("cli.key") } }),
	encKey: await F(U("mt", "/echo", "localhost"), { tls: { ca: rd("ca.pem"), cert: rd("cli.pem"), key: rd("cli-enc.key"), passphrase: "s3cret" } }),
	buffers: await F(U("mt", "/echo", "localhost"), { tls: { ca: [Buffer.from(rd("ca.pem"))], cert: Buffer.from(rd("cli.pem")), key: Buffer.from(rd("cli.key")) } }),
	files: await F(U("mt", "/echo", "localhost"), { tls: { caFile: join(CERTS, "ca.pem"), certFile: join(CERTS, "cli.pem"), keyFile: join(CERTS, "cli.key") } }),
	without: await F(U("mt", "/echo", "localhost"), { tls: { ca: rd("ca.pem") } }),
	badPass: await F(U("mt", "/echo", "localhost"), { tls: { ca: rd("ca.pem"), cert: rd("cli.pem"), key: rd("cli-enc.key"), passphrase: "nope" } }),
});
S["tls through a proxy: CA applies to the origin"] = async () => {
	await reset();
	const ok = await F(U("mt", "/echo", "localhost"), { proxy: pxURL(), tls: { ca: rd("ca.pem"), cert: rd("cli.pem"), key: rd("cli.key") } });
	const bad = await F(U("hs", "/echo", "localhost"), { proxy: pxURL() });
	return { ok, bad, seen: await seen() };
};
S["Request input with a tls init"] = async () => F(new Request(U("hs", "/echo", "localhost"), { method: "POST", body: "x", headers: { "x-test": "req" } }), { tls: caTLS() });

// ---- unix ------------------------------------------------------------------------
S["unix socket: http"] = async () => {
	await reset();
	const a = await F("http://localhost/echo", { unix: PORTS.sock });
	const b = await F("http://example.invalid:8080/echo?z", { unix: PORTS.sock, method: "POST", body: "hi" });
	return { a, b, conn: (await seen()).conn };
};
S["unix socket: missing path"] = async () => F("http://localhost/echo", { unix: join(CERTS, "nope.sock") });

// ---- keepalive -------------------------------------------------------------------
S["keepalive default: sequential requests reuse connections"] = async () => {
	await reset();
	const a = await F(U("hk"));
	const b = await F(U("hk"));
	const c = await F(U("hk"));
	// See the FetchSession reuse scenario: "fewer connections than requests".
	return { a: a.res && a.res.json.headers.connection, b: b.res && b.res.json.headers.connection, c: !!c.res, reused: (await seen()).conn.hk < 3 };
};
S["keepalive:false: a connection per request"] = async () => {
	await reset();
	const a = await F(U("hk"), { keepalive: false });
	const b = await F(U("hk"), { keepalive: false });
	const c = await F(U("hsk", "/echo", "localhost"), { keepalive: false, tls: caTLS() });
	const d = await F(U("hsk", "/echo", "localhost"), { keepalive: false, tls: caTLS() });
	const e = await F(U("hk"), { keepalive: 0 });
	return { a: a.res && a.res.json.headers.connection, b: b.res && b.res.json.headers.connection, c: !!c.res, d: !!d.res, e: e.res && e.res.json.headers.connection, conn: (await seen()).conn };
};

// ---- timeout ---------------------------------------------------------------------
S["timeout:false and default with a body stalled 6 s"] = async () => {
	const t = async (init) => { const t0 = Date.now(); const r = await F(U("h", "/slow?ms=6000"), init); return { r, took: bucket(Date.now() - t0) }; };
	const [def, off, t0] = await Promise.all([t({}), t({ timeout: false }), t({ timeout: 0 })]);
	return { def, off, t0 };
};
// Bun pads the idle timeout by one 4 s timer-wheel period (2 -> 6 s) and fires it
// on a 4 s tick, so a 12 s stall is past it however the tick falls.
S["BUN_CONFIG_HTTP_IDLE_TIMEOUT=2: default times out, timeout:false does not"] = async () => {
	const src = `const t=async(i)=>{const t0=Date.now();try{const r=await fetch(${JSON.stringify(U("h", "/slow?ms=12000"))},i);const x=await r.text();return "ok:"+x}catch(e){const s=(Date.now()-t0)/1000;return "err:"+e.name+":"+(e.code||"")+":"+(s<5.5?"<5.5s":s<11?"5.5-11s":">=11s")}};Promise.all([t({}),t({timeout:false})]).then(v=>console.log(JSON.stringify(v)))`;
	return child(src, { BUN_CONFIG_HTTP_IDLE_TIMEOUT: "2" });
};

S["numeric timeout: 2000 ms idle times out a 12 s stall, 30000 does not"] = async () => {
	const t = async (init) => { const t0 = Date.now(); const r = await F(U("h", "/slow?ms=12000"), init); const s = (Date.now() - t0) / 1000; return { ok: !!r.res, err: r.rejected ? r.rejected.name + ":" + (r.rejected.code || "") : null, took: s < 5.5 ? "<5.5s" : s < 11 ? "5.5-11s" : ">=11s" }; };
	const [short, long] = await Promise.all([t({ timeout: 2000 }), t({ timeout: 30000 })]);
	return { short, long };
};

// ---- decompress ------------------------------------------------------------------
S["decompress default and false"] = async () => {
	const def = await F(U("h", "/gzip"));
	let raw;
	try {
		const r = await fetch(U("h", "/gzip"), { decompress: false });
		const b = new Uint8Array(await r.arrayBuffer());
		raw = { isResponse: r instanceof Response, status: r.status, type: r.type, url: norm(r.url), redirected: r.redirected, ce: r.headers.get("content-encoding"), cl: r.headers.get("content-length"), len: b.length, magic: [b[0], b[1]] };
	} catch (e) { raw = errShape(e); }
	const redir = await F(U("h", "/redir"), { decompress: false });
	const zero = await F(U("h", "/gzip"), { decompress: 0 }, { text: false });
	return { def, raw, redir, zero };
};

// ---- FetchSession ----------------------------------------------------------------
S["FetchSession tls: session CA used, plain fetch unaffected"] = async () => {
	await reset();
	const s = new Bun.FetchSession({ tls: caTLS() });
	const a = await call(s.fetch, U("hs", "/echo", "localhost"));
	const b = await F(U("hs", "/echo", "localhost"));
	const c = await F(U("hs", "/echo", "localhost"), { session: s });
	const d = await call(s.fetch, U("hs", "/echo", "localhost"), { tls: { ca: rd("other.pem") } });
	s.close();
	return { a, b, c, d };
};
S["FetchSession reuse, separate pool, close, fetch after close"] = async () => {
	await reset();
	const s = new Bun.FetchSession({ tls: caTLS() });
	const u = U("hsk", "/echo", "localhost");
	const ok = [];
	for (let i = 0; i < 4; i++) ok.push(!!(await call(s.fetch, u)).res);
	const st1 = await seen();
	ok.push(!!(await F(u, { tls: caTLS() })).res);
	const st2 = await seen();
	const c1 = s.close();
	await sleep(300);
	const st3 = await seen(true);
	ok.push(!!(await call(s.fetch, u)).res);
	const st4 = await seen();
	s.close(); s.close();
	// Node's undici can hold two connections where Bun reuses one (LIMITATIONS.md):
	// reuse is "fewer connections than requests"; the rest are exact counts.
	return {
		ok, reused: st1.conn.hsk < 4, plainOpensItsOwn: st2.conn.hsk - st1.conn.hsk,
		openAfterClose: st3.open.hsk, afterCloseOpensOne: st4.conn.hsk - st3.conn.hsk, closeReturns: String(c1),
	};
};
S["FetchSession close while a request is in flight"] = async () => {
	const s = new Bun.FetchSession();
	const p = call(s.fetch, U("h", "/slow?ms=800"));
	await sleep(150);
	s.close();
	return p;
};
S["FetchSession keepAlive:false"] = async () => {
	await reset();
	const s = new Bun.FetchSession({ keepAlive: false });
	const a = await call(s.fetch, U("hk"));
	const b = await call(s.fetch, U("hk"));
	const c = await call(s.fetch, U("hk"), { keepalive: true });
	return { a: a.res && a.res.json.headers.connection, b: b.res && b.res.json.headers.connection, c: c.res && c.res.json.headers.connection, conn: (await seen()).conn };
};
S["FetchSession proxy and unix options"] = async () => {
	await reset();
	const sp = new Bun.FetchSession({ proxy: pxURL(), tls: caTLS() });
	const a = await call(sp.fetch, U("hs", "/echo", "localhost"));
	const b = await call(sp.fetch, U("hs", "/echo", "localhost"), { proxy: false });
	const su = new Bun.FetchSession({ unix: PORTS.sock });
	const c = await call(su.fetch, "http://localhost/echo");
	const d = await call(su.fetch, U("h"), { proxy: pxURL() });
	const e = await call(sp.fetch, "http://localhost/echo", { unix: PORTS.sock });
	return { a, b, c, d, e, seen: await seen() };
};
S["FetchSession analytics post() pattern"] = async () => withEnv({ NO_PROXY: "localhost" }, async () => {
	await reset();
	const s = new Bun.FetchSession({ tls: caTLS() });
	const isFS = s instanceof Bun.FetchSession;
	const r = await call(s.fetch, U("hs", "/echo", "localhost"), {
		method: "POST", headers: { "content-type": "application/json", "x-test": "post" }, body: JSON.stringify({ events: [1] }),
		signal: AbortSignal.timeout(5000), proxy: { url: pxURL(), respectNoProxy: false }, redirect: "manual",
	});
	const viaEnvOff = await call(s.fetch, U("hs", "/echo", "localhost"), { method: "POST", body: "{}", proxy: false });
	return { isFS, r, viaEnvOff, seen: await seen() };
});
S["FetchSession fetch with a detached this"] = async () => {
	const s = new Bun.FetchSession();
	const f = s.fetch;
	const a = await call(f, U("h"));
	const b = await call(f.bind(null), U("h"));
	let c;
	try { c = await call(Bun.FetchSession.prototype.close.bind({}), 1); } catch (e) { c = errShape(e); }
	return { a: !!a.res, b: !!b.res, c };
};
S["FetchSession in instanceof checks of Response/Request"] = async () => {
	const s = new Bun.FetchSession();
	const r = await s.fetch(new Request(U("h"), { headers: { "x-test": "q" } }));
	return { isResponse: r instanceof Response, xTest: (await r.json()).headers["x-test"] };
};

// ---- options the compat layer refuses ------------------------------------------------
S["refuse: verbose"] = async () => F(U("h"), { verbose: true }, { text: false });
S["s3 option on an http: URL is not read (as in Bun)"] = async () => F(U("h"), { s3: { accessKeyId: "x" } }, { text: false });
S["refuse: s3:// URL"] = async () => F("s3://bucket/key", { s3: { accessKeyId: "x", secretAccessKey: "y", endpoint: `http://127.0.0.1:${PORTS.h}` } }, { text: false });
S["refuse: compress"] = async () => F(U("h"), { method: "POST", body: "x", compress: "gzip" }, { text: false });
S["refuse: maxRedirects"] = async () => F(U("h", "/redir"), { maxRedirects: 3 }, { text: false });
S["refuse: protocol h2"] = async () => F(U("hs", "/echo", "localhost"), { protocol: "h2", tls: caTLS() }, { text: false });
S["refuse: tls minVersion"] = async () => F(U("hs", "/echo", "localhost"), { tls: { ca: rd("ca.pem"), minVersion: 771 } }, { text: false });
S["refuse: FetchSession keepAlive.maxIdleSockets"] = async () => { try { new Bun.FetchSession({ keepAlive: { maxIdleSockets: 2 } }); return "constructed"; } catch (e) { return errShape(e); } };

module.exports = { S, init(ports, certs) { PORTS = ports; CERTS = certs; } };
