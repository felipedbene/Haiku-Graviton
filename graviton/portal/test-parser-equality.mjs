#!/usr/bin/env node
// Parser-equality test for the DeBeOS package portal (issue #596).
//
// Asserts that the JS HPKR parser SHIPPED IN index.html produces, for every
// package, the same name / version / revision / arch / provides (and requires)
// as graviton/scripts/hpkg_meta.py read_repo on the LIVE index. To avoid drift
// the test does not re-implement the parser: it extracts the exact source
// between the HPKR_PARSER_START / HPKR_PARSER_END sentinels in index.html and
// evals it (DecompressionStream exists in modern Node), so the bytes under test
// are the bytes the browser runs.
//
// Usage:
//   node test-parser-equality.mjs [path-to-repo-index]
// With no argument it downloads https://packages.debene.dev/arm64/repo.
// The Python reference JSON is produced on the fly via hpkg_meta.read_repo_file,
// or supplied through REF_JSON=<file>.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";
import os from "node:os";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const HTML = path.join(__dirname, "index.html");
const SCRIPTS = path.resolve(__dirname, "..", "scripts");
const LIVE_URL = "https://packages.debene.dev/arm64/repo";

function extractParser() {
  const src = fs.readFileSync(HTML, "utf8");
  const a = src.indexOf("/*HPKR_PARSER_START*/");
  const b = src.indexOf("/*HPKR_PARSER_END*/");
  if (a < 0 || b < 0) throw new Error("parser sentinels not found in index.html");
  return src.slice(a, b);
}

async function loadIndex(arg) {
  if (arg) return new Uint8Array(fs.readFileSync(arg));
  const resp = await fetch(LIVE_URL, { cache: "no-cache" });
  if (!resp.ok) throw new Error("fetch " + LIVE_URL + " -> HTTP " + resp.status);
  return new Uint8Array(await resp.arrayBuffer());
}

function pythonReference(indexPath) {
  if (process.env.REF_JSON) return JSON.parse(fs.readFileSync(process.env.REF_JSON, "utf8"));
  const code =
    "import sys, json, hpkg_meta; " +
    "print(json.dumps(hpkg_meta.read_repo_file(sys.argv[1])))";
  const out = execFileSync("python3", ["-c", code, indexPath], {
    cwd: SCRIPTS, maxBuffer: 256 * 1024 * 1024,
  });
  return JSON.parse(out.toString());
}

function canon(p) {
  // Fields read_repo emits that we assert on. revision: int|null; provides and
  // requires compared in order.
  return JSON.stringify({
    name: p.name, version: p.version,
    revision: p.revision == null ? null : p.revision,
    arch: p.arch, provides: p.provides, requires: p.requires,
  });
}

async function main() {
  const arg = process.argv[2];
  let indexPath = arg;
  let bytes;
  if (arg) { bytes = new Uint8Array(fs.readFileSync(arg)); }
  else {
    bytes = await loadIndex();
    indexPath = path.join(os.tmpdir(), "debeos-repo-" + process.pid);
    fs.writeFileSync(indexPath, bytes);
  }

  // Evaluate the exact shipped parser block, then grab globalThis.HPKR.
  (0, eval)(extractParser());
  const HPKR = globalThis.HPKR;

  const jsPkgs = await HPKR.parseRepo(bytes);
  const refPkgs = pythonReference(indexPath);

  const jsByName = new Map(jsPkgs.map((p) => [p.name, p]));
  const refByName = new Map(refPkgs.map((p) => [p.name, p]));

  let compared = 0, mismatches = 0;
  const examples = [];

  // 1) same set of names
  const onlyRef = [...refByName.keys()].filter((n) => !jsByName.has(n));
  const onlyJs = [...jsByName.keys()].filter((n) => !refByName.has(n));

  // 2) field-by-field over the intersection
  for (const [name, ref] of refByName) {
    const js = jsByName.get(name);
    if (!js) continue;
    compared++;
    if (canon(ref) !== canon(js)) {
      mismatches++;
      if (examples.length < 10)
        examples.push({ name, ref: JSON.parse(canon(ref)), js: JSON.parse(canon(js)) });
    }
  }

  console.log("JS package count:     " + jsPkgs.length);
  console.log("Python package count: " + refPkgs.length);
  console.log("Packages compared:    " + compared);
  console.log("Mismatches:           " + mismatches);
  console.log("Only in Python:       " + onlyRef.length + (onlyRef.length ? " " + onlyRef.slice(0, 5).join(",") : ""));
  console.log("Only in JS:           " + onlyJs.length + (onlyJs.length ? " " + onlyJs.slice(0, 5).join(",") : ""));

  // Ranking acceptance: ?q=go -> golang first, ?q=git -> git first.
  const rankFirst = (q) => {
    q = q.toLowerCase();
    const scored = [];
    for (const p of jsPkgs) {
      const prov = p.provides.map((x) => x.toLowerCase());
      let rank = null, exact = "cmd:" + q, exApp = "app:" + q;
      if (prov.includes(exact) || prov.includes(exApp)) rank = 0;
      else if (p.name.toLowerCase() === q) rank = 1;
      else if (p.name.toLowerCase().startsWith(q)) rank = 2;
      else if (p.name.toLowerCase().includes(q)) rank = 3;
      else if (p.summary && p.summary.toLowerCase().includes(q)) rank = 4;
      else if (prov.some((x) => x.includes(q))) rank = 5;
      if (rank != null) scored.push({ name: p.name, rank });
    }
    scored.sort((a, b) => a.rank - b.rank || (a.name < b.name ? -1 : 1));
    return scored.length ? scored[0].name : null;
  };
  const goFirst = rankFirst("go");
  const gitFirst = rankFirst("git");
  const nonsense = rankFirst("zzqqxnonsense");
  console.log("?q=go  ranks first:   " + goFirst);
  console.log("?q=git ranks first:   " + gitFirst);
  console.log("?q=<nonsense> result: " + (nonsense === null ? "no results (ok)" : nonsense));

  if (examples.length) {
    console.log("\nFirst mismatches:");
    for (const e of examples) console.log("  " + e.name + "\n    ref: " + JSON.stringify(e.ref) + "\n    js:  " + JSON.stringify(e.js));
  }

  const ok =
    mismatches === 0 && onlyRef.length === 0 && onlyJs.length === 0 &&
    jsPkgs.length === refPkgs.length &&
    goFirst === "golang" && gitFirst === "git" && nonsense === null;

  console.log("\n" + (ok ? "PASS" : "FAIL"));
  process.exit(ok ? 0 : 1);
}

main().catch((e) => { console.error(e); process.exit(2); });
