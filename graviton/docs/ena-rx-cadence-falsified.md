# Interrupt cadence does not limit receive throughput

Measured 2026-08-24 on `c7g.16xlarge`, MTU 9001, 8 concurrent receive flows.
Base commit `96fe2a41f4`. Driver build stamp `rx-cadence-4-mod`, confirmed in
syslog and confirmed loaded from
`/boot/home/config/non-packaged/add-ons/kernel/drivers/network/ena` by
`listimage` before any number below was believed.

Predictions were pre-registered before measuring. **The hypothesis under test was
mine and it is falsified.** Interrupt cadence is not the limiter.

## Result in one table

Pooled over both boots, 35 valid 8-second windows, arms interleaved inside each
boot in three different non-ascending orders:

| lever | interrupts/s | frames/interrupt | goodput |
|---|---|---|---|
| re-arm point: in handler -> after drain | 73,445 -> 50,689 (**-31.0%**) | 1.818 -> 2.668 (**+46.8%**) | 6.989 -> 7.077 Gbit/s (+1.3%) |
| moderation interval: 0 -> 200 ticks (50x) | 52,398 -> 50,847 (-3.0%) | 2.596 -> 2.595 (-0.0%) | 7.123 -> 6.885 Gbit/s (-3.3%) |
| receive FIFO: 16 MiB -> 128 MiB | 50,689 -> 52,041 (+2.7%) | 2.703 -> 2.608 (-3.5%) | 7.003 -> 7.106 Gbit/s (+1.5%) |

The noise floor is **+-0.265 Gbit/s (3.8%)** in boot A and **+-0.337 Gbit/s
(4.7%)** in boot B, measured as the spread of goodput across *every* arm and
replicate within one boot. Every goodput number above is inside it. Nothing
smaller than that spread is called a difference.

So: the re-arm change moves frames-per-interrupt by nearly half, and goodput does
not follow. That is the whole finding.

## Per-arm detail

### Boot A, receive FIFO 16 MiB (shipped value)

| arm | n | Gbit/s | interrupts/s | frames/irq | rx frames/s |
|---|---|---|---|---|---|
| re-arm in handler, interval 20 | 3 | 6.998 +-0.306 | 73,505 +-2456 | 1.847 +-0.015 | 97,536 |
| re-arm after drain, interval 20 | 7 | 6.979 +-0.279 | 49,917 +-2978 | 2.703 +-0.241 | 97,273 |
| re-arm after drain, interval 0 | 3 | 7.113 +-0.287 | 52,151 +-335 | 2.632 +-0.088 | 99,144 |
| re-arm after drain, interval 4 | 3 | 7.056 +-0.239 | 48,142 +-5460 | 2.849 +-0.263 | 98,315 |
| re-arm after drain, interval 200 | 3 | 6.903 +-0.356 | 50,853 +-704 | 2.630 +-0.103 | 96,200 |

### Boot B, receive FIFO 128 MiB

| arm | n | Gbit/s | interrupts/s | frames/irq | rx frames/s |
|---|---|---|---|---|---|
| re-arm in handler, interval 20 | 3 | 6.980 +-0.278 | 73,386 +-1525 | 1.788 +-0.023 | 97,292 |
| re-arm after drain, interval 20 | 4 | 7.250 +-0.320 | 52,041 +-1317 | 2.608 +-0.054 | 101,043 |
| re-arm after drain, interval 0 | 3 | 7.132 +-0.396 | 52,646 +-1982 | 2.560 +-0.060 | 99,405 |
| re-arm after drain, interval 4 | 3 | 7.253 +-0.352 | 52,069 +-711 | 2.607 +-0.067 | 101,059 |
| re-arm after drain, interval 200 | 3 | 6.866 +-0.394 | 50,842 +-938 | 2.559 +-0.102 | 95,700 |

## The negative control moved its own intermediate variable and not the outcome

Enlarging the receive FIFO was included to kill the experiment if it moved the
ceiling as much as the cadence arms. It did not move goodput (+1.5%, inside the
noise floor). But it is **not** a null intervention that might simply have failed
to take effect, and that distinction is the reason to trust it:

| | FIFO 16 MiB | FIFO 128 MiB |
|---|---|---|
| frames dropped by the device interface | 757 / 14,873,289 = **0.00509%** | 178 / 13,804,418 = **0.00129%** |
| 8-second windows with **zero** drops | **0 of 19** | **15 of 19** |

The FIFO change demonstrably worked -- a 3.9x reduction in drops, and most
windows went completely clean -- and goodput did not move. A control that
announces itself and still does not move the outcome is worth more than one that
merely sits still.

This also re-confirms, from a third direction, that FIFO overflow is not the
limiter: eliminating the drops outright buys nothing.

## Moderation is inert on this device, which is a measurement, not a null result

Arm 3 swept the receive moderation interval over 0, 4, 20 and 200 register ticks
-- a 50-fold range, where 0 means "interrupt on every completion". **The
interrupt rate did not move**: 52,398 -> 50,847 per second, a 3.0% change against
a 1.4-8% within-arm spread.

By the pre-registered rules that voids any goodput conclusion from arm 3 about
moderation-as-latency, because the predicted intermediate variable did not move.
What it establishes instead is stronger and simpler: **the device is not honouring
the delay fields at all.** Two independent pieces of evidence:

1. `ena_com_init_interrupt_moderation()` returns success while leaving
   `intr_delay_resolution` at **0**. The only path that returns 0 without setting
   it is `ena_com_get_feature(ENA_ADMIN_INTERRUPT_MODERATION)` answering
   `ENA_COM_UNSUPPORTED`, which the common layer converts to success and returns
   early from. So **the device does not advertise the interrupt-moderation
   feature.** Logged at attach now rather than assumed.
2. Writing 0 versus 200 into `rx_intr_delay` changes the interrupt rate by
   nothing measurable, while the `intr_unmask` bit in the *same register write*
   plainly works -- moving the re-arm point changes the interrupt rate by 31%.
   The register is reaching the device; the delay fields are being ignored.

Consequence for the driver: `ENA_RX_IRQ_INTERVAL` / `ENA_TX_IRQ_INTERVAL` are
currently decoration. They are not a tuning knob, and the comment claiming they
are "the only thing keeping this vector from firing once per completion" was
wrong -- what actually bounds the interrupt rate is the re-arm point.

### A false pattern this refutes

Before measuring, the interval was read as microseconds and a tempting
arithmetic fit was noted: with a 1 us resolution the moderation ceiling would be
`1/20us + 1/50us = 70,000` interrupts/s, and an earlier session had measured
68,048/s in the re-arm-in-handler arm -- 97.2% of it. That looked like a driver
pinned against its moderation ceiling.

It was a coincidence. This session measures **73,445 interrupts/s** in the same
arm, *above* the supposed ceiling, and moderation is inert anyway. The fit was
labelled an inference at the time; it is now refuted. Worth recording because the
number is seductive and someone will find it again.

## Nothing is saturated, in any arm

Per-arm CPU, 8-second windows, whole-machine partition reconciled against
per-CPU active time to +0.00%:

| arm | machine CPU | ena consumer | ena reader | net timer | CPU per frame |
|---|---|---|---|---|---|
| re-arm after drain, interval 20 | 2.65% | 50.5% of a core | 40.3% | 18.4% | 12.23 us |
| re-arm in handler, interval 20 | 2.75% | 50.9% | 41.5% | 19.2% | 12.28 us |
| re-arm after drain, interval 200 | 2.64% | 50.2% | 39.4% | 18.5% | 12.58 us |
| re-arm after drain, interval 0 | 2.61% | 48.7% | 37.9% | 19.0% | 12.67 us |

A 64-vCPU machine is **97.3% idle** in every arm. The busiest single thread is
the device interface's consumer at half of one core. CPU per frame is flat to 2%
across arms that differ by 24,000 interrupts per second, which bounds the cost of
an interrupt at **<= 2.0 us** -- and that bound is itself inflated, because the
higher-interrupt arm also carried more frames. Twenty-four thousand extra
interrupts per second at 2 us is 0.075% of this machine.

**Interrupt CPU is not a cost worth optimising here.** That is a measurement, and
it is the second reason cadence cannot be the limiter: even if the interrupts
were free, the machine already behaves as though they were.

## Apparatus, and what would have invalidated it

- **Both boots' arms interleaved in one binary.** The driver carries runtime
  knobs for the re-arm point and the receive moderation interval, so arms 1-3 are
  switched by ioctl with no reboot and no module swap. Arm 4 needs a different
  `stack` module, so it is a second boot -- and arms 1-3 are re-measured inside
  that boot, which is what makes the FIFO comparison interpretable rather than a
  measurement of boot-to-boot drift.
- **Order was interleaved, never ascending**: three different non-ascending
  orders, plus four consecutive repeats of one arm at the start to establish the
  noise floor before comparing anything.
- **Build stamp read back before trusting a number.** `rx-cadence-4-mod` in
  syslog, and `listimage` confirming the non-packaged load path. On the first
  attempt the driver was placed in the legacy `drivers/bin` + `drivers/dev/net`
  layout, the packaged copy won, and syslog still said `irq-cadence-3-wd2`. The
  correct location for this device-manager driver is
  `add-ons/kernel/drivers/network/ena`. Without the stamp that run would have
  been reported as "the change did nothing".
- **Peer forking verified by observation, not by flag**: 8 simultaneous child
  processes and 8 established connections with distinct 5-tuples, checked on the
  peer while the load was running.
- **Not the per-flow cap.** A single flow measures 4,940 Mbit/s, which *is* the
  cap. Eight flows aggregate to ~7.0 Gbit/s, i.e. 0.875 Gbit/s per flow -- nowhere
  near the flat 5.00 signature, and well under the instance's aggregate ceiling.
  The result is not an artifact of either bound.
- **Two independent counters.** The driver's own frame counter agreed with the
  interface's counter to 0.996-1.000 in every window.
- **Reset counter read at both ends of every window.** `resetCount` moved in
  **zero** of 38 windows, so no window was discarded for that reason. Read from
  the driver's monotonic counter, not by bracketing syslog, which rotates.
- **`arms/irq == 1.000` exactly** in every window: one unmask register write per
  interrupt, as the re-arm design intends, in both arms.
- **Offered-load filter, declared once and applied identically to both boots**: a
  window counts if its receive frame rate is at least 85% of that boot's 90th
  percentile. It excludes 0 of 19 windows in boot A and 3 of 19 in boot B (two
  ramp-up windows before all eight flows had connected, and one transient dip).
  The filter never refers to which arm a window belongs to.
- **No `dprintf` on the datapath.** On this platform it writes the UART one
  character at a time, synchronously: a barrier, not a probe. The counters are
  plain integers read by ioctl, and the read takes neither datapath lock.

## What this closes, and where the limit is not

Three hypotheses for the receive ceiling are now dead, and this one makes the
third:

1. Consumer per-frame cost sets a 14.5 Gbit/s ceiling -- dead; the projected
   ceiling spanned 214% across operating points, so it was a curve.
2. FIFO overflow caps throughput -- dead; and now dead a second way, since
   removing the drops changes nothing.
3. **Interrupt cadence limits throughput -- dead.** Frames per interrupt moved
   +46.8% and goodput did not follow; the moderation knob is inert; interrupt CPU
   is bounded at 0.075% of the machine.

What survives is the picture that motivated the experiment, minus its proposed
cause: a serialized chain in which nothing is saturated. 97.3% of the machine
idle, the busiest thread at half a core, ~7.0 Gbit/s against 29.8 Gbit/s for
Linux on a single queue. The limit is somewhere in the serialization itself --
how many hand-offs a frame makes between the device and the socket, and how much
each hand-off costs in latency rather than in CPU -- and not in how often the
device raises its vector.

Two concrete leads that this session did *not* test, both visible in the CPU
partition above: the interface consumer and the driver's reader thread are
separate threads with a queue between them, and the `net timer` thread costs 18%
of a core at this rate. Neither is saturated, which is exactly why a latency
account is needed rather than a throughput one.
