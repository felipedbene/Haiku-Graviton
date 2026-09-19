/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Driver for the AWS Elastic Network Adapter (ENA), the only network device
 * offered by EC2 Nitro instances.
 */
#ifndef ENA_H
#define ENA_H


#include <ByteOrder.h>
#include <KernelExport.h>

#include <bus/PCI.h>
#include <device_manager.h>
#include <ethernet.h>
#include <ether_driver.h>
#include <lock.h>
#include <net_buffer.h>
#include <net_device.h>

extern "C" {
#include "ena-com/ena_com.h"
#include "ena-com/ena_eth_com.h"
}


#define ENA_DRIVER_MODULE_NAME	"drivers/network/ena/driver_v1"
#define ENA_DEVICE_MODULE_NAME	"drivers/network/ena/device_v1"
#define ENA_DEVICE_ID_GENERATOR	"ena/device_id"

#define ENA_PCI_VENDOR_AMAZON	0x1d0f

/* Reported to the device in the host attributes. The device records these for
   AWS support telemetry, but it also decides what to advertise back to us based
   on what we declare, so they are worth filling in properly. */
#define ENA_DRIVER_VERSION_MAJOR	1
#define ENA_DRIVER_VERSION_MINOR	0
#define ENA_DRIVER_VERSION_SUBMINOR	0
/* Numeric kernel-version reported to the device in the host attributes
   (info->kernel_ver / kernel_ver_str). This is device-facing telemetry with a
   fixed numeric shape, so it stays a constant rather than the DeBeOS-native
   revision string. The human-readable boot banner uses the live
   get_haiku_revision() instead (see ena_init_device, #201). */
#define ENA_HAIKU_REVISION		59996

/* Printed at attach, before anything else the driver says, so that a boot log
   can be tied to the source it was built from.

   This exists because of a real incident: reverting an experiment with `mv`
   restored a source file whose mtime predated the object built from the patched
   version, jam compared mtimes and kept the stale object, and days of boots
   silently tested code that was no longer in the tree. `git diff` was clean, a
   checksum against upstream was clean, and nothing in any log distinguished the
   two builds.

   Bump ENA_BUILD_TAG whenever you want a boot to be attributable to a specific
   change. The compiler-provided timestamp beside it is the more useful half: it
   is the moment *this translation unit* was actually compiled, so a stale ena.o
   announces itself. Note it cannot prove anything about the vendored HAL's
   object -- that one is only guaranteed by removing the driver's object
   directory before building, which is the habit to keep.

   Reading this back also catches the driver never having loaded at all: a
   drop-in replacement can lose the module-selection tie to the packaged copy in
   silence, because _FindBestDriver() takes strictly greater support. A day went
   into an unmoved measurement that turned out to be an unloaded driver rather
   than an ineffective change, so read the stamp out of the syslog before
   believing a number. */
#define ENA_BUILD_TAG		"rxbuf-1920"
#define ENA_BUILD_STAMP		__DATE__ " " __TIME__

/* BAR 0 holds the registers; BAR 2 is the Low Latency Queue push window. The
   MSI-X table lives in BAR 1, which the PCI bus manager maps itself. */
#define ENA_REGISTER_BAR	0
#define ENA_MEMORY_BAR		2

/* The revision-id bit that says memory-mapped register reads are unavailable,
   forcing the read-less path through the admin queue. */
#define ENA_MMIO_DISABLE_REG_READ	BIT(0)

/* One TX/RX pair. ENA supports many, but a single pair is enough to make an
   instance reachable and keeps the completion paths simple; scaling out is a
   separate change (see the TODO in ena_setup_io_queues). */
#define ENA_IO_QUEUE_PAIRS	1
#define ENA_TX_QUEUE_ID		0
#define ENA_RX_QUEUE_ID		1

/* Descriptor ring depth. Must be a power of two and is clamped to whatever the
   device reports it can do. */
/* Match the reference driver, which uses 512 transmit and 1024 receive
   entries on this hardware. Depths below these are rejected by the device with
   an unqualified error, so they are not merely a performance preference. */
#define ENA_DEFAULT_TX_RING_SIZE	512
#define ENA_DEFAULT_RX_RING_SIZE	1024

/* Packet buffers are a fixed size whatever the MTU: a frame larger than one
   buffer arrives as a *chain* of descriptors and is reassembled in
   ena_receive() (and split across a chain of slots in ena_send()). The
   reference driver does the same -- its default receive buffer is one page and
   a 9001 byte frame arrives in three descriptors (ena.c:424, ena_sysctl.h:81).

   Sizing the buffers to hold a whole jumbo frame instead is the obvious
   alternative and is deliberately rejected: it would want
   rxRingSize * 9216 bytes of *physically contiguous* memory, and that
   allocation has to succeed again on every device reset. A reset that fails
   because memory has fragmented leaves the instance with no network at all,
   which is strictly worse than the wedged NIC the watchdog was trying to
   fix. */
/* 1920 rather than the obvious 2048, and the 128 bytes are not waste -- they are
   the difference between a flat memcpy and a linked walk on every received
   segment. The stack's net_buffer slab is BUFFER_SIZE (2048) and a fresh data
   header holds BUFFER_SIZE - DATA_HEADER_SIZE - DATA_NODE_SIZE = 1952 usable
   bytes (net_buffer.cpp:42,147-149). Posting 2048-byte receive buffers therefore
   overflowed *every* segment by 96 bytes: append() had to allocate a further
   data_header and returned no contiguous buffer, which sends append_data() down
   the node-walking write_data() path instead of a single copy. On a jumbo frame
   that is roughly nine header and nine node allocations where five of each would
   do, on the busiest thread in the system, tens of thousands of times a second.
   Fitting inside 1952 costs 6% of the buffer and removes all of it.
   ceil(ETHER_MAX_JUMBO_FRAME_SIZE / 1920) is still 5, the same chain length as
   2048, so the negotiated MTU cannot regress; 1920 is also a multiple of 64 and
   so stays cacheline aligned. */
#define ENA_PACKET_BUFFER_SIZE	1920

/* MTU floor and fallback. ENA_DEFAULT_MTU is only what we use if the device
   reports a max_mtu that small; the MTU actually requested is derived from the
   device's advertised limit, capped by ETHER_MAX_JUMBO_MTU (what the ethernet
   device layer is prepared to allocate for, see ethernet.cpp) and by how much a
   descriptor chain can carry. ENA_MIN_MTU matches the reference (ena.h:148). */
#define ENA_DEFAULT_MTU		1500
#define ENA_MIN_MTU		128

/* Upper bound on the descriptors a single frame may span in either direction,
   and therefore the size of the per-packet ena_com_buf/ena_com_rx_buf_info
   arrays built on the stack. The reference driver's equivalent is
   ENA_PKT_MAX_BUFS (ena.h:117) = 19; ours only has to cover
   ceil(ETHER_MAX_JUMBO_FRAME_SIZE / ENA_PACKET_BUFFER_SIZE) = 5, and the
   device's own per-packet descriptor limit is applied on top of this at
   bring-up. The slack above 5 is there because the *device* decides how finely
   to split a received frame, and it is only obliged to respect the buffer
   lengths we posted -- not to use as few descriptors as possible. */
#define ENA_MAX_PACKET_DESCRIPTORS	8

/* How many completed packets' worth of descriptors are accumulated before
   ena_com_comp_ack() is called, mirroring the reference's ENA_TX_COMMIT
   (ena.h:131). Note that ena_com_comp_ack() is a plain `next_to_comp += n`
   with no register write, so this saves work rather than bus traffic. */
#define ENA_TX_COMMIT		32

/* Receive refill threshold: descriptors are handed back to the device in
   batches rather than one per frame, because each batch costs exactly one
   submission-queue doorbell write and that write is the expensive part. The
   reference uses min(ring_size / 8, 256) with a strict `>` test
   (ena_datapath.c:694-701, ENA_RX_REFILL_THRESH_DIVIDER / _PACKET).

   Holding descriptors back cannot starve the ring: at most
   rxRefillThreshold - 1 + ENA_MAX_PACKET_DESCRIPTORS of them are unposted at
   any moment, so with a 1024 entry ring the device always owns at least ~880.
   That also means an idle interface can sit with a partial batch unposted
   indefinitely, which is harmless for the same reason. */
#define ENA_RX_REFILL_DIVISOR		8
#define ENA_RX_REFILL_MAX_THRESHOLD	256

/* Non-adaptive interrupt moderation intervals, handed to the device in the
   interrupt-unmask register every time a vector is re-armed. The
   values are the reference driver's (ena.h:145 ENA_RX_IRQ_INTERVAL, ena.h:146
   ENA_TX_IRQ_INTERVAL), which is also where the asymmetry comes from: transmit
   completions only free descriptors, so they can wait longer than a frame that
   a socket is blocked on.

   Zero here does not mean "default", it means "interrupt on every completion",
   which is what this driver used to ask for. Both fields are 15 bits wide
   (ENA_ETH_IO_INTR_REG_{RX,TX}_INTR_DELAY_MASK), so these fit with room to
   spare.

   The unit is *device resolution ticks*, not microseconds, and the difference
   matters as soon as anyone tunes these. ena_com_update_intr_reg() writes its
   arguments straight into the register's delay fields; the microsecond ->  tick
   division lives in ena_com_update_nonadaptive_moderation_interval_rx/tx(),
   which divides by the device-reported intr_delay_resolution and which this
   driver does not call. So a tick is a microsecond exactly when the device
   reports a resolution of 1, and the value is logged at attach rather than
   assumed -- see the trace next to ena_com_init_interrupt_moderation(). */
#define ENA_RX_IRQ_INTERVAL	20
#define ENA_TX_IRQ_INTERVAL	50

/* The delay fields are 15 bits, so this is the largest interval the register can
   carry; anything wider would be silently truncated by the mask rather than
   rejected. */
#define ENA_MAX_IRQ_INTERVAL	32767

/* Adaptive receive moderation, first step (#108). The control loop samples the
   receive packet rate over a fixed wall-clock window and picks the moderation
   interval from a coarse static table; see ena_adaptive_moderation_sample() and
   graviton/docs/ena-interrupt-moderation.md.

   The window is a wall-clock span, not a frame count: it has to bound how long a
   rate spike can go unnoticed regardless of how few or many frames arrive, and
   system_time() (from CNTVCT_EL0) is the one clock trustworthy on this platform.
   100 ms is long enough that even the lowest non-idle bucket carries thousands
   of frames -- statistical mass, not a handful -- and short enough that the
   interval tracks a load change within a tenth of a second.

   ENA_MOD_LOW_PPS is the floor below which the interval is forced to zero: an
   interface this quiet is either idle or carrying latency-sensitive request/reply
   traffic, and neither must have a completion held back for a timer. Coalescing
   is only ever widened above this rate, where per-interrupt cost -- not delivery
   latency -- is what dominates. ENA_MOD_INTERVAL_MAX caps how far it is widened;
   it is deliberately far below ENA_MAX_IRQ_INTERVAL, because this first step is
   sized to be justified by a hardware A/B before it is widened, not to chase the
   largest interval the register can hold. */
#define ENA_MOD_WINDOW_US		100000
#define ENA_MOD_LOW_PPS			20000
#define ENA_MOD_INTERVAL_MAX	96

/* Watchdog cadence and timeout, both matching Linux and FreeBSD exactly: a
   one-second timer against a six-second keep-alive deadline
   (ena_netdev.h:130 ENA_DEVICE_KALIVE_TIMEOUT, ena.h:173
   ENA_DEFAULT_KEEP_ALIVE_TO). The device can also ask not to be watchdogged at
   all via a hardware hint; we do not read hints yet, which is recorded as a
   limitation in docs/watchdog-design.md rather than pretended away. */
#define ENA_WATCHDOG_INTERVAL_US	1000000
#define ENA_KEEP_ALIVE_TIMEOUT_US	6000000

/* How many consecutive checks must find the deadline missed before the device is
   reset. One is not enough, and that is measured rather than supposed: under
   sustained receive load the deadline was missed by 2.9% to 9.3% -- ages of 6174
   to 6560 ms against a 6000 ms limit -- and every one of those reset a device
   that was carrying traffic perfectly well. Ten occurrences, not one of them a
   device that had stopped. See
   graviton/docs/ena-keepalive-watchdog-false-reset.md.

   Requiring two consecutive misses is deliberately not the same thing as raising
   the deadline. A late keep-alive is transient: the next one arrives,
   lastKeepAlive advances, the count returns to zero and nothing is reset. What
   this refuses to do is reset on a single sample. A device that has genuinely
   stopped stays silent, keeps missing, and is still reset -- which is the whole
   point of the watchdog and must survive the fix.

   The arithmetic, stated plainly because the constant hides it: checks are
   ENA_WATCHDOG_INTERVAL_US apart, so N misses means silence of
   ENA_KEEP_ALIVE_TIMEOUT_US + (N-1) * ENA_WATCHDOG_INTERVAL_US, i.e. 7 s at
   N = 2 -- not 12 s. That is 0.44 s of margin over the worst stretch yet seen,
   and this is the one constant to raise if a heavier load exceeds it (N = 7 would
   give 12 s). Because the non-final misses are logged, a cadence that starts
   creeping becomes visible before it becomes a reset. */
#define ENA_KEEP_ALIVE_MISSES_BEFORE_RESET	2

/* And how many are required while the datapath is demonstrably still moving.
   N = 2 was measured and found insufficient: with it in place a healthy device
   under load was still reset once per 150 s, having gone quiet for 7290 ms.
   Raising the plain count again would just be a new guess about a tail we have
   not seen, so instead this asks a different question -- is the device actually
   dead? -- and only relaxes the deadline when the answer is demonstrably no.

   Frames still arriving or leaving is direct evidence the device has not stopped,
   so a keep-alive that is merely late costs nothing. It is a *bound*, not a
   disable: once this many deadlines have been missed the device is reset even
   with traffic flowing, which is what keeps a partial wedge -- a device still
   moving frames but whose management path has genuinely died -- catchable.

   Why the bound can be generous without blinding the watchdog: it is re-evaluated
   every tick against traffic advancing *this* tick (see ena_watchdog_check_keep_
   alive), so it only ever applies to a device that is still moving frames right
   now. A device that has actually stopped stops moving frames, and the required
   count immediately drops to ENA_KEEP_ALIVE_MISSES_BEFORE_RESET (2, ~7 s) -- so
   raising this constant does not slow detection of a dead NIC at all. Its only
   cost is delaying the reset of a still-fully-working NIC in the rare
   management-dead/datapath-alive partial wedge, where traffic still flows and
   there is no urgency.

   Eight (~13 s) was set against a 7290 ms observed worst case, but a later 8x8 GiB
   load test on this hardware saw the keep-alive gap reach ~13 s on a healthy NIC
   (all eight queue pairs then rebuilt in 66 ms, no panic -- a reset of a NIC that
   was merely busy). That put the eight-miss bound back at zero margin, the same
   marginal-deadline shape §7 warns against. Twenty misses give ~25 s of silence,
   ~1.9x over that 13 s worst case -- margin the eight-miss bound had lost -- while
   the paragraph above keeps a genuinely dead device caught in ~7 s.
   See graviton/docs/ena-keepalive-watchdog-false-reset.md. */
#define ENA_KEEP_ALIVE_MISSES_WITH_TRAFFIC	20

/* Missing-transmit-completion detection (docs/watchdog-design.md gap 4). The
   keep-alive check cannot see a wedged transmit path: an otherwise-healthy
   device keeps emitting keep-alives while a single lost completion leaves a
   frame outstanding forever. So the watchdog separately times the *oldest*
   still-outstanding transmit descriptor.

   ENA_MISSING_TX_COMPLETION_TIMEOUT_US matches the reference default
   (ena_netdev.h DEFAULT_TX_CMP_TO, 5000 ms). The device can override it through
   the NOTIFICATION/UPDATE_HINTS hardware hint (missing_tx_completion_timeout);
   we honour the hint when it is present because it is the device stating its
   own expectation, and fall back to this constant otherwise.

   OWED-TUNING: these values have NOT been swept on Graviton hardware. A frame
   that legitimately waits behind a full ring under backpressure must not be read
   as a lost completion, and the safe margin above the worst legitimate wait is
   exactly the number this project has no measurement for yet -- so the check is
   deliberately conservative on two independent axes (a generous per-frame
   deadline AND a consecutive-check requirement below) rather than trusting either
   one alone. See the false-reset history in
   graviton/docs/ena-keepalive-watchdog-false-reset.md: a check that resets a
   healthy NIC is worse than a check that misses a rare wedge. */
#define ENA_MISSING_TX_COMPLETION_TIMEOUT_US	5000000

/* How many descriptors must be over the deadline at once before the transmit
   path is judged wedged. A single straggler is not evidence; the reference
   (missing_tx_completion_threshold) uses a small count. Overridable by the
   hardware hint (missed_tx_completion_count_threshold_to_reset). OWED-TUNING. */
#define ENA_MISSING_TX_COMPLETION_THRESHOLD	4

/* And how many consecutive watchdog ticks must find the transmit path over the
   deadline before it is reset, mirroring ENA_KEEP_ALIVE_MISSES_BEFORE_RESET: a
   run, never a single sample. This is the primary guard against a spurious
   transmit-wedge reset, since a real wedge stays wedged across ticks while a
   transient backlog clears on the next one. */
#define ENA_MISSING_TX_CHECKS_BEFORE_RESET	3

/* Receive-stall detection (docs/watchdog-design.md gap 5) is DETECT-AND-LOG
   ONLY, not reset. A refill deadlock leaves descriptors owed to the device
   (rxPendingRefill > 0) making no progress, but descriptors owed is also the
   normal armed/batching state of a quiet ring, so "owed and not shrinking" on
   its own describes an idle link just as well as a stall (hardware-confirmed:
   the count over-fired every tick on a freshly-booted lightly-loaded instance,
   #211). What separates the two is whether inbound traffic is actually arriving
   and being lost: a real refill deadlock strands the device's free-buffer pool,
   so incoming frames are dropped and the device's own rx-drop counter climbs,
   whereas an idle link drops nothing. So the check additionally requires
   hwRxDrops to be climbing before it counts a tick -- proof the device has work
   it cannot place, not merely that nothing has arrived. An over-eager receive
   reset on a console-less instance is the exact failure §7 warns against, so
   even with the sharper signal the reset stays OWED until a threshold can be
   measured on hardware. */
#define ENA_RX_STALL_CHECKS_BEFORE_LOG	5

#define ENA_ADMIN_POLL_TIMEOUT_US	500000
#define ENA_MIN_POLL_DELAY_US		100

#define ENA_MAX_MULTICAST	32

#ifdef ENA_DEBUG_FAULT_INJECTION
/* Private ioctl for provoking a watchdog timeout without breaking hardware: it
   makes the keep-alive handler stop advancing the timestamp, so the watchdog sees
   a dead device while the device is healthy and any resulting failure is
   unambiguously the driver's. Chosen over unplugging something precisely for that
   reason.

   Value picked well above the ETHER_* range. Compiled out by default; the whole
   feature exists only when ENA_DEBUG_FAULT_INJECTION is defined at build time.
   Argument: 0 = off, 1 = suppress until the watchdog fires once, 2 = suppress
   until cleared (for the repeated-reset scenario).

   Mode 1 originally meant "drop one keep-alive event" and was useless: this
   device emits them about once a second against a six-second deadline, so a
   single drop can never reach the timeout. Measured, not guessed -- 90 s after a
   mode-1 request, zero triggers. The flag is now cleared by the reset path, at
   the moment the timeout it asked for has actually happened. */
#define ENA_IOCTL_SUPPRESS_KEEP_ALIVE	9800

/* Hold a reset open for N milliseconds at its widest point, so a concurrent
   teardown can be aimed at it.
   The reason this exists: a real reset is 27-84 ms on this hardware, which is far
   too narrow to hit from a shell. The torture campaign therefore never tested
   `ifconfig down` landing *inside* a reset, leaving resetLock and the `resetting`
   flag verified by code reading alone -- the weakest result in
   docs/watchdog-design.md section 8. Widening the window is the cheapest way to
   turn that into a real experiment.

   Bounded, because the hold happens with the rings already freed and the
   interface down: a long one is indistinguishable from a hang to anything using
   the network, including the ssh session running the test. */
#define ENA_IOCTL_HOLD_RESET		9801
#define ENA_MAX_RESET_HOLD_MS		30000
#endif

/* Measurement knob, always compiled in: ring the transmit doorbell N extra
   times per frame. The extra writes carry the same tail the device has already
   been told about, so they change nothing except how much MMIO the transmit path
   pays -- which is exactly how the cost of one doorbell was priced without first
   building the batched transmit entry point that real coalescing would need.

   Settable at runtime rather than only through driver settings so that the A/B
   can be interleaved inside a single boot; run-to-run transmit cost on this
   hardware is bimodal at about +-10%, which swamps the effect being measured if
   the conditions are separated by a reboot. See
   graviton/docs/ena-tx-offload.md. */
#define ENA_IOCTL_TX_EXTRA_DOORBELLS	9802
#define ENA_MAX_EXTRA_DOORBELLS		64

/* Read-only counter snapshot, always compiled in. What it exists to answer is
   how many frames one io interrupt actually accounts for: an interrupt rate on
   its own cannot tell a well-moderated device from a driver being woken once per
   frame, and that ratio is the entire subject of the interrupt-cadence work. A
   driver that is re-armed before it has consumed anything is bounded by the
   moderation interval and drains a handful of frames per interrupt; one that is
   re-armed after the drain lets a whole burst accumulate behind a masked vector
   and drains as many frames as arrived. The two are indistinguishable from
   throughput alone, so the ratio is measured directly.

   Deliberately raw totals rather than a rate: the caller picks the interval and
   the driver keeps no timers. Also deliberately outside any debug ifdef -- a
   measurement that only exists in a special build cannot be used to check that
   a change did anything in the build that ships. */
#define ENA_IOCTL_GET_IRQ_STATS		9803

/* Selects where the shared io vector is re-armed, so that the change this driver
   exists to make can be measured against the behaviour it replaces:

     0  in the interrupt handler, with every completion still unconsumed. What
        this driver shipped with. The device's moderation interval is then the
        only thing bounding the interrupt rate.
     1  at the end of a drain, once the queue reads empty (the default, and what
        both reference drivers do).

   Runtime rather than a build-time switch for the same reason as the doorbell
   knob above: separating the two arms by a reboot would put the boot-to-boot
   variation of this hardware between them, and that variation is large enough to
   swamp the effect. One binary, both arms, interleaved inside a single boot --
   and no module swap, which on this platform is the most expensive and most
   error-prone step in the loop. Reported back by ENA_IOCTL_GET_IRQ_STATS so a
   sample says which arm produced it. */
#define ENA_IOCTL_REARM_MODE		9804
#define ENA_REARM_IN_HANDLER		0
#define ENA_REARM_AFTER_DRAIN		1

/* Sets the receive moderation interval that the next re-arm will program, in the
   same device ticks as ENA_RX_IRQ_INTERVAL. Runtime for the same reason as the
   re-arm knob above, and the reason is worth restating because it is the whole
   apparatus: moderation is the second of the two mechanisms by which interrupt
   cadence could bound receive throughput, and the first one was already measured
   to move frames-per-interrupt by 27% while leaving goodput at 6.4 Gbit/s. Only
   a sweep can tell a lever from a coincidence, and a sweep whose points are
   separated by reboots measures this hardware's boot-to-boot drift instead of
   the lever.

   Transmit is deliberately *not* settable. One variable per arm: with rx alone
   moving, a change in the interrupt rate has one possible cause. Note that the
   device raises the vector when either timer expires, so the transmit interval
   still bounds the interrupt rate from below however long rx is made -- which is
   why lengthening rx cannot drive the interrupt rate to zero.

   Takes effect on the next re-arm rather than immediately, because the register
   is only written there. Under ENA_REARM_AFTER_DRAIN that is within one drain;
   the sampling windows here are seconds. */
#define ENA_IOCTL_RX_MODERATION		9805

/* Read-only ENI/device statistics snapshot, always compiled in (#106). Where
   ENA_IOCTL_GET_IRQ_STATS is scoped to the interrupt-cadence question,
   this is the general observability surface: the device's own drop counters,
   the driver's packet and byte totals per direction, the receive/transmit
   checksum-offload observations, and the reset accounting -- everything the
   syslog already narrates, gathered into one struct a tool can poll.

   Deliberately outside any debug ifdef: an operator diagnosing a live instance
   has neither a compiler nor a debug build, and drop counters that only exist in
   a special build cannot explain a production packet loss. Raw monotonic totals,
   not rates, for the same reason ENA_IOCTL_GET_IRQ_STATS is -- the caller owns
   the interval and the driver keeps no timers. Read lockless, see the handler. */
#define ENA_IOCTL_GET_ENI_STATS		9806

/* Turns adaptive receive moderation on (1) or off (0); #108. When on, the driver
   samples the receive packet rate over ENA_MOD_WINDOW_US and chooses the interval
   ENA_IOCTL_RX_MODERATION would otherwise be set to by hand -- so the two are
   mutually exclusive by construction: with adaptive on, the control loop owns
   rxIrqInterval and a manual sweep is immediately overwritten; with it off, the
   manual knob (and the compile-time default) hold.

   Runtime rather than a build switch, and a switch rather than a fixed policy,
   for the reason the whole cadence apparatus is: the honest comparison is
   adaptive against non-adaptive on one boot of one instance, because this
   hardware's boot-to-boot throughput drift is large enough to swamp the effect.
   Toggling it opens a fresh sampling window, so the first decision after the
   switch is taken over traffic seen after it, not across it. Turning it off
   restores the compile-time default interval, so the manual instrument always
   resumes from a known point rather than wherever the last window happened to
   leave it.

   Rejected with B_NOT_SUPPORTED if the device did not advertise the interrupt
   moderation feature at attach: without it the delay-resolution conversion the
   interval depends on is unknown, and a control loop programming ticks of an
   unknown length is worse than none. */
#define ENA_IOCTL_RX_ADAPTIVE_MODERATION	9807

struct ena_irq_stats {
	uint64	ioInterrupts;
	/* Unmask writes. Fewer than ioInterrupts means a vector was re-armed by one
	   direction for an interrupt the other direction also serviced, which is the
	   intended collapsing and not a lost interrupt. */
	uint64	irqArms;
	uint64	rxFrames;
	/* Times the receive ring read back empty, i.e. completed drains. rxFrames
	   divided by this is the average burst one wakeup was worth. */
	uint64	rxDrainCycles;
	uint64	txFrames;
	/* Not a cadence figure: it is here so a sample can invalidate itself. The
	   reset path zeroes ioInterrupts and does not zero rxFrames, so a reset
	   landing inside a sampling interval yields a frames-per-interrupt ratio that
	   is wrong without looking wrong. A caller that sees this move must throw the
	   sample away rather than report it. */
	uint64	resetCount;
	/* Which arm this sample was taken under: ENA_REARM_IN_HANDLER or
	   ENA_REARM_AFTER_DRAIN. Reported so a number cannot be attributed to the
	   wrong one. */
	uint64	rearmMode;
	/* The receive moderation interval in force, for the same reason: the two
	   knobs together identify the arm, and a sample that cannot name its own arm
	   is not evidence. */
	uint64	rxIrqInterval;
	/* The device's reported interrupt delay resolution, which is what converts
	   the interval above from ticks into time. Carried in the same snapshot so
	   that the conversion is a measurement taken alongside the numbers it
	   applies to, rather than an assumption made later. */
	uint64	intrDelayResolution;
};

/* Snapshot returned by ENA_IOCTL_GET_ENI_STATS. Every field is uint64 on
   purpose: it keeps the layout the same whatever the field's native width in the
   device struct, so the userland tool can duplicate this declaration without
   dragging in the driver's headers -- exactly as ena_fault does for
   ena_irq_stats. The handler checks the caller's sizeof against this, so keep
   any addition at the end and keep the two declarations in lockstep. */
struct ena_eni_stats {
	/* The device's own view, refreshed once per keep-alive from the descriptor
	   it sends: frames the hardware dropped, which the driver never sees on the
	   datapath and so cannot count itself. These are the counters #106 exists to
	   surface. */
	uint64	hwRxDrops;
	uint64	hwTxDrops;

	/* Driver-side datapath totals, monotonic per device. Packets are frames
	   passed to/from the stack; bytes are the summed L2 frame lengths. */
	uint64	rxPackets;
	uint64	rxBytes;
	uint64	txPackets;
	uint64	txBytes;

	/* Completed receive drains; rxPackets / rxDrainCycles is frames per wakeup. */
	uint64	rxDrainCycles;

	/* Receive checksum observations: frames the device validated L4 on, frames
	   that failed that validation, frames it parsed as IPv4, and frames whose L3
	   header checksum it flagged. See the offload discussion in ena_receive(). */
	uint64	rxL4CsumChecked;
	uint64	rxL4CsumErrors;
	uint64	rxL3Ipv4Frames;
	uint64	rxL3CsumErrors;

	/* Transmit checksum offload: frames handed to the device with the checksum
	   left to it, and frames dropped because the offload was requested on a frame
	   the device cannot finish. The second must stay zero. */
	uint64	txChecksumOffloaded;
	uint64	txChecksumRejected;

	/* Transmit doorbell accounting; see the txDoorbells/txBurstExhausted fields. */
	uint64	txDoorbells;
	uint64	txBurstExhausted;

	/* Resets, total and attributed by cause. resetCount counts every reset;
	   the rest break out the watchdog causes the driver now distinguishes. */
	uint64	resetCount;
	uint64	adminWedgeResets;
	uint64	fatalErrorResets;
	uint64	deviceRequestResets;
	uint64	missingTxResets;
	uint64	rxStallDetections;

	/* Context for the snapshot: link state (0/1) and the negotiated L3 MTU. */
	uint64	linkUp;
	uint64	mtu;
};

/* Refuse to attach below this, rather than dividing by a zero ring size if a
   device ever reports a nonsense depth. */
#define ENA_MIN_RING_SIZE	16

/* The narrower of the two LLQ ring entry sizes. Anything larger is the "wide"
   256 byte entry, which carries its own maximum queue depth. */
#define ENA_LLQ_NARROW_ENTRY_SIZE	128

/* RSS indirection table size, as a log2, matching Amazon's drivers. */
#define ENA_RSS_TABLE_LOG_SIZE	7
#define ENA_RSS_TABLE_SIZE	(1 << ENA_RSS_TABLE_LOG_SIZE)
#define ENA_RSS_HASH_KEY_SIZE	40


/* One bounce slot per descriptor, in each direction. The transmit and receive
   rings both number their request ids from zero, so they must not share
   storage. */
struct ena_packet_buffer {
	void*		data;
	phys_addr_t	physicalAddress;
	uint32		index;
};


struct ena_tx_buffer {
	ena_packet_buffer slot;
	net_buffer*	buffer;
	/* How many submission-queue entries this packet occupied, so the right
	   number can be acknowledged on completion. This is what
	   ena_com_prepare_tx() reported, so it includes a meta descriptor if one
	   was emitted. */
	uint16		descriptors;

	/* A frame longer than one bounce slot is copied into a chain of them. The
	   slots are drawn from the same txFreeIds stack as request ids -- a slot
	   and a request id are the same resource, since txBuffers is indexed by
	   request id -- so a five-slot frame consumes five ids and gives all five
	   back on completion. Only segmentIds[0] is told to the device as the
	   request id, and only txBuffers[segmentIds[0]].buffer is non-NULL, so the
	   other entries are invisible to the reclaim and teardown paths.

	   segments is 0 exactly when this entry is not in use. */
	uint16		segments;
	uint16		segmentIds[ENA_MAX_PACKET_DESCRIPTORS];

	/* system_time() at which this frame was handed to the device, set under
	   txLock alongside buffer/segments and cleared on completion. The watchdog
	   reads the minimum over the outstanding entries to time the oldest
	   un-acknowledged transmit; see the missing-TX-completion check in
	   ena_watchdog(). Only meaningful while buffer != NULL. */
	bigtime_t	submittedAt;
};


/* Named to match the forward declaration ena_plat.h hands to ena-com as
   ena_netdev. ena-com only stores the pointer. */
struct ena_haiku_device {
	device_node*			node;
	pci_device_module_info*		pci;
	pci_device*			pciDevice;
	pci_info			pciInfo;

	struct ena_com_dev		comDev;
	struct ena_bus			bus;
	area_id				registerArea;
	area_id				memoryArea;

	/* Physical address width the device advertises. Anything we hand it for
	   DMA must fit, so allocations are bounded by this. */
	uint32				dmaWidth;

	uint32				managementIrq;
	uint32				ioIrq;
	/* Diagnostics: MSI-X delivery on this arm64/GICv3+ITS port is new, and a
	   silently undelivered vector is otherwise indistinguishable from a
	   device that never completed a command. */
	/* Which MSI-X table index the io queues were actually created with. */
	uint32				ioVector;
	int32				managementInterrupts;
	int32				ioInterrupts;
	/* Whether the shared io vector is currently armed. The device masks a vector
	   by raising it, and the re-arm happens at the end of a drain rather than in
	   the handler, so this is what stops the two directions from both writing the
	   unmask register for the same interrupt. Written with atomic_test_and_set()
	   from either datapath and cleared by the handler; see
	   ena_rearm_io_interrupt(). */
	int32				irqArmed;
	/* How many unmask writes that produced, as a check that the re-arm is
	   actually reached: zero arms with a rising interrupt count would mean the
	   vector is being re-armed by something other than the drain. */
	int32				irqArms;
	/* ENA_REARM_IN_HANDLER or ENA_REARM_AFTER_DRAIN; see ENA_IOCTL_REARM_MODE.
	   Read on the interrupt path, so a plain atomic load rather than anything
	   that could block. */
	int32				rearmMode;
	/* The receive moderation interval the next re-arm will program, in device
	   ticks; see ENA_IOCTL_RX_MODERATION. Read on the re-arm path, which can run
	   from the interrupt handler, so again a plain atomic load. Written by the
	   manual knob and, when adaptive moderation is on, by the control loop. */
	int32				rxIrqInterval;
	/* Adaptive receive moderation on/off; see ENA_IOCTL_RX_ADAPTIVE_MODERATION and
	   ena_adaptive_moderation_sample(). Atomic because the enabling ioctl and the
	   sampling receive thread read it without a shared lock. */
	int32				rxAdaptive;
	/* Whether the device advertised the interrupt-moderation feature at attach,
	   which is what makes the delay-resolution conversion -- and therefore an
	   interval in ticks -- meaningful. Latched once; adaptive moderation is
	   refused if it is false. */
	bool				moderationSupported;
	/* Sampling-window state for adaptive moderation, touched only from the receive
	   drain (under rxLock) except that the enabling ioctl clears moderationWindowStart
	   to reopen the window. Zero start means "open a fresh window on the next
	   sample". These are wall-clock and frame-count anchors, not cadence figures. */
	bigtime_t			moderationWindowStart;
	uint64				moderationWindowFrames;
	bool				managementIrqInstalled;
	bool				ioIrqInstalled;
	/* Two states, not one: configure_msix() claims the vectors and is undone by
	   unconfigure_msi(), while enable_msix() only flips the device's enable bit.
	   The teardown paths key on msixConfigured, because a failure between the
	   two would otherwise leave the bus manager holding vectors forever and
	   every later ConfigureMSIX() returning B_BUSY. */
	bool				msixConfigured;
	bool				msixEnabled;

	struct ena_com_io_sq*		txSubmissionQueue;
	struct ena_com_io_cq*		txCompletionQueue;
	struct ena_com_io_sq*		rxSubmissionQueue;
	struct ena_com_io_cq*		rxCompletionQueue;

	uint16				txRingSize;
	uint16				rxRingSize;

	/* TX. txFreeIds is a stack of unused request ids; the device echoes a
	   request id back on completion, which is how we find the net_buffer to
	   release. */
	ena_tx_buffer*			txBuffers;
	area_id				txBufferArea;
	uint16*				txFreeIds;
	uint16				txFreeCount;
	mutex				txLock;
	sem_id				txCompleted;

	/* RX. One preallocated buffer per descriptor, indexed by request id. */
	ena_packet_buffer*		rxBuffers;
	area_id				rxBufferArea;
	uint16				rxNextToFill;
	/* Descriptors that have been drained but not yet handed back to the
	   device. Flushed by ena_return_receive_descriptors() once it reaches
	   rxRefillThreshold, so that one doorbell write covers a whole batch.
	   Reset wherever rxNextToFill is, because the two describe the same ring
	   and a stale count would post descriptors the device already owns. */
	uint16				rxPendingRefill;
	uint16				rxRefillThreshold;
	mutex				rxLock;
	/* Guards the per-device datapath setup in ena_open()/ena_close(), which must
	   happen once however many times the node is opened. */
	int32				openCount;
	sem_id				rxReady;

	/* --- device watchdog ------------------------------------------------- */
	/* The device emits ENA_ADMIN_KEEP_ALIVE events; their absence means it has
	   wedged, and on a console-less instance that is an unreachable machine
	   unless the driver resets it itself. See docs/watchdog-design.md. */

	/* Only true if the device actually granted the KEEP_ALIVE AENQ group. Both
	   reference drivers gate the watchdog on this rather than resetting a device
	   that never promised to send anything. */
	bool				watchdogActive;
	thread_id			watchdogThread;
	/* The watchdog sleeps on this with a one-second timeout, so teardown can
	   wake it at once instead of waiting out the last second. */
	sem_id				watchdogWake;
	bool				watchdogExiting;

	/* system_time() of the last keep-alive. Seeded at three points -- thread
	   start, end of a reset, and interface up -- because a zero here means
	   "last seen at the epoch" and would reset a healthy NIC six seconds into
	   every boot. */
	int64				lastKeepAlive;
	/* Consecutive watchdog checks that have found the keep-alive deadline
	   missed. Reset by the first check that finds the device talking again, so it
	   counts a run of misses and not a total. Touched only by the watchdog
	   thread, so it needs no atomics; see ENA_KEEP_ALIVE_MISSES_BEFORE_RESET. */
	uint32				keepAliveMisses;
	/* rxFrames + txFrames as of the previous watchdog check, so the watchdog can
	   tell whether the datapath moved at all in the last interval. Read without
	   either datapath lock: the question is only "did this change", and a torn or
	   stale read can at worst cost one interval's worth of patience. */
	uint64				watchdogLastTraffic;

	/* Set for the duration of a reset. The datapath tests it *under* txLock or
	   rxLock, never on its own: a bare flag is check-then-act, and a receiver
	   already inside ena_com_rx_pkt() when the rings are freed dereferences a
	   null page. */
	bool				resetting;
	mutex				resetLock;
	/* Reset gave up. The device is left inert rather than half-built. */
	bool				deviceDead;
	int32				resetCount;

	/* Counters the device reports in the keep-alive descriptor. */
	uint64				hwRxDrops;
	uint64				hwTxDrops;

	/* Asynchronous reset request, funnelled here from the AENQ handlers and the
	   admin-state check so the *thread* performs the reset. The AENQ handlers run
	   in management-interrupt context and must not block, but a reset issues admin
	   commands and takes mutexes -- the same reason the keep-alive handler only
	   records a timestamp. So FATAL_ERROR, DEVICE_REQUEST_RESET and a wedged admin
	   queue set resetRequest (with resetRequestReason) here, and ena_watchdog()
	   drains it on the next tick in a context that is allowed to block.
	   resetRequest is a plain 0/1 published with atomic_set(); resetRequestReason
	   is the enum ena_regs_reset_reason_types to pass through. */
	int32				resetRequest;
	int32				resetRequestReason;

	/* Consecutive watchdog ticks that have found the oldest outstanding transmit
	   descriptor over the deadline. A run, not a total, cleared the moment the
	   transmit path makes progress -- see ENA_MISSING_TX_CHECKS_BEFORE_RESET.
	   Touched only by the watchdog thread. */
	uint32				missingTxChecks;

	/* Receive-stall detection state, watchdog-thread only. rxStallLastPending is
	   rxPendingRefill as of the previous tick; rxStallLastDrops is hwRxDrops as of
	   the previous tick, so a tick only counts when the device's rx-drop counter
	   is climbing (traffic arriving and being lost, i.e. not an idle link, #211);
	   rxStallChecks counts consecutive ticks that meet all of: descriptors owed,
	   no receive progress, and drops still climbing. Used for logging only (gap 5
	   is detect-and-log, not reset). */
	uint16				rxStallLastPending;
	uint64				rxStallLastDrops;
	uint32				rxStallChecks;

	/* Hardware hints, from the NOTIFICATION/UPDATE_HINTS AENQ event. The device
	   states its own timeout expectations here; we store them rather than pretend
	   they do not exist (docs/watchdog-design.md §10). Written from the management
	   interrupt, read by the watchdog thread; a torn read costs at most one tick's
	   worth of the previous value. hwHintsReceived gates whether any of these are
	   authoritative over the compile-time defaults. */
	bool				hwHintsReceived;
	/* True when the device asked (driver_watchdog_timeout == NO_TIMEOUT) not to be
	   watchdogged for keep-alives at all. Honoured because it can only *reduce*
	   resets. A numeric keep-alive-timeout hint is deliberately NOT auto-applied to
	   the keep-alive deadline: the measured-safe value is a compile-time constant
	   with a consecutive-miss guard, and narrowing it from a hint is owed-tuning
	   (§7, §10). */
	bool				hwHintNoKeepAliveTimeout;
	/* Device-suggested missing-TX-completion timeout (ms) and threshold, applied
	   to the transmit-wedge check when present. Zero means "use the driver
	   default"; see the hint struct in ena_admin_defs.h. */
	uint32				hwHintTxCompletionTimeoutMs;
	uint32				hwHintTxCompletionThreshold;

	/* Observability: how many resets each newly-implemented cause has triggered.
	   Monotonic per device, logged in the reset lines and surfaced by
	   ENA_IOCTL_GET_ENI_STATS (#106), which is also what makes the syslog
	   self-describing. */
	uint32				adminWedgeResets;
	uint32				fatalErrorResets;
	uint32				deviceRequestResets;
	uint32				missingTxResets;
	uint32				rxStallDetections;

#ifdef ENA_DEBUG_FAULT_INJECTION
	/* Debug only, compiled out by default: makes the keep-alive handler stop
	   updating lastKeepAlive so the watchdog observes a dead device while the
	   device is in fact healthy. 0 = off, 1 = suppress until one timeout has
	   fired (cleared by the reset path), 2 = suppress until cleared. */
	int32				suppressKeepAlive;
	/* Milliseconds to stall inside a reset, at the point where the rings are
	   already freed. Lets a concurrent ifconfig down/up be aimed at a window that
	   is otherwise 27-84 ms wide. */
	int32				holdResetMs;
#endif

	uint8				macAddress[ETHER_ADDRESS_LENGTH];
	/* frameSize is the L3 MTU: what SET_FEATURE(MTU) was given and what the
	   stack derives its own MTU from. maxFrameSize is the same thing plus the
	   ethernet header, i.e. the largest buffer->size ena_send() will accept and
	   what ETHER_GETFRAMESIZE reports. */
	uint32				frameSize;
	uint32				maxFrameSize;
	uint32				maxSupportedMtu;

	/* How many descriptors one frame may span, per direction. Derived at
	   bring-up from the device's advertised per-packet limits and clamped to
	   ENA_MAX_PACKET_DESCRIPTORS; frameSize is then capped so that a maximum
	   frame always fits in this many ENA_PACKET_BUFFER_SIZE buffers. The
	   transmit figure already excludes the slot ena_com_prepare_tx() may need
	   for a meta descriptor. */
	uint16				rxMaxDescriptors;
	uint16				txMaxDescriptors;

	/* The device's stateless offload descriptor, as read at bring-up. Kept and
	   logged rather than discarded, because it had never been looked at and
	   because what it says is surprising: on c7g the device reports
	   rx_supported 0x7 and rx_enabled 0x0 while measurably validating L4
	   checksums on 98% of frames. So rx_enabled does not describe what the device
	   is doing and must not be used to gate anything -- these are diagnostics,
	   not control inputs. The receive path keys off the per-frame descriptor bits
	   instead; see ena_receive(). */
	uint32				offloadRxSupported;
	uint32				offloadRxEnabled;

	/* What the device actually reports per frame, as opposed to what it says it
	   supports. rx_enabled reading 0 while rx_supported reads 0x7 is ambiguous on
	   its own -- it could mean the device is not checking, or it could mean
	   rx_enabled is vestigial and the per-descriptor bits are the real answer --
	   and those two cases call for opposite code. Counting the descriptor bits
	   settles it. Deliberately outside any debug ifdef so they exist in every
	   build; they are two increments on a path that already does a memcpy per
	   frame. Read under rxLock, like everything else here. */
	uint64				rxFrames;
	/* Bytes handed to the stack, summed from each reassembled net_buffer's size,
	   so it is the L2 frame length the way an ENI byte counter reports it. Under
	   rxLock with rxFrames; surfaced by ENA_IOCTL_GET_ENI_STATS (#106). */
	uint64				rxBytes;
	/* Completed receive drains: incremented where ena_com_rx_pkt() reads the ring
	   empty, which is the point the vector is re-armed. rxFrames / rxDrainCycles
	   is the average number of frames one wakeup was worth, and is the number the
	   interrupt-cadence work has to move. Under rxLock with the rest of these. */
	uint64				rxDrainCycles;
	uint64				rxL4CsumChecked;
	uint64				rxL4CsumErrors;
	uint64				rxL3Ipv4Frames;
	uint64				rxL3CsumErrors;

	bool				linkUp;
	bool				nonBlocking;
	bool				promiscuous;
	bool				running;

	uint32				multicastCount;
	ether_address_t			multicast[ENA_MAX_MULTICAST];

	/* --- transmit doorbell accounting ------------------------------------ */
	/* Measurement, not diagnostics: the question these answer is whether
	   deferring the per-frame doorbell could ever amortise it on this device.
	   In LLQ mode the device grants a burst of only
	   llq_info.max_entries_in_tx_burst ring entries between doorbells, and a
	   doorbell is what refills that allowance -- so if one frame consumes the
	   whole burst, no two consecutive frames can share a doorbell however
	   clever the caller is. txBurstExhausted counts frames that left the
	   allowance at zero. See graviton/docs/ena-tx-offload.md. */
	uint64				txFrames;
	/* Bytes accepted for transmit, summed from each net_buffer's size under
	   txLock alongside txFrames; the transmit counterpart of rxBytes and likewise
	   surfaced by ENA_IOCTL_GET_ENI_STATS (#106). */
	uint64				txBytes;
	uint64				txDoorbells;
	uint64				txBurstExhausted;
	uint16				txBurstLeftMin;

	/* Debug knob, driver settings "tx_extra_doorbells": ring the doorbell this
	   many extra times per frame. Writing the same tail again is a no-op for
	   the device, so the only thing it changes is how much MMIO the transmit
	   path pays -- which is how the cost of one doorbell was priced without
	   having to build the batched entry point first. Zero unless asked for. */
	int32				txExtraDoorbells;

	/* --- transmit checksum offload --------------------------------------- */
	/* net_device_tx_checksum bits, as reported through
	   ETHER_GET_TX_CHECKSUM_OFFLOAD. Zero unless the device advertised the
	   partial (pseudo-header-seeded) form *and* transmit runs in LLQ placement
	   *and* the driver settings did not turn it off. Once this is non-zero the
	   stack stops computing TCP checksums for this interface, so it must never
	   claim more than ena_prepare_tx_checksum() can actually deliver. */
	uint32				txChecksumOffload;

	/* Frames handed to the device with the checksum left to it, and frames that
	   arrived asking for that but did not survive validation. The second must
	   stay at zero: it means something above set
	   NET_BUFFER_L4_CHECKSUM_NEEDED on a frame this device cannot finish, and
	   those frames are dropped rather than put on the wire with a wrong
	   checksum. */
	uint64				txChecksumOffloaded;
	uint64				txChecksumRejected;
};


#endif	/* ENA_H */
