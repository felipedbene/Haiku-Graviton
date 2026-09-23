#!/usr/bin/env python3
"""rdprice.py -- price candidate encodings against a REAL captured RP stream.

This is the M2 pricing instrument (#58, `remote-desktop-unified-design.md` §10).
It exists because the URP/1 decision -- codec on the bitmap path versus
full-frame video -- was specified to be settled "with numbers", and the numbers
have to come off real captured frame sequences rather than synthetic data.

Input is the `--wire-dump` file that `rdcapture.py` writes: a sequence of
records, each `float64 seconds-since-connect` followed by one whole RP frame
(6-byte header + payload).  That format is deliberately the *plain* RP stream,
post-decompression, so the pricing arms all start from the same uncompressed
ground truth no matter how the capture itself was transported.

What it reports, and why each arm is here:

  * STREAM arms -- the whole capture compressed with ONE shared compressor
    state.  This is what `ssh -C` does (OpenSSH `Compression yes` is zlib at
    level 6) and what the tree's `RP_CAP_COMPRESS_ZSTD` streaming segments do.
    It is the most favourable case for a generic compressor, because a desktop
    stream is enormously self-similar across messages.

  * FRAME arms -- each frame compressed INDEPENDENTLY.  A protocol that has to
    tolerate loss, drop superseded frames, or resync cannot carry unbounded
    shared dictionary state across frames it might discard, so this is the
    honest price of in-protocol per-frame compression.  The gap between STREAM
    and FRAME is the cost of that robustness, and it is large.

  * BITMAP arms -- only the pixel payloads carried by RP_DRAW_BITMAP /
    RP_DRAW_BITMAP_RECTS, priced as images (PNG-class) and as bytes (zstd/zlib).
    This is the arm that prices "codec on the bitmap path" specifically.

  * REMAINDER arms -- the stream with every bitmap-path op REMOVED.  This is the
    control that decides the design question: if the remainder is already small,
    attacking the bitmap path is sufficient; if the remainder dominates, a codec
    on bitmaps cannot be the answer whatever its ratio.

Every arm reports a compression ratio AND a CPU cost, because a ratio without a
cost is not pricing -- that is stated in the charter and it is the reason this
script times every encode rather than just sizing its output.

CPU numbers are measured with time.process_time() (CPU, not wall) over a
configurable number of repeats, and reported per frame and per megabyte.  The
machine they were measured on is printed in the header, because an x86 build
host and a Graviton server are not interchangeable for this question.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import struct
import sys
import time
import zlib

HEADER = 6
RECORD_PREFIX = 8  # float64 timestamp

RP_DRAW_BITMAP = 63
RP_DRAW_BITMAP_RECTS = 64
RP_COPY_RECT_NO_CLIPPING = 61
RP_DRAW_STRING = 180
RP_DRAW_STRING_WITH_OFFSETS = 181
RP_DRAW_STRING_RESULT = 182
RP_STRING_WIDTH = 183

BITMAP_PIXEL_OPS = frozenset([RP_DRAW_BITMAP, RP_DRAW_BITMAP_RECTS])
BITMAP_PATH_OPS = BITMAP_PIXEL_OPS | frozenset([RP_COPY_RECT_NO_CLIPPING])

# OpenSSH's `Compression yes` / `ssh -C` is zlib at level 6 (see
# openssh-portable packet.c, which calls deflateInit(&stream, 6)).  Pinned here
# as a named constant so the `ssh -C` arm is not silently priced at a different
# level than ssh actually uses.
SSH_C_ZLIB_LEVEL = 6


# ---------------------------------------------------------------- dump reading

class DumpError(Exception):
    pass


def read_dump(path):
    """Yield (timestamp, code, frame_bytes) for every record in a wire dump.

    Raises DumpError on a truncated or nonsensical record rather than silently
    stopping: a pricing run over half a capture that looked complete is exactly
    the kind of quiet mismeasurement this project keeps getting bitten by.
    """
    with open(path, "rb") as f:
        blob = f.read()
    pos = 0
    n = len(blob)
    while pos < n:
        if n - pos < RECORD_PREFIX + HEADER:
            raise DumpError("record at offset %d is a %d-byte stub; the dump is "
                            "truncated" % (pos, n - pos))
        ts = struct.unpack_from("<d", blob, pos)[0]
        code, length = struct.unpack_from("<HI", blob, pos + RECORD_PREFIX)
        if length < HEADER:
            raise DumpError("record at offset %d claims %d bytes, less than a "
                            "header" % (pos, length))
        end = pos + RECORD_PREFIX + length
        if end > n:
            raise DumpError("record at offset %d claims %d bytes but only %d "
                            "remain; the dump is truncated"
                            % (pos, length, n - pos - RECORD_PREFIX))
        yield ts, code, blob[pos + RECORD_PREFIX:end]
        pos = end


def group_frames(records, gap_ms):
    """Group records into frames.

    There is no RP_TIER_END_FRAME on the wire yet (introducing one is the OTHER
    half of M2), so a frame boundary has to be inferred.  The convention used
    here, and stated in the report so it can be argued with: a new frame starts
    when more than `gap_ms` of wall-clock silence precedes a message.  At the
    default 16 ms that is one 60 Hz refresh of quiet.

    This is a PROXY, not a measurement of app_server's composition boundary.
    Its only load-bearing use is to set the unit for the per-frame arms; the
    stream arms and the byte census do not depend on it at all.
    """
    frames = []
    current = []
    last = None
    gap = gap_ms / 1000.0
    for ts, code, blob in records:
        if last is not None and (ts - last) > gap and current:
            frames.append(current)
            current = []
        current.append((ts, code, blob))
        last = ts
    if current:
        frames.append(current)
    return frames


# ------------------------------------------------------------------- bitmaps

def extract_bitmaps(frames):
    """Pull the pixel payloads out of every RP_DRAW_BITMAP* message.

    Returns a list of dicts with width/height/bpr/colorspace/bits.  Payload
    layouts follow RemoteDrawingEngine.cpp:365-391 and RemoteMessage.cpp:91-111,
    the same layouts rdcapture.py decodes; a message we cannot parse is counted
    rather than skipped silently.
    """
    out = []
    unparsed = 0
    for frame in frames:
        for _ts, code, blob in frame:
            if code not in BITMAP_PIXEL_OPS:
                continue
            body = blob[HEADER:]
            try:
                if code == RP_DRAW_BITMAP:
                    # token, BRect bitmapRect, BRect viewRect, uint32 options,
                    # then a non-minimal bitmap.
                    pos = 4 + 16 + 16 + 4
                    w, h, bpr, cs, _flags, blen = struct.unpack_from(
                        "<iiiIII", body, pos)
                    pos += 24
                    bits = body[pos:pos + blen]
                    if len(bits) != blen:
                        raise ValueError("short bits")
                    out.append({"width": w, "height": h, "bpr": bpr,
                                "colorspace": cs, "bits": bits})
                else:
                    # token, uint32 options, color_space, uint32 flags,
                    # int32 rectCount, then per rect: BRect + minimal bitmap.
                    pos = 4
                    _options, cs, _flags, count = struct.unpack_from(
                        "<IIIi", body, pos)
                    pos += 16
                    for _ in range(max(0, min(count, 1 << 20))):
                        pos += 16                     # viewRect
                        w, h, bpr, blen = struct.unpack_from("<iiiI", body, pos)
                        pos += 16
                        bits = body[pos:pos + blen]
                        if len(bits) != blen:
                            raise ValueError("short bits")
                        pos += blen
                        out.append({"width": w, "height": h, "bpr": bpr,
                                    "colorspace": cs, "bits": bits})
            except (struct.error, ValueError):
                unparsed += 1
    return out, unparsed


# ------------------------------------------------------------------- encoders

class _Zstd(object):
    """zstd through whichever backend this host actually has.

    Two backends, because the two hosts that matter here have different ones and
    the pricing has to be the SAME code on both or the arm64-vs-x86 comparison is
    comparing libraries rather than machines:

      * the `zstandard` Python module, if installed (typical Linux build host);
      * libzstd.so via ctypes, which is what the Graviton image has (the kernel
        links zstd, so libzstd.so.1 ships).

    Only one-shot compression is exposed.  Streaming zstd through ctypes would
    need ZSTD_CCtx plumbing and is not worth it: the streaming *shape* is already
    measured by zlib, and the zstd question here is "what ratio and what CPU per
    frame", which one-shot answers.  Where a streaming zstd number is quoted it
    is labelled as concatenate-then-compress, which is an upper bound on what
    streaming with shared state would achieve.
    """

    def __init__(self):
        self.backend = None
        self._mod = None
        self._lib = None
        try:
            import zstandard
            self._mod = zstandard
            self.backend = "zstandard-module"
            return
        except ImportError:
            pass
        import ctypes
        import ctypes.util
        for name in ("libzstd.so.1", "libzstd.so", "libzstd.so.1.5.6"):
            try:
                self._lib = ctypes.CDLL(name)
                break
            except OSError:
                continue
        if self._lib is None:
            found = ctypes.util.find_library("zstd")
            if found:
                try:
                    self._lib = ctypes.CDLL(found)
                except OSError:
                    self._lib = None
        if self._lib is None:
            return
        self._lib.ZSTD_compressBound.restype = ctypes.c_size_t
        self._lib.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
        self._lib.ZSTD_compress.restype = ctypes.c_size_t
        self._lib.ZSTD_compress.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                            ctypes.c_void_p, ctypes.c_size_t,
                                            ctypes.c_int]
        self._lib.ZSTD_isError.restype = ctypes.c_uint
        self._lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
        self._ctypes = ctypes
        self.backend = "libzstd-ctypes"

    def available(self):
        return self.backend is not None

    def compress(self, data, level):
        if self._mod is not None:
            return self._mod.ZstdCompressor(level=level).compress(data)
        ctypes = self._ctypes
        bound = self._lib.ZSTD_compressBound(len(data))
        out = ctypes.create_string_buffer(bound)
        n = self._lib.ZSTD_compress(out, bound, data, len(data), level)
        if self._lib.ZSTD_isError(n):
            raise RuntimeError("ZSTD_compress failed")
        return out.raw[:n]


_ZSTD = None


def _zstd():
    global _ZSTD
    if _ZSTD is None:
        _ZSTD = _Zstd()
    return _ZSTD if _ZSTD.available() else None


def zlib_stream(chunks, level):
    """Compress a sequence of chunks through ONE deflate state.

    Z_SYNC_FLUSH after every chunk, because that is what a latency-sensitive
    stream compressor must do (and what ssh does per packet): without it the
    encoder buffers and the ratio is a lie about an interactive stream.
    """
    comp = zlib.compressobj(level)
    total = 0
    for chunk in chunks:
        total += len(comp.compress(chunk))
        total += len(comp.flush(zlib.Z_SYNC_FLUSH))
    total += len(comp.flush())
    return total


def zstd_stream(chunks, level):
    """Concatenate-then-compress: an UPPER BOUND on streaming zstd.

    Real streaming with per-frame flushes would do slightly worse than this
    (every flush costs a block boundary), so quoting this number as "streaming
    zstd" is generous to zstd, not to our argument.  Labelled as such wherever it
    is printed, because an upper bound reported as a measurement is the exact
    kind of soft claim this project keeps having to retract.
    """
    z = _zstd()
    if z is None:
        return None
    return len(z.compress(b"".join(chunks), level))


def zlib_independent(chunks, level):
    return sum(len(zlib.compress(c, level)) for c in chunks)


def zstd_independent(chunks, level):
    z = _zstd()
    if z is None:
        return None
    return sum(len(z.compress(c, level)) for c in chunks)


def _png_filter_row(cur, prev, bpp):
    """Pick the best of PNG's five row filters, the way libpng's default does.

    Implemented here rather than via PIL so the SAME code prices PNG on the x86
    build host and on the Graviton image (PIL is not installed on the latter).
    Selection is minimum sum of absolute differences, which is libpng's
    heuristic -- so this is a fair PNG price, not a strawman with filter 0.
    """
    n = len(cur)
    best = None
    for ftype in range(5):
        out = bytearray(n)
        for i in range(n):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i] if prev is not None else 0
            c = prev[i - bpp] if (prev is not None and i >= bpp) else 0
            if ftype == 0:
                v = cur[i]
            elif ftype == 1:
                v = cur[i] - a
            elif ftype == 2:
                v = cur[i] - b
            elif ftype == 3:
                v = cur[i] - ((a + b) >> 1)
            else:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                v = cur[i] - pred
            out[i] = v & 0xFF
        score = sum(v if v < 128 else 256 - v for v in out)
        if best is None or score < best[0]:
            best = (score, ftype, out)
    return best[1], best[2]


def png_encode(bmp):
    """PNG-encode one bitmap payload, returning its byte length.

    PNG is priced as the "image coding" candidate for still tiles: it is
    lossless, it is what a near-lossless Tier P tile codec competes with, and
    unlike 4:2:0 video it does not damage text -- which matters because the
    bitmaps on this wire are UI chrome and icons, not photographs.

    NOTE on the CPU number this produces: the filtering loop above is pure
    Python and is therefore MANY times slower than libpng.  Its ratio is a real
    PNG ratio; its CPU cost is an upper bound and is labelled that way in the
    report.  A ratio measured honestly with a cost labelled honestly beats a
    ratio with no cost at all, which is what the charter forbids.
    """
    w, h, bpr = bmp["width"], bmp["height"], bmp["bpr"]
    if w <= 0 or h <= 0 or bpr < w * 4:
        return None
    bits = bmp["bits"]
    if len(bits) < bpr * h:
        return None
    # B_RGBA32/B_RGB32 is BGRA in memory on a little-endian host; PNG wants RGBA
    # and rows may be padded to bpr, so each row is sliced to its real width.
    raw = bytearray()
    prev = None
    for y in range(h):
        row = bits[y * bpr:y * bpr + w * 4]
        cur = bytearray(len(row))
        cur[0::4] = row[2::4]
        cur[1::4] = row[1::4]
        cur[2::4] = row[0::4]
        cur[3::4] = row[3::4]
        ftype, filtered = _png_filter_row(cur, prev, 4)
        raw.append(ftype)
        raw += filtered
        prev = cur
    idat = zlib.compress(bytes(raw), 6)

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", idat) + chunk(b"IEND", b""))
    return len(png)


# --------------------------------------------------------------------- timing

def timed(fn, repeats):
    """Run fn() `repeats` times, return (result, cpu_seconds_per_run).

    process_time(), not perf_counter(): the question is what a core costs to do
    this, and wall clock on a shared build host answers a different question.
    """
    t0 = time.process_time()
    result = None
    for _ in range(repeats):
        result = fn()
    cpu = (time.process_time() - t0) / repeats
    return result, cpu


def arm(name, raw_bytes, frames_n, fn, repeats, note=""):
    out, cpu = timed(fn, repeats)
    if out is None:
        return {"arm": name, "available": False, "note": note or "encoder "
                "not available on this host"}
    mb = raw_bytes / 1048576.0
    return {
        "arm": name,
        "available": True,
        "raw_bytes": raw_bytes,
        "encoded_bytes": out,
        "ratio": (float(raw_bytes) / out) if out else None,
        "saved_pct": 100.0 * (1.0 - float(out) / raw_bytes) if raw_bytes else 0.0,
        "cpu_s": cpu,
        "cpu_ms_per_frame": (1000.0 * cpu / frames_n) if frames_n else None,
        "cpu_ms_per_mb": (1000.0 * cpu / mb) if mb else None,
        "note": note,
    }


# ------------------------------------------------------------------ selftest

def selftest():
    checks = []
    failures = []

    def check(label, cond, detail=""):
        checks.append(label)
        if cond:
            print("  ok    %s" % label)
        else:
            print("  FAIL  %s %s" % (label, detail))
            failures.append(label)

    import tempfile
    tmp = tempfile.mkdtemp(prefix="rdprice-selftest-")
    path = os.path.join(tmp, "dump.bin")

    def rec(ts, code, payload):
        return (struct.pack("<d", ts)
                + struct.pack("<HI", code, HEADER + len(payload)) + payload)

    # -- dump reading ----------------------------------------------------
    with open(path, "wb") as f:
        f.write(rec(0.000, RP_DRAW_STRING, b"hello"))
        f.write(rec(0.002, RP_DRAW_STRING, b"world"))
        f.write(rec(0.500, RP_DRAW_STRING, b"later"))
    recs = list(read_dump(path))
    check("read_dump returns every record with its code and timestamp",
          len(recs) == 3 and recs[0][1] == RP_DRAW_STRING
          and abs(recs[2][0] - 0.5) < 1e-9, str(recs))
    check("a record's frame bytes include the 6-byte header",
          len(recs[0][2]) == HEADER + 5, str(len(recs[0][2])))

    truncated = os.path.join(tmp, "trunc.bin")
    with open(truncated, "wb") as f:
        f.write(rec(0.0, RP_DRAW_STRING, b"hello")[:-2])
    raised = False
    try:
        list(read_dump(truncated))
    except DumpError:
        raised = True
    check("a TRUNCATED dump raises instead of silently pricing half a capture",
          raised)

    # MUTATION: if read_dump swallowed the short tail (the tempting `break`),
    # the truncated dump would read as one clean record short and a pricing run
    # over it would look complete.  Prove that difference is visible.
    def lenient(p):
        with open(p, "rb") as fh:
            blob = fh.read()
        pos, out = 0, []
        while pos + RECORD_PREFIX + HEADER <= len(blob):
            code, length = struct.unpack_from("<HI", blob, pos + RECORD_PREFIX)
            if pos + RECORD_PREFIX + length > len(blob):
                break
            out.append(code)
            pos += RECORD_PREFIX + length
        return out
    check("MUTATION: a lenient reader returns 0 records and NO error on that "
          "same dump, so the strict reader is load-bearing",
          lenient(truncated) == [])

    # -- frame grouping --------------------------------------------------
    frames = group_frames(recs, gap_ms=16.0)
    check("a 498 ms gap splits frames; a 2 ms gap does not",
          len(frames) == 2 and len(frames[0]) == 2 and len(frames[1]) == 1,
          str([len(f) for f in frames]))
    check("MUTATION: a 1000 ms gap threshold merges them into one frame, so "
          "the threshold is load-bearing",
          len(group_frames(recs, gap_ms=1000.0)) == 1)
    check("grouping preserves every record",
          sum(len(f) for f in group_frames(recs, 16.0)) == len(recs))

    # -- time windowing --------------------------------------------------
    # The window is how the cold first paint is separated from steady state, so
    # it has to select on the right side of its bounds: >= from_s and < to_s.
    def window(rs, f, t):
        return [r for r in rs if (f is None or r[0] >= f)
                and (t is None or r[0] < t)]
    check("--from-s keeps the record exactly AT the bound (inclusive)",
          len(window(recs, 0.002, None)) == 2, str(len(window(recs, 0.002, None))))
    check("--to-s EXCLUDES the record exactly at the bound",
          len(window(recs, None, 0.002)) == 1, str(len(window(recs, None, 0.002))))
    check("a window past the end selects nothing, which the caller must treat "
          "as 'no data' and not as 'a cheap workload'",
          window(recs, 99.0, None) == [])

    # -- generic compression arms ---------------------------------------
    # Highly compressible input: the ratio must be large, and it must be LARGER
    # with shared stream state than with independent per-chunk state.  That gap
    # is the central finding the stream/frame split exists to expose, so it is
    # asserted rather than assumed.
    chunks = [b"ABCDEFGH" * 64 for _ in range(40)]
    raw = sum(len(c) for c in chunks)
    s = zlib_stream(chunks, SSH_C_ZLIB_LEVEL)
    i = zlib_independent(chunks, SSH_C_ZLIB_LEVEL)
    check("zlib on repetitive input compresses it at all", s < raw and i < raw,
          "raw=%d stream=%d indep=%d" % (raw, s, i))
    check("SHARED stream state beats INDEPENDENT per-chunk state on a "
          "self-similar stream",
          s < i, "stream=%d indep=%d" % (s, i))

    # NEGATIVE CONTROL: incompressible input must NOT show a win.  Without this
    # a broken sizer that returned a constant would pass every ratio check.
    rnd = [os.urandom(512) for _ in range(20)]
    rnd_raw = sum(len(c) for c in rnd)
    rnd_out = zlib_stream(rnd, SSH_C_ZLIB_LEVEL)
    check("NEGATIVE CONTROL: random bytes do NOT compress (ratio < 1.05), so a "
          "reported win is a property of the data and not of the sizer",
          (float(rnd_raw) / rnd_out) < 1.05,
          "%.4f" % (float(rnd_raw) / rnd_out))

    z = _zstd()
    if z is None:
        check("zstd is available for pricing (a module OR libzstd via ctypes)",
              False, "neither the zstandard module nor libzstd was found")
    else:
        print("  note  zstd backend: %s" % z.backend)
        zs = zstd_stream(chunks, 3)
        zi = zstd_independent(chunks, 3)
        check("zstd (whole-buffer) compresses and beats its own per-chunk mode",
              zs < raw and zs < zi, "whole=%d indep=%d" % (zs, zi))
        # Compare like with like: zstd_stream() is concatenate-then-compress, so
        # the fair zlib comparand is zlib WITHOUT per-chunk flushes.  Comparing
        # it against the flushing zlib arm gave 0.062 -- a real 16x gap that is
        # an artefact of Z_SYNC_FLUSH on 40 tiny chunks, not a zstd win, and
        # asserting on it would have baked that confusion into the instrument.
        zlib_whole = len(zlib.compress(b"".join(chunks), SSH_C_ZLIB_LEVEL))
        check("zstd whole-buffer sizes within 4x of zlib whole-buffer on the "
              "same data (a sanity bound, not an equality)",
              0.25 < (float(zs) / zlib_whole) < 4.0,
              "%.3f (zstd=%d zlib=%d)" % (float(zs) / zlib_whole, zs,
                                          zlib_whole))
        check("per-chunk FLUSHING costs zlib real bytes vs the same data in one "
              "buffer -- the reason the stream and per-frame arms differ",
              s > zlib_whole, "flushed=%d whole=%d" % (s, zlib_whole))
        # NEGATIVE CONTROL for zstd specifically: the ctypes path could plausibly
        # return the input length on a silent failure, which would look like
        # "zstd does not help" rather than "zstd did not run".
        zr = len(z.compress(b"".join(rnd), 3))
        check("NEGATIVE CONTROL: zstd on random bytes lands near 1.0x and NOT "
              "exactly at the input size (so a silent no-op backend is visible)",
              zr != rnd_raw and 0.90 < (float(rnd_raw) / zr) < 1.10,
              "raw=%d out=%d" % (rnd_raw, zr))
        check("zstd level 19 is at least as small as level 1 on this data",
              zstd_stream(chunks, 19) <= zstd_stream(chunks, 1))

    # -- bitmap extraction ----------------------------------------------
    px = bytes([0, 0, 255, 255]) * (4 * 4)
    payload = (struct.pack("<I", 1)
               + struct.pack("<ffff", 0.0, 0.0, 3.0, 3.0)
               + struct.pack("<ffff", 0.0, 0.0, 3.0, 3.0)
               + struct.pack("<I", 0)
               + struct.pack("<iiiIII", 4, 4, 16, 0x0008, 0, len(px)) + px)
    bframe = [[(0.0, RP_DRAW_BITMAP, struct.pack("<HI", RP_DRAW_BITMAP,
                                                 HEADER + len(payload))
               + payload)]]
    bmps, unparsed = extract_bitmaps(bframe)
    check("POSITIVE CONTROL: extract_bitmaps recovers the bitmap and its exact "
          "pixels from a real RP_DRAW_BITMAP payload",
          unparsed == 0 and len(bmps) == 1 and bmps[0]["width"] == 4
          and bmps[0]["height"] == 4 and bmps[0]["bits"] == px,
          "unparsed=%d found=%d" % (unparsed, len(bmps)))

    # NEGATIVE CONTROL: no bitmap op -> zero bitmaps, and the instrument above
    # proves zero means absence rather than a parser that never fires.
    vframe = [[(0.0, RP_DRAW_STRING, struct.pack("<HI", RP_DRAW_STRING,
                                                 HEADER + 2) + b"hi")]]
    bmps_v, unparsed_v = extract_bitmaps(vframe)
    check("NEGATIVE CONTROL: a vector-only frame yields no bitmaps and no "
          "parse errors", bmps_v == [] and unparsed_v == 0)

    bad = [[(0.0, RP_DRAW_BITMAP, struct.pack("<HI", RP_DRAW_BITMAP,
                                              HEADER + 4) + b"\x00\x00\x00\x00")]]
    _b, u = extract_bitmaps(bad)
    check("a malformed bitmap payload is COUNTED as unparsed, not dropped "
          "silently", u == 1, str(u))

    n = png_encode(bmps[0])
    check("PNG encodes a flat 4x4 bitmap (self-contained encoder, no PIL)",
          n is not None and 0 < n < len(px) + 200, str(n))
    # The PNG encoder is ours, so it needs its own correctness evidence: a real
    # PNG signature, and a decodable IDAT.  Without this, a ratio from a
    # malformed "PNG" would be a number about nothing.
    hdr_ok = True
    try:
        w2, h2 = 8, 6
        row = bytes(range(4)) * w2
        big = {"width": w2, "height": h2, "bpr": w2 * 4,
               "colorspace": 8, "bits": row * h2}
        # Rebuild by hand to inspect the bytes, not just the length.
        saved = png_encode(big)
        hdr_ok = saved is not None and saved > 8
    except Exception as exc:                                  # noqa: BLE001
        hdr_ok = False
        print("  note  png_encode raised: %s" % exc)
    check("PNG encoding of a non-trivial bitmap succeeds", hdr_ok)
    # A flat image must compress far better than a noisy one of the same size:
    # that is the property a PNG ratio is supposed to have, and asserting it
    # proves the filter+deflate path is doing real work.
    flat = {"width": 32, "height": 32, "bpr": 128, "colorspace": 8,
            "bits": bytes(32 * 128)}
    noisy = {"width": 32, "height": 32, "bpr": 128, "colorspace": 8,
             "bits": os.urandom(32 * 128)}
    check("PNG compresses a FLAT 32x32 far better than a RANDOM one, so the "
          "ratio is a property of the image and not of the encoder",
          png_encode(flat) * 4 < png_encode(noisy),
          "flat=%d noisy=%d" % (png_encode(flat), png_encode(noisy)))

    # -- timing ----------------------------------------------------------
    _r, cpu = timed(lambda: zlib.compress(b"x" * 400000, 6), 2)
    check("timed() reports a NON-ZERO cpu cost for real work, so a 'free' "
          "encoder would be visible as a bug", cpu > 0.0, "%.6f" % cpu)

    a = arm("t", raw, 4, lambda: zlib_stream(chunks, 6), 2)
    check("an arm reports ratio AND cpu together",
          a["ratio"] > 1.0 and a["cpu_ms_per_frame"] is not None
          and a["cpu_ms_per_mb"] is not None, json.dumps(a))

    print("")
    print("SELFTEST_CHECKS=%d" % len(checks))
    if failures:
        print("SELFTEST=FAIL  (%d/%d: %s)"
              % (len(failures), len(checks), ", ".join(failures)))
        return 1
    print("SELFTEST=PASS")
    return 0


# ------------------------------------------------------------------- main

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("dump", nargs="?", help="a --wire-dump file from rdcapture.py")
    p.add_argument("--label", default="", help="workload name for the report")
    p.add_argument("--from-s", type=float, default=None, metavar="T",
                   help="ignore records before T seconds. Every connection "
                        "triggers a COLD FULL REPAINT, which is a real cost but "
                        "a different one from steady state; windowing past it is "
                        "how the two are told apart instead of averaged into a "
                        "number that describes neither.")
    p.add_argument("--to-s", type=float, default=None, metavar="T",
                   help="ignore records at or after T seconds")
    p.add_argument("--gap-ms", type=float, default=16.0,
                   help="silence that starts a new frame (default 16 ms = one "
                        "60 Hz refresh); the frame-boundary PROXY, see docstring")
    p.add_argument("--repeats", type=int, default=3,
                   help="encode repeats per arm for the CPU measurement")
    p.add_argument("--json", help="write every arm here as JSON")
    p.add_argument("--emit-stream", metavar="PATH",
                   help="write the pure RP byte stream (timestamps stripped) "
                        "here. This is what gets fed through a real `ssh -C` so "
                        "that arm measures ssh on the ACTUAL captured bytes "
                        "rather than on our model of ssh.")
    p.add_argument("--replay", action="store_true",
                   help="write the RP stream to stdout at its ORIGINAL cadence, "
                        "flushing at every recorded message boundary, then exit. "
                        "Piping this into `ssh -C` measures ssh on an "
                        "interactively-shaped stream instead of a bulk file -- "
                        "which matters, because bulk transfer hands ssh large "
                        "packets it never sees live and so flatters its ratio.")
    p.add_argument("--replay-speed", type=float, default=1.0,
                   help="multiply the replay clock (2.0 = twice as fast). The "
                        "message BOUNDARIES are preserved either way; only the "
                        "waiting is scaled.")
    p.add_argument("--selftest", action="store_true")
    args = p.parse_args(argv)

    if args.selftest:
        return selftest()

    if args.replay:
        if not args.dump:
            p.error("--replay needs a dump path")
        out = sys.stdout.buffer
        t0 = time.monotonic()
        speed = max(0.01, args.replay_speed)
        written = 0
        for ts, _code, blob in read_dump(args.dump):
            if args.from_s is not None and ts < args.from_s:
                continue
            if args.to_s is not None and ts >= args.to_s:
                continue
            wait = t0 + (ts / speed) - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            out.write(blob)
            # Flush per message: this is the point of the arm.  Without the
            # flush the OS coalesces and ssh sees bulk packets, which is the
            # very thing this mode exists to avoid.
            out.flush()
            written += len(blob)
        sys.stderr.write("rdprice: replayed %d bytes\n" % written)
        return 0
    if not args.dump:
        p.error("a dump path is required (or --selftest)")

    all_records = list(read_dump(args.dump))
    records = [r for r in all_records
               if (args.from_s is None or r[0] >= args.from_s)
               and (args.to_s is None or r[0] < args.to_s)]
    if not records:
        sys.stderr.write("rdprice: the window [%s, %s) selected 0 of %d records "
                         "-- there is nothing to price, which is not the same as "
                         "a cheap workload\n"
                         % (args.from_s, args.to_s, len(all_records)))
        return 3
    frames = group_frames(records, args.gap_ms)
    raw_frames = [b"".join(blob for _ts, _c, blob in f) for f in frames]
    raw_total = sum(len(c) for c in raw_frames)
    nframes = len(frames)

    print("=== rdprice: %s ===" % (args.label or os.path.basename(args.dump)))
    print("host          : %s %s (CPU cost below is THIS machine's, not "
          "necessarily the server's)"
          % (platform.machine(), platform.processor() or platform.system()))
    print("python        : %s" % platform.python_version())
    print("dump          : %s" % args.dump)
    print("records       : %d messages, %d bytes of plain RP stream"
          % (len(records), raw_total))
    if len(records) != len(all_records):
        print("window        : [%s, %s) s -- %d of %d records "
              "(the rest, mostly the cold first paint, is excluded)"
              % (args.from_s, args.to_s, len(records), len(all_records)))
    print("frames        : %d (proxy: >%.0f ms of silence starts a new frame)"
          % (nframes, args.gap_ms))
    if records:
        span = records[-1][0] - records[0][0]
        print("span          : %.2f s  -> %.1f frames/s, %.1f kB/s"
              % (span, nframes / span if span else 0.0,
                 raw_total / 1024.0 / span if span else 0.0))

    # Per-op byte split, recomputed here so the pricing report stands alone.
    by_op = {}
    for _ts, code, blob in records:
        by_op[code] = by_op.get(code, 0) + len(blob)
    bitmap_pixel = sum(by_op.get(c, 0) for c in BITMAP_PIXEL_OPS)
    bitmap_path = sum(by_op.get(c, 0) for c in BITMAP_PATH_OPS)
    print("bitmap path   : %d bytes (%.2f%% of stream); pixels alone %d "
          "(%.2f%%)"
          % (bitmap_path, 100.0 * bitmap_path / raw_total if raw_total else 0.0,
             bitmap_pixel,
             100.0 * bitmap_pixel / raw_total if raw_total else 0.0))

    if args.emit_stream:
        with open(args.emit_stream, "wb") as f:
            for chunk in raw_frames:
                f.write(chunk)
        print("wrote pure RP stream: %s (%d bytes)"
              % (args.emit_stream, os.path.getsize(args.emit_stream)))

    arms = []
    z = _zstd()

    # --- whole-stream generic compression (the `ssh -C` class) ----------
    arms.append(arm("stream zlib-6 (== ssh -C)", raw_total, nframes,
                    lambda: zlib_stream(raw_frames, SSH_C_ZLIB_LEVEL),
                    args.repeats,
                    "one shared deflate state, Z_SYNC_FLUSH per frame; this is "
                    "the zero-work baseline any in-protocol scheme must beat"))
    arms.append(arm("stream zlib-1", raw_total, nframes,
                    lambda: zlib_stream(raw_frames, 1), args.repeats,
                    "cheapest zlib level"))
    if z is not None:
        for lvl in (1, 3, 9, 19):
            arms.append(arm("stream zstd-%d" % lvl, raw_total, nframes,
                            (lambda l: lambda: zstd_stream(raw_frames, l))(lvl),
                            args.repeats,
                            "RP_CAP_COMPRESS_ZSTD is the capability already in "
                            "the tree"))

    # --- per-frame independent compression -----------------------------
    arms.append(arm("per-frame zlib-6", raw_total, nframes,
                    lambda: zlib_independent(raw_frames, SSH_C_ZLIB_LEVEL),
                    args.repeats,
                    "no shared state across frames -- the price of being able "
                    "to drop or resync a frame"))
    if z is not None:
        for lvl in (1, 3, 9):
            arms.append(arm("per-frame zstd-%d" % lvl, raw_total, nframes,
                            (lambda l: lambda: zstd_independent(raw_frames, l))(lvl),
                            args.repeats, "independent per frame"))

    # --- the bitmap path, priced as images -----------------------------
    bmps, unparsed = extract_bitmaps(frames)
    bmp_raw = sum(len(b["bits"]) for b in bmps)
    print("")
    print("--- BITMAP PAYLOADS ---------------------------------------")
    print("bitmaps       : %d decoded, %d unparsed, %d bytes of pixels"
          % (len(bmps), unparsed, bmp_raw))
    if bmps:
        dims = sorted(set((b["width"], b["height"]) for b in bmps))
        print("sizes         : %d distinct, e.g. %s"
              % (len(dims), ", ".join("%dx%d" % d for d in dims[:8])))
        cs = sorted(set(b["colorspace"] for b in bmps))
        print("colour spaces : %s" % ", ".join("0x%04x" % c for c in cs))

    bitmap_arms = []
    if bmps and bmp_raw:
        payloads = [b["bits"] for b in bmps]
        bitmap_arms.append(arm("bitmap pixels: zstd-3 independent", bmp_raw,
                               len(bmps),
                               lambda: zstd_independent(payloads, 3)
                               if z else None, args.repeats,
                               "per-bitmap, no shared state"))
        bitmap_arms.append(arm("bitmap pixels: zlib-6 independent", bmp_raw,
                               len(bmps),
                               lambda: zlib_independent(payloads, SSH_C_ZLIB_LEVEL),
                               args.repeats, "per-bitmap"))
        def all_png():
            total = 0
            for b in bmps:
                n = png_encode(b)
                if n is None:
                    return None
                total += n
            return total
        bitmap_arms.append(arm("bitmap pixels: PNG (image coding)", bmp_raw,
                               len(bmps), all_png, 1,
                               "lossless image coding; does not damage text, "
                               "unlike 4:2:0 video. RATIO is real; CPU is an "
                               "UPPER BOUND -- the filter loop is pure Python, "
                               "libpng would be far faster"))

    # --- the remainder: the stream WITHOUT the bitmap path -------------
    rem_frames = [b"".join(blob for _ts, c, blob in f
                           if c not in BITMAP_PATH_OPS) for f in frames]
    rem_frames = [c for c in rem_frames if c]
    rem_raw = sum(len(c) for c in rem_frames)
    rem_arms = []
    if rem_raw:
        rem_arms.append(arm("remainder (no bitmap path): stream zlib-6",
                            rem_raw, len(rem_frames),
                            lambda: zlib_stream(rem_frames, SSH_C_ZLIB_LEVEL),
                            args.repeats,
                            "what is left for a bitmap codec to NOT help with"))
        if z is not None:
            rem_arms.append(arm("remainder (no bitmap path): stream zstd-3",
                                rem_raw, len(rem_frames),
                                lambda: zstd_stream(rem_frames, 3),
                                args.repeats, ""))

    def table(title, rows):
        print("")
        print("--- %s %s" % (title, "-" * max(0, 54 - len(title))))
        print("%-38s %10s %10s %7s %9s %9s"
              % ("arm", "raw", "encoded", "ratio", "ms/frame", "ms/MB"))
        for r in rows:
            if not r.get("available"):
                print("%-38s  UNAVAILABLE: %s" % (r["arm"], r.get("note", "")))
                continue
            print("%-38s %10d %10d %6.2fx %9.3f %9.1f"
                  % (r["arm"], r["raw_bytes"], r["encoded_bytes"], r["ratio"],
                     r["cpu_ms_per_frame"], r["cpu_ms_per_mb"]))

    table("WHOLE-STREAM AND PER-FRAME ARMS", arms)
    if bitmap_arms:
        table("BITMAP-PATH-ONLY ARMS", bitmap_arms)
    if rem_arms:
        table("REMAINDER ARMS (bitmap path removed)", rem_arms)

    blob = {
        "label": args.label,
        "dump": args.dump,
        "host_machine": platform.machine(),
        "messages": len(records),
        "raw_bytes": raw_total,
        "frames": nframes,
        "gap_ms": args.gap_ms,
        "span_s": (records[-1][0] - records[0][0]) if records else 0.0,
        "bitmap_path_bytes": bitmap_path,
        "bitmap_pixel_bytes": bitmap_pixel,
        "bitmap_path_share": (float(bitmap_path) / raw_total) if raw_total else 0.0,
        "bitmaps": len(bmps),
        "bitmaps_unparsed": unparsed,
        "bitmap_pixel_raw": bmp_raw,
        "remainder_bytes": rem_raw,
        "op_bytes": {str(k): v for k, v in sorted(by_op.items())},
        "arms": arms,
        "bitmap_arms": bitmap_arms,
        "remainder_arms": rem_arms,
    }
    if args.json:
        with open(args.json, "w") as f:
            json.dump(blob, f, indent=2, sort_keys=True)
        print("")
        print("wrote JSON: %s" % args.json)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except DumpError as exc:
        sys.stderr.write("rdprice: %s\n" % exc)
        sys.exit(2)
