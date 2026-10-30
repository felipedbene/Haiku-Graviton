// Bun.which(command, {PATH?, cwd?}) -- port of Bun bun-v1.4.4 src/which/lib.rs
// (POSIX branch) and its binding in src/runtime/api/BunObject.rs/.bind.ts.
// Defaults as in Bun: PATH from the environment, cwd = the directory the
// process started in (Bun's top_level_dir), not the current process.cwd().
import { statSync, accessSync, constants } from "node:fs";
import { isAbsolute } from "node:path";

const TOP_LEVEL_DIR = process.cwd();
// bun_paths::MAX_PATH_BYTES is the platform PATH_MAX (B_PATH_NAME_LENGTH on Haiku).
const MAX_PATH_BYTES = process.platform === "haiku" ? 1024 : 4096;
const bytes = (s) => Buffer.byteLength(s, "utf8");

function isExecutableFile(p) {
	try {
		if (!statSync(p).isFile()) return false;
		accessSync(p, constants.X_OK);
		return true;
	} catch {
		return false;
	}
}

// is_valid: `[cwd/]segment/bin`, joined without normalisation.
function isValid(cwd, segment, bin) {
	const withSep = (part) => (part === "" ? "" : part.endsWith("/") ? part : part + "/");
	const p = withSep(cwd) + withSep(segment) + bin;
	if (bytes(p) + 1 > MAX_PATH_BYTES) return null;
	return isExecutableFile(p) ? p : null;
}

function which(path, cwd, bin) {
	if (bytes(bin) >= MAX_PATH_BYTES) return null;
	if (bin === "") return null;
	if (isAbsolute(bin)) return isExecutableFile(bin) ? bin : null;
	let cwdTrimmed = cwd;
	while (cwdTrimmed.length > 1 && cwdTrimmed.endsWith("/")) cwdTrimmed = cwdTrimmed.slice(0, -1);
	if (bin.includes("/")) {
		if (cwd !== "") {
			const r = isValid("", cwdTrimmed, bin.startsWith("./") ? bin.slice(2) : bin);
			if (r) return r;
		}
		return null;
	}
	const cwdForRelative = isAbsolute(cwd) ? cwdTrimmed : "";
	for (const segment of path.split(":")) {
		if (segment === "") continue;	// strings::tokenize skips empty segments
		const r = isValid(isAbsolute(segment) ? "" : cwdForRelative, segment, bin);
		if (r) return r;
	}
	return null;
}

// b.string.loose: ToString unless undefined (null too, for the nullable command).
export function bunWhich(command, options) {
	const cmd = command === undefined || command === null ? "" : String(command);
	if (bytes(cmd) >= MAX_PATH_BYTES) throw new Error("bin path is too long");
	let PATH, cwd;
	if (options !== null && options !== undefined && typeof options === "object") {
		if (options.PATH !== undefined) PATH = String(options.PATH);
		if (options.cwd !== undefined) cwd = String(options.cwd);
	}
	return which(PATH ?? process.env.PATH ?? "", cwd ?? TOP_LEVEL_DIR, cmd);
}
