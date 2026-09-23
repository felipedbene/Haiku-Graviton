#!/usr/bin/env python3
"""rdlatency.py -- measure DeBeOS remote-desktop interactive latency on the RP
wire, with the input timestamp and the response timestamp taken by the SAME
process off the SAME clock.

What it measures, precisely
---------------------------
For each trial:

  t0  = time.perf_counter() immediately BEFORE sendall() of the input frame(s)
  t1b = time.perf_counter() immediately AFTER the recv() that delivered the
        first response byte
  t1m = same, for the recv() that COMPLETED the first response message
  t1c = ... that completed the first cursor op (RP_MOVE_CURSOR_TO)
  t1d = ... that completed the first DRAWING op (fills/strokes/bitmaps/text)

So the reported figure is an **input-write -> response-arrival round trip on
the RP wire**, measured at the client. It is NOT glass-to-glass: it excludes
(a) real input capture in a GUI client and (b) client-side rasterise+present.
On loopback it also excludes any network.

Attribution ladder (each row adds one server stage over the row above):
  * echo   : RP_INIT_CONNECTION -> RP_INIT_CONNECTION reply. Receiver thread
             reads a message, does trivial work, flushes a reply. This is the
             wire + event-thread + send-ring floor INSIDE app_server.
  * cursor : RP_MOUSE_MOVED -> RP_MOVE_CURSOR_TO. Adds event-stream insert,
             Desktop event dispatch and cursor handling.
  * paint  : input -> first drawing op. Adds window/app dispatch and AGG
             rendering of the damaged area.

Floors measured in the same process/clock:
  * tcp    : 6-byte ping-pong over a 127.0.0.1 socket pair (thread echo).
             Bounds "syscall + loopback stack + wakeup" on this OS.

Attribution safety: every trial is preceded by a quiescence gate (no wire
traffic for --quiet ms), and a control arm measures how long the quiesced
desktop stays silent with NO input sent. If the control shows spontaneous
traffic on the same timescale as the trials, attribution is unsafe and the
numbers are not causal -- so the control is reported, not assumed.

A response is attributed to our input only when it paints at a site the idle
desktop was never seen to paint, and that site includes the ACTIVE CLIPPING
REGION, not just the op's own rectangle. app_server constrains the clip to the
current dirty region before every view draw, so the rectangle a view asks for
and the pixels that change are different things: a caret blink and a real
scroll of the same text view emit the same full-width FILL_RECT under entirely
different clips. Keying on the rectangle alone silently conflates them, which
is why the first run of this tool could not report a scroll figure at all
(issue #527). `--selftest` asserts the discrimination offline, and asserts that
the clip-blind key does NOT discriminate, so the regression cannot come back
quietly.
"""

import argparse
import errno
import json
import select
import socket
import struct
import sys
import threading
import time

HEADER = 6
MAX_MESSAGE = 64 * 1024 * 1024

RP_INIT_CONNECTION = 1
RP_UPDATE_DISPLAY_MODE = 2
RP_CLOSE_CONNECTION = 3
RP_GET_SYSTEM_PALETTE = 4
RP_GET_SYSTEM_PALETTE_RESULT = 5
RP_HELLO = 6
RP_HELLO_ACK = 7
RP_SESSION_COOKIE = 12
RP_COOKIE_METHOD_PER_BOOT = 1

RP_CREATE_STATE = 20
RP_DELETE_STATE = 21
RP_ENABLE_SYNC_DRAWING = 22
RP_DISABLE_SYNC_DRAWING = 23
RP_INVALIDATE_RECT = 24
RP_INVALIDATE_REGION = 25

RP_SET_OFFSETS = 40
RP_SET_FONT = 48

RP_CONSTRAIN_CLIPPING_REGION = 60
RP_COPY_RECT_NO_CLIPPING = 61
RP_INVERT_RECT = 62
RP_DRAW_BITMAP = 63
RP_DRAW_BITMAP_RECTS = 64

RP_STROKE_RECT = 84
RP_FILL_RECT = 104
RP_STROKE_RECT_1PX_COLOR = 142
RP_FILL_RECT_COLOR = 160

RP_DRAW_STRING = 180
RP_DRAW_STRING_WITH_OFFSETS = 181
RP_DRAW_STRING_RESULT = 182
RP_STRING_WIDTH = 183
RP_STRING_WIDTH_RESULT = 184
RP_READ_BITMAP = 185
RP_READ_BITMAP_RESULT = 186

RP_SET_CURSOR = 200
RP_SET_CURSOR_VISIBLE = 201
RP_MOVE_CURSOR_TO = 202

RP_MOUSE_MOVED = 220
RP_MOUSE_DOWN = 221
RP_MOUSE_UP = 222
RP_MOUSE_WHEEL_CHANGED = 223

RP_KEY_DOWN = 240
RP_KEY_UP = 241
RP_MODIFIERS_CHANGED = 244

B_RGB32 = 0x0008

# Codes whose payload does NOT begin with a uint32 token (RemoteMessage users
# that are not a drawing engine). Mirrors rdcapture.py's NO_TOKEN set for the
# server->client codes this tool actually needs to parse.
NO_TOKEN = frozenset([
    RP_INIT_CONNECTION, RP_UPDATE_DISPLAY_MODE, RP_CLOSE_CONNECTION,
    RP_GET_SYSTEM_PALETTE, RP_GET_SYSTEM_PALETTE_RESULT, RP_HELLO, RP_HELLO_ACK,
    RP_SET_CURSOR, RP_SET_CURSOR_VISIBLE, RP_MOVE_CURSOR_TO,
    RP_CREATE_STATE, RP_DELETE_STATE,
])

# "A pixel changed in a window" ops. Deliberately excludes the cursor ops (the
# cursor is composited by the client, not by app_server's renderer) and
# excludes pure state ops, which change nothing on screen by themselves.
DRAWING_OPS = frozenset(
    [RP_COPY_RECT_NO_CLIPPING, RP_INVERT_RECT, RP_DRAW_BITMAP,
     RP_DRAW_BITMAP_RECTS, RP_DRAW_STRING, RP_DRAW_STRING_WITH_OFFSETS]
    + list(range(80, 90))      # RP_STROKE_*
    + list(range(100, 109))    # RP_FILL_*
    + list(range(120, 129))    # RP_FILL_*_GRADIENT
    + list(range(140, 143))    # RP_STROKE_*_COLOR
    + list(range(160, 162))    # RP_FILL_RECT_COLOR, RP_FILL_REGION_COLOR
    + list(range(260, 269))    # RP_STROKE_*_GRADIENT
)

CURSOR_OPS = frozenset([RP_MOVE_CURSOR_TO, RP_SET_CURSOR,
                        RP_SET_CURSOR_VISIBLE])

_NAMES = {v: k for k, v in list(globals().items())
          if k.startswith("RP_") and isinstance(v, int)}


def code_name(code):
    return _NAMES.get(code, "RP_UNKNOWN_%d" % code)


def now():
    return time.perf_counter()


def frame(code, payload=b""):
    return struct.pack("<HI", code, HEADER + len(payload)) + payload


# ---------------------------------------------------------------------------
# input frames -- byte layouts verified against src/tools/html5_remote_desktop
# /HaikuRemoteDesktop.js (onMouseMove/Down/Up/onWheel/onKeyDownUp) and the
# server's RemoteEventStream::EventReceived.
# ---------------------------------------------------------------------------
def f_move(x, y):
    return frame(RP_MOUSE_MOVED, struct.pack("<ff", x, y))


def f_down(x, y, buttons=1, clicks=1):
    return frame(RP_MOUSE_DOWN, struct.pack("<ffII", x, y, buttons, clicks))


def f_up(x, y, buttons=0):
    return frame(RP_MOUSE_UP, struct.pack("<ffI", x, y, buttons))


def f_wheel(dx, dy):
    return frame(RP_MOUSE_WHEEL_CHANGED, struct.pack("<ff", dx, dy))


def f_key(down, ch, raw=0, keycode=0):
    b = ch.encode() if isinstance(ch, str) else ch
    return frame(RP_KEY_DOWN if down else RP_KEY_UP,
                 struct.pack("<I", len(b)) + b
                 + struct.pack("<II", raw, keycode))


class Reader(object):
    def __init__(self, buf):
        self.b = buf
        self.o = 0

    def left(self):
        return len(self.b) - self.o

    def take(self, n):
        if self.left() < n:
            raise ValueError("truncated")
        v = self.b[self.o:self.o + n]
        self.o += n
        return v

    def u8(self):
        return struct.unpack("<B", self.take(1))[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def f32(self):
        return struct.unpack("<f", self.take(4))[0]

    def point(self):
        return (self.f32(), self.f32())

    def rect(self):
        return (self.f32(), self.f32(), self.f32(), self.f32())

    def string(self):
        n = self.u32()
        return bytes(self.take(n))

    def font(self):
        # RemoteMessage.cpp AddFont, 29 bytes packed.
        self.u8(); self.u8(); self.u32(); self.u8()
        self.f32(); self.f32(); self.f32()
        size = self.f32()
        self.u16(); self.u32()
        return size


def utf8_chars(data):
    return sum(1 for b in data if (b & 0xC0) != 0x80)


class Session(object):
    """The RP connection, plus the bookkeeping needed to keep the server from
    blocking on a synchronous query (which would contaminate every later
    trial)."""

    def __init__(self, host, port, cookie, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.setblocking(False)
        self.buf = bytearray()
        self.font_size = {}
        self.replies = 0
        self.string_width_queries = 0
        self.read_bitmap_queries = 0
        self.bytes_in = 0
        self.chunks = 0
        if cookie:
            cb = cookie.encode() if isinstance(cookie, str) else cookie
            payload = struct.pack("<II", RP_COOKIE_METHOD_PER_BOOT,
                                  len(cb)) + cb
            self.send(frame(RP_SESSION_COOKIE, payload))

    def send(self, data):
        self.sock.setblocking(True)
        try:
            self.sock.sendall(data)
        finally:
            self.sock.setblocking(False)

    def pump(self, timeout):
        """One select+recv. Returns (t_recv, [(code, payload), ...]) or
        (None, []) on timeout. t_recv is the instant the bytes were handed to
        us by the kernel; every message completed by this chunk shares it."""
        try:
            r, _, _ = select.select([self.sock], [], [], max(0.0, timeout))
        except (OSError, select.error) as exc:
            if getattr(exc, "errno", None) == errno.EINTR:
                return None, []
            raise
        if not r:
            return None, []
        t = now()
        try:
            chunk = self.sock.recv(262144)
        except socket.error as exc:
            if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK, errno.EINTR):
                return None, []
            raise
        if not chunk:
            raise EOFError("peer closed")
        self.bytes_in += len(chunk)
        self.chunks += 1
        self.buf += chunk
        out = []
        while len(self.buf) >= HEADER:
            code, length = struct.unpack_from("<HI", self.buf, 0)
            if length < HEADER or length > MAX_MESSAGE:
                raise ValueError("implausible length %d for %s"
                                 % (length, code_name(code)))
            if len(self.buf) < length:
                break
            out.append((code, bytes(self.buf[HEADER:length])))
            del self.buf[:length]
        for code, payload in out:
            self._maybe_reply(code, payload)
        return t, out

    # -- keep the server unblocked -------------------------------------
    def _maybe_reply(self, code, payload):
        if code not in (RP_SET_FONT, RP_DRAW_STRING, RP_DRAW_STRING_WITH_OFFSETS,
                        RP_STRING_WIDTH, RP_READ_BITMAP):
            return
        try:
            r = Reader(payload)
            token = r.u32()
            if code == RP_SET_FONT:
                self.font_size[token] = r.font()
                return
            size = self.font_size.get(token, 12.0) or 12.0
            if code == RP_DRAW_STRING:
                point = r.point()
                text = r.string()
                adv = 0.55 * size * utf8_chars(text)
                self.send(frame(RP_DRAW_STRING_RESULT,
                                struct.pack("<Iff", token, point[0] + adv,
                                            point[1])))
                self.replies += 1
                return
            if code == RP_DRAW_STRING_WITH_OFFSETS:
                text = r.string()
                n = utf8_chars(text)
                last = (0.0, 0.0)
                for _ in range(n):
                    try:
                        last = r.point()
                    except ValueError:
                        break
                self.send(frame(RP_DRAW_STRING_RESULT,
                                struct.pack("<Iff", token,
                                            last[0] + 0.55 * size, last[1])))
                self.replies += 1
                return
            if code == RP_STRING_WIDTH:
                text = r.string()
                self.string_width_queries += 1
                self.send(frame(RP_STRING_WIDTH_RESULT,
                                struct.pack("<If", token,
                                            0.55 * size * utf8_chars(text))))
                self.replies += 1
                return
            if code == RP_READ_BITMAP:
                x0, y0, x1, y1 = r.rect()
                w = max(1, int(x1) - int(x0) + 1)
                h = max(1, int(y1) - int(y0) + 1)
                bpr = w * 4
                bits = bytes(bpr * h)
                self.read_bitmap_queries += 1
                self.send(frame(RP_READ_BITMAP_RESULT,
                                struct.pack("<IiiiIII", token, w, h, bpr,
                                            B_RGB32, 0, len(bits)) + bits))
                self.replies += 1
                return
        except (ValueError, struct.error):
            return

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# measurement
# ---------------------------------------------------------------------------
class Trial(object):
    __slots__ = ("label", "t_byte", "t_msg", "t_cursor", "t_draw", "t_novel",
                 "first_code", "draw_code", "novel_code", "novel_key", "codes",
                 "bytes", "chunks")

    def __init__(self, label):
        self.label = label
        self.t_byte = None
        self.t_msg = None
        self.t_cursor = None
        self.t_draw = None
        self.t_novel = None
        self.first_code = None
        self.draw_code = None
        self.novel_code = None
        self.novel_key = None
        self.codes = []
        self.bytes = 0
        self.chunks = 0

    def as_dict(self):
        ms = lambda v: None if v is None else round(v * 1000.0, 3)
        return {"label": self.label, "first_byte_ms": ms(self.t_byte),
                "first_msg_ms": ms(self.t_msg),
                "cursor_ms": ms(self.t_cursor), "draw_ms": ms(self.t_draw),
                "novel_ms": ms(self.t_novel),
                "first_code": (None if self.first_code is None
                               else code_name(self.first_code)),
                "draw_code": (None if self.draw_code is None
                              else code_name(self.draw_code)),
                "novel_code": (None if self.novel_code is None
                               else code_name(self.novel_code)),
                "novel_key": (None if self.novel_key is None
                              else str(self.novel_key)),
                "ops": len(self.codes), "bytes": self.bytes,
                "chunks": self.chunks}


RECT_OPS = frozenset([RP_FILL_RECT_COLOR, RP_STROKE_RECT_1PX_COLOR,
                      RP_FILL_RECT, RP_STROKE_RECT, RP_INVERT_RECT,
                      RP_INVALIDATE_RECT])


def op_box(code, payload):
    """Bounding box of the ops whose leading field is a BRect, else None.
    Used only by --map, to find where the windows are from the wire."""
    try:
        r = Reader(payload)
        if code not in NO_TOKEN:
            r.u32()
        if code in RECT_OPS:
            return r.rect()
        return None
    except (ValueError, struct.error):
        return None


def clip_token(payload):
    """Token of an RP_CONSTRAIN_CLIPPING_REGION, so its region can be filed
    against the drawing state it constrains."""
    try:
        return Reader(payload).u32()
    except (ValueError, struct.error):
        return None


def clip_box(payload):
    """Bounding box of a clipping region. None means "no constraint" -- an
    empty region (n <= 0) is app_server saying nothing is clipped away."""
    try:
        r = Reader(payload)
        r.u32()
        n = struct.unpack("<i", r.take(4))[0]
        if n <= 0 or n > 4096:
            return None
        rects = [r.rect() for _ in range(n)]
        return (min(v[0] for v in rects), min(v[1] for v in rects),
                max(v[2] for v in rects), max(v[3] for v in rects))
    except (ValueError, struct.error):
        return None


def geom_key(code, payload):
    """(token, quantised geometry) for the ops whose damaged area we can decode
    cheaply; (token, None) otherwise.

    This is the attribution key. The desktop paints on its own (a blinking
    caret, the Deskbar clock), so "a drawing op arrived" is not by itself
    evidence that OUR input caused it. What is evidence is a drawing op at a
    place the idle desktop was never seen to paint."""
    try:
        r = Reader(payload)
        tok = 0 if code in NO_TOKEN else r.u32()
        if code in RECT_OPS:
            return tok, tuple(int(v) for v in r.rect())
        if code == RP_COPY_RECT_NO_CLIPPING:
            xo = struct.unpack("<i", r.take(4))[0]
            yo = struct.unpack("<i", r.take(4))[0]
            return tok, (xo, yo) + tuple(int(v) for v in r.rect())
        if code == RP_DRAW_STRING:
            return tok, tuple(int(v) for v in r.point())
        if code == RP_DRAW_STRING_WITH_OFFSETS:
            r.string()
            return tok, tuple(int(v) for v in r.point())
        return tok, None
    except (ValueError, struct.error):
        return None, None


class Attribution(object):
    """The background key set AND the clipping state needed to compute a key.

    Why the clip is part of the key: app_server constrains the clipping region
    to the current dirty region before EVERY view drawing op
    (ServerWindow.cpp:2533), so the op's own rectangle is the shape the view
    asked to paint, not the shape that actually changed on screen. A blinking
    caret invalidates a 1-px column and BTextView then issues a full-width
    FILL_RECT of the whole text line -- clipped to that column. A real scroll of
    the same text view issues a FILL_RECT with the SAME box and a completely
    different clip.

    Keying on the op box alone therefore makes a caret blink and a scroll
    indistinguishable, which is exactly what made the scroll latency arm
    unmeasurable in the first run of this tool (issue #527): the discriminating
    information was on the wire and the key threw it away. Feed EVERY message
    through observe() in arrival order so the clip state stays in step with the
    stream -- a key computed from a stale clip is worse than no key.
    """

    def __init__(self):
        self.keys = set()
        self.tokens = set()
        self.clip = {}          # token -> clip bbox, or None for "unconstrained"

    def observe(self, code, payload):
        """Update clipping state; return this op's attribution key, or None if
        the op paints nothing."""
        if code == RP_CONSTRAIN_CLIPPING_REGION:
            tok = clip_token(payload)
            if tok is not None:
                self.clip[tok] = clip_box(payload)
            return None
        if code not in DRAWING_OPS:
            return None
        tok, g = geom_key(code, payload)
        return (code, tok, g, self.clip.get(tok))

    def learn(self, key):
        self.keys.add(key)
        self.tokens.add(key[1])

    def is_novel(self, key):
        return key not in self.keys

    def blind_key(self, key):
        """The pre-#527 key, kept only so the self-test can demonstrate that the
        clip-blind key really does fail to discriminate."""
        return key[:3]


def calibrate(sess, seconds):
    """Watch the idle desktop and learn what it paints by itself."""
    bg = Attribution()
    codes = {}
    events = []
    last = now()
    end = now() + seconds
    while now() < end:
        t, msgs = sess.pump(min(0.2, max(0.0, end - now())))
        if t is None:
            continue
        events.append((t - last) * 1000.0)
        last = t
        for code, payload in msgs:
            codes[code] = codes.get(code, 0) + 1
            key = bg.observe(code, payload)
            if key is not None:
                bg.learn(key)
    keys, tokens = bg.keys, bg.tokens
    gaps = sorted(events)[1:] if len(events) > 1 else events
    print("=== idle desktop (calibration, %.0f s) ===" % seconds)
    print("  chunks=%d  inter-arrival ms: min=%s p50=%s p95=%s max=%s"
          % (len(events),
             None if not gaps else round(min(gaps), 1),
             None if not gaps else round(pct(gaps, 50), 1),
             None if not gaps else round(pct(gaps, 95), 1),
             None if not gaps else round(max(gaps), 1)))
    top = sorted(codes.items(), key=lambda kv: -kv[1])[:8]
    print("  ops: %s" % ", ".join("%s=%d" % (code_name(c), n) for c, n in top))
    print("  self-painting sites: %d  tokens: %s"
          % (len(keys), sorted(t for t in tokens if t is not None)))
    # How much of the discrimination comes from the clip: if the clip-blind key
    # collapses several distinct sites into one, those are precisely the sites
    # the pre-#527 key could not tell apart from an input response.
    blind = set(bg.blind_key(k) for k in keys)
    if len(blind) != len(keys):
        print("  clip-aware sites %d vs clip-blind %d -- %d site(s) are only "
              "distinguishable by their clipping region"
              % (len(keys), len(blind), len(keys) - len(blind)))
    return bg, gaps


def _p_clip(token, rects):
    """RP_CONSTRAIN_CLIPPING_REGION payload: token, i32 count, count rects."""
    out = struct.pack("<I", token) + struct.pack("<i", len(rects))
    for r in rects:
        out += struct.pack("<ffff", *r)
    return out


def _p_rect(token, rect):
    """An RP_FILL_RECT_COLOR payload, truncated after the rect -- geom_key only
    ever reads the leading token and rect, so the colour is not needed."""
    return struct.pack("<I", token) + struct.pack("<ffff", *rect)


def selftest():
    """Offline checks on the attribution key. No instance, no network.

    The point of interest is check 2: it asserts BOTH that the clip-aware key
    discriminates AND that the clip-blind key does NOT. That second half is the
    mutation test -- if someone drops the clip from the key again, check 2 fails
    instead of quietly passing, which is how #527 stayed invisible.
    """
    fails = []
    ran = []

    def check(name, cond, detail=""):
        ran.append(name)
        print("  %-52s %s%s" % (name, "PASS" if cond else "FAIL",
                                "" if cond or not detail else "  " + detail))
        if not cond:
            fails.append(name)

    TOK = 164
    LINE = (8.0, 144.0, 488.0, 162.0)       # a text line, as seen on the wire
    CARET_CLIP = [(33.0, 144.0, 34.0, 161.0)]   # 1-px caret column
    SCROLL_CLIP = [(8.0, 144.0, 488.0, 162.0)]  # the whole line, really scrolled

    print("=== rdlatency self-test ===")

    # 1. The clip decodes, and an empty region reads as "unconstrained".
    check("clip_box decodes a single rect",
          clip_box(_p_clip(TOK, CARET_CLIP)) == (33, 144, 34, 161))
    check("clip_box unions a multi-rect region",
          clip_box(_p_clip(TOK, [(0.0, 0.0, 10.0, 10.0),
                                 (90.0, 90.0, 100.0, 100.0)]))
          == (0, 0, 100, 100))
    check("empty region reads as unconstrained (None)",
          clip_box(_p_clip(TOK, [])) is None)
    check("clip_token reads the token", clip_token(_p_clip(TOK, CARET_CLIP)) == TOK)

    # 2. THE point of the fix, plus its own mutation test.
    a = Attribution()
    a.observe(RP_CONSTRAIN_CLIPPING_REGION, _p_clip(TOK, CARET_CLIP))
    caret = a.observe(RP_FILL_RECT_COLOR, _p_rect(TOK, LINE))
    a.observe(RP_CONSTRAIN_CLIPPING_REGION, _p_clip(TOK, SCROLL_CLIP))
    scroll = a.observe(RP_FILL_RECT_COLOR, _p_rect(TOK, LINE))
    check("caret blink and scroll have DIFFERENT clip-aware keys",
          caret != scroll, "%s vs %s" % (caret, scroll))
    check("...and IDENTICAL clip-blind keys (mutation control)",
          a.blind_key(caret) == a.blind_key(scroll),
          "clip-blind key would not discriminate: %s" % (a.blind_key(caret),))

    # 3. Novelty: what calibration saw is not novel; the other clip is.
    b = Attribution()
    b.observe(RP_CONSTRAIN_CLIPPING_REGION, _p_clip(TOK, CARET_CLIP))
    b.learn(b.observe(RP_FILL_RECT_COLOR, _p_rect(TOK, LINE)))
    b.observe(RP_CONSTRAIN_CLIPPING_REGION, _p_clip(TOK, CARET_CLIP))
    check("a repeat of a calibrated site is NOT novel",
          not b.is_novel(b.observe(RP_FILL_RECT_COLOR, _p_rect(TOK, LINE))))
    b.observe(RP_CONSTRAIN_CLIPPING_REGION, _p_clip(TOK, SCROLL_CLIP))
    check("same box under a different clip IS novel",
          b.is_novel(b.observe(RP_FILL_RECT_COLOR, _p_rect(TOK, LINE))))

    # 4. State hygiene: the clip is per token, and a state op is not a paint.
    c = Attribution()
    c.observe(RP_CONSTRAIN_CLIPPING_REGION, _p_clip(TOK, CARET_CLIP))
    check("clip does not leak across tokens",
          c.observe(RP_FILL_RECT_COLOR, _p_rect(TOK + 1, LINE))[3] is None)
    check("a clipping op is not itself a paint",
          c.observe(RP_CONSTRAIN_CLIPPING_REGION,
                    _p_clip(TOK, CARET_CLIP)) is None)
    check("a non-drawing op yields no key",
          c.observe(RP_SET_OFFSETS, struct.pack("<I", TOK)) is None)
    check("a truncated payload does not raise",
          c.observe(RP_FILL_RECT_COLOR, b"\x01\x02") is not None)

    # Counted, not hardcoded: a hardcoded total is a check that stops checking
    # the moment someone adds or removes one above it.
    print("%d check(s), %d failure(s)" % (len(ran), len(fails)))
    if fails:
        print("FAILED: %s" % ", ".join(fails))
    return 1 if fails else 0


def run_timeline(sess, seconds):
    t0 = now()
    end = t0 + seconds
    while now() < end:
        t, msgs = sess.pump(min(0.2, max(0.0, end - now())))
        if t is None:
            continue
        parts = []
        for code, payload in msgs:
            tok, g = geom_key(code, payload)
            parts.append("%s/%s%s" % (code_name(code).replace("RP_", ""), tok,
                                      "" if g is None else str(g)))
        print("%8.1f ms  %s" % ((t - t0) * 1000.0, " ".join(parts[:10])))


def run_map(sess, seconds):
    """Report where things are on screen, from the drawing traffic alone."""
    boxes = {}
    clips = {}
    end = now() + seconds
    while now() < end:
        t, msgs = sess.pump(min(0.3, max(0.0, end - now())))
        for code, payload in msgs:
            if code == RP_CONSTRAIN_CLIPPING_REGION:
                b = clip_box(payload)
                if b:
                    tok = struct.unpack_from("<I", payload, 0)[0]
                    clips.setdefault(tok, {})
                    key = tuple(int(v) for v in b)
                    clips[tok][key] = clips[tok].get(key, 0) + 1
                continue
            b = op_box(code, payload)
            if b:
                tok = (0 if code in NO_TOKEN
                       else struct.unpack_from("<I", payload, 0)[0])
                boxes.setdefault(tok, []).append(b)
    print("=== map: clipping regions per token (where windows live) ===")
    for tok in sorted(clips):
        top = sorted(clips[tok].items(), key=lambda kv: -kv[1])[:4]
        print("  token %-6d %s" % (tok, "  ".join("%s x%d" % (k, n)
                                                  for k, n in top)))
    print("=== map: rect-op bounding boxes per token ===")
    for tok in sorted(boxes):
        bs = boxes[tok]
        print("  token %-6d n=%-5d bbox=(%d,%d,%d,%d)"
              % (tok, len(bs), min(b[0] for b in bs), min(b[1] for b in bs),
                 max(b[2] for b in bs), max(b[3] for b in bs)))


def quiesce(sess, quiet, budget):
    """Read until the wire has been silent for `quiet` seconds. Returns the
    number of messages drained and whether quiescence was reached."""
    drained = 0
    deadline = now() + budget
    last = now()
    while now() < deadline:
        t, msgs = sess.pump(min(quiet, deadline - now()))
        if t is None:
            if now() - last >= quiet:
                return drained, True
            continue
        drained += len(msgs)
        last = t
    return drained, (now() - last >= quiet)


def run_trial(sess, label, frames, collect, cap, bg=None):
    """Send `frames` (b"" for the null/control arm), then watch the wire.

    Stops `collect` seconds after the first NOVEL drawing op (or the first
    response byte if no novel op appears), and gives up after `cap` seconds."""
    tr = Trial(label)
    t0 = now()
    if frames:
        sess.send(frames)
    hard = t0 + cap
    stop = None
    while True:
        limit = hard if stop is None else min(hard, stop)
        if now() >= limit:
            break
        t, msgs = sess.pump(limit - now())
        if t is None:
            continue
        if tr.t_byte is None:
            tr.t_byte = t - t0
            tr.chunks = 1
            stop = t + collect
        else:
            tr.chunks += 1
        for code, payload in msgs:
            tr.codes.append(code)
            tr.bytes += HEADER + len(payload)
            if tr.t_msg is None:
                tr.t_msg = t - t0
                tr.first_code = code
            if tr.t_cursor is None and code in CURSOR_OPS:
                tr.t_cursor = t - t0
            # Every message, not just the drawing ones: the clip state lives in
            # RP_CONSTRAIN_CLIPPING_REGION and must advance with the stream.
            key = None if bg is None else bg.observe(code, payload)
            if code in DRAWING_OPS:
                if tr.t_draw is None:
                    tr.t_draw = t - t0
                    tr.draw_code = code
                if tr.t_novel is None and key is not None and bg.is_novel(key):
                    tr.t_novel = t - t0
                    tr.novel_code = code
                    tr.novel_key = key[1:]
                    stop = min(stop, t + collect) if stop else t + collect
    return tr


def tcp_floor(n):
    """6-byte ping-pong over 127.0.0.1 with TCP_NODELAY both ends, echoed by a
    thread in this process. Bounds send syscall + loopback stack + peer wakeup
    + recv syscall on this OS -- the irreducible part of every number above."""
    lis = socket.socket()
    lis.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lis.bind(("127.0.0.1", 0))
    lis.listen(1)
    port = lis.getsockname()[1]
    out = []
    err = []

    def echo():
        try:
            c, _ = lis.accept()
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            while True:
                d = c.recv(64)
                if not d:
                    break
                c.sendall(d)
            c.close()
        except Exception as exc:              # noqa: BLE001 - reported below
            err.append(str(exc))

    th = threading.Thread(target=echo)
    th.daemon = True
    th.start()
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(2.0)
    ping = b"\x01\x00\x06\x00\x00\x00"
    for _ in range(n + 5):                    # 5 warm-up round trips
        t0 = now()
        s.sendall(ping)
        got = b""
        while len(got) < 6:
            got += s.recv(6 - len(got))
        out.append((now() - t0) * 1000.0)
    s.close()
    lis.close()
    th.join(timeout=2)
    return out[5:], err


def pct(values, p):
    if not values:
        return None
    v = sorted(values)
    if len(v) == 1:
        return v[0]
    k = (len(v) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def summarise(name, values, scale=1000.0):
    """`values` are SECONDS unless scale says otherwise; printed in ms.

    The scale argument exists because the first version of this function was
    handed seconds and labelled them ms, which made every figure look 1000x
    better than it was. Keep the unit conversion in one place."""
    vals = [v * scale for v in values if v is not None]
    if not vals:
        return "%-22s n=0  (nothing)" % name
    return ("%-22s n=%-3d min=%8.3f p50=%8.3f p95=%8.3f max=%8.3f  (ms)"
            % (name, len(vals), min(vals), pct(vals, 50), pct(vals, 95),
               max(vals)))


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=10900)
    p.add_argument("--cookie-file",
                   help="required unless --selftest")
    p.add_argument("--selftest", action="store_true",
                   help="run offline checks on the attribution key and exit; needs no instance and no network")
    p.add_argument("--json", help="write every raw sample here")
    p.add_argument("--n", type=int, default=30, help="trials per scenario")
    p.add_argument("--calibrate", type=float, default=12.0,
                   help="seconds to watch the idle desktop first, to learn "
                        "what it paints without any input")
    p.add_argument("--quiet", type=float, default=0.025,
                   help="seconds of wire silence required before a trial")
    p.add_argument("--collect", type=float, default=0.12,
                   help="seconds to keep reading after the first response byte")
    p.add_argument("--cap", type=float, default=1.5,
                   help="give up on a trial after this long with no response")
    p.add_argument("--settle", type=float, default=6.0,
                   help="seconds to drain after connecting")
    p.add_argument("--width", type=int, default=1280)
    p.add_argument("--height", type=int, default=800)
    p.add_argument("--scenarios",
                   default="null,echo,cursor,hover,menu,key,scroll,sbar,drag")
    p.add_argument("--hover", default="",
                   help="x,y of a widget that repaints on hover "
                        "(default: Deskbar's clock area, top right)")
    p.add_argument("--hover-off-x", type=float, default=300.0)
    p.add_argument("--hover-off-y", type=float, default=460.0,
                   help="where to park the pointer between hover trials; must "
                        "be OUTSIDE the window under test or there is no "
                        "enter event to respond to")
    p.add_argument("--scroll-down", default="",
                   help="x,y of the vertical scrollbar's down arrow")
    p.add_argument("--scroll-up", default="",
                   help="x,y of the vertical scrollbar's up arrow")
    p.add_argument("--menu", default="",
                   help="x,y of a menu-bar item to open for the 'menu' "
                        "scenario")
    p.add_argument("--type", default="",
                   help="send this literal string as keystrokes first "
                        "(use \\n for newline)")
    p.add_argument("--probe-wheel-x", default="",
                   help="like --probe-wheel but a HORIZONTAL wheel delta")
    p.add_argument("--probe-wheel", default="",
                   help="like --probe but sends a wheel event at each point")
    p.add_argument("--probe", default="",
                   help="instead of measuring, click each 'x,y' in this "
                        "';'-separated list and report what repainted -- how "
                        "to find a title tab or a menu from the wire")
    p.add_argument("--window", default="",
                   help="x,y inside a window to scroll/drag (default centre)")
    p.add_argument("--drag-title", default="",
                   help="x,y on a window title tab to drag")
    p.add_argument("--timeline", type=float, default=0.0, metavar="SECONDS",
                   help="instead of measuring, print every arrival with its "
                        "timestamp, opcode, token and geometry")
    p.add_argument("--map", type=float, default=0.0, metavar="SECONDS",
                   help="instead of measuring, watch the wire this long and "
                        "report where the windows are")
    p.add_argument("--prime", type=int, default=0,
                   help="type this many characters into the focused window "
                        "before measuring, so scroll/keystroke have content")
    p.add_argument("--nudge", default="",
                   help="with --map: send a mouse move to x,y first")
    args = p.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.cookie_file:
        p.error("--cookie-file is required unless --selftest")

    with open(args.cookie_file) as f:
        cookie = f.read().strip()

    print("rdlatency: %s:%d  clock=perf_counter res=%.1f ns"
          % (args.host, args.port,
             time.get_clock_info("perf_counter").resolution * 1e9))

    floor, floor_err = tcp_floor(60)
    print(summarise("floor_tcp_loopback", floor, scale=1.0))
    if floor_err:
        print("floor errors: %s" % floor_err)

    sess = Session(args.host, args.port, cookie)
    sess.send(frame(RP_INIT_CONNECTION))

    if args.timeline:
        run_timeline(sess, args.timeline)
        sess.close()
        return 0

    if args.type:
        text = args.type.replace("\\n", "\n")
        for i, ch in enumerate(text):
            kc = 10 if ch == "\n" else ord(ch)
            sess.send(f_key(True, ch, 0, kc) + f_key(False, ch, 0, kc))
            if i % 12 == 11:
                quiesce(sess, 0.05, 1.5)
        print("typed %d characters" % len(text))
        quiesce(sess, 0.05, 3.0)

    if args.probe or args.probe_wheel:
        specs = ([(s, "click") for s in args.probe.split(";") if s]
                 + [(s, "wheel") for s in args.probe_wheel.split(";") if s]
                 + [(s, "wheelx") for s in args.probe_wheel_x.split(";") if s])
        for spec, mode in specs:
            x, y = [float(v) for v in spec.split(",")]
            sess.send(f_move(x, y))
            quiesce(sess, 0.05, 2.0)
            codes = {}
            boxes = []
            if mode == "wheel":
                sess.send(f_wheel(0.0, 3.0))
            elif mode == "wheelx":
                sess.send(f_wheel(3.0, 0.0))
            else:
                sess.send(f_down(x, y, 1, 1) + f_up(x, y, 0))
            end = now() + 0.6
            while now() < end:
                t, msgs = sess.pump(max(0.0, end - now()))
                for code, payload in msgs:
                    codes[code] = codes.get(code, 0) + 1
                    if code in DRAWING_OPS:
                        tok, g = geom_key(code, payload)
                        if g:
                            boxes.append((tok, g))
            paints = sum(n for c, n in codes.items() if c in DRAWING_OPS)
            print("probe %-6s %-10s paints=%-5d ops=%s" % (mode, spec, paints,
                  ", ".join("%s=%d" % (code_name(c), n) for c, n in
                            sorted(codes.items(), key=lambda kv: -kv[1])[:5])))
            origins = sorted(set(b[1] for b in boxes if len(b[1]) == 2))
            if origins:
                print("      text/point origins: %s" % (origins[:12],))
            if boxes:
                xs = [b[1] for b in boxes if len(b[1]) == 4]
                if xs:
                    print("      bbox=(%d,%d,%d,%d) tokens=%s"
                          % (min(v[0] for v in xs), min(v[1] for v in xs),
                             max(v[2] for v in xs), max(v[3] for v in xs),
                             sorted(set(b[0] for b in boxes))))
            # Undo anything a click may have opened.
            if mode == "click":
                sess.send(f_key(True, "\x1b", 0, 27)
                          + f_key(False, "\x1b", 0, 27))
            quiesce(sess, 0.05, 2.0)
        sess.close()
        return 0

    if args.map:
        if args.nudge:
            x, y = [float(v) for v in args.nudge.split(",")]
            sess.send(f_move(x, y))
        run_map(sess, args.map)
        print("bytes=%d chunks=%d string_width_queries=%d"
              % (sess.bytes_in, sess.chunks, sess.string_width_queries))
        sess.close()
        return 0

    drained, quiet_ok = quiesce(sess, args.quiet, args.settle)
    print("connect: drained %d messages, quiescent=%s, %d bytes"
          % (drained, quiet_ok, sess.bytes_in))
    if drained == 0:
        print("FATAL: server sent nothing -- wrong cookie or no desktop?")
        sess.close()
        return 3

    hover = ([float(v) for v in args.hover.split(",")] if args.hover
             else [args.width - 60.0, 10.0])
    win = ([float(v) for v in args.window.split(",")] if args.window
           else [args.width / 2.0, args.height / 2.0])
    title = ([float(v) for v in args.drag_title.split(",")] if args.drag_title
             else [win[0], win[1] - 100.0])
    menu = ([float(v) for v in args.menu.split(",")] if args.menu
            else [60.0, 36.0])
    sdown = ([float(v) for v in args.scroll_down.split(",")]
             if args.scroll_down else [500.0, 405.0])
    sup = ([float(v) for v in args.scroll_up.split(",")]
           if args.scroll_up else [500.0, 60.0])

    if args.prime:
        # Put text in the focused editor so "scroll" has something to scroll
        # and "keystroke" has a line to extend. Typed over the wire, because
        # that is the only input path this desktop has. Batched, with a drain
        # between batches: the client MUST keep reading or app_server's send
        # ring fills and the session wedges (the #-send-buffer failure).
        sent = 0
        batch = []
        for i in range(args.prime):
            ch = "\n" if i % 24 == 23 else "abcdefgh"[i % 8]
            kc = 10 if ch == "\n" else ord(ch)
            batch.append(f_key(True, ch, 0, kc))
            batch.append(f_key(False, ch, 0, kc))
            if len(batch) >= 24:
                sess.send(b"".join(batch))
                batch = []
                quiesce(sess, 0.05, 1.5)
            sent += 1
        if batch:
            sess.send(b"".join(batch))
        print("primed: %d keystrokes sent" % sent)
        quiesce(sess, args.quiet, 3.0)

    bg, idle_gaps = calibrate(sess, args.calibrate)

    results = {}
    raw = []

    def add(label, tr):
        results.setdefault(label, []).append(tr)
        raw.append(tr.as_dict())

    want = set(args.scenarios.split(","))

    for i in range(args.n):
        # A quiescence gate before every trial: the measurement is only causal
        # if nothing was already in flight. The desktop paints on its own, so
        # the gate can fail; the null arm below is what makes the numbers
        # interpretable when it does.
        if "null" in want:
            quiesce(sess, args.quiet, 3.0)
            add("z_null_no_input",
                run_trial(sess, "null", b"", args.collect, args.cap, bg))

        if "echo" in want:
            quiesce(sess, args.quiet, 3.0)
            add("a_echo_init_connection",
                run_trial(sess, "echo", frame(RP_INIT_CONNECTION),
                          args.collect, args.cap, bg))

        if "cursor" in want:
            # Mouse move over empty desktop: the cheapest real input response.
            x = 200.0 + (i % 13) * 7.0
            y = 430.0 + (i % 7) * 5.0
            quiesce(sess, args.quiet, 3.0)
            add("b_cursor_move_empty",
                run_trial(sess, "cursor", f_move(x, y), args.collect, args.cap,
                          bg))

        if "hover" in want:
            # Move off, then on, so each trial is a fresh enter event.
            sess.send(f_move(args.hover_off_x, args.hover_off_y))
            quiesce(sess, args.quiet, 3.0)
            add("c_hover_widget",
                run_trial(sess, "hover",
                          f_move(hover[0] + (i % 5), hover[1]),
                          args.collect, args.cap, bg))

        if "menu" in want:
            # The latency-sensitive case in its purest form: with a menu
            # already open, moving from one item to the next repaints two small
            # highlight bars and nothing else. Open the menu, settle, then time
            # ONE move between items.
            sess.send(f_down(menu[0], menu[1], 1, 1) + f_up(menu[0], menu[1], 0))
            quiesce(sess, 0.08, 3.0)
            sess.send(f_move(menu[0] + 10.0, menu[1] + 18.0))
            quiesce(sess, args.quiet, 3.0)
            add("g_menu_item_highlight",
                run_trial(sess, "menu",
                          f_move(menu[0] + 10.0, menu[1] + 36.0),
                          args.collect, args.cap, bg))
            sess.send(f_key(True, "\x1b", 0, 27) + f_key(False, "\x1b", 0, 27))
            quiesce(sess, args.quiet, 2.0)

        if "key" in want:
            quiesce(sess, args.quiet, 3.0)
            ch = "abcdefghijklmnopqrstuvwxyz"[i % 26]
            add("d_keystroke",
                run_trial(sess, "key", f_key(True, ch, 0, ord(ch)),
                          args.collect, args.cap, bg))
            sess.send(f_key(False, ch, 0, ord(ch)))

        if "scroll" in want:
            sess.send(f_move(win[0], win[1]))
            quiesce(sess, args.quiet, 3.0)
            add("e_scroll_wheel",
                run_trial(sess, "scroll",
                          f_wheel(0.0, 1.0 if i % 2 == 0 else -1.0),
                          args.collect, args.cap, bg))

        if "sbar" in want:
            # A real scroll. RP_MOUSE_WHEEL_CHANGED draws nothing on this path
            # (measured), so scroll the way that works: click a scrollbar
            # arrow. Alternating the two arrows keeps the view in range over
            # many trials instead of parking at an end where nothing moves.
            pt = sdown if i % 2 == 0 else sup
            sess.send(f_move(pt[0], pt[1]))
            quiesce(sess, args.quiet, 3.0)
            add("e2_scrollbar_click",
                run_trial(sess, "sbar", f_down(pt[0], pt[1], 1, 1),
                          args.collect, args.cap, bg))
            sess.send(f_up(pt[0], pt[1], 0))
            quiesce(sess, args.quiet, 1.0)

        if "drag" in want:
            sess.send(f_move(title[0], title[1]))
            quiesce(sess, args.quiet, 3.0)
            sess.send(f_down(title[0], title[1], 1, 1))
            quiesce(sess, args.quiet, 3.0)
            dx = 6.0 if i % 2 == 0 else -6.0
            add("f_drag_window",
                run_trial(sess, "drag",
                          f_move(title[0] + dx, title[1]),
                          args.collect, args.cap, bg))
            sess.send(f_up(title[0] + dx, title[1], 0))
            quiesce(sess, args.quiet, 1.0)

    print("")
    print("=== input-write -> response-arrival, both stamps in this process ===")
    for label in sorted(results):
        trs = results[label]
        print("-- %s (n=%d)" % (label, len(trs)))
        print("   " + summarise("first_byte", [t.t_byte for t in trs]))
        print("   " + summarise("first_msg", [t.t_msg for t in trs]))
        print("   " + summarise("first_cursor_op", [t.t_cursor for t in trs]))
        print("   " + summarise("first_drawing_op", [t.t_draw for t in trs]))
        print("   " + summarise("first_NOVEL_draw", [t.t_novel for t in trs]))
        codes = {}
        for t in trs:
            for c in t.codes:
                codes[c] = codes.get(c, 0) + 1
        top = sorted(codes.items(), key=lambda kv: -kv[1])[:6]
        print("   ops: %s" % ", ".join("%s=%d" % (code_name(c), n)
                                       for c, n in top))
        print("   bytes/trial p50=%s  no-response=%d  no-novel=%d"
              % (pct([float(t.bytes) for t in trs], 50),
                 sum(1 for t in trs if t.t_byte is None),
                 sum(1 for t in trs if t.t_novel is None)))
        nk = {}
        for t in trs:
            if t.novel_code is not None:
                k = "%s%s" % (code_name(t.novel_code), t.novel_key)
                nk[k] = nk.get(k, 0) + 1
        if nk:
            print("   novel sites: %s"
                  % ", ".join("%s x%d" % (k, n) for k, n in
                              sorted(nk.items(), key=lambda kv: -kv[1])[:4]))

    print("")
    print("string_width_queries=%d read_bitmap_queries=%d replies=%d"
          % (sess.string_width_queries, sess.read_bitmap_queries,
             sess.replies))

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"floor_tcp_loopback_ms": floor,
                       "floor_errors": floor_err,
                       "idle_gaps_ms": idle_gaps,
                       "host": args.host, "port": args.port,
                       "trials": raw,
                       "string_width_queries": sess.string_width_queries,
                       "read_bitmap_queries": sess.read_bitmap_queries},
                      f)
        print("json: %s" % args.json)
    sess.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
