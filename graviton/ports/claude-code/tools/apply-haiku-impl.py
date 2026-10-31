#!/usr/bin/env python3
"""apply-haiku-impl.py <tree> [--acorn DIR [--node NODE]]: Haiku implementations (P8-P17).

Runs after apply-haiku-names.py (P5-P7). Each site where the Linux build reaches
for something Haiku lacks (/proc, xdg-open, wl-copy, notify-send, systemd-era open
flags, ps columns) gets a branch taken only when process.platform is "haiku"
(or M() returns "haiku"). Every other platform runs the original expression: the
old text is still in the module, unchanged, on the non-haiku side of the test.
The sites come from AUDIT.md; the IDs match PATCHES.md.

Each rule is a list of exact (old, new, count) substitutions in one module,
asserted; a count other than the expected one stops the build. Some rules also
append helper functions to the end of their module (function declarations hoist,
so placement is free). With --acorn every rewritten module is parsed afterwards.
"""
import hashlib, json, os, subprocess, sys, tempfile

HK = 'process.platform==="haiku"'

# Shared helper for P17: runs <install-prefix>/bin/cc-teaminfo (native/cc-teaminfo.c).
# The modules live at the install prefix, so import.meta.dirname is the prefix
# (R3 already maps Bun's import.meta.dir to it). A missing or unrunnable helper is
# an install defect: warn once on stderr, then let the caller's catch decide.
TEAMINFO = (
	'function __ccTIBin(){return import.meta.dirname+"/bin/cc-teaminfo"}'
	'var __ccTIWarned=!1;'
	'function __ccTIParse(o){return o.split("\\n").filter(Boolean).map((l)=>{let f=l.split(" ");'
	'return{pid:Number(f[0]),ppid:Number(f[1]),pgid:Number(f[2]),sid:Number(f[3]),uid:Number(f[4]),'
	'start:f[5],startEpochUs:Number(f[6]),args:f.slice(7).join(" ")}})}'
	'function __ccTIFail(e){if(!__ccTIWarned&&e!=null&&(e.code==="ENOENT"||e.code==="EACCES"))'
	'__ccTIWarned=!0,process.emitWarning("cannot run "+__ccTIBin()+" ("+e.code+"): process identity and '
	'process-tree checks fail until it is installed","ClaudeCodeHaiku");return e}'
	'function __ccTISync(a){try{return __ccTIParse(process.getBuiltinModule("node:child_process")'
	'.execFileSync(__ccTIBin(),a,{encoding:"utf8",timeout:2000,stdio:["ignore","pipe","pipe"]}))}'
	'catch(e){throw __ccTIFail(e)}}'
	'function __ccTIAsync(a){return new Promise((s,j)=>{process.getBuiltinModule("node:child_process")'
	'.execFile(__ccTIBin(),a,{encoding:"utf8",timeout:2000},(e,o)=>e?j(__ccTIFail(e)):s(__ccTIParse(o)))})}'
)

RULES = [
	# P8 (AUDIT S20): openTreeAnchor's switch has no haiku case and no default, so
	# it returned undefined and callers threw on `"unavailable"in`. Haiku joins
	# "unknown" in the designed unanchored answer.
	("P8", "chunk-hv6m45s7.js", [
		('case"unknown":return{unavailable:"unsupported_platform"}}}',
		 'case"haiku":case"unknown":return{unavailable:"unsupported_platform"}}}', 1),
	], None),
	# P9 (S15): URL/file opener. Haiku's /bin/open opens URLs via
	# BUrl::OpenWithPreferredApplication and folders in Tracker (src/bin/open.cpp).
	("P9", "chunk-aa47jmf3.js", [
		('function s(r,e){if(r==="macos")',
		 'function s(r,e){if(r==="haiku")return{command:"open",args:[e.replace(/\\/[^/]*$/,"")||"/"]};if(r==="macos")', 1),
		('let{command:o,args:n}=s("linux",r)',
		 'let{command:o,args:n}=s(e==="haiku"?"haiku":"linux",r)', 1),
		('let o="xdg-open",{code:n}=await it(o,[r],f)',
		 'let o=' + HK + '?"open":"xdg-open",{code:n}=await it(o,[r],f)', 1),
		('return y(await it(o||"xdg-open",[r],f))',
		 'return y(await it(o||(' + HK + '?"open":"xdg-open"),[r],f))', 1),
	], None),
	# P10 (S18): text clipboard through Haiku's `clipboard` (src/bin/clipboard.cpp:
	# -i reads stdin into text/plain, -p prints text/plain plus a newline). It needs
	# the registrar, so a failure is logged at error level, never dropped.
	("P10", "chunk-9deh4a76.js", [
		('case"linux":if(typeof f().tool==="string")return"native";break}',
		 'case"linux":if(typeof f().tool==="string")return"native";break;case"haiku":return"native"}', 1),
		('case"windows":return it("powershell",["-NoProfile","-NonInteractive","-Command",k],r),!0}return!1}',
		 'case"windows":return it("powershell",["-NoProfile","-NonInteractive","-Command",k],r),!0;'
		 'case"haiku":return it("clipboard",["-i"],r).then((n)=>{if(n.code!==0)t("clipboard: Haiku '
		 '\'clipboard -i\' failed (exit "+n.code+"): "+String(n.stderr||n.error||"").trim()+'
		 '" -- it needs a running registrar/app_server",{level:"error"})}),!0}return!1}', 1),
		('default:return""}}async function ee(',
		 'case"haiku":{let n=await it("clipboard",["-p"],r);if(n.code===0)return n.stdout.replace(/\\n$/,"");'
		 't("clipboard: Haiku \'clipboard -p\' failed (exit "+n.code+"): "+String(n.stderr||n.error||"").trim()+'
		 '" -- it needs a running registrar/app_server",{level:"error"});return""}'
		 'default:return""}}async function ee(', 1),
	], None),
	# P11 (S23): desktop notification for daemon re-auth via Haiku's `notify`
	# (src/bin/notify.cpp: --title <title> [MESSAGE]); a failure is logged.
	("P11", "chunk-wg6ymv3p.js", [
		('c("notify-send",["Claude","Your Claude assistant needs re-authentication"])',
		 '(' + HK + '?c("notify",["--title","Claude","Your Claude assistant needs re-authentication"])'
		 '.then((r)=>{if(r.code!==0)n("auth: Haiku notify failed (exit "+r.code+"): "+String(r.stderr||r.error||"").trim())})'
		 ':c("notify-send",["Claude","Your Claude assistant needs re-authentication"]))', 1),
	], None),
	# P12 (S24): path-length precheck; Haiku PATH_MAX is 1024 (headers/posix/limits.h).
	("P12", "chunk-ddrd3r4k.js", [
		('var ir={macos:1024,linux:4096,wsl:4096}', 'var ir={macos:1024,linux:4096,wsl:4096,haiku:1024}', 1),
	], None),
	# P13 (S10): fd->path checks with no fallback. wIn falls through to its existing
	# realpath tail (after the dev/ino lstat match); Me checks realpath + stat dev/ino
	# against the open handle; a token fd is read directly (t9e), as for sockets.
	("P13", "chunk-k1pd3pap.js", [
		('if(s.dev!==n.dev||s.ino!==n.ino)return;try{let a=await o.readlink(`/proc/self/fd/${e.fd}`)',
		 'if(s.dev!==n.dev||s.ino!==n.ino)return;if(!(' + HK + '))try{let a=await o.readlink(`/proc/self/fd/${e.fd}`)', 1),
	], None),
	("P13", "chunk-v5xftfk7.js", [
		('let a=await Ct(`/proc/self/fd/${r.fd}`);if(!n(a))return null;',
		 'let a=' + HK + '?await(async()=>{let p=await le(e),q=await cn(p,{bigint:!0});'
		 'if(q.dev!==i.dev||q.ino!==i.ino)throw Error("the file was replaced after it was opened");return p})()'
		 ':await Ct(`/proc/self/fd/${r.fd}`);if(!n(a))return null;', 1),
	], None),
	("P13", "chunk-s7yyd1p6.js", [
		('if(_ao(p)){', 'if(_ao(p)||' + HK + '){', 1),
		('if(_ao(l)){', 'if(_ao(l)||' + HK + '){', 1),
	], None),
	# P14 (S11): served-file folder. Haiku hands out the verified real path and
	# re-checks it as fstat(handle) == lstat(path) by dev/ino.
	("P14", "chunk-12xea136.js", [
		('return{path:`/proc/self/fd/${s.fd}`,[Symbol.asyncDispose]',
		 'return{path:' + HK + '?n:`/proc/self/fd/${s.fd}`,[Symbol.asyncDispose]', 1),
		('async function qo(e,r){let n=await Nr(`/proc/self/fd/${e.fd}`);',
		 'async function qo(e,r){if(' + HK + '){let[a,b]=await Promise.all([e.stat({bigint:!0}),Ae(r,{bigint:!0})]);'
		 'if(!b.isDirectory()||a.dev!==b.dev||a.ino!==b.ino)throw Error("the path changed while it was being opened");return}'
		 'let n=await Nr(`/proc/self/fd/${e.fd}`);', 1),
	], None),
	# P15 (S12): directory chain. Haiku records the path each directory handle was
	# opened at (O_NOFOLLOW|O_DIRECTORY, as before) and uses it where Linux uses
	# /proc/self/fd/N. Weaker than an fd-held chain: see LIMITATIONS.md.
	("P15", "chunk-m6kk55rf.js", [
		('function s(t){return`/proc/self/fd/${t.fd}`}',
		 'function s(t){if(' + HK + '){let p=__ccDirPath.get(t);if(p===void 0)throw Error("no path recorded for this '
		 'directory handle");return p}return`/proc/self/fd/${t.fd}`}var __ccDirPath=new WeakMap;'
		 'async function __ccOpenDir(p){let t=await c(p,f);if(' + HK + ')__ccDirPath.set(t,p);return t}', 1),
		('await c(e,f)', 'await __ccOpenDir(e)', 1),
		('await c(a,f)', 'await __ccOpenDir(a)', 1),
		('await c(n,f)', 'await __ccOpenDir(n)', 1),
		('await c(r,f)', 'await __ccOpenDir(r)', 1),
	], None),
	# P16 (S38): Linux open(2) flag numbers baked in as literals. On Haiku 524288
	# (Linux O_CLOEXEC) is O_NOFOLLOW and 2097152 (Linux O_PATH) is O_DIRECTORY
	# (headers/posix/fcntl.h). Haiku O_CLOEXEC is 0x40; O_PATH has no Haiku
	# counterpart, and 0 is the value the darwin build uses (fejj39g0 tests y9===0).
	("P16", "chunk-szzxb7zd.js", [
		('var Bo=524288,GIe=0,y9=2097152,',
		 'var Bo=' + HK + '?64:524288,GIe=0,y9=' + HK + '?0:2097152,', 1),
	], None),
	("P16", "chunk-zy2k3hny.js", [
		('var sfr=524288;', 'var sfr=' + HK + '?64:524288;', 1),
	], None),
	# P17 (S06/S21/S32): team identity and tree from cc-teaminfo (native/).
	("P17", "chunk-6h8148nz.js", [
		('function QOe(e){try{let t=le().readFileSync(`/proc/${e}/stat`',
		 'function QOe(e){try{if(' + HK + ')return __ccTISync([String(e)])[0]?.start;let t=le().readFileSync(`/proc/${e}/stat`', 1),
		('function fas(e,t=12){let n=[],r=e;',
		 'function fas(e,t=12){let n=[],r=e;if(' + HK + '){let m;try{m=new Map(__ccTISync(["-a"]).map((x)=>[x.pid,x.ppid]))}'
		 'catch{return n}for(let o=0;o<t;o++){let i=m.get(r);if(!Number.isInteger(i)||i<1)break;if(n.push(i),i===1)break;r=i}return n}', 1),
		('async function tgr(e,t=10){',
		 'async function tgr(e,t=10){if(' + HK + '){let m;try{m=new Map((await __ccTIAsync(["-a"])).map((x)=>[x.pid,x.ppid]))}'
		 'catch{return{ancestors:[],readFailed:!0,truncated:!1}}let a=[],p=e;for(let i=0;i<t;i++){let q=m.get(p);'
		 'if(q===void 0)return{ancestors:a,readFailed:!0,truncated:!1};if(q===0||q===1)return{ancestors:a,readFailed:!1,truncated:!1};'
		 'a.push(q),p=q}return{ancestors:a,readFailed:!1,truncated:!0}}', 1),
		('function mas(e){try{',
		 'function mas(e){try{if(' + HK + '){let x=__ccTISync([String(e)])[0];return x&&x.args?x.args.trim():null}', 1),
		('function G3e(e){try{{',
		 'function G3e(e){try{if(' + HK + ')return __ccTISync([String(e)])[0]?.start;{', 1),
		('async function b(e,t){let n=t===void 0?{}:{env:t,extendEnv:!1};try{{',
		 'async function b(e,t){let n=t===void 0?{}:{env:t,extendEnv:!1};try{if(' + HK + ')return(await __ccTIAsync([String(e)]))[0]?.start;{', 1),
		('async function eIe(e){try{{',
		 'async function eIe(e){try{if(' + HK + '){let x=(await __ccTIAsync([String(e)]))[0];'
		 'return x===void 0||!Number.isFinite(x.startEpochUs)?null:Math.floor(x.startEpochUs/1000)}{', 1),
	], TEAMINFO),
	("P17", "chunk-fgccedbt.js", [
		('function HL(e){return new Promise((n,r)=>{',
		 'function HL(e){if(' + HK + ')return __ccTIAsync(["-a"]).then((L)=>L.map((x)=>e?x.pid+" "+x.ppid+" "+x.sid:x.pid+" "+x.ppid).join("\\n")+"\\n");'
		 'return new Promise((n,r)=>{', 1),
	], TEAMINFO),
	("P17", "chunk-2hxx68dq.js", [
		('async function Wn(e){let n=await dt("ps",',
		 'async function Wn(e){if(' + HK + '){let L=await __ccTIAsync(["-a"]);return L.length===0?void 0:'
		 '{rows:L.map((x)=>({pid:x.pid,ppid:x.ppid,pgrp:x.pgid,token:x.start})),degraded:!1}}let n=await dt("ps",', 1),
	], TEAMINFO),
	# The lock record carries the start token on Haiku too, so readers (uon/qv)
	# can prove identity instead of accepting any live pid.
	("P17", "chunk-s0dw5891.js", [
		('procStart:M()==="linux"||M()==="wsl"?await ud(process.pid):void 0',
		 'procStart:M()==="linux"||M()==="wsl"||M()==="haiku"?await ud(process.pid):void 0', 1),
	], None),
]

PARSEJS = r"""
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
const [acdir, ...files] = process.argv.slice(2);
const acorn = await import(pathToFileURL(join(acdir, "dist/acorn.mjs")).href);
let bad = 0;
for (const f of files) {
	try { acorn.parse(readFileSync(f, "utf8"), { ecmaVersion: "latest", sourceType: "module", allowHashBang: true }); }
	catch (e) { bad++; console.log("PARSE-FAIL " + f + ": " + e.message); }
}
console.log("parsed " + files.length + " modules, " + bad + " failed");
process.exit(bad ? 1 : 0);
"""


def die(msg):
	sys.exit("apply-haiku-impl: " + msg)


def main():
	args = sys.argv[1:]
	opts = {"--acorn": None, "--node": "node"}
	for k in list(opts):
		if k in args:
			i = args.index(k)
			opts[k] = args[i + 1]
			del args[i:i + 2]
	if len(args) != 1:
		die("usage: apply-haiku-impl.py <tree> [--acorn DIR [--node NODE]]")
	tree = args[0]
	relp = os.path.join(tree, "RELOCATION.json")
	rel = json.load(open(relp))
	if not rel["rule_totals"].get("P7"):
		die("run apply-haiku-names.py (P5-P7) first")
	ids = sorted({r[0] for r in RULES}, key=lambda r: int(r[1:]))
	if any(rel["rule_totals"].get(r) for r in ids):
		die("already applied to this tree")
	totals, sites, changed = {}, {}, []
	for rule, mod, subs, helper in RULES:
		p = os.path.join(tree, mod)
		s = open(p, encoding="utf-8").read()
		if helper is not None and "function __ccTIBin(" in s:
			die("%s: %s already holds the helper" % (rule, mod))
		n_mod = 0
		for old, new, want in subs:
			n = s.count(old)
			if n != want:
				die("%s: expected %d site(s) of %r in %s, found %d -- re-review" % (rule, want, old[:60], mod, n))
			s = s.replace(old, new)
			n_mod += want
		if helper is not None:
			s = s.rstrip("\n") + "\n" + helper + "\n"
		open(p, "w", encoding="utf-8").write(s)
		rel["files"][mod] = hashlib.sha256(s.encode()).hexdigest()
		totals[rule] = totals.get(rule, 0) + n_mod
		sites.setdefault(rule, []).append([mod, n_mod])
		changed.append(p)
		print("%s applied in %s (%d sites%s)" % (rule, mod, n_mod, ", + teaminfo helper" if helper else ""))
	if opts["--acorn"]:
		with tempfile.NamedTemporaryFile("w", suffix=".mjs", delete=False) as fh:
			fh.write(PARSEJS)
			js = fh.name
		try:
			r = subprocess.run([opts["--node"], js, os.path.abspath(opts["--acorn"])] + sorted(set(changed)))
		finally:
			os.unlink(js)
		if r.returncode:
			die("a rewritten module no longer parses")
	for rule in ids:
		rel["rule_totals"][rule] = totals[rule]
		rel.setdefault("rule_sites", {})[rule] = sites[rule]
	with open(relp, "w") as fh:
		json.dump(rel, fh, indent=1)


main()
