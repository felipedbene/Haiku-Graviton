#!/usr/bin/env python3
"""Extract the embedded module graph from a Bun `--compile` standalone executable.

Read-only with respect to the input: the binary is parsed, never executed.
Writes every module (contents, plus sourcemap/bytecode when present) under OUTDIR,
and a manifest.json describing each record, the entry point and the compile argv.

Format (Bun StandaloneModuleGraph, as found in Bun v1.4.x ELF and Mach-O builds):
  ELF section `.bun` (Mach-O: `__BUN,__bun`)
                     = u64 payload_len | payload | "\\n---- Bun! ----\\n"
  payload tail       = Offsets { u64 byte_count; StringPointer modules;
                                 u32 entry_point_id; StringPointer compile_exec_argv;
                                 u32 flags }                       (32 bytes)
  modules            = N x { StringPointer name, contents, sourcemap, bytecode,
                             module_info, bytecode_origin_path;
                             u8 encoding, loader, module_format, side } (52 bytes)
  StringPointer      = { u32 offset; u32 length }, offsets relative to payload.
The layout is version-specific, so every size is checked and the run fails loudly
if the binary does not match it rather than guessing.

Only `contents` is written per module. The `bytecode` pointers are NOT per-module
blobs: each runs from that module's start to the end of one shared JSC bytecode
region, so writing them per module multiplies ~100 MB by the module count. That
region is also specific to the embedding Bun's JavaScriptCore build and cannot run
on any other engine, so it is recorded (offset, length, sha256) but not extracted.
"""
import hashlib, json, os, struct, sys

TRAILER = b"\n---- Bun! ----\n"
OFFSETS_SIZE = 32
RECORD_SIZE = 52
FIELDS = ("name", "contents", "sourcemap", "bytecode", "module_info", "bytecode_origin_path")


def die(msg):
	sys.exit("extract-bun-bundle: " + msg)


def macho_bun_section(d):
	# Thin little-endian Mach-O 64 (Bun's darwin builds): the payload is the
	# section `__BUN,__bun`, with the same u64 len | payload | trailer layout.
	ncmds, sizeofcmds = struct.unpack_from("<II", d, 16)
	if 32 + sizeofcmds > len(d):
		die("Mach-O load commands run past end of file")
	found, o = [], 32
	for _ in range(ncmds):
		cmd, cmdsize = struct.unpack_from("<II", d, o)
		if cmdsize < 8 or o + cmdsize > 32 + sizeofcmds:
			die("Mach-O load command at %d has bad size %d" % (o, cmdsize))
		if cmd == 0x19:		# LC_SEGMENT_64
			nsects, = struct.unpack_from("<I", d, o + 64)
			if 72 + nsects * 80 > cmdsize:
				die("Mach-O segment at %d: %d sections overflow the command" % (o, nsects))
			for j in range(nsects):
				so = o + 72 + j * 80
				sect, seg = d[so:so + 16].rstrip(b"\0"), d[so + 16:so + 32].rstrip(b"\0")
				size, off = struct.unpack_from("<QI", d, so + 40)
				if (seg, sect) == (b"__BUN", b"__bun"):
					if off + size > len(d):
						die("__BUN,__bun runs past end of file")
					found.append(d[off:off + size])
		o += cmdsize
	if len(found) != 1:
		die("expected exactly one __BUN,__bun section, found %d" % len(found))
	return found[0]


def bun_section(d):
	if d[:4] == b"\xcf\xfa\xed\xfe":
		return macho_bun_section(d)
	if d[:4] in (b"\xca\xfe\xba\xbe", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe"):
		die("fat, big-endian or 32-bit Mach-O: unsupported layout")
	if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
		die("not a little-endian ELF64 or Mach-O 64 file")
	shoff, = struct.unpack_from("<Q", d, 0x28)
	shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
	sh = lambda i: struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize)
	strtab_off = sh(shstrndx)[4]
	for i in range(shnum):
		s = sh(i)
		if d[strtab_off + s[0]:].split(b"\0", 1)[0] == b".bun":
			return d[s[4]:s[4] + s[5]]
	die("no .bun section: not a Bun standalone executable (or a non-ELF layout)")


def main():
	if len(sys.argv) != 3:
		die("usage: extract-bun-bundle.py <bun-standalone-binary> <outdir>")
	src, out = sys.argv[1], sys.argv[2]
	d = open(src, "rb").read()
	sec = bun_section(d)
	plen, = struct.unpack_from("<Q", sec, 0)
	t = sec.rfind(TRAILER)
	if t < 0:
		die("Bun trailer not found in .bun section")
	payload = sec[8:t]
	if plen != len(payload) + len(TRAILER) and plen != len(payload):
		die("payload length %d disagrees with trailer position %d" % (plen, len(payload)))
	o = len(payload) - OFFSETS_SIZE
	byte_count, moff, mlen, entry_id, aoff, alen, flags = struct.unpack_from("<QIIIIII", payload, o)
	if mlen % RECORD_SIZE:
		die("module table %d bytes is not a multiple of %d: unknown Bun layout" % (mlen, RECORD_SIZE))
	sp = lambda off, ln: payload[off:off + ln]
	os.makedirs(out, exist_ok=False)
	mods = []
	for k in range(mlen // RECORD_SIZE):
		r = moff + k * RECORD_SIZE
		ptrs = struct.unpack_from("<12I", payload, r)
		enc, loader, fmt, side = struct.unpack_from("<4B", payload, r + 48)
		f = {n: (ptrs[2 * i], ptrs[2 * i + 1]) for i, n in enumerate(FIELDS)}
		name = sp(*f["name"]).decode("utf-8")
		if not name.startswith("/$bunfs/"):
			die("record %d name %r is not a /$bunfs/ path: unknown Bun layout" % (k, name))
		rel = name[len("/$bunfs/"):].lstrip("/")
		m = {"index": k, "name": name, "encoding": enc, "loader": loader,
			"module_format": fmt, "side": side}
		m["bytecode_start"] = f["bytecode"][0] if f["bytecode"][1] else None
		for field, suffix in (("contents", ""), ("sourcemap", ".map"), ("module_info", ".modinfo")):
			off, ln = f[field]
			m[field + "_len"] = ln
			if ln:
				blob = sp(off, ln)
				# Mirror the /$bunfs/ layout exactly, so OUTDIR can stand in for /$bunfs.
				p = os.path.join(out, rel + suffix)
				os.makedirs(os.path.dirname(p), exist_ok=True)
				with open(p, "wb") as fh:
					fh.write(blob)
				m[field + "_sha256"] = hashlib.sha256(blob).hexdigest()
		bop = f["bytecode_origin_path"]
		m["bytecode_origin_path"] = sp(*bop).decode("utf-8") if bop[1] else None
		mods.append(m)
	starts = [m["bytecode_start"] for m in mods if m["bytecode_start"] is not None]
	ends = {f_off + f_len for f_off, f_len in (
		struct.unpack_from("<II", payload, moff + k * RECORD_SIZE + 24) for k in range(len(mods)))
		if f_len}
	if len(ends) > 1:
		die("bytecode pointers do not share one end (%d distinct): unknown Bun layout" % len(ends))
	region = None
	if starts:
		lo, hi = min(starts), ends.pop()
		region = {"offset": lo, "length": hi - lo,
			"sha256": hashlib.sha256(payload[lo:hi]).hexdigest()}
	manifest = {
		"source": os.path.basename(src),
		"bytecode_region": region,
		"source_sha256": hashlib.sha256(d).hexdigest(),
		"bun_version": next((d[i:i + 32].split(b" (")[0].decode() for i in [d.find(b"Bun v1.")] if i >= 0), None),
		"byte_count": byte_count, "flags": flags,
		"entry_point": mods[entry_id]["name"] if entry_id < len(mods) else None,
		"compile_exec_argv": sp(aoff, alen).decode("utf-8", "replace"),
		"modules": mods,
	}
	with open(os.path.join(out, "manifest.json"), "w") as fh:
		json.dump(manifest, fh, indent=1)
	tot = lambda k: sum(m[k + "_len"] for m in mods)
	print("modules=%d entry=%s argv=%r" % (len(mods), manifest["entry_point"], manifest["compile_exec_argv"]))
	print("contents=%.1fMB sourcemap=%.1fMB module_info=%.1fMB bytecode_region=%s" % (
		tot("contents") / 1e6, tot("sourcemap") / 1e6, tot("module_info") / 1e6,
		"%.1fMB (not extracted)" % (region["length"] / 1e6) if region else "none"))


if __name__ == "__main__":
	main()
