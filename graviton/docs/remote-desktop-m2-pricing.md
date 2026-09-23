# M2 measurement gate — per-opcode byte census and encoder pricing

Status: **measurement report**. This is the M2 gate of
[`remote-desktop-unified-design.md`](remote-desktop-unified-design.md) §10, which
specified it verbatim:

> **M2 — pricing + flow control.** Run the specified experiments (per-opcode byte
> census, encoder pricing on real captured frame sequences, `ssh -C`, the headless
> stall) to choose codec-on-bitmap vs full-frame video with numbers.

It delivers those numbers and a recommendation. It does **not** implement a codec
path — choosing one was the task, and the numbers argue against building either
candidate next.

Everything here was measured on hardware we rent. Conclusions are labelled
**CONFIRMED** (observed) or **PLAUSIBLE** (reasoned). §8 lists what was *not*
measured, and §2.1 lists the three places this campaign's own instrument was wrong
before it was right — which is reported because in this tree a broken instrument
has been a likelier explanation than a broken OS.

---

## 0. The answer, in six lines

1. **Neither yet.** Codec-on-bitmap and full-frame video are both premature.
   **CONFIRMED.**
2. **On interactive desktop workloads the bitmap path is 0–9.7 % of steady-state
   wire bytes** (0 % for typing and menus), and the vector/state remainder — 90 %
   or more — already compresses **15–42×** with a generic stream compressor.
   **CONFIRMED.**
3. **Even on a deliberately photographic workload, where the bitmap path *is*
   66.8 % of the stream, a codec on it loses to plain stream compression by 6.6×**
   (§4.4). The pixels repeat across messages; per-image coding throws that away
   and a stream compressor keeps it. **CONFIRMED.**
4. **`ssh -C` already collects most of the win on vector-shaped streams: 5.19–6.86×
   measured on interactively-shaped real bytes.** It is one character and already
   in the path. **CONFIRMED.**
5. **The one change the numbers strongly support is turning on the zstd capability
   that already exists in the tree** (`RP_CAP_COMPRESS_ZSTD`). On Graviton it beats
   `ssh -C`-class zlib-6 by **9 % on idle and 3.9× on bitmap-heavy content, at 7–24×
   *less* CPU**. **CONFIRMED.**
6. **The headless stall (D1/D10) is closed.** An app that draws 980 text runs
   reaches quiescence in **1.11 s** with no client attached — a 1 s-per-run stall
   would predict ≥ 980 s, so it is falsified by ~880×. Separately, `app_server`
   issued **zero** `RP_STRING_WIDTH` queries in ~300 s even to a client that
   advertised the capability, which corroborates **#534**: the override that would
   send the query shadows a non-virtual base method and is unreachable.
   **CONFIRMED**, with the missing mechanism control reported honestly in §5.3.

---

## 1. What was measured, and on what

| | |
|---|---|
| Instances | two on-demand **`c7g.large`** (Graviton 3), `us-west-2`, both from the canonical AMI — resolved by the `canonical=true` tag, which agreed with the SSM parameter (`ami-032bdab7af564b2ea`) |
| Image | `hrev59996`, arm64 |
| Screen | **1200×760**, *verified* rather than assumed — §1.1 |
| Server | `app_server`'s `RemoteHWInterface` on loopback `:10900`, with the per-boot session cookie |
| Client | `graviton/scripts/rdcapture.py`, run **on the instance**, so the bytes counted are the bytes `app_server` emits — no tunnel, no websockify, no host-side compression in the way |
| Trials | 3 per workload for the census, 5 for the stall arms; tables quote the range across trials |

Arms compared against each other were always captured on the **same instance and
the same boot** — this project has had numbers confounded by burst credits,
per-flow caps and boot-to-boot drift before. The two instances ran two *different*
experiment sets, not two halves of one comparison. Both were rebooted to a
pristine desktop before the definitive runs, for the reason in §2.1.

### 1.1 The screen size, stated first because getting it wrong has already cost this project a false verdict

`RemoteHWInterface` takes its display mode from the client, so `--width/--height`
*is* the screen — but only if the server honoured it, and an event aimed one pixel
off the screen is correctly ignored, producing silence that reads exactly like a
dead feature. That mistake produced a false "feature dead" verdict once already, on
a run that aimed every wheel event at `(640,400)` on a 640×480 screen.

The capture reports the Deskbar clock drawn at origin `x = 1140` and an ink bounding
box reaching `x = 1189` on a screen declared 1200 wide; top-right furniture painted
at the top right. **CONFIRMED: the screen is 1200×760 and every coordinate used is
on it.** `rdcapture.py` additionally **refuses** to run a workload any of whose
points fall outside the declared screen (`off_screen_points()`), and its selftest
reproduces the exact `(640,400)`-on-640×480 trap plus a mutation arm showing that a
`<=` bound would have accepted it.

---

## 2. The instruments

Each was run with `--selftest` before use, and every extension added for this
campaign added checks, including mutation arms.

| instrument | selftest before | selftest after | added for M2 |
|---|---|---|---|
| `graviton/scripts/rdcapture.py` | **201 PASS** | **271 PASS** | per-opcode **byte** census (it counted only *occurrences* before), `--wire-dump`, an interactive workload driver with an on-screen coordinate guard, `--workload-rect`, `--workload-loop`, `--advertise-string-width-no-answer`, string-width inter-query gap timing |
| `graviton/scripts/rdlatency.py` | **18 PASS** | 18 PASS (unmodified) | — |
| `graviton/scripts/rdprice.py` (**new**) | — | **26 PASS** | offline pricing: stream / per-frame / bitmap-only / remainder arms, time windowing, cadence replay, a self-contained PNG encoder, zstd via module *or* `libzstd` through `ctypes` |

`rdcapture.py --selftest` passes **271/271 natively on arm64**, and `rdprice.py`
passes **26/26** there on the `libzstd`-through-`ctypes` backend — so the code that
produced the arm64 CPU numbers is the same code that produced the x86 ones, which
is the only thing that makes the two machines comparable.

**Two known traps were respected explicitly.** `rdlatency.py` deliberately
advertises no capability bits, so nothing here concludes anything about a negotiated
feature from it; the capability-advertising client is `rdcapture.py`
(`--answer-string-width`, and the new `--advertise-string-width-no-answer`). The
off-screen coordinate trap is §1.1.

New runner scripts: `rdcensus-run` (bare-desktop baseline), `rdcensus-run3`
(window-targeted, looped), `rdsshc-run` (real `ssh -C`), `rdstall-run` and
`rdstall2-run` (the headless-stall arms).

### 2.1 Three times the instrument was wrong, and what caught it

Reported in full because the charter asks for it, because all three are the same
class of failure this project keeps hitting, and because each one would have
produced a confident wrong number.

1. **The interactive arms did not discriminate.** `text`, `menu` and `scroll`, run
   against a **bare desktop with no window open**, produced op profiles within a few
   percent of each other: typing had no focused text view, wheel events were over
   nothing scrollable, the click landed on the root view. It measured "the mouse
   moved" three times under three names. *Caught by* diffing per-opcode profiles
   **across** arms rather than reading each arm's total alone. *Fixed by* launching a
   real application, **discovering** where its window landed from the capture's own
   per-token drawn bounding box (a measurement, not an assumption — `hey` returns
   success and prints nothing on this image), and aiming the workload into that rect.
2. **The "steady state" window landed in the idle tail.** The workloads lasted
   2.4–9 s inside a 30 s capture, so a window past the cold first paint measured the
   desktop sitting idle again, and all four arms converged on ~1.1 kB/s — the idle
   rate — for the second time in the campaign and for an entirely different reason.
   *Fixed by* `--workload-loop`, which repeats the action sequence until it fills the
   capture; the final arms run 27.9–31.1 s of activity in a 30 s capture.
3. **Applications leaked and contaminated the desktop.** `hey <app> quit` **returns
   success and does not quit** on this image, and a first cleanup attempt parsed the
   wrong `ps` field (`$2` is an *argument*, because the command column contains
   spaces; the pid is `$(NF-3)`), so it killed nothing silently. By the heavy trials
   there were **three NetSurf processes painting at once** plus a litter of crash
   alerts, all contributing bytes to a census meant to measure one application.
   *Caught by* writing out the framebuffer and **looking at it** — the op counts
   looked entirely plausible throughout. *Fixed by* killing by pid with verification,
   a per-arm cleanliness probe that warns if any window is still discoverable, and a
   reboot to a pristine desktop before the definitive runs.

The definitive matrix (§3.2) ran after all three fixes, on a rebooted instance, and
emitted **no cleanliness warnings**.

---

## 3. Experiment 1 — the per-opcode byte census

The census counts every message's whole wire cost, header included (`op:u16` +
`total_length:u32`). Omitting the header would understate the cheap-and-numerous
ops and therefore **overstate** the bitmap path's share — the bet under test.

The bitmap path is `RP_DRAW_BITMAP` + `RP_DRAW_BITMAP_RECTS` (which carry pixels)
plus `RP_COPY_RECT_NO_CLIPPING` (which carries none, but is the op a Tier P region
would subsume). The two are reported **separately** as well as combined, so a reader
can subtract rather than trust the grouping.

### 3.1 The most important split: cold first paint versus steady state

A connection's first second and its steady state are different workloads, and
averaging them describes neither.

**Bare idle desktop, 3 trials, 30 s each:**

| trial | total | cold paint (<1 s) | bitmap share of cold paint | steady state | steady rate | bitmap share of steady |
|---|---|---|---|---|---|---|
| 1 | 67 800 B | 31 945 B | 68.8 % | 33 601 B / 27 s | 1 244 B/s | 3.3 % |
| 2 | 64 275 B | 31 865 B | 69.0 % | 30 156 B / 27 s | 1 117 B/s | **0.0 %** |
| 3 | 67 720 B | 31 865 B | 69.0 % | 33 601 B / 27 s | 1 244 B/s | 3.3 % |

**CONFIRMED.** The bitmap path is **~69 % of the cold first paint** and **0–3.3 % of
steady-state idle**. Second by second, the idle desktop emits **exactly 1 120 bytes
every second** — the Deskbar clock — with one larger repaint at a minute boundary.

That 0.0 % is an **absence, not a blind instrument**: the same census, on the same
capture, saw 21 972 B of bitmap payload in the cold window. The positive control is
the other end of the same run.

Steady-state idle decomposes as:

| op | bytes/s | share of steady idle |
|---|---|---|
| `RP_FILL_RECT` | 416 | 37.1 % |
| `RP_SET_HIGH_COLOR` | 392 | 35.0 % |
| `RP_STROKE_LINE` | 312 | 27.9 % |
| everything else | ~0 | ~0 % |
| **bitmap path** | **0** | **0.0 %** |

Two incidental findings worth more than they cost:

* **The Deskbar clock is drawn as vector rectangles and lines, not as text** — which
  is why the text path is ~0.3 % of a bare idle capture while a clock is visibly
  updating. **CONFIRMED.**
* **35 % of all steady-state idle bytes are colour-setter ops** — 28 × 14 B of
  `RP_SET_HIGH_COLOR` per second. The design credits `RemoteDrawingEngine` with
  setter dedup; on this path it is not collapsing them. Measurement **CONFIRMED**;
  that a dedup fix would remove them is **PLAUSIBLE** (not attempted here). It would
  be a larger win on the idle desktop than any codec, with no protocol change.

### 3.2 The definitive interactive matrix

Real application (`StyledEdit` on a 1 200-line document), window rect **discovered**
at `(2,0)–(512,431)`, workload **looped** to fill the capture, on a **rebooted,
verified-clean** desktop. 3 trials each, 30 s. Whole capture, so cold paint is
included.

| workload | total bytes (3 trials) | bitmap path | text path | other (vector/state/input) |
|---|---|---|---|---|
| **text** (typing, 264 keystrokes) | 476 208 – 478 306 | **2.95 – 3.16 %** | **19.40 – 19.46 %** | ~77 % |
| **menu** (open/walk/dismiss ×6) | 104 958 – 108 319 | 13.37 – 13.97 % | 3.89 – 3.99 % | ~82 % |
| **scroll** (208 wheel events) | 245 311 – 248 882 | 8.17 – 8.50 % | 19.56 – 19.83 % | ~72 % |
| **window drag** (270 moves) | 204 624 – 208 517 | 15.46 – 15.74 % | 1.07 % | ~83 % |

The spread across trials is **under 2 %** on every arm. The text arm carries **~30×
more text bytes** than its bare-desktop namesake (19.4 % vs 0.65 %) — the positive
control that typing reached a real text view.

### 3.3 Steady-state windows, activity only

Windowed to `[2 s, 27 s)`, which on these looped captures is **all workload and no
cold paint**.

| workload (steady) | bytes | rate | bitmap path | pixels on the wire |
|---|---|---|---|---|
| text typing | 403 579 | **15.8 kB/s** | **0.00 %** | 0 B |
| menu | 60 355 | 2.4 kB/s | **0.00 %** | 0 B |
| scroll | 177 653 | 7.0 kB/s | 2.84 % | **0 B** (all `RP_COPY_RECT`, no pixels) |
| window drag | 139 720 | 5.5 kB/s | 9.67 % | 8 992 B in 78 bitmaps |
| *idle (reference)* | *1 120/s* | *1.1 kB/s* | *0.0 %* | *0 B* |

**CONFIRMED: in steady-state interaction the bitmap path carries between zero and
9.7 % of the bytes, and typing and menus put no pixels on the wire at all.** The
window-drag bitmaps are **16×1, 17×1 and 32×1** — one-pixel-tall strips. That is the
mechanism in §3.4.

### 3.4 Why the pixels are already small: M0's crop is merged

`RemoteDrawingEngine::DrawBitmap` in this tree already carries both byte savings the
design planned:

* **the crop/rebase fast path** — "The fast path historically shipped the *entire*
  source bitmap even when only `bitmapRect` was drawn … Crop to the integer-pixel
  rectangle that covers `bitmapRect` and rebase" — the M0 item the design called
  *"the single largest byte saving"*; and
* **the clipped `RP_DRAW_BITMAP_RECTS` path**, which splits a partially-obscured blit
  into per-clip-rect sub-blits.

**CONFIRMED by code, and corroborated by the measurement:** this is why a large blit
never crosses the wire whole, and why even the photographic arm (§4.4) produces
tiles no larger than 63×20 and the window drag produces one-pixel-tall strips. A
material part of the reason the bitmap path is small is that **the optimisation a
codec would have competed with has already landed.**

---

## 4. Experiment 2 — encoder pricing on real captured frame sequences

Priced **offline, on the real captured streams** (`--wire-dump` files), never on
synthetic data. Four arm families:

* **STREAM** — one shared compressor state across the capture, flushed at every
  frame boundary. This is what `ssh -C` does and what the tree's streaming zstd
  segments would do. Most favourable case for a generic compressor.
* **PER-FRAME** — every frame compressed independently. A protocol that must drop
  superseded frames or resync cannot carry unbounded shared dictionary state across
  frames it might discard, so this is the honest price of in-protocol per-frame
  compression.
* **BITMAP-ONLY** — just the pixel payloads, priced as images and as bytes. The arm
  that prices "codec on the bitmap path" specifically.
* **REMAINDER** — the stream with every bitmap-path op removed. The control that
  decides the design question.

A frame boundary is a **proxy**: there is no `RP_TIER_END_FRAME` on the wire yet
(introducing one is the other half of M2), so a new frame is taken to start after
>16 ms of silence — one 60 Hz refresh of quiet. Only the per-frame arms depend on it.

### 4.1 Steady-state interactive workloads (x86 host)

| workload (steady) | raw | **zlib-6 (`ssh -C`)** | **zstd-3** | zstd-19 | **per-frame zlib-6** | remainder zstd-3 |
|---|---|---|---|---|---|---|
| text typing | 403 579 | 26.51× | **39.72×** | 58.35× | **2.53×** | 39.72× |
| menu | 60 355 | 14.66× | **20.97×** | 26.05× | **2.74×** | 20.97× |
| scroll | 177 653 | 17.41× | **26.28×** | 36.58× | **2.38×** | 24.87× |
| window drag | 139 720 | 21.28× | **41.98×** | 60.56× | **2.88×** | 41.14× |
| idle (whole capture) | 67 800 | 5.77× | 6.31× | 7.30× | 2.89× | 14.11× |

**CONFIRMED: the vector stream compresses 15–42× with shared state and only 2.4–2.9×
without it.** The gap between the STREAM and PER-FRAME columns — a factor of **6–17**
— is the price of being able to drop a frame, and it is the single most important
number for the *other* half of M2 (§7.2, item 3).

Note also that the remainder column is essentially identical to the whole-stream
column on these workloads: removing the bitmap path changes almost nothing, because
there is almost nothing there to remove.

### 4.2 CPU cost, measured on both machines

Same code, same captures, two machines. CPU is `process_time()`, not wall clock.

| arm | ratio (x86) | **CPU ms/MB (x86)** | ratio (arm64) | **CPU ms/MB (arm64 `c7g.large`)** |
|---|---|---|---|---|
| stream zlib-6 (`ssh -C`) | 5.77× | 19.2 | 5.90× | **28.1** |
| stream zstd-1 | 6.07× | 2.1 | 5.83× | **3.4** |
| **stream zstd-3** | 6.31× | 3.9 | 6.05× | **4.0** |
| stream zstd-9 | 6.96× | 10.9 | 6.66× | 16.9 |
| stream zstd-19 | 7.30× | 230.0 | 6.98× | 276.0 |
| per-frame zlib-6 | 2.89× | 24.2 | 2.88× | 38.4 |
| bitmap pixels: zstd-3 | 2.50× | 6.5 | 2.52× | 15.4 |
| bitmap pixels: PNG | 2.29× | 1 585.8 | 2.34× | 3 286.1 |

(Idle captures; the arm64 and x86 rows are different captures of the same workload,
which is why ratios differ by a few percent. The ratios agree to within 4 %, which
is the cross-machine check.)

**CONFIRMED: on Graviton, zstd-3 gives a better ratio than zlib-6 at 7× less CPU
per megabyte** (4.0 vs 28.1 ms/MB). zstd-19 is not a candidate: 276 ms/MB buys 15 %
over zstd-3.

### 4.3 The arithmetic that kills codec-on-bitmap on a desktop

Idle capture. Build the codec-on-bitmap design honestly — encode the pixel payloads
with the best measured image coder, and still apply generic stream compression to
everything else, because nobody would ship the vector stream raw.

| design | bytes on the wire | versus plain zstd-3 |
|---|---|---|
| raw | 67 800 | — |
| **generic zstd-3 over everything** | **10 738** | baseline |
| codec-on-bitmap: PNG pixels (9 783) + zstd-3 remainder (3 170) + bitmap message headers (~618) | ≈ 13 571 | **26 % worse** |
| codec-on-bitmap with zstd on the pixels instead (8 995 + 3 170 + 618) | ≈ 12 783 | 19 % worse |

### 4.4 The photographic arm — the case Tier P was conceived for

This is the arm that could have overturned the recommendation, so it gets its own
section and its own honesty.

`ShowImage` displaying a generated **800×600 photographic PNG** (gradient plus
per-pixel noise — deliberately not synthetically compressible), with the window
dragged to force large moving damage. 20 s, and the arm **works**: the bitmap path
is the majority of the stream for the first time.

| | |
|---|---|
| raw stream | 431 053 B over 20.07 s = **21.0 kB/s** |
| **bitmap path** | **288 132 B = 66.84 %** |
| bitmaps | 124, all decoded, 280 304 B of pixels |
| bitmap sizes | 16×16, 22×4, 32×4, 32×8, 32×9, 32×19, 32×32, 63×20 — **none larger than 63×20**, because of §3.4 |

| arm | ratio | CPU ms/MB (arm64) |
|---|---|---|
| **stream zlib-6 (`ssh -C`)** | **5.52×** | 31.0 |
| **stream zstd-1** | **19.79×** | 1.4 |
| **stream zstd-3** | **21.47×** | **1.3** |
| stream zstd-9 | 23.28× | 11.5 |
| stream zstd-19 | 25.73× | 107.1 |
| per-frame zlib-6 | 3.33× | 36.4 |
| bitmap pixels only: zstd-3 independent | 2.67× | 12.2 |
| bitmap pixels only: zlib-6 independent | 2.83× | 42.1 |
| bitmap pixels only: PNG | 2.36× | 3 231.9 |
| remainder (no bitmap path): zstd-3 | 20.81× | 2.5 |

And the same arithmetic, on the workload most favourable to a codec:

| design | bytes | versus plain zstd-3 |
|---|---|---|
| raw | 431 053 | — |
| **generic zstd-3 over everything** | **20 075** | baseline |
| codec-on-bitmap: PNG pixels (118 740) + zstd-3 remainder (6 868) + bitmap headers (7 828) | ≈ 133 436 | **6.6× worse** |

**CONFIRMED: on the most bitmap-heavy workload this image can produce, a codec on
the bitmap path is 6.6× worse than simply compressing the whole stream.**

The mechanism is the finding, not the ratio. The pixel payloads compress only
**2.67×** in isolation, but the whole stream — two thirds of which *is* those
payloads — compresses **21.47×**. The difference is **redundancy across messages**:
dragging a window re-blits the same image regions repeatedly, and a stream
compressor sees the repeats while a per-image coder cannot. Any scheme that encodes
each tile independently discards exactly the redundancy that is carrying the win.

This also produces the strongest single argument for the in-protocol zstd
capability: **zlib-6 gets 5.52× here where zstd-3 gets 21.47× — 3.9× better at 24×
less CPU.** The mechanism is **PLAUSIBLE**: zlib's window is capped at 32 KiB while
zstd-3's is megabytes, so with ~280 kB of repeating tiles zstd sees repeats that
zlib structurally cannot. The ratios are **CONFIRMED**; the window explanation is
reasoned.

> **Honest limit of this arm.** The final framebuffer of the capture shows an error
> alert rather than the picture — `ShowImage` errored at some point during the drag.
> The census nevertheless recorded 124 decoded bitmaps and 288 kB of pixels, so the
> image *was* being blitted during the capture, and the pricing above is of real
> photographic pixel data. But this arm is "a large photographic image being blitted
> and re-blitted", not "a clean 20 s of stable image viewing".

### 4.5 NEON: is there a Graviton-native path for each candidate?

| candidate | NEON on arm64? | basis |
|---|---|---|
| zlib / `ssh -C` | **Mostly no.** Stock zlib has no NEON; `zlib-ng` and Chromium's zlib carry NEON CRC32/hash paths. OpenSSH links the system zlib, so the `ssh -C` arm as measured is scalar. | **PLAUSIBLE** — library knowledge, not verified on this image. |
| **zstd** | **Yes.** Upstream zstd ships NEON acceleration and builds it by default on aarch64. `libzstd.so.1.5.6` is already on the image. | Library **CONFIRMED present** and used for the pricing; that the NEON paths were *taken* is **PLAUSIBLE**. |
| PNG / libpng | **Yes** (`PNG_ARM_NEON` filter implementation). | Our pricing used a pure-Python filter loop — see §4.6. |
| libjpeg-turbo | **Yes, but off by default**; this tree already established `-DCMAKE_SYSTEM_PROCESSOR=aarch64` turns it on. | Relevant only if a still-tile codec is ever built. |
| libx264 | **Yes**, hand-written NEON. | **Not priced.** M3 work, and pricing an encoder for content §3.3/§4.4 show the desktop does not produce would be dishonest. |

### 4.6 What the CPU numbers do and do not mean

* CPU is `process_time()` — CPU, not wall clock — averaged over repeats.
* The **PNG ratio is a real PNG ratio** (adaptive per-row filter selection by minimum
  sum of absolute differences, libpng's own heuristic). Its **CPU cost is an upper
  bound**: the filter loop is pure Python and libpng with NEON would be far faster.
  A ratio measured honestly with a cost labelled honestly beats a ratio with no cost,
  which the charter forbids. **The recommendation does not rest on PNG's CPU** — it
  rests on PNG's *ratio*, which is real, and PNG loses on ratio.
* `zstd_stream()` is concatenate-then-compress, an **upper bound** on streaming zstd:
  real streaming with per-frame flushes does slightly worse. The bound is generous to
  zstd, not to the argument.

---

## 5. Experiment 4 — the headless stall (D1 / D10)

D1 said: with no client attached, `discardWithoutReader` makes `Flush()` succeed
after discarding, so **every `DrawString` stalls 1 s and every screenshot 10 s**.
D10 said a native client with no `RP_STRING_WIDTH` handler burns the full 1 s
timeout on every `StringWidth`.

### 5.1 The guards are in the tree

`src/servers/app/drawing/interface/remote/RemoteDrawingEngine.cpp`:

* `DrawString()` (both overloads) — `if (!fHWInterface->IsConnected()) return point;`
  before the wait, commented *"No client is attached, so nothing will ever answer:
  skip the wait rather than stall the drawing thread for the full timeout on every
  string."*
* `ReadBitmap()` — the same `IsConnected()` guard. That is the 10 s screenshot path.
* `StringWidth()` — queries **only** when `IsConnected() && (ClientCapabilities() &
  RP_CAP_STRING_WIDTH_REPLY)`; otherwise it returns `fState.Font().StringWidth(...)`
  from the server's own metrics with no wait at all.

**CONFIRMED by code.**

### 5.2 The headless arm, with its sensitivity quantified

A GUI app is launched with **no client connected** and timed to quiescence. Timing
to quiescence rather than to "`hey` answers" is deliberate: `hey` answers once the
app has *registered*, which can precede painting, so a fast `hey` answer would not
prove the `DrawString` path did not stall afterwards. A stalling app is asleep on a
semaphore — elapsed grows while CPU does not — so elapsed-to-quiescence is the
discriminator.

`StyledEdit` on a 1 200-line document, no client attached, 5 trials:

| trial | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| elapsed to quiescence | 1 114 ms | 1 122 ms | 1 132 ms | 1 130 ms | 1 108 ms |

**Sensitivity, measured rather than asserted:** a connected capture of the *same
application and document* counts **`TEXT_OPS=980`** text runs. If each `DrawString`
took the 1 s sync wait, the headless launch would need **≥ 980 s**. It took
**1.11 s (p50 1 122 ms, p95 1 132 ms, n=5)**.

**CONFIRMED: the headless `DrawString` stall is closed, falsified by a factor of
~880.** An earlier, weaker probe (`hey`-responsive, 5 trials, 76–78 ms) agrees but
is not relied on.

### 5.3 D10, and the mechanism control that does not exist

Three client postures, 5 trials each, 20 s each, with a text-drawing app active:

| arm | posture | wall time | string-width queries |
|---|---|---|---|
| **N** | advertises no capability | 20 379 – 20 533 ms | **0** |
| **A** | advertises **and** answers | 20 375 – 20 383 ms | **0** |
| **X** | advertises and **never answers** (intended positive control) | 20 379 – 20 381 ms | **0** |

**CONFIRMED: `app_server` issued zero `RP_STRING_WIDTH` queries across all 20 arms —
about 300 s of wall time with a text-drawing application active — even to a client
that advertised `RP_CAP_STRING_WIDTH_REPLY`.** Arms N, A and X are indistinguishable
in wall time and message count because the query is never issued. Not answering
therefore costs nothing, because the server never asks.

**Arm X was meant to be the mechanism positive control and it failed to be one.**
The server can only be made to take the 1 s wait if it issues a query, and it does
not. So this campaign has **no positive control that the server-side 1 s wait is
reachable at all**, and that is reported as a gap rather than papered over:

* What *is* controlled is the **instrument**: `rdcapture.py`'s selftest feeds
  synthetic query timestamps and checks that a 1 s gap is reported as ~1000 ms, that
  a sub-millisecond gap is reported as sub-millisecond, that the >900 ms stall rule
  counts exactly the injected waits, and — as a mutation arm — that a 2000 ms
  threshold would call the same data unstalled. **The gap metric can see a 1 s wait.**
* What is **not** controlled is the mechanism. No workload available on this image
  causes `app_server` to issue a string-width query, and with no screenshot binary
  present the `ReadBitmap` path could not be driven either.

**Why the query is never issued — and it is not the capability negotiation.** Issue
**#534** independently establishes the mechanism from the code, and this measurement
corroborates it: `DrawingEngine::StringWidth` is **not virtual**, so
`RemoteDrawingEngine::StringWidth` *shadows* rather than overrides it. Every caller
holds a `DrawingEngine*` — including `DrawingEngine::DrawString`, which computes the
pen advance through an unqualified `StringWidth(...)` inside the base class — so the
call binds statically to the base implementation and the override that would emit
`RP_STRING_WIDTH` is **unreachable**. That is a far better explanation of 0 queries
in ~300 s than "negotiation declined to ask", and the two readings are genuinely
different claims: this report's zero is evidence for #534, not for the negotiation
working as designed.

**Verdict: D1 and D10 are closed.** D1 by measurement at ~880× sensitivity plus the
code guards; D10 because the query is never issued — and, per #534, currently cannot
be.

**The residual hazard is a different bug from D1 and is presently unreachable.** A
client that advertises `RP_CAP_STRING_WIDTH_REPLY` and then fails to answer would
still cost the server a 1 s wait per query — but only once #534 is fixed and the
query can actually be sent. Whoever fixes #534 inherits that hazard, and the
capability guard in `StringWidth()` is what contains it. Worth saying out loud,
because "D1 is closed" and "the 1 s wait can never happen" are not the same
statement, and only the first is proven here.

---

## 6. Experiment 3 — `ssh -C`

The stream already travels through an SSH tunnel, so `ssh -C` is the cheapest
compression this project can ship and the baseline any in-protocol scheme must beat.

Measured with **OpenSSH's own accounting**, not a model of it: `ssh -v` prints
`compress outgoing: raw data N, compressed M, factor F` at session end. It also
prints `Enabling compression at level 6` — which is how the zlib level used in the
offline arms was *established* rather than assumed.

Two shapes, because only one is honest about an interactive desktop:

* **BULK** — the whole stream `cat` in. `ssh` gets large packets it would never see
  live: an **upper bound**.
* **CADENCE** — replayed at its recorded timing with a flush at every recorded
  message boundary (`rdprice.py --replay`). The shape `ssh` actually sees, and the
  ratio to quote.

| stream | shape | raw | compressed | ratio |
|---|---|---|---|---|
| **random bytes (NEGATIVE CONTROL)** | bulk | 400 251 | 400 380 | **1.00×** |
| idle | bulk | 67 961 | 10 923 | 6.22× |
| idle | **cadence** | 70 562 | 12 613 | **5.59×** |
| menu (bare) | bulk | 99 014 | 12 190 | 8.12× |
| menu (bare) | **cadence** | 103 496 | 15 122 | **6.84×** |
| scroll (bare) | bulk | 96 565 | 12 853 | 7.51× |
| scroll (bare) | **cadence** | 101 056 | 15 791 | **6.40×** |
| text (bare) | bulk | 96 407 | 11 895 | 8.10× |
| text (bare) | **cadence** | 100 646 | 14 662 | **6.86×** |
| window (bare) | bulk | 100 840 | 17 093 | 5.90× |
| window (bare) | **cadence** | 105 340 | 20 306 | **5.19×** |

**CONFIRMED: `ssh -C` delivers 5.19–6.86× on interactively-shaped real streams**
(5.90–8.12× bulk). The negative control at 1.00× — `ssh -C` very slightly *expands*
random bytes — proves a reported win is a property of the desktop stream and not of
the harness.

Two cross-checks worth recording:

* The offline model predicted **5.77×** for the idle stream; real `ssh -C` on the
  same bytes at recorded cadence measured **5.59×**. The offline stream arms are
  therefore trustworthy to a few percent, which is what licenses using them for arms
  that could not be run through a real `ssh`.
* Cadence arms show *more* raw bytes than bulk arms of the same stream (70 562 vs
  67 961) because `ssh`'s accounting includes per-packet protocol overhead and
  cadence replay produces many more, smaller packets. That overhead is real and is
  part of why cadence ratios are lower.

**This finding shrinks M2, and that should be said plainly.** For vector-shaped
traffic — which is what a desktop is — `ssh -C` is already deployed, costs nothing,
and captures the large majority of what any generic scheme could. The marginal case
for an in-protocol compressor is (a) the 9–35 % zstd-3 improvement on vector
workloads, (b) **3.9× on bitmap-heavy content** (§4.4), (c) 7–24× less CPU, and
(d) not depending on the transport being SSH. That is a real argument, and it is
nothing like the case for a codec.

---

## 7. Recommendation

### 7.1 Neither codec-on-bitmap nor full-frame video — not next

**Against full-frame video.** The idle desktop costs **1 120 B/s** on the vector
path. The design's own Route-2 anchor measured a single idle VNC frame at **9.8 kB**,
and a video pipeline pays that per refresh, forever, for a screen that is not
changing — ~9× the entire vector cost of an idle desktop at 1 Hz, ~260× at 30 Hz.
Steady-state interaction costs 2.4–15.8 kB/s (§3.3); a 30 Hz full-frame pipeline at
the Route-2 idle figure would be ~294 kB/s. Nothing in the census suggests a desktop
needs it, §9 of the design already records that 4:2:0 damages text and thin UI
strokes, and the Route-2 blocking unknown is still not green. **CONFIRMED** on the
byte counts.

**Against codec-on-bitmap.** This changes the design's stated bet, so it rests on
four independent measurements:

1. **Share.** The bitmap path is 0–9.7 % of steady-state interaction — zero for
   typing and menus — and 0–3.3 % of steady idle. It is ~69 % of the *cold first
   paint*, which is real but a one-off per connection, and M1's content-addressed
   bitmap cache is already the right tool for it.
2. **Compressibility.** Pixel payloads compress **2.36–2.83×**, the worst of
   anything on this wire. The vector remainder compresses **15–42×**.
3. **Arithmetic, on the desktop.** §4.3: substituting the best measured image coder
   and compressing the rest generically lands **19–26 % worse** than zstd-3 over
   everything.
4. **Arithmetic, on the workload most favourable to a codec.** §4.4: on a
   photographic blit where the bitmap path *is* 66.8 % of the stream, codec-on-bitmap
   is **6.6× worse**, because per-image coding discards the cross-message redundancy
   that is carrying the win.

And a fifth, structural reason: **the byte saving a bitmap codec would have competed
for has already been taken** by M0's crop/rebase and the clipped `RP_DRAW_BITMAP_RECTS`
path (§3.4). The largest bitmap anywhere in these captures is 63×20.

### 7.2 What to do instead, in measured order of payoff

1. **Turn on the zstd capability that already exists** (`RP_CAP_COMPRESS_ZSTD`).
   Whole-stream zstd-3 is 20.97–41.98× on interactive workloads, 21.47× on the
   photographic one, and 6.05× on idle, at **1.3–4.0 ms CPU per MB on Graviton** —
   *cheaper* than zlib at a better ratio, with a NEON-capable `libzstd.so.1.5.6`
   already on the image. It beats `ssh -C` by 9–35 % on vector traffic and **3.9× on
   bitmap-heavy traffic**, and unlike `ssh -C` it works whatever the transport. Best
   bytes-per-unit-of-work change available, and it needs no new opcode. Use **level 3**;
   level 19 costs 276 ms/MB for 15 % more.
2. **Kill the redundant colour setters.** 35 % of steady-state idle bytes are
   `RP_SET_HIGH_COLOR`, 28 per second, on a desktop where only a clock moves.
   Server-side dedup, no protocol change, bigger win on idle than any codec.
   (Cost **CONFIRMED**; that the fix removes it is **PLAUSIBLE**.)
3. **Specify M2's bounded queue knowing the per-frame penalty.** Independent
   per-frame compression is **2.38–2.88×** against **15–42×** with shared state — a
   factor of 6–17. A queue policy that drops whole superseded frames must either keep
   compressor state across the drop (and resynchronise the decoder explicitly) or
   accept that ratio collapse. **This is a design constraint the numbers impose and it
   belongs in the M2 flow-control spec**, which is the part of M2 this report does not
   itself deliver.
4. **Keep `TCP_NODELAY` and the M1 cache as the felt-latency levers.** The census
   shows thousands of small messages (25.6–36.9 B mean), exactly the shape Nagle
   punishes.
5. **If Tier P is ever built, build it for content the desktop does not generate** —
   video playback, GL output, a screen-share of moving imagery — and not as a
   bandwidth optimisation for UI chrome. Tier P remains the right *architecture* for
   that case; the finding is that the case does not currently exist on this image.

### 7.3 What would change this recommendation

Stated so the recommendation is falsifiable rather than merely argued:

* **A workload whose bitmap path exceeds ~50 % of steady-state bytes with payloads
  that do not repeat.** §4.4 found the share but not the non-repetition; the repeats
  are what make the stream compressor win. Genuinely novel pixels every frame — video
  — would break that, which is the same thing as saying Tier P is for video.
* **A real full-motion requirement** from a user or product, rather than from the
  desktop's own drawing.
* **Route 2 going green** on the metal (painting + networking + input in one boot).
  That would make M3 cheap to *try*, which is a different argument from the byte
  census and does not depend on it.
* **A transport that is not SSH**, which removes the `ssh -C` baseline and
  strengthens item 1 of §7.2 without touching the codec question.

---

## 8. What was NOT measured

A gap named is worth more than a number guessed.

* **WebPositive.** **Not on the canonical image** (the lean base is `@minimum-mmc`;
  browsers vend from the package repo) and no `webpositive` package exists in the
  repo. Substituted with **NetSurf** — and **NetSurf never opened a discoverable
  window**, even with 25 s of settle time, across six attempts. The heavy arm is
  therefore **ShowImage on a photographic PNG** (§4.4), not a browser. "A browser
  rendering a page" and "WebPositive rendering a page" are different claims and
  neither is made.
* **A clean, stable photographic viewing arm.** §4.4's `ShowImage` errored partway
  through the drag; the pixel data priced is real but the arm is not 20 s of stable
  image viewing.
* **A positive control that the server-side 1 s sync wait is reachable.** §5.3. The
  instrument is controlled; the mechanism is not. No available workload makes
  `app_server` issue a string-width query, and there is no screenshot binary on the
  image to drive `ReadBitmap`.
* **Glass-to-glass latency.** Not re-measured; §7 of the design carries the existing
  anchors. This report is about bytes and CPU.
* **Real streaming zstd with per-frame flushes.** Priced as concatenate-then-compress,
  an upper bound (§4.6). A `ZSTD_CCtx` streaming implementation through `ctypes` was
  judged not worth it because zlib already measures the streaming *shape*.
* **Native libpng / libjpeg-turbo / x264 CPU on Graviton.** PNG's ratio is real; its
  CPU is an upper bound from a pure-Python filter loop. No native encoder was built or
  timed. NEON availability (§4.5) is library knowledge, labelled **PLAUSIBLE**.
* **Whether the colour-setter dedup fix actually removes those bytes.** Only the 35 %
  cost is measured.
* **`ssh -C` on the looped interactive streams.** The table covers idle and
  bare-desktop streams; the cadence-vs-bulk relationship is established there and the
  data gives no reason to think it is workload-specific.
* **Multi-client and reconnect byte costs.** M1's territory, not re-measured.
* **Anything about the separate flagship client's own behaviour.** Not exercised.

---

## 9. Reproducing it

```bash
# instruments, with their selftests
graviton/scripts/rdcapture.py --selftest        # 271 checks
graviton/scripts/rdprice.py   --selftest        #  26 checks
graviton/scripts/rdlatency.py --selftest        #  18 checks

# on the instance: one workload, with a byte census and a wire dump
python3.10 rdcapture.py --port 10900 \
    --cookie-file /boot/system/settings/remote_desktop/session_cookie.10900 \
    --width 1200 --height 760 --seconds 30 --answer-string-width \
    --workload text --workload-rect 2,0,512,431 --workload-loop \
    --wire-dump text.dump --json text.json

# the whole matrix: discovers each window rect before driving it, and refuses
# to run an arm whose window it could not find
./rdcensus-run3 3 30

# price a capture offline; --from-s/--to-s separate activity from cold paint
python3 rdprice.py text.dump --label text --from-s 2.0 --to-s 27.0 \
    --json price-text.json

# real ssh -C on the captured bytes, bulk and at recorded cadence
python3 rdprice.py text.dump --emit-stream text.rp
graviton/scripts/rdsshc-run <ssh-port> /path/to/streams

# the headless-stall arms; rdstall2-run carries the quiescence-vs-CPU probe,
# the sensitivity count, and the photographic arm
./rdstall-run 5
./rdstall2-run 5
```

Raw data — captures, wire dumps, per-arm JSON, the `ssh -C` CSV, the run logs and the
framebuffer screenshots — is kept in a scratch directory outside the repository
(`m2-data/`, on the maintainer's workstation), because the dumps are megabytes of
binary and re-pricing them is the operation that gets repeated, not re-capturing.
