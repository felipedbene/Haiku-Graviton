# ENA adaptive interrupt moderation (first step)

Tracks issue #108. This document describes what the device advertises for
interrupt moderation, what the driver does today, the adaptive control loop this
first step adds, and — importantly — the measured reason the payoff is bounded
and conditional on the instance generation.

## What moderation is, in this device

ENA raises an interrupt when one of two per-direction timers expires (or on the
first completion after the vector was unmasked). The interval each timer waits is
programmed in the interrupt-unmask register, in *device ticks* whose length is
the device-reported `intr_delay_resolution` (a tick is one microsecond exactly
when the resolution is 1). ENA-com exposes:

- `ena_com_interrupt_moderation_supported()` — whether the device advertised the
  `ENA_ADMIN_INTERRUPT_MODERATION` feature at all.
- `ena_com_init_interrupt_moderation()` — reads the feature and latches
  `intr_delay_resolution`.
- `ena_com_update_intr_reg()` — packs the RX/TX intervals and the unmask bit into
  the register the re-arm path writes.

Note what this vendored `ena-com/` does **not** contain: there is no device-side
adaptive-moderation table and no `ena_com_calculate_interrupt_delay()`. The
`adaptive_coalescing` flag in `struct ena_com_dev` is only a boolean; the modern
reference design puts the entire control loop in the driver (Linux's `ena_dim.c`
over the DIM library). So an adaptive scheme for DeBeOS has to live in the Haiku
glue — which is exactly where this first step puts it, keeping `ena-com/`
untouched.

## What the driver did before this change

Fixed, non-adaptive moderation. `rxIrqInterval` starts at the compile-time
`ENA_RX_IRQ_INTERVAL` (20 ticks) and the re-arm path
(`ena_rearm_io_interrupt()`) programs it into the register on every unmask; TX is
the compile-time `ENA_TX_IRQ_INTERVAL` (50 ticks) and is never varied. The RX
interval is sweepable at runtime via `ENA_IOCTL_RX_MODERATION` — but purely as a
measurement instrument, not as a controller: nothing moves it on its own.

## The measurement that bounds the value of this work (READ THIS)

`ena-rx-cadence-falsified.md` (c7g.16xlarge, 2026-08-24, pre-registered A/B, 35
valid windows across two boots) established three things that constrain #108:

1. **On that instance the device did not advertise moderation at all.**
   `ena_com_init_interrupt_moderation()` returned success while leaving
   `intr_delay_resolution` at 0, and writing 0 vs 200 ticks into the delay field
   moved the interrupt rate by nothing measurable while the unmask bit in the
   *same register write* plainly worked. The delay fields are decoration on that
   device; the re-arm point, not the interval, is what bounds the interrupt rate.
2. **Interrupt cadence does not limit receive throughput.** Frames-per-interrupt
   moved +46.8% (by changing the re-arm point) and goodput did not follow.
3. **Interrupt CPU is not a cost worth optimising there.** It is bounded at
   ~0.075% of a 64-vCPU machine; the receive ceiling is `TCPEndpoint::fLock` plus
   FIFO bufferbloat, not interrupt handling.

The honest consequence: **where the device does not honour the delay fields,
adaptive moderation is a no-op by construction, and this first step deliberately
refuses to pretend otherwise** — enabling it there returns `B_NOT_SUPPORTED`.
The reason to build it anyway is that moderation support is a *device-probed*
capability, like `max_tx_header` and the offload caps, which have been observed
to differ across Graviton generations (c9g reported `max_tx_header` 224 vs 96 on
c7g). The capability must be probed per instance, surfaced, and — where a newer
device *does* advertise it — driven by something better than a fixed constant.
This change makes that possible and safe without claiming a win that the c7g
measurement already rules out on that hardware.

## Control-loop design

Host-side, receive-only, and deliberately coarse:

- **Signal.** Packet rate over a fixed wall-clock window. `system_time()` derives
  from `CNTVCT_EL0` and is trustworthy for a wall-clock span on this platform,
  unlike CPU-percentage accounting (CloudWatch ~55x, PMU ~40x under-report). Byte
  rate is not used in this first step; pps is the variable that drives interrupt
  count, which is what moderation acts on.
- **Window.** `ENA_MOD_WINDOW_US` = 100 ms. Long enough that even the lowest
  non-idle bucket carries thousands of frames (statistical mass, not a handful),
  short enough to track a load change within a tenth of a second.
- **Where it hooks.** At the end of a receive drain — the `if (!rearmed)` branch
  in `ena_receive()`, under `rxLock`, on the receive thread — immediately before
  `ena_rearm_io_interrupt()`. So the interval chosen is the one that same unmask
  programs, with no extra register write and no new lock.
- **Buckets.** `kRxModerationBuckets[]` maps a pps band to an RX interval in
  device ticks. The bands only ever *widen* the interval as the rate rises:

  | packet rate | RX interval (ticks) |
  |---|---|
  | < 20 000 pps (`ENA_MOD_LOW_PPS`) | 0 — interrupt per completion |
  | 20 000 – 50 000 | 16 |
  | 50 000 – 150 000 | 48 |
  | > 150 000 | 96 (`ENA_MOD_INTERVAL_MAX`) |

- **Latency safety.** The first band is a floor at interval 0: an interface this
  quiet is either idle or carrying latency-sensitive request/reply traffic, and
  neither ever has a completion held back for a timer. Coalescing is only widened
  above that rate, where per-interrupt cost — not delivery latency — dominates.
  The cap (96 ticks, ~96 µs at resolution 1) is far below the 15-bit register
  maximum (32767): this is sized to be justified by a hardware A/B before being
  widened, not to chase the largest interval the register can hold.
- **Coexistence with the manual knob.** Adaptive and `ENA_IOCTL_RX_MODERATION`
  are mutually exclusive by construction: with adaptive on, the control loop owns
  `rxIrqInterval` and overwrites any manual value; with it off, the manual knob
  and the compile-time default hold. Turning adaptive off restores the
  compile-time default, so the manual instrument resumes from a known point.

## What this first step implements

- Probe and surface the capability at attach: latch
  `ena_com_interrupt_moderation_supported()` into `device->moderationSupported`
  and log whether the feature was advertised (alongside the existing
  delay-resolution log).
- `ena_adaptive_moderation_sample()` — the control loop above; a no-op unless
  enabled.
- `ENA_IOCTL_RX_ADAPTIVE_MODERATION` (9807) — on/off toggle, runtime for the same
  reason the whole cadence apparatus is runtime (honest A/B is adaptive-vs-not on
  one boot of one instance, because boot-to-boot throughput drift on this
  hardware is large enough to swamp the effect). Refused with `B_NOT_SUPPORTED`
  when the device never advertised the feature — because an interval in ticks of
  an unknown resolution is worse than none.

Default is **off**. The change is behaviour-neutral until explicitly enabled,
which is what makes it the lowest-risk first step and gives the A/B a clean
switch inside one boot.

## Hardware A/B — owed

Not yet measured. The experiment, in order:

1. On the target instance, read the attach log / `ENA_IOCTL_GET_IRQ_STATS` for
   `intrDelayResolution`. **If it is 0 the device does not honour moderation and
   adaptive is correctly inert — record that and stop; there is nothing to
   measure on that instance.** (This is the c7g outcome; retest on c8g/c9g, which
   may differ.)
2. If moderation is honoured: on one boot, drive receive load with `nettput`
   (multi-flow, fixed instance), and interleave adaptive-off vs adaptive-on in
   non-ascending order, several replicates, establishing a noise floor first —
   the apparatus of `ena-rx-cadence-falsified.md`. Report interrupt rate
   (`ioInterrupts`), frames-per-interrupt (`rxFrames`/`irqArms`), CPU by
   wall-clock progress (never CloudWatch/`top` CPU%), and p99 request/reply
   latency on a separate low-rate flow to confirm the low-pps floor protects it.
3. A perf claim requires the measurement banked as evidence, not this design.

The driver can hotswap outside the boot path (proven), so this A/B needs no bake.
The PR is held regardless.
