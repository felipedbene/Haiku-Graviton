// Haiku: exec of a script whose "#!" interpreter does not exist succeeds; the
// child then dies in runtime_loader. Linux execve fails with ENOENT.
// Usage: node shebang.mjs <scratch-dir>
import { writeFileSync, chmodSync, mkdirSync } from "node:fs";
import { spawnSync, spawn } from "node:child_process";
const dir = process.argv[2];
mkdirSync(dir, { recursive: true });
const f = dir + "/badinterp.sh";
writeFileSync(f, "#!/no/such/interp\necho should-not-run\n");
chmodSync(f, 0o755);
const r = spawnSync(f, [], { encoding: "utf8" });
console.log(JSON.stringify({ spawnSync: { error: r.error && { code: r.error.code, errno: r.error.errno, syscall: r.error.syscall }, status: r.status, signal: r.signal, stdout: r.stdout, stderr: r.stderr } }));
const c = spawn(f, [], { stdio: ["ignore", "pipe", "pipe"] });
let err = "";
c.stderr.on("data", (d) => { err += d; });
c.on("error", (e) => console.log(JSON.stringify({ spawn: { errorEvent: { code: e.code, errno: e.errno, syscall: e.syscall } } })));
c.on("close", (code, sig) => console.log(JSON.stringify({ spawn: { pid: typeof c.pid, exitCode: code, signal: sig, stderr: err } })));
