// claude.mjs -- the entry point the launcher runs: `node --zero-fill-buffers
// <prefix>/claude.mjs ...`.
//
// Outside a Bun standalone binary, the bundle relaunches itself as
// `process.execPath process.argv[1] ...` (26 modules: background sessions, the
// daemon, eval runners). argv[1] is this file, so every relaunch comes back here
// and gets the same runtime as the first process:
//  - the Bun compat layer, loaded before the bundle so its text-loader hook is in
//    place when the bundle's module graph is resolved;
//  - --zero-fill-buffers, which Bun bakes into the binary's compile argv. A relaunch
//    doesn't carry node's execArgv, so a process started without it replaces itself
//    (process.execve: same pid and stdio, no intermediate process) with one that has it.
if (!process.execArgv.includes("--zero-fill-buffers")) {
	process.execve(process.execPath,
		[process.execPath, "--zero-fill-buffers", ...process.execArgv, ...process.argv.slice(1)],
		process.env);
}
await import("./bun-compat.mjs");
await import("./cli");
