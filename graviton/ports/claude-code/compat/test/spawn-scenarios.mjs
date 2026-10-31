// Bun.spawn diff-test scenarios, shared by both sides: run as
//   bun spawn-scenarios.mjs <out.json> <workdir>
//   node --import ./spawn-preload.mjs spawn-scenarios.mjs <out.json> <workdir>
// Under node, spawn-preload.mjs installs compat's spawn/file as Bun.spawn/Bun.file.
// Each scenario records only run-independent observations: PIDs are reduced to
// relations (sid == child pid, sid == parent sid, cmdline read via the pid) and
// timings to buckets, so the two JSON files compare with plain equality.
import { mkdirSync, rmSync, writeFileSync, readFileSync, statSync, openSync, closeSync, readdirSync, chmodSync, existsSync, realpathSync, accessSync, constants as FSC } from "node:fs";
import { join, delimiter } from "node:path";
import { execFileSync } from "node:child_process";
import os from "node:os";

const [OUT, WORK_ARG] = process.argv.slice(2);
rmSync(WORK_ARG, { recursive: true, force: true });
mkdirSync(WORK_ARG, { recursive: true });
// The real path, so a symlinked /tmp (Haiku: /tmp -> /boot/system/cache/tmp) is
// never seen half-resolved; both spellings are normalised away at write time.
const WORK = realpathSync(WORK_ARG);
const HERE = new URL(".", import.meta.url).pathname;
const isBun = typeof process.versions.bun === "string";
// The child runtime for the unref scenarios: the same runtime, with compat for node.
const SELF = isBun ? [process.execPath] : [process.execPath, "--import", join(HERE, "spawn-preload.mjs")];

// ---- platform observers ----
// What the scenarios observe from outside a child (its session, its argv as
// the process table shows it, its executable, its open fds) comes from /proc on
// Linux. Haiku has no /proc: there the kernel's team_info (through the port's
// cc-teaminfo helper, native/cc-teaminfo.c) and fdinfo answer the same
// questions. Each observer returns the same shape on both platforms. Where the
// Haiku kernel does not keep a value at all, the observation is an explicit
// UNOBSERVABLE marker with the reason, which check-spawn.mjs counts and prints
// as a documented gap (never as a match).
const PLATFORM = process.platform;
const UNOBS_KEY = "unobservable-on-" + PLATFORM;
const unobservable = (reason, extra = {}) => ({ [UNOBS_KEY]: reason, ...extra });
const TEAMINFO = process.env.CC_TEAMINFO || "/boot/system/lib/claude-code/bin/cc-teaminfo";
function teamLine(pid) {
	// team parent group session uid start_us start_epoch_us args (args is the rest of the line)
	const out = execFileSync(TEAMINFO, [String(pid)], { encoding: "latin1" });
	if (out === "") return null;
	const line = out.replace(/\n$/, "");
	const f = line.split(" ");
	if (f.length < 8 || f[0] !== String(pid)) throw new Error(`cc-teaminfo ${pid}: unexpected output ${JSON.stringify(out)}`);
	return { team: f[0], parent: f[1], session: f[3], args: f.slice(7).join(" ") };
}
// team_info.args holds at most 63 bytes (B_OS_NAME_LENGTH - 1 of a 64-byte field).
const HAIKU_ARGS_MAX = 63;
const OBS = PLATFORM === "linux" ? {
	// /proc/<pid>/stat field 6 is the session id (after "pid (comm) state ppid pgrp").
	sessionOf: (statText) => statText.slice(statText.lastIndexOf(")") + 2).split(" ")[3],
	selfSid: () => OBS.sessionOf(readFileSync("/proc/self/stat", "utf8")),
	// Raw process-table text for a pid, "" when not readable yet.
	argsText: (pid) => { try { return readFileSync(`/proc/${pid}/cmdline`, "latin1"); } catch { return ""; } },
	argv: (text) => { const a = text.split("\0"); if (a.at(-1) === "") a.pop(); return a; },
	// A shell snippet that prints the shell's own session on stdout, and its parser.
	childSessionCmd: "cat /proc/$$/stat",
	parseChildSession: (out) => OBS.sessionOf(out),
	childExeCmd: "readlink /proc/$$/exe",
	parseChildExe: (out) => out.replace(/\n$/, ""),
	fdsOf: (pid) => readdirSync(`/proc/${pid}/fd`).map(Number).sort((a, b) => a - b),
	envBin: "/usr/bin/env",
	readOnlyDir() {
		try { for (const s of readdirSync("/snap")) { const c = `/snap/${s}/current`; if (existsSync(join(c, "meta"))) return c; } } catch {}
		return null;
	},
} : PLATFORM === "haiku" ? {
	selfSid: () => teamLine(process.pid).session,
	argsText: (pid) => teamLine(pid)?.args ?? "",
	// The kernel builds team_info.args as "<exec path> <argv[1]> <argv[2]> ..."
	// (src/system/kernel/team.cpp, Team::SetArgs(path, args + 1, argc - 1)):
	// argv[0] is replaced by the path given to exec, so it is not observable from
	// outside the child; argv[0] itself is checked from inside the child by
	// "argv0 visible as $0". The remaining words are argv[1..] joined by single
	// spaces, which these scenarios keep free of spaces, so splitting is exact.
	argv(text) {
		if (Buffer.byteLength(text, "latin1") >= HAIKU_ARGS_MAX) throw new Error(`team_info.args is truncated (${text.length} bytes), argv not observable: ${JSON.stringify(text)}`);
		const [path, ...rest] = text.split(" ");
		return [unobservable("team_info.args starts with the exec path, not argv[0] (team.cpp Team::SetArgs); argv[0] is checked by 'argv0 visible as $0'", { kernelShows: path }), ...rest];
	},
	// "; :" keeps the shell from exec'ing the helper in its own place (bash does
	// that for a lone command after -c), which would make $$ name the helper.
	childSessionCmd: `${TEAMINFO} $$; :`,
	parseChildSession: (out) => out.split(" ")[3],
	// The exec path is the first word of team_info.args (see argv above): Haiku's
	// counterpart of /proc/<pid>/exe.
	childExeCmd: `${TEAMINFO} $$; :`,
	parseChildExe: (out) => out.replace(/\n$/, "").split(" ")[7],
	fdsOf(pid) {
		// fdinfo <team>: "Team: (<id>) <args>", then " <fd>  <R|W>  <dev>:<ino>" per fd;
		// negative numbers are the team's cwd/root entries, not descriptors.
		const out = execFileSync("fdinfo", [String(pid)], { encoding: "latin1" });
		const head = /^Team: \((\d+)\)/m.exec(out);
		if (!head || head[1] !== String(pid) || (out.match(/^Team: /gm) || []).length !== 1) throw new Error(`fdinfo ${pid}: unexpected output ${JSON.stringify(out)}`);
		return [...out.matchAll(/^\s*(-?\d+)\s/gm)].map((m) => +m[1]).filter((n) => n >= 0).sort((a, b) => a - b);
	},
	envBin: "/bin/env",
	// packagefs is mounted read-only: creating a file there fails with EROFS.
	readOnlyDir: () => "/boot/system/bin",
} : null;
if (!OBS) throw new Error(`spawn-scenarios: no process observers for platform ${PLATFORM}`);
// The executable PATH lookup a shell would do, for "which binary is sh".
function onPath(name) {
	for (const d of (process.env.PATH ?? "").split(delimiter)) {
		if (!d) continue;
		const f = join(d, name);
		try { accessSync(f, FSC.X_OK); if (statSync(f).isFile()) return f; } catch {}
	}
	return null;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const errShape = (e) => {
	if (!(e instanceof Object)) return { threwNonObject: String(e) };
	return {
		threw: true, ctor: e.constructor?.name, name: e.name, message: e.message,
		code: e.code, errno: e.errno, syscall: e.syscall, path: e.path,
		isError: e instanceof Error, isTypeError: e instanceof TypeError,
	};
};
const typeOf = (v) => (v === null ? "null" : v instanceof ReadableStream ? "ReadableStream" : typeof v);
const selfSid = OBS.selfSid();
const status = (p) => ({ exitCode: p.exitCode, signalCode: p.signalCode, killed: p.killed });
// Waits until the child has exec'd (its process-table entry stops being the
// parent's) and returns its argv as the process table shows it, as an array.
async function cmdlineOf(pid, notEqual) {
	for (let i = 0; i < 200; i++) {
		const c = OBS.argsText(pid);
		if (c !== "" && c !== notEqual) return OBS.argv(c);
		await sleep(10);
	}
	return "TIMEOUT";
}
const ownCmdline = OBS.argsText(process.pid);

const S = [];
const add = (name, fn) => S.push([name, fn]);

// ---- argument validation ----
const V = {
	"no args": () => Bun.spawn(),
	"null": () => Bun.spawn(null),
	"string cmd": () => Bun.spawn("echo"),
	"number cmd": () => Bun.spawn(5),
	"empty array": () => Bun.spawn([]),
	"object no cmd": () => Bun.spawn({}),
	"object cmd empty": () => Bun.spawn({ cmd: [] }),
	"object cmd string": () => Bun.spawn({ cmd: "echo" }),
	"env string": () => Bun.spawn(["true"], { env: "x" }),
	"env number": () => Bun.spawn(["true"], { env: 5 }),
	"stdio string": () => Bun.spawn(["true"], { stdio: "pipe" }),
	"stdio bogus entry": () => Bun.spawn(["true"], { stdio: ["bogus"] }),
	"stdout bogus": () => Bun.spawn(["true"], { stdout: "bogus" }),
	"stdout object": () => Bun.spawn(["true"], { stdout: {} }),
	"stdout negative fd": () => Bun.spawn(["true"], { stdout: -1 }),
	"stdin fd 1": () => Bun.spawn(["true"], { stdin: 1 }),
	"stdout fd 0": () => Bun.spawn(["true"], { stdout: 0 }),
	"socket-fd at 1": () => Bun.spawn(["true"], { stdio: ["ignore", "socket-fd"] }),
	"null byte arg0": () => Bun.spawn(["tr\0ue"]),
	"null byte arg1": () => Bun.spawn(["true", "a\0b"]),
	"null byte argv0": () => Bun.spawn(["true"], { argv0: "a\0b" }),
	"null byte cwd": () => Bun.spawn(["true"], { cwd: "/tmp\0x" }),
	"null byte env value": () => Bun.spawn(["true"], { env: { A: "x\0y" } }),
	"terminal number": () => Bun.spawn(["true"], { terminal: 5 }),
	"terminal string": () => Bun.spawn(["true"], { terminal: "x" }),
	"onExit not fn": () => Bun.spawn(["true"], { onExit: 5 }),
	"killSignal 0": () => Bun.spawn(["true"], { killSignal: 0 }),
	"killSignal bogus": () => Bun.spawn(["true"], { killSignal: "SIGBOGUS" }),
	"timeout NaN": () => Bun.spawn(["true"], { timeout: NaN }),
	"timeout negative": () => Bun.spawn(["true"], { timeout: -1 }),
	"signal not AbortSignal": () => Bun.spawn(["true"], { signal: {} }),
	"aborted signal": () => Bun.spawn(["true"], { signal: AbortSignal.abort() }),
	"blob stdout": () => Bun.spawn(["true"], { stdout: new Blob(["x"]) }),
};
for (const [n, f] of Object.entries(V)) add("validate " + n, async () => {
	try { const p = f(); const r = await p.exited; return { returned: true, exited: r }; } catch (e) { return errShape(e); }
});

// ---- basic I/O and the result object ----
add("echo via Response", async () => {
	const p = Bun.spawn(["echo", "hello", "world"], { stdout: "pipe", stderr: "pipe", windowsHide: true });
	const [o, e, x] = await Promise.all([new Response(p.stdout).text(), new Response(p.stderr).text(), p.exited]);
	return { o, e, x, ...status(p), pidOk: Number.isInteger(p.pid) && p.pid > 0 };
});
add("stdout.text direct", async () => {
	const p = Bun.spawn(["sh", "-c", "printf 'a\\nb'; printf err >&2"], { stderr: "pipe" });
	const [o, e, x] = await Promise.all([p.stdout.text(), p.stderr.text(), p.exited]);
	return { o, e, x };
});
add("stream methods", async () => {
	const p = Bun.spawn(["sh", "-c", "printf '{\"a\":1}'"]);
	const s = p.stdout;
	return { proto: ["text", "json", "bytes", "arrayBuffer", "blob"].map((m) => typeof s[m]), json: await s.json(), x: await p.exited };
});
add("bytes chunks", async () => {
	const p = Bun.spawn(["printf", "xyz"]);
	const b = await p.stdout.bytes();
	return { ctor: b.constructor.name, len: b.length, s: Buffer.from(b).toString() };
});
add("reader chunks Uint8Array", async () => {
	const p = Bun.spawn(["printf", "xyz"]);
	const r = p.stdout.getReader();
	let ctor = null, total = "";
	for (;;) { const { done, value } = await r.read(); if (done) break; ctor ??= value.constructor.name; total += Buffer.from(value).toString(); }
	return { ctor, total };
});
add("defaults", async () => {
	const p = Bun.spawn(["sh", "-c", "cat; echo rc=$?"]);
	const o = await p.stdout.text();
	return { o, stdin: typeOf(p.stdin), stdout: "ReadableStream", stderr: typeOf(p.stderr), stdio: p.stdio, terminal: typeOf(p.terminal), x: await p.exited };
});
add("ignore and inherit getters", async () => {
	const p = Bun.spawn(["true"], { stdin: "ignore", stdout: "ignore", stderr: "inherit" });
	return { stdin: typeOf(p.stdin), stdout: typeOf(p.stdout), stderr: typeOf(p.stderr), readable: typeOf(p.readable), x: await p.exited };
});
add("same stream object", async () => {
	const p = Bun.spawn(["true"]);
	const same = p.stdout === p.stdout && p.readable === p.stdout;
	await p.exited;
	return { same };
});
add("object form", async () => {
	const p = Bun.spawn({ cmd: ["echo", "obj"], stdout: "pipe" });
	return { o: await p.stdout.text(), x: await p.exited };
});
add("coerced args", async () => {
	const p = Bun.spawn(["echo", 1, true, null, undefined]);
	return { o: await p.stdout.text() };
});
add("toStringTag", async () => {
	const p = Bun.spawn(["true"]);
	await p.exited;
	return { tag: Object.prototype.toString.call(p), ctor: p.constructor?.name, kinds: ["pid", "exited", "kill", "unref", "ref", "killed", "exitCode", "signalCode", "stdout"].map((k) => k in p) };
});
add("nonzero exit", async () => {
	const p = Bun.spawn(["sh", "-c", "exit 7"]);
	const before = status(p);
	return { before, x: await p.exited, after: status(p) };
});
add("exited identity", async () => {
	const p = Bun.spawn(["sh", "-c", "exit 3"]);
	const a = p.exited, b = p.exited;
	const r = [a === b, await a];
	return { pendingSame: r[0], v: r[1], again: await p.exited, afterSame: p.exited === p.exited };
});
add("large output unread until exit", async () => {
	const p = Bun.spawn(["sh", "-c", "head -c 3000000 /dev/zero | tr '\\0' a"]);
	const x = await p.exited;
	const t = await p.stdout.text();
	return { x, len: t.length, allA: /^a*$/.test(t) };
});
add("large stderr unread until exit", async () => {
	const p = Bun.spawn(["sh", "-c", "head -c 1000000 /dev/zero >&2"], { stdout: "ignore", stderr: "pipe" });
	const x = await p.exited;
	return { x, len: (await p.stderr.bytes()).length };
});
add("stdin pipe", async () => {
	const p = Bun.spawn(["cat"], { stdin: "pipe" });
	const w = p.stdin.write("abc");
	const w2 = p.stdin.write(new Uint8Array([100, 101]));
	p.stdin.flush();
	p.stdin.end();
	return { w, w2, o: await p.stdout.text(), x: await p.exited };
});
add("stdin from Bun.file", async () => {
	const f = join(WORK, "in.txt");
	writeFileSync(f, "from-file\n");
	const p = Bun.spawn(["cat"], { stdin: Bun.file(f) });
	return { o: await p.stdout.text(), x: await p.exited };
});
add("stdin from fd", async () => {
	const f = join(WORK, "in2.txt");
	writeFileSync(f, "from-fd\n");
	const fd = openSync(f, "r");
	const p = Bun.spawn(["cat"], { stdin: fd });
	const r = { o: await p.stdout.text(), x: await p.exited };
	closeSync(fd);
	return r;
});

// ---- exit status, signals, kill ----
add("kill default", async () => {
	const p = Bun.spawn(["sleep", "30"]);
	const before = status(p);
	const r = p.kill();
	return { before, r, x: await p.exited, after: status(p) };
});
for (const sig of ["SIGKILL", 9, "SIGINT", 15, "SIGHUP", "", NaN]) add(`kill ${JSON.stringify(sig) ?? String(sig)} ${typeof sig}`, async () => {
	const p = Bun.spawn(["sleep", "30"]);
	const r = p.kill(sig);
	return { r, x: await p.exited, after: status(p) };
});
for (const sig of ["SIGBOGUS", 99, -1, 1.5, Infinity, {}, "sigterm", "TERM"]) add(`kill invalid ${String(sig)}`, async () => {
	const p = Bun.spawn(["sleep", "30"]);
	let r;
	try { r = { returned: p.kill(sig) }; } catch (e) { r = errShape(e); }
	p.kill("SIGKILL");
	return { r, x: await p.exited, after: status(p) };
});
add("kill 0 on live", async () => {
	const p = Bun.spawn(["sleep", "30"]);
	const r = p.kill(0);
	const mid = status(p);
	p.kill(9);
	return { r, mid, x: await p.exited };
});
add("kill after exit", async () => {
	const p = Bun.spawn(["true"]);
	await p.exited;
	let r;
	try { r = { returned: p.kill() }; } catch (e) { r = errShape(e); }
	return { r, after: status(p) };
});
add("self-signalled", async () => {
	const p = Bun.spawn(["sh", "-c", "kill -TERM $$"]);
	return { x: await p.exited, after: status(p) };
});
add("signalled SIGSEGV", async () => {
	const p = Bun.spawn(["sh", "-c", "kill -SEGV $$"]);
	return { x: await p.exited, after: status(p) };
});
add("exit 255", async () => {
	const p = Bun.spawn(["sh", "-c", "exit 255"]);
	return { x: await p.exited, after: status(p) };
});
add("onExit callback", async () => {
	let got;
	const done = new Promise((r) => { got = r; });
	const p = Bun.spawn(["sh", "-c", "exit 4"], { onExit(proc, code, sig, err) { got({ same: proc === p, code, sig, err: err === undefined ? "undefined" : String(err), n: arguments.length }); } });
	const a = await done;
	return { a, x: await p.exited };
});
add("onExit on kill", async () => {
	let got;
	const done = new Promise((r) => { got = r; });
	const p = Bun.spawn(["sleep", "30"], { onExit: (proc, code, sig) => got({ code, sig }) });
	p.kill();
	return { a: await done, x: await p.exited };
});
add("timeout default killSignal", async () => {
	const t0 = Date.now();
	const p = Bun.spawn(["sleep", "30"], { timeout: 200 });
	const x = await p.exited;
	return { x, after: status(p), quick: Date.now() - t0 < 5000 };
});
add("timeout killSignal SIGKILL", async () => {
	const p = Bun.spawn(["sleep", "30"], { timeout: 200, killSignal: "SIGKILL" });
	return { x: await p.exited, after: status(p) };
});
add("timeout killSignal number", async () => {
	const p = Bun.spawn(["sleep", "30"], { timeout: 200, killSignal: 2 });
	return { x: await p.exited, after: status(p) };
});
add("abort signal", async () => {
	const ac = new AbortController();
	const p = Bun.spawn(["sleep", "30"], { signal: ac.signal });
	setTimeout(() => ac.abort(), 100);
	return { x: await p.exited, after: status(p) };
});
add("abort signal with killSignal", async () => {
	const ac = new AbortController();
	const p = Bun.spawn(["sleep", "30"], { signal: ac.signal, killSignal: "SIGKILL" });
	setTimeout(() => ac.abort(), 100);
	return { x: await p.exited, after: status(p) };
});
add("maxBuffer", async () => {
	const p = Bun.spawn(["sh", "-c", "head -c 100000 /dev/zero; sleep 30"], { maxBuffer: 1000 });
	const x = await p.exited;
	return { x, after: status(p) };
});
add("async dispose", async () => {
	const p = Bun.spawn(["sleep", "30"]);
	const r = await p[Symbol.asyncDispose]();
	return { r, after: status(p) };
});

// ---- argv0, cwd, env, detached ----
add("argv0 cmdline", async () => {
	const p = Bun.spawn(["sleep", "5"], { argv0: "claude bg-pty-host" });
	const c = await cmdlineOf(p.pid, ownCmdline);
	p.kill(9);
	await p.exited;
	return { c };
});
add("argv0 visible as $0", async () => {
	const p = Bun.spawn(["sh", "-c", "echo $0"], { argv0: "pretend-sh" });
	return { o: await p.stdout.text() };
});
add("argv0 empty string ignored", async () => {
	const p = Bun.spawn(["sh", "-c", "echo $0"], { argv0: "" });
	return { o: await p.stdout.text() };
});
add("argv0 resolves real binary", async () => {
	// The exec'd image is the real sh found on PATH, whatever argv[0] says. The
	// path itself is host-specific (dash on Ubuntu, bash on Haiku), so the
	// observation is the relation, with the raw output kept when it fails.
	const p = Bun.spawn(["sh", "-c", OBS.childExeCmd], { argv0: "nonexistent-name" });
	const exe = OBS.parseChildExe(await p.stdout.text());
	let real = null;
	try { real = realpathSync(exe); } catch {}
	const sh = onPath("sh");
	const isSh = real !== null && sh !== null && real === realpathSync(sh);
	return isSh ? { exeIsRealSh: true } : { exeIsRealSh: false, exe, real, sh };
});
add("no argv0 cmdline keeps cmd[0]", async () => {
	const p = Bun.spawn(["sleep", "5"]);
	const c = await cmdlineOf(p.pid, ownCmdline);
	p.kill(9);
	await p.exited;
	return { c };
});
add("cwd", async () => {
	const p = Bun.spawn(["pwd"], { cwd: "/tmp" });
	// pwd prints the physical directory; /tmp is a symlink on Haiku.
	const o = await p.stdout.text();
	return { o: o === realpathSync("/tmp") + "\n" ? "<realpath /tmp>\n" : o };
});
add("cwd relative binary", async () => {
	const d = join(WORK, "bin");
	mkdirSync(d, { recursive: true });
	writeFileSync(join(d, "hi.sh"), "#!/bin/sh\necho relhi\n");
	chmodSync(join(d, "hi.sh"), 0o755);
	const p = Bun.spawn(["./hi.sh"], { cwd: d });
	return { o: await p.stdout.text(), x: await p.exited };
});
add("PATH lookup uses env PATH", async () => {
	const d = join(WORK, "pbin");
	mkdirSync(d, { recursive: true });
	writeFileSync(join(d, "only-here"), "#!/bin/sh\necho found-in-env-path\n");
	chmodSync(join(d, "only-here"), 0o755);
	const p = Bun.spawn(["only-here"], { env: { PATH: d + ":/usr/bin:/bin" } });
	return { o: await p.stdout.text(), x: await p.exited };
});
add("PATH relative segment against cwd", async () => {
	const d = join(WORK, "rel");
	mkdirSync(join(d, "sub"), { recursive: true });
	writeFileSync(join(d, "sub", "relcmd"), "#!/bin/sh\necho rel-segment\n");
	chmodSync(join(d, "sub", "relcmd"), 0o755);
	const p = Bun.spawn(["relcmd"], { cwd: d, env: { PATH: "sub:/usr/bin:/bin" } });
	return { o: await p.stdout.text(), x: await p.exited };
});
add("env replaces", async () => {
	const p = Bun.spawn([OBS.envBin], { env: { FOO: "bar", NUM: 5, U: undefined, N: null, B: false, E: "" } });
	return { o: (await p.stdout.text()).split("\n").sort() };
});
add("env without PATH uses default path", async () => {
	const p = Bun.spawn(["env"], { env: { ONLY: "1" } });
	return { o: await p.stdout.text(), x: await p.exited };
});
add("env PATH empty uses default path", async () => {
	const p = Bun.spawn(["env"], { env: { PATH: "", Z: "1" } });
	return { o: (await p.stdout.text()).split("\n").sort(), x: await p.exited };
});
add("env PATH no match", async () => {
	try { Bun.spawn(["env"], { env: { PATH: "/nonexistent-dir" } }); return { returned: true }; } catch (e) { return errShape(e); }
});
add("env inherited includes process.env mutation", async () => {
	process.env.CC_SPAWN_MUTATED = "yes";
	const p = Bun.spawn(["sh", "-c", "echo $CC_SPAWN_MUTATED"]);
	const o = await p.stdout.text();
	delete process.env.CC_SPAWN_MUTATED;
	return { o };
});
add("env inherited sees deletion", async () => {
	process.env.CC_SPAWN_DEL = "x";
	delete process.env.CC_SPAWN_DEL;
	const p = Bun.spawn(["sh", "-c", "echo [${CC_SPAWN_DEL-unset}]"]);
	return { o: await p.stdout.text() };
});
add("env spread", async () => {
	const p = Bun.spawn(["sh", "-c", "echo $TERM:$HOME"], { env: { ...process.env, TERM: "xterm-256color" } });
	// HOME is the host's (/root, /boot/home); it is compared as a placeholder.
	const home = process.env.HOME;
	return { o: (await p.stdout.text()).split(home).join("<HOME>"), home: home ? "<HOME>" : home };
});
add("env proto keys ignored", async () => {
	const env = Object.create({ INHERITED: "1" });
	env.OWN = "2";
	const p = Bun.spawn([OBS.envBin], { env });
	return { o: (await p.stdout.text()).split("\n").sort() };
});
add("env symbol and nonenumerable", async () => {
	const env = { A: "1", [Symbol("s")]: "x" };
	Object.defineProperty(env, "HIDDEN", { value: "h", enumerable: false });
	const p = Bun.spawn([OBS.envBin], { env });
	return { o: (await p.stdout.text()).split("\n").sort() };
});
add("detached session", async () => {
	const p = Bun.spawn(["sh", "-c", OBS.childSessionCmd], { detached: true });
	const sid = OBS.parseChildSession(await p.stdout.text());
	return { sidIsChild: sid === String(p.pid), sidIsParent: sid === selfSid };
});
add("not detached session", async () => {
	const p = Bun.spawn(["sh", "-c", OBS.childSessionCmd], { detached: false });
	const sid = OBS.parseChildSession(await p.stdout.text());
	return { sidIsChild: sid === String(p.pid), sidIsParent: sid === selfSid };
});
add("detached non-boolean ignored", async () => {
	const p = Bun.spawn(["sh", "-c", OBS.childSessionCmd], { detached: 1 });
	const sid = OBS.parseChildSession(await p.stdout.text());
	return { sidIsChild: sid === String(p.pid), sidIsParent: sid === selfSid };
});

// ---- stdio array, Bun.file and fd targets ----
add("stdio Bun.file stderr", async () => {
	const f = join(WORK, "crumb.log");
	const p = Bun.spawn(["sh", "-c", "echo to-err >&2; echo to-out"], { stdio: ["ignore", "ignore", Bun.file(f)] });
	const x = await p.exited;
	return { x, content: readFileSync(f, "utf8"), mode: (statSync(f).mode & 0o777).toString(8), stdout: typeOf(p.stdout), stderr: typeOf(p.stderr) };
});
add("stdio Bun.file existing not truncated", async () => {
	const f = join(WORK, "existing.log");
	writeFileSync(f, "0123456789ABCDEF\n");
	chmodSync(f, 0o600);
	const p = Bun.spawn(["sh", "-c", "printf xy >&2"], { stdio: ["ignore", "ignore", Bun.file(f)] });
	await p.exited;
	return { content: readFileSync(f, "utf8"), mode: (statSync(f).mode & 0o777).toString(8) };
});
add("stdio Bun.file stdout", async () => {
	const f = join(WORK, "out.log");
	const p = Bun.spawn(["echo", "into-file"], { stdio: ["ignore", Bun.file(f), "ignore"] });
	await p.exited;
	return { content: readFileSync(f, "utf8"), stdout: typeOf(p.stdout) };
});
add("stdout option Bun.file", async () => {
	const f = join(WORK, "out2.log");
	const p = Bun.spawn(["echo", "opt-file"], { stdout: Bun.file(f) });
	await p.exited;
	return { content: readFileSync(f, "utf8") };
});
add("stdio Bun.file missing dir", async () => {
	try {
		const p = Bun.spawn(["echo", "x"], { stdio: ["ignore", "ignore", Bun.file(join(WORK, "no", "such", "dir", "f.log"))] });
		return { returned: true, x: await p.exited };
	} catch (e) { return errShape(e); }
});
add("stdio Bun.file missing dir with argv0", async () => {
	try {
		const p = Bun.spawn(["echo", "x"], { argv0: "claude bg-pty-host", stdio: ["ignore", "ignore", Bun.file(join(WORK, "no", "f.log"))] });
		return { returned: true, x: await p.exited };
	} catch (e) { return errShape(e); }
});
add("stdio Bun.file is a directory", async () => {
	try {
		const p = Bun.spawn(["echo", "x"], { stdio: ["ignore", "ignore", Bun.file(WORK)] });
		return { returned: true, x: await p.exited };
	} catch (e) { return errShape(e); }
});
add("stdio Bun.file read-only fs", async () => {
	// A read-only mount: a squashfs snap on Ubuntu, packagefs on Haiku.
	const dir = OBS.readOnlyDir();
	if (!dir) throw new Error("no read-only filesystem found on this host");
	try {
		const p = Bun.spawn(["echo", "x"], { stdio: ["ignore", "ignore", Bun.file(join(dir, "cc-spawn-new.log"))] });
		return { returned: true, x: await p.exited };
	} catch (e) { const s = errShape(e); s.message = s.message?.replace(dir, "<rofs>"); s.path = s.path?.replace(dir, "<rofs>"); return s; }
});
add("stdio missing dir and bad cwd", async () => {
	try {
		const f = join(WORK, "cwdfile"); writeFileSync(f, "x");
		Bun.spawn(["echo", "x"], { cwd: f, stdio: ["ignore", "ignore", Bun.file(join(WORK, "no", "f.log"))] });
		return { returned: true };
	} catch (e) { return errShape(e); }
});
add("stdio numeric fd", async () => {
	const f = join(WORK, "fd.log");
	const fd = openSync(f, "w");
	const p = Bun.spawn(["sh", "-c", "echo via-fd >&2"], { stdio: ["ignore", "ignore", fd] });
	await p.exited;
	closeSync(fd);
	return { content: readFileSync(f, "utf8"), stderr: typeOf(p.stderr) };
});
add("stdio fd closed by parent right after spawn", async () => {
	const f = join(WORK, "fd2.log");
	const fd = openSync(f, "w");
	const p = Bun.spawn(["sh", "-c", "sleep 0.2; echo late >&2"], { stdio: ["ignore", "ignore", fd] });
	closeSync(fd);
	await p.exited;
	return { content: readFileSync(f, "utf8") };
});
add("stdio inherit fd 2 number", async () => {
	const p = Bun.spawn(["true"], { stdio: ["ignore", "pipe", 2] });
	return { stderr: typeOf(p.stderr), x: await p.exited };
});
add("stdio short array keeps defaults", async () => {
	const p = Bun.spawn(["sh", "-c", "echo short"], { stdio: ["ignore"] });
	return { stdout: typeOf(p.stdout), o: p.stdout ? await p.stdout.text() : null, stderr: typeOf(p.stderr) };
});
add("stdio null entries", async () => {
	const p = Bun.spawn(["sh", "-c", "echo n"], { stdio: [null, null, null] });
	return { stdout: typeOf(p.stdout), x: await p.exited };
});
add("stdio extra ignore slot", async () => {
	// An "ignore" slot past stderr is left closed in the child (a /dev/null there
	// would show as fd 3). The child's fd table is read from outside while it runs.
	const p = Bun.spawn(["sleep", "5"], { stdio: ["ignore", "pipe", "ignore", "ignore"] });
	const c = await cmdlineOf(p.pid, ownCmdline);
	// Both kernels publish the new argv before they close the close-on-exec fds
	// (Linux begin_new_exec: exec_mmap, then do_close_on_exec; Haiku exec_team:
	// SetArgs, then vfs_exec_io_context), so the table is read once exec settled.
	await sleep(300);
	const fds = OBS.fdsOf(p.pid);
	p.kill(9);
	await p.exited;
	return { exec: c === "TIMEOUT" ? c : "exec'd", fds, stdio: p.stdio.length };
});
add("stdio extra Bun.file slot", async () => {
	const f = join(WORK, "extra.log");
	const p = Bun.spawn(["sh", "-c", "echo extra >&3"], { stdio: ["ignore", "ignore", "ignore", Bun.file(f)] });
	await p.exited;
	return { content: readFileSync(f, "utf8") };
});
add("stdio overrides stdout option", async () => {
	const p = Bun.spawn(["echo", "x"], { stdio: ["ignore", "ignore", "ignore"], stdout: "pipe" });
	return { stdout: typeOf(p.stdout) };
});

// ---- spawn failures ----
const fail = (name, fn) => add(name, async () => { try { const p = fn(); return { returned: true, x: await p.exited }; } catch (e) { return errShape(e); } });
fail("missing bare binary", () => Bun.spawn(["definitely-not-a-cmd-xyz"]));
fail("missing bare binary with argv0", () => Bun.spawn(["definitely-not-a-cmd-xyz"], { argv0: "shown" }));
fail("missing absolute binary", () => Bun.spawn(["/nonexistent/bin/tool"]));
fail("missing absolute binary with argv0", () => Bun.spawn(["/nonexistent/bin/tool"], { argv0: "claude bg-pty-host" }));
fail("missing relative binary", () => Bun.spawn(["./nope-here"], { cwd: WORK }));
fail("non-executable file", () => { const f = join(WORK, "noexec.sh"); writeFileSync(f, "#!/bin/sh\n"); chmodSync(f, 0o644); return Bun.spawn([f]); });
fail("directory as binary", () => Bun.spawn([WORK]));
fail("cwd missing", () => Bun.spawn(["true"], { cwd: join(WORK, "missing-cwd") }));
fail("cwd is a file", () => { const f = join(WORK, "afile"); writeFileSync(f, "x"); return Bun.spawn(["true"], { cwd: f }); });
fail("cwd is a file with argv0", () => Bun.spawn(["true"], { cwd: join(WORK, "afile"), argv0: "pretend" }));
fail("cwd empty string ignored", () => Bun.spawn(["true"], { cwd: "" }));
fail("bad interpreter", () => { const f = join(WORK, "badinterp.sh"); writeFileSync(f, "#!/no/such/interp\n"); chmodSync(f, 0o755); return Bun.spawn([f]); });
add("missing binary with code check like the bundle", async () => {
	try { Bun.spawn(["/nonexistent/x"], { stdio: ["ignore", "ignore", "ignore"] }); return { returned: true }; } catch (g) {
		const f = g instanceof Error && "code" in g ? String(g.code) : "EIO";
		return { f, inList: ["ENOENT", "ENOSPC", "EACCES", "EROFS"].includes(f) };
	}
});

// ---- terminal ----
add("terminal options object", async () => {
	try { const p = Bun.spawn(["true"], { terminal: {} }); const t = typeOf(p.terminal); p.terminal?.close?.(); return { returned: true, terminal: t, x: await p.exited }; } catch (e) { return errShape(e); }
});
add("terminal falsy ignored", async () => {
	const p = Bun.spawn(["echo", "t"], { terminal: false });
	return { o: await p.stdout.text(), terminal: typeOf(p.terminal) };
});

// ---- ref/unref keep-alive, observed from a separate runtime process ----
// The helper's stderr is not captured: its sleeper may inherit it, and a pipe
// held open by the sleeper would measure the sleeper rather than the helper.
const helper = join(HERE, "spawn-unref-helper.mjs");
for (const mode of ["ref", "unref", "unref-pipe", "unref-pipe-read", "unref-pipe-text", "unref-ignore", "ref-after-unref", "unref-stdin-pipe"]) add(`keepalive ${mode}`, async () => {
	const t0 = Date.now();
	const p = Bun.spawn([...SELF, helper, mode], { stdout: "pipe", stderr: "ignore" });
	const x = await p.exited;
	const ms = Date.now() - t0;
	// The helper's child sleeps 3 s; a helper that waits for it takes >= 3 s.
	return { o: await p.stdout.text(), x, waitedForChild: ms >= 2900 };
});


// ---- more option validation ----
const V2 = {
	"timeout string": { timeout: "100" }, "timeout fraction": { timeout: 1.5 }, "timeout -Infinity": { timeout: -Infinity },
	"timeout huge": { timeout: 2 ** 60 }, "timeout null": { timeout: null }, "timeout 0": { timeout: 0 }, "timeout Infinity": { timeout: Infinity },
	"uid string": { uid: "x" }, "uid fraction": { uid: 1.5 }, "uid huge": { uid: 2 ** 40 }, "gid string": { gid: "x" },
	"killSignal null": { killSignal: null }, "killSignal NaN": { killSignal: NaN }, "killSignal 99": { killSignal: 99 }, "killSignal object": { killSignal: {} },
	"maxBuffer string": { maxBuffer: "5" }, "maxBuffer 0": { maxBuffer: 0 }, "maxBuffer Infinity": { maxBuffer: Infinity },
	"env false": { env: false }, "env empty string": { env: "" }, "env null": { env: null }, "env array": { env: ["A=1"] },
	"argv0 number": { argv0: 5 }, "cwd number": { cwd: 5 }, "signal false": { signal: false }, "signal null": { signal: null }, "signal 5": { signal: 5 },
	"onExit null": { onExit: null }, "onExit false": { onExit: false }, "onDisconnect 5": { onDisconnect: 5 },
	"terminal false": { terminal: false }, "terminal 0": { terminal: 0 }, "terminal null": { terminal: null }, "terminal empty string": { terminal: "" },
	"stdio empty array": { stdio: [] }, "stdio null": { stdio: null }, "stdio object": { stdio: {} },
	"stdin number 2147483647": { stdin: 2147483647 }, "stdout overlapped": { stdout: "overlapped" }, "stdio socket-fd extra": null,
	"lazy string": { lazy: "yes" }, "detached string": { detached: "yes" }, "windowsHide string": { windowsHide: "x" },
	"stdout ArrayBuffer": { stdout: new Uint8Array([1]) }, "stdio Blob at extra": { stdio: ["ignore", "ignore", "ignore", new Blob(["x"])] },
	"stdin empty Blob": { stdin: new Blob([]) }, "stdin empty ArrayBuffer": { stdin: new ArrayBuffer(0) },
	"stdio ignore at 5": { stdio: ["ignore", "ignore", "ignore", undefined, null, "ignore"] },
};
for (const [n, o] of Object.entries(V2)) {
	if (o === null) continue;
	add("validate2 " + n, async () => {
		try { const p = Bun.spawn(["sh", "-c", "echo ok"], o); const r = await p.exited; return { returned: true, exited: r, stdout: typeOf(p.stdout), stdin: typeOf(p.stdin), stdio: p.stdio }; } catch (e) { return errShape(e); }
	});
}
add("stdin Blob", async () => {
	const p = Bun.spawn(["cat"], { stdin: new Blob(["blob-in"]) });
	return { stdin: typeOf(p.stdin), o: await p.stdout.text(), x: await p.exited };
});
add("stdin Uint8Array", async () => {
	const p = Bun.spawn(["cat"], { stdin: new TextEncoder().encode("u8-in") });
	return { stdin: typeOf(p.stdin), o: await p.stdout.text(), x: await p.exited };
});
add("stdin Bun.file missing is created", async () => {
	const f = join(WORK, "created-by-stdin");
	const p = Bun.spawn(["cat"], { stdin: Bun.file(f) });
	return { o: await p.stdout.text(), x: await p.exited, exists: existsSync(f) };
});
add("FileSink results", async () => {
	const p = Bun.spawn(["cat"], { stdin: "pipe" });
	const s = p.stdin;
	const w = s.write("hello ");
	const f = s.flush();
	const w2 = s.write("x".repeat(100));
	const e = s.end();
	const fr = f instanceof Promise ? ["promise", await f] : [typeof f, f];
	const er = e instanceof Promise ? ["promise", await e] : [typeof e, e];
	return { w, w2, fr, er, writable: p.writable === s, o: (await p.stdout.text()).length, x: await p.exited, methods: ["write", "flush", "end", "ref", "unref", "start"].map((m) => typeof s[m]) };
});
add("ipc members without ipc", async () => {
	const p = Bun.spawn(["true"]);
	let send;
	try { send = { returned: p.send("x") }; } catch (e) { send = errShape(e); }
	let disc;
	try { disc = { returned: p.disconnect() }; } catch (e) { disc = errShape(e); }
	const r = { connected: p.connected, send, disc, x: await p.exited };
	return r;
});
add("stdio extra numeric fd value", async () => {
	const f = join(WORK, "x3.log");
	const fd = openSync(f, "w");
	const p = Bun.spawn(["sh", "-c", "echo three >&3"], { stdio: ["ignore", "ignore", "ignore", fd] });
	const v = p.stdio[3] === fd;
	await p.exited;
	closeSync(fd);
	return { v, len: p.stdio.length, content: readFileSync(f, "utf8") };
});
add("stdio extra Bun.file value", async () => {
	const p = Bun.spawn(["true"], { stdio: ["ignore", "ignore", "ignore", Bun.file(join(WORK, "x4.log")), "ignore"] });
	await p.exited;
	return { stdio: p.stdio };
});
add("stdio fd for stdout getter", async () => {
	const f = join(WORK, "x5.log");
	const fd = openSync(f, "w");
	const p = Bun.spawn(["echo", "five"], { stdout: fd });
	const same = p.stdout === fd;
	await p.exited;
	closeSync(fd);
	return { same, content: readFileSync(f, "utf8") };
});
add("Bun.file by fd as stdout", async () => {
	const f = join(WORK, "x6.log");
	const fd = openSync(f, "w");
	const p = Bun.spawn(["echo", "six"], { stdout: Bun.file(fd) });
	const t = typeOf(p.stdout);
	await p.exited;
	closeSync(fd);
	return { t, content: readFileSync(f, "utf8") };
});
add("Bun.file(1) as stdout inherits", async () => {
	const p = Bun.spawn(["true"], { stdout: Bun.file(1) });
	return { t: typeOf(p.stdout), x: await p.exited };
});
for (const n of [999, 1000, 1001]) add(`maxBuffer boundary ${n} bytes into 1000`, async () => {
	const p = Bun.spawn(["sh", "-c", `head -c ${n} /dev/zero; sleep 2`], { maxBuffer: 1000 });
	const x = await p.exited;
	return { x, after: status(p) };
});
add("maxBuffer stderr", async () => {
	const p = Bun.spawn(["sh", "-c", "head -c 5000 /dev/zero >&2; sleep 30"], { stderr: "pipe", maxBuffer: 100 });
	return { x: await p.exited, after: status(p) };
});
add("maxBuffer ignored without pipes", async () => {
	const p = Bun.spawn(["sh", "-c", "head -c 5000 /dev/zero"], { stdout: "ignore", maxBuffer: 10 });
	return { x: await p.exited };
});
add("env snapshot: overwrite preset", async () => {
	process.env.CC_SPAWN_PRESET = "changed";
	const p = Bun.spawn(["sh", "-c", "echo [$CC_SPAWN_PRESET]"]);
	return { o: await p.stdout.text() };
});
add("env snapshot: delete preset", async () => {
	delete process.env.CC_SPAWN_PRESET2;
	const p = Bun.spawn(["sh", "-c", "echo [${CC_SPAWN_PRESET2-unset}]"]);
	return { o: await p.stdout.text(), seen: process.env.CC_SPAWN_PRESET2 ?? "deleted" };
});
add("env snapshot: PATH lookup uses changed PATH", async () => {
	const d = join(WORK, "pbin2");
	mkdirSync(d, { recursive: true });
	writeFileSync(join(d, "only-in-new-path"), "#!/bin/sh\necho found\n");
	chmodSync(join(d, "only-in-new-path"), 0o755);
	const old = process.env.PATH;
	process.env.PATH = d + ":" + old;
	let r;
	try { const p = Bun.spawn(["only-in-new-path"]); r = { o: await p.stdout.text() }; } catch (e) { r = errShape(e); }
	process.env.PATH = old;
	return r;
});
add("exited after kill then await twice", async () => {
	const p = Bun.spawn(["sleep", "30"]);
	p.kill("SIGKILL");
	const [a, b] = await Promise.all([p.exited, p.exited]);
	return { a, b };
});
add("resourceUsage", async () => {
	const p = Bun.spawn(["true"]);
	const before = typeof p.resourceUsage();
	await p.exited;
	let after;
	try { const r = p.resourceUsage(); after = { t: typeof r, keys: r && Object.keys(Object.getPrototypeOf(r)).sort() }; } catch (e) { after = errShape(e); }
	return { before, after };
});
add("cgroup option", async () => {
	try { const p = Bun.spawn(["true"], { cgroup: "/sys/fs/cgroup" }); return { returned: true, x: await p.exited }; } catch (e) { return errShape(e); }
});
add("ipc option", async () => {
	try { const p = Bun.spawn(["true"], { ipc() {} }); return { returned: true, x: await p.exited }; } catch (e) { return errShape(e); }
});
add("stdio extra pipe", async () => {
	try { const p = Bun.spawn(["true"], { stdio: ["ignore", "ignore", "ignore", "pipe"] }); const t = typeof p.stdio[3]; await p.exited; return { returned: true, t }; } catch (e) { return errShape(e); }
});
add("cmd array-like object", async () => {
	try { const p = Bun.spawn({ cmd: { length: 2, 0: "echo", 1: "al" } }); return { o: await p.stdout.text() }; } catch (e) { return errShape(e); }
});
add("cmd with hole", async () => {
	const p = Bun.spawn(["echo", , "z"]);
	return { o: await p.stdout.text() };
});
add("cmd symbol arg", async () => {
	try { Bun.spawn(["echo", Symbol("s")]); return { returned: true }; } catch (e) { return errShape(e); }
});
add("second arg ignored with object form", async () => {
	const p = Bun.spawn({ cmd: ["sh", "-c", "pwd"] }, { cwd: "/" });
	return { o: await p.stdout.text() };
});

add("lazy stdout", async () => {
	const p = Bun.spawn(["sh", "-c", "echo lazy-out; echo lazy-err >&2"], { lazy: true, stderr: "pipe" });
	const x = await p.exited;
	return { x, o: await p.stdout.text(), e: await p.stderr.text() };
});
add("stdout read after exit and stream end", async () => {
	const p = Bun.spawn(["sh", "-c", "printf abc"]);
	await p.exited;
	await new Promise((r) => setTimeout(r, 100));
	return { o: await new Response(p.stdout).text() };
});

// ---- run ----
const out = [];
for (const [name, fn] of S) {
	let r;
	try { r = await fn(); } catch (e) { r = { scenarioThrew: errShape(e) }; }
	out.push([name, r]);
}
// Host-specific absolute paths (the work dir, this directory) become placeholders,
// both as given and fully resolved; longest first so a prefix never wins.
const hereDir = HERE.replace(/\/$/, "");
const places = [[WORK, "<WORK>"], [WORK_ARG.replace(/\/$/, ""), "<WORK>"], [hereDir, "<HERE>"], [realpathSync(hereDir), "<HERE>"]].sort((a, b) => b[0].length - a[0].length);
let text = JSON.stringify(out, null, 1);
for (const [from, to] of places) text = text.split(from).join(to);
// What the checker needs to tell a platform difference from a defect: the
// platform, and its own errno and signal tables (as this runtime reports them).
const meta = ["__platform", { platform: PLATFORM, runtime: isBun ? "bun" : "node", errno: os.constants.errno, signals: os.constants.signals }];
writeFileSync(OUT, JSON.stringify([meta, ...JSON.parse(text)], null, 1));
console.log(`${isBun ? "bun" : "node"}: ${out.length} scenarios -> ${OUT}`);
