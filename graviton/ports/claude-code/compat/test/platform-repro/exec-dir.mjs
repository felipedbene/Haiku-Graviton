// The error for exec'ing a directory: EACCES on Linux, EISDIR on Haiku.
// Usage: node exec-dir.mjs <existing-directory>
import { spawnSync } from "node:child_process";
const r = spawnSync(process.argv[2], []);
console.log(JSON.stringify({ platform: process.platform, error: r.error && { code: r.error.code, errno: r.error.errno, syscall: r.error.syscall } }));
