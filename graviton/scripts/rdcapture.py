#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""rdcapture.py -- minimal client for Haiku app_server's "remote desktop"
drawing protocol, used as a *measuring instrument*: it connects, drives the
handshake, decodes the drawing op stream, rasterises what it can into a
software framebuffer and reports per-token op counts plus a pure-black pixel
census (so "is the Deskbar actually drawing?" gets a numeric answer).

TEXT (#475).  It rasterises text too, through libfreetype, and reports a text
census next to the pixel one: runs received, runs rasterised, glyphs, ink
pixels written and ink pixels that actually changed the picture.  That census
exists because the pixel total cannot answer "did text draw?" -- this tool once
reported two million pixels touched for a desktop whose every label and the
clock were missing, and anything compared against it inherited that blindness.
What the census supports and what it does not is spelled out at the "Glyph
rasterisation" section below; the short form is that runs, glyph counts,
origins, advances and ink boxes are assertable and pixel equality with
app_server is not.  --min-text-runs / --min-glyph-ink / --expect-text are the
assertions; text ground truth is required by default and --allow-text-estimate
opts out.

TRANSFORMED TEXT (#494).  Font rotation, shear and false_bold_width are honoured
too, copied from AGGTextRenderer::SetFont -- see the FontTransform class, which
cites every line it took a convention from.  This is a second layer of the same
defect as #475: that one was text not drawing at all, this one is text drawing
horizontally when the wire said to turn it.  Both in-tree instruments and the
out-of-tree client all dropped these three fields, so they AGREED with each
other while disagreeing with app_server, and a comparison between them could
not fail.  --expect-text-angle is the assertion that discriminates: it measures
the direction the ink actually ran, which --expect-text (an origin the rotated
and unrotated runs SHARE) and --min-glyph-ink (a pixel count they share too)
both cannot see.

Stdlib only.  No PIL: the PNG is written by hand with zlib + struct, and the
one non-stdlib dependency is libfreetype.so.6 via ctypes -- which is also the
library app_server renders with, and whose absence is reported as "not ground
truth" rather than quietly producing blank text.

EXIT STATUS
  0  ran (even if nothing arrived -- grep CONNECTED=/MESSAGES=)
  2  bad invocation     3  broker auth denied     4  certificate pin mismatch
  5  not text ground truth (see --allow-text-estimate)
  6  a --min-text-runs / --min-glyph-ink / --expect-text /
     --expect-text-angle assertion failed

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

Session cookie -- NetReceiver.cpp (_ReceiveCandidateData).  The session port
  requires RP_SESSION_COOKIE{uint32 method = 1, length-prefixed cookie} as the
  FIRST frame of a direct connection, and app_server mints that cookie into
  <system settings>/remote_desktop/session_cookie.<port> (mode 0600) before it
  starts listening.  So a direct run needs --cookie-file; with --wss the broker
  reads the file and presents the cookie itself, and this tool must send none.
  The gate consumes the frame, so everything below this line is unchanged.

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

# app_server's own gate (#423): the per-boot session cookie, which must be the
# FIRST frame of a direct connection to the session port.  Read it from the
# server's file with --cookie-file.  Through the broker (--wss) the broker
# presents its own copy and this tool sends none -- sending one there would put
# a second cookie frame into the session stream.
RP_SESSION_COOKIE = 12
RP_COOKIE_METHOD_PER_BOOT = 1

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


def utf8_codepoints(data: bytes):
    """Decode to codepoints the way UTF8CountChars counts them.

    One entry per non-continuation byte, so the list length always matches the
    count the server used to size the RP_DRAW_STRING_WITH_OFFSETS point list.
    Undecodable bytes become U+FFFD rather than disappearing: a decoder that
    silently dropped one would desynchronise from that point list."""
    out = []
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        if b < 0x80:
            out.append(b)
            i += 1
            continue
        if (b & 0xE0) == 0xC0:
            need = 1
            cp = b & 0x1F
        elif (b & 0xF0) == 0xE0:
            need = 2
            cp = b & 0x0F
        elif (b & 0xF8) == 0xF0:
            need = 3
            cp = b & 0x07
        else:
            out.append(0xFFFD)
            i += 1
            continue
        if i + need >= n:
            out.append(0xFFFD)
            i += 1
            continue
        ok = True
        for k in range(1, need + 1):
            if (data[i + k] & 0xC0) != 0x80:
                ok = False
                break
            cp = (cp << 6) | (data[i + k] & 0x3F)
        if not ok:
            out.append(0xFFFD)
            i += 1
            continue
        out.append(cp)
        i += need + 1
    return out


# ---------------------------------------------------------------------------
# Glyph rasterisation (#475)
#
# The protocol never sends glyph bitmaps: RP_DRAW_STRING carries text plus a
# font *specification* (RemoteMessage.cpp:147-161, AddFont), so a client that
# wants text pixels has to own a rasteriser.  Without one this tool painted a
# flat guessed box per run, which made it blind to exactly the regressions it
# is trusted to catch -- a dropped or mispositioned string read as "both sides
# agree" (#475).  This binds libfreetype.so.6 through ctypes, in the same
# spirit as the libzstd binding further down: no third-party Python package,
# and the same library app_server renders with.
#
# WHAT FIDELITY IS CLAIMED, AND WHAT IS NOT
#   CLAIMED, and therefore assertable: that text was drawn; how many runs and
#     glyphs; the origin (baseline-left) of each run as it came off the wire;
#     the advance width and resulting pen position; the ink bounding box and
#     ink pixel count.  A run that is dropped, blanked, truncated or moved
#     fails those.
#   NOT CLAIMED: pixel equality with app_server.  Three reasons it cannot be:
#     1. the wire's familyAndStyle is an app_server FontManager id
#        (family index << 16 | style index) and means nothing in another
#        process, so the family cannot be recovered from the stream.  We pick a
#        file from the *face bits* plus Haiku's own documented default family
#        (ServerConfig.h: DEFAULT_PLAIN_FONT_FAMILY "Noto Sans") and report
#        which file we used.
#     2. hinting, gamma and app_server's optional subpixel (LCD) filtering are
#        not reproduced: 8-bit grey coverage, integer pen positions.
#     3. synthetic bold/oblique is used when no real styled file is installed,
#        which is not the outline the server has.
#   So: compare the CENSUS (runs, glyphs, origins, advances, ink box) between
#   two clients, not the bytes of the PNG.
# ---------------------------------------------------------------------------

# font face flags -- headers/os/interface/Font.h:79-91
B_ITALIC_FACE = 0x0001
B_UNDERSCORE_FACE = 0x0002
B_NEGATIVE_FACE = 0x0004
B_OUTLINED_FACE = 0x0008
B_STRIKEOUT_FACE = 0x0010
B_BOLD_FACE = 0x0020
B_REGULAR_FACE = 0x0040
B_CONDENSED_FACE = 0x0080
B_LIGHT_FACE = 0x0100
B_HEAVY_FACE = 0x0200

# font_spacing -- headers/os/interface/Font.h:23-28
B_CHAR_SPACING = 0
B_STRING_SPACING = 1
B_BITMAP_SPACING = 2
B_FIXED_SPACING = 3

# font_encoding -- headers/os/interface/Font.h:55
B_UNICODE_UTF8 = 0

# GlyphLayoutEngine::IsWhiteSpace (GlyphLayoutEngine.h:225-242) -- which half
# of an escapement_delta a character gets.
WHITESPACE_CODEPOINTS = frozenset([0x0009, 0x000A, 0x000B, 0x000C, 0x000D,
                                   0x0020, 0x00A0, 0x2028, 0x2029])

# Where to look for outline fonts.  Haiku's own path first so a run on the
# target renders with the target's files.
FONT_ROOTS = (
    "/boot/system/data/fonts/ttfonts",
    "/boot/system/non-packaged/data/fonts/ttfonts",
    "/boot/home/config/non-packaged/data/fonts/ttfonts",
    "/usr/share/fonts",
    "/usr/local/share/fonts",
    "/usr/share/X11/fonts",
    "~/.fonts",
    "~/.local/share/fonts",
)

# Preferred family stems, best first.  "Noto Sans" leads because it is what
# app_server itself defaults to (ServerConfig.h), so the metrics are as close
# as a second rasteriser gets; the rest are the usual metric-compatible
# stand-ins found on build hosts.
SANS_FAMILY_STEMS = ("notosans", "dejavusans", "liberationsans", "arimo",
                     "freesans", "opensans", "roboto", "carlito", "arial",
                     "helvetica")
MONO_FAMILY_STEMS = ("notosansmono", "dejavusansmono", "liberationmono",
                     "cousine", "freemono", "robotomono", "couriernew",
                     "courier")

_STYLE_NOISE = ("bolditalic", "boldoblique", "bold", "italic", "oblique",
                "regular", "book", "roman", "medium", "variable", "wght", "vf")


def _font_stem(basename):
    """Normalise a font file name to (family stem, bold, italic).

    "NotoSans[wght].ttf" -> ("notosans", False, False)
    "DejaVuSansMono-BoldOblique.ttf" -> ("dejavusansmono", True, True)
    Everything that is not alphanumeric is dropped, so bracketed variable-font
    axis names and hyphens do not create a family of their own."""
    stem = basename.rsplit(".", 1)[0]
    name = "".join(c for c in stem.lower() if c.isalnum())
    bold = ("bold" in name) or ("heavy" in name) or ("black" in name)
    italic = ("italic" in name) or ("oblique" in name)
    core = name
    for noise in _STYLE_NOISE:
        core = core.replace(noise, "")
    return core, bold, italic


class FontSet(object):
    """The font files a rasteriser will use, and how they were chosen.

    Chosen by *name*, deliberately: the wire cannot tell us the family (see the
    section comment), so the selection is a documented substitution rather than
    a decode, and `describe()` puts it in the report where a reader comparing
    two clients will see it."""

    def __init__(self):
        # (mono, bold, italic) -> path
        self.files = {}
        self.sans_family = None
        self.mono_family = None
        self.roots_scanned = []
        self.files_seen = 0

    # -- discovery -------------------------------------------------------
    def scan(self, roots=FONT_ROOTS, limit=60000):
        best = {}           # (mono, bold, italic) -> (rank, path)
        for root in roots:
            root = os.path.expanduser(root)
            if not os.path.isdir(root):
                continue
            self.roots_scanned.append(root)
            for dirpath, _dirs, names in os.walk(root):
                for name in names:
                    lower = name.lower()
                    if not (lower.endswith(".ttf") or lower.endswith(".otf")):
                        continue
                    self.files_seen += 1
                    if self.files_seen > limit:
                        break
                    core, bold, italic = _font_stem(name)
                    if core in SANS_FAMILY_STEMS:
                        mono = False
                        rank = SANS_FAMILY_STEMS.index(core)
                    elif core in MONO_FAMILY_STEMS:
                        mono = True
                        rank = MONO_FAMILY_STEMS.index(core)
                    else:
                        continue
                    key = (mono, bold, italic)
                    path = os.path.join(dirpath, name)
                    if key not in best or rank < best[key][0]:
                        best[key] = (rank, path, core)

        # Keep one family per slot class: mixing DejaVu bold with Noto regular
        # would put two different metric sets in one capture.
        for mono in (False, True):
            plain = best.get((mono, False, False))
            if plain is None:
                continue
            family = plain[2]
            if mono:
                self.mono_family = family
            else:
                self.sans_family = family
            for bold in (False, True):
                for italic in (False, True):
                    entry = best.get((mono, bold, italic))
                    if entry is not None and entry[2] == family:
                        self.files[(mono, bold, italic)] = entry[1]
        return self

    def override(self, regular=None, bold=None, fixed=None):
        if regular:
            self.files[(False, False, False)] = regular
            self.sans_family = "override"
        if bold:
            self.files[(False, True, False)] = bold
        if fixed:
            self.files[(True, False, False)] = fixed
            self.mono_family = "override"
        return self

    # -- selection -------------------------------------------------------
    def select(self, mono, bold, italic):
        """-> (path, synth_bold, synth_italic) or None.

        Falls back towards the family's regular file and says so, so a missing
        Bold shows up in the report as synthesis instead of as plain text that
        silently is not bold."""
        for want_bold, want_italic, sb, si in (
                (bold, italic, False, False),
                (bold, False, False, italic),
                (False, italic, bold, False),
                (False, False, bold, italic)):
            path = self.files.get((mono, want_bold, want_italic))
            if path is not None:
                return path, sb, si
        if mono:
            return self.select(False, bold, italic)
        return None

    def describe(self):
        plain = self.files.get((False, False, False))
        if plain is None:
            return "none"
        return "%s:%s (%d file(s) from %s)" % (
            self.sans_family or "?", plain, len(self.files),
            ",".join(self.roots_scanned) or "-")


def _load_libfreetype():
    """Open libfreetype.so.6 through ctypes.

    Same reasoning as _load_libzstd(): the versioned soname is what we rely on
    and ctypes.util.find_library() is demoted to a hint that is allowed to
    fail, because its POSIX implementation reads LIBRARY_PATH out of the
    environment and raises when it is unset -- which is every non-interactive
    run."""
    import ctypes
    candidates = []
    try:
        import ctypes.util
        found = ctypes.util.find_library("freetype")
        if found:
            candidates.append(found)
    except Exception:
        pass
    candidates += ["libfreetype.so.6", "libfreetype.so", "libfreetype.6.dylib"]
    errors = []
    for name in candidates:
        try:
            return ctypes.CDLL(name)
        except OSError as error:
            errors.append("%s: %s" % (name, error))
    raise OSError("libfreetype not loadable (%s)" % "; ".join(errors))


_FT_TYPES = None


def _ft_types():
    """ctypes mirrors of the FreeType structs this tool reads.

    Only the prefix up to the last field we touch is declared; FreeType's
    public struct layout has been stable across the whole 2.x series, and
    GlyphRasteriser.selfcheck() proves the layout on the running library
    rather than trusting that sentence."""
    global _FT_TYPES
    if _FT_TYPES is not None:
        return _FT_TYPES
    import ctypes
    FT_Pos = ctypes.c_long
    FT_Fixed = ctypes.c_long

    class FT_Vector(ctypes.Structure):
        _fields_ = [("x", FT_Pos), ("y", FT_Pos)]

    class FT_BBox(ctypes.Structure):
        _fields_ = [("xMin", FT_Pos), ("yMin", FT_Pos),
                    ("xMax", FT_Pos), ("yMax", FT_Pos)]

    class FT_Generic(ctypes.Structure):
        _fields_ = [("data", ctypes.c_void_p), ("finalizer", ctypes.c_void_p)]

    class FT_Bitmap(ctypes.Structure):
        _fields_ = [("rows", ctypes.c_uint), ("width", ctypes.c_uint),
                    ("pitch", ctypes.c_int),
                    ("buffer", ctypes.POINTER(ctypes.c_ubyte)),
                    ("num_grays", ctypes.c_ushort),
                    ("pixel_mode", ctypes.c_ubyte),
                    ("palette_mode", ctypes.c_ubyte),
                    ("palette", ctypes.c_void_p)]

    class FT_Glyph_Metrics(ctypes.Structure):
        _fields_ = [(n, FT_Pos) for n in
                    ("width", "height", "horiBearingX", "horiBearingY",
                     "horiAdvance", "vertBearingX", "vertBearingY",
                     "vertAdvance")]

    class FT_Matrix(ctypes.Structure):
        # 16.16 fixed point, x' = xx*x + xy*y, y' = yx*x + yy*y.
        _fields_ = [("xx", FT_Fixed), ("xy", FT_Fixed),
                    ("yx", FT_Fixed), ("yy", FT_Fixed)]

    class FT_Outline(ctypes.Structure):
        # n_contours/n_points changed signedness in FreeType 2.13.1 but never
        # width; everything after them is pointer-sized, so the layout this
        # mirror computes is the one the C compiler computes.  Proved on the
        # running library by GlyphRasteriser._selfcheck() rather than asserted.
        _fields_ = [("n_contours", ctypes.c_ushort),
                    ("n_points", ctypes.c_ushort),
                    ("points", ctypes.POINTER(FT_Vector)),
                    ("tags", ctypes.c_char_p),
                    ("contours", ctypes.POINTER(ctypes.c_ushort)),
                    ("flags", ctypes.c_int)]

    class FT_GlyphSlotRec(ctypes.Structure):
        _fields_ = [("library", ctypes.c_void_p), ("face", ctypes.c_void_p),
                    ("next", ctypes.c_void_p),
                    ("glyph_index", ctypes.c_uint),
                    ("generic", FT_Generic),
                    ("metrics", FT_Glyph_Metrics),
                    ("linearHoriAdvance", FT_Fixed),
                    ("linearVertAdvance", FT_Fixed),
                    ("advance", FT_Vector),
                    ("format", ctypes.c_int),
                    ("bitmap", FT_Bitmap),
                    ("bitmap_left", ctypes.c_int),
                    ("bitmap_top", ctypes.c_int),
                    ("outline", FT_Outline)]

    class FT_Size_Metrics(ctypes.Structure):
        _fields_ = [("x_ppem", ctypes.c_ushort), ("y_ppem", ctypes.c_ushort),
                    ("x_scale", FT_Fixed), ("y_scale", FT_Fixed),
                    ("ascender", FT_Pos), ("descender", FT_Pos),
                    ("height", FT_Pos), ("max_advance", FT_Pos)]

    class FT_SizeRec(ctypes.Structure):
        _fields_ = [("face", ctypes.c_void_p), ("generic", FT_Generic),
                    ("metrics", FT_Size_Metrics),
                    ("internal", ctypes.c_void_p)]

    class FT_FaceRec(ctypes.Structure):
        _fields_ = [("num_faces", ctypes.c_long),
                    ("face_index", ctypes.c_long),
                    ("face_flags", ctypes.c_long),
                    ("style_flags", ctypes.c_long),
                    ("num_glyphs", ctypes.c_long),
                    ("family_name", ctypes.c_char_p),
                    ("style_name", ctypes.c_char_p),
                    ("num_fixed_sizes", ctypes.c_int),
                    ("available_sizes", ctypes.c_void_p),
                    ("num_charmaps", ctypes.c_int),
                    ("charmaps", ctypes.c_void_p),
                    ("generic", FT_Generic),
                    ("bbox", FT_BBox),
                    ("units_per_EM", ctypes.c_ushort),
                    ("ascender", ctypes.c_short),
                    ("descender", ctypes.c_short),
                    ("height", ctypes.c_short),
                    ("max_advance_width", ctypes.c_short),
                    ("max_advance_height", ctypes.c_short),
                    ("underline_position", ctypes.c_short),
                    ("underline_thickness", ctypes.c_short),
                    ("glyph", ctypes.POINTER(FT_GlyphSlotRec)),
                    ("size", ctypes.POINTER(FT_SizeRec)),
                    ("charmap", ctypes.c_void_p)]

    _FT_TYPES = {"FT_FaceRec": FT_FaceRec, "FT_GlyphSlotRec": FT_GlyphSlotRec,
                 "FT_Matrix": FT_Matrix, "FT_Outline": FT_Outline}
    return _FT_TYPES


def font_selfcheck_problems(units_per_em, num_glyphs, family, pointers_ok,
                            glyph):
    """Known-good facts about a real face and a real 'H'; [] means plausible.

    Pure on purpose.  A wrong struct offset would not crash -- it would hand
    back plausible garbage and this tool would report confident nonsense, which
    is the failure mode the whole change exists to remove.  Keeping the
    predicate separate from the ctypes calls means it can be fed deliberately
    wrong facts in the self-test, so the guard is something that demonstrably
    fails rather than decoration."""
    problems = []
    if not (16 <= units_per_em <= 16384):
        problems.append("units_per_EM=%s" % units_per_em)
    if not (1 <= num_glyphs <= 1 << 22):
        problems.append("num_glyphs=%s" % num_glyphs)
    if not family or not all(32 <= b < 127 for b in family):
        problems.append("family_name=%r" % family)
    if not pointers_ok:
        problems.append("null glyph/size pointer")
    if glyph is None or glyph.missing:
        problems.append("no glyph for 'H'")
        return problems
    if not (4 <= glyph.rows <= 64 and 2 <= glyph.width <= 64):
        problems.append("H bitmap %dx%d" % (glyph.width, glyph.rows))
    if not (2 <= glyph.top <= 32):
        problems.append("H bitmap_top=%d" % glyph.top)
    if not (2.0 <= glyph.advance <= 64.0):
        problems.append("H advance=%.2f" % glyph.advance)
    if not any(glyph.cov):
        problems.append("H rendered with no coverage")
    return problems


def outline_selfcheck_problems(n_contours, n_points, pointers_ok):
    """Known-good facts about the FT_Outline of a real 'H'; [] means plausible.

    The transformed-text path reaches past bitmap_top into FT_GlyphSlotRec's
    `outline`, which is the deepest this file reaches into a struct it does not
    own.  A wrong offset there would not crash either: FT_Outline_Transform
    would read a point count and a pointer out of neighbouring fields and
    scribble.  So the offset is proved on the running library, on a glyph whose
    shape is known -- 'H' is one or two contours of a few dozen points in every
    real font -- and the predicate is kept pure so the self-test can feed it
    wrong facts and require it to object."""
    problems = []
    if not (1 <= n_contours <= 8):
        problems.append("H n_contours=%s" % n_contours)
    if not (4 <= n_points <= 400):
        problems.append("H n_points=%s" % n_points)
    if not pointers_ok:
        problems.append("null outline points/tags/contours pointer")
    return problems


FT_GLYPH_FORMAT_OUTLINE = 0x6F75746C            # 'outl'


class FontTransform(object):
    """The embedded font transformation: shear, then rotation, about the origin.

    COPIED, not invented.  app_server's only definition of what the wire's
    `rotation` and `shear` mean is AGGTextRenderer::SetFont
    (AGGTextRenderer.cpp:72-85):

        fEmbeddedTransformation.Reset();
        fEmbeddedTransformation.ShearBy(B_ORIGIN,
            (90.0 - font.Shear()) * M_PI / 180.0, 0.0);
        fEmbeddedTransformation.RotateBy(B_ORIGIN,
            -font.Rotation() * M_PI / 180.0);
        fContour.width(font.FalseBoldWidth() * 2.0);

    Unpacking each piece against its own source:

    * ORDER.  Transformable::ShearBy/RotateBy both end in
      trans_affine::multiply (Transformable.cpp:287-320), and multiply()
      composes so the matrix already held is applied FIRST and the argument
      second (agg_trans_affine.cpp:70-82).  So the composition is
      shear-then-rotate: M = R * S, not S * R.  Getting this backwards is
      invisible when either field is at its default and wrong whenever both
      are set, which is exactly the kind of near-miss this file exists to
      catch.
    * SHEAR.  ShearBy uses agg::trans_affine_skewing, whose matrix is
      (sx=1, shy=tan(y), shx=tan(x), sy=1) (agg_trans_affine.h:448-456), and
      agg applies it as x' = x*sx + y*shx, y' = x*shy + y*sy
      (agg_trans_affine.h:293-298).  With yShear fixed at 0 that is
      x' = x + y*tan(s), y' = y, where s = (90 - shear) in radians.  shear=90
      is therefore upright, and the field is an ANGLE in degrees, not a factor.
    * ROTATION.  RotateBy uses agg::trans_affine_rotation
      (agg_trans_affine.h:416-422) with the angle NEGATED, which is what makes
      a positive `rotation` counter-clockwise on screen.
    * SPACE.  All of the above acts on y-DOWN screen coordinates, because
      FontEngine decomposes every glyph outline with kFlipY = true
      (FontEngine.cpp:42, 573).  FreeType's own outlines are y-UP, so the
      matrix handed to FreeType is this one conjugated by diag(1, -1) --
      see ft_matrix().
    * WHAT IT ACTS ON.  RenderString translates by the baseline AFTER the
      embedded transformation (AGGTextRenderer.cpp:379-381), and the glyph
      advances are accumulated untransformed in GlyphLayoutEngine.  Because the
      transform is linear it therefore applies to the pen positions as well as
      to the outlines: the run is laid out horizontally and the whole layout is
      then turned about its baseline origin.  That is why pens here are kept in
      untransformed font space and mapped through apply() at paint time.
    * FALSE BOLD.  fContour is an agg conv_contour, and conv_contour::width(w)
      forwards to math_stroke::width which halves it (agg_vcgen_contour.h:54,
      agg_math_stroke.h:136-138).  width(falseBoldWidth * 2.0) therefore
      offsets the outline OUTWARD by falseBoldWidth pixels -- the field is the
      growth per side, in pixels.  conv_contour wraps the glyph before the
      transform (AGGTextRenderer.cpp:388-391), so the widening happens in glyph
      space and is itself rotated.
    """

    __slots__ = ("rotation", "shear", "m00", "m01", "m10", "m11", "identity")

    def __init__(self, rotation=0.0, shear=90.0):
        self.rotation = float(rotation or 0.0)
        self.shear = 90.0 if shear is None else float(shear)
        tan_s = math.tan(math.radians(90.0 - self.shear))
        # S = ((1, tan_s), (0, 1)) in row-of-output form: x' = x + tan_s*y.
        angle = math.radians(-self.rotation)
        cos_a = math.cos(angle)
        sin_a = math.sin(angle)
        # M = R * S.
        self.m00 = cos_a
        self.m01 = cos_a * tan_s - sin_a
        self.m10 = sin_a
        self.m11 = sin_a * tan_s + cos_a
        self.identity = (abs(self.rotation) <= 1e-6
                         and abs(self.shear - 90.0) <= 1e-6)

    def apply(self, x, y):
        """Map a point from untransformed font space into device space."""
        return (self.m00 * x + self.m01 * y, self.m10 * x + self.m11 * y)

    def transform_box(self, left, top, right, bottom):
        """Device-space bounds of a transformed font-space rectangle.

        The four corners are mapped and re-bounded: an axis-aligned box stays
        axis-aligned only while the matrix is, which under rotation it is not.
        """
        pts = [self.apply(left, top), self.apply(right, top),
               self.apply(right, bottom), self.apply(left, bottom)]
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        return (min(xs), min(ys), max(xs), max(ys))

    def ft_matrix(self):
        """The same transformation in FreeType's y-UP space, 16.16 fixed.

        F * M * F with F = diag(1, -1), F being its own inverse: the off-
        diagonal terms flip sign and the diagonal ones do not.  Skipping this
        conjugation rotates text the wrong way round, which looks plausible and
        is wrong -- it is the reason the sign is derived here rather than
        eyeballed from a screenshot."""
        one = 1 << 16
        return (int(round(self.m00 * one)), int(round(-self.m01 * one)),
                int(round(-self.m10 * one)), int(round(self.m11 * one)))

    def key(self):
        return self.ft_matrix() if not self.identity else None

    def describe(self):
        return "rotation=%g shear=%g" % (self.rotation, self.shear)


class Glyph(object):
    """One rendered glyph: 8-bit coverage plus its placement and advance."""

    __slots__ = ("left", "top", "width", "rows", "pitch", "cov",
                 "advance", "linear_advance", "missing")

    def __init__(self, left, top, width, rows, pitch, cov, advance,
                 linear_advance, missing):
        self.left = left                # pixels right of the pen
        self.top = top                  # pixels above the baseline
        self.width = width
        self.rows = rows
        self.pitch = pitch
        self.cov = cov
        self.advance = advance          # hinted, in pixels
        self.linear_advance = linear_advance
        self.missing = missing          # no glyph for this codepoint


class ShapedRun(object):
    __slots__ = ("glyphs", "pens", "offsets", "advance", "ascent", "descent",
                 "missing", "synth_bold", "synth_italic", "path", "transform",
                 "false_bold")

    def __init__(self):
        self.glyphs = []
        self.pens = []                  # pen offset from the origin, font space
        self.offsets = []               # the same pens mapped into device space
        self.advance = 0.0              # untransformed, as StringWidth reports
        self.ascent = 0.0
        self.descent = 0.0
        self.missing = 0
        self.synth_bold = False
        self.synth_italic = False
        self.path = None
        self.transform = None
        self.false_bold = 0.0


class GlyphRasteriser(object):
    """FreeType through ctypes, sized and shaped the way app_server does it.

    Layout follows GlyphLayoutEngine.h:340-352 exactly: B_CHAR_SPACING takes
    the precise (linear, unhinted) advance scaled by the size, every other
    spacing takes the hinted integer advance, an escapement_delta is added to
    each character's advance in *pixels* (space vs nonspace by whitespace), and
    a codepoint with no glyph advances by nothing at all.  Those are the rules
    that decide where the next character lands, so they are the rules a
    mispositioning regression has to be measured against."""

    FT_LOAD_DEFAULT = 0
    FT_RENDER_MODE_NORMAL = 0

    def __init__(self, fontset):
        import ctypes
        self._ctypes = ctypes
        self._lib = _load_libfreetype()
        self._t = _ft_types()
        lib = self._lib
        lib.FT_Init_FreeType.argtypes = [ctypes.c_void_p]
        lib.FT_New_Face.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                    ctypes.c_long, ctypes.c_void_p]
        lib.FT_Set_Char_Size.argtypes = [ctypes.c_void_p, ctypes.c_long,
                                         ctypes.c_long, ctypes.c_uint,
                                         ctypes.c_uint]
        lib.FT_Get_Char_Index.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        lib.FT_Get_Char_Index.restype = ctypes.c_uint
        lib.FT_Load_Glyph.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                      ctypes.c_int]
        lib.FT_Render_Glyph.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.FT_GlyphSlot_Embolden.argtypes = [ctypes.c_void_p]
        lib.FT_GlyphSlot_Oblique.argtypes = [ctypes.c_void_p]
        # The three that make a transformed glyph possible.  They mirror the
        # server's pipeline in the server's order: widen the outline in glyph
        # space, then transform it, then rasterise (AGGTextRenderer.cpp:388-391
        # wraps the contour INSIDE the transform, so contour-then-transform is
        # the composition, not the other way round).
        #
        # Bound inside a try so a libfreetype without them is reported as "no
        # rasteriser" -- which the caller already turns into "NOT text ground
        # truth" -- rather than escaping as an AttributeError traceback that
        # nothing catches.  A tool that dies is at least loud; a tool that dies
        # in a way its caller does not handle stops the measurement it was there
        # to take.
        try:
            lib.FT_Outline_EmboldenXY.argtypes = [ctypes.c_void_p,
                                                  ctypes.c_long, ctypes.c_long]
            lib.FT_Outline_Translate.argtypes = [ctypes.c_void_p, ctypes.c_long,
                                                 ctypes.c_long]
            lib.FT_Outline_Transform.argtypes = [ctypes.c_void_p,
                                                 ctypes.c_void_p]
        except AttributeError as exc:
            raise OSError("libfreetype has no outline transform API (%s); "
                          "rotated, sheared and false-bold text could not be "
                          "drawn faithfully, so no text is drawn at all" % exc)

        self._library = ctypes.c_void_p()
        err = lib.FT_Init_FreeType(ctypes.byref(self._library))
        if err or not self._library:
            raise OSError("FT_Init_FreeType = %d" % err)

        self.fonts = fontset
        self._faces = {}
        self._glyphs = {}
        self.glyph_cache_limit = 20000
        self.selfcheck = self._selfcheck()

    # -- faces -----------------------------------------------------------
    def _face(self, path):
        entry = self._faces.get(path)
        if entry is None:
            ctypes = self._ctypes
            handle = ctypes.c_void_p()
            err = self._lib.FT_New_Face(self._library, path.encode("utf-8"), 0,
                                        ctypes.byref(handle))
            if err or not handle:
                raise OSError("FT_New_Face(%s) = %d" % (path, err))
            rec = ctypes.cast(handle,
                              ctypes.POINTER(self._t["FT_FaceRec"])).contents
            entry = [handle, rec, None]
            self._faces[path] = entry
        return entry

    def _sized(self, path, size26_6):
        entry = self._face(path)
        if entry[2] != size26_6:
            err = self._lib.FT_Set_Char_Size(entry[0], 0, size26_6, 72, 72)
            if err:
                raise OSError("FT_Set_Char_Size(%s, %d) = %d"
                              % (path, size26_6, err))
            entry[2] = size26_6
        return entry

    def _probe_outline(self, path):
        """(n_contours, n_points, pointers_ok) for a freshly loaded 'H'."""
        entry = self._sized(path, 16 * 64)
        index = self._lib.FT_Get_Char_Index(entry[0], ord("H"))
        if self._lib.FT_Load_Glyph(entry[0], index, self.FT_LOAD_DEFAULT):
            return (0, 0, False)
        slot_ptr = entry[1].glyph
        if not slot_ptr:
            return (0, 0, False)
        live = slot_ptr.contents
        if live.format != FT_GLYPH_FORMAT_OUTLINE:
            return (0, 0, False)
        o = live.outline
        return (int(o.n_contours), int(o.n_points),
                bool(o.points) and bool(o.tags) and bool(o.contours))

    def _selfcheck(self):
        """Prove the struct layout on the library that is actually loaded."""
        choice = self.fonts.select(False, False, False)
        if choice is None:
            raise OSError("no usable font file found (looked in %s)"
                          % (",".join(self.fonts.roots_scanned) or "nothing"))
        path = choice[0]
        entry = self._sized(path, 16 * 64)
        rec = entry[1]
        family = rec.family_name or b""
        pointers_ok = bool(rec.glyph) and bool(rec.size)
        glyph = None
        if pointers_ok:
            glyph = self.glyph(path, 16 * 64, ord("H"), False, False)
        problems = font_selfcheck_problems(rec.units_per_EM, rec.num_glyphs,
                                           family, pointers_ok, glyph)
        if pointers_ok and not problems:
            problems = outline_selfcheck_problems(*self._probe_outline(path))
        if problems:
            raise OSError("freetype struct layout self-check failed on %s: %s"
                          % (path, ", ".join(problems)))
        return "%s (%s), units_per_EM=%d" % (
            family.decode("ascii", "replace"),
            (rec.style_name or b"?").decode("ascii", "replace"),
            rec.units_per_EM)

    # -- glyphs ----------------------------------------------------------
    def glyph(self, path, size26_6, codepoint, embolden, oblique,
              transform=None, bold26_6=0):
        """One rendered glyph, optionally widened and transformed.

        `transform` is a FontTransform and `bold26_6` the outward contour offset
        in 26.6 pixels (2 * false_bold_width, see FontTransform).  Both are
        applied to the OUTLINE before rasterising, in the server's own order,
        and neither touches slot->advance -- app_server does not adjust advances
        for either (GlyphLayoutEngine takes them from the untransformed metrics,
        and conv_contour has no opinion about them), so neither does this."""
        xf_key = None if transform is None else transform.key()
        key = (path, size26_6, codepoint, embolden, oblique, xf_key, bold26_6)
        hit = self._glyphs.get(key)
        if hit is not None:
            return hit
        ctypes = self._ctypes
        entry = self._sized(path, size26_6)
        index = self._lib.FT_Get_Char_Index(entry[0], codepoint)
        if self._lib.FT_Load_Glyph(entry[0], index, self.FT_LOAD_DEFAULT):
            return None
        slot_ptr = entry[1].glyph
        if not slot_ptr:
            return None
        raw = ctypes.cast(slot_ptr, ctypes.c_void_p)
        live = slot_ptr.contents      # a view on the slot, not a copy
        if embolden:
            self._lib.FT_GlyphSlot_Embolden(raw)
        if oblique:
            self._lib.FT_GlyphSlot_Oblique(raw)
        if bold26_6 or xf_key is not None:
            if live.format != FT_GLYPH_FORMAT_OUTLINE:
                # A bitmap-strike glyph cannot be widened or turned.  app_server
                # forces vector rendering for exactly this case
                # (FontCacheEntry.cpp:429-430); with no outline to force, refuse
                # rather than draw the untransformed glyph and call it truth.
                return None
            outline = ctypes.byref(live.outline)
            if bold26_6:
                self._lib.FT_Outline_EmboldenXY(outline, bold26_6, bold26_6)
                # FT_Outline_EmboldenXY grows up and to the right; conv_contour
                # grows symmetrically, so re-centre by half the strength.  That
                # is what makes false_bold_width the growth PER SIDE, which is
                # what the server's contour width means.
                self._lib.FT_Outline_Translate(outline, -(bold26_6 // 2),
                                               -(bold26_6 // 2))
            if xf_key is not None:
                m = self._t["FT_Matrix"](*xf_key)
                self._lib.FT_Outline_Transform(outline, ctypes.byref(m))
        if self._lib.FT_Render_Glyph(raw, self.FT_RENDER_MODE_NORMAL):
            return None
        slot = live                     # the same view; rendering filled it in
        bitmap = slot.bitmap
        rows = int(bitmap.rows)
        width = int(bitmap.width)
        pitch = int(bitmap.pitch)
        cov = b""
        if rows and width and bitmap.buffer:
            span = abs(pitch) * rows
            data = ctypes.string_at(bitmap.buffer, span)
            if pitch < 0:
                # Bottom-up bitmap: re-order so row 0 is the top row.
                step = -pitch
                rowsdata = [data[i * step:i * step + step]
                            for i in range(rows)]
                rowsdata.reverse()
                data = b"".join(rowsdata)
                pitch = step
            cov = data
        glyph = Glyph(int(slot.bitmap_left), int(slot.bitmap_top), width, rows,
                      pitch, cov, slot.advance.x / 64.0,
                      slot.linearHoriAdvance / 65536.0, index == 0)
        if len(self._glyphs) < self.glyph_cache_limit:
            self._glyphs[key] = glyph
        return glyph

    # -- runs ------------------------------------------------------------
    def shape(self, text, size, face=0, spacing=B_CHAR_SPACING,
              false_bold=0.0, delta=None, rotation=0.0, shear=90.0):
        """Lay a run out at the origin; the caller places it on screen.

        Rotation and shear do not change the layout: the advances come from the
        untransformed metrics and the whole horizontal layout is turned
        afterwards (see FontTransform).  So run.advance stays the number
        StringWidth answers with, and run.offsets carries where the glyphs
        actually land."""
        if size <= 0.0:
            size = 12.0
        mono = spacing == B_FIXED_SPACING
        choice = self.fonts.select(mono, bool(face & B_BOLD_FACE),
                                   bool(face & B_ITALIC_FACE))
        if choice is None:
            return None
        path, synth_bold, synth_italic = choice
        size26_6 = max(1, int(round(size * 64.0)))
        transform = FontTransform(rotation, shear)
        # Clamped at zero: a negative contour width would invert agg's outline
        # orientation, and guessing what the server would then draw is not
        # something this tool should do quietly.
        false_bold = max(0.0, float(false_bold or 0.0))
        # conv_contour offsets outward by falseBoldWidth (FontTransform), and
        # FT_Outline_EmboldenXY's strength is the TOTAL growth, so double it.
        bold26_6 = int(round(false_bold * 2.0 * 64.0))
        run = ShapedRun()
        run.path = path
        run.synth_bold = synth_bold
        run.synth_italic = synth_italic
        run.transform = transform
        run.false_bold = false_bold
        entry = self._sized(path, size26_6)
        metrics = entry[1].size.contents.metrics
        run.ascent = metrics.ascender / 64.0
        run.descent = -metrics.descender / 64.0
        pen = 0.0
        for codepoint in utf8_codepoints(text):
            glyph = self.glyph(path, size26_6, codepoint, synth_bold,
                               synth_italic, transform, bold26_6)
            run.pens.append(pen)
            run.offsets.append(transform.apply(pen, 0.0))
            run.glyphs.append(glyph)
            if glyph is None or glyph.missing:
                # GlyphLayoutEngine.h:336-340: an empty glyph advances by zero.
                run.missing += 1
                continue
            if spacing == B_CHAR_SPACING:
                advance = glyph.linear_advance
            else:
                advance = float(int(round(glyph.advance)))
            if delta is not None:
                advance += (delta[1] if codepoint in WHITESPACE_CODEPOINTS
                            else delta[0])
            pen += advance
        run.advance = pen
        return run


# ---------------------------------------------------------------------------
# PNG writer (zlib + struct, 8-bit RGB, filter type 0)
# ---------------------------------------------------------------------------

def run_axis_degrees(centres):
    """The direction a run's glyphs actually marched, in BFont rotation degrees.

    Measured from the INK -- the centres of the glyph boxes that reached the
    framebuffer -- and not from the matrix, so it is a reading of the picture
    and not a restatement of the intent.  This is the number that tells a
    rotated run from a horizontal one; a glyph count and an ink-pixel total
    cannot, because both are the same either way, which is precisely how #494
    went unnoticed.

    Degrees are reported the way the wire field is: counter-clockwise positive,
    with y up, so the sign of dy is inverted from device space.  None when
    fewer than two glyphs inked, because one glyph has no direction and saying
    "0" for it would be the same false negative in a new place."""
    if centres is None or len(centres) < 2:
        return None
    dx = centres[-1][0] - centres[0][0]
    dy = centres[-1][1] - centres[0][1]
    if abs(dx) < 1e-9 and abs(dy) < 1e-9:
        return None
    return math.degrees(math.atan2(-dy, dx))


def angle_difference(a, b):
    """Smallest absolute difference between two angles, in degrees."""
    return abs((a - b + 180.0) % 360.0 - 180.0)


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

    def blend_coverage(self, x, y, width, rows, pitch, cov, color, box=None):
        """Alpha-blend an 8-bit coverage bitmap (a rendered glyph).

        Returns the number of pixels that received ink.  Coverage pixels are
        *not* marked estimated: they come from a real outline, so they belong in
        the strict census -- which is the whole difference between this and the
        flat box that used to stand in for text.  \a box optionally limits the
        blit to an inclusive clip rectangle.

        Returns (written, changed).  The two differ when text is painted in the
        colour it is painted over: ink that is there on the wire and invisible in
        the picture, which is the exact symptom of #84's blank body.  A count of
        written pixels alone would call that healthy, so both are reported."""
        if not cov or width <= 0 or rows <= 0:
            return 0, 0
        x0, y0 = x, y
        x1, y1 = x + width - 1, y + rows - 1
        if box is not None:
            x0 = max(x0, box[0])
            y0 = max(y0, box[1])
            x1 = min(x1, box[2])
            y1 = min(y1, box[3])
        clipped = self._clip_box(x0, y0, x1, y1)
        if clipped is None:
            return 0, 0
        x0, y0, x1, y1 = clipped
        cr, cg, cb = color[0], color[1], color[2]
        buf = self.buf
        written = 0
        changed = 0
        for py in range(y0, y1 + 1):
            crow = (py - y) * pitch
            base = py * self.width
            for px in range(x0, x1 + 1):
                alpha = cov[crow + (px - x)]
                if not alpha:
                    continue
                off = (base + px) * 3
                was = (buf[off], buf[off + 1], buf[off + 2])
                if alpha == 255:
                    buf[off] = cr
                    buf[off + 1] = cg
                    buf[off + 2] = cb
                else:
                    inv = 255 - alpha
                    buf[off] = (buf[off] * inv + cr * alpha + 127) // 255
                    buf[off + 1] = (buf[off + 1] * inv + cg * alpha
                                    + 127) // 255
                    buf[off + 2] = (buf[off + 2] * inv + cb * alpha
                                    + 127) // 255
                self.est[base + px] = 0
                written += 1
                if was != (buf[off], buf[off + 1], buf[off + 2]):
                    changed += 1
        return written, changed

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
        # The whole font spec, not just the size: the face bits pick the file,
        # the spacing picks the advance rule, and a rotation or a shear means we
        # cannot claim to have rasterised the run at all.
        self.font = {"size": 12.0, "face": 0, "spacing": B_CHAR_SPACING,
                     "encoding": B_UNICODE_UTF8, "rotation": 0.0,
                     "shear": 90.0, "false_bold_width": 0.0,
                     "family_and_style": 0, "flags": 0, "direction": 0}
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
                 verbose=False, reply=True, glyphs=None,
                 answer_string_width=False):
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
        # Text census (#475).  Counting these separately from the pixel total is
        # the point: "2,056,420 pixels touched" was true of a capture with no
        # text in it at all, so the pixel count cannot answer "did text draw?".
        self.glyphs = glyphs
        self.answer_string_width = answer_string_width
        self.text_ops = 0
        self.text_runs_rasterised = 0
        self.text_chars = 0
        self.text_glyphs = 0
        self.text_glyphs_missing = 0
        self.text_ink_pixels = 0
        self.text_ink_visible = 0
        self.text_invisible_runs = 0
        self.text_ink_bbox = None
        self.text_synth_face_runs = 0
        self.text_transformed_runs = 0   # non-default rotation/shear/false bold
        self.text_unsupported = {}
        self.text_records = []
        self.text_records_limit = 4000
        self.text_trailing_bytes = 0
        self.string_width_queries = 0
        self.clipped_out_ops = 0
        self.bitmaps_decoded = 0
        self.bitmaps_placeholder = 0
        self.bitmap_colorspaces = {}
        self.outbox = []                # queued replies (code, payload bytes)
        self.errors = []
        self.negotiated_version = None
        self.negotiated_capabilities = None

    # -- verdicts --------------------------------------------------------
    def glyph_truth(self, allow_missing_glyphs=False):
        """(ok, reason): may this capture be treated as text ground truth?

        "No text arrived" is NOT ground truth when there is no rasteriser: that
        combination is exactly the trap this flag exists to close, because a
        text-blind run produces zero estimated runs as readily as a clean one.
        The capability is part of the verdict, not just the outcome."""
        if self.glyphs is None:
            return False, "no glyph rasteriser is loaded"
        if self.estimated_text_ops:
            return False, ("%d text run(s) painted as ESTIMATED boxes (%s)"
                           % (self.estimated_text_ops,
                              ", ".join("%s x%d" % (k, v) for k, v
                                        in sorted(
                                            self.text_unsupported.items()))
                              or "no reason recorded"))
        if self.text_trailing_bytes:
            return False, ("%d unread byte(s) after a text op: the run may have "
                           "been decoded against the wrong wire shape"
                           % self.text_trailing_bytes)
        if self.text_glyphs_missing and not allow_missing_glyphs:
            return False, ("%d glyph(s) have no outline in %s, so those "
                           "characters are holes here and are not in the "
                           "server's render"
                           % (self.text_glyphs_missing,
                              self.glyphs.fonts.describe()))
        return True, "ok"

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

    def _paint_glyph(self, st, glyph, gx, gy, color):
        """Blit one rendered glyph, clipped the same way a fill would be.

        Separate from _paint() because it reports two numbers: pixels written
        and pixels actually changed (see Framebuffer.blend_coverage)."""
        clips = self._clip_boxes(st)
        if clips is not None and len(clips) == 0:
            self.clipped_out_ops += 1
            return 0, 0
        written = changed = 0
        if clips is None:
            written, changed = self.fb.blend_coverage(
                gx, gy, glyph.width, glyph.rows, glyph.pitch, glyph.cov, color)
        else:
            ox, oy = self._xy(st)
            for clip in clips:
                cx0, cy0, cx1, cy1 = rect_to_pixels(clip)
                part = self.fb.blend_coverage(
                    gx, gy, glyph.width, glyph.rows, glyph.pitch, glyph.cov,
                    color, (cx0 + ox, cy0 + oy, cx1 + ox, cy1 + oy))
                written += part[0]
                changed += part[1]
        if written:
            st.pixels_touched += written
        return written, changed

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
            st.font = r.font()
            st.font_size = st.font["size"]
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
            # RemoteDrawingEngine.cpp:984-995 -- token, BPoint point,
            # AddString(string,length), bool hasDelta, [one escapement_delta].
            # EXACTLY ONE delta: escapement_delta is a single struct applied to
            # the whole string, and the server sends message.Add(delta[0]).
            # This used to read UTF8-byte-length deltas (the pre-D5 shape the
            # server no longer sends), so every delta-bearing string over one
            # byte long over-ran its payload, raised Truncated, and was dropped
            # whole -- no pixels, no run recorded, and --require-glyph-truth
            # still passed because a dropped run is not an *estimated* run. The
            # consumed-exactly check below is what makes a repeat of that shape
            # visible instead of silent.
            point = r.point()
            text = r.string()
            has_delta = r.bool8()
            delta = None
            if has_delta:
                delta = (r.f32(), r.f32())      # nonspace, space
            if r.left():
                self.text_trailing_bytes += r.left()
                if len(self.errors) < 20:
                    self.errors.append(
                        "RP_DRAW_STRING: %d byte(s) left unread after one "
                        "escapement_delta -- wire shape disagreement"
                        % r.left())
            run = self._paint_text(st, point, text, delta=delta)
            if self.reply:
                self._reply_draw_string(token, point, text, st, run=run,
                                        delta=delta)
            return

        if code == RP_DRAW_STRING_WITH_OFFSETS:
            # RemoteDrawingEngine.cpp:1034-1037 -- token, AddString, then
            # UTF8CountChars(string) BPoints.
            text = r.string()
            count = utf8_count_chars(text)
            offsets = []
            for _ in range(count):
                try:
                    offsets.append(r.point())
                except Truncated:
                    break
            if len(offsets) != count and len(self.errors) < 20:
                self.errors.append(
                    "RP_DRAW_STRING_WITH_OFFSETS: %d of %d offsets present"
                    % (len(offsets), count))
            if r.left():
                self.text_trailing_bytes += r.left()
            if offsets:
                run = self._paint_text(st, offsets[0], text, offsets=offsets)
                if self.reply:
                    self._reply_draw_string(token, offsets[-1], text, st,
                                            run=run, last_glyph_only=True,
                                            transform_origin=True)
            return

        if code == RP_STRING_WIDTH:
            text = r.string()
            self.string_width_queries += 1
            if self.reply and self.answer_string_width:
                self.outbox.append((RP_STRING_WIDTH_RESULT,
                                    struct.pack("<If", token,
                                                self._measure_width(st, text))))
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
        """Crude proportional-font advance estimate.  NOT a measurement.

        Only reached when there is no rasteriser; every caller that uses it
        also marks the run estimated, so it can never be mistaken for one."""
        size = st.font_size if st.font_size and st.font_size > 0 else 12.0
        return 0.55 * size * max(0, utf8_count_chars(text))

    def _measure_width(self, st, text, delta=None):
        """Advance width of a run: measured when we can, estimated when not."""
        run = self._shape(st, text, delta=delta)
        if run is not None:
            return run.advance
        return self._estimate_width(st, text)

    def _text_unsupported_reason(self, st):
        """Why a run cannot be rasterised faithfully, or None.

        A run we would draw *wrongly* is worse than one we refuse to draw: it
        would be counted as glyph truth.  So whatever is left here bails out to
        the estimated box and is named in the report.

        Rotation and shear USED to be listed here, and that was the #494 defect
        one level up: refusing them meant a rotated label was an estimated grey
        box in this instrument and a horizontal label in a client that decoded
        the fields and threw them away -- and "the instrument agrees" was
        reported for a picture nobody had checked.  They are honoured now (see
        FontTransform), so the only thing that still forfeits a run is a font
        state we genuinely cannot reproduce."""
        if self.glyphs is None:
            return "no rasteriser"
        font = st.font
        if font.get("encoding", B_UNICODE_UTF8) != B_UNICODE_UTF8:
            return "encoding=%d" % font["encoding"]
        return None

    def _shape(self, st, text, delta=None):
        if self._text_unsupported_reason(st) is not None:
            return None
        font = st.font
        try:
            return self.glyphs.shape(text, font.get("size", st.font_size),
                                     face=font.get("face", 0),
                                     spacing=font.get("spacing",
                                                      B_CHAR_SPACING),
                                     false_bold=font.get("false_bold_width",
                                                         0.0),
                                     delta=delta,
                                     rotation=font.get("rotation", 0.0),
                                     shear=font.get("shear", 90.0))
        except OSError as exc:
            if len(self.errors) < 20:
                self.errors.append("rasteriser: %s" % exc)
            return None

    def _paint_text(self, st, point, text, offsets=None, delta=None):
        """Rasterise a text run into the framebuffer.

        The origin of a run is its BASELINE-LEFT: BView::DrawString draws with
        the pen on the baseline, which is why both in-tree clients place text
        with no vertical adjustment (RemoteView.cpp:1400 draws at `point`;
        HaikuRemoteDesktop.js:1383 uses fillText with the default "alphabetic"
        textBaseline).  So a glyph goes at x = pen + bitmap_left and
        y = baseline - bitmap_top, and ink that lands *below* the origin is the
        signature of a flipped placement."""
        self.text_ops += 1
        reason = self._text_unsupported_reason(st)
        run = None if reason is not None else self._shape(st, text, delta=delta)
        if run is None:
            if reason is None:
                reason = "shaping failed"
            self.text_unsupported[reason] = \
                self.text_unsupported.get(reason, 0) + 1
            self._paint_estimated_text(st, point, text, offsets)
            return None

        ox, oy = self._xy(st)
        color = st.effective_color()
        xf = run.transform or FontTransform()
        ink = 0
        visible = 0
        ink_box = None
        drawn = 0
        centres = []
        for index, glyph in enumerate(run.glyphs):
            if offsets is not None:
                if index >= len(offsets):
                    break
                # The WITH_OFFSETS overload of RenderString does NOT translate
                # by a baseline (AGGTextRenderer.cpp:415-416: embedded transform
                # times the view transform, and nothing else), so the server's
                # own glyph origins go through the embedded transform too.  That
                # is app_server's behaviour, not a choice made here; copying it
                # is the only way a comparison against app_server can fail for
                # the right reason.
                px, py = xf.apply(offsets[index][0], offsets[index][1])
            else:
                dx, dy = run.offsets[index]
                px, py = point[0] + dx, point[1] + dy
            if glyph is None or not glyph.cov:
                continue
            gx = int(math.floor(px + 0.5)) + glyph.left + ox
            gy = int(math.floor(py + 0.5)) - glyph.top + oy
            painted, altered = self._paint_glyph(st, glyph, gx, gy, color)
            drawn += 1
            visible += altered
            if painted:
                ink += painted
                box = (gx, gy, gx + glyph.width - 1, gy + glyph.rows - 1)
                centres.append(((box[0] + box[2]) / 2.0,
                                (box[1] + box[3]) / 2.0))
                self._note_dest(st, *box)
                ink_box = box if ink_box is None else (
                    min(ink_box[0], box[0]), min(ink_box[1], box[1]),
                    max(ink_box[2], box[2]), max(ink_box[3], box[3]))

        # The declared box is what the run *claims* to occupy, from the wire's
        # own origin and the face's ascent/descent. Reported next to the ink box
        # so "the glyphs went somewhere else" is a visible disagreement rather
        # than something only a human eye on the PNG would catch.
        if offsets is not None:
            left = min(p[0] for p in offsets)
            right = max(p[0] for p in offsets) + (
                run.glyphs[-1].advance if run.glyphs and run.glyphs[-1] else 0)
            top = min(p[1] for p in offsets) - run.ascent
            bottom = max(p[1] for p in offsets) + run.descent
            left, top, right, bottom = xf.transform_box(left, top, right,
                                                        bottom)
        else:
            left, top, right, bottom = xf.transform_box(
                0.0, -run.ascent, run.advance, run.descent)
            left += point[0]
            right += point[0]
            top += point[1]
            bottom += point[1]
        declared = rect_to_pixels((left, top, right, bottom))
        declared = (declared[0] + ox, declared[1] + oy,
                    declared[2] + ox, declared[3] + oy)
        self._note_dest(st, *declared)

        axis = run_axis_degrees(centres)
        if not xf.identity or run.false_bold > 0.0:
            self.text_transformed_runs += 1
        self.text_runs_rasterised += 1
        self.text_chars += len(run.glyphs)
        self.text_glyphs += drawn
        self.text_glyphs_missing += run.missing
        self.text_ink_pixels += ink
        self.text_ink_visible += visible
        if ink and not visible:
            self.text_invisible_runs += 1
        if run.synth_bold or run.synth_italic:
            self.text_synth_face_runs += 1
        if ink_box is not None:
            self.text_ink_bbox = ink_box if self.text_ink_bbox is None else (
                min(self.text_ink_bbox[0], ink_box[0]),
                min(self.text_ink_bbox[1], ink_box[1]),
                max(self.text_ink_bbox[2], ink_box[2]),
                max(self.text_ink_bbox[3], ink_box[3]))
        self._record_text(st, point, text, run, ink, ink_box, declared,
                          offsets, visible=visible, axis=axis)
        return run

    def _paint_estimated_text(self, st, point, text, offsets=None):
        """Fall back to the ESTIMATED bounding box of a text run.

        Reached only when there is no rasteriser or the run's transform is one
        we will not fake.  These pixels are marked "estimated" so they stay out
        of the strict census, ESTIMATED_TEXT_OPS counts them, and
        --require-glyph-truth refuses the capture: a missing rasteriser must
        mean "not ground truth", never silently blank text."""
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
        self._record_text(st, point, text, None, 0, None, box, offsets)

    def _record_text(self, st, point, text, run, ink, ink_box, declared,
                     offsets, visible=0, axis=None):
        if len(self.text_records) >= self.text_records_limit:
            return
        self.text_records.append({
            "token": st.token,
            "text": text.decode("utf-8", "replace"),
            "origin": [round(point[0], 3), round(point[1], 3)],
            "size": st.font.get("size", st.font_size),
            "face": st.font.get("face", 0),
            "spacing": st.font.get("spacing", B_CHAR_SPACING),
            # The three fields #494 is about, recorded whether or not they were
            # honoured, so a census can be grepped for transformed runs.
            "rotation": st.font.get("rotation", 0.0),
            "shear": st.font.get("shear", 90.0),
            "false_bold_width": st.font.get("false_bold_width", 0.0),
            # ...and what the ink did about them, which is the assertable part.
            "ink_axis_deg": None if axis is None else round(axis, 2),
            "with_offsets": offsets is not None,
            "chars": utf8_count_chars(text),
            "glyphs": 0 if run is None else len(run.glyphs),
            "missing_glyphs": 0 if run is None else run.missing,
            "advance": None if run is None else round(run.advance, 3),
            "ink_pixels": ink,
            "ink_visible": visible,
            "ink_bbox": None if ink_box is None else list(ink_box),
            "declared_bbox": list(declared),
            "rasterised": run is not None,
            "font_file": None if run is None else run.path,
            "synthetic_face": bool(run is not None
                                   and (run.synth_bold or run.synth_italic)),
        })

    def _reply_draw_string(self, token, point, text, st, run=None, delta=None,
                           last_glyph_only=False, transform_origin=False):
        """RP_DRAW_STRING_RESULT: uint32 token, BPoint penLocation.

        Consumed by RemoteDrawingEngine::_DrawingEngineResult
        (RemoteDrawingEngine.cpp:1214-1224).  MUST be sent: DrawString blocks
        the app_server for up to 1 s waiting for it
        (RemoteDrawingEngine.cpp:1015-1018), so a silent client would itself
        suppress the drawing we are trying to measure.

        The pen lands at origin + the sum of the advances
        (GlyphLayoutEngine.h:368-370 adds the last advance after the loop), so
        with a rasteriser this is now a measurement and not the old
        0.55-per-character guess.  It matters beyond tidiness: the server
        *uses* this position for whatever it draws next, so a client that
        answers with a wrong pen makes the server itself mislay the rest of the
        line.

        Under a rotated or sheared font the pen does not stay on a horizontal
        line, and StringRenderer::Finish puts the untransformed pen through the
        full transform before handing it back (AGGTextRenderer.cpp:182-186).  So
        the advance is mapped the same way here: answering point.x + advance for
        a rotated run would send the server off along a line its own renderer
        never drew."""
        if run is not None:
            if last_glyph_only:
                # RP_DRAW_STRING_WITH_OFFSETS: the server placed every glyph
                # itself, so the pen after it is the last offset plus that one
                # glyph's advance -- which is what BView::PenLocation() returns
                # in the native client (RemoteView.cpp:1435).
                last = run.glyphs[-1] if run.glyphs else None
                advance = 0.0 if last is None else last.advance
            else:
                advance = run.advance
            xf = run.transform or FontTransform()
        else:
            advance = self._estimate_width(st, text)
            xf = FontTransform(st.font.get("rotation", 0.0),
                               st.font.get("shear", 90.0))
        if transform_origin:
            # WITH_OFFSETS: the origin is one of the server's own offsets, and
            # those go through the transform too (see _paint_text), so the pen
            # after the run has to be measured in the same space the glyphs were
            # drawn in.  Transforming only the advance would put the reported
            # pen somewhere no glyph ever went.
            base = xf.apply(point[0] + advance, point[1])
            self.outbox.append((RP_DRAW_STRING_RESULT,
                                struct.pack("<Iff", token, base[0], base[1])))
            return
        dx, dy = xf.apply(advance, 0.0)
        self.outbox.append((RP_DRAW_STRING_RESULT,
                            struct.pack("<Iff", token, point[0] + dx,
                                        point[1] + dy)))

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
                 capabilities=0, cookie=None):
        self.deadline = deadline
        self.sock = socket.create_connection((host, port),
                                            timeout=connect_timeout)
        self.sock.settimeout(0.5)
        self.buf = bytearray()
        self.capabilities = capabilities
        # Always present, so the byte counters work in both arms of an A/B.
        # With no compression capability it is a pure passthrough.
        self.wire = WireDecoder(capabilities)

        # The session cookie, before any other byte.  app_server's candidate
        # gate reads exactly this frame and decides the connection's fate on
        # it; RP_INIT_CONNECTION sent ahead of it is refused, and the gate
        # consumes the cookie, so nothing above this line has to know about it.
        if cookie:
            cookie_bytes = (cookie.encode() if isinstance(cookie, str)
                            else cookie)
            payload = (struct.pack("<II", RP_COOKIE_METHOD_PER_BOOT,
                                   len(cookie_bytes)) + cookie_bytes)
            if not self.send(frame(RP_SESSION_COOKIE, payload)):
                raise EOFError("failed to send RP_SESSION_COOKIE")

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


def parse_text_expectation(spec):
    """'Tracker@120,404' -> ('Tracker', 120.0, 404.0); 'Tracker' -> (t, None, None).

    The '@' is split from the RIGHT so a string containing one still works."""
    if "@" in spec:
        text, _, where = spec.rpartition("@")
        parts = where.split(",")
        if len(parts) == 2:
            try:
                return text, float(parts[0]), float(parts[1])
            except ValueError:
                pass
        # Not a coordinate after all: it was part of the text.
        return spec, None, None
    return spec, None, None


def parse_angle_expectation(spec):
    """'Rotated@90' -> ('Rotated', 90.0); None when there is no angle.

    Split from the RIGHT like parse_text_expectation, so a '@' in the text
    survives."""
    text, sep, where = spec.rpartition("@")
    if not sep:
        return None
    try:
        return text, float(where.strip())
    except ValueError:
        return None


def text_angle_failures(cap, args):
    """--expect-text-angle: the run's INK must march in the stated direction.

    This is the assertion #494 is about, and the reason it is a new one rather
    than a tightening of --expect-text: --expect-text checks the ORIGIN, which a
    rotated run shares exactly with a horizontal one, and --min-glyph-ink counts
    pixels, of which a rotated run has the same number.  Both pass on a client
    that decoded the rotation and threw it away.  An assertion that cannot go
    red for the defect it is aimed at is not evidence, and three of the last
    five bugs found here were exactly that shape (#475, #479, #485).

    The measured angle comes from run_axis_degrees -- first inked glyph centre
    to last -- so what is compared is a property of the picture.  The expected
    angle comes from the command line.  A horizontal run asserted at 90 degrees
    fails; that is the whole test."""
    failures = []
    # getattr, not attribute access: text_expectation_failures is called with
    # hand-rolled args objects in places, and an assertion that raises
    # AttributeError instead of returning a verdict is not a working assertion.
    tolerance = getattr(args, "text_angle_tolerance", 12.0)
    for spec in getattr(args, "expect_text_angle", []) or []:
        parsed = parse_angle_expectation(spec)
        if parsed is None:
            failures.append("--expect-text-angle %r: expected TEXT@DEGREES"
                            % spec)
            continue
        want, want_deg = parsed
        seen = []
        matched = False
        for record in cap.text_records:
            if record["text"] != want:
                continue
            axis = record.get("ink_axis_deg")
            if axis is None:
                seen.append("drew %d glyph(s), too few to have a direction"
                            % record.get("glyphs", 0))
                continue
            if angle_difference(axis, want_deg) <= tolerance:
                matched = True
                break
            seen.append("ink ran at %.1f deg (font said rotation=%g)"
                        % (axis, record.get("rotation", 0.0)))
        if matched:
            continue
        if not seen:
            failures.append("--expect-text-angle %r: no run with that text "
                            "drew any ink (%d run(s) recorded)"
                            % (spec, len(cap.text_records)))
        else:
            failures.append("--expect-text-angle %r: no run ran within %.1f "
                            "deg of %.1f -- found %s"
                            % (spec, tolerance, want_deg,
                               "; ".join(seen[:4])))
    return failures


def text_expectation_failures(cap, args):
    """Check the caller's text assertions against the census.

    The expected values come from the CALLER, never from the decoder. That is
    deliberate: #423's wire checks were self-consistent -- encoder and decoder
    shared their constants, so mutating the opcode still passed -- and the cure
    was to pin a value the code could not derive. A threshold or an origin typed
    on the command line is the same cure: if the decoder stops drawing text, or
    draws it somewhere else, there is nothing for it to agree with."""
    failures = []
    if args.min_text_runs and cap.text_runs_rasterised < args.min_text_runs:
        failures.append("--min-text-runs %d: only %d run(s) were rasterised "
                        "(TEXT_OPS=%d received)"
                        % (args.min_text_runs, cap.text_runs_rasterised,
                           cap.text_ops))
    if args.min_glyph_ink and cap.text_ink_visible < args.min_glyph_ink:
        # Visible, not merely written: text painted in the colour it was painted
        # over is on the wire and absent from the picture (#84's symptom), and
        # asserting on written pixels alone would pass it.
        failures.append("--min-glyph-ink %d: only %d glyph ink pixel(s) changed "
                        "the framebuffer (%d written)"
                        % (args.min_glyph_ink, cap.text_ink_visible,
                           cap.text_ink_pixels))
    for spec in args.expect_text:
        want, wx, wy = parse_text_expectation(spec)
        seen_text = False
        matched = False
        near = []
        for record in cap.text_records:
            if record["text"] != want:
                continue
            seen_text = True
            if not record["rasterised"] or not record["ink_visible"]:
                near.append("drawn no ink at (%.1f,%.1f)"
                            % tuple(record["origin"]))
                continue
            if wx is None:
                matched = True
                break
            if (abs(record["origin"][0] - wx) <= args.text_tolerance
                    and abs(record["origin"][1] - wy) <= args.text_tolerance):
                matched = True
                break
            near.append("at (%.1f,%.1f)" % tuple(record["origin"]))
        if matched:
            continue
        if not seen_text:
            # Say when the search was over a truncated list, so "not received"
            # cannot quietly mean "past the record limit".
            failures.append("--expect-text %r: no run with that text was "
                            "received at all (%d run(s) recorded%s)"
                            % (spec, len(cap.text_records),
                               " -- AT the %d record limit, so later runs were "
                               "not kept" % cap.text_records_limit
                               if len(cap.text_records)
                               >= cap.text_records_limit else ""))
        elif wx is None:
            failures.append("--expect-text %r: the run arrived but was not "
                            "rasterised with ink (%s)"
                            % (spec, "; ".join(near[:4])))
        else:
            failures.append("--expect-text %r: the run arrived but its origin "
                            "is not within %.1f px of (%.1f,%.1f) -- found %s"
                            % (spec, args.text_tolerance, wx, wy,
                               "; ".join(near[:4])))
    failures.extend(text_angle_failures(cap, args))
    return failures


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
    # Text census (#475).  TEXT_INK_PIXELS is the one to assert on: it is zero
    # for a capture that received text and drew none, which is precisely the
    # state a pixel total cannot distinguish from a healthy one.
    truth_ok, truth_why = cap.glyph_truth(
        allow_missing_glyphs=getattr(args, "allow_missing_glyphs", False))
    out.append("GLYPH_RASTERISER=%s"
               % ("none" if cap.glyphs is None
                  else cap.glyphs.fonts.describe()))
    out.append("GLYPH_TRUTH=%d" % (1 if truth_ok else 0))
    out.append("GLYPH_TRUTH_WHY=%s" % truth_why)
    out.append("TEXT_OPS=%d" % cap.text_ops)
    out.append("TEXT_RUNS_RASTERISED=%d" % cap.text_runs_rasterised)
    out.append("TEXT_CHARS=%d" % cap.text_chars)
    out.append("TEXT_GLYPHS=%d" % cap.text_glyphs)
    out.append("TEXT_GLYPHS_MISSING=%d" % cap.text_glyphs_missing)
    out.append("TEXT_INK_PIXELS=%d" % cap.text_ink_pixels)
    out.append("TEXT_INK_VISIBLE=%d" % cap.text_ink_visible)
    out.append("TEXT_INVISIBLE_RUNS=%d" % cap.text_invisible_runs)
    out.append("TEXT_INK_BBOX=%s"
               % ("-" if cap.text_ink_bbox is None
                  else "%d,%d,%d,%d" % cap.text_ink_bbox))
    out.append("TEXT_SYNTHETIC_FACE_RUNS=%d" % cap.text_synth_face_runs)
    out.append("TEXT_TRANSFORMED_RUNS=%d" % cap.text_transformed_runs)
    out.append("TEXT_INK_AXES=%s"
               % (",".join("%g" % a for a in sorted(
                   set(r["ink_axis_deg"] for r in cap.text_records
                       if r.get("ink_axis_deg") is not None))) or "-"))
    out.append("TEXT_TRAILING_BYTES=%d" % cap.text_trailing_bytes)
    out.append("STRING_WIDTH_QUERIES=%d" % cap.string_width_queries)
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
    if cap.undecoded_drawing_ops:
        print("NOTE: %d drawing op(s) were not rasterised properly; where a "
              "destination rect was decodable it was painted flat %s "
              "(%d had no decodable rect at all)."
              % (cap.undecoded_drawing_ops, str(PLACEHOLDER),
                 cap.undecoded_no_rect_ops))
    print("")
    print("--- TEXT --------------------------------------------------")
    print("rasteriser    : %s"
          % ("NONE -- text is not ground truth" if cap.glyphs is None
             else cap.glyphs.selfcheck))
    if cap.glyphs is not None:
        print("font files    : %s" % cap.glyphs.fonts.describe())
    print("runs          : %d received, %d rasterised, %d estimated"
          % (cap.text_ops, cap.text_runs_rasterised, cap.estimated_text_ops))
    print("glyphs        : %d drawn, %d with no outline, %d ink pixels "
          "(%d changed the picture)"
          % (cap.text_glyphs, cap.text_glyphs_missing, cap.text_ink_pixels,
             cap.text_ink_visible))
    if cap.text_invisible_runs:
        print("NOTE: %d run(s) drew ink that changed NOTHING -- text painted in "
              "the colour it was painted over. That is text present on the wire "
              "and absent from the picture, which is what #84 looked like."
              % cap.text_invisible_runs)
    print("ink bbox      : %s"
          % ("none" if cap.text_ink_bbox is None
             else "(%d,%d)-(%d,%d)" % cap.text_ink_bbox))
    print("GLYPH_TRUTH   : %s (%s)" % ("YES" if truth_ok else "NO", truth_why))
    if cap.text_synth_face_runs:
        print("NOTE: %d run(s) used a SYNTHETIC bold/oblique because no real "
              "styled file was installed; their outlines are not the server's."
              % cap.text_synth_face_runs)
    if cap.string_width_queries:
        print("RP_STRING_WIDTH: %d query/queries%s"
              % (cap.string_width_queries,
                 " -- answered from our own metrics"
                 if cap.answer_string_width else " -- NOT answered"))
    # What may and may not be concluded from the glyphs above. Stated here, at
    # the point of production, because the over-claim is the bug (#475) and an
    # over-claiming instrument is no better than a blind one.
    if cap.text_transformed_runs:
        axes = sorted(set((r["rotation"], r["shear"], r["false_bold_width"],
                           r["ink_axis_deg"]) for r in cap.text_records
                          if r["rasterised"]
                          and (r["rotation"] or r["shear"] != 90.0
                               or r["false_bold_width"])))
        print("transformed   : %d run(s) with a non-default rotation, shear or "
              "false bold" % cap.text_transformed_runs)
        for rot, shear, fb, axis in axes[:6]:
            print("  rotation=%-6g shear=%-6g false_bold=%-4g -> ink ran at %s"
                  % (rot, shear, fb,
                     "%.1f deg" % axis if axis is not None
                     else "no direction (one glyph)"))
    if cap.text_runs_rasterised:
        print("fidelity      : ASSERTABLE -- that text drew, how many runs and "
              "glyphs, their origins, advances and ink box, and the DIRECTION "
              "the ink ran in (so a rotation or shear that was decoded and "
              "discarded fails, #494).  NOT ASSERTABLE -- pixel equality with "
              "app_server (family cannot be recovered from the wire, and "
              "hinting/subpixel filtering differ), nor sub-degree accuracy of "
              "the rotation, nor the exact widening of a false-bold outline: "
              "the transform and the contour offset are applied to FreeType's "
              "outline here and to an AGG path there, so the two agree on "
              "geometry and not on pixels.")
    for reason, count in sorted(cap.text_unsupported.items()):
        print("NOTE: %d text run(s) not rasterised: %s" % (count, reason))
    if cap.estimated_text_ops:
        print("NOTE: %d text run(s) were painted as an ESTIMATED bounding box "
              "flat %s.  Those pixels are excluded from the *_STRICT counts, "
              "and this capture is NOT text ground truth."
              % (cap.estimated_text_ops, str(PLACEHOLDER)))
    for record in cap.text_records[:8]:
        print("  %-24r origin=(%.1f,%.1f) size=%.1f glyphs=%d ink=%d/%d axis=%s "
              "%s"
              % (record["text"][:24], record["origin"][0], record["origin"][1],
                 record["size"], record["glyphs"], record["ink_visible"],
                 record["ink_pixels"],
                 "-" if record.get("ink_axis_deg") is None
                 else "%.1f" % record["ink_axis_deg"],
                 "" if record["rasterised"] else "ESTIMATED"))
    if len(cap.text_records) > 8:
        print("  ... %d more run(s)" % (len(cap.text_records) - 8))
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
            "glyph_truth": truth_ok,
            "glyph_truth_why": truth_why,
            "glyph_rasteriser": (None if cap.glyphs is None
                                 else cap.glyphs.fonts.describe()),
            "text_ops": cap.text_ops,
            "text_runs_rasterised": cap.text_runs_rasterised,
            "text_chars": cap.text_chars,
            "text_glyphs": cap.text_glyphs,
            "text_glyphs_missing": cap.text_glyphs_missing,
            "text_ink_pixels": cap.text_ink_pixels,
            "text_ink_visible": cap.text_ink_visible,
            "text_invisible_runs": cap.text_invisible_runs,
            "text_ink_bbox": (None if cap.text_ink_bbox is None
                              else list(cap.text_ink_bbox)),
            "text_synthetic_face_runs": cap.text_synth_face_runs,
            "text_transformed_runs": cap.text_transformed_runs,
            "text_trailing_bytes": cap.text_trailing_bytes,
            "text_unsupported": dict(cap.text_unsupported),
            "text_runs": cap.text_records,
            "string_width_queries": cap.string_width_queries,
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
        # Label the artifact at the point of production. The summary already
        # notes estimated text, but that note sits at the end of a long report
        # and the PNG outlives it: this capture has been used as the known-good
        # side of comparisons (the #416 wire measurements among them), and a
        # reader who sees only "wrote PNG" has no way to know the glyphs are
        # flat boxes. A comparison that is blind to text reads a text
        # regression as "both sides agree" (#475).
        if not truth_ok:
            print("       !!  NOT TEXT GROUND TRUTH: %s. Do not diff this PNG "
                  "against a real client and conclude the client is wrong; "
                  "compare the census instead." % truth_why)
        else:
            print("       text is ground truth: %d run(s), %d glyph(s), %d "
                  "visible ink pixels, from %s. Assert on the census "
                  "(runs/glyphs/origins/advances/ink box), NOT on pixel "
                  "equality with app_server: the wire does not carry the font "
                  "family, and hinting and subpixel filtering differ."
                  % (cap.text_runs_rasterised, cap.text_glyphs,
                     cap.text_ink_visible, cap.glyphs.fonts.describe()))
        if cap.undecoded_drawing_ops:
            # Separate sentence, separate subject: these are NON-text ops, and
            # folding them into the text verdict is how "ok" ended up printed
            # inside a line that began "NOT TEXT GROUND TRUTH".
            print("       !!  %d NON-text drawing op(s) were painted as flat "
                  "placeholders; those areas are not ground truth either."
                  % cap.undecoded_drawing_ops)


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def selftest(allow_skip=False):
    """Synthesise a byte stream and push it through the real parser.

    This is the "does the instrument work" control: without it a zero result
    is indistinguishable from a broken decoder.

    With \a allow_skip the run still passes when a check group could not run
    (no libzstd, say).  By default it does not: see skip() below."""
    import os
    import tempfile

    width, height = 200, 120
    failures = []
    checks = []
    skipped = []

    def check(label, cond, detail=""):
        checks.append(label)
        if cond:
            print("  ok    %s" % label)
        else:
            print("  FAIL  %s %s" % (label, detail))
            failures.append(label)

    def skip(label, why):
        """Record a check that did not run.

        This exists because it once did not.  When ctypes.util.find_library()
        raised on a missing LIBRARY_PATH, the four compressed-wire checks below
        were skipped and the run still printed SELFTEST=PASS -- a green result
        that covered nothing of the feature it was run to cover.  A skip is now
        a *failure* of the run unless the caller passed --allow-skip, and the
        check count is printed either way, so a silently shrinking self-test
        cannot look like a passing one.
        """
        print("  skip  %s (%s)" % (label, why))
        skipped.append("%s: %s" % (label, why))

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
    # glyphs=None on purpose: this arm covers the DEGRADED path, the one a host
    # with no libfreetype falls back to. It has to keep behaving exactly as it
    # did -- flat estimated box, counted, excluded from the strict census -- so
    # that "no rasteriser" can never look like "text matched". The rasterised
    # path is a second arm further down, and the difference between the two arms
    # is the discrimination this tool was missing (#475).
    cap = Capture(width, height, clip=True, apply_offsets=False, verbose=False,
                  reply=True, glyphs=None)
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

    # ---- session cookie frame (#423) ---------------------------------
    print("  -- session cookie --")

    # The gate in app_server (NetReceiver::_ReceiveCandidateData) reads the six
    # byte header, then exactly the rest of the length it declared, and decodes
    # the method at +6, the cookie length at +10 and the cookie at +14.  This is
    # that decoder, at those offsets, so the two implementations are checked
    # against each other rather than each against its author's memory.  A
    # disagreement here means every direct connection is refused on hardware --
    # the expensive place to find out.
    def gate_decode(data, expect_cookie):
        if len(data) < HEADER:
            return "short header"
        code, length = struct.unpack_from("<HI", data, 0)
        if code != RP_SESSION_COOKIE:
            return "not a session cookie"
        if length < HEADER + 8 or length > HEADER + 8 + 256:
            return "implausible length"
        if len(data) < length:
            return "incomplete frame"
        method, cookie_length = struct.unpack_from("<II", data, HEADER)
        if method != RP_COOKIE_METHOD_PER_BOOT:
            return "wrong method"
        if HEADER + 8 + cookie_length != length:
            return "length mismatch"
        got = data[HEADER + 8:length]
        return "ok" if got == expect_cookie else "wrong cookie"

    secret = b"a" * 64
    encoded = frame(RP_SESSION_COOKIE,
                    struct.pack("<II", RP_COOKIE_METHOD_PER_BOOT, len(secret))
                    + secret)
    check("cookie frame is header + method + length + cookie",
          len(encoded) == HEADER + 8 + len(secret), str(len(encoded)))
    check("cookie frame decodes at the gate's offsets",
          gate_decode(encoded, secret) == "ok", gate_decode(encoded, secret))
    check("a wrong cookie of the same length is refused",
          gate_decode(encoded, b"b" * 64) == "wrong cookie")
    check("the gate never guesses on a partial header",
          gate_decode(encoded[:HEADER - 1], secret) == "short header")
    check("the gate waits for an incomplete body",
          gate_decode(encoded[:-1], secret) == "incomplete frame")
    check("RP_INIT_CONNECTION as the first frame is refused",
          gate_decode(frame(RP_INIT_CONNECTION), secret)
              == "not a session cookie")
    # A declared length that disagrees with the embedded cookie length is the
    # shape a hand-rolled or byte-swapped client produces; it must be refused
    # rather than read past.
    lying = bytearray(encoded)
    struct.pack_into("<I", lying, HEADER + 4, len(secret) - 1)
    check("a length that disagrees with the frame is refused",
          gate_decode(bytes(lying), secret) == "length mismatch")
    # And the connection this tool actually opens must produce that frame first.
    # A Connection is not built here (no server), so the encoder is checked
    # through the same call the constructor makes.
    check("the cookie encoder matches what Connection sends",
          frame(RP_SESSION_COOKIE,
                struct.pack("<II", RP_COOKIE_METHOD_PER_BOOT, len(secret))
                + secret) == encoded)

    # A golden vector, byte for byte, because everything above is self
    # consistent: encoder and decoder share these constants, so a run with the
    # opcode or the method changed to a wrong value still passed every check
    # until this one existed.  What is pinned here is the wire as the OTHER
    # implementations spell it -- RP_SESSION_COOKIE = 12 and method 1 in
    # RemoteMessage.h, kRPSessionCookie/kCookieMethodPerBoot in
    # RemoteBroker.cpp -- and little-endian framing.
    golden = (bytes((12, 0))                        # code 12
              + bytes((78, 0, 0, 0))                # total length 6 + 8 + 64
              + bytes((1, 0, 0, 0))                 # method 1, per-boot cookie
              + bytes((64, 0, 0, 0))                # cookie length 64
              + secret)
    check("cookie frame matches the golden wire bytes", encoded == golden,
          encoded[:14].hex())

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
        skip("the whole compressed wire block", "no usable libzstd: %s" % exc)
        enc = None

    if enc is not None:
        ack = frame(RP_HELLO_ACK, struct.pack("<II", URP_PROTOCOL_VERSION,
                                              CAP_COMPRESS_ZSTD))
        fill = frame(RP_FILL_RECT_COLOR,
                     struct.pack("<Iffff", token, 1.0, 1.0, 9.0, 9.0)
                     + bytes((7, 7, 7, 255)))
        # Messages after the acknowledgement: two ordinary ones, then an exempt
        # (already-compressed) one that must travel as a raw segment, then two
        # more ordinary ones.  The last two are the point: the raw segment is
        # interleaved *inside* the compressed stream, so if the passthrough had
        # been fed to the compressor -- or if the per-message flush did not land
        # on the message boundary -- the messages after it would not decode.
        # The tail is what RemoteWireFormat.h claims and nothing else asserts.
        exempt_index = 2
        tail = [fill, fill, frame(RP_CODEC_TILE, os.urandom(512)), fill, fill]

        wire = bytearray(ack)
        for i, msg in enumerate(tail):
            if i == exempt_index:
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
              dec.raw_segments == 1 and dec.compressed_segments == 4,
              "(raw=%d compressed=%d)"
              % (dec.raw_segments, dec.compressed_segments))
        check("the compressed stream survives an interleaved raw segment",
              bytes(recovered).endswith(fill + fill))
        check("repeated messages compress",
              dec.wire_bytes < dec.plain_bytes,
              "(wire=%d plain=%d)" % (dec.wire_bytes, dec.plain_bytes))
        dec.close()

    # ---- text: the wire shape, the decode, and the census (#475) ------
    print("  -- text ops --")

    # A golden wire vector for RP_DRAW_STRING, spelled the way the SERVER
    # spells it (RemoteDrawingEngine.cpp:984-995) rather than the way this file
    # spells it: opcode 180 and total length pinned as raw bytes, one
    # escapement_delta and not one per character.  The old decoder read
    # UTF8-byte-length deltas, so this exact frame over-ran its payload and the
    # run was dropped whole -- no pixels, no run recorded, and no estimated run
    # either, which is why --require-glyph-truth used to pass on a stream whose
    # every delta-bearing string had vanished.
    gtoken = 11
    gtext = b"Hi"
    delta_bytes = struct.pack("<ff", 1.5, 2.5)
    golden_string = (bytes((180, 0))                    # RP_DRAW_STRING
                     + bytes((33, 0, 0, 0))             # 6+4+8+4+2+1+8
                     + bytes((11, 0, 0, 0))             # token 11
                     + struct.pack("<ff", 30.0, 90.0)   # baseline-left origin
                     + bytes((2, 0, 0, 0))              # string length 2
                     + gtext
                     + bytes((1,))                      # hasDelta
                     + delta_bytes)
    built = frame(RP_DRAW_STRING,
                  struct.pack("<I", gtoken) + struct.pack("<ff", 30.0, 90.0)
                  + struct.pack("<I", len(gtext)) + gtext + b"\x01"
                  + delta_bytes)
    check("RP_DRAW_STRING frame matches the golden wire bytes",
          built == golden_string, built.hex())
    check("a delta-bearing RP_DRAW_STRING carries exactly one delta",
          len(golden_string) == HEADER + 4 + 8 + 4 + len(gtext) + 1 + 8,
          str(len(golden_string)))

    def font_payload(token, size, face=0, spacing=B_CHAR_SPACING,
                     rotation=0.0, shear=90.0, false_bold=0.0, encoding=0):
        # RemoteMessage.cpp:147-161 (AddFont), packed, 29 bytes.
        return struct.pack("<IBBIBffffHI", token, 0, encoding, 0, spacing,
                           shear, rotation, false_bold, size, face, 0)

    def text_stream(origin=(30.0, 90.0), text=b"Hi", delta=delta_bytes,
                    size=16.0, extra=b"", deltas=1):
        out = bytearray()
        out += frame(RP_CREATE_STATE, struct.pack("<I", gtoken))
        out += frame(RP_SET_HIGH_COLOR,
                     struct.pack("<I", gtoken) + bytes((255, 255, 255, 255)))
        out += frame(RP_SET_FONT, font_payload(gtoken, size))
        payload = (struct.pack("<I", gtoken)
                   + struct.pack("<ff", origin[0], origin[1])
                   + struct.pack("<I", len(text)) + text)
        if delta is None:
            payload += b"\x00"
        else:
            payload += b"\x01" + delta * deltas
        out += frame(RP_DRAW_STRING, payload)
        out += extra
        return bytes(out)

    def run_stream(data, **kwargs):
        c = Capture(width, height, clip=True, apply_offsets=False,
                    verbose=False, reply=True, **kwargs)
        pos = 0
        while pos + HEADER <= len(data):
            code, length = struct.unpack_from("<HI", data, pos)
            c.handle(code, bytes(data[pos + HEADER:pos + length]))
            pos += length
        return c

    # The degraded arm first, so the two are comparable: same bytes, no
    # rasteriser.  This is what every capture before #475 looked like.
    blind = run_stream(text_stream(), glyphs=None)
    check("without a rasteriser a text run is estimated, not drawn",
          (blind.text_ops == 1 and blind.text_runs_rasterised == 0
           and blind.text_ink_pixels == 0 and blind.estimated_text_ops == 1),
          "ops=%d rast=%d ink=%d est=%d"
          % (blind.text_ops, blind.text_runs_rasterised,
             blind.text_ink_pixels, blind.estimated_text_ops))
    check("a delta-bearing run is no longer dropped as truncated",
          blind.truncated == 0 and blind.text_trailing_bytes == 0,
          "truncated=%d trailing=%d"
          % (blind.truncated, blind.text_trailing_bytes))
    check("a capture with no rasteriser is NOT glyph truth",
          blind.glyph_truth()[0] is False, blind.glyph_truth()[1])

    try:
        fonts = FontSet().scan()
        ras = GlyphRasteriser(fonts)
    except (OSError, RuntimeError) as exc:
        skip("the whole glyph rasterisation block",
             "no usable freetype/font: %s" % exc)
        ras = None

    if ras is not None:
        print("  (rasteriser: %s)" % ras.selfcheck)
        lit = run_stream(text_stream(), glyphs=ras)
        check("with a rasteriser the run is rasterised",
              (lit.text_ops == 1 and lit.text_runs_rasterised == 1
               and lit.estimated_text_ops == 0),
              "ops=%d rast=%d est=%d" % (lit.text_ops,
                                         lit.text_runs_rasterised,
                                         lit.estimated_text_ops))
        check("both glyphs of a two-character run were drawn",
              lit.text_glyphs == 2 and lit.text_chars == 2,
              "glyphs=%d chars=%d" % (lit.text_glyphs, lit.text_chars))
        check("no codepoint was missing an outline",
              lit.text_glyphs_missing == 0, str(lit.text_glyphs_missing))
        check("glyph ink reached the framebuffer",
              lit.text_ink_pixels > 0, str(lit.text_ink_pixels))
        check("a rasterised capture IS glyph truth",
              lit.glyph_truth()[0] is True, lit.glyph_truth()[1])
        check("glyph pixels are not marked estimated (they are in the strict "
              "census)",
              lit.fb.black_counts()[0] == lit.fb.black_counts()[1],
              "%d vs %d" % lit.fb.black_counts())

        # The baseline convention, which is protocol and not preference:
        # BView::DrawString puts the pen ON the baseline, so 'Hi' -- no
        # descenders -- must sit entirely above y=90 and within one ascent of
        # it.  A flipped or offset blit fails this without needing an eye on the
        # PNG.
        box = lit.text_ink_bbox
        ascent = ras.shape(gtext, 16.0).ascent
        check("ink sits above the baseline it was given",
              box is not None and box[3] <= 90, str(box))
        check("ink starts within one ascent above the baseline",
              box is not None and box[1] >= 90 - int(math.ceil(ascent)) - 1,
              "%s ascent=%.2f" % (box, ascent))
        check("ink starts at the origin x, not before it",
              box is not None and box[0] >= 29, str(box))

        # Advance arithmetic.  The +4.0 is pinned by hand from the delta in the
        # golden frame (nonspace 1.5 on each of two characters, plus space 2.5
        # applied to nothing here... so 3.0), and the whitespace branch is
        # pinned separately below; neither number is derived from the code under
        # test.
        plain_run = ras.shape(gtext, 16.0)
        delta_run = ras.shape(gtext, 16.0, delta=(1.5, 2.5))
        check("an escapement_delta adds nonspace once per character",
              abs((delta_run.advance - plain_run.advance) - 3.0) < 1e-3,
              "%.4f vs %.4f" % (delta_run.advance, plain_run.advance))
        spaced = ras.shape(b"A B", 16.0)
        spaced_delta = ras.shape(b"A B", 16.0, delta=(1.0, 10.0))
        check("an escapement_delta uses the space half for whitespace",
              abs((spaced_delta.advance - spaced.advance) - 12.0) < 1e-3,
              "%.4f vs %.4f" % (spaced_delta.advance, spaced.advance))
        one = ras.shape(b"H", 16.0)
        two = ras.shape(b"HH", 16.0)
        check("advances accumulate linearly",
              abs(two.advance - 2 * one.advance) < 1e-3,
              "%.4f vs %.4f" % (two.advance, one.advance))
        check("B_BITMAP_SPACING advances by whole pixels",
              float(ras.shape(b"Hi", 16.0,
                              spacing=B_BITMAP_SPACING).advance).is_integer(),
              str(ras.shape(b"Hi", 16.0, spacing=B_BITMAP_SPACING).advance))

        # The pen position we hand back is what the server uses for whatever it
        # draws next, so it is measured, not guessed.
        pen = [pl for c, pl in lit.outbox if c == RP_DRAW_STRING_RESULT]
        check("RP_DRAW_STRING_RESULT carries the measured pen",
              len(pen) == 1
              and abs(struct.unpack("<Iff", pen[0])[1]
                      - (30.0 + delta_run.advance)) < 1e-3,
              pen[0].hex() if pen else "-")

        # RP_STRING_WIDTH: the server only asks a client that advertised
        # RP_CAP_STRING_WIDTH_REPLY, so the default is to count the query and
        # not answer it.
        query = frame(RP_STRING_WIDTH,
                      struct.pack("<I", gtoken)
                      + struct.pack("<I", len(gtext)) + gtext)
        quiet = run_stream(text_stream(extra=query), glyphs=ras)
        loud = run_stream(text_stream(extra=query), glyphs=ras,
                          answer_string_width=True)
        check("RP_STRING_WIDTH is counted either way",
              quiet.string_width_queries == 1 == loud.string_width_queries,
              "%d/%d" % (quiet.string_width_queries,
                         loud.string_width_queries))
        check("RP_STRING_WIDTH is not answered unless the capability was "
              "advertised",
              RP_STRING_WIDTH_RESULT not in [c for c, _ in quiet.outbox])
        widths = [pl for c, pl in loud.outbox if c == RP_STRING_WIDTH_RESULT]
        check("with --answer-string-width the reply is the measured width",
              len(widths) == 1
              and abs(struct.unpack("<If", widths[0])[1]
                      - plain_run.advance) < 1e-3,
              widths[0].hex() if widths else "-")

        # The pre-D5 wire shape (one delta per character) must be *visible*,
        # not silently tolerated: an extra delta on the wire means the sender
        # and this decoder disagree about the message, and a run decoded against
        # the wrong shape is not ground truth even if it produced ink.
        stale = run_stream(text_stream(deltas=2), glyphs=ras)
        check("a second escapement_delta is reported as trailing bytes",
              stale.text_trailing_bytes == 8, str(stale.text_trailing_bytes))
        check("trailing bytes after a text op forfeit glyph truth",
              stale.glyph_truth()[0] is False, stale.glyph_truth()[1])

        # RP_DRAW_STRING_WITH_OFFSETS: one origin per character, placed by the
        # server; the reply is the last origin plus that glyph's advance.
        offs = frame(RP_DRAW_STRING_WITH_OFFSETS,
                     struct.pack("<I", gtoken)
                     + struct.pack("<I", 3) + b"abc"
                     + struct.pack("<ff", 40.0, 60.0)
                     + struct.pack("<ff", 60.0, 60.0)
                     + struct.pack("<ff", 80.0, 60.0))
        with_offsets = run_stream(
            bytes(frame(RP_CREATE_STATE, struct.pack("<I", gtoken))
                  + frame(RP_SET_HIGH_COLOR, struct.pack("<I", gtoken)
                          + bytes((255, 255, 255, 255)))
                  + frame(RP_SET_FONT, font_payload(gtoken, 16.0)) + offs),
            glyphs=ras)
        check("WITH_OFFSETS rasterises one glyph per offset",
              with_offsets.text_glyphs == 3
              and with_offsets.text_runs_rasterised == 1,
              "glyphs=%d" % with_offsets.text_glyphs)
        wide = with_offsets.text_ink_bbox
        check("WITH_OFFSETS honours the server's own glyph origins",
              wide is not None and wide[0] >= 39 and wide[2] >= 80,
              str(wide))

        # Text painted in the colour it is painted over: present on the wire,
        # absent from the picture.  That is what #84's blank body looked like,
        # and a count of *written* ink pixels calls it healthy -- so the visible
        # count is what the assertion uses.
        onto_white = run_stream(
            bytes(frame(RP_CREATE_STATE, struct.pack("<I", gtoken))
                  + frame(RP_FILL_RECT_COLOR,
                          struct.pack("<Iffff", gtoken, 0.0, 0.0, 199.0, 119.0)
                          + bytes((255, 255, 255, 255)))
                  + frame(RP_SET_HIGH_COLOR, struct.pack("<I", gtoken)
                          + bytes((255, 255, 255, 255)))
                  + frame(RP_SET_FONT, font_payload(gtoken, 16.0))
                  + frame(RP_DRAW_STRING,
                          struct.pack("<I", gtoken)
                          + struct.pack("<ff", 30.0, 90.0)
                          + struct.pack("<I", 2) + b"Hi" + b"\x00")),
            glyphs=ras)
        check("invisible text is written ink with nothing changed",
              (onto_white.text_ink_pixels > 0
               and onto_white.text_ink_visible == 0
               and onto_white.text_invisible_runs == 1),
              "written=%d visible=%d runs=%d"
              % (onto_white.text_ink_pixels, onto_white.text_ink_visible,
                 onto_white.text_invisible_runs))

        # ---- the assertions, mutation tested ------------------------
        # Every check above this line lives in the same file as the code it
        # checks. These do not: they run the caller-facing assertions with
        # deliberately wrong expectations and require them to FAIL. An
        # assertion that cannot go red is the defect being fixed, not a test of
        # it (#423 shipped exactly that shape).
        class FakeArgs(object):
            def __init__(self, **kw):
                self.min_text_runs = 0
                self.min_glyph_ink = 0
                self.expect_text = []
                self.text_tolerance = 2.0
                self.__dict__.update(kw)

        check("--min-text-runs passes on a capture that drew text",
              text_expectation_failures(lit, FakeArgs(min_text_runs=1)) == [])
        check("--min-text-runs FAILS when text is missing (the #84 shape)",
              text_expectation_failures(blind, FakeArgs(min_text_runs=1)) != [])
        check("--min-glyph-ink FAILS on a text-blind capture",
              text_expectation_failures(blind, FakeArgs(min_glyph_ink=1)) != [])
        check("--min-glyph-ink passes on real ink",
              text_expectation_failures(lit, FakeArgs(min_glyph_ink=1)) == [])
        check("--min-glyph-ink FAILS on invisible text (written but unchanged)",
              text_expectation_failures(onto_white,
                                        FakeArgs(min_glyph_ink=1)) != [])
        check("--expect-text FAILS on invisible text",
              text_expectation_failures(
                  onto_white, FakeArgs(expect_text=["Hi@30,90"])) != [])
        check("--expect-text matches the run that was drawn",
              text_expectation_failures(
                  lit, FakeArgs(expect_text=["Hi@30,90"])) == [])
        check("--expect-text FAILS on a mispositioned run",
              text_expectation_failures(
                  lit, FakeArgs(expect_text=["Hi@50,90"])) != [])
        check("--expect-text FAILS on a dropped run",
              text_expectation_failures(
                  lit, FakeArgs(expect_text=["Tracker"])) != [])
        check("--expect-text FAILS when the run drew no glyphs",
              text_expectation_failures(
                  blind, FakeArgs(expect_text=["Hi@30,90"])) != [])
        check("--expect-text tolerance is honoured, not ignored",
              text_expectation_failures(
                  lit, FakeArgs(expect_text=["Hi@31,90"])) == []
              and text_expectation_failures(
                  lit, FakeArgs(expect_text=["Hi@31,90"],
                                text_tolerance=0.1)) != [])
        check("an '@' inside the text is not mistaken for a position",
              parse_text_expectation("a@b") == ("a@b", None, None),
              str(parse_text_expectation("a@b")))
        check("a position is parsed off the right of the text",
              parse_text_expectation("a@b@1,2") == ("a@b", 1.0, 2.0),
              str(parse_text_expectation("a@b@1,2")))

        # The layout guard, fed deliberately wrong facts.  Without this the
        # guard is untested code: deleting its 'H' assertions left the whole
        # self-test green.
        good_h = ras.glyph(ras.fonts.select(False, False, False)[0], 16 * 64,
                           ord("H"), False, False)
        check("the layout guard accepts a real face",
              font_selfcheck_problems(1000, 3000, b"Noto Sans", True,
                                      good_h) == [],
              str(font_selfcheck_problems(1000, 3000, b"Noto Sans", True,
                                          good_h)))
        bad_facts = [
            ("units_per_EM of 0", (0, 3000, b"Noto Sans", True, good_h)),
            ("units_per_EM of 2**31", (1 << 31, 3000, b"S", True, good_h)),
            ("no glyphs", (1000, 0, b"Noto Sans", True, good_h)),
            ("a family name of binary noise",
             (1000, 3000, b"\x01\xff", True, good_h)),
            ("an empty family name", (1000, 3000, b"", True, good_h)),
            ("a null glyph pointer", (1000, 3000, b"Noto Sans", False,
                                      good_h)),
            ("no 'H' at all", (1000, 3000, b"Noto Sans", True, None)),
            ("an 'H' with no coverage",
             (1000, 3000, b"Noto Sans", True,
              Glyph(0, 12, 10, 12, 10, b"\x00" * 120, 10.0, 10.0, False))),
            ("an 'H' rendered below the baseline",
             (1000, 3000, b"Noto Sans", True,
              Glyph(0, -4, 10, 12, 10, b"\xff" * 120, 10.0, 10.0, False))),
            ("an 'H' with an absurd advance",
             (1000, 3000, b"Noto Sans", True,
              Glyph(0, 12, 10, 12, 10, b"\xff" * 120, 4096.0, 10.0, False))),
            ("an 'H' with no outline in the font",
             (1000, 3000, b"Noto Sans", True,
              Glyph(0, 12, 10, 12, 10, b"\xff" * 120, 10.0, 10.0, True))),
        ]
        for label, facts in bad_facts:
            check("the layout guard rejects %s" % label,
                  font_selfcheck_problems(*facts) != [])
        broken = FontSet()
        broken.files[(False, False, False)] = os.path.abspath(__file__)
        refused = False
        try:
            GlyphRasteriser(broken)
        except OSError:
            refused = True
        check("a file that is not a font is refused, not half-used", refused)
        # ...and the guard is actually wired to the refusal: a predicate nobody
        # acts on is the same bug one level up.
        saved_guard = globals()["font_selfcheck_problems"]
        globals()["font_selfcheck_problems"] = lambda *a, **k: ["forced"]
        try:
            wired = False
            try:
                GlyphRasteriser(FontSet().scan())
            except OSError as exc:
                wired = "forced" in str(exc)
        finally:
            globals()["font_selfcheck_problems"] = saved_guard
        check("a layout-guard problem refuses the rasteriser", wired)

        check("the outline guard accepts a real 'H'",
              outline_selfcheck_problems(1, 12, True) == [],
              str(outline_selfcheck_problems(1, 12, True)))
        for label, facts in [("no contours", (0, 12, True)),
                             ("a garbage contour count", (4096, 12, True)),
                             ("no points", (1, 0, True)),
                             ("a garbage point count", (1, 70000, True)),
                             ("a null points pointer", (1, 12, False))]:
            check("the outline guard rejects %s" % label,
                  outline_selfcheck_problems(*facts) != [])
        check("the outline offset is proved on the running library, not assumed",
              outline_selfcheck_problems(
                  *ras._probe_outline(
                      ras.fonts.select(False, False, False)[0])) == [],
              str(ras._probe_outline(ras.fonts.select(False, False, False)[0])))

        # ---- font rotation, shear and false bold (#494) --------------
        print("  -- transformed text (#494) --")

        class AngleArgs(object):
            """The caller-facing assertion args, with nothing set by default."""
            def __init__(self, **kw):
                self.min_text_runs = 0
                self.min_glyph_ink = 0
                self.expect_text = []
                self.text_tolerance = 2.0
                self.expect_text_angle = []
                self.text_angle_tolerance = 12.0
                self.__dict__.update(kw)

        # The CONVENTION, pinned against hand-computed matrices.  Deriving the
        # expectation from FontTransform would make this a tautology; these four
        # are worked out from AGGTextRenderer.cpp:72-85 with a pencil, in y-DOWN
        # device space:
        #   a = -rotation in radians;  t = tan(90 - shear in radians)
        #   M = R * S = ((cos a, cos a * t - sin a), (sin a, sin a * t + cos a))
        # rotation=30 alone      -> a = -30 deg: (( .866025,  .5     ),
        #                                        (-.5     ,  .866025))
        # shear=45 alone         -> t = 1      -> ((1, 1), (0, 1))
        # both                   -> ((.866025, .866025+.5), (-.5, -.5+.866025))
        # rotation=90 alone      -> ((0, 1), (-1, 0))
        # The last one is the sign check that matters: a positive rotation must
        # send the advance UP the screen, because BFont rotation is
        # counter-clockwise and screen y grows downwards.
        golden_matrices = [
            (0.0, 90.0, (1.0, 0.0, 0.0, 1.0)),
            (30.0, 90.0, (0.8660254, 0.5, -0.5, 0.8660254)),
            (0.0, 45.0, (1.0, 1.0, 0.0, 1.0)),
            (30.0, 45.0, (0.8660254, 1.3660254, -0.5, 0.3660254)),
            (90.0, 90.0, (0.0, 1.0, -1.0, 0.0)),
        ]
        for rot, shear, want in golden_matrices:
            xf = FontTransform(rot, shear)
            got = (xf.m00, xf.m01, xf.m10, xf.m11)
            check("rotation=%g shear=%g composes to the hand-computed matrix"
                  % (rot, shear),
                  all(abs(a - b) < 1e-6 for a, b in zip(got, want)),
                  "%s vs %s" % (tuple(round(v, 7) for v in got), want))
        check("shear and rotation compose in the server's order (R*S, not S*R)",
              abs(FontTransform(30.0, 45.0).m01 - 1.3660254) < 1e-6
              and abs(FontTransform(30.0, 45.0).m01 - 0.3660254) > 0.5,
              "%.7f" % FontTransform(30.0, 45.0).m01)
        check("a positive rotation sends the advance UP the screen",
              FontTransform(90.0).apply(10.0, 0.0)[1] < -9.9,
              str(FontTransform(90.0).apply(10.0, 0.0)))
        check("shear=90 and rotation=0 is the identity, so untransformed text "
              "takes the untransformed path",
              FontTransform(0.0, 90.0).identity
              and not FontTransform(0.0, 89.0).identity
              and not FontTransform(0.1, 90.0).identity)
        # FreeType works in y-UP, so the matrix handed to it is conjugated by
        # diag(1,-1): the off-diagonal terms flip and the diagonal ones do not.
        # 16.16 fixed, so 1.0 is 65536 and 0.5 is 32768 -- pinned as integers.
        check("the FreeType matrix is the y-UP conjugate, in 16.16",
              FontTransform(90.0, 90.0).ft_matrix() == (0, -65536, 65536, 0),
              str(FontTransform(90.0, 90.0).ft_matrix()))
        check("the FreeType matrix of rotation=30 flips only the off-diagonal",
              FontTransform(30.0, 90.0).ft_matrix() == (56756, -32768, 32768,
                                                       56756),
              str(FontTransform(30.0, 90.0).ft_matrix()))

        # The fixture: one 7-glyph run, drawn flat and then turned a quarter
        # turn from the same origin.  7 glyphs because orientation needs a
        # direction and a direction needs two points.  Its own square canvas,
        # with the origin in the middle, so that a run turned ANY way stays
        # inside it -- an ink box truncated by the framebuffer edge would make
        # the geometry checks below depend on the canvas and not on the font.
        TURN_ORIGIN = (120.0, 120.0)

        def turned(rotation=0.0, shear=90.0, false_bold=0.0, text=b"Rotated",
                   origin=TURN_ORIGIN):
            data = bytes(
                frame(RP_CREATE_STATE, struct.pack("<I", gtoken))
                + frame(RP_SET_HIGH_COLOR, struct.pack("<I", gtoken)
                        + bytes((255, 255, 255, 255)))
                + frame(RP_SET_FONT,
                        font_payload(gtoken, 16.0, rotation=rotation,
                                     shear=shear, false_bold=false_bold))
                + frame(RP_DRAW_STRING,
                        struct.pack("<I", gtoken)
                        + struct.pack("<ff", origin[0], origin[1])
                        + struct.pack("<I", len(text)) + text + b"\x00"))
            cap = Capture(240, 240, clip=True, apply_offsets=False,
                          verbose=False, reply=True, glyphs=ras)
            pos = 0
            while pos + HEADER <= len(data):
                code, length = struct.unpack_from("<HI", data, pos)
                cap.handle(code, bytes(data[pos + HEADER:pos + length]))
                pos += length
            return cap

        flat = turned()
        quarter = turned(rotation=90.0)
        check("a rotated run is RASTERISED now, not refused",
              (quarter.text_runs_rasterised == 1
               and quarter.estimated_text_ops == 0
               and quarter.text_unsupported == {}),
              "rast=%d est=%d %s" % (quarter.text_runs_rasterised,
                                     quarter.estimated_text_ops,
                                     quarter.text_unsupported))
        check("a rotated run IS glyph truth",
              quarter.glyph_truth()[0] is True, quarter.glyph_truth()[1])
        check("a rotated run is counted as transformed",
              quarter.text_transformed_runs == 1
              and flat.text_transformed_runs == 0,
              "%d vs %d" % (quarter.text_transformed_runs,
                            flat.text_transformed_runs))

        # THE DISCRIMINATOR.  The flat run's ink box is wide and short; the
        # quarter-turned one's is narrow and tall.  Asserted on the boxes and
        # not on a count, because...
        flat_box = flat.text_ink_bbox
        turn_box = quarter.text_ink_bbox
        flat_w = flat_box[2] - flat_box[0]
        flat_h = flat_box[3] - flat_box[1]
        turn_w = turn_box[2] - turn_box[0]
        turn_h = turn_box[3] - turn_box[1]
        check("a flat run's ink box is wider than it is tall",
              flat_w > 2 * flat_h, "%dx%d" % (flat_w, flat_h))
        check("a quarter-turned run's ink box is TALLER than it is wide",
              turn_h > 2 * turn_w, "%dx%d" % (turn_w, turn_h))
        check("the turned ink box is not the flat one",
              turn_box != flat_box, "%s == %s" % (turn_box, flat_box))
        check("turning the run swaps the box's extents",
              abs(turn_h - flat_w) <= 3 and abs(turn_w - flat_h) <= 3,
              "flat %dx%d turned %dx%d" % (flat_w, flat_h, turn_w, turn_h))

        # The DECLARED box -- what the run claims to occupy, from the origin and
        # the face's ascent/descent -- has to turn with the ink, or the two
        # disagree for a reason that is this tool's fault and a reader has no way
        # to tell that from a real disagreement.
        flat_decl = flat.text_records[0]["declared_bbox"]
        turn_decl = quarter.text_records[0]["declared_bbox"]
        check("a flat run's declared box is wider than it is tall",
              (flat_decl[2] - flat_decl[0]) > 2 * (flat_decl[3] - flat_decl[1]),
              str(flat_decl))
        check("a quarter-turned run's declared box turns with the ink",
              (turn_decl[3] - turn_decl[1]) > 2 * (turn_decl[2] - turn_decl[0]),
              str(turn_decl))
        check("the declared box still contains the ink after turning",
              (turn_decl[0] <= turn_box[0] and turn_decl[1] <= turn_box[1]
               and turn_decl[2] >= turn_box[2] and turn_decl[3] >= turn_box[3]),
              "declared %s vs ink %s" % (turn_decl, turn_box))

        # ...because a COUNT cannot tell them apart.  This is the check that
        # says why the box check above has to exist: the same glyphs, the same
        # ink, the same origin, and only the geometry differs. Every assertion
        # this file had before #494 lived on the left-hand side of these.
        check("glyph counts CANNOT tell a rotated run from a flat one",
              quarter.text_glyphs == flat.text_glyphs == 7,
              "%d vs %d" % (quarter.text_glyphs, flat.text_glyphs))
        check("ink pixel counts CANNOT tell a rotated run from a flat one",
              abs(quarter.text_ink_visible - flat.text_ink_visible)
              < 0.1 * flat.text_ink_visible,
              "%d vs %d" % (quarter.text_ink_visible, flat.text_ink_visible))
        check("the two runs share their origin, so --expect-text CANNOT tell "
              "them apart either",
              quarter.text_records[0]["origin"]
              == flat.text_records[0]["origin"] == [120.0, 120.0],
              str(quarter.text_records[0]["origin"]))

        # The measured direction, read off the ink.  0 / 90 / 45 / -90 are the
        # angles asked for on the wire; the tolerance is the measurement's, and
        # the values are not derived from the matrix.
        for rot, want in [(0.0, 0.0), (90.0, 90.0), (45.0, 45.0),
                          (-90.0, -90.0), (180.0, 180.0)]:
            cap = turned(rotation=rot)
            axis = cap.text_records[0]["ink_axis_deg"]
            check("ink drawn at rotation=%g runs at %g degrees" % (rot, want),
                  axis is not None and angle_difference(axis, want) <= 3.0,
                  str(axis))
        check("a one-glyph run reports NO direction rather than a made-up 0",
              turned(text=b"X").text_records[0]["ink_axis_deg"] is None,
              str(turned(text=b"X").text_records[0]["ink_axis_deg"]))

        # SHEAR.  shear=90 is upright; away from it the tops of the glyphs slide
        # sideways, so the ink box grows horizontally while the run axis stays
        # flat.  Which WAY it slides is pinned: FontTransform gives
        # x' = x + y*tan(90-shear), and glyph ink is above the baseline (y < 0
        # in device space), so shear < 90 moves ink LEFT of the origin and
        # shear > 90 moves it right.
        lean_back = turned(shear=45.0)
        lean_fwd = turned(shear=135.0)
        check("a sheared run is rasterised and counted as transformed",
              (lean_back.text_runs_rasterised == 1
               and lean_back.text_transformed_runs == 1
               and lean_back.glyph_truth()[0] is True),
              lean_back.glyph_truth()[1])
        check("shear widens the ink box without turning the run",
              (lean_back.text_ink_bbox[2] - lean_back.text_ink_bbox[0]
               > flat_w + 4
               and angle_difference(
                   lean_back.text_records[0]["ink_axis_deg"], 0.0) <= 3.0),
              "%s vs flat width %d" % (lean_back.text_ink_bbox, flat_w))
        check("shear<90 leans the ink LEFT of the origin",
              lean_back.text_ink_bbox[0] < flat_box[0] - 4,
              "%d vs %d" % (lean_back.text_ink_bbox[0], flat_box[0]))
        check("shear>90 leans the ink RIGHT, past where upright text ended",
              lean_fwd.text_ink_bbox[2] > flat_box[2] + 4,
              "%d vs %d" % (lean_fwd.text_ink_bbox[2], flat_box[2]))
        check("the two shears lean opposite ways, so the sign is not ignored",
              lean_back.text_ink_bbox[0] < lean_fwd.text_ink_bbox[0]
              and lean_back.text_ink_bbox[2] < lean_fwd.text_ink_bbox[2],
              "%s vs %s" % (lean_back.text_ink_bbox, lean_fwd.text_ink_bbox))

        # FALSE BOLD.  agg conv_contour::width(falseBoldWidth * 2) offsets the
        # outline outward by falseBoldWidth (agg_vcgen_contour.h:54 +
        # agg_math_stroke.h:136-138), so the ink must grow by that many pixels
        # on EVERY side and the advance must not move at all.  2.0 in, 2 px per
        # side out: pinned from the specification, not measured and blessed.
        bold2 = turned(false_bold=2.0)
        bb = bold2.text_ink_bbox
        check("false_bold_width grows the ink by itself on every side",
              (abs((flat_box[0] - bb[0]) - 2) <= 1
               and abs((flat_box[1] - bb[1]) - 2) <= 1
               and abs((bb[2] - flat_box[2]) - 2) <= 1
               and abs((bb[3] - flat_box[3]) - 2) <= 1),
              "%s vs %s" % (bb, flat_box))
        check("false_bold_width puts down strictly more ink",
              bold2.text_ink_visible > flat.text_ink_visible * 1.5,
              "%d vs %d" % (bold2.text_ink_visible, flat.text_ink_visible))
        check("false_bold_width does NOT move the advance (conv_contour has no "
              "opinion about advances, and neither does app_server)",
              abs(ras.shape(b"Rotated", 16.0, false_bold=2.0).advance
                  - ras.shape(b"Rotated", 16.0).advance) < 1e-6,
              "%.4f vs %.4f"
              % (ras.shape(b"Rotated", 16.0, false_bold=2.0).advance,
                 ras.shape(b"Rotated", 16.0).advance))
        check("false_bold_width is counted as transformed even with no rotation",
              bold2.text_transformed_runs == 1,
              str(bold2.text_transformed_runs))

        # The pen we hand back must follow the rotated baseline: a quarter turn
        # from (120,120) ends straight UP at (120, 120 - advance), not to the
        # right at (120 + advance, 120).
        advance = ras.shape(b"Rotated", 16.0).advance
        turn_pen = [pl for c, pl in quarter.outbox
                    if c == RP_DRAW_STRING_RESULT]
        check("RP_DRAW_STRING_RESULT follows the rotated baseline",
              len(turn_pen) == 1
              and abs(struct.unpack("<Iff", turn_pen[0])[1] - 120.0) < 0.01
              and abs(struct.unpack("<Iff", turn_pen[0])[2]
                      - (120.0 - advance)) < 0.01,
              str(struct.unpack("<Iff", turn_pen[0])[1:])
              if turn_pen else "-")

        # RP_DRAW_STRING_WITH_OFFSETS under a rotated font.  The server's second
        # RenderString overload does not translate by a baseline
        # (AGGTextRenderer.cpp:415-416), so the offsets it sent are themselves
        # put through the embedded transform: three origins spaced along x come
        # out spaced along the rotated axis, AND displaced, because the rotation
        # is about the view origin rather than about the text.  That is
        # app_server's behaviour, surprising or not, and these are checks that we
        # copied it rather than checks that we like it -- a rotated
        # WITH_OFFSETS run genuinely leaves the offsets the server named.  20
        # degrees and not 90 only so the result stays on the canvas; at 90 the
        # whole run lands at negative y, which is worth knowing and is a
        # statement about app_server, not about this file.
        def offsets_run(rotation):
            data = bytes(
                frame(RP_CREATE_STATE, struct.pack("<I", gtoken))
                + frame(RP_SET_HIGH_COLOR, struct.pack("<I", gtoken)
                        + bytes((255, 255, 255, 255)))
                + frame(RP_SET_FONT,
                        font_payload(gtoken, 16.0, rotation=rotation))
                + frame(RP_DRAW_STRING_WITH_OFFSETS,
                        struct.pack("<I", gtoken) + struct.pack("<I", 3)
                        + b"abc" + struct.pack("<ff", 60.0, 120.0)
                        + struct.pack("<ff", 80.0, 120.0)
                        + struct.pack("<ff", 100.0, 120.0)))
            cap = Capture(240, 240, clip=True, apply_offsets=False,
                          verbose=False, reply=True, glyphs=ras)
            pos = 0
            while pos + HEADER <= len(data):
                code, length = struct.unpack_from("<HI", data, pos)
                cap.handle(code, bytes(data[pos + HEADER:pos + length]))
                pos += length
            return cap

        off_flat = offsets_run(0.0)
        off_turned = offsets_run(20.0)
        off_flat_axis = off_flat.text_records[0]["ink_axis_deg"]
        off_turn_axis = off_turned.text_records[0]["ink_axis_deg"]
        check("WITH_OFFSETS still runs flat when the font is upright",
              (off_flat.text_glyphs == 3 and off_flat_axis is not None
               and angle_difference(off_flat_axis, 0.0) <= 3.0),
              str(off_flat_axis))
        check("WITH_OFFSETS puts the SERVER's own origins through the font "
              "transform, as the server does",
              (off_turned.text_glyphs == 3 and off_turn_axis is not None
               and angle_difference(off_turn_axis, 20.0) <= 4.0),
              str(off_turn_axis))
        check("a turned WITH_OFFSETS run does not land in the flat one's box",
              off_turned.text_ink_bbox != off_flat.text_ink_bbox,
              "%s == %s" % (off_turned.text_ink_bbox, off_flat.text_ink_bbox))
        # The pen after a turned WITH_OFFSETS run has to be measured in the
        # space the glyphs were drawn in: transform(last offset + advance), not
        # last offset + transform(advance).  Those differ by the displacement
        # the view-origin rotation introduces, which is tens of pixels here.
        off_pen = [pl for c, pl in off_turned.outbox
                   if c == RP_DRAW_STRING_RESULT]
        off_xf = FontTransform(20.0)
        off_last_adv = ras.shape(b"abc", 16.0).glyphs[-1].advance
        want_pen = off_xf.apply(100.0 + off_last_adv, 120.0)
        check("RP_DRAW_STRING_RESULT for a turned WITH_OFFSETS run is in the "
              "space the glyphs were drawn in",
              len(off_pen) == 1
              and abs(struct.unpack("<Iff", off_pen[0])[1] - want_pen[0]) < 0.01
              and abs(struct.unpack("<Iff", off_pen[0])[2] - want_pen[1]) < 0.01,
              "%s want %s" % (struct.unpack("<Iff", off_pen[0])[1:]
                              if off_pen else "-",
                              tuple(round(v, 2) for v in want_pen)))
        check("--expect-text-angle discriminates WITH_OFFSETS runs too",
              text_expectation_failures(
                  off_turned, AngleArgs(expect_text_angle=["abc@20"])) == []
              and text_expectation_failures(
                  off_flat, AngleArgs(expect_text_angle=["abc@20"])) != [])

        # ---- the orientation assertion, mutation tested -------------
        check("--expect-text-angle passes on a run drawn at the stated angle",
              text_expectation_failures(
                  quarter, AngleArgs(expect_text_angle=["Rotated@90"])) == [])
        check("--expect-text-angle FAILS on a run drawn FLAT -- the #494 defect",
              text_expectation_failures(
                  flat, AngleArgs(expect_text_angle=["Rotated@90"])) != [])
        check("--expect-text-angle FAILS the other way round too (a turned run "
              "asserted flat)",
              text_expectation_failures(
                  quarter, AngleArgs(expect_text_angle=["Rotated@0"])) != [])
        check("--expect-text-angle passes a flat run asserted flat",
              text_expectation_failures(
                  flat, AngleArgs(expect_text_angle=["Rotated@0"])) == [])
        check("--expect-text-angle distinguishes +90 from -90",
              text_expectation_failures(
                  quarter, AngleArgs(expect_text_angle=["Rotated@-90"])) != []
              and text_expectation_failures(
                  turned(rotation=-90.0),
                  AngleArgs(expect_text_angle=["Rotated@-90"])) == [])
        check("--expect-text-angle FAILS on a run that drew no ink at all",
              text_expectation_failures(
                  blind, AngleArgs(expect_text_angle=["Hi@0"])) != [])
        check("--expect-text-angle FAILS on a one-glyph run rather than "
              "passing by default",
              text_expectation_failures(
                  turned(text=b"X"), AngleArgs(expect_text_angle=["X@90"]))
              != [])
        check("--expect-text-angle tolerance is honoured, not ignored",
              text_expectation_failures(
                  turned(rotation=80.0),
                  AngleArgs(expect_text_angle=["Rotated@90"])) == []
              and text_expectation_failures(
                  turned(rotation=80.0),
                  AngleArgs(expect_text_angle=["Rotated@90"],
                            text_angle_tolerance=2.0)) != [])
        check("--expect-text-angle rejects a spec with no angle in it",
              text_expectation_failures(
                  quarter, AngleArgs(expect_text_angle=["Rotated"])) != [])
        check("an '@' inside the text is not mistaken for an angle",
              parse_angle_expectation("a@b") is None,
              str(parse_angle_expectation("a@b")))
        check("an angle is parsed off the right of the text",
              parse_angle_expectation("a@b@90") == ("a@b", 90.0),
              str(parse_angle_expectation("a@b@90")))

        # ...and the assertion is wired to the IMPLEMENTATION, not merely to the
        # fixture.  Neuter FontTransform so every run comes out flat -- exactly
        # what the three clients did with these fields before #494 -- and the
        # orientation assertion must go red on bytes that used to pass it.  This
        # is the arm #423 shipped without: its wire check stayed green with the
        # opcode mutated, because nothing in it was pinned outside the code
        # under test.
        saved_transform = globals()["FontTransform"]

        class FlatTransform(saved_transform):
            def __init__(self, rotation=0.0, shear=90.0):
                saved_transform.__init__(self, 0.0, 90.0)

        globals()["FontTransform"] = FlatTransform
        try:
            ras._glyphs.clear()
            mutated = turned(rotation=90.0)
            mutated_fails = text_expectation_failures(
                mutated, AngleArgs(expect_text_angle=["Rotated@90"]))
            mutated_box = mutated.text_ink_bbox
            mutated_ink = mutated.text_ink_visible
            mutated_runs = mutated.text_runs_rasterised
        finally:
            globals()["FontTransform"] = saved_transform
            ras._glyphs.clear()
        check("MUTATION: with the font transform discarded, a rotated run "
              "still rasterises and still inks",
              mutated_runs == 1 and mutated_ink > 0,
              "runs=%d ink=%d" % (mutated_runs, mutated_ink))
        check("MUTATION: with the font transform discarded, --expect-text-angle "
              "goes RED",
              mutated_fails != [], str(mutated_fails))
        check("MUTATION: ...and it goes red because the ink came out flat, in "
              "the flat run's own box",
              mutated_box == flat_box, "%s vs %s" % (mutated_box, flat_box))
        check("MUTATION: --min-glyph-ink and --expect-text stay GREEN on that "
              "same mutated capture, which is why they were never going to "
              "catch this",
              text_expectation_failures(
                  mutated, AngleArgs(min_glyph_ink=1,
                                     expect_text=["Rotated@120,120"])) == [])
        check("the real transform is back after the mutation arm",
              turned(rotation=90.0).text_ink_bbox == turn_box,
              str(turned(rotation=90.0).text_ink_bbox))

    # UTF-8 decoding has to agree with the count the server sized its point
    # list by, or WITH_OFFSETS desynchronises.  Pinned, not derived.
    check("utf8_codepoints agrees with UTF8CountChars on multibyte text",
          len(utf8_codepoints("aé€".encode("utf-8")))
          == utf8_count_chars("aé€".encode("utf-8")) == 3,
          str(utf8_codepoints("aé€".encode("utf-8"))))
    check("utf8_codepoints decodes the expected codepoints",
          utf8_codepoints("aé€".encode("utf-8")) == [0x61, 0xE9, 0x20AC],
          str(utf8_codepoints("aé€".encode("utf-8"))))
    check("a truncated multibyte sequence still yields one entry per "
          "non-continuation byte",
          len(utf8_codepoints(b"a\xc3")) == utf8_count_chars(b"a\xc3") == 2,
          str(utf8_codepoints(b"a\xc3")))

    print("")
    # Coverage first, verdict second.  A green line that covered four checks
    # fewer than the last run is the failure mode this reports out of.
    print("SELFTEST_CHECKS=%d" % len(checks))
    print("SELFTEST_SKIPPED=%d" % len(skipped))
    for entry in skipped:
        print("  skipped: %s" % entry)

    if failures:
        print("SELFTEST=FAIL  (%d/%d checks failed: %s)"
              % (len(failures), len(checks), ", ".join(failures)))
        return 1
    if skipped and not allow_skip:
        print("SELFTEST=FAIL  (%d check group(s) did not run; pass "
              "--allow-skip to accept reduced coverage)" % len(skipped))
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
    p.add_argument("--require-glyph-truth", action="store_true",
                   help="DEFAULT since #475 and kept only for existing "
                        "callers: exit 5 unless every text run was rasterised "
                        "from real outlines. Use --allow-text-estimate to opt "
                        "out.")
    p.add_argument("--allow-text-estimate", action="store_true",
                   help="accept a capture whose text is estimated boxes (or "
                        "that has no rasteriser at all). Off by default: an "
                        "automated comparison must not silently treat a "
                        "text-blind capture as ground truth (#475).")
    p.add_argument("--allow-missing-glyphs", action="store_true",
                   help="accept codepoints the chosen font file has no outline "
                        "for. They are holes here and are not holes in the "
                        "server's render, so by default they forfeit ground "
                        "truth.")
    p.add_argument("--no-glyphs", dest="glyphs", action="store_false",
                   help="do not rasterise text; paint the old flat ESTIMATED "
                        "box instead. Exists to reproduce the pre-#475 "
                        "text-blind behaviour as the control arm of an A/B, "
                        "and it implies --allow-text-estimate is needed.")
    p.add_argument("--font", metavar="TTF",
                   help="use this file for the plain face instead of the "
                        "discovered one (app_server's default family is "
                        "\"Noto Sans\"; the wire cannot tell us the family)")
    p.add_argument("--font-bold", metavar="TTF",
                   help="file for the bold face (else synthesised)")
    p.add_argument("--font-fixed", metavar="TTF",
                   help="file for B_FIXED_SPACING runs")
    p.add_argument("--min-text-runs", type=int, default=0, metavar="N",
                   help="exit 6 unless at least N text runs were rasterised. "
                        "This is the assertion that catches text going missing "
                        "entirely -- the #84 shape, which no pixel count can "
                        "see. The threshold comes from you, not from the "
                        "decoder, so it can actually fail.")
    p.add_argument("--min-glyph-ink", type=int, default=0, metavar="N",
                   help="exit 6 unless at least N glyph ink pixels were "
                        "painted (text present but blank fails this)")
    p.add_argument("--expect-text", action="append", default=[],
                   metavar="TEXT[@X,Y]",
                   help="exit 6 unless a run with exactly this text was "
                        "rasterised, optionally within --text-tolerance of "
                        "origin X,Y. Repeatable. The positional form is the "
                        "one that catches mispositioned text.")
    p.add_argument("--text-tolerance", type=float, default=2.0, metavar="PX",
                   help="origin tolerance for --expect-text (default 2.0)")
    p.add_argument("--expect-text-angle", action="append", default=[],
                   metavar="TEXT@DEGREES",
                   help="exit 6 unless a run with exactly this text drew ink "
                        "running in the given direction (BFont rotation "
                        "degrees: counter-clockwise, 0 = left to right). "
                        "Repeatable. This is the only text assertion here that "
                        "can tell a rotated run from a horizontal one: "
                        "--expect-text checks an origin the two SHARE, and "
                        "--min-glyph-ink counts pixels both have the same "
                        "number of, so neither can fail on a client that "
                        "decodes font rotation and discards it (#494).")
    p.add_argument("--text-angle-tolerance", type=float, default=12.0,
                   metavar="DEG",
                   help="direction tolerance for --expect-text-angle "
                        "(default 12.0). Generous on purpose: this is here to "
                        "separate 'rotated' from 'not rotated', not to grade "
                        "sub-degree accuracy we do not claim.")
    p.add_argument("--answer-string-width", action="store_true",
                   help="advertise RP_CAP_STRING_WIDTH_REPLY and answer "
                        "RP_STRING_WIDTH from our own metrics. OFF by default "
                        "and deliberately: since the D1/D10 fix the server only "
                        "asks a client that advertised the bit and otherwise "
                        "uses its own authoritative metrics with no stall, so "
                        "answering would replace the server's layout metrics "
                        "with ours -- an instrument perturbing what it "
                        "measures. Use it only to exercise the query path.")
    p.add_argument("--selftest", action="store_true",
                   help="run the parser/PNG self-test and exit")
    p.add_argument("--allow-skip", action="store_true",
                   help="with --selftest: pass even if a check group could "
                        "not run (e.g. no libzstd). Off by default, so a "
                        "self-test that silently stops covering the wire "
                        "compression fails instead of printing PASS.")
    p.add_argument("--wss", action="store_true",
                   help="connect through the remote_broker daemon: TLS + "
                        "WebSocket + RP_AUTHENTICATE (implies --port 10902 "
                        "unless --port is given)")
    p.add_argument("--token", help="authentication token for --wss")
    p.add_argument("--token-file",
                   help="file containing the authentication token for --wss")
    p.add_argument("--cookie",
                   help="app_server's per-boot session cookie, required for a "
                        "direct connection to the session port (i.e. without "
                        "--wss).  Prefer --cookie-file: an argument is visible "
                        "in ps and in shell history")
    p.add_argument("--cookie-file",
                   help="file containing the session cookie.  On the server it "
                        "is <system settings>/remote_desktop/session_cookie."
                        "<port>, readable only by the user app_server runs as; "
                        "read it there (e.g. over SSM) and point this at a "
                        "local copy")
    p.add_argument("--no-cookie", action="store_true",
                   help="connect to a server that predates the session-cookie "
                        "gate. Its candidate gate accepts only "
                        "RP_INIT_CONNECTION or RP_HELLO as the first frame "
                        "(validate_first_frame before commit 26123a5521), so a "
                        "cookie frame is not merely unnecessary there -- it is "
                        "DROPPED, and the connection with it. Needed for any "
                        "image baked before that commit.")
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
        return selftest(allow_skip=args.allow_skip)

    if args.width <= 0 or args.height <= 0:
        p.error("--width/--height must be positive")

    glyphs = None
    if args.glyphs:
        try:
            fonts = FontSet().scan().override(regular=args.font,
                                              bold=args.font_bold,
                                              fixed=args.font_fixed)
            glyphs = GlyphRasteriser(fonts)
            print("GLYPH_RASTERISER_OK=%s" % glyphs.selfcheck)
        except (OSError, RuntimeError) as exc:
            # Loud, and it forfeits ground truth: the one outcome that must
            # never happen is silently blank text (#475).
            sys.stderr.write("rdcapture: no glyph rasteriser (%s); text will "
                             "be painted as ESTIMATED boxes and this capture "
                             "is NOT text ground truth\n" % exc)
            print("GLYPH_RASTERISER_ERROR=%s" % exc)

    cap = Capture(args.width, args.height, clip=args.clip,
                  apply_offsets=args.apply_offsets, verbose=args.verbose,
                  reply=args.reply, glyphs=glyphs,
                  answer_string_width=args.answer_string_width)

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

    cookie = args.cookie
    if args.cookie_file:
        try:
            with open(args.cookie_file) as f:
                cookie = f.read().strip()
        except OSError as exc:
            sys.stderr.write("rdcapture: cannot read cookie file: %s\n" % exc)
            return 2

    # The two secrets belong to two different hops; exactly one of them is ours
    # to send.  Refused rather than ignored, because "I passed the cookie and it
    # went nowhere" is the kind of silence that costs an afternoon.
    if args.wss and cookie:
        sys.stderr.write("rdcapture: --cookie/--cookie-file is for a direct "
                         "connection to the session port; with --wss the "
                         "broker presents the session cookie\n")
        return 2

    if args.no_cookie and (cookie or args.wss):
        sys.stderr.write("rdcapture: --no-cookie is for a direct connection to "
                         "a server that predates the cookie gate; it cannot be "
                         "combined with a cookie or with --wss\n")
        return 2

    if not args.wss and not cookie and not args.no_cookie:
        sys.stderr.write("rdcapture: a direct connection to the session port "
                         "requires app_server's per-boot session cookie: pass "
                         "--cookie-file (the server's "
                         "<system settings>/remote_desktop/session_cookie.%d), "
                         "or --wss to go through the broker, or --no-cookie "
                         "for an image that predates the gate\n" % args.port)
        return 2

    capabilities = CAP_COMPRESS_ZSTD if args.zstd else 0
    if args.answer_string_width:
        capabilities |= CAP_STRING_WIDTH_REPLY

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
                              capabilities=capabilities, cookie=cookie)
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
    if capabilities:
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

    # Text ground truth is now the DEFAULT posture, not an opt-in: a caller that
    # only checks the exit status is the one most likely to draw a wrong
    # conclusion from a text-blind capture, so it has to opt *out* (#475).
    truth_ok, truth_why = cap.glyph_truth(
        allow_missing_glyphs=args.allow_missing_glyphs)
    if not truth_ok and not args.allow_text_estimate:
        sys.stderr.write("rdcapture: not text ground truth: %s. Pass "
                         "--allow-text-estimate to accept a text-blind "
                         "capture\n" % truth_why)
        return 5

    failures = text_expectation_failures(cap, args)
    if failures:
        for line in failures:
            sys.stderr.write("rdcapture: %s\n" % line)
        return 6
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
