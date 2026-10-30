// Child side of the keepalive scenarios: spawn a 3 s sleeper, then let the
// runtime decide whether it stays alive for it.
const mode = process.argv[2];
const opts = mode === "unref-ignore" ? { stdio: ["ignore", "ignore", "ignore"], detached: true }
	: mode === "unref-stdin-pipe" ? { stdin: "pipe", stdout: "ignore", stderr: "ignore" }
	: mode.includes("pipe") ? { stderr: "ignore" } : { stdout: "ignore", stderr: "ignore" };
const p = Bun.spawn(["sleep", "3"], opts);
if (mode.startsWith("unref") || mode === "ref-after-unref") p.unref();
if (mode === "ref-after-unref") p.ref();
if (mode === "unref-pipe-read") void p.stdout;
if (mode === "unref-pipe-text") p.stdout.text().then(() => {});
console.log("spawned", mode);
