#!/usr/bin/env python3
"""Find every process.platform / process.arch value Bun's bundler constant-folded.

  find-folds.py <la-dir> <da-dir> <lx-dir> <acorn-dir> <out.json> [--report r.json] [--node NODE]

<la-dir>, <da-dir>, <lx-dir> are extract-bun-bundle.py outputs of the SAME Claude
Code version built for linux-arm64 (the one we ship), darwin-arm64 and linux-x64.
The other two builds are data: they are parsed, never executed. <acorn-dir> is the
fetch-deps.sh acorn package; node runs it to tokenise and parse (minified code
defeats any hand-written lexer, and acorn is the parser analyze-requires.mjs uses).

Bun's bundler substitutes the build target's platform and arch at build time, then
deletes the branches it proved dead. One build alone cannot tell a folded "linux"
from a genuine one ("linux" in a list of platform names, a comparison of a runtime
value). Comparing builds of the same version can: a literal that is "linux" in
linux-arm64 but "darwin" at the same place in darwin-arm64 was process.platform; one
that is "arm64" in linux-arm64 but "x64" in linux-x64 was process.arch. A literal
equal in all builds is genuine. Nothing is decided by name, position or heuristic.

Method:
 1. Tokenise every JS module of all three builds with acorn.
 2. Match modules across builds (chunk file names are content hashes, so they
    differ). A module's canonical form is its token stream with platform/arch
    words inside literals replaced by a placeholder, chunk specifiers by another,
    and non-property identifiers alpha-renamed in order of first appearance
    (Bun mangles cross-chunk names globally, so they differ between builds).
    Pairing order: same non-hash name or identical bytes; unique canonical form;
    canonical form + specifiers resolved through the pairs so far (iterated);
    unmatched facade modules by their unmangled `export{x as Name}` names; then
    token-shingle similarity confirmed by alignment, mutual best only. The rest is
    reported unmatched; a module left unpaired leaves its words unverified.
 3. Align each matched pair token by token (patience diff on a renaming-blind key).
    A "linux" is decided by darwin-arm64 (fold iff "darwin" there), an "arm64" by
    linux-x64 (fold iff "x64"), word by word inside literals, template text and
    platform-word names (`.arm64` vs `.x64`); the token must sit in SUPPORT agreeing
    aligned neighbours on each side. Every other difference (code present in one
    build only, a different literal, a boolean folded differently) is a
    dead-code-elimination finding we cannot restore and is reported with context.
 4. For each site, acorn locates the literal in the linux-arm64 module and its
    syntactic position, and the site gets a context snippet that occurs exactly
    once in that module plus the replacement text apply-folds.py splices in.

<out.json> = {"summary", "sites" (what apply-folds.py rewrites), "undecidable" (a fold
the method saw but cannot restore safely), "unverified" (platform/arch words it could
not compare in the deciding build), "platform_helpers" (functions/tables mapping a
platform to names: they need deliberate haiku cases)}. --report adds the audit input:
module pairing leftovers and every non-literal difference (dead-code eliminations).
Deterministic: same inputs, same bytes.
"""
import bisect, collections, difflib, hashlib, json, os, re, subprocess, sys, tempfile
from array import array

BUILDS = ("la", "da", "lx")		# linux-arm64 (shipped), darwin-arm64, linux-x64
# Words a folded platform/arch value can appear as. Only these may differ inside an
# aligned literal for it to count as a fold; anything else is reported.
WORDS = re.compile(r"(?<![A-Za-z0-9_$])(linux|darwin|arm64|x64)(?![A-Za-z0-9_$])")
SPEC = re.compile(r"/\$bunfs/root/chunk-[a-z0-9]{8}\.js")
# Text the relocation pass rewrites; a snippet must not contain any of it, or it
# would not be found again in the relocated tree.
RELOC_TEXT = ("/$bunfs/root/", "import.meta.require", "import.meta.dir")
# relocate-bundle.py P1/P2 rewrite these discriminants; their sites are tagged so
# apply-folds.py can skip them when the relocation already applied the rule.
P_RULES = {'switch("linux")': "P1", 'switch("arm64")': "P2"}
CTX = 24			# minimum snippet context each side
SUPPORT = 4			# aligned neighbours required on each side of a decided token
MAX_CTX = 400

# acorn token stream per module: kind codes, starts, ends (code-point offsets).
TOKJS = r"""
import { readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
const [acdir, ex, out] = process.argv.slice(2);
const acorn = await import(pathToFileURL(join(acdir, "dist/acorn.mjs")).href);
const man = JSON.parse(readFileSync(join(ex, "manifest.json"), "utf8"));
const chunks = [];
for (const m of man.modules) {
	if (m.loader !== 1) continue;
	const src = readFileSync(join(ex, m.name.slice("/$bunfs/".length)), "utf8");
	const toks = [];
	acorn.parse(src, { ecmaVersion: "latest", sourceType: "module", allowHashBang: true, onToken: toks });
	// UTF-16 offsets -> code-point offsets (what Python indexes by).
	let cp = null;
	if (/[\uD800-\uDFFF]/.test(src)) {
		// cp[i] = code points before UTF-16 index i; a pair's low half adds none
		cp = new Int32Array(src.length + 1);
		for (let i = 0, c = 0; i <= src.length; i++) {
			cp[i] = c;
			const u = src.charCodeAt(i);
			if (!(u >= 0xDC00 && u <= 0xDFFF && i > 0 && src.charCodeAt(i - 1) >= 0xD800 && src.charCodeAt(i - 1) <= 0xDBFF)) c++;
		}
	}
	const n = toks.length - 1;		// drop eof
	const a = new Int32Array(1 + 3 * n);
	a[0] = n;
	for (let i = 0; i < n; i++) {
		const t = toks[i], L = t.type.label;
		const k = t.type.keyword ? 1 : L === "name" ? 0 : L === "string" ? 2 : L === "template" ? 3
			: L === "num" ? 4 : L === "regexp" ? 5 : L === "privateId" ? 7 : 6;
		a[1 + 3 * i] = k;
		a[2 + 3 * i] = cp ? cp[t.start] : t.start;
		a[3 + 3 * i] = cp ? cp[t.end] : t.end;
	}
	const name = Buffer.from(m.name, "utf8");
	const hdr = new Int32Array([name.length]);
	chunks.push(Buffer.from(hdr.buffer), name, Buffer.alloc((4 - name.length % 4) % 4), Buffer.from(a.buffer));
}
writeFileSync(out, Buffer.concat(chunks));
"""

# acorn: for each requested offset, the innermost Literal / TemplateElement /
# Identifier around it and its last parent links [type, key, computed, special].
LOCJS = r"""
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
const [acdir, reqf] = process.argv.slice(2);
const acorn = await import(pathToFileURL(join(acdir, "dist/acorn.mjs")).href);
const req = JSON.parse(readFileSync(reqf, "utf8"));
const res = {};
for (const [file, offs] of Object.entries(req)) {
	const src = readFileSync(file, "utf8");
	const ast = acorn.parse(src, { ecmaVersion: "latest", sourceType: "module", allowHashBang: true });
	// code-point -> UTF-16 offsets
	const u16 = [];
	for (let i = 0, c = 0; i <= src.length; ) { u16[c++] = i; if (i === src.length) break;
		const u = src.charCodeAt(i); i += (u >= 0xD800 && u <= 0xDBFF) ? 2 : 1; }
	const back = new Map(u16.map((v, c) => [v, c]));
	const out = [];
	for (const off of offs) {
		const o = u16[off];
		let best = null;
		const walk = (n, chain) => {
			if (!n || typeof n.type !== "string" || o < n.start || o >= n.end) return;
			if (n.type === "Literal" || n.type === "TemplateElement" || n.type === "Identifier") best = { n, chain };
			for (const k of Object.keys(n)) {
				if (k === "type" || k === "start" || k === "end") continue;
				const v = n[k];
				const step = (c) => walk(c, chain.concat([[n.type, k, n.computed ?? null,
					n.type === "Property" ? (n.shorthand || n.method || n.kind !== "init") : null]]));
				if (Array.isArray(v)) v.forEach(step); else if (v && typeof v.type === "string") step(v);
			}
		};
		walk(ast, []);
		if (!best) { out.push(null); continue; }
		const { n, chain } = best;
		out.push({ type: n.type, start: back.get(n.start), end: back.get(n.end),
			value: n.type === "Literal" ? n.value : n.type === "Identifier" ? n.name : n.value.raw,
			chain: chain.slice(-4) });
	}
	res[file] = out;
}
process.stdout.write(JSON.stringify(res));
"""


def die(msg):
	sys.exit("find-folds: " + msg)


def run_node(node, script, args, capture=False):
	with tempfile.NamedTemporaryFile("w", suffix=".mjs", delete=False) as fh:
		fh.write(script)
		path = fh.name
	try:
		r = subprocess.run([node, path] + args, check=True,
			stdout=subprocess.PIPE if capture else None)
		return r.stdout
	finally:
		os.unlink(path)


class Module:
	__slots__ = ("name", "src", "kind", "start", "end", "keys", "canon", "lits", "_ms")

	def modsyntax(self):
		"""Mask of tokens inside top-level import/export lists (`import{..}from".."`)."""
		if getattr(self, "_ms", None) is None:
			k, src, st, en = self.kind, self.src, self.start, self.end
			ms = bytearray(len(k))
			i = 0
			while i < len(k):
				t = src[st[i]:en[i]]
				nxt = src[st[i + 1]:en[i + 1]] if i + 1 < len(k) else ""
				if k[i] == 1 and t in ("import", "export") and (nxt in ("{", "*") or (t == "import" and k[i + 1] == 2)):
					j = i
					while j < len(k) and src[st[j]:en[j]] != ";" and not (j > i and k[j] == 1 and src[st[j]:en[j]] in ("import", "export") ):
						ms[j] = 1; j += 1
					if j < len(k) and src[st[j]:en[j]] == ";":
						ms[j] = 1; j += 1
					i = j
				else:
					i += 1
			self._ms = ms
		return self._ms


def load_build(node, acorn, ex, tag, work):
	tf = os.path.join(work, tag + ".tok")
	run_node(node, TOKJS, [acorn, ex, tf])
	raw = open(tf, "rb").read()
	os.unlink(tf)
	mods, p = {}, 0
	while p < len(raw):
		nl = int.from_bytes(raw[p:p + 4], "little")
		p += 4
		name = raw[p:p + nl].decode("utf-8")
		p += nl + (4 - nl % 4) % 4
		n = int.from_bytes(raw[p:p + 4], "little")
		a = array("i")
		a.frombytes(raw[p + 4:p + 4 + 12 * n])
		p += 4 + 12 * n
		m = Module()
		m.name = name
		m.src = open(os.path.join(ex, name[len("/$bunfs/"):]), encoding="utf-8").read()
		m.kind, m.start, m.end = a[0::3], a[1::3], a[2::3]
		mods[name] = m
	return mods


def norm_lit(s):
	return WORDS.sub("\x01", SPEC.sub("\x02", s))


def keys_of(m, intern):
	"""Renaming-blind alignment key per token, and the alpha-renamed canonical hash."""
	src, kind, st, en = m.src, m.kind, m.start, m.end
	keys = array("i")
	h = hashlib.sha256()
	alpha = {}
	lits = set()
	prev = ""
	for i in range(len(kind)):
		k = kind[i]
		t = src[st[i]:en[i]]
		if k == 0 and prev not in (".", "?."):
			key, can = "I", "I%d" % alpha.setdefault(t, len(alpha))
		elif k == 0:
			key = can = "P" + t
		elif k in (2, 3):
			key = can = "S" + norm_lit(t)
			if len(t) >= 10:
				lits.add(key)
		else:
			key = can = t
		keys.append(intern.setdefault(key, len(intern)))
		h.update(can.encode("utf-8", "surrogatepass"))
		h.update(b"\0")
		prev = t if k == 6 else ""
	m.keys, m.canon, m.lits = keys, h.hexdigest(), lits


def lis_pairs(pairs):
	"""Longest increasing subsequence on the b index of (a, b) pairs sorted by a."""
	tails, tidx, back = [], [], [None] * len(pairs)
	for i, (_, b) in enumerate(pairs):
		j = bisect.bisect_left(tails, b)
		if j == len(tails):
			tails.append(b); tidx.append(i)
		else:
			tails[j] = b; tidx[j] = i
		back[i] = tidx[j - 1] if j else None
	out, i = [], tidx[-1] if tidx else None
	while i is not None:
		out.append(pairs[i]); i = back[i]
	return out[::-1]


def align(A, B):
	"""Patience diff of key arrays; returns matched (i, j) pairs, increasing."""
	out = []
	stack = [(0, len(A), 0, len(B))]
	segs = []
	while stack:
		alo, ahi, blo, bhi = stack.pop()
		pre = []
		while alo < ahi and blo < bhi and A[alo] == B[blo]:
			pre.append((alo, blo)); alo += 1; blo += 1
		suf = []
		while alo < ahi and blo < bhi and A[ahi - 1] == B[bhi - 1]:
			ahi -= 1; bhi -= 1; suf.append((ahi, bhi))
		segs.extend(pre)
		segs.extend(suf)
		if alo >= ahi or blo >= bhi:
			continue
		ca = collections.Counter(A[alo:ahi])
		cb = collections.Counter(B[blo:bhi])
		posb = {}
		for j in range(blo, bhi):
			if cb[B[j]] == 1 and ca[B[j]] == 1:
				posb[B[j]] = j
		anchors = [(i, posb[A[i]]) for i in range(alo, ahi) if A[i] in posb]
		anchors = lis_pairs(anchors)
		if not anchors:
			if (ahi - alo) * (bhi - blo) <= 4_000_000:
				sm = difflib.SequenceMatcher(None, A[alo:ahi], B[blo:bhi], autojunk=False)
				for a0, b0, n in sm.get_matching_blocks():
					segs.extend((alo + a0 + d, blo + b0 + d) for d in range(n))
			continue
		segs.extend(anchors)
		pa, pb = alo, blo
		for i, j in anchors:
			stack.append((pa, i, pb, j))
			pa, pb = i + 1, j + 1
		stack.append((pa, ahi, pb, bhi))
	segs.sort()
	return segs


CHUNK = re.compile(r"/\$bunfs/root/chunk-[a-z0-9]{8}\.js$")


def spec_hash(m, mapping):
	"""Canonical hash with chunk specifiers resolved through a module mapping."""
	h = hashlib.sha256(m.canon.encode())
	for i in range(len(m.kind)):
		if m.kind[i] == 2:
			for sp in SPEC.findall(m.src[m.start[i]:m.end[i]]):
				h.update(mapping.get(sp, "#").encode() + b"\0")
	return h.hexdigest()


def shingles(m, k=8):
	ks = m.keys
	return {hash(tuple(ks[i:i + k])) for i in range(0, max(1, len(ks) - k + 1))}


def has_words(m):
	return any(m.kind[i] in (2, 3) and WORDS.search(m.src[m.start[i]:m.end[i]])
		for i in range(len(m.kind)))


EXPORT_AS = re.compile(r"\bexport\{([^{}]*)\}")


def export_names(m):
	"""Public names of `export{a as Name,...}` (only aliased ones are unmangled)."""
	names = []
	for mt in EXPORT_AS.finditer(m.src):
		for part in mt.group(1).split(","):
			if " as " in part:
				names.append(part.split(" as ")[1].strip())
	return tuple(sorted(names))


def pair_modules(la, ob):
	"""Match la modules to another build. Order: same non-hash name (entry points,
	addon loaders), unique canonical form, canonical form + resolved specifiers for
	repeated forms (iterated), then similarity -- mutual best only."""
	pairs, method = {}, {}
	for n in sorted(la):
		if not CHUNK.match(n) and n in ob:
			pairs[n] = n; method[n] = "name"
		elif n in ob and la[n].src == ob[n].src:
			pairs[n] = n; method[n] = "identical"	# same content-hash name, same bytes
	while True:
		used = set(pairs.values())
		inv = {v: k for k, v in pairs.items()}
		# la specifiers resolve to themselves once paired; ob ones through the pairing
		mla = {"/$bunfs/root/" + short(k): k for k in pairs}
		mob = {v: k for v, k in inv.items()}
		by_a, by_b = collections.defaultdict(list), collections.defaultdict(list)
		for n in la:
			if n not in pairs:
				by_a[(la[n].canon, spec_hash(la[n], mla))].append(n)
		for n in ob:
			if n not in used:
				by_b[(ob[n].canon, spec_hash(ob[n], mob))].append(n)
		# first pass on the plain canonical form, then on canonical + specifiers
		new = 0
		plain_a, plain_b = collections.defaultdict(list), collections.defaultdict(list)
		for (c, _), v in by_a.items():
			plain_a[c].extend(v)
		for (c, _), v in by_b.items():
			plain_b[c].extend(v)
		for c, an in plain_a.items():
			bn = plain_b.get(c, [])
			if len(an) == 1 and len(bn) == 1:
				pairs[an[0]] = bn[0]; method[an[0]] = "exact"; new += 1
		for c, an in by_a.items():
			bn = by_b.get(c, [])
			an = [x for x in an if x not in pairs]
			bn = [y for y in bn if y not in set(pairs.values())]
			if len(an) == 1 and len(bn) == 1:
				pairs[an[0]] = bn[0]; method[an[0]] = "exact+specifiers"; new += 1
		if not new:
			break
	# Repeated identical forms that specifiers cannot separate: pair only when every
	# member is raw-identical (then any pairing aligns the same tokens).
	used = set(pairs.values())
	plain_a, plain_b = collections.defaultdict(list), collections.defaultdict(list)
	for n in la:
		if n not in pairs:
			plain_a[la[n].canon].append(n)
	for n in ob:
		if n not in used:
			plain_b[ob[n].canon].append(n)
	ambiguous = []
	norm = lambda t: SPEC.sub("#", t)
	for c, an in sorted(plain_a.items()):
		bn = plain_b.get(c, [])
		if not bn:
			continue
		if len(an) == len(bn) and len({norm(la[x].src) for x in an}) == 1 and len({norm(ob[y].src) for y in bn}) == 1:
			for x, y in zip(sorted(an), sorted(bn)):
				pairs[x] = y; method[x] = "exact-group"
		elif len(an) == len(bn) and not any(has_words(la[x]) for x in an):
			# Equal canonical forms align token for token, so the only possible
			# differences are literal words; with none in any member, nothing can
			# hide here and the pairing order is immaterial.
			for x, y in zip(sorted(an), sorted(bn)):
				pairs[x] = y; method[x] = "exact-group-wordless"
		else:
			ambiguous.append([sorted(an), sorted(bn)])
	amb_a = {x for g in ambiguous for x in g[0]}
	amb_b = {y for g in ambiguous for y in g[1]}
	used = set(pairs.values())
	ra = sorted(n for n in la if n not in pairs and n not in amb_a)
	rb = sorted(n for n in ob if n not in used and n not in amb_b)
	# Facade modules (import list + `export{x as Name}`) differ only in their import
	# lists; their public export names are not mangled and identify them.
	ex_a, ex_b = collections.defaultdict(list), collections.defaultdict(list)
	for x in ra:
		k = export_names(la[x])
		if k:
			ex_a[k].append(x)
	for y in rb:
		k = export_names(ob[y])
		if k:
			ex_b[k].append(y)
	for k, an in ex_a.items():
		bn = ex_b.get(k, [])
		if len(an) == 1 and len(bn) == 1:
			x, y = an[0], bn[0]
			sc = len(align(la[x].keys, ob[y].keys)) / max(len(la[x].keys), len(ob[y].keys), 1)
			pairs[x] = y; method[x] = "exports:%.4f" % sc
	used = set(pairs.values())
	ra = [x for x in ra if x not in pairs]
	rb = [y for y in rb if y not in used]
	# Similarity: all-pairs shingle Jaccard over the leftovers, confirmed by alignment.
	sa = {x: shingles(la[x]) for x in ra}
	sb = {y: shingles(ob[y]) for y in rb}
	jac = {}
	for x in ra:
		best = []
		for y in rb:
			inter = len(sa[x] & sb[y])
			if inter:
				best.append((inter / len(sa[x] | sb[y]), y))
		best.sort(key=lambda t: (-t[0], t[1]))
		jac[x] = best[:3]

	def score(x, y):
		A, B = la[x].keys, ob[y].keys
		return len(align(A, B)) / max(len(A), len(B), 1)

	best_a = {}
	for x in ra:
		sc = sorted(((score(x, y), y) for j, y in jac[x] if j >= 0.2), key=lambda t: (-t[0], t[1]))
		if sc and sc[0][0] >= 0.5 and (len(sc) == 1 or sc[0][0] > sc[1][0]):
			best_a[x] = sc[0]
	claimed = collections.defaultdict(list)
	for x, (sc, y) in best_a.items():
		claimed[y].append((sc, x))
	for y, lst in sorted(claimed.items()):
		lst.sort(key=lambda t: (-t[0], t[1]))
		if len(lst) == 1 or lst[0][0] > lst[1][0]:
			sc, x = lst[0]
			pairs[x] = y; method[x] = "similar:%.4f" % sc
	ua = sorted(n for n in la if n not in pairs)
	used = set(pairs.values())
	ub = sorted(n for n in ob if n not in used)
	return pairs, method, ua, ub, ambiguous


def short(name):
	return name[len("/$bunfs/root/"):]


def ctx(src, s, e, w=60):
	return src[max(0, s - w):s] + "⟦" + src[s:e] + "⟧" + src[e:e + w]


def diff_pair(m, o, tag, sites, dce):
	"""Walk aligned tokens of la module m and other-build module o."""
	A, B = m.keys, o.keys
	pairs = align(A, B)
	amap = dict(pairs)

	def wordname(a, b):
		return (m.kind[a] == 0 and o.kind[b] == 0 and WORDS.fullmatch(m.src[m.start[a]:m.end[a]])
			and WORDS.fullmatch(o.src[o.start[b]:o.end[b]]))

	def side(i, j, step):
		n = 0
		while n < SUPPORT:
			a, b = i + step * (n + 1), j + step * (n + 1)
			if a < 0 or b < 0 or a >= len(A) or b >= len(B):
				return SUPPORT if (a < 0 and b < 0) or (a >= len(A) and b >= len(B)) else n
			if amap.get(a) != b and not wordname(a, b):
				break
			n += 1
		return n

	def support(i, j):
		"""Contiguous agreeing neighbours on the weaker side (capped at SUPPORT); a
		neighbour that is itself a differing platform-word name still agrees."""
		return min(side(i, j, -1), side(i, j, 1))

	for i, j in pairs:
		if m.kind[i] in (2, 3):
			ta = m.src[m.start[i]:m.end[i]]
			tb = o.src[o.start[j]:o.end[j]]
			# Record every la literal holding a platform/arch word (to decide it), and
			# every other literal that differs; chunk specifiers are content hashes
			# and differ between builds by design.
			if WORDS.search(ta) or SPEC.sub("\x02", ta) != SPEC.sub("\x02", tb):
				d = sites[(m.name, m.start[i])]
				d.setdefault("tok", (m.start[i], m.end[i], ta))
				d[tag] = tb
				d[tag + "_support"] = support(i, j)
		elif m.kind[i] == 0:
			ta = m.src[m.start[i]:m.end[i]]
			tb = o.src[o.start[j]:o.end[j]]
			if WORDS.fullmatch(ta):
				# a platform word used as a name, aligned: same in both, or (for
				# renaming-blind identifiers) a difference worth reporting
				d = sites[(m.name, m.start[i])]
				d.setdefault("tok", (m.start[i], m.end[i], ta))
				d["name"] = True
				d[tag] = tb
				d[tag + "_support"] = support(i, j)
	# A property name is part of the alignment key, so `.arm64` vs `.x64` (a folded
	# computed member `[process.arch]`) surfaces as a one-token run: pick those up.
	prev = (-1, -1)
	for i, j in pairs + [(len(A), len(B))]:
		if i == prev[0] + 2 and j == prev[1] + 2:
			x, y = i - 1, j - 1
			ta = m.src[m.start[x]:m.end[x]]
			tb = o.src[o.start[y]:o.end[y]]
			if m.kind[x] == 0 and o.kind[y] == 0 and WORDS.fullmatch(ta) and WORDS.fullmatch(tb):
				d = sites[(m.name, m.start[x])]
				d.setdefault("tok", (m.start[x], m.end[x], ta))
				d["name"] = True
				d[tag] = tb
				d[tag + "_support"] = support(x, y)
		prev = (i, j)
	# unaligned runs: code present in one build only, or differing tokens
	runs = []
	prev = (-1, -1)
	for i, j in pairs + [(len(A), len(B))]:
		if i > prev[0] + 1 or j > prev[1] + 1:
			runs.append((prev[0] + 1, i, prev[1] + 1, j))
		prev = (i, j)
	for a0, a1, b0, b1 in runs:
		sa = m.src[m.start[a0]:m.end[a1 - 1]] if a1 > a0 else ""
		sb = o.src[o.start[b0]:o.end[b1 - 1]] if b1 > b0 else ""
		ppos = m.start[a0] if a1 > a0 else (m.end[a0 - 1] if a0 else 0)
		ma, mb = m.modsyntax(), o.modsyntax()
		ka = [m.kind[i] for i in range(a0, a1)]
		kb = [o.kind[j] for j in range(b0, b1)]
		if all(ma[a0:a1]) and all(mb[b0:b1]):
			cls = "import-export-list"
		elif (ka and kb) and (all(ma[a0:a1]) or all(mb[b0:b1])):
			cls = "restructured"		# code on one side, import lines on the other
		elif len(ka) == 1 and len(kb) == 1 and ka[0] in (2, 3) and kb[0] in (2, 3):
			cls = "literal"
		elif {sa, sb} <= {"0", "1", "!0", "!1", "true", "false"}:
			cls = "boolean"
		elif not kb:
			cls = "la-only-code"
		elif not ka:
			cls = "other-only-code"
		else:
			cls = "changed-code"
		dce.append({"module": short(m.name), "vs": tag, "other_module": short(o.name), "class": cls,
			"offset": ppos, "la_tokens": a1 - a0, "other_tokens": b1 - b0,
			"la_text": sa[:300], "other_text": sb[:300],
			"la_context": ctx(m.src, ppos, ppos + len(sa), 80)[:500]})


def classify(chain):
	"""Syntactic position of a literal from its acorn parent chain."""
	if not chain:
		return "program"
	t, k, comp, special = chain[-1]
	if t == "Property":
		if k == "key":
			return "property-key-computed" if comp else "property-key"
		return "property-value"
	if t == "AssignmentPattern":
		if len(chain) >= 2 and chain[-2][0] in ("FunctionDeclaration", "FunctionExpression",
				"ArrowFunctionExpression") and chain[-2][1] == "params":
			return "default-param"
		return "default-value"
	if t == "ReturnStatement":
		return "return-value"
	if t == "TemplateLiteral":
		return "template-substitution" if k == "expressions" else "template-text"
	if t == "ArrayExpression":
		return "array-element"
	if t in ("CallExpression", "NewExpression"):
		return "call-arg" if k == "arguments" else "callee"
	if t == "SwitchStatement":
		return "switch-discriminant"
	if t == "SwitchCase":
		return "case-test"
	if t == "BinaryExpression":
		return "binary-operand"
	if t == "LogicalExpression":
		return "logical-operand"
	if t == "ConditionalExpression":
		return "conditional-" + k
	if t == "VariableDeclarator":
		return "var-init"
	if t == "AssignmentExpression":
		return "assignment-value"
	if t == "MemberExpression":
		return "member-object" if k == "object" else "member-property"
	if t == "ArrowFunctionExpression":
		return "arrow-body"
	if t in ("ImportDeclaration", "ExportNamedDeclaration", "ExportAllDeclaration",
			"ImportExpression", "ImportSpecifier", "ExportSpecifier"):
		return "module-syntax"
	if t == "ExpressionStatement":
		return "expression-statement"
	return t + "." + k


def runtime_expr(parts, q):
	"""parts: raw string pieces and ('platform',) / ('arch',) markers -> JS expression."""
	terms = []
	for p in parts:
		if isinstance(p, tuple):
			terms.append("process." + p[0])
		elif p:
			terms.append(q + p + q)
	if len(terms) == 1:
		return terms[0]
	# A string operand first keeps `+` a concatenation whatever the values are.
	if not terms[0].startswith(q):
		terms.insert(0, q + q)
	return "(" + "+".join(terms) + ")"


def esc_split(parts):
	"""True if a folded word is preceded by an odd run of backslashes (an escape)."""
	for k, p in enumerate(parts):
		if isinstance(p, tuple) and k:
			prev = parts[k - 1]
			if isinstance(prev, str) and (len(prev) - len(prev.rstrip("\\"))) % 2:
				return True
	return False


def main():
	args = sys.argv[1:]
	node, reportf = "node", None
	if "--node" in args:
		i = args.index("--node"); node = args[i + 1]; del args[i:i + 2]
	if "--report" in args:
		i = args.index("--report"); reportf = args[i + 1]; del args[i:i + 2]
	if len(args) != 5:
		die("usage: find-folds.py <la-dir> <da-dir> <lx-dir> <acorn-dir> <out.json> [--report r.json] [--node NODE]")
	dirs = dict(zip(BUILDS, args[:3]))
	acorn, outf = os.path.abspath(args[3]), args[4]
	mans = {b: json.load(open(os.path.join(d, "manifest.json"))) for b, d in dirs.items()}
	vers = set()
	for b, mf in mans.items():
		cli = os.path.join(dirs[b], "root", "cli")
		if not os.path.exists(cli):
			die(b + ": no root/cli")
		mv = re.search(r"// Version: (\S+)", open(cli, encoding="utf-8").read(4096))
		vers.add(mv.group(1) if mv else None)
	if len(vers) != 1 or None in vers:
		die("builds are not the same version: %r" % vers)
	version = vers.pop()
	work = tempfile.mkdtemp(prefix="find-folds-")
	intern = {}
	mods = {}
	for b in BUILDS:
		mods[b] = load_build(node, acorn, os.path.abspath(dirs[b]), b, work)
		for m in mods[b].values():
			keys_of(m, intern)
		print("find-folds: %s %d JS modules" % (b, len(mods[b])), file=sys.stderr)
	os.rmdir(work)
	la = mods["la"]

	# Non-JS modules: compared by name and content hash.
	nonjs = {}
	for b in BUILDS:
		nonjs[b] = {m["name"]: m.get("contents_sha256") for m in mans[b]["modules"] if m["loader"] != 1}
	nonjs_diff = sorted(n for n in set(nonjs["la"]) | set(nonjs["da"]) | set(nonjs["lx"])
		if len({nonjs[b].get(n, "absent") for b in BUILDS}) > 1)

	sites = collections.defaultdict(dict)
	dce = []
	unmatched, pairing = {}, {}
	for tag in ("da", "lx"):
		pairs, method, ua, ub, amb = pair_modules(la, mods[tag])
		unmatched[tag] = {"la_only": [short(n) for n in ua], "other_only": [short(n) for n in ub],
			"ambiguous": [[[short(x) for x in g[0]], [short(y) for y in g[1]]] for g in amb]}
		pairing[tag] = collections.Counter(v.split(":")[0] for v in method.values())
		for n in sorted(pairs):
			diff_pair(la[n], mods[tag][pairs[n]], tag, sites, dce)
		print("find-folds: %s paired %d (%s), unmatched la=%d other=%d" % (tag, len(pairs),
			dict(pairing[tag]), len(ua), len(ub)), file=sys.stderr)
		pairing[tag + "_pairs"] = pairs

	# Decide each recorded literal, word by word. A "linux" is decided by darwin-arm64
	# (folded iff it reads "darwin" there), an "arm64" by linux-x64 (folded iff "x64");
	# the other build must agree with linux-arm64 at that word. A literal whose token
	# is not aligned in the deciding build stays unverified -- never assumed genuine.
	undecidable, folds, unverified, otherlit = [], [], [], []
	DECIDER = {"linux": ("da", "darwin", "platform"), "arm64": ("lx", "x64", "arch")}
	for (name, off), d in sorted(sites.items()):
		s0, s1, ta = d["tok"]
		m = la[name]
		cand = {t: d[t] for t in ("da", "lx") if t in d}
		base = {"module": short(name), "offset": s0, "la": ta, **cand,
			**{t + "_support": d[t + "_support"] for t in cand}}
		if d.get("name"):
			base["token"] = "name"
		pa = WORDS.split(ta)
		if len(pa) == 1:
			# no platform/arch word in linux-arm64, yet another build differs
			otherlit.append({**base, "context": ctx(m.src, s0, s1)})
			continue
		pb = {t: WORDS.split(tb) for t, tb in cand.items()}
		if any(len(v) != len(pa) or any(SPEC.sub("\x02", pa[k]) != SPEC.sub("\x02", v[k])
				for k in range(0, len(pa), 2)) for v in pb.values()):
			undecidable.append({**base, "why": "literal differs beyond platform/arch words",
				"context": ctx(m.src, s0, s1)})
			continue
		parts, kinds, bad, unknown = [], set(), None, []
		for k, w in enumerate(pa):
			if k % 2 == 0:
				parts.append(w)
				continue
			dec = DECIDER.get(w)
			for t, v in pb.items():
				if v[k] != w and not (dec and t == dec[0] and v[k] == dec[1]):
					bad = "unexpected substitution %s %r -> %r" % (t, w, v[k])
			if dec is None:
				parts.append(w)			# "darwin"/"x64" in linux-arm64: never a fold here
			elif dec[0] not in pb or d.get(dec[0] + "_support", 0) < SUPPORT:
				# not aligned in the deciding build, or aligned with too little
				# agreeing context around it to trust the alignment
				unknown.append(w); parts.append(w)
			elif pb[dec[0]][k] == dec[1]:
				kinds.add(dec[2]); parts.append((dec[2],))
			else:
				parts.append(w)			# same word in the deciding build: genuine
		if bad:
			undecidable.append({**base, "why": bad, "context": ctx(m.src, s0, s1)})
		elif unknown and kinds:
			undecidable.append({**base, "why": "partly decided; no well-aligned token in %s for %r"
				% (",".join(sorted({DECIDER[w][0] for w in unknown})), unknown),
				"context": ctx(m.src, s0, s1)})
		elif unknown:
			unverified.append({**base, "words": unknown,
				"undecided_in": sorted({DECIDER[w][0] for w in unknown}),
				"paired": {t: name in pairing[t + "_pairs"] for t in ("da", "lx")},
				"context": ctx(m.src, s0, s1, 40)})
		elif kinds:
			folds.append((name, s0, s1, ta, parts, kinds, cand, d.get("name", False)))
	# A literal aligned in neither build (module unpaired, or inside code that only
	# linux-arm64 kept) was never recorded: its platform/arch words are unverified too.
	for name, m in sorted(la.items()):
		for i in range(len(m.kind)):
			if m.kind[i] in (0, 2, 3) and (name, m.start[i]) not in sites:
				t = m.src[m.start[i]:m.end[i]]
				ws = [w for w in (WORDS.findall(t) if m.kind[i] else [t]) if w in DECIDER]
				if ws:
					unverified.append({"module": short(name), "offset": m.start[i], "la": t[:200],
						"words": ws, "undecided_in": sorted({DECIDER[w][0] for w in ws}),
						"paired": {x: name in pairing[x + "_pairs"] for x in ("da", "lx")},
						"context": ctx(m.src, m.start[i], m.end[i], 40)})
	unverified.sort(key=lambda r: (r["module"], r["offset"]))

	# acorn: the literal around each site and its syntactic position.
	req = collections.defaultdict(list)
	for name, s0, *_ in folds:
		req[os.path.join(os.path.abspath(dirs["la"]), name[len("/$bunfs/"):])].append(s0)
	with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as fh:
		json.dump(req, fh)
		reqf = fh.name
	loc = json.loads(run_node(node, LOCJS, [acorn, reqf], capture=True))
	os.unlink(reqf)
	out_sites = []
	for name, s0, s1, ta, parts, kinds, cand, is_name in folds:
		m = la[name]
		f = os.path.join(os.path.abspath(dirs["la"]), name[len("/$bunfs/"):])
		L = loc[f][req[f].index(s0)]
		rec = {"module": short(name), "offset": s0, "la": ta, **{t: cand[t] for t in sorted(cand)},
			"kind": "both" if len(kinds) == 2 else kinds.pop()}
		if L is None:
			undecidable.append({**rec, "why": "acorn finds no literal at the token"})
			continue
		if is_name:
			# A platform word as a name: `.arm64` / `?.arm64` (folded computed member) or
			# an object key `{arm64:...}` (folded computed key) become computed again.
			chain = [tuple(c) for c in L["chain"]]
			if (L["type"], L["start"], L["end"]) != ("Identifier", s0, s1) or not chain:
				undecidable.append({**rec, "why": "acorn identifier %r disagrees with token" % L})
				continue
			t, k, comp, special = chain[-1]
			expr = "process." + parts[1][0]
			if t == "MemberExpression" and k == "property" and not comp:
				pos = "member-property"
				dot = "?." if m.src[s0 - 2:s0] == "?." else "."
				if m.src[s0 - len(dot):s0] != dot:
					undecidable.append({**rec, "why": "no dot before member name"})
					continue
				s0 -= len(dot)
				repl = ("?." if dot == "?." else "") + "[" + expr + "]"
			elif t == "Property" and k == "key" and not comp and not special:
				pos = "property-key"
				repl = "[" + expr + "]"
			else:
				undecidable.append({**rec, "why": "platform word as a name in %s.%s" % (t, k)})
				continue
		elif m.kind[bisect.bisect_left(m.start, s0)] == 2:
			if (L["type"], L["start"], L["end"]) != ("Literal", s0, s1):
				undecidable.append({**rec, "why": "acorn literal %r disagrees with token" % L})
				continue
			pos = classify([tuple(c) for c in L["chain"]])
			q = ta[0]
			# Pieces stay raw source text, each re-quoted with the literal's own quote,
			# so escapes keep their meaning -- unless a folded word is itself the tail
			# of an escape (e.g. \x64), which is reported instead.
			if esc_split(parts):
				undecidable.append({**rec, "why": "folded word inside an escape", "position": pos})
				continue
			expr = runtime_expr(parts_strip(parts), q)
			if pos in ("module-syntax", "property-key", "expression-statement"):
				if pos == "property-key":
					repl = "[" + expr + "]"
				else:
					undecidable.append({**rec, "why": "literal in %s position" % pos})
					continue
			else:
				repl = expr
		else:
			# Template text: the token is the raw chunk between ` / } and ${ / `.
			pos = "template-text"
			if not (L["type"] == "TemplateElement"):
				undecidable.append({**rec, "why": "acorn element %r disagrees with token" % L})
				continue
			chain = [tuple(c) for c in L["chain"]]
			if len(chain) >= 2 and chain[-2][0] == "TaggedTemplateExpression":
				undecidable.append({**rec, "why": "tagged template text"})
				continue
			if esc_split(parts):
				undecidable.append({**rec, "why": "folded word inside an escape", "position": pos})
				continue
			repl = "".join(p if not isinstance(p, tuple) else "${process.%s}" % p[0] for p in parts)
		# Minified code abuts tokens (`return"linux"`, `"linux"in e`): keep them apart.
		idch = lambda ch: ch.isalnum() or ch in "_$\\"
		if pos not in ("template-text", "member-property"):
			if s0 and idch(m.src[s0 - 1]) and idch(repl[0]):
				repl = " " + repl
			if s1 < len(m.src) and idch(m.src[s1]) and idch(repl[-1]):
				repl = repl + " "
		rec.update({"position": pos, "start": s0, "end": s1, "original": m.src[s0:s1], "replacement": repl,
			"expression": repl.strip() if pos != "template-text" else "`…" + repl + "…`"})
		out_sites.append(rec)

	# Context snippets: smallest window, >= CTX each side, that occurs once in the
	# module and contains none of the text relocation rewrites.
	by_mod = collections.defaultdict(list)
	for r in out_sites:
		by_mod[r["module"]].append(r)
	final = []
	for mod in sorted(by_mod):
		src = la["/$bunfs/root/" + mod].src
		for r in sorted(by_mod[mod], key=lambda r: r["start"]):
			s0, s1 = r["start"], r["end"]
			w, snip = CTX, None
			while w <= MAX_CTX:
				a, b = max(0, s0 - w), min(len(src), s1 + w)
				c = src[a:b]
				if src.count(c) == 1 and not any(x in c for x in RELOC_TEXT):
					snip = (a, c)
					break
				w += 8
			if snip is None:
				undecidable.append({**{k: r[k] for k in ("module", "offset", "la")},
					"why": "no unique relocation-safe snippet within %d chars" % MAX_CTX,
					"context": ctx(src, s0, s1)})
				continue
			a, c = snip
			r["snippet"] = c
			r["snippet_offset"] = s0 - a
			r["relocate_rule"] = next((rule for pat, rule in P_RULES.items()
				if src[s0 - len("switch("):s1 + 1] == pat), None)
			del r["start"], r["end"]
			final.append(r)

	# A one-token run that is a decided name site (`.linux` vs `.darwin`) is a fold,
	# not dead code.
	site_at = {(r["module"], r["offset"]) for r in final}
	for d in dce:
		if d["la_tokens"] == 1 and d["other_tokens"] == 1 and (d["module"], d["offset"]) in site_at:
			d["class"] = "fold-site"
	helpers = platform_helpers(node, acorn, os.path.abspath(dirs["la"]), la)
	summary = {
		"version": version,
		"sites": len(final),
		"by_kind": dict(sorted(collections.Counter(r["kind"] for r in final).items())),
		"by_position": dict(sorted(collections.Counter(r["position"] for r in final).items())),
		"by_relocate_rule": dict(sorted(collections.Counter(str(r["relocate_rule"]) for r in final).items())),
		"modules_with_sites": len(by_mod),
		"pairing": {t: dict(sorted(pairing[t].items())) for t in ("da", "lx")},
		"unmatched_counts": {t: {k: len(v) for k, v in unmatched[t].items()} for t in ("da", "lx")},
		"dce_runs": {t: dict(sorted(collections.Counter(d["class"] for d in dce if d["vs"] == t).items()))
			for t in ("da", "lx")},
		"platform_helpers": dict(collections.Counter(h["kind"] for h in helpers)),
		"undecidable": len(undecidable),
		"other_literal_diffs": len(otherlit),
		"unverified_platform_words": len(unverified),
		"nonjs_differing": nonjs_diff,
		"inputs": {b: {"source_sha256": mans[b]["source_sha256"], "modules": len(mans[b]["modules"])} for b in BUILDS},
	}
	# <out.json>: what apply-folds.py consumes plus what needs a human decision.
	out = {"summary": summary, "sites": final, "undecidable": undecidable,
		"unverified": unverified, "platform_helpers": helpers}
	with open(outf, "w", encoding="utf-8") as fh:
		json.dump(out, fh, indent=1, ensure_ascii=False)
		fh.write("\n")
	if reportf:
		# --report: the dead-code audit input. Import/export-list runs are counted
		# only; a run repeated identically in many modules (a build define) is
		# collapsed to one record with its module count.
		groups = collections.defaultdict(list)
		for d in dce:
			if d["class"] != "import-export-list":
				groups[(d["vs"], d["class"], d["la_text"], d["other_text"])].append(d)
		runs = []
		for k, g in groups.items():
			if len(g) >= 10:
				runs.append({**g[0], "repeated_in_modules": len(g),
					"modules": sorted(x["module"] for x in g)[:5]})
			else:
				runs.extend(g)
		runs.sort(key=lambda d: (d["vs"], d["module"], d["offset"]))
		with open(reportf, "w", encoding="utf-8") as fh:
			json.dump({"summary": summary, "unmatched": unmatched, "other_literal_diffs": otherlit,
				"dce": runs}, fh, indent=1, ensure_ascii=False)
			fh.write("\n")
	print(json.dumps(summary, indent=1))


def parts_strip(parts):
	"""Quote-free pieces of a quoted literal split on words (first/last lose a quote)."""
	parts = list(parts)
	parts[0] = parts[0][1:]
	parts[-1] = parts[-1][:-1]
	return parts


# acorn: functions that branch on a platform value -- at least two distinct platform
# names tested (switch cases or ===/!== operands) in the same innermost function.
# "name-map" if that function also yields two or more distinct strings (returned,
# assigned, or ternary results), else "dispatch". Plus object tables keyed by two or
# more platform names with string values ("table").
HELPJS = r"""
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
const [acdir, ex] = process.argv.slice(2);
const acorn = await import(pathToFileURL(join(acdir, "dist/acorn.mjs")).href);
const man = JSON.parse(readFileSync(join(ex, "manifest.json"), "utf8"));
const PLAT = new Set(["aix", "android", "darwin", "freebsd", "haiku", "linux", "openbsd", "sunos", "win32", "cygwin", "netbsd"]);
const isFn = (n) => /^(FunctionDeclaration|FunctionExpression|ArrowFunctionExpression)$/.test(n.type);
const str = (n) => n && n.type === "Literal" && typeof n.value === "string";
const out = [];
for (const m of man.modules) {
	if (m.loader !== 1) continue;
	const src = readFileSync(join(ex, m.name.slice("/$bunfs/".length)), "utf8");
	const ast = acorn.parse(src, { ecmaVersion: "latest", sourceType: "module", allowHashBang: true });
	const stack = [{ node: ast, tests: new Set(), outs: new Set() }];
	const walk = (n) => {
		if (!n || typeof n.type !== "string") return;
		const fn = isFn(n);
		if (fn) stack.push({ node: n, tests: new Set(), outs: new Set() });
		const top = stack[stack.length - 1];
		if (n.type === "SwitchCase" && str(n.test) && PLAT.has(n.test.value)) top.tests.add(n.test.value);
		if (n.type === "BinaryExpression" && /^[!=]==?$/.test(n.operator))
			for (const s of [n.left, n.right]) if (str(s) && PLAT.has(s.value)) top.tests.add(s.value);
		if (n.type === "ReturnStatement" && str(n.argument)) top.outs.add(n.argument.value);
		if (n.type === "AssignmentExpression" && str(n.right)) top.outs.add(n.right.value);
		if (n.type === "ConditionalExpression") for (const s of [n.consequent, n.alternate]) if (str(s)) top.outs.add(s.value);
		if (n.type === "ObjectExpression") {
			const keys = n.properties.filter((p) => p.type === "Property" && !p.computed
				&& PLAT.has(p.key.type === "Identifier" ? p.key.name : p.key.value) && str(p.value));
			if (keys.length >= 2)
				out.push({ module: m.name, kind: "table", start_utf16: n.start,
					context: src.slice(n.start, Math.min(n.end, n.start + 500)),
					keys: keys.map((p) => p.key.name ?? p.key.value) });
		}
		for (const k of Object.keys(n)) {
			if (k === "type" || k === "start" || k === "end") continue;
			const v = n[k];
			if (Array.isArray(v)) v.forEach(walk); else if (v && typeof v.type === "string") walk(v);
		}
		if (fn) {
			const f = stack.pop();
			if (f.tests.size >= 2)
				out.push({ module: m.name, kind: f.outs.size >= 2 ? "name-map" : "dispatch", start_utf16: n.start,
					context: src.slice(n.start, Math.min(n.end, n.start + 500)),
					name: n.id?.name ?? null, tests: [...f.tests].sort(), outs: [...f.outs].sort().slice(0, 12) });
		}
	};
	walk(ast);
}
process.stdout.write(JSON.stringify(out));
"""


def platform_helpers(node, acorn, ladir, la):
	"""Helpers mapping a platform to a name set: these need deliberate haiku cases."""
	res = json.loads(run_node(node, HELPJS, [acorn, ladir], capture=True))
	out = [{**h, "module": short(h["module"])} for h in res]
	out.sort(key=lambda h: (h["module"], h["start_utf16"]))
	return out


if __name__ == "__main__":
	main()
