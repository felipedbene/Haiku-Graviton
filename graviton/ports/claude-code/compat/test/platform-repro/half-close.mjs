// A server that answers and ends (end("hi")), and a default client (allowHalfOpen
// false, so it ends its side on 'end'). Logs each side's events with times, over
// 127.0.0.1 and over ::1. Both sides should see 'end' and 'close' within a few
// ms. After the client has closed, the TCP table is printed once (Haiku netstat
// / Linux ss), then the run waits up to WAIT ms (argv[2], default 3000) for the
// server side's 'close' and gives up.
// Usage: node half-close.mjs [wait-ms]   (loopback, port 0; everything is destroyed)
import net from "node:net";
import { execFileSync } from "node:child_process";
const WAIT = +(process.argv[2] ?? 3000);
const table = (port) => {
	const [cmd, args] = process.platform === "haiku" ? ["netstat", ["-n", "-t"]] : ["ss", ["-tan"]];
	return execFileSync(cmd, args, { encoding: "utf8" }).split("\n").filter((l) => l.includes(":" + port) || l.includes("." + port)).map((l) => l.trim().replace(/\s+/g, " "));
};
const run = (host) => new Promise((resolve) => {
	const log = [];
	const start = Date.now();
	const at = (w) => log.push(`${Date.now() - start}ms ${w}`);
	let srvSock = null, cli = null, done = false, port = 0;
	const finish = () => {
		if (done) return;
		done = true;
		clearTimeout(timer);
		srvSock?.destroy(); cli?.destroy(); s.close();
		resolve({ host, log });
	};
	const s = net.createServer((c) => {
		srvSock = c;
		at("server: connection");
		c.on("end", () => at("server: end"));
		c.on("close", () => { at("server: close"); finish(); });
		c.on("error", (e) => at("server: error " + e.code));
		c.end("hi");
	});
	s.listen({ host, port: 0, ...(net.isIP(host) === 6 ? { ipv6Only: true } : {}) }, () => {
		port = s.address().port;
		cli = net.connect({ host, port });
		cli.on("data", (d) => at("client: data " + d));
		cli.on("end", () => at("client: end"));
		cli.on("close", () => { at("client: close"); setTimeout(() => { if (!done) at("tcp table 100ms after client close: " + JSON.stringify(table(port))); }, 100); });
		cli.on("error", (e) => at("client: error " + e.code));
	});
	const timer = setTimeout(() => { at(`TIMEOUT: no server close after ${WAIT} ms`); finish(); }, WAIT);
});
for (const h of ["127.0.0.1", "::1"]) console.log(JSON.stringify(await run(h)));
