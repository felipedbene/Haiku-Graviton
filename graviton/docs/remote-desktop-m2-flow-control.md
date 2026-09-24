# M2, flow-control half — `RP_TIER_END_FRAME` and a bounded, op-aware queue

Status: implemented and hardware-verified on Graviton. This is the second half of
**M2** in `remote-desktop-unified-design.md` §10; the first half (the measurement
gate) is `remote-desktop-m2-pricing.md`, and its central number is what this
design had to be built around.

The charter, verbatim from §10:

> Introduce `RP_TIER_END_FRAME` as a real frame boundary and a bounded,
> **op-aware** queue (queue messages not bytes; on overflow coalesce within a
> frame and drop *superseded* whole-frame content, never partial messages; worst
> case degrades to `RP_RESYNC`). Replaces "discard when nobody listens" with a
> policy.

---

## 0. The policy in one page

| | |
|---|---|
| **What is queued** | whole framed `RP` messages, never bytes |
| **Where** | `RemoteFlowQueue`, inside `RemoteWireWriter`, **upstream of the compressor** |
| **When it engages** | when the send ring has no reader, or less free space than the message plus one staging buffer |
| **Bound** | **32768 messages** and **4 MiB**, whichever is reached first, plus at most one oversized message |
| **At the bound** | coalesce → supersede → collapse, in that order |
| **Coalesce** | an `RP_SET_*` / `RP_CONSTRAIN_CLIPPING_REGION` / `RP_MOVE_CURSOR_TO` that a later message in the same frame, from the same emitter, for the same token, sets again with nothing in between that could have observed it |
| **Supersede** | a whole frame, if it is closed, contains nothing but pure pixel producers, declares a non-empty damage region, has no surface-reading op after it anywhere in the queue, and its damage is covered by the union of the damage of the later frames that will be kept |
| **Collapse** | drop everything queued and latch a debt: `RP_RESYNC` barrier + `ReplayState()` + a full repaint |
| **Never** | a partial message; a dropped state op; a dropped round-trip query; a silent discard |

The contract is deliberately **not** "nothing is ever lost". Nothing can hold an
unbounded stream for a client that may never arrive. The contract is:

> everything that is dropped is dropped by a rule, and any drop that cannot be
> proven invisible forces an `RP_RESYNC`.

---

## 1. The defect being replaced

The send ring is constructed with `discardWithoutReader`, so
`StreamingRingBuffer::Write()` **returns `B_OK` having written nothing** the
moment its reader goes away (`StreamingRingBuffer.cpp`, the
`fDiscardWithoutReader && fReader == NULL` branch). That branch is load-bearing
and stays: without it a drawing thread blocks forever on a buffer nobody drains,
which is the wedge that left the Deskbar black for a whole boot
(`remote-desktop-send-buffer-wedge.md`).

But it is also how content comes to be **believed-sent-and-never-delivered**.
Combined with `RemoteDrawingEngine`'s setter dedup — which will not re-send a
value it thinks the client already has — that is the server half of
black-screen-on-reconnect (D4), and the same shape as D1. A `Flush()` that
succeeds and delivers nothing is a lie the rest of the server is entitled to
believe.

M1 fixed the *reconnect* case by construction: a generation-stamped shadow and
`ReplayState()` re-state every engine unconditionally on accept. What M1 did not
do is give the *mid-session* case a policy. This does.

---

## 2. The compression problem, and why the frame boundary does not pay it

`remote-desktop-m2-pricing.md` measured, on this hardware:

| arm | ratio |
|---|---|
| one shared zstd stream over the whole session | **20.97 – 41.98×** |
| each frame compressed independently | **2.38 – 2.88×** |

A frame boundary that forced a flush-and-reset would hand back most of what zstd
buys, and the pricing half's recommendation was explicitly that "a policy that
drops whole frames must either keep compressor state across the drop or accept
that collapse."

> **Correction (settled by #546/#540, measured after this section was first
> written).** The two-arm table above is real, but the premise that once read in
> this section — that a per-message `ZSTD_e_flush` *delivers* the shared-stream
> arm — is wrong. Retaining the window is necessary, not sufficient: measured on
> hardware, per-message flush delivers **1.85×** (67.9 kB → 34.7 kB, n=3, same
> instance and boot; `remote-desktop-m2-pricing.md`), not the 20–42× a stream
> flushed once would, because the ratio tracks *messages-per-flush*, not merely
> whether the window is kept. So "shipped compression" is 1.85×, and the shared-
> stream arm is only reachable by flushing far less often (which #546's drain
> window does, batching several whole messages into one flush). This does not
> weaken the design below; it corrects *which number* the design is protecting.

This design keeps the compressor state, and it does it structurally rather than
by being careful:

- **`RP_TIER_END_FRAME` is an ordinary message, and only ever a marker.** It goes
  through `RemoteWireWriter::Write()` like every other message and is compressed
  like every other drawing op. It is **not** in `_IsPreCompressed()` (so it is
  never a raw segment) and **not** in `_MustFlushNow()` (so it never forces the
  drain window closed) — under #546 it is simply a few bytes carried inside
  whatever window is already open, and adds no new flush *class*, only one more
  ~10-byte message per composed frame. Nothing in this change calls `ZSTD_e_end`
  or `_ResetCodec()`; those remain reachable only from `Reset()`, at a connection
  boundary, where a fresh client needs a fresh window anyway.

  > **Condition on the no-ratio claim (required by #546).** "Boundary = marker,
  > never a forced flush" is the condition, not a description. Left as a marker,
  > `RP_TIER_END_FRAME` is *cheaper* than priced here — a few bytes in a shared
  > window. The moment anyone adds it to `_MustFlushNow()` — the natural reflex
  > when wiring a Tier P `RP_FRAME_ACK` pacing path that needs the boundary
  > delivered *promptly* — it becomes the window's terminator, and its cost is the
  > ratio given up by closing early, read off the messages-per-flush law: a fat
  > frame is ~free, a 2-message frame drags the shipped ratio back toward the
  > independent-frame arm. Prompt frame delivery and full compression are the same
  > trade-off named twice; a future Tier P path must choose deliberately.

- **The queue sits upstream of the compressor.** That is the whole reason a drop
  costs no ratio, and it is unaffected by the correction above: a message dropped
  from the queue *never entered the compressor*, so the window carries on across
  the drop unbroken. The client sees a different sequence of messages, not a
  restarted stream. A queue placed *behind* the compressor could not drop anything
  at all without resetting it — once bytes are in the window they cannot be taken
  back, which is the same argument `_IsPreCompressed()` already makes for why the
  raw-segment exemption has to be decided from the opcode before a byte is
  compressed.

**Measured, no longer merely plausible:** the shipped per-message-flush ratio was
re-measured for #546/#540 at **1.85×** (above); the queue-drop argument itself
rests on placement (the compressor cannot see a dropped message), which no
measurement is needed to establish. What is still *not* measured is the ratio with
the flow-control queue actively coalescing and dropping under load — that arm needs
the `zstd` build feature enabled, which these builds did not carry.

---

## 3. What a frame is

app_server has no "composition finished" event; `Desktop::MarkDirty` fans out and
never hears back. What it *does* have is `HWInterface::InvalidateRegion()`, which
every `DrawTransaction` destructor calls on its way out
(`DrawingEngine.cpp`, `DrawTransaction::~DrawTransaction`) and which
`Window::EndUpdate()` calls for a whole update session via
`DrawingEngine::CopyToFront()`. That call is the instant the client is told to
copy back to front, so it is by construction the instant at which everything
needed to make the display correct has been emitted. That is what a frame
boundary has to mean, and it is already on the wire as
`RP_INVALIDATE_RECT` / `RP_INVALIDATE_REGION`.

So:

- **`RP_TIER_END_FRAME` (opcode 282, payload `frame_seq:u32`)** is emitted from
  `RemoteHWInterface::InvalidateRegion()` / `Invalidate()`, immediately after the
  invalidate, **only for a client that negotiated `RP_CAP_FRAME_BOUNDARY`**
  (bit 3). It carries no damage region: the damage is in the message directly in
  front of it, from the same thread, and a second copy of the only expensive
  field would be bytes for nothing.
- **Flow control is not gated on the capability.** For a client that did not ask,
  the queue treats the invalidate itself as the boundary. Both are the same
  instant in the same thread's stream, so this is one rule with two spellings
  (`RemoteFlowQueue::FrameIsClose()`), not two policies.
- The capability is gated because the in-tree native client routes an unknown
  opcode to its `default:` case (D10's shape). A client that did not ask never
  sees one, and its byte stream is unchanged.

`frame_seq` is monotonic for the life of the interface, not per connection: it is
an identity a client can quote back (Tier P's `RP_FRAME_ACK`), and reusing
numbers across a reconnect would make two different frames answer to the same
name.

### Frames belong to their emitter

A frame is the run of messages **from one emitting thread** between two of that
thread's own boundaries — not a run of the byte stream.

This is not tidiness, it is soundness. Drawing engines run in parallel under the
`HWInterface` read lock, so engine B's ops land inside engine A's span while B's
damage is only declared later. If frames were a property of the stream, dropping
"A's frame" would drop B's ops whose pixels nothing has promised to repaint. The
unit test `testFramesArePerEmitter` is exactly that arrangement, and the mutation
that makes frames global (`e.owner = 0`) turns it red.

Within one emitter the span is either one transaction, or — across an update
session with copy-to-front disabled — several transactions whose union the closing
invalidate declares. Either way the closing damage is a statement about all of
them.

---

## 4. "Superseded", defined so it can be argued with

A queued frame *N* may be dropped **only if all six hold**:

1. **Closed.** Its boundary message is in the queue. An open frame's damage has
   not been declared yet.
2. **Nothing but pure pixel producers.** Every message in it classifies as
   `OP_DROPPABLE` (`RemoteFlowQueue::Classify`). One state op, one round-trip
   query, one unrecognised opcode, and the frame is pinned.
3. **Non-empty declared damage.** At least one `RP_INVALIDATE_*` inside it, whose
   rectangles parse and are integral. A frame that painted pixels and declared no
   damage is unprovable, so it is kept — a frame with zero damage would otherwise
   be "covered" by anything at all, which is the trap this clause closes.
4. **No surface-reading op after it, anywhere in the queue.** See below.
5. **Coverage.** Its damage region is geometrically contained in the union of the
   damage declared by the frames *after* it **that are being kept**. Dropped
   frames contribute no coverage, which is what makes the rule composable rather
   than a chain of wishes.
6. **Same emitter.** Coverage only counts from the same emitting thread (§3).

Clause 4 is the one a naive coverage test gets wrong, and the reason it exists is
concrete. `RP_COPY_RECT_NO_CLIPPING` is a **scroll**: its source is pixels an
earlier frame wrote. Drop that earlier frame and the copy moves *stale* content —
and the damage still looks covered, so a coverage-only rule would call the result
invisible when it is a visibly wrong screen. `RP_INVERT_RECT` and `RP_READ_BITMAP`
read the destination too. All three are classified `OP_BARRIER`, and **nothing
older than the newest barrier may be dropped.**

The same argument applies to blending, which is a *destination read* the opcode
does not reveal. The queue therefore tracks each token's drawing mode by watching
`RP_SET_DRAWING_MODE` go past — including messages it does not store, which is why
`Observe()` is called for every outbound message and not only for queued ones —
and **demotes a pixel op to `OP_BARRIER` whenever its token's mode is not known to
be `B_OP_COPY`.** "Not known" includes "never seen", so the conservative direction
is the default. `ReplayState()` emits `RP_SET_DRAWING_MODE` for every live engine
on accept, so within a connection the mode is known before any drawing; before the
first connection nothing is droppable, which is the right answer for a client that
does not exist.

Exempt from the mode check are the fast colour primitives
(`RP_FILL_RECT_COLOR`, `RP_STROKE_*_COLOR`, `RP_FILL_REGION_COLOR_NO_CLIPPING`),
which exist for the opaque case, and the frame/damage bookkeeping itself.

### Coalescing

Within a frame, an earlier `OP_COALESCABLE` message is removed when a later
message **in the same frame, from the same emitter, for the same token, with the
same opcode** sets the same thing again and nothing between them could have
observed the earlier value. The scan stops at the first same-emitter message that
is not itself coalescable — and because a frame's closing message is *droppable*,
not coalescable, that single condition is what keeps coalescing inside one frame.

Coalescable is a strictly weaker licence than droppable: these messages may be
*collapsed into their successor* but never dropped outright, because
`RemoteDrawingEngine` dedups against its shadow and would never re-send them. A
dropped setter is a client drawing with the wrong state for the rest of the
session — D4 from the other direction.

This is where the pricing half's colour-setter finding lands: 35 % of steady-state
idle bytes are `RP_SET_HIGH_COLOR`. Coalescing does not fix that (the fix is
server-side dedup, elsewhere), but it does mean an overflow spends its first
effort on bytes that provably nothing needs.

### Classification, and the default that matters

`Classify()` names every opcode, and the `default:` case is **`OP_PINNED`**. A
future opcode is therefore undroppable until someone writes down why it is safe —
the opposite of the usual accident, where a new opcode silently inherits whatever
the numeric range it landed in already meant. The mutation
`unknown-opcode-droppable` turns that clause red.

Two conservative choices worth naming:

- **`RP_DRAW_STRING` is pinned**, because it is a pixel producer *and* a round
  trip (`RP_DRAW_STRING_RESULT` carries the advance back) and a drawing thread can
  be parked on the reply. Dropping a query turns a dropped frame into a stalled
  desktop. The cost is that **a frame containing text is never dropped**. Text
  frames are the cheap ones (measured: the whole text path is 19 % of a typing
  capture at 15.8 kB/s), so the conservatism buys a rule with no exception to get
  wrong. It is the obvious first knob if supersession ever needs to fire more
  often.
- **`RP_SET_CURSOR` / `RP_SET_CURSOR_VISIBLE` are pinned**, `RP_MOVE_CURSOR_TO` is
  coalescable: cursor identity is not re-sent, cursor position is latest-wins.

---

## 5. The bound

**32768 messages and 4 MiB.**

4 MiB is ~128 cold first paints (measured: ~32 kB) and ~265 s of the heaviest
steady interactive stream this tree has measured (15.8 kB/s, text typing). 32768
messages is ~30 s of the highest message rate at that byte rate. The pair is
deliberately far above anything a healthy session produces: the queue exists for a
reader that stopped, not for ordinary burstiness, and a bound a real session can
reach is a bound that converts latency into policy for no reason. With the 1 MiB
send ring in front of it, a session holds at most ~5 MiB of undelivered stream.

**A single message is never split to fit.** If one message is larger than the
whole bound, the queue collapses and then holds that one message alone, so the
effective bound is "4 MiB plus one message". A larger bound is a cost; a split
message is unrecoverable. Repeated oversized messages do not accumulate, because
each one collapses first.

At the limit the order is fixed: **coalesce, then supersede, then collapse.**
Cheapest and most provably-invisible first.

### Collapse

`CollapseToResync()` drops everything queued and latches a debt. The debt is paid
as three steps, in this order:

1. `RP_RESYNC` barrier, for a client that negotiated `RP_CAP_RESYNC` — it says
   which generation the bytes behind it belong to, so a client caching content
   across connections knows what to throw away;
2. `ReplayState()`, which re-states every engine's shadow using existing opcodes
   only, so it repairs clients that have never heard of URP/1;
3. `_NotifyScreenChanged()`, the full repaint, which is what restores *what* was
   drawn rather than *how* to draw it.

**The repair runs on the event thread, and that restriction was found on hardware.**
The first implementation did steps 1 and 2 inline, from the drawing thread that
noticed the debt — inside a `DrawTransaction` destructor, with that engine's
exclusive lock already held. `_ReplayState()` then walks *every* registered engine
and takes each one's exclusive lock in turn; two drawing threads doing that
concurrently take the same two locks in opposite orders. On a real desktop the
result was not a dropped frame, it was **app_server's drawing threads deadlocked**:
the collapse was logged exactly once and the connected client then received **zero
messages in 180 s**. So the drawing thread only latches
(`RemoteHWInterface::_CheckResyncOwed`), and the event thread — the thread where
the client-requested `RP_RESYNC` path already does the identical three steps — does
the repair.

The cost is that the repair waits for the event thread's next inbound message.
Any session with a client attached is a session sending input, so in practice that
is immediate; **this is reasoned, not measured.** The far commoner case never
reaches there at all: a collapse while no client is attached is dropped at the
connection boundary, because `_NewConnection()` replays state and the arriving
client repaints the whole screen anyway.

---

## 6. Verification

### 6.1 Unit level, with a mutation matrix

The policy takes no locks (`RemoteWireWriter::fLock` already serialises every
writer, and the compressed stream requires that anyway), so it compiles and runs
off-target with a host compiler. Flow control that can only be exercised by
booting an image is flow control nobody tests.

```
src/tests/servers/app/remote_flow_queue/run.sh            -> PASS  78 checks, 0 failures
src/tests/servers/app/remote_flow_queue/mutation_test.sh  -> mutants killed 12, survived 0
```

The test includes the awkward cases by name: overflow with a frame left open,
state ops interleaved with droppable ones, a message larger than the whole bound,
coverage by the union of two later frames, an in-flight op from a second emitter,
an unknown drawing mode, a blending frame, fractional damage, and a 1101-message
session that never overflows (which must leave every counter at zero).

Both scripts include `RemoteProtocol.h` — the opcode table, split out of
`RemoteMessage.h` for exactly this reason — so the classification test runs against
the real vocabulary rather than a copy of it that can drift.

**Every rule has a mutation that kills it.** `mutation_test.sh` rewrites one rule
at a time in a throwaway copy and requires the suite to go red:

| mutant | what it breaks | killed by |
|---|---|---|
| `emit-partial-message` | `PeekFront` reports one byte less | 14 checks incl. the framing walk |
| `drop-non-superseded-frame` | skips the coverage test | 12 checks |
| `exceed-the-bound` | never enforces the bound | 22 checks |
| `drop-a-state-op` | `RP_SET_HIGH_COLOR` becomes droppable | 4 checks |
| `ignore-barriers` | a scroll no longer protects what it reads | 3 checks |
| `collapse-without-resync` | collapse stops owing a resync (the original silent discard) | 9 checks |
| `unknown-opcode-droppable` | `default:` becomes droppable | 2 checks |
| `trust-unknown-drawing-mode` | a blend is treated as opaque | 2 checks |
| `coalesce-across-a-draw-op` | coalesces past an observer | 3 checks |
| `frames-not-per-emitter` | frames become a stream property | 2 checks |
| `drop-frame-without-damage` | a frame with no declared damage becomes coverable (**two** guards, mutated together, because either alone is masked by the other) | 2 checks |
| `trust-fractional-damage` | non-integral edges enter inclusive-edge arithmetic | 1 check |

The "no partial message" instrument is a framing walker with **three positive
controls of its own**: it must reject a stream truncated by one byte, a header
promising bytes that are not there, and a length below the 6-byte header. Without
those, "no partial messages observed" would be unfalsifiable.

A mutation that matched more than one site is refused by `mutate.py` rather than
applied, so a mutant always breaks the rule it names.

### 6.2 Hardware

One on-demand `c7g.large`, `us-west-2`, canonical AMI, `hrev59996`, 1200×760.
`app_server` cross-built from this branch, pushed chunked and **sha256-verified**
(the image's sshd truncates large writes), selected by a
`~/config/settings/launch/` override, and **confirmed from `listimage`'s image
path on every boot, not assumed.** Instruments: `rdcapture.py --selftest` → **208
checks PASS** on the host and **208 PASS natively on arm64**; `rdlatency.py
--selftest` → **18 PASS** (unmodified).

**An ordinary session is unaffected — CONFIRMED.**

| arm (20 s) | messages | pixels touched | black px | truncated | frame boundaries |
|---|---|---|---|---|---|
| no `RP_CAP_FRAME_BOUNDARY` (negative control) | 1482 | 2 468 637 | 765 | 0 | **0** |
| `RP_CAP_FRAME_BOUNDARY` negotiated | 1409 | 1 831 136 | 765 | 0 | **4**, 0 gaps |

Server-side, for the same connection:

```
connection wire summary: msgs 1523 plain 57890 wire 57890
    queued 0 drained 0 coalesced 0 superseded 0 collapses 0
```

Zero of everything: **the queue is invisible when nothing overflows**, which is
the requirement. Black-pixel count is identical (765) with and without the
capability, so the extra opcode changes nothing a client renders.

**`RP_TIER_END_FRAME` is real on the wire — CONFIRMED, with both controls.**
`NEGOTIATED_CAPABILITIES=8` (bit 3), boundaries counted in strictly increasing
sequence with `FRAME_BOUNDARY_SEQ_GAPS=0` across every arm (4 to 157 boundaries),
`UNKNOWN_CODES=-` so the opcode is understood rather than skipped, and
`FRAME_OPS_MAX` between 56 and 223 drawing ops attributed to a single frame. The
negative control is the first row above: without the capability the count is 0, so
a non-zero count is a measurement. The instrument's own mutation control is in its
selftest — a renumbered boundary is detected as a gap.

**A starved reader triggers the policy, not a wedge — CONFIRMED.** A client that
completes the handshake, registers as the send ring's reader, then stops draining
while holding the socket open (`rdcapture.py --starve-after`), with app_server kept
drawing:

| arm | queued | coalesced | superseded | collapses | plain → wire | blocked-producer samples | truncated |
|---|---|---|---|---|---|---|---|
| shipped bound (32768 / 4 MiB), 300 s | **3861** | 0 | 0 | 0 | 1 100 144 → 902 139 | **0 / 30** | 0 |
| bound scaled to 64 / 64 KiB, 300 s | **12359** | **1080** | 0 | **176** | 1 461 357 → 839 096 | **0 / 30** | 0 |

- The queue engages rather than discarding: at the shipped bound 3861 messages
  (~198 kB) were held and accounted for instead of vanishing.
- **No drawing thread was ever blocked.** `listsem <app_server> | grep 'write
  notif'` never read `-1` in 60 samples across the two arms — the same reading that
  was `-1` in 12 of 12 stock first boots in `remote-desktop-send-buffer-wedge.md`.
  That published result is the positive control for this instrument; **it was not
  re-demonstrated firing in this session**, and that is a real gap in this report.
- `TRUNCATED_MESSAGES=0` in every capture: no partial message reached a client.
- The scaled-bound arm is the same code with two constants reduced, because filling
  4 MiB behind a 1 MiB ring on this workload takes tens of minutes. It is the arm
  that exercises coalesce-and-collapse; the shipped-bound arm shows the bound is
  generous enough never to reach them. Both are stated as what they are.

**Supersession did not fire on hardware.** `superseded 0` in both arms. That is
consistent with the rules — these workloads are full of pinned text ops and
`RP_COPY_RECT` barriers, and at a 64-message bound a frame rarely has a later
covering frame queued behind it — but it means **supersession is verified only at
unit level, with mutation controls, and not on a real desktop.** It is the
largest un-measured thing here.

**The black follow-up is pre-existing, and the A/B says so.** The starve arms were
driven by a load script that launches and `SIGKILL`s five applications in a loop.
After 150 rounds of that, a *fresh* client connecting afterwards got 149 messages,
0 pixels and a fully black screen. Running the identical script against the
**stock AMI `app_server`** reproduced it exactly — 149 messages, 0 pixels,
`BLACK_SCREEN=912000` — and the desktop stayed dead on the stock binary too. So it
is the load script destroying the desktop, not this policy. A cleaner
starve-then-reconnect arm was attempted and lost to the instance: the wedged
desktop stopped honouring `shutdown -r`, and a forced stop/start did not bring
sshd back. **That leg is owed.**

---

## 7. What this does not do

- **No ratio measurement with the queue engaged** (§2). The `zstd` build feature
  was not enabled in these builds, so every hardware arm ran the plain wire
  (`plain == wire` on the unqueued paths). The no-flush argument is structural.
- **Supersession is not hardware-observed** (§6.2).
- **The repair's latency is not measured** (§5). It waits for the event thread's
  next inbound message; that this is immediate in a real session is reasoned.
- **Tier P opcodes are classified but not exercised.** `RP_CODEC_TILE` and
  `RP_TIER_BEGIN_FRAME` are droppable-by-classification because the media channel
  is lossy-tolerant by design; nothing emits them yet.
- **The multi-emitter soundness argument is reasoned from the code**, not from a
  measurement of two engines racing. The unit test constructs the arrangement, but
  a synthetic arrangement is not a proof that app_server produces it.
- **`RemoteDrawingEngine`'s setter dedup is still not collapsing
  `RP_SET_HIGH_COLOR`** (the pricing half's 35 % finding). Coalescing mitigates it
  only under overflow; the fix belongs in the engine.
- **`_LargerThanTheRing()` keeps the old blocking write** for a message the ring
  could never hold whole. That path is unchanged from before this queue existed and
  is only reachable near a megabyte in one message, which the merged `DrawBitmap`
  crop makes rare. It was not exercised here.

---

## 8. Files

| file | role |
|---|---|
| `src/servers/app/drawing/interface/remote/RemoteFlowQueue.{h,cpp}` | the policy; no locks, host-compilable |
| `.../RemoteProtocol.h` | the opcode table and capability bits, split out of `RemoteMessage.h` so the test can include it |
| `.../RemoteWireWriter.{h,cpp}` | engagement, drain, and the resync debt; the queue lives here, upstream of the compressor |
| `.../RemoteHWInterface.{h,cpp}` | `RP_CAP_FRAME_BOUNDARY` negotiation, `RP_TIER_END_FRAME` emission, the event-thread repair |
| `.../StreamingRingBuffer.{h,cpp}` | `FreeSpace()`, `BufferSize()`, `HasReader()` — so a caller can ask "will this whole message fit" before starting to write it |
| `src/tests/servers/app/remote_flow_queue/` | `run.sh` (78 checks), `mutation_test.sh` (12 mutants), `mutate.py` |
| `graviton/scripts/rdcapture.py` | `--frame-boundary`, `--starve-after`, boundary/sequence-gap census, 7 new selftest checks with a mutation arm |
