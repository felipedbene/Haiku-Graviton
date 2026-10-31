// Haiku: every IPv6 listen through Node fails with EINVAL unless ipv6Only is set.
// libuv's bind always calls setsockopt(IPV6_V6ONLY, 0 or 1); Haiku's IPv6
// sockets are v6-only and refuse 0 with B_BAD_VALUE (EINVAL).
// Usage: node v6only.mjs   (loopback, port 0; every server is closed)
import net from "node:net";
const tryListen = (opts) => new Promise((resolve) => {
	const s = net.createServer();
	s.once("error", (e) => resolve({ opts, error: { code: e.code, errno: e.errno, syscall: e.syscall, message: e.message } }));
	s.listen(opts, () => { const a = s.address(); s.close(() => resolve({ opts, listening: { address: a.address, family: a.family } })); });
});
for (const o of [{ host: "::1", port: 0 }, { host: "::1", port: 0, ipv6Only: true }, { host: "::", port: 0 }, { host: "::", port: 0, ipv6Only: true }, { host: "127.0.0.1", port: 0 }])
	console.log(JSON.stringify(await tryListen(o)));
console.log(JSON.stringify({ platform: process.platform, node: process.version }));
