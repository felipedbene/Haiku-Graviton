// SO_REUSEPORT through Node (net.Server listen({reusePort:true})): libuv answers
// ENOTSUP where it has no load-balancing SO_REUSEPORT; then a second plain bind
// of the same loopback port (what compat falls back to) is EADDRINUSE.
// Usage: node reuseport.mjs   (loopback, port 0; every server is closed)
import net from "node:net";
const listen = (opts) => new Promise((resolve) => {
	const s = net.createServer();
	s.once("error", (e) => resolve({ error: { code: e.code, errno: e.errno, syscall: e.syscall } }));
	s.listen(opts, () => resolve({ s, port: s.address().port }));
});
const a = await listen({ host: "127.0.0.1", port: 0, reusePort: true });
console.log(JSON.stringify({ first: a.error ?? "listening" }));
const base = a.s ? a : await listen({ host: "127.0.0.1", port: 0 });
const b = await listen({ host: "127.0.0.1", port: base.port, reusePort: true });
console.log(JSON.stringify({ secondSamePortReusePort: b.error ?? "listening" }));
const c = await listen({ host: "127.0.0.1", port: base.port });
console.log(JSON.stringify({ secondSamePortPlain: c.error ?? "listening" }));
for (const x of [base, b, c]) x.s?.close();
