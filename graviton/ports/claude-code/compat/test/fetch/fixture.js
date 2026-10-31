// fixture.js <certdir> <sockdir>: the servers the fetch scenarios talk to. Always
// run under Node (never the runtime under test), so both recordings see the
// same servers. Prints one JSON line of ports, then serves until stdin closes.
//
// Network rules: every TCP server binds 127.0.0.1 on port 0; the unix socket
// lives in <sockdir> (a fresh mkdtemp). The proxies tunnel and forward only to
// 127.0.0.1 / localhost; anything else is answered 403 and never dialled.
"use strict";
const http = require("http");
const https = require("https");
const net = require("net");
const zlib = require("zlib");
const { readFileSync } = require("fs");
const { join } = require("path");

const [certs, sockdir] = process.argv.slice(2);
const rd = (f) => readFileSync(join(certs, f));
const log = { conn: {}, open: {}, req: {}, proxy: [] };
const LOOP = "127.0.0.1";
// conn: connections accepted since the last reset; open: connections open now.
const countConn = (name) => (sock) => {
	log.conn[name] = (log.conn[name] || 0) + 1;
	log.open[name] = (log.open[name] || 0) + 1;
	sock.once("close", () => { log.open[name]--; });
};
const lower = (raw) => { const o = []; for (let i = 0; i < raw.length; i += 2) o.push([raw[i].toLowerCase(), raw[i + 1]]); return o; };
// Origin and forwarded headers are recorded by name: their wire order is each
// runtime's own. CONNECT headers keep their wire order (this layer writes them).
const byName = (a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0);
const GZ = zlib.gzipSync(Buffer.from("hello gzip world\n".repeat(10)));
// Headers recorded from requests. The rest (user-agent, accept-encoding, ...) are each
// runtime's own fetch defaults, not what the scenarios test.
const SHOWN = ["host", "connection", "proxy-authorization", "proxy-connection", "x-test"];
// Only these servers keep connections alive; every other response closes its
// connection, so a pooled socket can never hide which route a request took.
const KEEP = new Set(["hk", "hsk"]);

function handler(name) {
	return (req, res) => {
		const u = new URL(req.url, "http://x");
		const sock = req.socket;
		(log.req[name] = log.req[name] || []).push({ method: req.method, path: req.url });
		const headers = Object.fromEntries(lower(req.rawHeaders).filter(([k]) => SHOWN.includes(k)).sort(byName));
		if (!KEEP.has(name)) res.setHeader("connection", "close");
		if (u.pathname === "/echo") {
			const body = JSON.stringify({
				server: name, method: req.method, path: req.url, headers,
				servername: sock.servername === undefined ? null : sock.servername,
				clientCN: typeof sock.getPeerCertificate === "function" ? (sock.getPeerCertificate().subject || {}).CN || null : null,
			});
			res.writeHead(200, { "content-type": "application/json" });
			return res.end(body);
		}
		if (u.pathname === "/gzip") {
			res.writeHead(200, { "content-type": "text/plain", "content-encoding": "gzip", "content-length": GZ.length });
			return res.end(GZ);
		}
		if (u.pathname === "/slow") {
			res.writeHead(200, { "content-type": "text/plain", "content-length": 2 });
			res.write("a");
			const t = setTimeout(() => res.end("b"), Number(u.searchParams.get("ms")));
			res.on("close", () => clearTimeout(t));
			return;
		}
		if (u.pathname === "/redir") {
			res.writeHead(302, { location: "/echo" });
			return res.end();
		}
		res.writeHead(404);
		res.end();
	};
}

function loopTarget(hostport) {
	const m = /^(?:\[(.*)\]|([^:]+)):(\d+)$/.exec(hostport);
	if (!m) return null;
	const host = m[1] || m[2];
	return host === LOOP || host === "localhost" ? Number(m[3]) : null;
}

function proxy(name, refuse, tlsOpts) {
	const s = (tlsOpts ? https.createServer.bind(https, tlsOpts) : http.createServer)((req, res) => {
		log.proxy.push({ proxy: name, kind: "forward", url: req.url, headers: lower(req.rawHeaders).filter(([k]) => SHOWN.includes(k)).sort(byName) });
		if (refuse) { res.writeHead(407, { "proxy-authenticate": 'Basic realm="cc"' }); return res.end(); }
		let t;
		try { t = new URL(req.url); } catch { res.writeHead(400); return res.end(); }
		const port = loopTarget(t.host);
		if (t.protocol !== "http:" || port === null) { res.writeHead(403); return res.end(); }
		const hdrs = {};
		for (const [k, v] of lower(req.rawHeaders)) if (!k.startsWith("proxy-")) hdrs[k] = v;
		const up = http.request({ host: LOOP, port, method: req.method, path: t.pathname + t.search, headers: hdrs }, (r) => {
			res.writeHead(r.statusCode, r.rawHeaders);
			r.pipe(res);
		});
		up.on("error", () => { res.writeHead(502); res.end(); });
		req.pipe(up);
	});
	s.on("connect", (req, client, head) => {
		log.proxy.push({ proxy: name, kind: "connect", url: req.url, headers: lower(req.rawHeaders) });
		client.on("error", () => {});
		if (refuse) {
			client.end('HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm="cc"\r\nContent-Length: 0\r\n\r\n');
			return;
		}
		const port = loopTarget(req.url);
		if (port === null) { client.end("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"); return; }
		const up = net.connect(port, LOOP, () => {
			client.write("HTTP/1.1 200 Connection Established\r\n\r\n");
			if (head && head.length) up.write(head);
			up.pipe(client); client.pipe(up);
		});
		up.on("error", () => client.destroy());
	});
	return s;
}

const servers = {
	h: http.createServer(handler("h")),
	hk: http.createServer(handler("hk")),
	hs: https.createServer({ key: rd("srv.key"), cert: rd("srv.pem") }, handler("hs")),
	hsk: https.createServer({ key: rd("srv.key"), cert: rd("srv.pem") }, handler("hsk")),
	hsn: https.createServer({ key: rd("srvn.key"), cert: rd("srvn.pem") }, handler("hsn")),
	mt: https.createServer({ key: rd("srv.key"), cert: rd("srv.pem"), ca: rd("cca.pem"), requestCert: true, rejectUnauthorized: true }, handler("mt")),
	px: proxy("px", false),
	px407: proxy("px407", true),
	pxs: proxy("pxs", false, { key: rd("srv.key"), cert: rd("srv.pem") }),
};
for (const [n, s] of Object.entries(servers)) s.on(s instanceof https.Server ? "secureConnection" : "connection", countConn(n));
const ux = http.createServer(handler("ux"));
ux.on("connection", countConn("ux"));
const ctl = net.createServer((c) => {
	let b = "";
	c.setEncoding("utf8");
	c.on("data", (d) => {
		b += d;
		if (!b.includes("\n")) return;
		const cmd = b.trim();
		if (cmd === "reset") { log.conn = {}; log.req = {}; log.proxy = []; }
		c.end(JSON.stringify(log));
	});
});

(async () => {
	const ports = {};
	for (const [n, s] of Object.entries(servers)) {
		await new Promise((ok) => s.listen(0, LOOP, ok));
		ports[n] = s.address().port;
	}
	await new Promise((ok) => ctl.listen(0, LOOP, ok));
	ports.ctl = ctl.address().port;
	const sock = join(sockdir, "ux.sock");
	await new Promise((ok) => ux.listen(sock, ok));
	ports.sock = sock;
	process.stdout.write(JSON.stringify(ports) + "\n");
	process.stdin.resume();
	process.stdin.on("end", () => process.exit(0));
})();
