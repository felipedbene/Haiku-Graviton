#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rdcapture.py -- minimal client for Haiku app_server's "remote desktop"
drawing protocol, used as a *measuring instrument*: it connects, drives the
handshake, decodes the drawing op stream, rasterises what it can into a
software framebuffer and reports per-token op counts plus a pure-black pixel
census (so "is the Deskbar actually drawing?" gets a numeric answer).

Stdlib only.  No PIL: the PNG is written by hand with zlib + struct.

PROTOCOL PROVENANCE (all paths relative to the Haiku source tree)
-----------------------------------------------------------------
Framing -- src/servers/app/drawing/interface/remote/RemoteMessage.h:239-263
  Start(code) writes uint16 code then a uint32 size placeholder; Flush()
  back-patches that uint32 with fWriteIndex, i.e. the TOTAL byte count
  *including* the 6-byte header.  RemoteMessage.cpp:53-76 (NextMessage)
  confirms it by computing fDataLeft = dataLeft - kHeaderSize with
  kHeaderSize = sizeof(uint16) + sizeof(uint32) = 6.
  Add<T>() is a raw memcpy of sizeof(T) with no alignment padding
  (RemoteMessage.h:266-276), so payloads are packed and host-endian.
  Haiku/x86 and Haiku/arm64 are little-endian, so "<" everywhere.

Handshake -- src/servers/app/drawing/interface/remote/RemoteHWInterface.cpp:
  267-289 (server side) and src/apps/remotedesktop/RemoteView.cpp:446-480
  (reference client).  Client sends RP_INIT_CONNECTION (no payload); server
  answers RP_INIT_CONNECTION, RP_SET_CURSOR, RP_SET_CURSOR_VISIBLE,
  RP_MOVE_CURSOR_TO; on RP_INIT_CONNECTION the client replies
  RP_UPDATE_DISPLAY_MODE{int32 width, int32 height}, which sets
  fIsConnected = true and triggers _NotifyScreenChanged() -> full repaint.

Tokens -- RemoteDrawingEngine has one token per drawing engine (one per
  window/offscreen, RemoteDrawingEngine.cpp:31) and *most* ops start with
  uint32 token.  Exceptions are load-bearing and are listed in NO_TOKEN
  below; see the note on RP_FILL_REGION_COLOR_NO_CLIPPING.

OFFSETS note -- RP_SET_OFFSETS is *not* a coordinate translation.
  RemoteDrawingEngine::SetDrawState() (RemoteDrawingEngine.cpp:103-122) sends
  RP_SET_OFFSETS{int32 x, int32 y} with the values ServerWindow passes at
  src/servers/app/ServerWindow.cpp:4506-4508, which are the current view's
  pen-to-screen origin.  They exist so the client can position the *affine
  transform* and pattern origin, exactly as Painter::SetDrawState() uses them
  (src/servers/app/drawing/Painter/Painter.cpp:294-305 -> SetTransform).
  Every geometry argument that reaches a DrawingEngine has already been
  converted to screen coordinates by the caller -- e.g.
  ServerWindow.cpp:2635 does `fCurrentView->PenToScreenTransform().Apply(&rect)`
  immediately before `drawingEngine->FillRect(rect)`.
  Both reference clients agree:
    * src/apps/remotedesktop/RemoteView.cpp:638-647 only does
      offscreen->MovePenTo(xOffset, yOffset) and never translates a rect;
    * src/tools/html5_remote_desktop/HaikuRemoteDesktop.js:1086-1090 uses the
      offsets *solely* to bracket a non-identity transform
      (translate(off) / transform / translate(-off)).
  So this tool records offsets and reports them but does NOT translate by
  them.  --apply-offsets exists to test that reading, and it should make the
  picture visibly wrong.
"""

from __future__ import annotations

import argparse
import base64
import errno
import hashlib
import json
import math
import os
import socket
import ssl
import struct
import sys
import time
import zlib


# ---------------------------------------------------------------------------
# Message codes -- verbatim from RemoteMessage.h:38-139
# ---------------------------------------------------------------------------

RP_INIT_CONNECTION = 1
RP_UPDATE_DISPLAY_MODE = 2
RP_CLOSE_CONNECTION = 3
RP_GET_SYSTEM_PALETTE = 4
RP_GET_SYSTEM_PALETTE_RESULT = 5

# URP/1 capability handshake.  Optional in both directions: a client that never
# sends RP_HELLO negotiates nothing and gets the legacy stream, which is what
# this tool does unless --zstd is given.
RP_HELLO = 6
RP_HELLO_ACK = 7

# Reserved Tier P (pixel/codec) opcodes.  Nothing emits them yet; they are
# named here because they are the wire-compression exemption list -- their
# payloads are already-compressed codec bytes and travel as raw segments.
RP_TIER_BEGIN_FRAME = 280
RP_CODEC_TILE = 281
RP_TIER_END_FRAME = 282
RP_AUDIO_PACKET = 283
RP_FRAME_ACK = 284

# Transport-security preamble, spoken with the remote_broker daemon (never
# with app_server itself): RP_AUTHENTICATE must be the first message on a
# broker connection, RP_AUTH_RESULT is its answer (uint32 status, 0 = ok).
RP_AUTHENTICATE = 10
RP_AUTH_RESULT = 11

RP_CREATE_STATE = 20
RP_DELETE_STATE = 21
RP_ENABLE_SYNC_DRAWING = 22
RP_DISABLE_SYNC_DRAWING = 23
RP_INVALIDATE_RECT = 24
RP_INVALIDATE_REGION = 25

RP_SET_OFFSETS = 40
RP_SET_HIGH_COLOR = 41
RP_SET_LOW_COLOR = 42
RP_SET_PEN_SIZE = 43
RP_SET_STROKE_MODE = 44
RP_SET_BLENDING_MODE = 45
RP_SET_PATTERN = 46
RP_SET_DRAWING_MODE = 47
RP_SET_FONT = 48
RP_SET_TRANSFORM = 49

RP_CONSTRAIN_CLIPPING_REGION = 60
RP_COPY_RECT_NO_CLIPPING = 61
RP_INVERT_RECT = 62
RP_DRAW_BITMAP = 63
RP_DRAW_BITMAP_RECTS = 64

RP_STROKE_ARC = 80
RP_STROKE_BEZIER = 81
RP_STROKE_ELLIPSE = 82
RP_STROKE_POLYGON = 83
RP_STROKE_RECT = 84
RP_STROKE_ROUND_RECT = 85
RP_STROKE_SHAPE = 86
RP_STROKE_TRIANGLE = 87
RP_STROKE_LINE = 88
RP_STROKE_LINE_ARRAY = 89

RP_FILL_ARC = 100
RP_FILL_BEZIER = 101
RP_FILL_ELLIPSE = 102
RP_FILL_POLYGON = 103
RP_FILL_RECT = 104
RP_FILL_ROUND_RECT = 105
RP_FILL_SHAPE = 106
RP_FILL_TRIANGLE = 107
RP_FILL_REGION = 108

RP_FILL_ARC_GRADIENT = 120
RP_FILL_BEZIER_GRADIENT = 121
RP_FILL_ELLIPSE_GRADIENT = 122
RP_FILL_POLYGON_GRADIENT = 123
RP_FILL_RECT_GRADIENT = 124
RP_FILL_ROUND_RECT_GRADIENT = 125
RP_FILL_SHAPE_GRADIENT = 126
RP_FILL_TRIANGLE_GRADIENT = 127
RP_FILL_REGION_GRADIENT = 128

RP_STROKE_POINT_COLOR = 140
RP_STROKE_LINE_1PX_COLOR = 141
RP_STROKE_RECT_1PX_COLOR = 142

RP_FILL_RECT_COLOR = 160
RP_FILL_REGION_COLOR_NO_CLIPPING = 161

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
RP_UNMAPPED_KEY_DOWN = 242
RP_UNMAPPED_KEY_UP = 243
RP_MODIFIERS_CHANGED = 244

RP_STROKE_ARC_GRADIENT = 260
RP_STROKE_BEZIER_GRADIENT = 261
RP_STROKE_ELLIPSE_GRADIENT = 262
RP_STROKE_POLYGON_GRADIENT = 263
RP_STROKE_RECT_GRADIENT = 264
RP_STROKE_ROUND_RECT_GRADIENT = 265
RP_STROKE_SHAPE_GRADIENT = 266
RP_STROKE_TRIANGLE_GRADIENT = 267
RP_STROKE_LINE_GRADIENT = 268

CODE_NAMES = {v: k for k, v in list(globals().items())
              if k.startswith("RP_") and isinstance(v, int)}


def code_name(code: int) -> str:
    return CODE_NAMES.get(code, "RP_UNKNOWN_%d" % code)


# ---------------------------------------------------------------------------
# Colour spaces -- headers/os/interface/GraphicsDefs.h:168-200
# ---------------------------------------------------------------------------

B_NO_COLOR_SPACE = 0x0000
B_RGB32 = 0x0008        # in-memory bytes: B G R -
B_RGBA32 = 0x2008       # in-memory bytes: B G R A
B_RGB24 = 0x0003        # in-memory bytes: B G R
B_RGB32_BIG = 0x1008
B_RGBA32_BIG = 0x3008
B_CMAP8 = 0x0004
B_GRAY8 = 0x0002

# src/kits/interface/GraphicsDefs.cpp:33 -- 0x00777477 as a little-endian
# uint32 is the byte triple (0x77, 0x74, 0x77) with alpha 0x00.
TRANSPARENT_MAGIC_BGR = (0x77, 0x74, 0x77)

PLACEHOLDER = (1, 1, 1)     # "something was drawn here but we did not decode it"


# ---------------------------------------------------------------------------
# Op classification
# ---------------------------------------------------------------------------

DRAWING_OPS = frozenset(
    [RP_DRAW_STRING, RP_DRAW_STRING_WITH_OFFSETS, RP_DRAW_BITMAP,
     RP_DRAW_BITMAP_RECTS, RP_INVERT_RECT]
    + list(range(RP_STROKE_ARC, RP_STROKE_LINE_ARRAY + 1))          # 80..89
    + list(range(RP_FILL_ARC, RP_FILL_REGION + 1))                  # 100..108
    + list(range(RP_FILL_ARC_GRADIENT, RP_FILL_REGION_GRADIENT + 1))  # 120..128
    + list(range(RP_STROKE_POINT_COLOR, RP_STROKE_RECT_1PX_COLOR + 1))  # 140..142
    + [RP_FILL_RECT_COLOR, RP_FILL_REGION_COLOR_NO_CLIPPING]        # 160..161
    + list(range(RP_STROKE_ARC_GRADIENT, RP_STROKE_LINE_GRADIENT + 1))  # 260..268
)

STATE_OPS = frozenset(
    list(range(RP_SET_OFFSETS, RP_SET_TRANSFORM + 1))               # 40..49
    + [RP_CREATE_STATE, RP_DELETE_STATE, RP_ENABLE_SYNC_DRAWING,
       RP_DISABLE_SYNC_DRAWING, RP_INVALIDATE_RECT, RP_INVALIDATE_REGION,
       RP_CONSTRAIN_CLIPPING_REGION,
       RP_INIT_CONNECTION, RP_UPDATE_DISPLAY_MODE, RP_CLOSE_CONNECTION,
       RP_GET_SYSTEM_PALETTE, RP_GET_SYSTEM_PALETTE_RESULT,
       RP_SET_CURSOR, RP_SET_CURSOR_VISIBLE, RP_MOVE_CURSOR_TO]
)

# RP_COPY_RECT_NO_CLIPPING is a blit, not in either list above; it is counted
# in its own bucket.  RP_STRING_WIDTH / RP_READ_BITMAP are queries.
OTHER_OPS = frozenset([RP_COPY_RECT_NO_CLIPPING, RP_STRING_WIDTH,
                       RP_READ_BITMAP])

# Ops whose payload does NOT begin with uint32 token.
#
# Everything RemoteView::_DrawThread() handles *before* its unconditional
# `message.Read(token)` at src/apps/remotedesktop/RemoteView.cpp:605-606.
# Two of these are surprising and matter:
#   * RP_FILL_REGION_COLOR_NO_CLIPPING (RemoteDrawingEngine.cpp:604-610)
#     -- FillRegion(region, color) writes NO token, only region + colour.
#        The reference client decodes it token-less at RemoteView.cpp:570-585.
#   * RP_COPY_RECT_NO_CLIPPING (RemoteDrawingEngine.cpp:292-300)
#     -- CopyRect() writes NO token either; RemoteView.cpp:587-602 agrees.
# RP_CREATE_STATE / RP_DELETE_STATE DO carry a token (RemoteView.cpp:488-500).
NO_TOKEN = frozenset([
    RP_INIT_CONNECTION, RP_UPDATE_DISPLAY_MODE, RP_CLOSE_CONNECTION,
    RP_GET_SYSTEM_PALETTE, RP_GET_SYSTEM_PALETTE_RESULT,
    RP_HELLO, RP_HELLO_ACK,
    RP_INVALIDATE_RECT, RP_INVALIDATE_REGION,
    RP_COPY_RECT_NO_CLIPPING, RP_FILL_REGION_COLOR_NO_CLIPPING,
    RP_SET_CURSOR, RP_SET_CURSOR_VISIBLE, RP_MOVE_CURSOR_TO,
])

# Ops whose payload is: uint32 token, BRect bounds, <more>.  We only need the
# leading rect to know where they drew, so they are "approximated": the bounds
# get painted with PLACEHOLDER rather than properly rasterised.
LEADING_RECT_OPS = frozenset([
    RP_STROKE_ARC, RP_STROKE_ELLIPSE, RP_STROKE_POLYGON, RP_STROKE_ROUND_RECT,
    RP_STROKE_SHAPE,
    RP_FILL_ARC, RP_FILL_ELLIPSE, RP_FILL_POLYGON, RP_FILL_ROUND_RECT,
    RP_FILL_SHAPE,
    RP_FILL_ARC_GRADIENT, RP_FILL_ELLIPSE_GRADIENT, RP_FILL_POLYGON_GRADIENT,
    RP_FILL_RECT_GRADIENT, RP_FILL_ROUND_RECT_GRADIENT, RP_FILL_SHAPE_GRADIENT,
    RP_STROKE_ARC_GRADIENT, RP_STROKE_ELLIPSE_GRADIENT,
    RP_STROKE_POLYGON_GRADIENT, RP_STROKE_RECT_GRADIENT,
    RP_STROKE_ROUND_RECT_GRADIENT, RP_STROKE_SHAPE_GRADIENT,
])

# uint32 token, then N BPoints; bounds derived from the points.
POINT_LIST_OPS = {
    RP_STROKE_BEZIER: 4, RP_FILL_BEZIER: 4,
    RP_STROKE_BEZIER_GRADIENT: 4, RP_FILL_BEZIER_GRADIENT: 4,
    RP_STROKE_TRIANGLE: 3, RP_FILL_TRIANGLE: 3,
    RP_STROKE_TRIANGLE_GRADIENT: 3, RP_FILL_TRIANGLE_GRADIENT: 3,
    RP_STROKE_LINE_GRADIENT: 2,
}

# uint32 token, then a region (int32 count + count BRects).
REGION_OPS = frozenset([RP_FILL_REGION, RP_FILL_REGION_GRADIENT])


# ---------------------------------------------------------------------------
# Little-endian payload reader
# ---------------------------------------------------------------------------

class Truncated(Exception):
    pass


class Reader(object):
    """Reads packed little-endian fields out of one message payload."""

    __slots__ = ("buf", "pos")

    def __init__(self, buf):
        self.buf = buf
        self.pos = 0

    def left(self):
        return len(self.buf) - self.pos

    def _take(self, n):
        if self.pos + n > len(self.buf):
            raise Truncated("want %d bytes, %d left" % (n, self.left()))
        out = self.buf[self.pos:self.pos + n]
        self.pos += n
        return out

    def u8(self):
        return self._take(1)[0]

    def bool8(self):
        # Add(const bool&) writes sizeof(bool) == 1 byte.
        return self._take(1)[0] != 0

    def u16(self):
        return struct.unpack_from("<H", self._take(2))[0]

    def i32(self):
        return struct.unpack_from("<i", self._take(4))[0]

    def u32(self):
        return struct.unpack_from("<I", self._take(4))[0]

    def f32(self):
        return struct.unpack_from("<f", self._take(4))[0]

    def point(self):
        # BPoint { float x, y }
        return struct.unpack_from("<ff", self._take(8))

    def rect(self):
        # BRect { float left, top, right, bottom } -- right/bottom inclusive
        return struct.unpack_from("<ffff", self._take(16))

    def color(self):
        # rgb_color { uint8 red, green, blue, alpha }
        b = self._take(4)
        return (b[0], b[1], b[2], b[3])

    def pattern(self):
        # pattern { uint8 data[8] }
        return bytes(self._take(8))

    def region(self):
        # RemoteMessage.h:292-300 (AddRegion): int32 rectCount, then rects
        count = self.i32()
        if count < 0 or count > 1 << 20:
            raise Truncated("absurd region rect count %d" % count)
        return [self.rect() for _ in range(count)]

    def string(self):
        # RemoteMessage.h:279-289 (AddString): uint32 length, then bytes
        length = self.u32()
        if length > self.left():
            raise Truncated("string length %d > %d left" % (length, self.left()))
        return bytes(self._take(length))

    def transform(self):
        # RemoteMessage.cpp:280-294 (AddTransform): bool isIdentity, then
        # 6 floats sx shy shx sy tx ty when not identity.
        if self.bool8():
            return None
        return struct.unpack_from("<ffffff", self._take(24))

    def font(self):
        """RemoteMessage.cpp:115-127 (AddFont), packed, 29 bytes:
        uint8 direction, uint8 encoding, uint32 flags, uint8 spacing,
        float shear, float rotation, float falseBoldWidth, float size,
        uint16 face, uint32 familyAndStyle."""
        direction = self.u8()
        encoding = self.u8()
        flags = self.u32()
        spacing = self.u8()
        shear = self.f32()
        rotation = self.f32()
        false_bold = self.f32()
        size = self.f32()
        face = self.u16()
        family_and_style = self.u32()
        return {
            "direction": direction, "encoding": encoding, "flags": flags,
            "spacing": spacing, "shear": shear, "rotation": rotation,
            "false_bold_width": false_bold, "size": size, "face": face,
            "family_and_style": family_and_style,
        }

    def gradient(self):
        """RemoteMessage.cpp:196-276 (AddGradient)."""
        gtype = self.u32()
        if gtype == 1:              # TYPE_LINEAR: start, end
            self.point(); self.point()
        elif gtype == 2:            # TYPE_RADIAL: center, radius
            self.point(); self.f32()
        elif gtype == 3:            # TYPE_RADIAL_FOCUS: center, focal, radius
            self.point(); self.point(); self.f32()
        elif gtype == 4:            # TYPE_DIAMOND: center
            self.point()
        elif gtype == 5:            # TYPE_CONIC: center, angle
            self.point(); self.f32()
        stops = self.i32()
        if stops < 0 or stops > 1 << 16:
            raise Truncated("absurd gradient stop count %d" % stops)
        for _ in range(stops):
            self.color(); self.f32()
        return gtype

    def bitmap(self, minimal=False, colorspace=None, flags=0):
        """RemoteMessage.cpp:91-111 (server AddBitmap) / 333-382 (ReadBitmap):
        int32 width, int32 height, int32 bytesPerRow,
        [color_space colorSpace (4 bytes), uint32 flags]  <- only if !minimal,
        uint32 bitsLength, then bitsLength raw bytes."""
        width = self.i32()
        height = self.i32()
        bytes_per_row = self.i32()
        if not minimal:
            colorspace = self.u32()
            flags = self.u32()
        bits_length = self.u32()
        if bits_length > self.left():
            raise Truncated("bitmap bits %d > %d left" % (bits_length,
                                                          self.left()))
        bits = self._take(bits_length)
        return {"width": width, "height": height, "bpr": bytes_per_row,
                "colorspace": colorspace, "flags": flags, "bits": bits}


def utf8_count_chars(data: bytes) -> int:
    """utf8_functions.h UTF8CountChars(): count non-continuation bytes."""
    return sum(1 for b in data if (b & 0xC0) != 0x80)


# ---------------------------------------------------------------------------
# PNG writer (zlib + struct, 8-bit RGB, filter type 0)
# ---------------------------------------------------------------------------

def write_png(path: str, width: int, height: int, rgb: bytes) -> None:
    stride = width * 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)                                   # filter type 0 (None)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    out = [b"\x89PNG\r\n\x1a\n",
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)),
           chunk(b"IDAT", zlib.compress(bytes(raw), 6)),
           chunk(b"IEND", b"")]
    with open(path, "wb") as f:
        f.write(b"".join(out))


# ---------------------------------------------------------------------------
# Software framebuffer
# ---------------------------------------------------------------------------

def rect_to_pixels(rect):
    """BRect (float, right/bottom inclusive) -> inclusive integer pixel box.

    Haiku rasterises non-subpixel geometry by rounding, and BRect(0,0,9,9) is
    a 10x10 area, so both edges are inclusive after rounding."""
    left, top, right, bottom = rect
    x0 = int(math.floor(left + 0.5))
    y0 = int(math.floor(top + 0.5))
    x1 = int(math.floor(right + 0.5))
    y1 = int(math.floor(bottom + 0.5))
    return x0, y0, x1, y1


class Framebuffer(object):
    """RGB8 framebuffer plus an "estimated" mask.

    The mask records pixels that are non-black *only* because we painted a
    guess there (an undecoded op's bounding box, or the estimated bounding box
    of a text run).  It lets us report a strict black count that does not
    depend on any guess -- which is the whole point of being able to trust a
    negative result."""

    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.buf = bytearray(width * height * 3)        # pure black
        self.est = bytearray(width * height)            # 1 = guessed pixel

    # -- primitives ------------------------------------------------------
    def _clip_box(self, x0, y0, x1, y1):
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        x0 = max(0, x0)
        y0 = max(0, y0)
        x1 = min(self.width - 1, x1)
        y1 = min(self.height - 1, y1)
        if x0 > x1 or y0 > y1:
            return None
        return x0, y0, x1, y1

    def fill_box(self, x0, y0, x1, y1, color, estimated=False):
        """Returns the number of pixels touched (0 if fully off-screen)."""
        box = self._clip_box(x0, y0, x1, y1)
        if box is None:
            return 0
        x0, y0, x1, y1 = box
        n = x1 - x0 + 1
        row = bytes(color[:3]) * n
        touched = 0
        for y in range(y0, y1 + 1):
            base = y * self.width + x0
            off = base * 3
            if estimated:
                # Only overwrite pixels that are still pure black, so a guess
                # never destroys a genuinely decoded pixel.
                seg = self.buf[off:off + n * 3]
                for i in range(n):
                    j = i * 3
                    if seg[j] == 0 and seg[j + 1] == 0 and seg[j + 2] == 0:
                        seg[j] = color[0]
                        seg[j + 1] = color[1]
                        seg[j + 2] = color[2]
                        self.est[base + i] = 1
                        touched += 1
                self.buf[off:off + n * 3] = seg
            else:
                self.buf[off:off + n * 3] = row
                self.est[base:base + n] = b"\x00" * n
                touched += n
        return touched

    def set_pixel(self, x, y, color, estimated=False):
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return 0
        base = y * self.width + x
        off = base * 3
        if estimated:
            if self.buf[off] or self.buf[off + 1] or self.buf[off + 2]:
                return 0
            self.est[base] = 1
        else:
            self.est[base] = 0
        self.buf[off] = color[0]
        self.buf[off + 1] = color[1]
        self.buf[off + 2] = color[2]
        return 1

    def stroke_box(self, x0, y0, x1, y1, color, estimated=False):
        box = self._clip_box(x0, y0, x1, y1)
        if box is None:
            return 0
        touched = 0
        touched += self.fill_box(x0, y0, x1, y0, color, estimated)
        touched += self.fill_box(x0, y1, x1, y1, color, estimated)
        touched += self.fill_box(x0, y0, x0, y1, color, estimated)
        touched += self.fill_box(x1, y0, x1, y1, color, estimated)
        return touched

    def line(self, xa, ya, xb, yb, color, estimated=False):
        """Integer Bresenham, 1px."""
        touched = 0
        dx = abs(xb - xa)
        dy = abs(yb - ya)
        sx = 1 if xa < xb else -1
        sy = 1 if ya < yb else -1
        err = dx - dy
        x, y = xa, ya
        guard = dx + dy + 2
        while guard > 0:
            guard -= 1
            touched += self.set_pixel(x, y, color, estimated)
            if x == xb and y == yb:
                break
            e2 = 2 * err
            if e2 > -dy:
                err -= dy
                x += sx
            if e2 < dx:
                err += dx
                y += sy
        return touched

    def invert_box(self, x0, y0, x1, y1):
        box = self._clip_box(x0, y0, x1, y1)
        if box is None:
            return 0
        x0, y0, x1, y1 = box
        n = x1 - x0 + 1
        for y in range(y0, y1 + 1):
            base = y * self.width + x0
            off = base * 3
            seg = self.buf[off:off + n * 3]
            for i in range(len(seg)):
                seg[i] = 255 - seg[i]
            self.buf[off:off + n * 3] = seg
            self.est[base:base + n] = b"\x00" * n
        return n * (y1 - y0 + 1)

    def copy_box(self, sx0, sy0, sx1, sy1, dx, dy):
        src = self._clip_box(sx0, sy0, sx1, sy1)
        if src is None:
            return 0
        sx0, sy0, sx1, sy1 = src
        n = sx1 - sx0 + 1
        rows = []
        for y in range(sy0, sy1 + 1):
            off = (y * self.width + sx0) * 3
            rows.append(bytes(self.buf[off:off + n * 3]))
        touched = 0
        for i, rowdata in enumerate(rows):
            ty = dy + i
            if ty < 0 or ty >= self.height:
                continue
            tx0 = dx
            take = rowdata
            if tx0 < 0:
                take = take[-3 * tx0:]
                tx0 = 0
            overflow = tx0 + len(take) // 3 - self.width
            if overflow > 0:
                take = take[:len(take) - 3 * overflow]
            if not take:
                continue
            base = ty * self.width + tx0
            self.buf[base * 3:base * 3 + len(take)] = take
            self.est[base:base + len(take) // 3] = b"\x00" * (len(take) // 3)
            touched += len(take) // 3
        return touched

    # -- bitmap blit -----------------------------------------------------
    def draw_bitmap(self, bmp, src_rect, dst_rect):
        """Nearest-neighbour blit of a decoded bitmap into dst_rect.

        Returns (touched, decoded) where decoded is False when the colour space
        was not handled and dst_rect got the flat placeholder instead."""
        cs = bmp["colorspace"]
        w, h, bpr, bits = bmp["width"], bmp["height"], bmp["bpr"], bmp["bits"]
        dx0, dy0, dx1, dy1 = rect_to_pixels(dst_rect)
        if cs not in (B_RGB32, B_RGBA32) or w <= 0 or h <= 0 or bpr <= 0:
            return self.fill_box(dx0, dy0, dx1, dy1, PLACEHOLDER,
                                 estimated=True), False

        sx0, sy0, sx1, sy1 = rect_to_pixels(src_rect)
        sw = max(1, sx1 - sx0 + 1)
        sh = max(1, sy1 - sy0 + 1)
        dw = dx1 - dx0 + 1
        dh = dy1 - dy0 + 1
        if dw <= 0 or dh <= 0:
            return 0, True

        has_alpha = (cs == B_RGBA32)
        touched = 0
        for ty in range(dh):
            y = dy0 + ty
            if y < 0 or y >= self.height:
                continue
            src_y = sy0 + (ty * sh) // dh
            if src_y < 0 or src_y >= h:
                continue
            rowbase = src_y * bpr
            for tx in range(dw):
                x = dx0 + tx
                if x < 0 or x >= self.width:
                    continue
                src_x = sx0 + (tx * sw) // dw
                if src_x < 0 or src_x >= w:
                    continue
                o = rowbase + src_x * 4
                if o + 3 > len(bits):
                    continue
                b, g, r = bits[o], bits[o + 1], bits[o + 2]
                a = bits[o + 3]
                if has_alpha and a == 0:
                    continue
                if (not has_alpha
                        and (b, g, r) == TRANSPARENT_MAGIC_BGR and a == 0):
                    continue        # B_TRANSPARENT_MAGIC_RGBA32
                touched += self.set_pixel(x, y, (r, g, b))
        return touched, True

    # -- census ----------------------------------------------------------
    def black_counts(self, box=None):
        """Returns (black, black_strict) inside box (inclusive) or whole screen.

        black        -- pixels that are exactly (0,0,0) in the framebuffer
        black_strict -- black pixels PLUS pixels that are non-black only
                        because a guessed placeholder was painted there
        """
        if box is None:
            x0, y0, x1, y1 = 0, 0, self.width - 1, self.height - 1
        else:
            b = self._clip_box(*box)
            if b is None:
                return 0, 0
            x0, y0, x1, y1 = b
        black = 0
        strict = 0
        buf = self.buf
        est = self.est
        for y in range(y0, y1 + 1):
            base = y * self.width
            off = base * 3
            for x in range(x0, x1 + 1):
                o = off + x * 3
                if buf[o] == 0 and buf[o + 1] == 0 and buf[o + 2] == 0:
                    black += 1
                    strict += 1
                elif est[base + x]:
                    strict += 1
        return black, strict


# ---------------------------------------------------------------------------
# Per-token bookkeeping
# ---------------------------------------------------------------------------

class TokenState(object):
    def __init__(self, token):
        self.token = token
        self.offsets = []               # every (x, y) seen, in order
        self.op_counts = {}
        self.high_color = (0, 0, 0, 255)
        self.low_color = (255, 255, 255, 255)
        self.pattern = b"\xff" * 8      # RemoteView.cpp:397 -- B_SOLID_HIGH
        self.pen_size = 1.0
        self.font_size = 12.0           # be_plain_font default
        self.clip = None                # None = no clipping constraint yet
        self.bbox = None                # union of everything it drew into
        self.drawing_ops = 0
        self.state_ops = 0
        self.other_ops = 0
        self.undecoded_drawing_ops = 0
        self.estimated_ops = 0
        self.pixels_touched = 0
        self.created = False
        self.deleted = False

    def note_op(self, code):
        self.op_counts[code] = self.op_counts.get(code, 0) + 1
        if code in DRAWING_OPS:
            self.drawing_ops += 1
        elif code in STATE_OPS:
            self.state_ops += 1
        else:
            self.other_ops += 1

    def note_rect(self, x0, y0, x1, y1):
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        if self.bbox is None:
            self.bbox = [x0, y0, x1, y1]
        else:
            b = self.bbox
            b[0] = min(b[0], x0)
            b[1] = min(b[1], y0)
            b[2] = max(b[2], x1)
            b[3] = max(b[3], y1)

    def effective_color(self):
        """RP_FILL_RECT / RP_STROKE_RECT / RP_FILL_REGION use the view's
        pattern with high/low colour (RemoteView.cpp:989, 1208).  We do not
        rasterise 8x8 patterns: all-zero pattern (B_SOLID_LOW) -> low colour,
        anything else -> high colour."""
        if self.pattern == b"\x00" * 8:
            return self.low_color[:3]
        return self.high_color[:3]

    def current_offsets(self):
        return self.offsets[-1] if self.offsets else (0, 0)

    def breakdown(self):
        out = {"draw_string": 0, "draw_bitmap": 0, "fill": 0, "stroke": 0,
               "invert": 0}
        for code, n in self.op_counts.items():
            if code in (RP_DRAW_STRING, RP_DRAW_STRING_WITH_OFFSETS):
                out["draw_string"] += n
            elif code in (RP_DRAW_BITMAP, RP_DRAW_BITMAP_RECTS):
                out["draw_bitmap"] += n
            elif code == RP_INVERT_RECT:
                out["invert"] += n
            elif code in DRAWING_OPS:
                name = code_name(code)
                if "FILL" in name:
                    out["fill"] += n
                elif "STROKE" in name:
                    out["stroke"] += n
        return out


NO_TOKEN_KEY = -1       # pseudo-token for token-less drawing ops


# ---------------------------------------------------------------------------
# The decoder / rasteriser
# ---------------------------------------------------------------------------

class Capture(object):
    # Ops we rasterise from their real geometry and real colour.
    FULLY_DECODED = frozenset([
        RP_FILL_RECT_COLOR, RP_FILL_REGION_COLOR_NO_CLIPPING,
        RP_STROKE_RECT_1PX_COLOR, RP_STROKE_LINE_1PX_COLOR,
        RP_STROKE_POINT_COLOR, RP_STROKE_LINE_ARRAY,
        RP_INVERT_RECT, RP_DRAW_BITMAP, RP_DRAW_BITMAP_RECTS,
    ])
    # Ops we rasterise from real geometry but an approximated colour (the
    # token's last high/low colour instead of a real 8x8 pattern).
    PATTERN_APPROX = frozenset([
        RP_FILL_RECT, RP_FILL_REGION, RP_STROKE_RECT, RP_STROKE_LINE,
    ])

    def __init__(self, width, height, clip=True, apply_offsets=False,
                 verbose=False, reply=True):
        self.fb = Framebuffer(width, height)
        self.width = width
        self.height = height
        self.do_clip = clip
        self.apply_offsets = apply_offsets
        self.verbose = verbose
        self.reply = reply
        self.tokens = {}
        self.global_counts = {}
        self.messages = 0
        self.truncated = 0
        self.unknown_codes = {}
        self.undecoded_drawing_ops = 0
        self.undecoded_no_rect_ops = 0
        self.estimated_text_ops = 0
        self.clipped_out_ops = 0
        self.bitmaps_decoded = 0
        self.bitmaps_placeholder = 0
        self.bitmap_colorspaces = {}
        self.outbox = []                # queued replies (code, payload bytes)
        self.errors = []
        self.negotiated_version = None
        self.negotiated_capabilities = None

    # -- helpers ---------------------------------------------------------
    def token_state(self, token):
        st = self.tokens.get(token)
        if st is None:
            st = TokenState(token)
            self.tokens[token] = st
        return st

    def _xy(self, st):
        if self.apply_offsets:
            return st.current_offsets()
        return (0, 0)

    def _clip_boxes(self, st):
        """None -> unclipped; [] -> everything clipped away."""
        if not self.do_clip or st is None or st.clip is None:
            return None
        return st.clip

    def _paint(self, st, kind, geom, color, estimated=False):
        """kind in {'fill','stroke','line','point','invert'}; geom in pixels."""
        ox, oy = self._xy(st) if st is not None else (0, 0)
        clips = self._clip_boxes(st)
        if clips is not None and len(clips) == 0:
            self.clipped_out_ops += 1
            return 0

        def boxes_for(x0, y0, x1, y1):
            if clips is None:
                return [(x0, y0, x1, y1)]
            out = []
            for c in clips:
                cx0, cy0, cx1, cy1 = rect_to_pixels(c)
                ix0 = max(x0, cx0 + ox)
                iy0 = max(y0, cy0 + oy)
                ix1 = min(x1, cx1 + ox)
                iy1 = min(y1, cy1 + oy)
                if ix0 <= ix1 and iy0 <= iy1:
                    out.append((ix0, iy0, ix1, iy1))
            return out

        touched = 0
        if kind == "fill":
            x0, y0, x1, y1 = geom
            for b in boxes_for(x0, y0, x1, y1):
                touched += self.fb.fill_box(b[0], b[1], b[2], b[3], color,
                                            estimated)
        elif kind == "stroke":
            x0, y0, x1, y1 = geom
            if clips is None:
                touched += self.fb.stroke_box(x0, y0, x1, y1, color, estimated)
            else:
                # Stroke the four edges, each clipped as a thin fill.
                for edge in ((x0, y0, x1, y0), (x0, y1, x1, y1),
                             (x0, y0, x0, y1), (x1, y0, x1, y1)):
                    for b in boxes_for(*edge):
                        touched += self.fb.fill_box(b[0], b[1], b[2], b[3],
                                                    color, estimated)
        elif kind == "line":
            xa, ya, xb, yb = geom
            if clips is None:
                touched += self.fb.line(xa, ya, xb, yb, color, estimated)
            else:
                # Cheap: only draw the line if its bbox meets some clip rect.
                bx0, by0 = min(xa, xb), min(ya, yb)
                bx1, by1 = max(xa, xb), max(ya, yb)
                if boxes_for(bx0, by0, bx1, by1):
                    touched += self.fb.line(xa, ya, xb, yb, color, estimated)
                else:
                    self.clipped_out_ops += 1
        elif kind == "point":
            x, y = geom
            if clips is None or boxes_for(x, y, x, y):
                touched += self.fb.set_pixel(x, y, color, estimated)
        elif kind == "invert":
            x0, y0, x1, y1 = geom
            for b in boxes_for(x0, y0, x1, y1):
                touched += self.fb.invert_box(b[0], b[1], b[2], b[3])

        if st is not None and touched:
            st.pixels_touched += touched
        return touched

    def _rect_px(self, st, rect):
        ox, oy = self._xy(st) if st is not None else (0, 0)
        x0, y0, x1, y1 = rect_to_pixels(rect)
        return x0 + ox, y0 + oy, x1 + ox, y1 + oy

    def _note_dest(self, st, x0, y0, x1, y1):
        if st is not None:
            st.note_rect(x0, y0, x1, y1)

    # -- top-level dispatch ---------------------------------------------
    def handle(self, code, payload):
        self.messages += 1
        self.global_counts[code] = self.global_counts.get(code, 0) + 1
        if code not in CODE_NAMES:
            self.unknown_codes[code] = self.unknown_codes.get(code, 0) + 1

        r = Reader(payload)
        token = NO_TOKEN_KEY
        st = None
        if code not in NO_TOKEN:
            try:
                token = r.u32()
            except Truncated:
                self.truncated += 1
                return
            st = self.token_state(token)
        else:
            st = self.token_state(NO_TOKEN_KEY)

        st.note_op(code)

        if self.verbose:
            tok = "-" if token == NO_TOKEN_KEY else str(token)
            sys.stderr.write("  msg %-34s token=%-10s payload=%d\n"
                             % (code_name(code), tok, len(payload)))

        try:
            self._decode(code, r, st, token)
        except Truncated as exc:
            self.truncated += 1
            if len(self.errors) < 20:
                self.errors.append("%s: %s" % (code_name(code), exc))

    def _decode(self, code, r, st, token):
        fb = self.fb

        # ---- connection / cursor / misc state -------------------------
        if code == RP_INIT_CONNECTION:
            self.outbox.append((RP_UPDATE_DISPLAY_MODE,
                                struct.pack("<ii", self.width, self.height)))
            return
        if code in (RP_CLOSE_CONNECTION, RP_UPDATE_DISPLAY_MODE,
                    RP_GET_SYSTEM_PALETTE, RP_GET_SYSTEM_PALETTE_RESULT):
            return
        if code == RP_HELLO_ACK:
            # uint32 negotiated version, uint32 negotiated capabilities.  The
            # wire decoder acts on the capabilities well before this point (it
            # sits under the framing); recorded here only for the report.
            self.negotiated_version = r.u32()
            self.negotiated_capabilities = r.u32()
            return
        if code == RP_SET_CURSOR:
            r.point()
            r.bitmap()
            return
        if code == RP_SET_CURSOR_VISIBLE:
            r.bool8()
            return
        if code == RP_MOVE_CURSOR_TO:
            r.point()
            return
        if code == RP_INVALIDATE_RECT:
            r.rect()
            return
        if code == RP_INVALIDATE_REGION:
            r.region()
            return

        # ---- per-token state -----------------------------------------
        if code == RP_CREATE_STATE:
            st.created = True
            return
        if code == RP_DELETE_STATE:
            st.deleted = True
            return
        if code in (RP_ENABLE_SYNC_DRAWING, RP_DISABLE_SYNC_DRAWING):
            return
        if code == RP_SET_OFFSETS:
            x = r.i32()
            y = r.i32()
            st.offsets.append((x, y))
            return
        if code == RP_SET_HIGH_COLOR:
            st.high_color = r.color()
            return
        if code == RP_SET_LOW_COLOR:
            st.low_color = r.color()
            return
        if code == RP_SET_PEN_SIZE:
            st.pen_size = r.f32()
            return
        if code == RP_SET_STROKE_MODE:
            r.u32(); r.u32(); r.f32()
            return
        if code == RP_SET_BLENDING_MODE:
            r.u32(); r.u32()
            return
        if code == RP_SET_PATTERN:
            st.pattern = r.pattern()
            return
        if code == RP_SET_DRAWING_MODE:
            r.u32()
            return
        if code == RP_SET_FONT:
            st.font_size = r.font()["size"]
            return
        if code == RP_SET_TRANSFORM:
            r.transform()
            return
        if code == RP_CONSTRAIN_CLIPPING_REGION:
            st.clip = r.region()
            return

        # ---- blit ----------------------------------------------------
        if code == RP_COPY_RECT_NO_CLIPPING:
            # RemoteDrawingEngine.cpp:292-300 -- NO token: int32 xOffset,
            # int32 yOffset, BRect rect.  RemoteView.cpp:587-602 agrees.
            xo = r.i32()
            yo = r.i32()
            x0, y0, x1, y1 = rect_to_pixels(r.rect())
            n = fb.copy_box(x0, y0, x1, y1, x0 + xo, y0 + yo)
            st.pixels_touched += n
            self._note_dest(st, x0 + xo, y0 + yo, x1 + xo, y1 + yo)
            return

        # ---- fully decoded drawing ops -------------------------------
        if code == RP_FILL_RECT_COLOR:
            rect = r.rect()
            color = r.color()
            box = self._rect_px(st, rect)
            self._note_dest(st, *box)
            self._paint(st, "fill", box, color[:3])
            return

        if code == RP_STROKE_RECT_1PX_COLOR:
            rect = r.rect()
            color = r.color()
            box = self._rect_px(st, rect)
            self._note_dest(st, *box)
            self._paint(st, "stroke", box, color[:3])
            return

        if code == RP_STROKE_LINE_1PX_COLOR:
            p0 = r.point()
            p1 = r.point()
            color = r.color()
            ox, oy = self._xy(st)
            xa = int(round(p0[0])) + ox
            ya = int(round(p0[1])) + oy
            xb = int(round(p1[0])) + ox
            yb = int(round(p1[1])) + oy
            self._note_dest(st, min(xa, xb), min(ya, yb), max(xa, xb),
                            max(ya, yb))
            self._paint(st, "line", (xa, ya, xb, yb), color[:3])
            return

        if code == RP_STROKE_POINT_COLOR:
            p = r.point()
            color = r.color()
            ox, oy = self._xy(st)
            x = int(round(p[0])) + ox
            y = int(round(p[1])) + oy
            self._note_dest(st, x, y, x, y)
            self._paint(st, "point", (x, y), color[:3])
            return

        if code == RP_STROKE_LINE_ARRAY:
            # RemoteDrawingEngine.cpp:863-872 -- token, int32 numLines, then
            # per line AddArrayLine: BPoint start, BPoint end, rgb_color.
            n = r.i32()
            for _ in range(max(0, min(n, 1 << 20))):
                p0 = r.point()
                p1 = r.point()
                color = r.color()
                ox, oy = self._xy(st)
                xa = int(round(p0[0])) + ox
                ya = int(round(p0[1])) + oy
                xb = int(round(p1[0])) + ox
                yb = int(round(p1[1])) + oy
                self._note_dest(st, min(xa, xb), min(ya, yb), max(xa, xb),
                                max(ya, yb))
                self._paint(st, "line", (xa, ya, xb, yb), color[:3])
            return

        if code == RP_FILL_REGION_COLOR_NO_CLIPPING:
            # RemoteDrawingEngine.cpp:604-610 -- NO token: region, rgb_color.
            rects = r.region()
            color = r.color()
            for rect in rects:
                box = rect_to_pixels(rect)
                self._note_dest(st, *box)
                # "NO_CLIPPING" is literal: never clip this one.
                n = fb.fill_box(box[0], box[1], box[2], box[3], color[:3])
                st.pixels_touched += n
            return

        if code == RP_INVERT_RECT:
            rect = r.rect()
            box = self._rect_px(st, rect)
            self._note_dest(st, *box)
            self._paint(st, "invert", box, None)
            return

        if code == RP_DRAW_BITMAP:
            # RemoteDrawingEngine.cpp:384-391 -- token, BRect bitmapRect,
            # BRect viewRect, uint32 options, AddBitmap (non-minimal).
            bitmap_rect = r.rect()
            view_rect = r.rect()
            r.u32()                                  # options
            bmp = r.bitmap()
            self.bitmap_colorspaces[bmp["colorspace"]] = \
                self.bitmap_colorspaces.get(bmp["colorspace"], 0) + 1
            ox, oy = self._xy(st)
            dst = (view_rect[0] + ox, view_rect[1] + oy,
                   view_rect[2] + ox, view_rect[3] + oy)
            self._note_dest(st, *rect_to_pixels(dst))
            n, decoded = fb.draw_bitmap(bmp, bitmap_rect, dst)
            st.pixels_touched += n
            if decoded:
                self.bitmaps_decoded += 1
            else:
                self.bitmaps_placeholder += 1
                st.estimated_ops += 1
            return

        if code == RP_DRAW_BITMAP_RECTS:
            # RemoteDrawingEngine.cpp:365-380 -- token, uint32 options,
            # color_space, uint32 flags, int32 rectCount, then per rect:
            # BRect viewRect + AddBitmap(minimal=true).
            r.u32()                                  # options
            colorspace = r.u32()
            flags = r.u32()
            count = r.i32()
            self.bitmap_colorspaces[colorspace] = \
                self.bitmap_colorspaces.get(colorspace, 0) + 1
            for _ in range(max(0, min(count, 1 << 20))):
                view_rect = r.rect()
                bmp = r.bitmap(minimal=True, colorspace=colorspace,
                               flags=flags)
                ox, oy = self._xy(st)
                dst = (view_rect[0] + ox, view_rect[1] + oy,
                       view_rect[2] + ox, view_rect[3] + oy)
                self._note_dest(st, *rect_to_pixels(dst))
                # RemoteView.cpp:798 draws bitmap->Bounds() -> viewRect.
                src = (0.0, 0.0, float(bmp["width"] - 1),
                       float(bmp["height"] - 1))
                n, decoded = fb.draw_bitmap(bmp, src, dst)
                st.pixels_touched += n
                if decoded:
                    self.bitmaps_decoded += 1
                else:
                    self.bitmaps_placeholder += 1
                    st.estimated_ops += 1
            return

        # ---- pattern-approximated drawing ops ------------------------
        if code == RP_FILL_RECT:
            rect = r.rect()
            box = self._rect_px(st, rect)
            self._note_dest(st, *box)
            self._paint(st, "fill", box, st.effective_color())
            return

        if code == RP_STROKE_RECT:
            rect = r.rect()
            box = self._rect_px(st, rect)
            self._note_dest(st, *box)
            self._paint(st, "stroke", box, st.effective_color())
            return

        if code == RP_STROKE_LINE:
            p0 = r.point()
            p1 = r.point()
            ox, oy = self._xy(st)
            xa = int(round(p0[0])) + ox
            ya = int(round(p0[1])) + oy
            xb = int(round(p1[0])) + ox
            yb = int(round(p1[1])) + oy
            self._note_dest(st, min(xa, xb), min(ya, yb), max(xa, xb),
                            max(ya, yb))
            self._paint(st, "line", (xa, ya, xb, yb), st.effective_color())
            return

        if code in REGION_OPS:
            rects = r.region()
            color = (st.effective_color() if code == RP_FILL_REGION
                     else PLACEHOLDER)
            estimated = (code != RP_FILL_REGION)
            for rect in rects:
                box = self._rect_px(st, rect)
                self._note_dest(st, *box)
                self._paint(st, "fill", box, color, estimated)
            if estimated:
                st.estimated_ops += 1
                self.undecoded_drawing_ops += 1
                st.undecoded_drawing_ops += 1
            return

        # ---- text ----------------------------------------------------
        if code == RP_DRAW_STRING:
            # RemoteDrawingEngine.cpp:882-891 -- token, BPoint point,
            # AddString(string,length), bool hasDelta, [length escapement_delta]
            point = r.point()
            text = r.string()
            has_delta = r.bool8()
            if has_delta:
                # AddList(delta, length) where length is the *byte* length
                # (RemoteDrawingEngine.cpp:890); RemoteView.cpp:1308-1309 reads
                # the same count.  escapement_delta is 2 floats.
                for _ in range(len(text)):
                    r.f32(); r.f32()
            self._paint_text(st, point, text)
            if self.reply:
                self._reply_draw_string(token, point, text, st)
            return

        if code == RP_DRAW_STRING_WITH_OFFSETS:
            # RemoteDrawingEngine.cpp:916-921 -- token, AddString, then
            # UTF8CountChars(string) BPoints.
            text = r.string()
            count = utf8_count_chars(text)
            offsets = []
            for _ in range(count):
                try:
                    offsets.append(r.point())
                except Truncated:
                    break
            if offsets:
                self._paint_text(st, offsets[0], text, offsets=offsets)
                if self.reply:
                    self._reply_draw_string(token, offsets[-1], b"", st)
            return

        if code == RP_STRING_WIDTH:
            text = r.string()
            if self.reply:
                width = self._estimate_width(st, text)
                self.outbox.append((RP_STRING_WIDTH_RESULT,
                                    struct.pack("<If", token, width)))
            return

        if code == RP_READ_BITMAP:
            rect = r.rect()
            r.bool8()                                # drawCursor
            if self.reply:
                self._reply_read_bitmap(token, rect)
            return

        # ---- approximated geometry -----------------------------------
        if code in LEADING_RECT_OPS:
            rect = r.rect()
            box = self._rect_px(st, rect)
            self._note_dest(st, *box)
            self._paint(st, "fill", box, PLACEHOLDER, estimated=True)
            self.undecoded_drawing_ops += 1
            st.undecoded_drawing_ops += 1
            st.estimated_ops += 1
            return

        if code in POINT_LIST_OPS:
            n = POINT_LIST_OPS[code]
            pts = [r.point() for _ in range(n)]
            ox, oy = self._xy(st)
            xs = [int(round(p[0])) + ox for p in pts]
            ys = [int(round(p[1])) + oy for p in pts]
            box = (min(xs), min(ys), max(xs), max(ys))
            self._note_dest(st, *box)
            self._paint(st, "fill", box, PLACEHOLDER, estimated=True)
            self.undecoded_drawing_ops += 1
            st.undecoded_drawing_ops += 1
            st.estimated_ops += 1
            return

        # ---- anything left ------------------------------------------
        if code in DRAWING_OPS:
            self.undecoded_drawing_ops += 1
            st.undecoded_drawing_ops += 1
            self.undecoded_no_rect_ops += 1
        return

    # -- text helpers ----------------------------------------------------
    def _estimate_width(self, st, text):
        """Crude proportional-font advance estimate.  NOT a measurement."""
        size = st.font_size if st.font_size and st.font_size > 0 else 12.0
        return 0.55 * size * max(0, utf8_count_chars(text))

    def _paint_text(self, st, point, text, offsets=None):
        """Paint the ESTIMATED bounding box of a text run.

        We have no font rasteriser, so this box is a guess: its pixels are
        marked "estimated" and excluded from the strict black census."""
        size = st.font_size if st.font_size and st.font_size > 0 else 12.0
        ascent = 0.8 * size
        descent = 0.25 * size
        ox, oy = self._xy(st)
        if offsets:
            xs = [p[0] for p in offsets]
            ys = [p[1] for p in offsets]
            left = min(xs)
            right = max(xs) + 0.55 * size
            top = min(ys) - ascent
            bottom = max(ys) + descent
        else:
            left = point[0]
            right = point[0] + self._estimate_width(st, text)
            top = point[1] - ascent
            bottom = point[1] + descent
        box = rect_to_pixels((left, top, right, bottom))
        box = (box[0] + ox, box[1] + oy, box[2] + ox, box[3] + oy)
        self._note_dest(st, *box)
        self._paint(st, "fill", box, PLACEHOLDER, estimated=True)
        self.estimated_text_ops += 1
        st.estimated_ops += 1

    def _reply_draw_string(self, token, point, text, st):
        """RP_DRAW_STRING_RESULT: uint32 token, BPoint penLocation.

        Consumed by RemoteDrawingEngine::_DrawingEngineResult
        (RemoteDrawingEngine.cpp:1046-1056).  MUST be sent: DrawString blocks
        the app_server for up to 1 s waiting for it
        (RemoteDrawingEngine.cpp:899-905), so a silent client would itself
        suppress the drawing we are trying to measure."""
        pen_x = point[0] + self._estimate_width(st, text)
        self.outbox.append((RP_DRAW_STRING_RESULT,
                            struct.pack("<Iff", token, pen_x, point[1])))

    def _reply_read_bitmap(self, token, rect):
        """RP_READ_BITMAP_RESULT: uint32 token, then a non-minimal bitmap.

        Server side reads it with RemoteMessage::ReadBitmap (minimal=false),
        RemoteMessage.cpp:333-382, and waits up to 10 s
        (RemoteDrawingEngine.cpp:998-1002)."""
        x0, y0, x1, y1 = rect_to_pixels(rect)
        w = max(1, x1 - x0 + 1)
        h = max(1, y1 - y0 + 1)
        bpr = w * 4
        bits = bytearray(bpr * h)
        for y in range(h):
            sy = y0 + y
            for x in range(w):
                sx = x0 + x
                o = y * bpr + x * 4
                if 0 <= sx < self.width and 0 <= sy < self.height:
                    s = (sy * self.width + sx) * 3
                    bits[o] = self.fb.buf[s + 2]        # B
                    bits[o + 1] = self.fb.buf[s + 1]    # G
                    bits[o + 2] = self.fb.buf[s]        # R
                bits[o + 3] = 255
        payload = struct.pack("<IiiiIII", token, w, h, bpr, B_RGB32, 0,
                              len(bits)) + bytes(bits)
        self.outbox.append((RP_READ_BITMAP_RESULT, payload))


# ---------------------------------------------------------------------------
# Wire framing
# ---------------------------------------------------------------------------
HEADER = 6
MAX_MESSAGE = 64 * 1024 * 1024


def frame(code: int, payload: bytes = b"") -> bytes:
    return struct.pack("<HI", code, HEADER + len(payload)) + payload


# ---------------------------------------------------------------------------
# URP/1 stream compression (server -> client only)
#
# These deliberately do NOT start with "RP_": CODE_NAMES above is built by
# scanning globals() for that prefix, so an "RP_CAP_..." constant would be
# mistaken for an opcode and would shadow the opcode with the same value.
# ---------------------------------------------------------------------------
URP_PROTOCOL_VERSION = 1
CAP_STRING_WIDTH_REPLY = 1 << 0
CAP_COMPRESS_ZSTD = 1 << 1

# Must match REMOTE_SEGMENT_MAX_PAYLOAD / REMOTE_SEGMENT_MAX_VARINT_SIZE in
# src/servers/app/drawing/interface/remote/RemoteWireFormat.h.
SEGMENT_MAX_PAYLOAD = 64 * 1024 * 1024
SEGMENT_MAX_VARINT = 5


def segment_header(payload_length: int, raw: bool) -> bytes:
    """Encodes one segment header: LEB128 of (length << 1) | raw."""
    value = (payload_length << 1) | (1 if raw else 0)
    out = bytearray()
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def read_segment_header(buf):
    """Returns (consumed, payload_length, raw) or (0, None, None) when the
    header is still incomplete.  Raises ValueError when it cannot be one."""
    value = 0
    shift = 0
    for i, byte in enumerate(buf[:SEGMENT_MAX_VARINT]):
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            length = value >> 1
            if length > SEGMENT_MAX_PAYLOAD:
                raise ValueError("segment claims %d bytes" % length)
            return i + 1, length, bool(value & 1)
        shift += 7
    if len(buf) >= SEGMENT_MAX_VARINT:
        raise ValueError("segment header longer than %d bytes"
                         % SEGMENT_MAX_VARINT)
    return 0, None, None


def _load_libzstd():
    """Open libzstd.so.1 through ctypes, tolerating a hostile find_library().

    ctypes.util.find_library() is only a convenience and on Haiku it is an
    actively unreliable one: its POSIX implementation consults LIBRARY_PATH from
    the environment without a default, so in any process that does not happen to
    have that variable set -- which is every non-interactive one, including every
    command run over the management channel -- it raises KeyError before it ever
    looks at a directory. That surfaced as "no usable libzstd", i.e. as the
    absence of the very capability being measured, which is the most misleading
    failure this tool could have. So the soname is what we actually rely on and
    find_library is demoted to a hint that is allowed to fail.
    """
    import ctypes
    candidates = []
    try:
        import ctypes.util
        found = ctypes.util.find_library("zstd")
        if found:
            candidates.append(found)
    except Exception:
        pass
    # The versioned soname first: that is the name the server itself links
    # against, so if the server can compress, this name resolves.
    candidates += ["libzstd.so.1", "libzstd.so", "libzstd.so.1.5.6"]

    errors = []
    for name in candidates:
        try:
            return ctypes.CDLL(name)
        except OSError as error:
            errors.append("%s: %s" % (name, error))
    raise OSError("libzstd not loadable (%s)" % "; ".join(errors))


class ZstdStream(object):
    """Streaming zstd decompressor over libzstd through ctypes.

    ctypes rather than a Python module on purpose: neither `compression.zstd`
    (3.14+) nor the third-party `zstandard` is present on the images this tool
    runs against, while libzstd.so.1 is -- the system already depends on it.
    It also means this decoder is literally the same code the C client uses, so
    a disagreement here is a real protocol disagreement.
    """

    def __init__(self, window_log_max=20):
        import ctypes
        self._ctypes = ctypes
        self._lib = _load_libzstd()

        class InBuffer(ctypes.Structure):
            _fields_ = [("src", ctypes.c_void_p), ("size", ctypes.c_size_t),
                        ("pos", ctypes.c_size_t)]

        class OutBuffer(ctypes.Structure):
            _fields_ = [("dst", ctypes.c_void_p), ("size", ctypes.c_size_t),
                        ("pos", ctypes.c_size_t)]

        self._InBuffer = InBuffer
        self._OutBuffer = OutBuffer

        self._lib.ZSTD_createDCtx.restype = ctypes.c_void_p
        self._lib.ZSTD_freeDCtx.argtypes = [ctypes.c_void_p]
        self._lib.ZSTD_isError.restype = ctypes.c_uint
        self._lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
        self._lib.ZSTD_getErrorName.restype = ctypes.c_char_p
        self._lib.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
        self._lib.ZSTD_decompressStream.restype = ctypes.c_size_t
        self._lib.ZSTD_decompressStream.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(OutBuffer),
            ctypes.POINTER(InBuffer)]
        self._lib.ZSTD_DCtx_setParameter.restype = ctypes.c_size_t
        self._lib.ZSTD_DCtx_setParameter.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int]

        self._ctx = self._lib.ZSTD_createDCtx()
        if not self._ctx:
            raise RuntimeError("ZSTD_createDCtx failed")
        # 100 == ZSTD_d_windowLogMax
        self._check(self._lib.ZSTD_DCtx_setParameter(self._ctx, 100,
                                                     window_log_max))
        self._out = ctypes.create_string_buffer(64 * 1024)

    def _check(self, code):
        if self._lib.ZSTD_isError(code):
            raise ValueError("zstd: %s"
                             % self._lib.ZSTD_getErrorName(code).decode())
        return code

    def decompress(self, data: bytes) -> bytes:
        ctypes = self._ctypes
        src = ctypes.create_string_buffer(data, len(data))
        inbuf = self._InBuffer(ctypes.cast(src, ctypes.c_void_p), len(data), 0)
        produced = bytearray()
        while inbuf.pos < inbuf.size:
            outbuf = self._OutBuffer(ctypes.cast(self._out, ctypes.c_void_p),
                                     len(self._out), 0)
            before = inbuf.pos
            self._check(self._lib.ZSTD_decompressStream(
                self._ctx, ctypes.byref(outbuf), ctypes.byref(inbuf)))
            if outbuf.pos:
                produced += self._out.raw[:outbuf.pos]
            elif inbuf.pos == before:
                raise ValueError("zstd made no progress")
        return bytes(produced)

    def close(self):
        if getattr(self, "_ctx", None):
            self._lib.ZSTD_freeDCtx(self._ctx)
            self._ctx = None


def _selftest_encoder():
    """A compressor configured exactly as RemoteWireWriter configures its own.

    Only the self-test needs this -- the tool is a client and never compresses
    on the wire.  It exists so the decoder can be exercised against real zstd
    output produced with the same level and window, instead of against a
    fixture that would go stale the moment the server's settings changed.
    """
    import ctypes
    lib = _load_libzstd()

    class InBuffer(ctypes.Structure):
        _fields_ = [("src", ctypes.c_void_p), ("size", ctypes.c_size_t),
                    ("pos", ctypes.c_size_t)]

    class OutBuffer(ctypes.Structure):
        _fields_ = [("dst", ctypes.c_void_p), ("size", ctypes.c_size_t),
                    ("pos", ctypes.c_size_t)]

    lib.ZSTD_createCCtx.restype = ctypes.c_void_p
    lib.ZSTD_isError.restype = ctypes.c_uint
    lib.ZSTD_isError.argtypes = [ctypes.c_size_t]
    lib.ZSTD_getErrorName.restype = ctypes.c_char_p
    lib.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
    lib.ZSTD_CCtx_setParameter.restype = ctypes.c_size_t
    lib.ZSTD_CCtx_setParameter.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                           ctypes.c_int]
    lib.ZSTD_compressStream2.restype = ctypes.c_size_t
    lib.ZSTD_compressStream2.argtypes = [ctypes.c_void_p,
                                         ctypes.POINTER(OutBuffer),
                                         ctypes.POINTER(InBuffer),
                                         ctypes.c_int]

    def guard(code):
        if lib.ZSTD_isError(code):
            raise ValueError("zstd: %s" % lib.ZSTD_getErrorName(code).decode())
        return code

    ctx = lib.ZSTD_createCCtx()
    if not ctx:
        raise RuntimeError("ZSTD_createCCtx failed")
    guard(lib.ZSTD_CCtx_setParameter(ctx, 100, 1))     # compressionLevel = 1
    guard(lib.ZSTD_CCtx_setParameter(ctx, 101, 20))    # windowLog = 20
    out = ctypes.create_string_buffer(64 * 1024)

    def encode(data):
        src = ctypes.create_string_buffer(data, len(data))
        inbuf = InBuffer(ctypes.cast(src, ctypes.c_void_p), len(data), 0)
        produced = bytearray()
        while True:
            outbuf = OutBuffer(ctypes.cast(out, ctypes.c_void_p), len(out), 0)
            # 1 == ZSTD_e_flush: emit a decodable block boundary but keep the
            # window, which is where the cross-message ratio comes from.
            remaining = guard(lib.ZSTD_compressStream2(
                ctx, ctypes.byref(outbuf), ctypes.byref(inbuf), 1))
            if outbuf.pos:
                produced += out.raw[:outbuf.pos]
            if remaining == 0 and inbuf.pos == inbuf.size:
                return bytes(produced)

    return encode


class WireDecoder(object):
    """Turns the bytes coming off the socket back into the plain RP stream.

    Mirrors RemoteWireReader in the C client, including *why* it works this
    way: the switch-over point is found by watching the framing go past rather
    than by being told, because the negotiated capability set arrives inside
    RP_HELLO_ACK and the very next byte after that message is already a
    segment.  Deciding after the message had been parsed would be one message
    too late.
    """

    def __init__(self, capabilities):
        self.capabilities = capabilities
        self.compressed = False
        self.stream = None
        self.wire_bytes = 0          # bytes read from the socket
        self.plain_bytes = 0         # bytes handed to the RP framing layer
        self.raw_segments = 0        # exempt (already-compressed) segments
        self.compressed_segments = 0

        self._pending = bytearray()  # not yet consumed by the segment layer
        self._header = bytearray()   # partial RP header, plain phase only
        self._body_left = 0
        self._ack = bytearray()
        self._capturing_ack = False
        self._segment_left = 0
        self._segment_raw = False
        self._in_segment = False

    def feed(self, chunk: bytes) -> bytes:
        """Returns the plain RP bytes this chunk yielded."""
        self.wire_bytes += len(chunk)
        self._pending += chunk
        out = bytearray()
        while self._pending:
            if self.compressed:
                if not self._segments(out):
                    break
            else:
                if not self._plain(out):
                    break
        self.plain_bytes += len(out)
        return bytes(out)

    # -- still-plain stream: watch framing, look for RP_HELLO_ACK -------
    def _plain(self, out):
        # _body_left > 0 is the "inside a message body" state; anything else is
        # "accumulating the next 6-byte header".
        if self._body_left == 0:
            take = min(len(self._pending), HEADER - len(self._header))
            out += self._pending[:take]
            self._header += self._pending[:take]
            del self._pending[:take]
            if len(self._header) < HEADER:
                return False

            code, length = struct.unpack_from("<HI", self._header, 0)
            del self._header[:]
            if length < HEADER:
                raise ValueError("message claims %d bytes, less than a header"
                                 % length)
            self._body_left = length - HEADER
            self._capturing_ack = code == RP_HELLO_ACK
            del self._ack[:]
            if self._body_left == 0:
                # An empty message cannot be an acknowledgement.
                self._capturing_ack = False
            return True

        take = min(len(self._pending), self._body_left)
        out += self._pending[:take]
        if self._capturing_ack and len(self._ack) < 8:
            self._ack += self._pending[:min(take, 8 - len(self._ack))]
        del self._pending[:take]
        self._body_left -= take
        if self._body_left:
            return False

        if not self._capturing_ack:
            return True

        self._capturing_ack = False
        if len(self._ack) < 8:
            return True
        negotiated = struct.unpack_from("<II", self._ack, 0)[1]
        if negotiated & self.capabilities & CAP_COMPRESS_ZSTD:
            self.stream = ZstdStream()
            self.compressed = True
        return True

    # -- segmented stream ----------------------------------------------
    def _segments(self, out):
        if not self._in_segment:
            consumed, length, raw = read_segment_header(self._pending)
            if consumed == 0:
                return False
            del self._pending[:consumed]
            self._segment_left = length
            self._segment_raw = raw
            self._in_segment = True
            if raw:
                self.raw_segments += 1
            else:
                self.compressed_segments += 1
            if length == 0:
                self._in_segment = False
                return True

        if not self._pending:
            return False
        take = min(len(self._pending), self._segment_left)
        chunk = bytes(self._pending[:take])
        del self._pending[:take]
        self._segment_left -= take
        if self._segment_raw:
            out += chunk
        else:
            out += self.stream.decompress(chunk)
        if self._segment_left == 0:
            self._in_segment = False
        return True

    def close(self):
        if self.stream is not None:
            self.stream.close()
            self.stream = None


class Connection(object):
    def __init__(self, host, port, deadline, connect_timeout=5.0,
                 capabilities=0):
        self.deadline = deadline
        self.sock = socket.create_connection((host, port),
                                            timeout=connect_timeout)
        self.sock.settimeout(0.5)
        self.buf = bytearray()
        self.capabilities = capabilities
        # Always present, so the byte counters work in both arms of an A/B.
        # With no compression capability it is a pure passthrough.
        self.wire = WireDecoder(capabilities)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        self.wire.close()

    def send(self, data):
        try:
            self.sock.sendall(data)
            return True
        except OSError:
            return False

    def _ingest(self, chunk):
        """The single point where bytes off the wire become the plain RP
        stream.  Every transport (plain TCP, WebSocket) funnels through here so
        the compression layer is transport-independent -- and so the wire-byte
        counters mean the same thing in both."""
        self.buf += self.wire.feed(bytes(chunk))

    def _fill(self, want):
        """Read until self.buf has >= want bytes, the deadline passes, or EOF.
        Returns True on success."""
        while len(self.buf) < want:
            if time.monotonic() >= self.deadline:
                return False
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError as exc:
                if exc.errno in (errno.EINTR, errno.EAGAIN):
                    continue
                raise EOFError("socket error: %s" % exc)
            if not chunk:
                raise EOFError("peer closed")
            self._ingest(chunk)
        return True

    def next_message(self):
        """Returns (code, payload) or None on deadline/EOF."""
        if not self._fill(HEADER):
            return None
        code, length = struct.unpack_from("<HI", self.buf, 0)
        if length < HEADER or length > MAX_MESSAGE:
            raise ValueError("implausible message length %d for code %d (%s)"
                             % (length, code, code_name(code)))
        if not self._fill(length):
            return None
        payload = bytes(self.buf[HEADER:length])
        del self.buf[:length]
        return code, payload


class AuthenticationError(Exception):
    pass


class PinMismatchError(Exception):
    pass


class WssConnection(Connection):
    """Connection through the remote_broker daemon: TLS (trust = certificate
    pinning), a WebSocket client handshake, then RP_AUTHENTICATE before
    anything else.  The decapsulated WebSocket payload is the same RP byte
    stream Connection carries, so everything above this class is unchanged.
    """

    WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, host, port, deadline, connect_timeout=5.0,
                 token=None, pin=None, insecure=False, capabilities=0):
        self.deadline = deadline
        self.buf = bytearray()       # decoded RP byte stream
        self.capabilities = capabilities
        self.wire = WireDecoder(capabilities)
        self._wsbuf = bytearray()    # raw, still-framed WebSocket bytes
        self._frame_remaining = 0    # payload bytes left in the current frame
        self._frame_opcode = 0
        self._ctrl = bytearray()     # control-frame payload being assembled
        self._closed = False

        raw = socket.create_connection((host, port), timeout=connect_timeout)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE   # trust decision is the pin
        self.sock = context.wrap_socket(raw, server_hostname=host)

        fingerprint = hashlib.sha256(
            self.sock.getpeercert(binary_form=True)).hexdigest()
        if pin:
            expected = pin.lower().removeprefix("sha256:").replace(":", "")
            if not hmac_compare(expected, fingerprint):
                self.sock.close()
                raise PinMismatchError(
                    "server certificate sha256=%s does not match the pin"
                    % fingerprint)
        elif not insecure:
            self.sock.close()
            raise PinMismatchError(
                "no --pin given (server certificate sha256=%s); verify it "
                "out-of-band (broker.fingerprint on the server) and pass "
                "--pin, or pass --insecure" % fingerprint)
        self.server_fingerprint = fingerprint

        self.sock.settimeout(5.0)
        self._ws_handshake(host, port)
        if token is None:
            raise AuthenticationError("broker requires a token; pass "
                                      "--token/--token-file")
        self._authenticate(token)
        self.sock.settimeout(0.5)

    # -- WebSocket client plumbing ------------------------------------

    def _ws_handshake(self, host, port):
        key = base64.b64encode(os.urandom(16)).decode()
        request = ("GET / HTTP/1.1\r\n"
                   "Host: %s:%d\r\n"
                   "Upgrade: websocket\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\n"
                   "Sec-WebSocket-Version: 13\r\n"
                   "Sec-WebSocket-Protocol: binary\r\n"
                   "\r\n" % (host, port, key)).encode()
        self.sock.sendall(request)

        response = bytearray()
        while b"\r\n\r\n" not in response:
            if len(response) > 65536:
                raise EOFError("oversized upgrade response")
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("peer closed during WebSocket handshake")
            response += chunk
        headers_end = response.index(b"\r\n\r\n") + 4
        headers = response[:headers_end].decode("latin-1")
        self._wsbuf += response[headers_end:]

        if not headers.startswith("HTTP/1.1 101"):
            raise EOFError("WebSocket upgrade refused: %s"
                           % headers.splitlines()[0])
        expected = base64.b64encode(hashlib.sha1(
            (key + self.WS_GUID).encode()).digest()).decode()
        accept = ""
        for line in headers.split("\r\n"):
            name, _, value = line.partition(":")
            if name.strip().lower() == "sec-websocket-accept":
                accept = value.strip()
        if accept != expected:
            raise EOFError("Sec-WebSocket-Accept mismatch")

    def send(self, data):
        """Wraps the RP byte stream in one masked binary frame."""
        try:
            length = len(data)
            if length <= 125:
                header = struct.pack("<BB", 0x82, 0x80 | length)
            elif length <= 65535:
                header = struct.pack(">BBH", 0x82, 0x80 | 126, length)
            else:
                header = struct.pack(">BBQ", 0x82, 0x80 | 127, length)
            mask = os.urandom(4)
            masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            self.sock.sendall(header + mask + masked)
            return True
        except OSError:
            return False

    def _fill(self, want):
        """Decodes WebSocket frames into self.buf until it has >= want
        bytes, the deadline passes, or the connection ends."""
        while len(self.buf) < want:
            if self._decode_ws():
                continue
            if self._closed:
                raise EOFError("peer sent WebSocket close")
            if time.monotonic() >= self.deadline:
                return False
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError as exc:
                if exc.errno in (errno.EINTR, errno.EAGAIN):
                    continue
                raise EOFError("socket error: %s" % exc)
            if not chunk:
                raise EOFError("peer closed")
            self._wsbuf += chunk
        return True

    def _decode_ws(self):
        """Moves payload bytes from self._wsbuf into self.buf.  Returns True
        when any progress was made."""
        progress = False
        while True:
            if self._frame_remaining > 0:
                if not self._wsbuf:
                    return progress
                take = min(self._frame_remaining, len(self._wsbuf))
                chunk = self._wsbuf[:take]
                del self._wsbuf[:take]
                self._frame_remaining -= take
                if self._frame_opcode in (0x0, 0x2):
                    self._ingest(chunk)     # server frames are unmasked
                else:
                    self._ctrl += chunk
                    if self._frame_remaining == 0:
                        self._control_frame(self._frame_opcode,
                                            bytes(self._ctrl))
                        del self._ctrl[:]
                progress = True
                continue

            if len(self._wsbuf) < 2:
                return progress
            first, second = self._wsbuf[0], self._wsbuf[1]
            if second & 0x80:
                raise EOFError("masked frame from server")
            length = second & 0x7F
            header = 2
            if length == 126:
                if len(self._wsbuf) < 4:
                    return progress
                length = struct.unpack_from(">H", self._wsbuf, 2)[0]
                header = 4
            elif length == 127:
                if len(self._wsbuf) < 10:
                    return progress
                length = struct.unpack_from(">Q", self._wsbuf, 2)[0]
                header = 10
            opcode = first & 0x0F
            if opcode == 0x1:
                raise EOFError("unexpected text frame")
            if opcode & 0x8 and length == 0:
                self._control_frame(opcode, b"")
            del self._wsbuf[:header]
            if not (opcode & 0x8 and length == 0):
                self._frame_remaining = length
                self._frame_opcode = opcode
            progress = True

    def _control_frame(self, opcode, payload):
        if opcode == 0x9:                    # ping -> pong
            mask = os.urandom(4)
            masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
            self.sock.sendall(struct.pack("<BB", 0x8A, 0x80 | len(payload))
                              + mask + masked)
        elif opcode == 0x8:                  # close
            self._closed = True

    # -- authentication ------------------------------------------------

    def _authenticate(self, token):
        token_bytes = token.encode() if isinstance(token, str) else token
        payload = struct.pack("<II", 1, len(token_bytes)) + token_bytes
        if not self.send(frame(RP_AUTHENTICATE, payload)):
            raise AuthenticationError("failed to send RP_AUTHENTICATE")

        item = self.next_message()
        if item is None:
            raise AuthenticationError("no RP_AUTH_RESULT before deadline")
        code, payload = item
        if code != RP_AUTH_RESULT or len(payload) < 4:
            raise AuthenticationError("expected RP_AUTH_RESULT, got %s"
                                      % code_name(code))
        status = struct.unpack_from("<I", payload, 0)[0]
        if status != 0:
            raise AuthenticationError("broker denied the token (status %d)"
                                      % status)


def hmac_compare(a, b):
    import hmac as _hmac
    return _hmac.compare_digest(a.encode(), b.encode())


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

TR_W = 137
TR_H = 70


def topright_box(width, height):
    return (max(0, width - TR_W), 0, width - 1, min(height, TR_H) - 1)


def boxes_intersect(a, b):
    return not (a[2] < b[0] or b[2] < a[0] or a[3] < b[1] or b[3] < a[1])


def report(cap, args, connected, elapsed, stop_reason, wire=None):
    fb = cap.fb
    tr = topright_box(cap.width, cap.height)
    black_all, strict_all = fb.black_counts()
    black_tr, strict_tr = fb.black_counts(tr)

    tokens = sorted(cap.tokens.items(),
                    key=lambda kv: (kv[0] == NO_TOKEN_KEY, kv[0]))

    # ---- machine-readable ------------------------------------------
    out = []
    out.append("CONNECTED=%d" % (1 if connected else 0))
    out.append("MESSAGES=%d" % cap.messages)
    out.append("ELAPSED_S=%.2f" % elapsed)
    out.append("STOP_REASON=%s" % stop_reason)
    out.append("SCREEN=%dx%d" % (cap.width, cap.height))
    out.append("TOKENS=%d" % len([t for t, _ in tokens if t != NO_TOKEN_KEY]))
    out.append("DRAWING_OPS=%d" % sum(s.drawing_ops for _, s in tokens))
    out.append("STATE_OPS=%d" % sum(s.state_ops for _, s in tokens))
    out.append("OTHER_OPS=%d" % sum(s.other_ops for _, s in tokens))
    out.append("UNDECODED_DRAWING_OPS=%d" % cap.undecoded_drawing_ops)
    out.append("UNDECODED_NO_RECT_OPS=%d" % cap.undecoded_no_rect_ops)
    out.append("ESTIMATED_TEXT_OPS=%d" % cap.estimated_text_ops)
    out.append("BITMAPS_DECODED=%d" % cap.bitmaps_decoded)
    out.append("BITMAPS_PLACEHOLDER=%d" % cap.bitmaps_placeholder)
    out.append("CLIPPED_OUT_OPS=%d" % cap.clipped_out_ops)
    out.append("TRUNCATED_MESSAGES=%d" % cap.truncated)
    out.append("UNKNOWN_CODES=%s"
               % (",".join(str(c) for c in sorted(cap.unknown_codes)) or "-"))
    # Bytes on the wire vs bytes of RP protocol.  These are the A/B numbers:
    # WIRE_BYTES is what the link carried, PLAIN_BYTES what the parser saw.
    # Equal (bar the handshake) when nothing was negotiated.
    if wire is not None:
        out.append("WIRE_BYTES=%d" % wire.wire_bytes)
        out.append("PLAIN_BYTES=%d" % wire.plain_bytes)
        out.append("WIRE_COMPRESSED=%d" % (1 if wire.compressed else 0))
        out.append("WIRE_SEGMENTS_COMPRESSED=%d" % wire.compressed_segments)
        out.append("WIRE_SEGMENTS_RAW=%d" % wire.raw_segments)
        ratio = (float(wire.plain_bytes) / wire.wire_bytes
                 if wire.wire_bytes else 0.0)
        out.append("WIRE_RATIO=%.3f" % ratio)
    if cap.negotiated_capabilities is not None:
        out.append("NEGOTIATED_CAPABILITIES=%d" % cap.negotiated_capabilities)
        out.append("NEGOTIATED_VERSION=%d" % cap.negotiated_version)
    out.append("PIXELS_TOUCHED=%d" % sum(s.pixels_touched for _, s in tokens))
    out.append("BLACK_TOPRIGHT=%d" % black_tr)
    out.append("BLACK_SCREEN=%d" % black_all)
    out.append("BLACK_TOPRIGHT_STRICT=%d" % strict_tr)
    out.append("BLACK_SCREEN_STRICT=%d" % strict_all)
    out.append("TOPRIGHT_REGION=%d,%d,%d,%d" % tr)
    out.append("TOPRIGHT_PIXELS=%d"
               % ((tr[2] - tr[0] + 1) * (tr[3] - tr[1] + 1)))

    tr_tokens = []
    for token, st in tokens:
        if st.bbox is not None and boxes_intersect(st.bbox, tr):
            tr_tokens.append((token, st))
    out.append("TOPRIGHT_TOKENS=%s"
               % (",".join(("notoken" if t == NO_TOKEN_KEY else str(t))
                           for t, _ in tr_tokens) or "-"))
    out.append("TOPRIGHT_DRAWERS=%d" % len(tr_tokens))
    print("\n".join(out))

    # ---- human summary --------------------------------------------
    print("")
    print("=== rdcapture summary ===================================")
    print("target        : %s:%d  (%dx%d)" % (args.host, args.port, cap.width,
                                              cap.height))
    print("connected     : %s   messages=%d   elapsed=%.2fs   stop=%s"
          % (connected, cap.messages, elapsed, stop_reason))
    if wire is not None:
        print("wire          : %s  on-wire=%d B  protocol=%d B  ratio=%.3fx  "
              "segments=%d+%draw"
              % ("zstd" if wire.compressed else "plain (nothing negotiated)",
                 wire.wire_bytes, wire.plain_bytes,
                 (float(wire.plain_bytes) / wire.wire_bytes
                  if wire.wire_bytes else 0.0),
                 wire.compressed_segments, wire.raw_segments))
    print("offsets applied as translation: %s (see OFFSETS note in source)"
          % ("YES" if cap.apply_offsets else "no"))
    print("clipping applied              : %s"
          % ("yes" if cap.do_clip else "no"))
    if cap.errors:
        print("decode complaints (first %d): %s"
              % (len(cap.errors), "; ".join(cap.errors)))

    print("")
    print("--- per token -------------------------------------------")
    if not tokens:
        print("(no tokens seen)")
    for token, st in tokens:
        name = "no-token ops" if token == NO_TOKEN_KEY else "token %d" % token
        bd = st.breakdown()
        offs = st.offsets
        if not offs:
            offs_desc = "(none seen)"
        else:
            uniq = []
            for o in offs:
                if not uniq or uniq[-1] != o:
                    uniq.append(o)
            offs_desc = " ".join("(%d,%d)" % o for o in uniq[:12])
            if len(uniq) > 12:
                offs_desc += " ... %d distinct-runs total" % len(uniq)
        print("%-14s created=%s deleted=%s" % (name, st.created, st.deleted))
        print("    offsets   : %s" % offs_desc)
        print("    drawing=%-6d state=%-6d other=%-4d undecoded_drawing=%d"
              % (st.drawing_ops, st.state_ops, st.other_ops,
                 st.undecoded_drawing_ops))
        print("    breakdown : DRAW_STRING=%d DRAW_BITMAP=%d FILL=%d "
              "STROKE=%d INVERT=%d"
              % (bd["draw_string"], bd["draw_bitmap"], bd["fill"],
                 bd["stroke"], bd["invert"]))
        print("    bbox      : %s   pixels_touched=%d"
              % ("none" if st.bbox is None
                 else "(%d,%d)-(%d,%d)" % tuple(st.bbox), st.pixels_touched))
        tops = sorted(st.op_counts.items(), key=lambda kv: -kv[1])[:8]
        print("    top ops   : %s"
              % (", ".join("%s=%d" % (code_name(c), n) for c, n in tops)
                 or "-"))

    print("")
    print("--- TOP-RIGHT REGION verdict ----------------------------")
    print("region        : x in [%d, %d), y in [0, %d)  -> box (%d,%d)-(%d,%d)"
          % (max(0, cap.width - TR_W), cap.width, min(cap.height, TR_H),
             tr[0], tr[1], tr[2], tr[3]))
    if not tr_tokens:
        print("NO token drew anything intersecting the top-right region.")
    else:
        for token, st in tr_tokens:
            name = "no-token" if token == NO_TOKEN_KEY else "token %d" % token
            print("  %-12s drawing_ops=%-6d bbox=(%d,%d)-(%d,%d)"
                  % (name, st.drawing_ops, st.bbox[0], st.bbox[1], st.bbox[2],
                     st.bbox[3]))
    total_tr = (tr[2] - tr[0] + 1) * (tr[3] - tr[1] + 1)
    print("pixels        : %d total, %d pure black (%.1f%%), %d black-or-guessed"
          % (total_tr, black_tr,
             100.0 * black_tr / total_tr if total_tr else 0.0, strict_tr))
    print("whole screen  : %d total, %d pure black (%.1f%%), %d black-or-guessed"
          % (cap.width * cap.height, black_all,
             100.0 * black_all / (cap.width * cap.height), strict_all))
    if cap.bitmaps_placeholder:
        print("NOTE: %d bitmap(s) had a colour space we do not decode "
              "(%s); their destination rects were painted flat %s instead of "
              "real pixels."
              % (cap.bitmaps_placeholder,
                 ",".join("0x%04x" % cs for cs in
                          sorted(cap.bitmap_colorspaces)),
                 str(PLACEHOLDER)))
    if cap.estimated_text_ops:
        print("NOTE: %d text run(s) have no glyph rasteriser here; an "
              "ESTIMATED bounding box was painted flat %s.  Those pixels are "
              "excluded from the *_STRICT counts."
              % (cap.estimated_text_ops, str(PLACEHOLDER)))
    if cap.undecoded_drawing_ops:
        print("NOTE: %d drawing op(s) were not rasterised properly; where a "
              "destination rect was decodable it was painted flat %s "
              "(%d had no decodable rect at all)."
              % (cap.undecoded_drawing_ops, str(PLACEHOLDER),
                 cap.undecoded_no_rect_ops))
    print("=========================================================")

    if args.json:
        blob = {
            "connected": connected,
            "messages": cap.messages,
            "elapsed_s": elapsed,
            "stop_reason": stop_reason,
            "screen": [cap.width, cap.height],
            "apply_offsets": cap.apply_offsets,
            "clipping": cap.do_clip,
            "black_screen": black_all,
            "black_screen_strict": strict_all,
            "black_topright": black_tr,
            "black_topright_strict": strict_tr,
            "topright_region": list(tr),
            "topright_tokens": [("notoken" if t == NO_TOKEN_KEY else t)
                                for t, _ in tr_tokens],
            "wire": (None if wire is None else {
                "wire_bytes": wire.wire_bytes,
                "plain_bytes": wire.plain_bytes,
                "compressed": wire.compressed,
                "segments_compressed": wire.compressed_segments,
                "segments_raw": wire.raw_segments,
                "ratio": (float(wire.plain_bytes) / wire.wire_bytes
                          if wire.wire_bytes else 0.0),
            }),
            "negotiated_capabilities": cap.negotiated_capabilities,
            "undecoded_drawing_ops": cap.undecoded_drawing_ops,
            "undecoded_no_rect_ops": cap.undecoded_no_rect_ops,
            "estimated_text_ops": cap.estimated_text_ops,
            "bitmaps_decoded": cap.bitmaps_decoded,
            "bitmaps_placeholder": cap.bitmaps_placeholder,
            "bitmap_colorspaces": {"0x%04x" % k: v for k, v
                                   in cap.bitmap_colorspaces.items()},
            "clipped_out_ops": cap.clipped_out_ops,
            "truncated_messages": cap.truncated,
            "unknown_codes": sorted(cap.unknown_codes),
            "global_op_counts": {code_name(c): n for c, n
                                 in sorted(cap.global_counts.items())},
            "tokens": [],
            "errors": cap.errors,
        }
        for token, st in tokens:
            blob["tokens"].append({
                "token": ("notoken" if token == NO_TOKEN_KEY else token),
                "created": st.created,
                "deleted": st.deleted,
                "offsets": [list(o) for o in st.offsets],
                "drawing_ops": st.drawing_ops,
                "state_ops": st.state_ops,
                "other_ops": st.other_ops,
                "undecoded_drawing_ops": st.undecoded_drawing_ops,
                "estimated_ops": st.estimated_ops,
                "pixels_touched": st.pixels_touched,
                "bbox": st.bbox,
                "breakdown": st.breakdown(),
                "op_counts": {code_name(c): n for c, n
                              in sorted(st.op_counts.items())},
            })
        with open(args.json, "w") as f:
            json.dump(blob, f, indent=2, sort_keys=True)
        print("wrote JSON: %s" % args.json)

    if args.png:
        write_png(args.png, cap.width, cap.height, bytes(fb.buf))
        print("wrote PNG : %s" % args.png)


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def selftest():
    """Synthesise a byte stream and push it through the real parser.

    This is the "does the instrument work" control: without it a zero result
    is indistinguishable from a broken decoder."""
    import os
    import tempfile

    width, height = 200, 120
    failures = []

    def check(label, cond, detail=""):
        if cond:
            print("  ok    %s" % label)
        else:
            print("  FAIL  %s %s" % (label, detail))
            failures.append(label)

    stream = bytearray()

    def add(code, payload=b""):
        stream.extend(frame(code, payload))

    token = 7
    add(RP_CREATE_STATE, struct.pack("<I", token))
    add(RP_SET_OFFSETS, struct.pack("<Iii", token, 5, 9))

    # 1. RP_FILL_RECT_COLOR: token, BRect, rgb_color  -> 10x10 = 100 px
    add(RP_FILL_RECT_COLOR,
        struct.pack("<Iffff", token, 10.0, 10.0, 19.0, 19.0)
        + bytes((0, 0, 255, 255)))

    # 2. RP_STROKE_RECT_1PX_COLOR -> perimeter of a 10x10 box = 36 px
    add(RP_STROKE_RECT_1PX_COLOR,
        struct.pack("<Iffff", token, 40.0, 10.0, 49.0, 19.0)
        + bytes((0, 255, 0, 255)))

    # 3. RP_STROKE_LINE_1PX_COLOR: horizontal, 10 px
    add(RP_STROKE_LINE_1PX_COLOR,
        struct.pack("<Iffff", token, 60.0, 30.0, 69.0, 30.0)
        + bytes((255, 255, 0, 255)))

    # 4. RP_SET_HIGH_COLOR + RP_FILL_RECT (pattern/high-colour path) -> 25 px
    add(RP_SET_HIGH_COLOR, struct.pack("<I", token) + bytes((11, 22, 33, 255)))
    add(RP_FILL_RECT, struct.pack("<Iffff", token, 80.0, 40.0, 84.0, 44.0))

    # 5. RP_FILL_REGION: token + region of two 5x5 rects -> 50 px
    add(RP_FILL_REGION,
        struct.pack("<Ii", token, 2)
        + struct.pack("<ffff", 100.0, 40.0, 104.0, 44.0)
        + struct.pack("<ffff", 110.0, 40.0, 114.0, 44.0))

    # 6. RP_FILL_REGION_COLOR_NO_CLIPPING: NO token -> region + colour, 20 px
    add(RP_FILL_REGION_COLOR_NO_CLIPPING,
        struct.pack("<i", 1)
        + struct.pack("<ffff", 0.0, 100.0, 19.0, 100.0)
        + bytes((5, 6, 7, 255)))

    # 7. RP_DRAW_BITMAP, B_RGB32 4x4 all opaque white -> 16 px into the
    #    top-right region so the verdict has something to find.
    bw = bh = 4
    bits = bytes([255, 255, 255, 255]) * (bw * bh)
    bmp = struct.pack("<iiiIII", bw, bh, bw * 4, B_RGB32, 0, len(bits)) + bits
    add(RP_DRAW_BITMAP,
        struct.pack("<I", token)
        + struct.pack("<ffff", 0.0, 0.0, 3.0, 3.0)          # bitmapRect
        + struct.pack("<ffff", float(width - 4), 2.0,
                      float(width - 1), 5.0)                # viewRect
        + struct.pack("<I", 0) + bmp)

    # 8. RP_DRAW_BITMAP with an undecoded colour space (B_CMAP8) -> placeholder
    cbits = bytes([9]) * 16
    cbmp = struct.pack("<iiiIII", 4, 4, 4, B_CMAP8, 0, len(cbits)) + cbits
    add(RP_DRAW_BITMAP,
        struct.pack("<I", token)
        + struct.pack("<ffff", 0.0, 0.0, 3.0, 3.0)
        + struct.pack("<ffff", 150.0, 90.0, 153.0, 93.0)
        + struct.pack("<I", 0) + cbmp)

    # 9. An unknown code, and an undecoded-but-bounded drawing op.
    add(RP_FILL_ELLIPSE, struct.pack("<Iffff", token, 20.0, 60.0, 29.0, 69.0))
    add(31337, b"\x01\x02\x03\x04")

    # 10. RP_DRAW_STRING (estimated text box + reply generation)
    text = b"Hi"
    add(RP_DRAW_STRING,
        struct.pack("<I", token) + struct.pack("<ff", 30.0, 90.0)
        + struct.pack("<I", len(text)) + text + b"\x00")

    # ---- run it through the framing + parser -----------------------
    cap = Capture(width, height, clip=True, apply_offsets=False, verbose=False,
                  reply=True)
    total_px = width * height
    black0, _ = cap.fb.black_counts()
    check("framebuffer starts all black", black0 == total_px,
          "(%d != %d)" % (black0, total_px))

    pos = 0
    parsed = 0
    while pos + HEADER <= len(stream):
        code, length = struct.unpack_from("<HI", stream, pos)
        assert length >= HEADER and length <= MAX_MESSAGE, length
        payload = bytes(stream[pos + HEADER:pos + length])
        check_len = len(payload) == length - HEADER
        if not check_len:
            failures.append("framing length for code %d" % code)
        cap.handle(code, payload)
        pos += length
        parsed += 1
    check("consumed the whole synthetic stream", pos == len(stream),
          "(%d of %d bytes)" % (pos, len(stream)))
    check("parsed every message", parsed == cap.messages,
          "(%d vs %d)" % (parsed, cap.messages))
    check("no truncated payloads", cap.truncated == 0,
          "errors=%s" % cap.errors)

    black1, strict1 = cap.fb.black_counts()
    check("black count changed", black1 < black0,
          "(%d -> %d)" % (black0, black1))

    # exact geometry checks on the fully decoded ops
    def px(x, y):
        o = (y * width + x) * 3
        return (cap.fb.buf[o], cap.fb.buf[o + 1], cap.fb.buf[o + 2])

    check("FILL_RECT_COLOR painted its interior", px(15, 15) == (0, 0, 255),
          str(px(15, 15)))
    check("FILL_RECT_COLOR did not bleed", px(20, 15) == (0, 0, 0),
          str(px(20, 15)))
    check("STROKE_RECT_1PX_COLOR drew the edge", px(40, 10) == (0, 255, 0),
          str(px(40, 10)))
    check("STROKE_RECT_1PX_COLOR left the middle black",
          px(45, 15) == (0, 0, 0), str(px(45, 15)))
    check("STROKE_LINE_1PX_COLOR drew", px(65, 30) == (255, 255, 0),
          str(px(65, 30)))
    check("FILL_RECT used the token high colour", px(82, 42) == (11, 22, 33),
          str(px(82, 42)))
    check("FILL_REGION painted rect 0", px(102, 42) == (11, 22, 33),
          str(px(102, 42)))
    check("FILL_REGION painted rect 1", px(112, 42) == (11, 22, 33),
          str(px(112, 42)))
    check("FILL_REGION_COLOR_NO_CLIPPING (token-less) painted",
          px(10, 100) == (5, 6, 7), str(px(10, 100)))
    check("DRAW_BITMAP B_RGB32 painted real pixels",
          px(width - 2, 3) == (255, 255, 255), str(px(width - 2, 3)))
    check("DRAW_BITMAP unknown colour space -> placeholder",
          px(151, 91) == PLACEHOLDER, str(px(151, 91)))
    check("undecoded bounded op -> placeholder", px(25, 65) == PLACEHOLDER,
          str(px(25, 65)))
    check("estimated text box -> placeholder", px(31, 88) == PLACEHOLDER,
          str(px(31, 88)))

    # accounting
    check("one bitmap placeholder recorded", cap.bitmaps_placeholder == 1,
          str(cap.bitmaps_placeholder))
    check("one bitmap decoded", cap.bitmaps_decoded == 1,
          str(cap.bitmaps_decoded))
    check("unknown code recorded", 31337 in cap.unknown_codes,
          str(cap.unknown_codes))
    check("token-less ops bucketed separately", NO_TOKEN_KEY in cap.tokens,
          str(sorted(cap.tokens)))
    check("token %d seen and created" % token,
          token in cap.tokens and cap.tokens[token].created)
    check("offsets recorded", cap.tokens[token].offsets == [(5, 9)],
          str(cap.tokens[token].offsets))
    check("undecoded drawing ops counted", cap.undecoded_drawing_ops >= 1,
          str(cap.undecoded_drawing_ops))
    check("estimated text ops counted", cap.estimated_text_ops == 1,
          str(cap.estimated_text_ops))
    check("strict black >= black", strict1 >= black1,
          "(%d vs %d)" % (strict1, black1))
    check("strict black excludes the guessed pixels", strict1 > black1,
          "(%d vs %d)" % (strict1, black1))

    # replies that keep the server from stalling
    reply_codes = [c for c, _ in cap.outbox]
    check("DRAW_STRING generated RP_DRAW_STRING_RESULT",
          RP_DRAW_STRING_RESULT in reply_codes, str(reply_codes))

    # exact pixel arithmetic on the simple fills
    painted = total_px - black1
    expected_min = 100 + 36 + 10 + 25 + 50 + 20 + 16 + 16
    check("painted pixel count is at least the sum of the exact fills",
          painted >= expected_min, "(%d < %d)" % (painted, expected_min))

    # top-right verdict must find the bitmap we planted there
    tr = topright_box(width, height)
    hits = [t for t, s in cap.tokens.items()
            if s.bbox is not None and boxes_intersect(s.bbox, tr)]
    check("top-right verdict found the planted bitmap", token in hits,
          str(hits))

    # ---- PNG writer round trip -------------------------------------
    tmpdir = tempfile.mkdtemp(prefix="rdcapture-selftest-")
    png_path = os.path.join(tmpdir, "selftest.png")
    write_png(png_path, width, height, bytes(cap.fb.buf))
    with open(png_path, "rb") as f:
        data = f.read()
    check("PNG signature", data[:8] == b"\x89PNG\r\n\x1a\n")
    # walk the chunks, verify CRCs, and re-inflate IDAT
    off = 8
    chunks = {}
    idat = b""
    crc_ok = True
    while off + 8 <= len(data):
        clen = struct.unpack_from(">I", data, off)[0]
        ctag = data[off + 4:off + 8]
        cdata = data[off + 8:off + 8 + clen]
        ccrc = struct.unpack_from(">I", data, off + 8 + clen)[0]
        if zlib.crc32(ctag + cdata) & 0xFFFFFFFF != ccrc:
            crc_ok = False
        chunks[ctag] = cdata
        if ctag == b"IDAT":
            idat += cdata
        off += 12 + clen
    check("PNG chunk CRCs", crc_ok)
    check("PNG has IHDR/IDAT/IEND",
          all(t in chunks for t in (b"IHDR", b"IDAT", b"IEND")),
          str(sorted(chunks)))
    ihdr = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    check("PNG IHDR is %dx%d 8-bit RGB" % (width, height),
          ihdr[:5] == (width, height, 8, 2, 0), str(ihdr))
    raw = zlib.decompress(idat)
    check("PNG raw size is height*(1+width*3)",
          len(raw) == height * (1 + width * 3),
          "(%d vs %d)" % (len(raw), height * (1 + width * 3)))
    # a known pixel must survive the round trip
    row = 15
    rowstart = row * (1 + width * 3)
    check("PNG filter byte is 0", raw[rowstart] == 0, str(raw[rowstart]))
    o = rowstart + 1 + 15 * 3
    check("PNG pixel round trip", tuple(raw[o:o + 3]) == (0, 0, 255),
          str(tuple(raw[o:o + 3])))
    try:
        os.unlink(png_path)
        os.rmdir(tmpdir)
    except OSError:
        pass

    # ---- URP/1 wire compression -------------------------------------
    print("  -- wire compression --")

    # Segment header round trip, including the boundaries where the varint
    # grows a byte.  This is the part a fixed-size header would get wrong.
    hdr_ok = True
    for length in (0, 1, 13, 63, 64, 127, 128, 8191, 8192, 1 << 20,
                   SEGMENT_MAX_PAYLOAD):
        for raw in (False, True):
            enc = segment_header(length, raw)
            consumed, got_len, got_raw = read_segment_header(enc)
            if (consumed, got_len, got_raw) != (len(enc), length, raw):
                hdr_ok = False
            if len(enc) > SEGMENT_MAX_VARINT:
                hdr_ok = False
            # A partial header must report "incomplete", never guess.
            if len(enc) > 1 and read_segment_header(enc[:-1])[0] != 0:
                hdr_ok = False
    check("segment header round trip", hdr_ok)
    check("small messages are not made bigger",
          len(segment_header(14, False)) == 1)

    # An unnegotiated decoder is a byte-for-byte passthrough: the legacy
    # guarantee, asserted rather than assumed.
    legacy = WireDecoder(0)
    plain = bytes(stream)
    passthrough = bytearray()
    for i in range(0, len(plain), 7):        # deliberately awkward chunking
        passthrough += legacy.feed(plain[i:i + 7])
    check("unnegotiated stream is byte-identical",
          bytes(passthrough) == plain,
          "(%d vs %d bytes)" % (len(passthrough), len(plain)))
    check("unnegotiated wire counters agree",
          legacy.wire_bytes == legacy.plain_bytes == len(plain))

    # Now the negotiated path, driven end to end through a real libzstd
    # compressor configured the way RemoteWireWriter configures it.
    try:
        enc = _selftest_encoder()
    except Exception as exc:                 # noqa: BLE001 - reported, not raised
        print("  skip  compressed round trip (no usable libzstd: %s)" % exc)
        enc = None

    if enc is not None:
        ack = frame(RP_HELLO_ACK, struct.pack("<II", URP_PROTOCOL_VERSION,
                                              CAP_COMPRESS_ZSTD))
        # Messages after the acknowledgement: two ordinary ones, then an
        # exempt (already-compressed) one that must travel as a raw segment.
        tail = [frame(RP_FILL_RECT_COLOR,
                      struct.pack("<Iffff", token, 1.0, 1.0, 9.0, 9.0)
                      + bytes((7, 7, 7, 255))),
                frame(RP_FILL_RECT_COLOR,
                      struct.pack("<Iffff", token, 1.0, 1.0, 9.0, 9.0)
                      + bytes((7, 7, 7, 255))),
                frame(RP_CODEC_TILE, os.urandom(512))]

        wire = bytearray(ack)
        for i, msg in enumerate(tail):
            exempt = i == len(tail) - 1
            if exempt:
                wire += segment_header(len(msg), True) + msg
            else:
                body = enc(msg)
                wire += segment_header(len(body), False) + body

        dec = WireDecoder(CAP_COMPRESS_ZSTD)
        recovered = bytearray()
        for i in range(0, len(wire), 5):      # split segments and headers
            recovered += dec.feed(bytes(wire[i:i + 5]))
        expect = ack + b"".join(tail)
        check("compressed stream round trip", bytes(recovered) == expect,
              "(%d vs %d bytes)" % (len(recovered), len(expect)))
        check("decoder switched at the acknowledgement", dec.compressed)
        check("exempt payload travelled as a raw segment",
              dec.raw_segments == 1 and dec.compressed_segments == 2,
              "(raw=%d compressed=%d)"
              % (dec.raw_segments, dec.compressed_segments))
        check("repeated messages compress",
              dec.wire_bytes < dec.plain_bytes,
              "(wire=%d plain=%d)" % (dec.wire_bytes, dec.plain_bytes))
        dec.close()

    print("")
    if failures:
        print("SELFTEST=FAIL  (%d checks failed: %s)"
              % (len(failures), ", ".join(failures)))
        return 1
    print("SELFTEST=PASS")
    print("BLACK_BEFORE=%d" % black0)
    print("BLACK_AFTER=%d" % black1)
    print("BLACK_AFTER_STRICT=%d" % strict1)
    print("PIXELS_PAINTED=%d" % painted)
    return 0


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(
        description="Minimal Haiku app_server remote-desktop protocol client "
                    "that measures whether anything is actually drawing.",
        epilog="Exits 0 with a summary even when nothing arrives; grep "
               "CONNECTED=, MESSAGES=, BLACK_TOPRIGHT=, BLACK_SCREEN=.")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=None,
                   help="default 10900 (app_server), or 10902 with --wss "
                        "(remote_broker)")
    p.add_argument("--width", type=int, default=1200)
    p.add_argument("--height", type=int, default=760)
    p.add_argument("--seconds", type=float, default=25.0,
                   help="overall wall-clock budget (default 25)")
    p.add_argument("--connect-timeout", type=float, default=5.0)
    p.add_argument("--png", help="write the framebuffer here as 8-bit RGB PNG")
    p.add_argument("--json", help="write the full summary here as JSON")
    p.add_argument("--verbose", action="store_true",
                   help="log every message to stderr")
    p.add_argument("--no-clip", dest="clip", action="store_false",
                   help="ignore RP_CONSTRAIN_CLIPPING_REGION when painting")
    p.add_argument("--apply-offsets", action="store_true",
                   help="translate op coordinates by the token's RP_SET_OFFSETS."
                        " OFF by default because both reference clients show "
                        "coordinates are already screen-absolute; see the "
                        "OFFSETS note in the source.")
    p.add_argument("--no-reply", dest="reply", action="store_false",
                   help="do not answer RP_DRAW_STRING / RP_STRING_WIDTH / "
                        "RP_READ_BITMAP (WARNING: makes the server block ~1s "
                        "per string, which suppresses the drawing you are "
                        "trying to measure)")
    p.add_argument("--zstd", action="store_true",
                   help="send RP_HELLO advertising zstd stream compression and "
                        "decode the compressed wire.  WITHOUT this flag no "
                        "RP_HELLO is sent at all, so the server has to serve "
                        "the legacy uncompressed stream -- which is what makes "
                        "the two invocations a clean A/B.")
    p.add_argument("--selftest", action="store_true",
                   help="run the parser/PNG self-test and exit")
    p.add_argument("--wss", action="store_true",
                   help="connect through the remote_broker daemon: TLS + "
                        "WebSocket + RP_AUTHENTICATE (implies --port 10902 "
                        "unless --port is given)")
    p.add_argument("--token", help="authentication token for --wss")
    p.add_argument("--token-file",
                   help="file containing the authentication token for --wss")
    p.add_argument("--pin",
                   help="pin the broker certificate to this SHA-256 hex "
                        "fingerprint (see broker.fingerprint on the server)")
    p.add_argument("--insecure", action="store_true",
                   help="with --wss: skip certificate pinning (still TLS)")
    args = p.parse_args(argv)

    if args.port is None:
        args.port = 10902 if args.wss else 10900

    if args.selftest:
        print("rdcapture selftest")
        return selftest()

    if args.width <= 0 or args.height <= 0:
        p.error("--width/--height must be positive")

    cap = Capture(args.width, args.height, clip=args.clip,
                  apply_offsets=args.apply_offsets, verbose=args.verbose,
                  reply=args.reply)

    started = time.monotonic()
    deadline = started + max(0.1, args.seconds)
    connected = False
    stop_reason = "unknown"
    conn = None

    token = args.token
    if args.token_file:
        try:
            with open(args.token_file) as f:
                token = f.read().strip()
        except OSError as exc:
            sys.stderr.write("rdcapture: cannot read token file: %s\n" % exc)
            return 2

    capabilities = CAP_COMPRESS_ZSTD if args.zstd else 0

    try:
        if args.wss:
            conn = WssConnection(args.host, args.port, deadline,
                                 connect_timeout=args.connect_timeout,
                                 token=token, pin=args.pin,
                                 insecure=args.insecure,
                                 capabilities=capabilities)
        else:
            conn = Connection(args.host, args.port, deadline,
                              connect_timeout=args.connect_timeout,
                              capabilities=capabilities)
    except AuthenticationError as exc:
        stop_reason = "auth_denied:%s" % exc
        sys.stderr.write("rdcapture: authentication failed: %s\n" % exc)
        print("AUTH=DENIED")
        report(cap, args, False, time.monotonic() - started, stop_reason)
        return 3
    except PinMismatchError as exc:
        stop_reason = "pin_mismatch"
        sys.stderr.write("rdcapture: %s\n" % exc)
        report(cap, args, False, time.monotonic() - started, stop_reason)
        return 4
    except (OSError, EOFError, ssl.SSLError) as exc:
        stop_reason = "connect_failed:%s" % (getattr(exc, "strerror", None)
                                             or exc)
        sys.stderr.write("rdcapture: connect to %s:%d failed: %s\n"
                         % (args.host, args.port, exc))
        report(cap, args, False, time.monotonic() - started, stop_reason)
        return 0

    if args.wss:
        print("AUTH=OK")
        print("TLS_PEER_SHA256=%s" % conn.server_fingerprint)

    connected = True
    hello = b""
    if args.zstd:
        # uint32 version, capabilities, max decode width/height (no Tier P),
        # then our screen size -- the same layout RemoteView.cpp sends.
        hello = frame(RP_HELLO, struct.pack("<IIIIII", URP_PROTOCOL_VERSION,
                                            capabilities, 0, 0,
                                            args.width, args.height))
    if not conn.send(frame(RP_INIT_CONNECTION) + hello):
        stop_reason = "send_init_failed"
    else:
        try:
            while time.monotonic() < deadline:
                item = conn.next_message()
                if item is None:
                    stop_reason = "deadline"
                    break
                code, payload = item
                cap.handle(code, payload)
                if cap.outbox:
                    pending = cap.outbox
                    cap.outbox = []
                    blob = b"".join(frame(c, pl) for c, pl in pending)
                    if not conn.send(blob):
                        stop_reason = "send_failed"
                        break
                if code == RP_CLOSE_CONNECTION:
                    stop_reason = "server_closed_connection"
                    break
            else:
                stop_reason = "deadline"
        except EOFError as exc:
            stop_reason = "eof:%s" % exc
        except ValueError as exc:
            stop_reason = "bad_framing"
            sys.stderr.write("rdcapture: %s -- stopping cleanly\n" % exc)
        except KeyboardInterrupt:
            stop_reason = "interrupted"
    wire = conn.wire if conn is not None else None
    if conn is not None:
        conn.close()

    report(cap, args, connected, time.monotonic() - started, stop_reason,
           wire=wire)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
