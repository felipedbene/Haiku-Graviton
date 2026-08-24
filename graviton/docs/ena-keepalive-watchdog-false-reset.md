# The keep-alive watchdog resets a healthy NIC under sustained receive load

Found 2026-08-24 on `c7g.16xlarge` while A/B testing the io-vector re-arm point.
**This is a pre-existing defect in shipped code, not a consequence of that work** —
it reproduces with the re-arm left where it has always been, and at a higher rate
than with the change.

## What happens

Under sustained receive load the keep-alive watchdog fires and resets the device.
The device is healthy: it keeps carrying traffic, the reset completes in ~32 ms,
and the link comes straight back. Each reset is a ~32 ms hole in a working NIC,
and on a console-less instance a repeated one is indistinguishable from a fault.

Measured, 8 concurrent receive flows, MTU 9001, 150 s per phase, one boot:

| phase | where the io vector is re-armed | resets / 150 s |
|---|---|---|
| C | in the interrupt handler — **shipped code** | **2** |
| B | after the drain | 1 |
| A | switched between the two every 5 s | 0 |

Corroborated two ways: `grep -c` on the syslog bracketing each phase, and the
driver's own monotonic reset counter (`#1`, `#2` in phase C; `#3` in phase B;
none in phase A). Switching the re-arm point repeatedly under load produced no
resets at all, so an arm *transition* is not the trigger either.

## The signature says the deadline is too tight, not that anything is wedged

Every timeout age observed, against a 6000 ms limit:

```
6174  6198  6233  6348  6384  6420  6456  6491  6526  6560   (ms)
```

All ten are **2.9%–9.3% over the limit** — none is a device that stopped talking.
A wedged device produces ages that keep growing; these cluster just past the
deadline, which is what a marginal deadline looks like.

Second piece of evidence, from `ena_watchdog()`'s own diagnosis: the reason
logged is always **`keep-alive timeout`** and never `missing admin interrupt`.
That branch is chosen by `ena_com_aenq_has_keep_alive()`, so on every occurrence
there was **no keep-alive sitting unconsumed in the AENQ**. The interrupt was not
missed and the ring was not backed up: the event had not been posted yet.

So under load the device's keep-alive cadence stretches slightly past one second,
and `ENA_KEEP_ALIVE_TIMEOUT_US` at 6 s — six intervals with no slack for jitter —
turns that into a reset.

Worth noting for whoever picks this up: the AENQ is drained from exactly one
place, `ena_management_interrupt()` (`ena.cpp:185`), and nothing polls it. The
keep-alive path therefore depends entirely on that vector being delivered
promptly, and it is the same period during which the io vector is taking
50,000–99,000 interrupts a second.

## Why it is not the re-arm change

The change *reduces* io interrupt load by about 24% (68,048/s to 51,832/s at the
same offered load). Any mechanism in which interrupt pressure delays the
management vector is therefore monotonically **less** likely with the change than
without it — and the measurement above agrees: 2 resets on shipped code, 1 with
the change, 0 while switching.

## The fix, and why the obvious version was not enough

Requiring **two consecutive** missed deadlines was tried first and **measured
insufficient**. It halved the rate — 2 resets per 150 s down to 1 — but did not
eliminate it, because the gap reaches further than anything previously observed:

```
6288 ms   miss 1 of 2, not resetting yet
7290 ms   reset, after 2 consecutive missed deadlines
```

Those two samples are one watchdog poll apart with no keep-alive in between, so
the device was silent for 7.29 s while carrying 87,137 frames/s. Any plain count
is a guess about a tail not yet seen, and 7.29 s is what that guess cost.

So the fix asks a different question — *is the device actually dead?* — and uses
the datapath as the evidence:

- deadline missed, datapath **idle**: reset after
  `ENA_KEEP_ALIVE_MISSES_BEFORE_RESET` (2) misses, ~7 s;
- deadline missed, frames **still moving**: up to
  `ENA_KEEP_ALIVE_MISSES_WITH_TRAFFIC` (8) misses, ~13 s, then reset anyway.

The second is a **bound, not a disable**, and that distinction is the design. A
device still moving frames whose management path has died is a partial wedge and
is exactly what a watchdog exists to catch, so traffic buys patience and never
immunity. Non-final misses and recoveries are both logged, so the fix is visible
on the occasions when it works — a fix that is silent when it works cannot be told
apart from a broken watchdog.

## Verification

The risk here is not that spurious resets survive, which is easy to measure, but
that they were removed by breaking the watchdog — which stays silent until a
device genuinely wedges. Three controls, using the driver's fault-injection path
(`HAIKU_ENA_FAULT_INJECTION`, `ena_fault 1`) to stall keep-alives on a device that
is otherwise perfectly healthy:

| control | expectation |
|---|---|
| stall while idle | reset still fires, after **2** misses (~7 s) |
| stall under load | reset still fires, after **8** misses (~13 s) — the partial-wedge case |
| load, no stall | **0** resets in 150 s, against 2 on shipped code |

## Protocol

1. `c7g.16xlarge`, MTU 9001, receive, 8 **genuinely concurrent** flows (the peer
   must fork per connection; a sequential peer collapses N flows to one).
2. Sustain for at least 150 s per condition and bracket each with both the
   syslog reset count and the driver's reset counter.
3. Watch for syslog rotation: it will silently make a bracketed delta go
   negative or small, which is why the monotonic reset number is the better
   witness.
