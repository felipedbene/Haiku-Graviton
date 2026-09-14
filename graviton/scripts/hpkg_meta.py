#!/usr/bin/env python3
# hpkg_meta.py -- read package metadata (name, version, revision, vendor,
# packager) straight out of a Haiku .hpkg WITHOUT any Haiku host tool.
#
# Why this exists: the DeBeOS repo-hygiene scripts (packager re-stamp #41, pool
# GC #42) need to *read* the packager/vendor/version of pooled packages to
# decide what to touch. The authoritative reader is Haiku's `package list -i`,
# which only runs on a Haiku host. This module parses the on-disk HPKG format
# directly (see docs/develop/packages/FileFormat.rst) so the *dry-run* / audit
# side runs anywhere -- including reading only the tail of an object over an S3
# ranged GET, so a whole pool can be scanned without downloading 6+ GB.
#
# It reads ONLY the package-attributes section (the small tree at the end of the
# heap that `package list -i` prints); it never touches file data. The apply /
# mutation side of the re-stamp still uses the real `package` tool on a Haiku
# host -- this module is read-only.
#
# Attribute IDs and encodings track headers/os/package/hpkg/PackageAttributes.h
# and docs/develop/packages/FileFormat.rst (HPKG v2).

import math
import struct
import zlib

HEADER_SIZE_MIN = 80
B_HPKG_MAGIC = b"hpkg"

# package-attributes section string-typed IDs we care about (PackageAttributes.h)
_ID_NAME = 15
_ID_VENDOR = 18
_ID_PACKAGER = 19
_ID_VERSION_MAJOR = 22
_ID_VERSION_MINOR = 23
_ID_VERSION_MICRO = 24
_ID_VERSION_REVISION = 25  # UINT
_ID_VERSION_PRE_RELEASE = 36

_WANT = {
    _ID_NAME: "name",
    _ID_VENDOR: "vendor",
    _ID_PACKAGER: "packager",
    _ID_VERSION_MAJOR: "version_major",
    _ID_VERSION_MINOR: "version_minor",
    _ID_VERSION_MICRO: "version_micro",
    _ID_VERSION_REVISION: "version_revision",
    _ID_VERSION_PRE_RELEASE: "version_prerelease",
}


class HpkgError(Exception):
    pass


def _leb128(buf, off):
    shift = 0
    result = 0
    while True:
        b = buf[off]
        off += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, off
        shift += 7


def parse_header(raw):
    if len(raw) < HEADER_SIZE_MIN or raw[:4] != B_HPKG_MAGIC:
        raise HpkgError("not an hpkg (bad magic)")
    hdr = {}
    hdr["header_size"] = struct.unpack(">H", raw[4:6])[0]
    hdr["version"] = struct.unpack(">H", raw[6:8])[0]
    hdr["total_size"] = struct.unpack(">Q", raw[8:16])[0]
    hdr["heap_compression"] = struct.unpack(">H", raw[18:20])[0]
    hdr["heap_chunk_size"] = struct.unpack(">I", raw[20:24])[0]
    hdr["heap_size_compressed"] = struct.unpack(">Q", raw[24:32])[0]
    hdr["heap_size_uncompressed"] = struct.unpack(">Q", raw[32:40])[0]
    hdr["attributes_length"] = struct.unpack(">I", raw[40:44])[0]
    hdr["attributes_strings_length"] = struct.unpack(">I", raw[44:48])[0]
    hdr["attributes_strings_count"] = struct.unpack(">I", raw[48:52])[0]
    return hdr


def tail_length_needed(hdr):
    """Bytes from the END of the file that a ranged reader must fetch to cover
    the package-attributes section (which sits at the very end of the heap).

    Covers: the last heap chunk(s) that hold the attributes section, plus the
    uint16 chunk-size array that lets us locate them, plus a safety margin.
    Callers that cannot compute this may just fetch the header + a generous tail
    (e.g. 512 KiB) -- attributes sections are small.
    """
    chunk = hdr["heap_chunk_size"] or 65536
    heap_u = hdr["heap_size_uncompressed"]
    n = math.ceil(heap_u / chunk) if heap_u else 1
    # attributes span; how many trailing chunks it touches (usually 1)
    attr = hdr["attributes_length"]
    chunks_touched = math.ceil((attr + chunk) / chunk) + 1
    # rough compressed bound: trailing chunks + chunk-size array
    return chunks_touched * chunk + 2 * n + 4096


def _decompress_uncompressed_range(raw, hdr, u_start, u_end):
    """Return the uncompressed heap bytes for [u_start, u_end) and the absolute
    uncompressed offset of the first returned byte. `raw` must contain the full
    file (offsets are absolute)."""
    hs = hdr["header_size"]
    chunk = hdr["heap_chunk_size"] or 65536
    heap_c = hdr["heap_size_compressed"]
    heap_u = hdr["heap_size_uncompressed"]
    comp = hdr["heap_compression"]
    if comp == 0:  # B_HPKG_COMPRESSION_NONE: no chunk-size array
        return raw[hs + u_start : hs + u_end], u_start
    if comp not in (1, 2):
        raise HpkgError("unsupported heap compression %d" % comp)

    n = math.ceil(heap_u / chunk) if heap_u else 1
    # uint16 chunk-size array (compressed size - 1) for all but the last chunk,
    # stored at the end of the compressed heap.
    arr_off = hs + heap_c - 2 * (n - 1)
    sizes = [struct.unpack(">H", raw[arr_off + 2 * i : arr_off + 2 * i + 2])[0] + 1
             for i in range(n - 1)]
    coff = [hs]
    for s in sizes:
        coff.append(coff[-1] + s)
    last_c = arr_off - coff[n - 1]  # implied compressed size of final chunk

    first = u_start // chunk
    last = (u_end - 1) // chunk
    out = bytearray()
    for i in range(first, last + 1):
        u_len = chunk if i < n - 1 else heap_u - (n - 1) * chunk
        c_len = sizes[i] if i < n - 1 else last_c
        cb = raw[coff[i] : coff[i] + c_len]
        if c_len == u_len:  # chunk stored uncompressed (compression didn't help)
            out += cb
        elif comp == 1:
            out += zlib.decompress(cb)
        else:  # zstd
            import zstandard  # optional; only if a zstd-compressed heap appears
            out += zstandard.ZstdDecompressor().decompress(cb, max_output_size=u_len)
    return bytes(out), first * chunk


def _parse_attribute_list(attr, off, strings, result):
    """Walk one attribute list (0-tag terminated), recursing into children,
    collecting the string/uint values in _WANT. `off` is an index into attr."""
    while True:
        tag, off = _leb128(attr, off)
        if tag == 0:
            return off
        t = tag - 1
        aid = t & 0x7F
        dtype = (t >> 7) & 0x7
        has_children = (t >> 10) & 1
        encoding = (t >> 11) & 0x3
        value = None
        if dtype in (1, 2):  # INT / UINT
            length = {0: 1, 1: 2, 2: 4, 3: 8}[encoding]
            value = int.from_bytes(attr[off : off + length], "big")
            off += length
        elif dtype == 3:  # STRING
            if encoding == 0:  # inline null-terminated
                end = attr.index(b"\x00", off)
                value = attr[off:end].decode("utf-8", "replace")
                off = end + 1
            else:  # index into strings table
                idx, off = _leb128(attr, off)
                value = strings[idx] if idx < len(strings) else None
        elif dtype == 4:  # RAW
            size, off = _leb128(attr, off)
            if encoding == 0:  # inline
                off += size
            else:  # heap ref: size + offset
                _heap_off, off = _leb128(attr, off)
        else:
            raise HpkgError("bad attribute data type %d" % dtype)
        if aid in _WANT and _WANT[aid] not in result:
            result[_WANT[aid]] = value
        if has_children:
            off = _parse_attribute_list(attr, off, strings, result)


def read_meta(raw):
    """Parse the package-attributes section of a full-file hpkg byte string.
    Returns {name, vendor, packager, version} (missing keys omitted)."""
    hdr = parse_header(raw)
    heap_u = hdr["heap_size_uncompressed"]
    attr_len = hdr["attributes_length"]
    astr_len = hdr["attributes_strings_length"]
    seg, base = _decompress_uncompressed_range(raw, hdr, heap_u - attr_len, heap_u)
    attr = seg[(heap_u - attr_len) - base : heap_u - base]

    # shared strings subsection: null-terminated strings, whole subsection
    # terminated by an extra 0 byte; astr_len covers it.
    strings = []
    o = 0
    while o < astr_len and attr[o] != 0:
        end = attr.index(b"\x00", o)
        strings.append(attr[o:end].decode("utf-8", "replace"))
        o = end + 1
    result = {}
    _parse_attribute_list(attr, astr_len, strings, result)

    # compose a human version string like `package list -i` / the filename
    parts = [result.get("version_major")]
    for k in ("version_minor", "version_micro"):
        if result.get(k):
            parts.append(result[k])
    ver = ".".join(p for p in parts if p)
    if result.get("version_prerelease"):
        ver += "~" + result["version_prerelease"]
    out = {
        "name": result.get("name"),
        "vendor": result.get("vendor"),
        "packager": result.get("packager"),
        "version": ver or None,
        "revision": result.get("version_revision"),
    }
    return out


def read_meta_file(path):
    with open(path, "rb") as f:
        return read_meta(f.read())


def read_meta_via_ranges(get_bytes):
    """Parse metadata using only ranged reads, for scanning a remote pool
    without downloading whole packages.

    `get_bytes(start, length)` MUST return exactly the file bytes
    [start, start+length). We fetch the header, then just the trailing chunk(s)
    plus the chunk-size array (the package-attributes section lives at the end of
    the heap), and reassemble a sparse buffer so the absolute-offset parser works
    unchanged.
    """
    head = get_bytes(0, HEADER_SIZE_MIN)
    hdr = parse_header(head)
    hs = hdr["header_size"]
    heap_c = hdr["heap_size_compressed"]
    total = hdr["total_size"]
    end_of_heap = hs + heap_c  # attributes + chunk-size array live here

    tail_len = min(tail_length_needed(hdr), end_of_heap - hs)
    tail_start = end_of_heap - tail_len
    tail = get_bytes(tail_start, tail_len)

    def assemble(t_start, t_bytes):
        buf = bytearray(total)
        buf[0:len(head)] = head
        buf[t_start:t_start + len(t_bytes)] = t_bytes
        return bytes(buf)

    buf = assemble(tail_start, tail)
    # Verify the chunks the parser will actually touch are inside the fetched
    # tail; if the attributes section spilled into an earlier chunk, widen once.
    chunk = hdr["heap_chunk_size"] or 65536
    heap_u = hdr["heap_size_uncompressed"]
    if hdr["heap_compression"] != 0 and heap_u:
        n = math.ceil(heap_u / chunk)
        first = (heap_u - hdr["attributes_length"]) // chunk
        arr_off = end_of_heap - 2 * (n - 1)
        sizes = [struct.unpack(">H", buf[arr_off + 2 * i : arr_off + 2 * i + 2])[0] + 1
                 for i in range(n - 1)]
        coff_first = hs + sum(sizes[:first])
        if coff_first < tail_start:
            widen = get_bytes(coff_first, end_of_heap - coff_first)
            buf = assemble(coff_first, widen)
    return read_meta(buf)


if __name__ == "__main__":
    import json
    import sys

    if len(sys.argv) < 2:
        sys.exit("usage: hpkg_meta.py <file.hpkg> [...]")
    for p in sys.argv[1:]:
        try:
            print(p, json.dumps(read_meta_file(p)))
        except Exception as exc:  # noqa: BLE001 - CLI diagnostic
            print(p, "ERROR", exc)
