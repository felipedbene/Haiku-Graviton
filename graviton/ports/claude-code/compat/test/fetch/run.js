// run.js <out.json> <certdir> <sockdir> [only-regex]: start fixture.js under
// $FIXTURE_NODE (always Node, so both runtimes meet the same servers), run every
// scenario, record [name, result | {threw}] pairs, stop the fixture.
const { writeFileSync, mkdtempSync, rmSync } = require("fs");
const { spawn } = require("child_process");
const { join } = require("path");
const { S, init } = require("./scenarios.js");
const [out, certs, sockdir, only] = process.argv.slice(2);
(async () => {
	const sock = mkdtempSync(join(sockdir, "run-"));	// a fresh dir for this run's unix socket
	const fx = spawn(process.env.FIXTURE_NODE, [join(__dirname, "fixture.js"), certs, sock], { stdio: ["pipe", "pipe", "inherit"] });
	const ports = await new Promise((ok, fail) => {
		let b = "";
		fx.stdout.on("data", (d) => { b += d; if (b.includes("\n")) ok(JSON.parse(b)); });
		fx.on("exit", (c) => fail(new Error("fixture exited " + c)));
	});
	init(ports, certs);
	const res = [];
	for (const [k, f] of Object.entries(S)) {
		if (only && !new RegExp(only).test(k)) continue;
		try { res.push([k, await f()]); } catch (e) { res.push([k, { threw: String(e && e.message) }]); }
	}
	writeFileSync(out, JSON.stringify(res, null, 1));
	console.log("scenarios:", res.length);
	fx.stdin.end();
	await new Promise((ok) => fx.on("exit", ok));
	rmSync(sock, { recursive: true, force: true });
	process.exit(0);
})();
