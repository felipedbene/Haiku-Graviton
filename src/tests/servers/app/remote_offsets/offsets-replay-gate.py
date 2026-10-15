#!/usr/bin/env python3
"""The #544 mutation gate, asserted ON THE WIRE.

The raster cannot discriminate (see remote_offsets.cpp's docstring: on the
full-invalidate repaint app_server re-teaches the origin by the ordinary drawing
path, replay or no replay). What the fix adds -- and what its removal takes away
-- is the RP_SET_OFFSETS in ReplayState(), so the assertion is made on the
message sequence instead.

THE ASSERTION: for every drawing state the burst draws through with a
non-identity TRANSFORM, RP_SET_OFFSETS for that state must precede the FIRST
DRAWING OP OF THE BURST -- it must be in the replay prologue, not merely
somewhere ahead of that state's own first op.

Both halves of that are load-bearing, and both were arrived at by watching a
weaker version pass on the broken build:

* Scoped to transformed states, because the offsets are only the origin the
  transform is conjugated about (p' = off + M * (p - off),
  Painter.cpp:372-383 and HaikuRemoteDesktop.js:1348-1352). On a state that
  never leaves the identity they move no pixel, and app_server has never sent
  them for such a state, before this change or after. Asserting over all states
  fails identically on both builds, on the decorator's untransformed region
  fills -- and a check that fails the same way on the fixed and the broken
  server measures nothing.

* Anchored to the first drawing op of the BURST, not of the state. "Offsets
  somewhere before this state's own first op" also passes on the broken build,
  and by luck rather than by correctness: the drawing engine survives the
  client's disconnect with fXOffset/fYOffset still holding the previous
  session's (340, 100), so the next SetTransform's (0, 0) reads as a CHANGE and
  the dedup guard lets it through -- the right message for the wrong reason,
  and only because that state happened to be left dirty. The property that
  actually distinguishes a replayed state from an accidentally re-taught one is
  ordering against the burst: a client must not be handed drawing ops before
  the state they render against has been replayed.

Record format (rdcapture --wire-dump): '<d' timestamp, '<HI' opcode/length,
payload at +14, stride 8 + length. Every drawing op and every state op begins
its payload with the '<I' state token.
"""

import struct
import sys

RP_SET_OFFSETS = 40
RP_SET_TRANSFORM = 49
RP_CONSTRAIN_CLIPPING_REGION = 60


# Opcode ranges that put ink on the client's framebuffer. 60 itself is the
# clipping region -- state, not ink -- and is excluded deliberately: it is
# installed before the transform and reads no offsets.
def is_drawing(code):
	if code == RP_CONSTRAIN_CLIPPING_REGION:
		return False
	return (61 <= code <= 182) or (260 <= code <= 279)


def records(path):
	blob = open(path, "rb").read()
	off = 0
	while off + 14 <= len(blob):
		code, length = struct.unpack_from("<HI", blob, off + 8)
		yield code, blob[off + 14:off + 8 + length]
		if length < 6:
			break
		off += 8 + length


def gate(path, label):
	# Pass one: which states ever carry a non-identity transform. An identity
	# RP_SET_TRANSFORM is a reset and conjugates nothing.
	transformed = set()
	for code, payload in records(path):
		if code == RP_SET_TRANSFORM and len(payload) >= 5 and payload[4] == 0:
			token, = struct.unpack_from("<I", payload, 0)
			transformed.add(token)

	# Pass two: the prologue -- everything up to the burst's first drawing op.
	prologue = set()
	drawn = set()
	head = []
	total = 0
	offsets_count = 0
	burst_open = True
	for code, payload in records(path):
		total += 1
		token = None
		if len(payload) >= 4:
			token, = struct.unpack_from("<I", payload, 0)
		if code == RP_SET_OFFSETS:
			offsets_count += 1
			ox, oy = struct.unpack_from("<ii", payload, 4)
			if burst_open:
				prologue.add(token)
			head.append(("OFFSETS", token, "(%d, %d)%s"
				% (ox, oy, "  <- prologue" if burst_open else "")))
		elif is_drawing(code):
			if token not in drawn:
				drawn.add(token)
				head.append(("FIRST-DRAW", token, "op %d" % code))
			burst_open = False

	print("=== %s ===" % label)
	print("  %s" % path)
	print("  records %d   RP_SET_OFFSETS %d   states drawn on %d   "
		"of which transformed %d"
		% (total, offsets_count, len(drawn), len(transformed)))
	ok = True
	for token in sorted(drawn):
		if token not in transformed:
			print("    token %-4d untransformed -- not in scope" % token)
			continue
		good = token in prologue
		if not good:
			ok = False
		print("    token %-4d offsets in the replay prologue: %-5s  %s"
			% (token, good, "PASS" if good else "FAIL"))
	for token in sorted(transformed - drawn):
		print("    token %-4d transformed but never drawn on" % token)
	print("  sequence head:")
	for kind, token, extra in head[:8]:
		print("    %-11s token=%-4d %s" % (kind, token, extra))
	print("  GATE: %s" % ("PASS" if ok else "FAIL"))
	return ok


if __name__ == "__main__":
	results = []
	for arg in sys.argv[1:]:
		path, _, label = arg.partition("=")
		results.append(gate(path, label or path))
		print()
	sys.exit(0 if all(results) else 1)
