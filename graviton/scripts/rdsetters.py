#!/usr/bin/env python3
"""rdsetters.py -- audit a captured URP/1 stream for redundant state setters.

WHY THIS EXISTS
---------------
The M2 pricing measurement found that 35 % of steady-state idle wire bytes are
``RP_SET_HIGH_COLOR`` messages, and inferred from that cost that
``RemoteDrawingEngine``'s setter dedup "is not collapsing them" -- so killing the
redundant colour setters was written down as the second-largest byte win
available, ahead of any codec.

That inference is wrong, and this tool is what establishes it.  A byte count says
what a message costs; it cannot say whether the message was *needed*.  Deciding
that needs the payloads, in order, per drawing-state token -- which is what this
reads out of a ``rdcapture.py --wire-dump`` file.

Measured on a c7g.large from the canonical AMI, 1200x760, 30 s idle, n=3: of 884
``RP_SET_HIGH_COLOR`` messages, **zero** repeat the colour already in effect for
their token, and in the steady-state window (2 s to 27 s) zero are even
superseded before use -- all 700 of them are genuine colour changes consumed
immediately by the next drawing op.  The Deskbar clock alone emits 870 setters in
48 distinct colours, because it is drawn as antialiased vector segments.  The
dedup in ``SetHighColor()`` is already correct and already collapsing everything
it can; the 35 % is irreducible cost, not waste.

So this tool's job changed from "find the waste" to "keep proving there is
none" -- and to find the setter that IS unguarded, which turned out to be
``RP_SET_OFFSETS`` (30 of 48 carrying a value already in effect, because
``SetDrawState()`` sends it with no guard at all).

TWO KINDS OF REDUNDANCY, COUNTED SEPARATELY
-------------------------------------------
``same-value`` (R1)
    The message sets a value equal to the one already in effect for that token.
    Pure waste, removable by a guard, and what every guarded setter in
    ``RemoteDrawingEngine`` already drives to zero.  A non-zero count here is a
    missing or broken guard.

``superseded`` (R2)
    The message is overwritten by another setter for the same token and op before
    any drawing op consumed it.  Also removable, but only by *deferring* the send
    until a consumer appears -- which splits the engine's shadow into "what was
    requested" and "what the client actually has".  That is the exact mechanism
    behind D4 (state believed sent but never delivered -> black screen on
    reconnect), so R2 is reported as an opportunity and deliberately not
    presented as a free one.

A "consumer" is an op that renders using the current state.  Ops that carry their
own colour in the payload (``RP_FILL_RECT_COLOR``, ``RP_STROKE_LINE_1PX_COLOR``,
``RP_FILL_REGION_COLOR_NO_CLIPPING``, the gradient fills) do NOT consume the
colour state and are excluded -- counting them as consumers would understate R2.

THE ABSENCE IS CONTROLLED
-------------------------
"No redundant colour setters remain" is a claim about an absence, so the
instrument has to be shown detecting one.  ``--inject-redundant N`` duplicates N
setter messages in the stream before auditing; the selftest requires the count to
rise by exactly N, and a mutation arm requires the audit to go blind when the
same-value comparison is removed.  Without that, a broken parser and a clean
stream are indistinguishable -- which in this tree has been the likelier of the
two more than once.

USAGE
-----
    rdsetters.py --selftest
    rdsetters.py idle.dump
    rdsetters.py idle.dump --from-s 2.0 --to-s 27.0
    rdsetters.py idle.dump --inject-redundant 50     # positive control
    rdsetters.py idle.dump --max-same-value 0        # use as a gate (exit 1)
"""

import argparse
import struct
import sys


# ---------------------------------------------------------------------------
# Opcodes.  Kept in step with src/servers/app/drawing/interface/remote/
# RemoteMessage.h; only the ops this audit reasons about are named.

RP_CREATE_STATE = 20
RP_DELETE_STATE = 21

# Per-token drawing-state setters.  Each is "token, then the new value", so the
# payload after the token IS the value and can be compared without knowing its
# shape -- which is what lets one loop audit all of them.
SETTERS = {
    40: "RP_SET_OFFSETS",
    41: "RP_SET_HIGH_COLOR",
    42: "RP_SET_LOW_COLOR",
    43: "RP_SET_PEN_SIZE",
    44: "RP_SET_STROKE_MODE",
    45: "RP_SET_BLENDING_MODE",
    46: "RP_SET_PATTERN",
    47: "RP_SET_DRAWING_MODE",
    48: "RP_SET_FONT",
    49: "RP_SET_TRANSFORM",
    60: "RP_CONSTRAIN_CLIPPING_REGION",
}

# Ops that render using the current state, and so consume a pending setter.
# Deliberately NOT included: RP_FILL_*_GRADIENT (120-128) and the *_COLOR ops
# (140-142, 160-161), which carry their own colour and leave the state unread.
CONSUMERS = (
    set(range(61, 65))          # COPY_RECT, INVERT_RECT, DRAW_BITMAP(_RECTS)
    | set(range(80, 90))        # STROKE_ARC .. STROKE_LINE_ARRAY
    | set(range(100, 109))      # FILL_ARC .. FILL_REGION
    | {180, 181}               # DRAW_STRING, DRAW_STRING_WITH_OFFSETS
)

# rdcapture.py --wire-dump record: float64 seconds since connect, then the whole
# RP frame (uint16 op, uint32 total_length, payload).  total_length includes the
# 6-byte header, so the smallest possible record is 8 + 6 = 14 bytes.
_RECORD_PREFIX = 8
_FRAME_HEADER = 6
_TOKEN_SIZE = 4


class Record(object):
    __slots__ = ("time", "op", "frame")

    def __init__(self, time, op, frame):
        self.time = time
        self.op = op
        self.frame = frame

    @property
    def token(self):
        """The drawing-state token, for the ops that carry one first."""
        if len(self.frame) < _FRAME_HEADER + _TOKEN_SIZE:
            return None
        return struct.unpack_from("<I", self.frame, _FRAME_HEADER)[0]

    @property
    def value(self):
        """Everything after the token: the value a setter is setting."""
        return self.frame[_FRAME_HEADER + _TOKEN_SIZE:]


def parse_dump(data):
    """Decode a wire dump into Records.

    Raises ValueError on a truncated or malformed stream rather than returning a
    short list, because "fewer records than expected" is exactly how a broken
    parser reports a clean stream.
    """
    records = []
    offset = 0
    while offset < len(data):
        if offset + _RECORD_PREFIX + _FRAME_HEADER > len(data):
            raise ValueError("truncated record at offset %d (%d bytes left)"
                             % (offset, len(data) - offset))
        timestamp = struct.unpack_from("<d", data, offset)[0]
        offset += _RECORD_PREFIX
        op, total = struct.unpack_from("<HI", data, offset)
        if total < _FRAME_HEADER:
            raise ValueError("frame at offset %d declares total_length %d, "
                             "below the %d-byte header"
                             % (offset, total, _FRAME_HEADER))
        if offset + total > len(data):
            raise ValueError("frame at offset %d declares %d bytes, only %d left"
                             % (offset, total, len(data) - offset))
        records.append(Record(timestamp, op, data[offset:offset + total]))
        offset += total
    return records


def inject_redundant(records, count, op=41):
    """POSITIVE CONTROL: duplicate up to `count` messages of `op` in place.

    A duplicate immediately after its original is redundant by construction: it
    sets the value that message just set.  If the audit does not see these, it
    would not see a real one either.
    """
    out = []
    injected = 0
    for record in records:
        out.append(record)
        if record.op == op and injected < count:
            out.append(Record(record.time, record.op, record.frame))
            injected += 1
    return out, injected


class Audit(object):
    def __init__(self):
        self.count = {}
        self.byte_count = {}
        self.same_value = {}
        self.superseded = {}
        self.trailing = {}
        self.distinct = {}

    def _bump(self, table, op, delta=1):
        table[op] = table.get(op, 0) + delta

    def ops(self):
        return sorted(self.count, key=lambda op: -self.byte_count[op])

    def total(self, table):
        return sum(table.values())


def audit(records, from_s=None, to_s=None, compare_values=True):
    """Count R1/R2 redundancy per setter op.

    `compare_values=False` is the mutation arm: it removes the same-value
    comparison, which must make the audit blind to injected duplicates.
    """
    result = Audit()
    effective = {}      # op -> token -> value currently in effect on the client
    pending = {}        # op -> token -> value sent but not yet consumed
    values = {}         # op -> token -> set of distinct values seen

    for record in records:
        if from_s is not None and record.time < from_s:
            continue
        if to_s is not None and record.time >= to_s:
            continue

        op = record.op
        if op in SETTERS:
            token = record.token
            if token is None:
                continue
            value = record.value
            result._bump(result.count, op)
            result._bump(result.byte_count, op, len(record.frame))
            values.setdefault(op, {}).setdefault(token, set()).add(value)

            if compare_values and effective.get(op, {}).get(token) == value:
                result._bump(result.same_value, op)
            if token in pending.get(op, {}):
                result._bump(result.superseded, op)

            pending.setdefault(op, {})[token] = value
            effective.setdefault(op, {})[token] = value
        elif op in CONSUMERS:
            token = record.token
            if token is None:
                continue
            for setter in pending:
                pending[setter].pop(token, None)
        elif op in (RP_CREATE_STATE, RP_DELETE_STATE):
            # A fresh or destroyed token has no state on the client, so nothing
            # carried over may be treated as still in effect.  Getting this wrong
            # would report a correct post-reconnect replay as redundant -- which
            # is the D4 failure read backwards.
            token = record.token
            if token is None:
                continue
            for setter in list(effective):
                effective[setter].pop(token, None)
            for setter in list(pending):
                pending[setter].pop(token, None)

    for op in result.count:
        result.trailing[op] = len(pending.get(op, {}))
        result.distinct[op] = max(
            (len(v) for v in values.get(op, {}).values()), default=0)
    return result


def report(result, label, stream=sys.stdout):
    write = stream.write
    write("--- setter audit: %s\n" % label)
    write("%-30s%8s%9s%14s%14s%10s\n"
          % ("op", "count", "bytes", "same-value", "superseded", "distinct"))
    for op in result.ops():
        write("%-30s%8d%9d%14d%14d%10d\n"
              % (SETTERS[op], result.count[op], result.byte_count[op],
                 result.same_value.get(op, 0), result.superseded.get(op, 0),
                 result.distinct.get(op, 0)))
    write("%-30s%8d%9d%14d%14d\n"
          % ("TOTAL", result.total(result.count), result.total(result.byte_count),
             result.total(result.same_value), result.total(result.superseded)))
    write("SETTER_BYTES=%d\n" % result.total(result.byte_count))
    write("SETTER_SAME_VALUE=%d\n" % result.total(result.same_value))
    write("SETTER_SUPERSEDED=%d\n" % result.total(result.superseded))
    write("SETTER_SAME_VALUE_BYTES=%d\n"
          % sum(result.same_value.get(op, 0)
                * (result.byte_count[op] // max(result.count[op], 1))
                for op in result.count))


# ---------------------------------------------------------------------------
# selftest

def _frame(op, token=None, value=b""):
    payload = b""
    if token is not None:
        payload = struct.pack("<I", token) + value
    return struct.pack("<HI", op, _FRAME_HEADER + len(payload)) + payload


def _dump(entries):
    out = b""
    for time, op, token, value in entries:
        frame = _frame(op, token, value)
        out += struct.pack("<d", time) + frame
    return out


def selftest():
    checks = []
    failures = []

    def check(label, ok, detail=""):
        checks.append(label)
        if ok:
            print("  ok    %s" % label)
        else:
            failures.append(label)
            print("  FAIL  %s  %s" % (label, detail))

    RED, GREEN, BLUE = b"\xff\x00\x00\xff", b"\x00\xff\x00\xff", b"\x00\x00\xff\xff"
    FILL_RECT, SET_HIGH = 104, 41

    print("  -- the parser --")
    data = _dump([(0.0, SET_HIGH, 7, RED), (0.1, FILL_RECT, 7, b"\x00" * 16)])
    records = parse_dump(data)
    check("two records round trip", len(records) == 2, str(len(records)))
    check("the opcode survives", records[0].op == SET_HIGH)
    check("the token survives", records[0].token == 7, str(records[0].token))
    check("the value survives", records[0].value == RED)
    check("the timestamp survives", abs(records[1].time - 0.1) < 1e-9)
    check("a whole-frame byte count includes the 6-byte header",
          len(records[0].frame) == 6 + 4 + 4, str(len(records[0].frame)))

    # The parser must FAIL on a short stream rather than silently return fewer
    # records: a truncated dump read as a clean one is a confident wrong zero.
    truncated = data[:-3]
    try:
        parse_dump(truncated)
        check("a truncated dump is rejected", False, "it was accepted")
    except ValueError:
        check("a truncated dump is rejected, not silently shortened", True)
    try:
        parse_dump(struct.pack("<d", 0.0) + struct.pack("<HI", SET_HIGH, 2))
        check("a frame shorter than its header is rejected", False)
    except ValueError:
        check("a frame shorter than its header is rejected", True)

    print("  -- a genuinely alternating stream has no same-value redundancy --")
    # RED fill, GREEN fill, RED fill: every setter is a real change consumed
    # immediately.  This is the shape the Deskbar clock actually produces.
    alternating = _dump([
        (0.0, SET_HIGH, 7, RED), (0.01, FILL_RECT, 7, b"a" * 16),
        (0.02, SET_HIGH, 7, GREEN), (0.03, FILL_RECT, 7, b"a" * 16),
        (0.04, SET_HIGH, 7, RED), (0.05, FILL_RECT, 7, b"a" * 16),
    ])
    a = audit(parse_dump(alternating))
    check("three setters counted", a.count[SET_HIGH] == 3,
          str(a.count.get(SET_HIGH)))
    check("same-value is ZERO -- alternation is not redundancy",
          a.same_value.get(SET_HIGH, 0) == 0)
    check("superseded is ZERO -- each was consumed before the next",
          a.superseded.get(SET_HIGH, 0) == 0)
    check("it saw 2 distinct colours on the token", a.distinct[SET_HIGH] == 2,
          str(a.distinct.get(SET_HIGH)))

    print("  -- and a redundant one is caught --")
    redundant = _dump([
        (0.0, SET_HIGH, 7, RED), (0.01, FILL_RECT, 7, b"a" * 16),
        (0.02, SET_HIGH, 7, RED), (0.03, FILL_RECT, 7, b"a" * 16),
    ])
    r = audit(parse_dump(redundant))
    check("the repeat of an in-effect colour is same-value redundancy",
          r.same_value.get(SET_HIGH, 0) == 1, str(r.same_value.get(SET_HIGH)))
    check("...and it is not double-counted as superseded",
          r.superseded.get(SET_HIGH, 0) == 0)
    # The wasted-byte figure, asserted rather than merely printed: a setter frame
    # here is 6 + 4 + 4 = 14 bytes, and exactly one was wasted.
    wasted = sum(r.same_value.get(op, 0)
                 * (r.byte_count[op] // max(r.count[op], 1)) for op in r.count)
    check("the wasted-byte figure is the frame size, not the payload size",
          wasted == 14, str(wasted))

    print("  -- superseded is a different count from same-value --")
    # Two DIFFERENT colours with no consumer between them: the first was never
    # used, but it was not a repeat either.
    sup = _dump([
        (0.0, SET_HIGH, 7, RED), (0.01, SET_HIGH, 7, GREEN),
        (0.02, FILL_RECT, 7, b"a" * 16),
    ])
    s = audit(parse_dump(sup))
    check("a setter overwritten before use counts as superseded",
          s.superseded.get(SET_HIGH, 0) == 1, str(s.superseded.get(SET_HIGH)))
    check("...and NOT as same-value, because the value differed",
          s.same_value.get(SET_HIGH, 0) == 0)

    print("  -- tokens are independent, as the client's per-token state is --")
    # Token 7 red, token 8 red, token 7 red again.  Nothing here is redundant:
    # each token has its own state on the client.  Auditing across tokens would
    # report the third as a repeat and invent a saving that does not exist.
    twotoken = _dump([
        (0.0, SET_HIGH, 7, RED), (0.01, FILL_RECT, 7, b"a" * 16),
        (0.02, SET_HIGH, 8, RED), (0.03, FILL_RECT, 8, b"a" * 16),
        (0.04, SET_HIGH, 7, GREEN), (0.05, FILL_RECT, 7, b"a" * 16),
    ])
    t = audit(parse_dump(twotoken))
    check("per-token state means no cross-token false positives",
          t.same_value.get(SET_HIGH, 0) == 0, str(t.same_value.get(SET_HIGH)))

    print("  -- ops that carry their own colour are not consumers --")
    # RP_FILL_RECT_COLOR (160) carries a colour in its payload and leaves the
    # state unread, so it must NOT clear a pending setter.
    own_colour = _dump([
        (0.0, SET_HIGH, 7, RED), (0.01, 160, 7, b"a" * 20),
        (0.02, SET_HIGH, 7, GREEN), (0.03, FILL_RECT, 7, b"a" * 16),
    ])
    o = audit(parse_dump(own_colour))
    check("an inline-colour fill does not consume the colour state, so the "
          "first setter is still superseded",
          o.superseded.get(SET_HIGH, 0) == 1, str(o.superseded.get(SET_HIGH)))

    print("  -- RP_CREATE_STATE clears the shadow (a replay is not redundant) --")
    # This is D4 read backwards: after a reconnect the server replays state into
    # a fresh token, and an auditor that carried the old value forward would
    # report the correct replay as waste and invite someone to remove it.
    replay = _dump([
        (0.0, SET_HIGH, 7, RED), (0.01, FILL_RECT, 7, b"a" * 16),
        (0.02, RP_CREATE_STATE, 7, b""),
        (0.03, SET_HIGH, 7, RED), (0.04, FILL_RECT, 7, b"a" * 16),
    ])
    p = audit(parse_dump(replay))
    check("a replayed setter after RP_CREATE_STATE is NOT same-value redundancy",
          p.same_value.get(SET_HIGH, 0) == 0, str(p.same_value.get(SET_HIGH)))
    # MUTATION of that rule: if RP_CREATE_STATE did not clear the shadow, the
    # replay would be miscounted -- the false positive that would have argued
    # for reintroducing D4.
    mutated = audit([rec for rec in parse_dump(replay)
                     if rec.op != RP_CREATE_STATE])
    check("MUTATION: with RP_CREATE_STATE removed the replay IS miscounted as "
          "redundant -- so that rule is load-bearing, not decoration",
          mutated.same_value.get(SET_HIGH, 0) == 1,
          str(mutated.same_value.get(SET_HIGH)))

    print("  -- the time window --")
    windowed = _dump([
        (0.0, SET_HIGH, 7, RED), (0.5, FILL_RECT, 7, b"a" * 16),
        (5.0, SET_HIGH, 7, GREEN), (5.5, FILL_RECT, 7, b"a" * 16),
    ])
    w = audit(parse_dump(windowed), from_s=2.0)
    check("--from-s drops the earlier setter", w.count[SET_HIGH] == 1,
          str(w.count.get(SET_HIGH)))
    w2 = audit(parse_dump(windowed), to_s=2.0)
    check("--to-s drops the later one", w2.count[SET_HIGH] == 1,
          str(w2.count.get(SET_HIGH)))
    check("--to-s is exclusive at the bound",
          audit(parse_dump(windowed), from_s=5.0).count[SET_HIGH] == 1)

    print("  -- POSITIVE CONTROL: injected redundancy must be seen --")
    base = parse_dump(alternating)
    clean = audit(base)
    injected, n = inject_redundant(base, 3)
    dirty = audit(injected)
    check("the injector duplicated exactly 3 setters", n == 3, str(n))
    check("the clean stream reports 0 same-value", clean.total(clean.same_value) == 0)
    check("POSITIVE CONTROL: the injected stream reports exactly 3 -- so a zero "
          "on a real capture is an absence, not a blind instrument",
          dirty.same_value.get(SET_HIGH, 0) == 3,
          str(dirty.same_value.get(SET_HIGH)))
    check("...and the message count rose by 3 too",
          dirty.count[SET_HIGH] - clean.count[SET_HIGH] == 3)

    print("  -- MUTATION: remove the comparison and the audit goes blind --")
    blind = audit(injected, compare_values=False)
    check("MUTATION: with the same-value comparison removed the injected "
          "duplicates are NOT reported",
          blind.same_value.get(SET_HIGH, 0) == 0,
          str(blind.same_value.get(SET_HIGH)))
    check("MUTATION: ...so the positive-control assertion goes RED",
          not (blind.same_value.get(SET_HIGH, 0) == 3))
    check("MUTATION: ...while the plain message COUNT stays green, which is "
          "exactly why a byte census could not have answered this question",
          blind.count[SET_HIGH] == dirty.count[SET_HIGH])
    check("the real audit is back after the mutation arm",
          audit(injected).same_value.get(SET_HIGH, 0) == 3)

    print("")
    print("SELFTEST_CHECKS=%d" % len(checks))
    if failures:
        print("SELFTEST=FAIL  (%d/%d failed: %s)"
              % (len(failures), len(checks), ", ".join(failures)))
        return 1
    print("SELFTEST=PASS")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Audit a URP/1 wire dump for redundant drawing-state "
                    "setters. See the module docstring for why a byte census "
                    "cannot answer this.")
    parser.add_argument("dump", nargs="?",
                        help="a rdcapture.py --wire-dump file")
    parser.add_argument("--from-s", type=float, default=None,
                        help="ignore records before this many seconds "
                             "(2.0 excludes the cold first paint)")
    parser.add_argument("--to-s", type=float, default=None,
                        help="ignore records at or after this many seconds")
    parser.add_argument("--inject-redundant", type=int, default=0,
                        metavar="N",
                        help="POSITIVE CONTROL: duplicate N setter messages "
                             "before auditing; the count must rise by N")
    parser.add_argument("--inject-op", type=int, default=41,
                        help="which op --inject-redundant duplicates "
                             "(default 41 = RP_SET_HIGH_COLOR)")
    parser.add_argument("--max-same-value", type=int, default=None,
                        metavar="N",
                        help="exit 1 if more than N same-value setters are "
                             "found; use as a regression gate")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.dump:
        parser.error("a dump is required unless --selftest is given")

    with open(args.dump, "rb") as handle:
        records = parse_dump(handle.read())
    label = args.dump
    if args.inject_redundant:
        records, n = inject_redundant(records, args.inject_redundant,
                                      args.inject_op)
        label += " [+%d INJECTED redundant op %d]" % (n, args.inject_op)
    if args.from_s is not None or args.to_s is not None:
        label += " [%s, %s)" % (args.from_s, args.to_s)

    result = audit(records, from_s=args.from_s, to_s=args.to_s)
    report(result, label)

    if args.max_same_value is not None:
        found = result.total(result.same_value)
        if found > args.max_same_value:
            print("GATE=FAIL  %d same-value setters, limit %d"
                  % (found, args.max_same_value))
            return 1
        print("GATE=PASS  %d same-value setters, limit %d"
              % (found, args.max_same_value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
