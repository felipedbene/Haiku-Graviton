// Installs compat's Bun.spawn / Bun.file for the spawn diff test under Node
// (the integrator wires the real preload; this keeps the test self-contained).
import { spawn } from "../lib/spawn.mjs";
import { file } from "../lib/misc.mjs";
globalThis.Bun = { spawn, file };
