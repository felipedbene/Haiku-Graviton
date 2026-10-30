// Child process for the "unref'd server lets the process exit" scenario: serves on
// a loopback ephemeral port, prints it, and holds nothing else. Under Bun it uses
// the real Bun.serve/Bun.listen; under Node, compat/lib/net.mjs.
const mode = process.argv[2];
const api = process.versions.bun
	? { serve: (o) => Bun.serve(o), listen: (o) => Bun.listen(o) }
	: await import(new URL("../lib/net.mjs", import.meta.url).href);
let port;
if (mode.startsWith("serve")) {
	const s = api.serve({ hostname: "127.0.0.1", port: 0, fetch: () => new Response("ok") });
	if (mode !== "serve-ref") s.unref();
	port = s.port;
	// serve-ref is the control: a ref'd server must keep the child alive.
} else {
	const l = api.listen({ hostname: "127.0.0.1", port: 0, socket: { data() {} } });
	l.unref();
	port = l.port;
}
console.log("PORT " + port);
// Hold the loop for 300 ms so the parent can connect before only the unref'd
// server (and its connection) are left to decide whether the process exits.
setTimeout(() => {}, 300);
