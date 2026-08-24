# Receive-side ACK transmits are ~28% of all io interrupts

Measured 2026-08-24 on `c7g.16xlarge`, MTU 9001, while A/B testing where the io
vector is re-armed. It is recorded separately because it is independent of that
question and survives whatever happens to the re-arm work.

## The measurement

A pure *receive* workload is not pure. Every inbound data frame is acknowledged,
and on this driver each acknowledgement is a full `ena_send()`: one transmit
descriptor, **one doorbell write**, and one transmit completion. Transmit and
receive share a single MSI-X vector, so those completions raise the same
interrupt the receive path uses.

Eight concurrent receive flows, 6 s window, counters read out of the driver and
cross-checked against the interface's own counters over the same window:

| | re-arm in handler | re-arm after drain |
|---|---|---|
| rx frames/s | 88,866 | 85,669 |
| **tx (ACK) frames/s** | **35,367** | **34,603** |
| io interrupts/s | 68,048 | 51,832 |
| frames/irq, rx only | 1.30 | 1.65 |
| frames/irq, rx **+ tx** | 1.82 | 2.31 |

So roughly **0.40 ACK transmits per inbound frame**, and the transmit side
accounts for about **28%** of everything arriving on the shared vector
(35,367 / (88,866 + 35,367)). The driver's own frame counter agreed with the
interface counter to 0.996–0.997, so this is not a counting artifact.

The doorbell cost is not amortised: the transmit accounting counters show
one doorbell per frame (`txFrames == txDoorbells`), and each ACK is 66 bytes in
one descriptor. In LLQ mode the device grants only 2 entries per burst, so
consecutive frames cannot share a doorbell however the caller batches — see
`ena-tx-offload.md`.

## Why it is a lever

Whatever bounds receive throughput, 28% of the interrupt load on the receive
path is being spent on acknowledgements, and it is downstream of nothing: it
needs no device feature, no negotiation, and no change to the shared-vector
ownership rules. Candidates, cheapest first:

- **Reclaim transmit completions from the receive drain.** They already share the
  vector and the receive reader is already awake. Note the lock order is
  `txLock` before `rxLock` (see the reset path), so the receive path must not
  take `txLock` — this needs a `try`-style acquire or a reordering, not a nested
  lock.
- **Suppress the interrupt for ACK-only completions** if the device can be told
  to, so acknowledgements cost a doorbell but not a wakeup.
- **Reduce the ACK rate itself.** 0.40 ACKs per frame is high; this is a stack
  question (delayed/stretch acknowledgements) rather than a driver one, and it
  would cut doorbells, completions and interrupts together.

## Protocol, so the number can be reproduced or refuted

1. `c7g.16xlarge`, MTU 9001, receive direction, **8 genuinely concurrent flows**.
2. The peer **must** fork per connection. A peer that serves connections
   sequentially makes N flows into one flow at the per-flow cap, and then a sum
   of per-flow averages reports an aggregate that was never carried — measured
   here as an apparent 13.5–23.2 Gbit/s against a true 5.0 Gbit/s.
3. Read `ENA_IOCTL_GET_IRQ_STATS` before and after a fixed window
   (`ena_fault stats <seconds>`), and **bracket the same window** with the
   interface's `Receive:`/`Transmit:` counters. Two independent counters, because
   a single one cannot detect its own miscount.
4. Discard any sample whose `resetCount` moved: a device reset zeroes
   `ioInterrupts` without zeroing `rxFrames`, which yields a frames-per-interrupt
   ratio that is wrong without looking wrong.
