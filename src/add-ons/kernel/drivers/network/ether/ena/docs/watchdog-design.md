# The ENA device watchdog: one check, one reset path, and nine checks that do not exist

**Written 2026-08-31**, from the code in this directory, to discharge the eight
in-tree citations of `docs/watchdog-design.md` that had nothing behind them
(#107). Hardware context for the numbers quoted: `c7g` (Graviton3), `us-west-2`,
single queue, MTU 9001. This file lives in the driver directory because that is
where the citations point — six of them say `docs/watchdog-design.md` from
`ena.cpp` and `ena.h`, relative to themselves. Longer-form measurement writeups
for the same driver live in `graviton/docs/ena-*.md` and are cited from here by
their repo-root paths, exactly as the driver's own comments cite them.

> **What this document is not.** It is not a design for the watchdog the driver
> ought to have. It describes the **one** check that is implemented, and then
> §9 lists the nine that are not (#105, `type:blocker`). The driver's comments
> promise a design document; a document that quietly read as complete would be
> worse than the dangling reference it replaces, because it would retire an open
> blocker by prose.
>
> **Citations here are by symbol, not by line.** The readiness backlog cites
> `ena.h:173,207,352` and `ena.cpp:1246,1635,…` for these same eight sites and
> every one of those numbers is now wrong. Function and constant names survive
> edits; line numbers do not.

---

## 0. Status at a glance

| | State |
|---|---|
| Keep-alive liveness check | **Implemented.** 1000 ms cadence, 6000 ms deadline, consecutive-miss requirement |
| Device reset and re-bring-up | **Implemented**, single ordering shared with attach |
| Spurious resets of a healthy NIC | **Known defect, mitigated, not closed.** See §7 and `graviton/docs/ena-keepalive-watchdog-false-reset.md` |
| Admin-queue wedge, missing-TX-completion, RX stall, `DEVICE_REQUEST_RESET`, `NOTIFICATION`/hardware hints, reset-reason coverage, post-reset validation, queue-size backoff, uninit teardown order | **Not implemented.** Nine gaps, #105 |
| Reset-vs-teardown race guards | **Verified by code reading only** — the weakest result in §8 |
| Device drop counters harvested by the watchdog path | Stored, **no ioctl reads them** (#106) |

The one-line summary the driver prints when it starts, which is the cheapest
confirmation that any of this is live:

```
ena: watchdog running: 6000 ms keep-alive timeout, 1000 ms cadence
```

Absence of that line is meaningful: see §3 for the two ways it can be missing.

## 1. Why there is a watchdog at all

A Graviton EC2 instance has **no console and no video device** (see
`graviton/docs/graviton-optimization-plan.md` and the serial-console notes in
`graviton/docs/arm64-serial-console-c7g.md`): the network interface is the
machine's only interactive surface. A wedged NIC is therefore not a degraded
instance, it is an unreachable one — there is nobody to log in and run `ifconfig
down/up`, because logging in is what stopped working. This asymmetry is the whole
justification for a driver that resets its own device without being asked, and it
is why the reset path is willing to be as disruptive as it is (§5): a 30–80 ms
hole in a working link is cheap next to an instance that has to be terminated.

The device cooperates by emitting `ENA_ADMIN_KEEP_ALIVE` AENQ events roughly once
per second. Their absence is the signal the driver watches.

## 2. Why a thread, and not `add_timer()`

`ena_watchdog()` is a kernel thread that sleeps on a semaphore with a timeout.
The obvious alternative, `add_timer()`, is unusable: timer hooks run in interrupt
context, and the action the watchdog has to be able to take — `ena_watchdog_reset()`
— issues admin commands, takes mutexes, and blocks for tens of milliseconds. This
thread *is* the deferred context FreeBSD gets from its taskqueue.

Three consequences worth stating because they are load-bearing:

- **The sleep is `acquire_sem_etc(watchdogWake, …, B_RELATIVE_TIMEOUT,
  ENA_WATCHDOG_INTERVAL_US)`, not `snooze()`**, so `ena_watchdog_stop()` can
  release the semaphore and have the thread exit at once instead of waiting out
  the remaining second of a one-second tick.
- **Priority is `B_URGENT_DISPLAY_PRIORITY`**, above normal. A watchdog that is
  itself scheduled late measures scheduler latency and calls it device death. On
  arm64 this is not hypothetical — see the scheduler placement work in
  `graviton/docs/scheduler-smp-placement.md`.
- **Deciding is separated from observing.** `ena_aenq_keep_alive()` runs from the
  management interrupt and does exactly two things: harvest the device's drop
  counters out of the descriptor and `atomic_set64(&device->lastKeepAlive,
  system_time())`. It never acts. Everything that can block lives in the thread.

## 3. The one check that exists: keep-alive liveness

Per tick, in order:

1. **Exit test.** `watchdogExiting` — teardown wins immediately.
2. **Eligibility test.** If `!watchdogActive || !running || resetting ||
   deviceDead`, reset `keepAliveMisses` to zero and skip. Zeroing here is not
   tidiness: carrying a run of misses across a down/up or across a reset would
   let two unrelated samples add up to a reset. Testing `resetting` here is also
   how double entry into the reset path is prevented **by construction**, rather
   than by a flag test at the trigger.
3. **Traffic sample.** `rxFrames + txFrames` against `watchdogLastTraffic`, giving
   a boolean "did the datapath move during the interval just ended". Sampled
   every tick, before the age test, so it describes the interval whether or not a
   deadline was missed in it. Read without either datapath lock on purpose: the
   question is only *did this change*, and a torn or stale read costs at most one
   interval's worth of patience.
4. **Age test.** `system_time() - lastKeepAlive` against
   `ENA_KEEP_ALIVE_TIMEOUT_US`. Within the deadline, a non-zero miss run is
   logged as recovered and cleared. That log line exists because a fix that is
   silent when it works cannot be distinguished from a broken watchdog.
5. **Blame attribution.** Over the deadline, `ena_com_aenq_has_keep_alive()` asks
   whether a keep-alive is sitting *unconsumed* in the AENQ. If one is, the device
   is alive and our management interrupt went missing, and the reset reason
   becomes `ENA_REGS_RESET_MISSING_ADMIN_INTERRUPT` instead of
   `ENA_REGS_RESET_KEEP_ALIVE_TO`. This costs a ring scan and is the difference
   between blaming the device and blaming ourselves. It has already paid for
   itself once: in the false-reset investigation the reason logged was *always*
   plain keep-alive timeout, which is what ruled out interrupt loss as the
   mechanism (§7).
6. **Consecutive-miss requirement.** `keepAliveMisses++`, then compare against
   `ENA_KEEP_ALIVE_MISSES_WITH_TRAFFIC` (8) if the datapath moved, or
   `ENA_KEEP_ALIVE_MISSES_BEFORE_RESET` (2) if it did not. Below the requirement,
   log the miss and look again next tick.
7. **Reset.** At or above the requirement, log loudly and call
   `ena_watchdog_reset()`.

`watchdogActive` is set at bring-up from the AENQ negotiation: the driver asks for
`LINK_CHANGE | FATAL_ERROR | WARNING | NOTIFICATION | KEEP_ALIVE`, masks that
against `features->aenq.supported_groups`, and sets `watchdogActive` only if
`KEEP_ALIVE` survived. Both reference drivers gate the same way, and the reason is
sound: a device that never promised to send keep-alives must not be reset in a
loop for failing to send them. So the "watchdog running" line can be missing for
two quite different reasons — the device declined the group (there is a distinct
`(no keep-alive: watchdog disabled)` trace for that), or `create_sem()`/
`spawn_kernel_thread()` failed, which is logged as an error.

**`lastKeepAlive` is seeded at exactly three points**, and each one is a bug that
was fixed rather than a precaution: thread start in `ena_watchdog_start()`, the
end of `ena_device_bringup()` (so both attach and the tail of a reset), and
`ena_open()`. A zero there means "last seen at the epoch" and resets a healthy
NIC six seconds into every boot; a stale value in `ena_open()` means a reset six
seconds after `ifconfig up` following a long down period.

**`deviceDead` is the stop.** If a reset fails to bring the device back, the
device is left inert and the watchdog stops trying. A reset loop makes the
instance strictly worse than the wedged NIC it was trying to fix, and on a
console-less host nobody is watching to stop it.

## 4. The constants, and why they are what they are

| Constant | Value | Origin |
|---|---|---|
| `ENA_WATCHDOG_INTERVAL_US` | 1 000 000 | Matches Linux and FreeBSD exactly (`ena_netdev.h` `ENA_DEVICE_KALIVE_TIMEOUT`) |
| `ENA_KEEP_ALIVE_TIMEOUT_US` | 6 000 000 | Matches the reference `ENA_DEFAULT_KEEP_ALIVE_TO`. Six device intervals, **with no slack for jitter** — see §7 |
| `ENA_KEEP_ALIVE_MISSES_BEFORE_RESET` | 2 | Measured, not chosen. One miss was resetting healthy devices |
| `ENA_KEEP_ALIVE_MISSES_WITH_TRAFFIC` | 8 | Measured; 2 was tried here first and found insufficient |

The arithmetic the constants hide, stated plainly because it is repeatedly
misread as multiplication: checks are one interval apart, so **N misses means
silence of `ENA_KEEP_ALIVE_TIMEOUT_US + (N-1) × ENA_WATCHDOG_INTERVAL_US`** —
7 s at N = 2, ~13 s at N = 8. Not 12 s and not 48 s.

Two consecutive misses is deliberately **not** the same change as raising the
deadline. A late keep-alive is transient: the next event arrives, `lastKeepAlive`
advances, the count returns to zero, nothing is reset. What the requirement
refuses to do is reset on a single sample. A device that has genuinely stopped
stays silent, keeps missing, and is still reset — that property is the watchdog
and had to survive the fix.

The traffic-conditioned bound is the more interesting of the two, and the reason
it is phrased as a bound is the design: frames still moving is direct evidence the
device has not stopped, so a merely-late keep-alive costs nothing; but at eight
misses the device is reset **even with traffic flowing**, because a device that
moves frames while its management path is dead is a partial wedge and is precisely
what a watchdog exists to catch. Traffic buys patience, never immunity.
`ENA_KEEP_ALIVE_MISSES_BEFORE_RESET` is the single constant to raise if a heavier
load than anything measured so far overruns it; because non-final misses are
logged, a cadence that starts creeping becomes visible before it becomes a reset.

## 5. The reset, and the ordering that is not stylistic

`ena_watchdog_reset()` runs under `resetLock`, publishes `resetting` **before**
dismantling anything, then tears down and calls `ena_device_bringup()` — the same
function attach uses. Three orderings in it are correctness, not taste:

1. **`ena_com_dev_reset()` runs before the descriptor rings are freed.** After
   `ena_com_set_admin_running_state(false)`, `ena_com_submit_admin_cmd()` returns
   immediately, so the `DESTROY_SQ`/`DESTROY_CQ` inside
   `ena_com_destroy_io_queue()` never reach the device — while the free happens
   regardless. Freeing first hands ring memory back to the VM while the device
   still holds its addresses and has not been told to stop: silent, delayed
   corruption of whatever gets that memory next. FreeBSD resets in `ena_down()`
   before destroying queues for the same reason. Note that the reset is a
   *register write*, which is why it works irrespective of admin state.
2. **The frees happen with `txLock` and `rxLock` held.** The `resetting` flag
   alone cannot close the window — a bare flag is check-then-act. A receiver
   already inside `ena_com_rx_pkt()` when the rings vanish dereferences a null
   page, and the net stack's reader retries every 10 ms, so there is no window to
   be lucky in. The datapath therefore tests `resetting` **under** one of those
   locks, never on its own.
3. **Bring-up is `ena_device_bringup()`, shared with attach.** The order it
   performs — host attributes, device attributes, LLQ placement, ring sizes,
   MSI-X, interrupt handlers, interrupt moderation, RSS, queue pairs, RSS flush,
   MTU **last** — is a conjunction the device validates as a whole. Two copies
   would drift, and the drift would present as `CREATE_CQ` failing months later
   for no visible reason.

Also in the sequence, in the order the code does it: `linkUp` cleared (§6,
criterion B); `rxReady`/`txCompleted` released with a count of 8, because
`release_sem()` wakes exactly one waiter and there can be several; interrupt
handlers removed; RSS, admin queue and MMIO read request destroyed; `host_info`
deleted (one contiguous page leaked per reset if forgotten, because `comDev`
outlives the reset); MSI-X unconfigured; the interrupt counters zeroed (§6,
criterion A); and, on the way back, the receive ring refilled **only if
`openCount > 0`**, otherwise `ena_open()` will do it and doing it here would race.

Cost, measured: **27–84 ms**, ~32 ms typical. `resetCount` is monotonic per
device and is reported in both the start and completion log lines; it is the
better witness than `grep -c` on the syslog, which silently under-reports across
a rotation.

Ordering out of the reset, `ena_watchdog_stop()`: set `watchdogExiting`, release
the semaphore, `wait_for_thread()`. Because the thread tests `watchdogExiting`
after taking `resetLock`, a reset already in flight completes and the join is
bounded by the admin-command timeouts rather than being open-ended.
`ena_uninit_device()` calls it before anything else is dismantled.

## 6. Acceptance criteria for a reset

"The reset returned `B_OK`" is not evidence that the device works: every
interesting failure mode here returns success and then carries no traffic. These
are the criteria a reset is judged by, and two of them are load-bearing on
specific lines of the driver, which is why those lines carry comments pointing
here.

**Criterion A — the recreated io vector actually delivers interrupts.**
The failure mode to fear is a reset that rebuilds everything, logs completion,
and never receives another frame because the new MSI-X vector was never armed.
That state is indistinguishable from health in every other log line. Two things
make it detectable: the reset **zeroes `managementInterrupts` and `ioInterrupts`**
so that `first management interrupt delivered` / `first io interrupt delivered`
print again for the *new* vectors, and the completion line reports interrupts
taken since the reset began. A completion line reading `0 io` is the signature.
(A completion queue created by `ena_com_create_io_queue()` starts masked, which is
why bring-up and reset both call `ena_rearm_io_interrupt(device, true)` — forced,
because `irqArmed` describes the vector belonging to the queue that was just
destroyed.)

**Criterion B — the network stack recovers its addresses, not merely its link.**
This is *measured behaviour of the stack, not a driver design decision*, and the
driver depends on it: clearing `device->linkUp` is the most consequential single
line in the reset. `ethernet_link_checker()` polls `ETHER_GET_LINK_STATE` on a
1 s interval (`kLinkCheckInterval`) and publishes link changes; in
`AutoconfigLooper`, media losing `IFM_ACTIVE` calls `_RemoveClient()` — the DHCP
client is deleted — and regaining it calls `_ConfigureIPv4()`, which constructs a
fresh `DHCPClient`. So a reset that did *not* clear `linkUp` would leave the
interface holding a lease negotiated against a device state that no longer
exists. Verification must check that the address is back, not just that the link
is up.

**Criterion C — the driver is still the thing that recovered.** A reset that
happens to coincide with the peer retrying, or with the stack timing out and
reconnecting, proves nothing. Fault injection (§7) exists so the fault is known
and the device is known-healthy underneath it.

**Criterion D — no descriptor accounting was lost.** `rxPendingRefill` is reset
wherever `rxNextToFill` is, because the two describe the same ring and a stale
count posts descriptors the device already owns. After a reset the receive ring
should be full: `receive ring only partly filled: N of M descriptors still owed`
is the diagnostic that says it is not.

## 7. Known broken: the watchdog resets a healthy NIC

**This is the current state, not history.** Full writeup, with the protocol:
`graviton/docs/ena-keepalive-watchdog-false-reset.md`. Tracked with the rest of
the watchdog work under #105.

Under sustained receive load on `c7g.16xlarge` (8 genuinely concurrent flows,
MTU 9001), shipped code reset a **healthy** device about **twice per 150 s** —
roughly one spurious reset per 75 s. The device was carrying ~87 k frames/s
throughout, each reset completed in ~32 ms, and the link came straight back. Every
observed timeout age was **2.9 %–9.3 % over the 6000 ms limit**:

```
6174  6198  6233  6348  6384  6420  6456  6491  6526  6560   (ms)
```

That distribution is the finding. A wedged device produces ages that keep
growing; ages that cluster just past the deadline are what a **marginal deadline**
looks like. Corroborating it, the reason logged was always plain `keep-alive
timeout` and never `missing admin interrupt`, so `ena_com_aenq_has_keep_alive()`
found nothing unconsumed on any occurrence: the interrupt was not lost and the
ring was not backed up — the event had not been posted yet. Under load the
device's keep-alive cadence stretches slightly past one second, and a 6000 ms
deadline is exactly six intervals with **zero slack for jitter**.

Requiring two consecutive misses was tried first and **measured insufficient** —
it halved the rate rather than removing it, because the gap reaches further than
anything previously seen (one sample at **7290 ms** of silence while the device
carried 87,137 frames/s). Any plain count is a guess about a tail not yet
observed. Hence the traffic-conditioned bound of §4.

**What is honestly known, and what is not:**

- The mitigation (`2` idle / `8` with traffic) is **in the tree**. The three
  fault-injection controls that would prove it removed spurious resets *without*
  breaking detection are specified in that document's Verification table; this
  file does not claim they have all been run on hardware. Treat the mitigation as
  implemented and plausible, **not** as proven.
- The underlying cause is untouched: the deadline still has no slack, and the
  driver still does not read the device's hardware hints, which is where the
  device would tell us its own keep-alive expectations (§10).
- **The measurement predates a datapath change.** Those rates were taken when the
  io vector was re-armed inside the interrupt handler. The tree now defaults to
  `ENA_REARM_AFTER_DRAIN` (`ena_io_interrupt()` records the vector masked and the
  *drain* re-arms), with the old behaviour retained as a runtime switch
  (`ENA_IOCTL_REARM_MODE`) precisely so the two can be compared in one boot. The
  after-drain arm cut io interrupt load ~24 % and showed a *lower* reset rate, so
  the numbers above are the pessimistic arm — but they are not a measurement of
  today's default. #98 tracks the unmask-after-drain question and is still open;
  what its remaining scope is belongs to that issue, not here.
- A structural fact to carry into any further work: **the AENQ is drained from
  exactly one place**, `ena_management_interrupt()`, and nothing polls it. The
  whole keep-alive path therefore depends on that vector being delivered promptly
  — during the same period in which the io vector is taking 50 k–99 k interrupts
  per second. #108 (adaptive interrupt moderation) changes that pressure, so it
  is a watchdog-adjacent change even though it is filed as a performance
  enhancement: anything that alters io interrupt load alters the margin measured
  above and the reset rate should be re-measured with it.

## 8. Verification: what was proven, and what was only read

The watchdog is exercised by a compiled-out fault-injection path, enabled with
`jam -sHAIKU_ENA_FAULT_INJECTION=1 …` (`ENA_DEBUG_FAULT_INJECTION`) and driven by
the `ena_fault` tool. It is off in every promotable image by design — it is an
ioctl whose purpose is to make the watchdog see a dead device — and the Jamfile
echoes `fault injection COMPILED IN -- verification image, do not promote`. Tag
such an AMI so it can never be promoted.

- `ena_fault 0|1|2` → `ENA_IOCTL_SUPPRESS_KEEP_ALIVE`. Makes
  `ena_aenq_keep_alive()` stop advancing `lastKeepAlive`, so the watchdog observes
  a dead device while the device is in fact healthy — which is the point, because
  any failure during the test is then unambiguously the driver's. Chosen over
  unplugging something for exactly that reason. Mode 1 = suppress until one
  timeout has fired, mode 2 = until cleared.
  - **Mode 1 originally meant "drop one keep-alive event" and was useless**: the
    device emits one about every second against a six-second deadline, so a single
    drop can never reach the timeout. Measured, not reasoned: 90 s after a mode-1
    request, zero triggers. Mode 1 is now cleared by the **reset path**, at the
    moment the timeout it asked for has actually happened.
- `ena_fault hold <ms>` → `ENA_IOCTL_HOLD_RESET`, bounded by
  `ENA_MAX_RESET_HOLD_MS` (30 s). Stalls a reset at its **widest** point — rings
  and bounce buffers already freed, admin about to be torn down, the device at its
  least consistent — so a concurrent teardown can be aimed at it. Bounded because
  the hold happens with the interface down and is indistinguishable from a hang to
  anything using the network, including the ssh session running the test.

**The weakest result, stated plainly because two code comments point at this
paragraph.** A real reset is 27–84 ms wide, far too narrow to hit from a shell.
The torture campaign therefore **never tested `ifconfig down` landing inside a
reset**, so `resetLock` and the `resetting` flag guarding against a concurrent
`ena_uninit_device()` are **verified by code structure alone** — reasoning, not
measurement, and the weakest claim in this document. `ENA_IOCTL_HOLD_RESET` exists
to convert that into a real experiment and, as of this writing, that experiment is
owed.

What *was* exercised on hardware: watchdog-triggered reset and recovery via
suppression, repeated resets (mode 2), and the descriptor-reclaim/unwind paths.
What was **not**: the concurrency window above, and the three controls in §7.

Adjacent verification discipline that applies to every number in this file: a
success return is not an artifact; check whether a bound is mechanism or capacity
before theorising; and discard any sample whose `resetCount` moved, since a reset
zeroes `ioInterrupts` without zeroing `rxFrames` and produces a
frames-per-interrupt ratio that is wrong without looking wrong.

## 9. What is not implemented

Nine checks, from the file-level comparison against the FreeBSD reference
(`amzn-drivers`, `ena_freebsd_2.8.4`). Full text and priority ordering in
`graviton/docs/ena-production-readiness.md`; tracked as **#105**
(`type:blocker`), inside the broader device-watchdog framework of **#91**. **The
driver implements 1 of the reference's 5 watchdog checks.** Reproduced here
because six code comments send a reader to this file for "the watchdog design",
and the honest answer to that request includes the absences.

| # | Gap | Consequence |
|---|---|---|
| 1 | **Wedged admin queue undetected** (no `check_for_admin_com_state`) | The HAL sets `running_state = false` on a command timeout and *nothing notices*. Every later admin command fails while the interface reads "up" forever |
| 2 | **`ENA_ADMIN_DEVICE_REQUEST_RESET` not subscribed** | The group exists in the vendored `ena_admin_defs.h` and is never asked for: the device's own "reset me" is ignored |
| 3 | **`NOTIFICATION` subscribed with no handler** | `FATAL_ERROR`, `WARNING` and `NOTIFICATION` are all requested; only `LINK_CHANGE` and `KEEP_ALIVE` have handlers, so the rest fall to `ena_aenq_unimplemented()` and are logged as unhandled errors. This is also the channel carrying `ENA_HW_HINTS_NO_TIMEOUT` (§10) |
| 4 | **No missing-TX-completion watchdog** | One lost completion wedges transmit permanently, and the keep-alive check cannot see it: an idle-but-healthy device keeps sending keep-alives. Needs a timestamp in `struct ena_tx_buffer`; take the 2.8.4 shape, which contains a timestamp-race fix |
| 5 | **No RX-stall / missed-RX-interrupt detection** | No recovery from a refill deadlock |
| 6 | **Three reset reasons written, of 20 the register defines** | `ENA_REGS_RESET_NORMAL` for deliberate teardown, plus `KEEP_ALIVE_TO` and `MISSING_ADMIN_INTERRUPT` from the watchdog. The receive path deliberately reclaims and carries on where FreeBSD escalates (`TOO_MANY_RX_DESCS`, `RX_DESCRIPTOR_MALFORMED`, `INV_RX_REQ_ID`) — that choice keeps a recoverable link recoverable, but after a `FAULT` the HAL's partial-packet accumulator is left describing a packet whose completions were already acknowledged, so a malformed-descriptor storm has no terminating condition |
| 7 | **No post-reset parameter validation** | MAC / `max_mtu` can be silently overwritten across a reset |
| 8 | **No queue-creation size backoff** | One attempt, then fail |
| 9 | **`ena_uninit_device()` teardown order** frees IO queues *before* removing the interrupt handler — the inverse of both the reference and of `ena_watchdog_reset()` in this same file | Latent use-after-free |

Note the shape of that list: **gaps 1, 4 and 5 are all cases the keep-alive check
cannot detect even in principle**, because the device keeps emitting keep-alives
while a specific path of it is dead. "The watchdog is running" is therefore a much
weaker statement than it sounds, and no amount of tuning §4 improves it.

## 10. Limitations and open questions

- **Hardware hints are not read.** The device can ask, via the `NOTIFICATION`
  AENQ group, not to be watchdogged at all (`ENA_HW_HINTS_NO_TIMEOUT`) and can
  state its own timeout expectations. The driver does not read hints, so
  `ENA_KEEP_ALIVE_TIMEOUT_US` is a compile-time constant where the device is
  willing to supply the value. This is recorded as a limitation rather than
  pretended away, and it is the principled fix for §7's zero-slack deadline: it is
  gap 3 above.
- **`hwRxDrops` / `hwTxDrops` are write-only.** The keep-alive handler harvests
  the device's own drop counters out of the descriptor — the only place they are
  reported — and no ioctl reads them back. #106.
- **The AENQ has a single drain point and no poll fallback** (§7). A management
  vector that stops being delivered is exactly the case gap 1 would catch, and it
  does not exist.
- **`watchdogLastTraffic` is read unlocked** — intentional, and bounded to one
  interval of extra patience if it tears; stated so it is not "fixed" into a
  lock-order problem later.
- **`resetCount` never resets and there is no rate limit on resets.** The only
  brake is `deviceDead` after a *failed* reset; a device that keeps wedging and
  keeps recovering will be reset indefinitely at up to one per ~7 s. Acceptable
  today only because §7's spurious resets are the observed case and they succeed.

## 11. Related documents and issues

- `graviton/docs/ena-keepalive-watchdog-false-reset.md` — the spurious-reset
  measurement, the mitigation, and the reproduction protocol. Read this before
  touching §4's constants.
- `graviton/docs/ena-production-readiness.md` — the full backlog, including the
  P1 list reproduced in §9 and the caution about `[verified]` tags outliving the
  facts they certify.
- `graviton/docs/ena-ack-completion-interrupts.md` — receive-side ACK transmits
  are ~28 % of io interrupts; relevant because interrupt pressure is the
  suspected mechanism behind the stretched keep-alive cadence.
- `graviton/docs/ena-tx-offload.md` — includes why deferred doorbells would
  deadlock against a watchdog that is keep-alive-only (§9, gap 4).
- `ena-com/README.md` — the re-vendoring discipline. The HAL is byte-identical to
  `ena_freebsd_2.8.4`; nothing in this document justifies editing it.
- Issues: **#105** complete the watchdog (blocker) · **#91** device-watchdog
  framework · **#98** unmask after ring drain · **#106** observability /
  ENI metrics · **#107** this document · **#108** adaptive interrupt moderation.

Still dangling after this file, for whoever hits them: `ena.cpp` also cites
**`FINDINGS.md`** (from `ena_device_bringup()`, for the ordering conjunction that
cost a week) and **`HANDOFF.md`** (from `ena_setup_io_queues()`, for a `CREATE_CQ`
rejection the working driver has evidently since outlived — that `NOTE:` reads as
stale and wants re-checking, not just a document). Neither file exists. They are
the other two named alongside this one in the #107 discussion and are **not**
resolved here.
