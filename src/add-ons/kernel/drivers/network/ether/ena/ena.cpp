/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Driver for the AWS Elastic Network Adapter (ENA).
 *
 * ENA is the only network device an EC2 Nitro instance offers, so without this
 * driver a Haiku instance has no network at all -- not even to reach the
 * instance metadata service. Note that no emulator implements ENA, so this can
 * only be tested on a real instance.
 *
 * The device is driven through Amazon's ena-com HAL, which lives verbatim in
 * ena-com/ and reaches Haiku through ena-com/ena_plat.h. This file is the
 * driver proper: PCI attach, interrupts, queue setup and the Haiku ethernet
 * device API.
 *
 * Scope of this version: one TX/RX queue pair, and a frame may span several
 * descriptors in either direction, so the MTU is whatever the device advertises
 * up to the jumbo ceiling the ethernet device layer will accept. Layer-4
 * checksums are offloaded in both directions. Frames are still copied through
 * per-descriptor bounce buffers, and multiple queues with RSS remain a separate
 * change.
 */


#include "ena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <net/if_media.h>
#include <netinet/in.h>

#include <driver_settings.h>
#include <kernel.h>
#include <ksystem_info.h>
#include <util/AutoLock.h>
#include <vm/vm.h>


//#define TRACE_ENA
#ifdef TRACE_ENA
#	define TRACE(x...)	dprintf("ena: " x)
#else
#	define TRACE(x...)	;
#endif
#define TRACE_ALWAYS(x...)	dprintf("ena: " x)
#define ERROR(x...)		dprintf("\33[33mena:\33[0m " x)
#define CALLED()		TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


/* MSI-X table indices as the device understands them. These are the values
   handed to the device in ena_com_create_io_ctx::msix_vector, and are not the
   IRQ numbers Haiku uses. */
#define ENA_MGMNT_VECTOR_IDX	0
#define ENA_IO_VECTOR_IDX	1
#define ENA_MSIX_VECTOR_COUNT	2


static device_manager_info* sDeviceManager;
static net_buffer_module_info* sBufferModule;

static struct ena_aenq_handlers sAenqHandlers;


//	#pragma mark - error translation


/*!	Turns an ena-com result into a Haiku status_t.

	ena-com deals in small positive POSIX-style codes -- it has to, because it
	passes them through pointers (see the ENA_COM_* block in ena_plat.h) -- so
	they must never be returned to the kernel as a status_t: 12 would read as
	success. Everything crossing out of the HAL goes through here.
*/
static status_t
ena_translate_error(int error)
{
	switch (error) {
		case ENA_COM_OK:		return B_OK;
		case ENA_COM_FAULT:		return B_BAD_ADDRESS;
		case ENA_COM_INVAL:		return B_BAD_VALUE;
		case ENA_COM_NO_MEM:		return B_NO_MEMORY;
		case ENA_COM_NO_SPACE:		return B_BUFFER_OVERFLOW;
		case ENA_COM_TRY_AGAIN:		return B_WOULD_BLOCK;
		case ENA_COM_UNSUPPORTED:	return B_NOT_SUPPORTED;
		case ENA_COM_NO_DEVICE:		return B_DEV_NOT_READY;
		case ENA_COM_PERMISSION:	return B_PERMISSION_DENIED;
		case ENA_COM_TIMER_EXPIRED:	return B_TIMED_OUT;
		case ENA_COM_EIO:		return B_IO_ERROR;
		case ENA_COM_DEVICE_BUSY:	return B_BUSY;
		default:			return B_ERROR;
	}
}


//	#pragma mark - asynchronous event notification queue


static void
ena_aenq_link_change(void* data, struct ena_admin_aenq_entry* entry)
{
	ena_haiku_device* device = (ena_haiku_device*)data;
	struct ena_admin_aenq_link_change_desc* description
		= (struct ena_admin_aenq_link_change_desc*)entry;

	device->linkUp = (description->flags
		& ENA_ADMIN_AENQ_LINK_CHANGE_DESC_LINK_STATUS_MASK) != 0;

	TRACE_ALWAYS("link is %s\n", device->linkUp ? "up" : "down");
}


static void
ena_aenq_keep_alive(void* data, struct ena_admin_aenq_entry* entry)
{
	ena_haiku_device* device = (ena_haiku_device*)data;
	struct ena_admin_aenq_keep_alive_desc* description
		= (struct ena_admin_aenq_keep_alive_desc*)entry;

	/* Both reference drivers pull the device's own drop counters out of this
	   descriptor while they are here; it is the only place they are reported. */
	device->hwRxDrops = ((uint64)description->rx_drops_high << 32)
		| description->rx_drops_low;
	device->hwTxDrops = ((uint64)description->tx_drops_high << 32)
		| description->tx_drops_low;

#ifdef ENA_DEBUG_FAULT_INJECTION
	/* Debug fault injection: stop advancing the timestamp so the watchdog sees a
	   dead device while the device is in fact perfectly healthy. That way a
	   failure during the test is unambiguously ours.
	   Mode 1 means "suppress until the watchdog has fired once", not "drop one
	   event". Dropping a single keep-alive cannot ever trigger a timeout, because
	   this device emits them roughly once a second against a six-second deadline
	   -- measured: 90 s after a mode-1 request, zero triggers. It is the reset
	   path that clears mode 1, so both modes stay suppressed here. */
	if (device->suppressKeepAlive != 0)
		return;
#endif

	/* Deliberately the only thing this handler decides: record, never act. The
	   watchdog thread does the deciding, because a reset issues admin commands
	   and blocks, and this runs from the management interrupt. */
	atomic_set64(&device->lastKeepAlive, system_time());
}


/*!	Records that the device should be reset, without doing it here.

	The AENQ handlers run from the management interrupt, where blocking is not
	allowed, but a reset issues admin commands and takes mutexes. So the device's
	own reset triggers -- FATAL_ERROR and DEVICE_REQUEST_RESET -- do exactly what
	the keep-alive handler does: record and return. ena_watchdog() drains the
	request on its next tick, in a context that may block. Publishing the reason
	before the request keeps the pair consistent from the reader's side even
	without a lock: the watchdog only reads the reason once it has seen the flag.
*/
static void
ena_request_reset(ena_haiku_device* device,
	enum ena_regs_reset_reason_types reason)
{
	atomic_set(&device->resetRequestReason, (int32)reason);
	atomic_set(&device->resetRequest, 1);
}


static void
ena_aenq_fatal_error(void* data, struct ena_admin_aenq_entry* entry)
{
	ena_haiku_device* device = (ena_haiku_device*)data;

	/* The device is telling us it has hit a fatal error. There is no partial
	   recovery from this short of a full reset, and unlike the keep-alive
	   timeout there is no false-positive question to weigh: the device asked.
	   Defer the actual reset to the watchdog thread. */
	ERROR("device reported a FATAL_ERROR AENQ event (syndrome %u); requesting "
		"reset\n", entry->aenq_common_desc.syndrome);
	ena_request_reset(device, ENA_REGS_RESET_GENERIC);
}


static void
ena_aenq_warning(void* data, struct ena_admin_aenq_entry* entry)
{
	/* Advisory only: the device is flagging a condition it wants noted, not a
	   reason to reset. Previously this fell to ena_aenq_unimplemented() and was
	   logged as an unhandled error, which mislabelled a benign event. */
	TRACE_ALWAYS("device reported a WARNING AENQ event (syndrome %u)\n",
		entry->aenq_common_desc.syndrome);
}


static void
ena_aenq_notification(void* data, struct ena_admin_aenq_entry* entry)
{
	ena_haiku_device* device = (ena_haiku_device*)data;

	/* The NOTIFICATION group carries the device's hardware hints -- among them
	   its own keep-alive and missing-TX-completion timeout expectations. The
	   driver stored none of this before, so the compile-time constants were used
	   even where the device was willing to supply the value (docs/watchdog-design.md
	   §10). Record the hints; the watchdog reads them on its own thread. */
	switch (entry->aenq_common_desc.syndrome) {
		case ENA_ADMIN_UPDATE_HINTS:
		{
			struct ena_admin_ena_hw_hints* hints
				= (struct ena_admin_ena_hw_hints*)&entry->inline_data_w4[0];

			/* NO_TIMEOUT means "do not watchdog me for keep-alives". Honoured
			   because it can only reduce resets. A numeric keep-alive timeout is
			   deliberately not auto-applied to the deadline: the measured-safe
			   value is a constant with a consecutive-miss guard and narrowing it
			   from a hint is owed-tuning (§7). */
			device->hwHintNoKeepAliveTimeout
				= hints->driver_watchdog_timeout == ENA_HW_HINTS_NO_TIMEOUT;

			/* The missing-TX-completion check is new and device-authoritative, so
			   its thresholds do follow the hint when present. Zero means "use the
			   driver default", per the hint struct. */
			device->hwHintTxCompletionTimeoutMs
				= hints->missing_tx_completion_timeout;
			device->hwHintTxCompletionThreshold
				= hints->missed_tx_completion_count_threshold_to_reset;

			device->hwHintsReceived = true;

			TRACE_ALWAYS("device hardware hints: watchdog_timeout %u ms%s, "
				"missing_tx_completion %u ms, tx_completion_threshold %u\n",
				hints->driver_watchdog_timeout,
				device->hwHintNoKeepAliveTimeout ? " (NO_TIMEOUT)" : "",
				hints->missing_tx_completion_timeout,
				hints->missed_tx_completion_count_threshold_to_reset);
			break;
		}

		default:
			TRACE_ALWAYS("device NOTIFICATION AENQ event, unhandled syndrome %u\n",
				entry->aenq_common_desc.syndrome);
			break;
	}
}


static void
ena_aenq_device_request_reset(void* data, struct ena_admin_aenq_entry* entry)
{
	ena_haiku_device* device = (ena_haiku_device*)data;

	/* The device is explicitly asking to be reset. Like FATAL_ERROR this is
	   device-driven, so there is no false-positive question: defer it to the
	   watchdog thread with the reason the register defines for it. */
	ERROR("device requested a reset via AENQ (syndrome %u)\n",
		entry->aenq_common_desc.syndrome);
	ena_request_reset(device, ENA_REGS_RESET_DEVICE_REQUEST);
}


static void
ena_aenq_unimplemented(void* data, struct ena_admin_aenq_entry* entry)
{
	ERROR("unhandled AENQ event, group %u syndrome %u\n",
		entry->aenq_common_desc.group, entry->aenq_common_desc.syndrome);
}


static void
ena_init_aenq_handlers()
{
	memset(&sAenqHandlers, 0, sizeof(sAenqHandlers));

	/* Not a designated-initialiser table: those are a C99 feature that C++
	   does not have. */
	sAenqHandlers.handlers[ENA_ADMIN_LINK_CHANGE] = ena_aenq_link_change;
	sAenqHandlers.handlers[ENA_ADMIN_FATAL_ERROR] = ena_aenq_fatal_error;
	sAenqHandlers.handlers[ENA_ADMIN_WARNING] = ena_aenq_warning;
	sAenqHandlers.handlers[ENA_ADMIN_NOTIFICATION] = ena_aenq_notification;
	sAenqHandlers.handlers[ENA_ADMIN_KEEP_ALIVE] = ena_aenq_keep_alive;
	sAenqHandlers.handlers[ENA_ADMIN_DEVICE_REQUEST_RESET]
		= ena_aenq_device_request_reset;
	sAenqHandlers.unimplemented_handler = ena_aenq_unimplemented;
}


//	#pragma mark - interrupts


static int32
ena_management_interrupt(void* arg)
{
	ena_haiku_device* device = (ena_haiku_device*)arg;

	if (atomic_add(&device->managementInterrupts, 1) == 0)
		TRACE_ALWAYS("first management interrupt delivered\n");

	ena_com_admin_q_comp_intr_handler(&device->comDev);

	/* The AENQ is only live once admin init has completed and we have left
	   polling mode; before that the ring does not exist. */
	if (device->running)
		ena_com_aenq_intr_handler(&device->comDev, device);

	/* The admin completion handler wakes a waiting thread, so let the
	   scheduler run rather than making it wait for the next tick. */
	return B_INVOKE_SCHEDULER;
}


/*!	Re-arms the shared io vector, at most once per interrupt.

	ENA masks a vector when it raises it, so it must be re-armed or the interrupt
	just taken is the last one we ever see. Each completion queue carries its own
	unmask register offset, and both of Amazon's drivers re-arm a queue pair
	through its \em transmit CQ -- so this must be txCompletionQueue even though
	receive is what we mostly care about. Re-arming also reprograms the moderation
	timers, which is why the intervals are passed on every call.

	Called at the end of a drain, never from the interrupt handler: that is what
	makes a delivered interrupt mean "work has arrived" rather than "work has
	arrived and is still sitting there". While the vector stays masked the whole
	of a burst accumulates behind it and is consumed by one wakeup, instead of
	each completion re-raising a vector that only the device's moderation
	interval was holding back.

	\a force is for the bring-up and reset paths, where the completion queue has
	just been created and starts masked whatever \c irqArmed happens to say.
*/
static void
ena_rearm_io_interrupt(ena_haiku_device* device, bool force)
{
	if (device->txCompletionQueue == NULL)
		return;

	if (force)
		atomic_set(&device->irqArmed, 0);

	/* Whichever direction finishes draining first performs the single unmask
	   write; the other finds the vector already armed and skips it. Both have to
	   try, because either one can be the last to finish -- see the ownership note
	   in ena_io_interrupt() for why this is not a two-sided handshake. */
	if (atomic_test_and_set(&device->irqArmed, 1, 0) != 0)
		return;

	/* The receive interval is read from the device rather than used as a constant
	   so that it can be swept at runtime; see ENA_IOCTL_RX_MODERATION. Transmit
	   stays at the compile-time value, because a sweep that moved both could not
	   attribute a change in the interrupt rate to either. */
	struct ena_eth_io_intr_reg interruptRegister;
	ena_com_update_intr_reg(&interruptRegister,
		(uint32)atomic_get(&device->rxIrqInterval), ENA_TX_IRQ_INTERVAL, true,
		false);
	ena_com_unmask_intr(device->txCompletionQueue, &interruptRegister);

	atomic_add(&device->irqArms, 1);
}


/*!	Receive moderation intervals as a function of the observed packet rate (#108).

	Coarse and static on purpose: it is the lowest-risk shape of adaptive
	moderation, meant to be justified and then refined by a hardware A/B, not a
	port of the reference driver's DIM controller. Each entry gives the exclusive
	upper bound of a packet-per-second band and the receive interval (in device
	ticks) to use within it. The bands only ever widen the interval as the rate
	rises: at low rates per-frame delivery latency is what matters, at high rates
	per-interrupt CPU is, and the table trades one for the other in that
	direction only. The first band is the ENA_MOD_LOW_PPS floor at interval zero
	-- an interface this quiet takes an interrupt per completion so nothing
	latency-sensitive is ever held for a timer. The last band's bound is set so it
	always matches, so the lookup below cannot fall through. */
static const struct ena_moderation_bucket {
	uint32	maxPps;
	int32	interval;
} kRxModerationBuckets[] = {
	{ ENA_MOD_LOW_PPS,		0 },
	{ 50000,				16 },
	{ 150000,				48 },
	{ 0xffffffffU,			ENA_MOD_INTERVAL_MAX },
};


/*!	Adaptive receive moderation control loop, first step (#108).

	Called from the end of a receive drain, under rxLock, on the receive thread --
	the same point ena_rearm_io_interrupt() runs, so the interval chosen here is
	the one the immediately following re-arm programs into the device. Samples the
	receive packet rate over ENA_MOD_WINDOW_US of wall clock and moves
	rxIrqInterval to the matching bucket; system_time() comes from CNTVCT_EL0 and
	is trustworthy for a wall-clock span on this platform even where CPU-percentage
	accounting is not.

	A no-op unless adaptive moderation was turned on (ENA_IOCTL_RX_ADAPTIVE_
	MODERATION), so with it off the manual knob and the compile-time default are
	left untouched. rxFrames and the window anchors are written only on this
	thread; moderationWindowStart may additionally be cleared to zero by the
	enabling ioctl to reopen the window, which is a benign single-word write --
	the worst a straddling read can do is reopen the window a second time. */
static void
ena_adaptive_moderation_sample(ena_haiku_device* device)
{
	if (atomic_get(&device->rxAdaptive) == 0)
		return;

	const bigtime_t now = system_time();

	/* Fresh window: anchor it and wait for it to fill before deciding. */
	if (device->moderationWindowStart == 0) {
		device->moderationWindowStart = now;
		device->moderationWindowFrames = device->rxFrames;
		return;
	}

	const bigtime_t elapsed = now - device->moderationWindowStart;
	if (elapsed < ENA_MOD_WINDOW_US)
		return;

	const uint64 frames = device->rxFrames - device->moderationWindowFrames;
	const uint64 pps = frames * 1000000ULL / (uint64)elapsed;

	int32 interval = ENA_MOD_INTERVAL_MAX;
	for (size_t i = 0; i < sizeof(kRxModerationBuckets)
			/ sizeof(kRxModerationBuckets[0]); i++) {
		if (pps < kRxModerationBuckets[i].maxPps) {
			interval = kRxModerationBuckets[i].interval;
			break;
		}
	}

	if (interval != atomic_get(&device->rxIrqInterval)) {
		atomic_set(&device->rxIrqInterval, interval);
		/* Logged only on a band change, which tracks load transitions rather than
		   the sampling cadence, so this cannot flood the console under steady
		   traffic. */
		TRACE_ALWAYS("adaptive rx moderation: %" B_PRIu64 " pps -> interval %"
			B_PRId32 " ticks (resolution %u)\n", pps, interval,
			device->comDev.intr_delay_resolution);
	}

	/* Open the next window from here rather than from the nominal deadline: a
	   drain that ran long only reports the rate it actually observed. */
	device->moderationWindowStart = now;
	device->moderationWindowFrames = device->rxFrames;
}


static int32
ena_io_interrupt(void* arg)
{
	ena_haiku_device* device = (ena_haiku_device*)arg;

	if (atomic_add(&device->ioInterrupts, 1) == 0)
		TRACE_ALWAYS("first io interrupt delivered\n");

	/* The device masked this vector by raising it, and this handler deliberately
	   does not re-arm it -- the drain does, once there is nothing left to
	   consume. Record that it is masked so that whichever reader gets there
	   first does the one unmask.

	   Ownership is the whole difficulty, because the two directions share this
	   vector and are not symmetric:

	   - Receive has a thread of its own. This handler wakes it and it always
	     comes back round into ena_receive(), so it is a guaranteed re-armer.
	   - Transmit has no thread at all. ena_reclaim_transmitted() runs only from
	     ena_send(), so on a receive-only workload nothing on the transmit side
	     ever executes.

	   So a scheme where both directions had to acknowledge before the vector was
	   re-armed would leave it masked forever the moment transmit went idle: a
	   dead NIC, and dead precisely under the receive-only load this is meant to
	   speed up. Instead either direction may re-arm as soon as its own queue
	   reads empty, the atomic in ena_rearm_io_interrupt() collapses that to a
	   single write, and liveness rests on the receive reader alone -- which is
	   the one context this handler is certain to have woken.

	   The cost of that asymmetry is bounded and benign: receive can re-arm while
	   transmit completions are still unconsumed, which may cost one extra
	   interrupt. That interrupt does real work (it is what wakes a sender waiting
	   on a full ring), so it is not the storm this replaces, where every
	   completion re-raised a vector nothing had yet looked at. */
	if (atomic_get(&device->rearmMode) == ENA_REARM_IN_HANDLER) {
		/* The behaviour this replaces, kept switchable so the two can be
		   measured against each other in one boot. Forcing the write here also
		   leaves irqArmed set, which is what makes the re-arms at the end of both
		   drains no-ops without needing to test the mode again. */
		ena_rearm_io_interrupt(device, true);
	} else
		atomic_set(&device->irqArmed, 0);

	/* Both directions share this vector, so wake both waiters and let them
	   find out whether there was anything for them. */
	if (device->rxReady >= 0)
		release_sem_etc(device->rxReady, 1, B_DO_NOT_RESCHEDULE);
	if (device->txCompleted >= 0)
		release_sem_etc(device->txCompleted, 1, B_DO_NOT_RESCHEDULE);

	return B_INVOKE_SCHEDULER;
}


//	#pragma mark - PCI helpers


/*!	Maps one of the device's memory BARs.

	Handles the 64-bit BAR case, where the address and size are split across
	two consecutive base register slots.
*/
static status_t
ena_map_bar(ena_haiku_device* device, uint8 barIndex, const char* name,
	area_id* _area, addr_t* _address, size_t* _size,
	phys_addr_t* _physical = NULL)
{
	pci_info& info = device->pciInfo;

	uint64 address = info.u.h0.base_registers[barIndex];
	uint64 size = info.u.h0.base_register_sizes[barIndex];

	if ((info.u.h0.base_register_flags[barIndex] & PCI_address_type)
			== PCI_address_type_64) {
		address |= (uint64)info.u.h0.base_registers[barIndex + 1] << 32;
		size |= (uint64)info.u.h0.base_register_sizes[barIndex + 1] << 32;
	}

	if (address == 0 || size == 0)
		return B_DEV_NO_MEMORY;

	void* mapped = NULL;
	area_id area = map_physical_memory(name, (phys_addr_t)address,
		(size_t)size, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &mapped);
	if (area < B_OK)
		return area;

	*_area = area;
	*_address = (addr_t)mapped;
	*_size = (size_t)size;
	if (_physical != NULL)
		*_physical = (phys_addr_t)address;

	return B_OK;
}


static status_t
ena_enable_msix(ena_haiku_device* device)
{
	if (device->pci->get_msix_count(device->pciDevice) < ENA_MSIX_VECTOR_COUNT) {
		ERROR("device offers fewer than %d MSI-X vectors\n",
			ENA_MSIX_VECTOR_COUNT);
		return B_NOT_SUPPORTED;
	}

	uint32 startVector = 0;
	status_t status = device->pci->configure_msix(device->pciDevice,
		ENA_MSIX_VECTOR_COUNT, &startVector);
	if (status != B_OK)
		return status;

	/* Set before the next call can fail. configure_msix() has already claimed
	   vectors and set the bus manager's configured_count, and unconfigure_msi()
	   is the only way to release them -- so from here on the unwind path *must*
	   run even if nothing was ever enabled. Getting this wrong is not a leak
	   that a reboot merely tidies up: PCI::ConfigureMSIX() returns B_BUSY while
	   configured_count is non-zero, so a single failure here would make the
	   interface permanently unattachable for the rest of the boot. */
	device->msixConfigured = true;

	status = device->pci->enable_msix(device->pciDevice);
	if (status != B_OK)
		return status;

	/* Haiku hands back the first IRQ of a contiguous block; the device-side
	   table indices are 0..count-1 in the same order. */
	device->managementIrq = startVector + ENA_MGMNT_VECTOR_IDX;
	device->ioIrq = startVector + ENA_IO_VECTOR_IDX;
	device->msixEnabled = true;

	TRACE_ALWAYS("using MSI-X, management irq %" B_PRIu32 ", io irq %" B_PRIu32
		"\n", device->managementIrq, device->ioIrq);

	return B_OK;
}


//	#pragma mark - device bring-up


/*!	Reports host attributes to the device.

	Amazon's drivers do this during bring-up and treat a failure as
	non-fatal -- it is largely telemetry for AWS support -- but it happens
	before queue creation there, so we do it in the same place.
*/
static void
ena_config_host_info(ena_haiku_device* device)
{
	int result = ena_com_allocate_host_info(&device->comDev);
	if (result != ENA_COM_OK) {
		ERROR("cannot allocate the host info buffer: %d\n", result);
		return;
	}

	struct ena_admin_host_info* info = device->comDev.host_attr.host_info;

	info->os_type = ENA_ADMIN_OS_HAIKU;
	strlcpy((char*)info->os_dist_str, "Haiku", sizeof(info->os_dist_str));
	snprintf((char*)info->kernel_ver_str, sizeof(info->kernel_ver_str),
		"hrev%d", ENA_HAIKU_REVISION);
	info->kernel_ver = ENA_HAIKU_REVISION;
	info->driver_version = (ENA_DRIVER_VERSION_MAJOR
			& ENA_ADMIN_HOST_INFO_MAJOR_MASK)
		| ((ENA_DRIVER_VERSION_MINOR << ENA_ADMIN_HOST_INFO_MINOR_SHIFT)
			& ENA_ADMIN_HOST_INFO_MINOR_MASK)
		| ((ENA_DRIVER_VERSION_SUBMINOR
				<< ENA_ADMIN_HOST_INFO_SUB_MINOR_SHIFT)
			& ENA_ADMIN_HOST_INFO_SUB_MINOR_MASK);
	info->bdf = (device->pciInfo.bus << 8) | (device->pciInfo.device << 3)
		| device->pciInfo.function;
	info->num_cpus = (uint16)smp_get_num_cpus();

	/* What we declare here changes what the device is willing to advertise
	   back. Without RSS_CONFIGURABLE_FUNCTION_KEY the device withholds
	   ENA_ADMIN_RSS_HASH_FUNCTION from supported_features entirely, so
	   ena_com_get_feature_ex()'s support gate silently skips every hash
	   function command -- which is how our admin command stream came to be
	   missing three commands that FreeBSD sends before it creates a queue.
	   Amazon's drivers declare both bits; so do we. */
	info->driver_supported_features
		= ENA_ADMIN_HOST_INFO_RX_OFFSET_MASK
			| ENA_ADMIN_HOST_INFO_RSS_CONFIGURABLE_FUNCTION_KEY_MASK;

	result = ena_com_set_host_attributes(&device->comDev);
	if (result != ENA_COM_OK) {
		/* Not fatal: the device works without knowing about us. */
		ERROR("cannot set host attributes: %d\n", result);
		ena_com_delete_host_info(&device->comDev);
	}
}


/*!	Runs the register-level and admin-queue handshake with the device.

	The order here is prescribed by the device and mirrors Amazon's own
	drivers: read-less register access has to be set up before the reset,
	the reset before the version check, and the admin queue has to be driven
	in polling mode until we know how many interrupts to allocate.
*/
static status_t
ena_device_init(ena_haiku_device* device,
	struct ena_com_dev_get_features_ctx* features)
{
	struct ena_com_dev* comDev = &device->comDev;

	int result = ena_com_mmio_reg_read_request_init(comDev);
	if (result != ENA_COM_OK) {
		ERROR("failed to set up read-less register access\n");
		return ena_translate_error(result);
	}

	/* A bit in the PCI revision id tells us whether plain memory-mapped
	   register reads work; if not, ena-com routes reads through a DMA
	   mailbox instead. */
	uint8 revision = device->pci->read_pci_config(device->pciDevice,
		PCI_revision, 1);
	bool readlessSupported = (revision & ENA_MMIO_DISABLE_REG_READ) == 0;
	ena_com_set_mmio_read_mode(comDev, readlessSupported);

	result = ena_com_dev_reset(comDev, ENA_REGS_RESET_NORMAL);
	if (result != ENA_COM_OK) {
		ERROR("cannot reset device\n");
		goto err_mmio;
	}

	result = ena_com_validate_version(comDev);
	if (result != ENA_COM_OK) {
		ERROR("device version is too low\n");
		goto err_mmio;
	}

	{
		/* Errors come back as small positive codes, not as negatives, so
		   check the value is a plausible width rather than testing for a
		   negative return the way the HAL's own callers do. */
		int dmaWidth = ena_com_get_dma_width(comDev);
		if (dmaWidth < 32 || dmaWidth > ENA_MAX_PHYS_ADDR_SIZE_BITS) {
			ERROR("device reported an unusable DMA width (%d)\n", dmaWidth);
			result = ENA_COM_INVAL;
			goto err_mmio;
		}
		/* Everything allocated from here on is bounded by this; see the
		   conservative default set in ena_init_device(). */
		device->dmaWidth = (uint32)dmaWidth;
		TRACE_ALWAYS("device drives %" B_PRIu32 " physical address bits\n",
			device->dmaWidth);
	}

	result = ena_com_admin_init(comDev, &sAenqHandlers);
	if (result != ENA_COM_OK) {
		ERROR("cannot initialise the admin queue\n");
		goto err_mmio;
	}

	/* Interrupts are not set up yet -- and we need the device's answers to
	   decide how many to ask for -- so drive the admin queue by polling
	   until they are. */
	ena_com_set_admin_polling_mode(comDev, true);

	/* Before reading device attributes, not after: the device's reported
	   capabilities depend on it. Announcing ourselves late made it stop
	   reporting LLQ support entirely. Amazon's driver also does this first. */
	ena_config_host_info(device);

	result = ena_com_get_dev_attr_feat(comDev, features);
	if (result != ENA_COM_OK) {
		ERROR("cannot read device attributes: %d\n", result);
		goto err_admin;
	}

	/* The queue *counts*, as opposed to depths. CREATE_CQ is the only command
	   that asks the device to allocate a queue resource, and it is the only one
	   that fails, so how many it thinks it has is worth knowing. */
	if ((comDev->supported_features & BIT(ENA_ADMIN_MAX_QUEUES_EXT)) != 0) {
		struct ena_admin_queue_ext_feature_fields* q
			= &features->max_queue_ext.max_queue_ext;
		TRACE_ALWAYS("queue counts (ext v%u): tx_sq %" B_PRIu32 " tx_cq %"
			B_PRIu32 " rx_sq %" B_PRIu32 " rx_cq %" B_PRIu32 ", per-packet tx %u"
			" rx %u, max_tx_header %" B_PRIu32 "\n",
			features->max_queue_ext.version, q->max_tx_sq_num,
			q->max_tx_cq_num, q->max_rx_sq_num, q->max_rx_cq_num,
			q->max_per_packet_tx_descs, q->max_per_packet_rx_descs,
			q->max_tx_header_size);
	} else {
		struct ena_admin_queue_feature_desc* q = &features->max_queues;
		TRACE_ALWAYS("queue counts (legacy): sq %" B_PRIu32 " cq %" B_PRIu32
			", max_header %" B_PRIu32 "\n", q->max_sq_num, q->max_cq_num,
			q->max_header_size);
	}
	TRACE_ALWAYS("supported_features %#" B_PRIx32 ", capabilities %#" B_PRIx32
		"\n", comDev->supported_features, comDev->capabilities);

	TRACE_ALWAYS("device reports AENQ supported_groups %#" B_PRIx32 "\n",
		features->aenq.supported_groups);

	{
		uint32 groups = BIT(ENA_ADMIN_LINK_CHANGE)
			| BIT(ENA_ADMIN_FATAL_ERROR)
			| BIT(ENA_ADMIN_WARNING)
			| BIT(ENA_ADMIN_NOTIFICATION)
			| BIT(ENA_ADMIN_KEEP_ALIVE)
			| BIT(ENA_ADMIN_DEVICE_REQUEST_RESET);
		groups &= features->aenq.supported_groups;

		/* The watchdog is only legitimate if the device agreed to send
		   keep-alives. Both references gate on exactly this, rather than
		   resetting a device in a loop for failing to send something it never
		   promised. */
		device->watchdogActive = (groups & BIT(ENA_ADMIN_KEEP_ALIVE)) != 0;

		TRACE_ALWAYS("configuring AENQ groups %#" B_PRIx32 "%s\n", groups,
			device->watchdogActive ? "" : " (no keep-alive: watchdog disabled)");

		result = ena_com_set_aenq_config(comDev, groups);
		if (result != ENA_COM_OK) {
			ERROR("cannot configure AENQ groups: %d\n", result);
			goto err_admin;
		}
	}

	return B_OK;

err_admin:
	ena_com_admin_destroy(comDev);
err_mmio:
	ena_com_mmio_reg_read_request_destroy(comDev);

	return ena_translate_error(result);
}


/*!	Chooses and applies the transmit placement policy.

	Mirrors ena_set_llq_configurations() in Amazon's driver: inline headers,
	multiple descriptors per entry, two descriptors before the header, and the
	larger 256 byte ring entry when the device recommends it.
*/
static void
ena_configure_placement_policy(ena_haiku_device* device,
	struct ena_admin_feature_llq_desc* llq)
{
	struct ena_com_dev* comDev = &device->comDev;

	if ((comDev->supported_features & BIT(ENA_ADMIN_LLQ)) == 0) {
		TRACE_ALWAYS("device does not support LLQ; using host placement\n");
		return;
	}
	if (comDev->mem_bar == NULL)
		return;

	struct ena_llq_configurations config;
	config.llq_header_location = ENA_ADMIN_INLINE_HEADER;
	config.llq_stride_ctrl = ENA_ADMIN_MULTIPLE_DESCS_PER_ENTRY;
	config.llq_num_decs_before_header
		= ENA_ADMIN_LLQ_NUM_DESCS_BEFORE_HEADER_2;

	bool large = (llq->entry_size_recommended
			== ENA_ADMIN_LIST_ENTRY_SIZE_256B)
		&& (llq->entry_size_ctrl_supported
			& ENA_ADMIN_LIST_ENTRY_SIZE_256B) != 0;
	if (large) {
		config.llq_ring_entry_size = ENA_ADMIN_LIST_ENTRY_SIZE_256B;
		config.llq_ring_entry_size_value = 256;
	} else {
		config.llq_ring_entry_size = ENA_ADMIN_LIST_ENTRY_SIZE_128B;
		config.llq_ring_entry_size_value = 128;
	}

	/* The depth limits that apply in device-placement mode, which we do not
	   currently honour: with LLQ the transmit depth is bounded by
	   max_llq_depth rather than max_tx_sq_depth, and with 256 byte ("wide")
	   entries by max_wide_llq_depth -- halved if the device reports zero. See
	   amzn-drivers#368, where ignoring exactly this produced UNKNOWN_ERROR on
	   queue creation for another from-scratch driver on Nitro v4. */
	TRACE_ALWAYS("LLQ negotiation: max_llq_num %" B_PRIu32 ", max_llq_depth %"
		B_PRIu32 ", max_wide_llq_depth %u, entry_size_recommended %u, "
		"entry_size_supported %#x, accel_mode supported %#x\n",
		llq->max_llq_num, llq->max_llq_depth, llq->max_wide_llq_depth,
		llq->entry_size_recommended, llq->entry_size_ctrl_supported,
		llq->accel_mode.u.get.supported_flags);

	int result = ena_com_config_dev_mode(comDev, llq, &config);
	if (result != ENA_COM_OK) {
		ERROR("cannot configure LLQ (%d); falling back to host placement\n",
			result);
		comDev->tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
		return;
	}

	TRACE_ALWAYS("LLQ configured: %u byte entries, %u descriptors per entry, "
		"max %u per burst, transmit header limit %" B_PRIu32 "\n",
		comDev->llq_info.desc_list_entry_size,
		comDev->llq_info.descs_per_entry,
		comDev->llq_info.max_entries_in_tx_burst,
		comDev->tx_max_header_size);
}


/*!	Records what the device says it can offload, and reads the transmit knobs.

	Nothing here changes behaviour on its own. The capability word is wanted for
	two separate reasons: transmit checksum offload needs to know whether the
	device will finish a partially-computed L4 sum or insists on computing the
	whole thing, and the doorbell question (see the txBurst* counters in ena.h)
	needed the LLQ burst allowance on the record before any code was written for
	it.

	The bit layout is documented at ena_admin_defs.h:846-858.
*/
static void
ena_report_offload_capabilities(ena_haiku_device* device,
	struct ena_com_dev_get_features_ctx* features)
{
	const struct ena_admin_feature_offload_desc* offload = &features->offload;

	TRACE_ALWAYS("offload: tx %#" B_PRIx32 " (ipv4 l3 csum %d, ipv4 l4 csum "
		"part %d full %d, ipv6 l4 csum part %d full %d, tso v4 %d v6 %d), "
		"rx supported %#" B_PRIx32 ", rx enabled %#" B_PRIx32 "\n",
		offload->tx,
		(int)get_ena_admin_feature_offload_desc_TX_L3_csum_ipv4(offload),
		(int)get_ena_admin_feature_offload_desc_TX_L4_ipv4_csum_part(offload),
		(int)get_ena_admin_feature_offload_desc_TX_L4_ipv4_csum_full(offload),
		(int)get_ena_admin_feature_offload_desc_TX_L4_ipv6_csum_part(offload),
		(int)get_ena_admin_feature_offload_desc_TX_L4_ipv6_csum_full(offload),
		(int)get_ena_admin_feature_offload_desc_tso_ipv4(offload),
		(int)get_ena_admin_feature_offload_desc_tso_ipv6(offload),
		offload->rx_supported, offload->rx_enabled);

	device->txBurstLeftMin = 0xffff;
	device->txChecksumOffload = 0;

	/* Only the *partial* form is any use to us, and it is also the only form
	   this device offers (tx 0x3 on Graviton3: ipv4 l3 csum and ipv4 l4 csum
	   part, with full absent). Partial means the packet arrives with the folded
	   pseudo-header sum already in the checksum field and the device folds the
	   rest in -- which is precisely the contract
	   NET_BUFFER_L4_CHECKSUM_NEEDED describes, and it is what lets the stack
	   skip its pass over the payload.

	   The "full" variant is deliberately not used even if a device offers it:
	   the two are mutually exclusive in the descriptor (l4_csum_partial), the
	   stack would have to be told which convention to write, and computing a
	   pseudo-header sum costs a handful of adds either way. One code path.

	   IPv6 is claimed only if the device claims it; this one does not. */
	bool partialIPv4
		= get_ena_admin_feature_offload_desc_TX_L4_ipv4_csum_part(offload) != 0;
	bool partialIPv6
		= get_ena_admin_feature_offload_desc_TX_L4_ipv6_csum_part(offload) != 0;

	bool enabled = true;
	void* handle = load_driver_settings("ena");
	if (handle != NULL) {
		enabled = get_driver_boolean_parameter(handle, "tx_checksum_offload",
			true, true);
		const char* value = get_driver_parameter(handle,
			"tx_extra_doorbells", NULL, NULL);
		if (value != NULL) {
			int extra = atoi(value);
			if (extra > 0 && extra <= ENA_MAX_EXTRA_DOORBELLS)
				device->txExtraDoorbells = extra;
			TRACE_ALWAYS("settings: tx_extra_doorbells %" B_PRId32 " (asked "
				"for %s)\n", device->txExtraDoorbells, value);
		}
		unload_driver_settings(handle);
	}

	/* LLQ only. In device placement the frame's headers are pushed into the
	   device's own memory window, so the descriptor's header_length already
	   means "bytes pushed" and covering the layer-4 header is free. Host
	   placement needs header_length set to the layer-4 offset plus header size
	   instead -- a different rule, on a path this hardware never takes, and
	   untested code in a checksum path is worse than no code. */
	if (device->comDev.tx_mem_queue_type != ENA_ADMIN_PLACEMENT_POLICY_DEV) {
		TRACE_ALWAYS("transmit checksum offload off: host placement\n");
		return;
	}
	if (!enabled) {
		TRACE_ALWAYS("transmit checksum offload off by driver settings\n");
		return;
	}

	if (partialIPv4)
		device->txChecksumOffload |= NET_DEVICE_TX_CHECKSUM_IPV4_L4;
	if (partialIPv6)
		device->txChecksumOffload |= NET_DEVICE_TX_CHECKSUM_IPV6_L4;

	TRACE_ALWAYS("transmit checksum offload: advertising %#" B_PRIx32 "\n",
		device->txChecksumOffload);
}


/*!	Asks the device to finish this frame's layer-4 checksum.

	Returns true if the request was programmed into \a context, false if the
	frame is not one this device can finish -- in which case the caller must not
	transmit it, because the protocol above has already declined to compute the
	checksum and nothing else will.

	Everything is read out of \a frame, the contiguous bounce copy the caller has
	just made, rather than out of the net_buffer: the headers are then a pointer
	dereference away instead of a walk across a node chain.

	The validation is deliberately stricter than the negotiation. The stack only
	sets NET_BUFFER_L4_CHECKSUM_NEEDED for TCP over a family this device claimed,
	so every rejection here is a bug somewhere above -- which is exactly why it is
	checked rather than assumed, and why the caller counts and drops instead of
	putting a frame with a half-computed checksum on the wire.
*/
static bool
ena_prepare_tx_checksum(ena_haiku_device* device,
	struct ena_com_tx_ctx* context, const uint8* frame, size_t frameLength,
	size_t pushedHeaderLength)
{
	/* Big-endian 16-bit read from an arbitrary offset. Byte-wise so that it
	   does not depend on unaligned loads being legal. */
	#define ENA_READ_BE16(p) \
		((uint16)(((uint16)((p)[0]) << 8) | (uint16)((p)[1])))

	if (frameLength < ETHER_HEADER_LENGTH + 20)
		return false;

	if (ENA_READ_BE16(frame + 12) != ETHER_TYPE_IP)
		return false;
	if ((device->txChecksumOffload & NET_DEVICE_TX_CHECKSUM_IPV4_L4) == 0)
		return false;

	const uint8* ip = frame + ETHER_HEADER_LENGTH;
	if ((ip[0] >> 4) != 4)
		return false;
	const uint16 ipHeaderLength = (uint16)((ip[0] & 0x0f) * 4);
	if (ipHeaderLength < 20)
		return false;

	/* A fragment carries only part of the layer-4 payload, and the sum has to
	   cover all of it, so neither the offset nor the more-fragments bit may be
	   set. The remaining bit of that field is DF, which the descriptor wants
	   told separately. */
	const uint16 fragmentField = ENA_READ_BE16(ip + 6);
	if ((fragmentField & 0x3fff) != 0)
		return false;

	const uint8 protocol = ip[9];
	if (protocol != IPPROTO_TCP && protocol != IPPROTO_UDP)
		return false;

	const size_t l4Offset = ETHER_HEADER_LENGTH + ipHeaderLength;
	uint16 l4HeaderLength;
	if (protocol == IPPROTO_TCP) {
		if (frameLength < l4Offset + 20)
			return false;
		l4HeaderLength = (uint16)((frame[l4Offset + 12] >> 4) * 4);
		if (l4HeaderLength < 20)
			return false;
	} else
		l4HeaderLength = 8;

	if (frameLength < l4Offset + l4HeaderLength)
		return false;

	/* The device modifies the checksum field, so the whole layer-4 header has
	   to be inside what was pushed into its memory window. With a 224 byte push
	   limit and headers that cannot exceed 14 + 60 + 60 this never bites; it is
	   here so that it cannot start to. */
	if (pushedHeaderLength < l4Offset + l4HeaderLength)
		return false;

	context->meta_valid = 1;
	context->l3_proto = ENA_ETH_IO_L3_PROTO_IPV4;
	context->l4_proto = protocol == IPPROTO_TCP
		? ENA_ETH_IO_L4_PROTO_TCP : ENA_ETH_IO_L4_PROTO_UDP;
	context->l4_csum_enable = 1;
	context->l4_csum_partial = 1;
	context->df = (fragmentField & 0x4000) != 0 ? 1 : 0;

	/* mss is for TSO, which is not enabled; the header geometry is what the
	   device needs to find the field. l4_hdr_len is in 32-bit words, matching
	   the TCP data-offset field it came from. The meta descriptor is cached by
	   ena-com and only re-emitted when one of these changes, so a steady stream
	   costs no extra descriptor at all. */
	context->ena_meta.mss = 0;
	context->ena_meta.l3_hdr_offset = ETHER_HEADER_LENGTH;
	context->ena_meta.l3_hdr_len = ipHeaderLength;
	context->ena_meta.l4_hdr_len = (uint16)(l4HeaderLength / 4);

	#undef ENA_READ_BE16
	return true;
}


/*!	Works out how deep the descriptor rings may be.

	Newer devices describe their limits through the "queue ext" feature; older
	ones use the original descriptor. Take the smallest relevant limit and
	round down to a power of two, which the rings require.
*/
static void
ena_calculate_ring_sizes(ena_haiku_device* device,
	struct ena_com_dev_get_features_ctx* features)
{
	uint32 maxTx;
	uint32 maxRx;

	if ((device->comDev.supported_features
			& BIT(ENA_ADMIN_MAX_QUEUES_EXT)) != 0) {
		struct ena_admin_queue_ext_feature_fields* fields
			= &features->max_queue_ext.max_queue_ext;
		maxTx = min_c(fields->max_tx_sq_depth, fields->max_tx_cq_depth);
		maxRx = min_c(fields->max_rx_sq_depth, fields->max_rx_cq_depth);
	} else {
		struct ena_admin_queue_feature_desc* fields = &features->max_queues;
		maxTx = min_c(fields->max_sq_depth, fields->max_cq_depth);
		maxRx = maxTx;
	}

	/* In device-placement (LLQ) mode the transmit depth is governed by the LLQ
	   negotiation, not by max_tx_sq_depth -- and the two disagree. Amazon's
	   driver applies these bounds in ena_calc_io_queue_size()
	   (ena_netdev.c:4120-4160), and amzn-drivers#368 records a Nitro v4 device
	   accepting SET_FEATURE(LLQ) with its own recommended 256 byte entry size
	   and then rejecting queue creation with an unqualified UNKNOWN_ERROR
	   because the depth was too large for that entry size. We were sizing the
	   rings purely from max_queue_ext and ignoring all of this. */
	if (device->comDev.tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
		const uint32 llqDepth = features->llq.max_llq_depth;
		if (llqDepth > 0 && llqDepth < maxTx) {
			TRACE_ALWAYS("LLQ caps the transmit depth at %" B_PRIu32
				" (was %" B_PRIu32 ")\n", llqDepth, maxTx);
			maxTx = llqDepth;
		}

		/* The wider 256 byte entries have their own, smaller limit. A device
		   reporting zero means "no separate limit", and the reference halves
		   the depth instead so the memory footprint stays the same. */
		if (device->comDev.llq_info.desc_list_entry_size
				> ENA_LLQ_NARROW_ENTRY_SIZE) {
			const uint32 wide = features->llq.max_wide_llq_depth;
			if (wide == 0) {
				TRACE_ALWAYS("no wide LLQ depth reported; halving the transmit"
					" depth from %" B_PRIu32 "\n", maxTx);
				maxTx /= 2;
			} else if (wide < maxTx) {
				TRACE_ALWAYS("wide LLQ caps the transmit depth at %" B_PRIu32
					" (was %" B_PRIu32 ")\n", wide, maxTx);
				maxTx = wide;
			}
		}
	}

	uint32 txSize = min_c((uint32)ENA_DEFAULT_TX_RING_SIZE, maxTx);
	uint32 rxSize = min_c((uint32)ENA_DEFAULT_RX_RING_SIZE, maxRx);

	/* Round down to a power of two. */
	while ((txSize & (txSize - 1)) != 0)
		txSize &= txSize - 1;
	while ((rxSize & (rxSize - 1)) != 0)
		rxSize &= rxSize - 1;

	device->txRingSize = (uint16)txSize;
	device->rxRingSize = (uint16)rxSize;

	TRACE_ALWAYS("ring sizes: %u tx, %u rx (device limits: %" B_PRIu32 " tx, %"
		B_PRIu32 " rx, queue-ext feature %s)\n", device->txRingSize,
		device->rxRingSize, maxTx, maxRx,
		(device->comDev.supported_features & BIT(ENA_ADMIN_MAX_QUEUES_EXT))
			!= 0 ? "yes" : "no");
}


/*!	Sets the receive refill batch from the current ring size: a fraction of the
	ring (ena_datapath.c:696-698), but never a batch so large that a small ring
	would never reach it.

	Kept as a helper because the ring size is not final at the time
	ena_calculate_frame_limits() first computes it -- ena_setup_io_queues() may
	back the ring off to a smaller depth when the device refuses queue creation,
	and a threshold left sized to the original (larger) ring would exceed the
	backed-off ring entirely, so a non-forced refill could never trigger and the
	receive path would stall. Recomputing it there keeps the two in step.
*/
static void
ena_update_rx_refill_threshold(ena_haiku_device* device)
{
	device->rxRefillThreshold = (uint16)min_c(
		(uint32)device->rxRingSize / ENA_RX_REFILL_DIVISOR,
		(uint32)ENA_RX_REFILL_MAX_THRESHOLD);
	if (device->rxRefillThreshold < 1)
		device->rxRefillThreshold = 1;
}


/*!	Works out how many descriptors a frame may span, and hence the MTU.

	Three separate limits meet here, and the MTU is the smallest of them:

	- what the device says it will accept (\a dev_attr.max_mtu),
	- what the ethernet device layer is prepared to allocate for on receive
	  (ETHER_MAX_JUMBO_MTU; it clamps ETHER_GETFRAMESIZE to it regardless of what
	  we report, so asking for more would only desynchronise the two),
	- and what a chain of ENA_PACKET_BUFFER_SIZE buffers can actually carry,
	  which is what makes this a driver limit rather than a policy.

	The third is the one that has to be got right. The device chooses how to
	split a received frame across the descriptors we posted; if it needs more
	than \a rxMaxDescriptors of them, ena_com_rx_pkt() refuses the whole packet
	with ENA_COM_NO_SPACE and we drop a frame we told the device it could send.
	So the MTU is reduced until the worst case fits, rather than hoping it does.

	The 256 bytes held back from the receive capacity are for \a pkt_offset: we
	advertise ENA_ADMIN_HOST_INFO_RX_OFFSET, so the device may place the payload
	at an offset inside the *first* buffer, and ena_rx_ctx::pkt_offset is a u8 so
	that offset is bounded by 255.

	Must run after ena_calculate_ring_sizes(), which is where rxRingSize comes
	from, and before both SET_FEATURE(MTU) and ena_setup_buffers().
*/
static status_t
ena_calculate_frame_limits(ena_haiku_device* device,
	struct ena_com_dev_get_features_ctx* features)
{
	uint16 maxRxDescriptors;
	uint16 maxTxDescriptors;

	if ((device->comDev.supported_features
			& BIT(ENA_ADMIN_MAX_QUEUES_EXT)) != 0) {
		struct ena_admin_queue_ext_feature_fields* fields
			= &features->max_queue_ext.max_queue_ext;
		maxRxDescriptors = fields->max_per_packet_rx_descs;
		maxTxDescriptors = fields->max_per_packet_tx_descs;
	} else {
		struct ena_admin_queue_feature_desc* fields = &features->max_queues;
		maxRxDescriptors = fields->max_packet_rx_descs;
		maxTxDescriptors = fields->max_packet_tx_descs;
	}

	/* A device that reports nothing here is not saying "no limit", it is a
	   device we cannot make a chaining decision about -- so assume the one
	   descriptor per frame this driver used to be limited to. */
	if (maxRxDescriptors == 0)
		maxRxDescriptors = 1;
	if (maxTxDescriptors == 0)
		maxTxDescriptors = 1;

	/* The transmit figure counts the meta descriptor, which ena_com_prepare_tx()
	   may emit ahead of ours, so one has to be reserved out of it. The reference
	   does the same by sizing its DMA tag at max_tx_sgl_size - 1 (ena.c:514,
	   and see ena_check_and_collapse_mbuf()). */
	if (maxTxDescriptors > 1)
		maxTxDescriptors--;

	device->rxMaxDescriptors = min_c((uint16)ENA_MAX_PACKET_DESCRIPTORS,
		maxRxDescriptors);
	device->txMaxDescriptors = min_c((uint16)ENA_MAX_PACKET_DESCRIPTORS,
		maxTxDescriptors);

	uint32 receiveCapacity = (uint32)device->rxMaxDescriptors
		* ENA_PACKET_BUFFER_SIZE;
	/* Worst-case pkt_offset in the first buffer; see above. */
	receiveCapacity -= min_c(receiveCapacity, (uint32)256);

	uint32 transmitCapacity = (uint32)device->txMaxDescriptors
		* ENA_PACKET_BUFFER_SIZE;

	uint32 capacity = min_c(receiveCapacity, transmitCapacity);
	capacity -= min_c(capacity, (uint32)ETHER_HEADER_LENGTH);

	uint32 mtu = device->maxSupportedMtu;
	if (mtu == 0)
		mtu = ENA_DEFAULT_MTU;
	mtu = min_c(mtu, (uint32)ETHER_MAX_JUMBO_MTU);
	mtu = min_c(mtu, capacity);

	if (mtu < ENA_MIN_MTU) {
		ERROR("cannot reach even a %d byte MTU: device limit %" B_PRIu32
			", chain capacity %" B_PRIu32 " (%u rx, %u tx descriptors of %d "
			"bytes)\n", ENA_MIN_MTU, device->maxSupportedMtu, capacity,
			device->rxMaxDescriptors, device->txMaxDescriptors,
			ENA_PACKET_BUFFER_SIZE);
		return B_NOT_SUPPORTED;
	}

	device->frameSize = mtu;
	device->maxFrameSize = mtu + ETHER_HEADER_LENGTH;

	ena_update_rx_refill_threshold(device);

	TRACE_ALWAYS("MTU %" B_PRIu32 " (device limit %" B_PRIu32 ", stack ceiling "
		"%d, chain capacity %" B_PRIu32 "); a frame may span %u receive and %u "
		"transmit descriptors; receive refill batch %u\n", device->frameSize,
		device->maxSupportedMtu, ETHER_MAX_JUMBO_MTU, capacity,
		device->rxMaxDescriptors, device->txMaxDescriptors,
		device->rxRefillThreshold);

	return B_OK;
}


/*!	Resets the device and tears down the admin queue.

	Order matters: the device keeps writing keep-alive events into the AENQ
	roughly once a second, and its AQ/ACQ/AENQ base registers still point at our
	rings. Freeing those before the reset leaves the device DMAing into memory
	the VM has handed to somebody else -- silent, delayed corruption. Amazon's
	driver resets first for the same reason.
*/
static void
ena_destroy_device(ena_haiku_device* device)
{
	device->running = false;

	ena_com_set_admin_running_state(&device->comDev, false);
	ena_com_dev_reset(&device->comDev, ENA_REGS_RESET_NORMAL);

	/* Not any earlier: the device's host attribute registers point at this page
	   until the reset above. comDev belongs to the driver rather than to the
	   open device, so it survives every init/uninit cycle -- and with it
	   host_attr.host_info, which the next ena_config_host_info() would replace
	   with a fresh page and orphan this one. */
	ena_com_delete_host_info(&device->comDev);

	ena_com_admin_destroy(&device->comDev);
	ena_com_mmio_reg_read_request_destroy(&device->comDev);
}


/*!	Prepares receive-side scaling, host side.

	Split in two on purpose. Everything here can be done before any IO queue
	exists; flushing the indirection table cannot, because
	ena_com_ind_tbl_convert_to_device() translates each host entry through
	io_sq_queues[qid].idx and rejects any queue whose direction is not yet set
	to RX -- which only ena_com_create_io_queue() does. Amazon's drivers make
	the same split: this part runs at probe, ena_flush_rss()'s part runs from
	their ifup path after the queues are created.

	We have one queue pair, so RSS buys us nothing directly; it is configured
	because the reference does so before creating any queue.
*/
static status_t
ena_prepare_rss(ena_haiku_device* device)
{
	struct ena_com_dev* comDev = &device->comDev;

	int result = ena_com_rss_init(comDev, ENA_RSS_TABLE_LOG_SIZE);
	if (result != ENA_COM_OK) {
		ERROR("cannot initialise the RSS indirection table: %d\n", result);
		return ena_translate_error(result);
	}

	for (uint16 i = 0; i < ENA_RSS_TABLE_SIZE; i++) {
		/* One queue pair, so everything lands on our single receive queue. */
		result = ena_com_indirect_table_fill_entry(comDev, i,
			ENA_RX_QUEUE_ID);
		if (result != ENA_COM_OK && result != ENA_COM_UNSUPPORTED) {
			ERROR("cannot fill indirection entry %u: %d\n", i, result);
			goto err;
		}
	}

	/* A NULL key asks ena-com to generate one via ENA_RSS_FILL_KEY. */
	result = ena_com_fill_hash_function(comDev, ENA_ADMIN_TOEPLITZ, NULL,
		ENA_RSS_HASH_KEY_SIZE, 0x0);
	if (result != ENA_COM_OK && result != ENA_COM_UNSUPPORTED) {
		ERROR("cannot fill the hash function: %d\n", result);
		goto err;
	}

	result = ena_com_set_default_hash_ctrl(comDev);
	if (result != ENA_COM_OK && result != ENA_COM_UNSUPPORTED) {
		ERROR("cannot set the default hash control: %d\n", result);
		goto err;
	}

	return B_OK;

err:
	ena_com_rss_destroy(comDev);
	return ena_translate_error(result);
}


/*!	Flushes the RSS configuration to the device.

	Must run after the IO queues exist; see ena_prepare_rss(). Best effort, as
	in the reference: an older device may answer ENA_COM_UNSUPPORTED.
*/
static void
ena_flush_rss(ena_haiku_device* device)
{
	struct ena_com_dev* comDev = &device->comDev;

	int result = ena_com_indirect_table_set(comDev);
	if (result != ENA_COM_OK && result != ENA_COM_UNSUPPORTED)
		ERROR("cannot flush the indirection table: %d\n", result);

	result = ena_com_set_hash_function(comDev);
	if (result != ENA_COM_OK && result != ENA_COM_UNSUPPORTED)
		ERROR("cannot flush the hash function: %d\n", result);

	result = ena_com_set_hash_ctrl(comDev);
	if (result != ENA_COM_OK && result != ENA_COM_UNSUPPORTED)
		ERROR("cannot flush the hash control: %d\n", result);
}


/*!	Decodes every admin command we have submitted, in order.

	The admin submission ring holds q_depth (32) entries and we issue far fewer
	than that before the first queue creation, so on the first lap the ring *is*
	the command history. One line per command, so it can be diffed against the
	same sequence captured from a working driver.
*/
static void
ena_dump_admin_command_stream(ena_haiku_device* device)
{
	struct ena_com_admin_queue* admin = &device->comDev.admin_queue;
	struct ena_admin_aq_entry* entries = admin->sq.entries;
	if (entries == NULL || admin->q_depth == 0)
		return;

	const uint16 total = admin->sq.tail;
	const uint16 submitted = min_c((uint32)total, (uint32)admin->q_depth);

	/* Once the ring has wrapped, slot order is no longer command order, so walk
	   back from the tail rather than from slot zero. */
	const uint16 first = (uint16)(total - submitted);

	TRACE_ALWAYS("--- admin command stream: %u commands (tail %u, depth %u) "
		"---\n", submitted, total, admin->q_depth);

	for (uint16 n = 0; n < submitted; n++) {
		const uint16 i = (uint16)((first + n) & (admin->q_depth - 1));
		uint8* raw = (uint8*)&entries[i];
		const uint8 opcode = raw[2];

		/* Always print the bytes next to whatever we claim they mean. Two of
		   our own measurements once disagreed about cq_caps_1 and msix_vector
		   while agreeing about the fields between them, so no parsed value from
		   here is trustworthy unless the raw bytes back it up. */
		TRACE_ALWAYS("  [%02u] slot %02u raw %02x %02x %02x %02x | %02x %02x "
			"%02x %02x | %02x %02x %02x %02x | %02x %02x %02x %02x\n",
			first + n, i, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
			raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12], raw[13],
			raw[14], raw[15]);

		switch (opcode) {
			case ENA_ADMIN_GET_FEATURE:
			case ENA_ADMIN_SET_FEATURE:
				/* feat_common follows aq_common (4) + control_buffer (12). */
				TRACE_ALWAYS("       %s feature_id %u version %u\n",
					opcode == ENA_ADMIN_GET_FEATURE ? "GET_FEATURE"
						: "SET_FEATURE", raw[17], raw[18]);
				break;

			case ENA_ADMIN_CREATE_CQ:
				TRACE_ALWAYS("       CREATE_CQ interrupt_mode %d "
					"entry_size_words %u depth %u msix_vector %u\n",
					(raw[4] & 0x20) != 0 ? 1 : 0, raw[5] & 0x1f,
					raw[6] | (raw[7] << 8), raw[8] | (raw[9] << 8));
				break;

			case ENA_ADMIN_CREATE_SQ:
				TRACE_ALWAYS("       CREATE_SQ direction/caps %#02x %#02x "
					"depth %u cq_idx %u\n", raw[4], raw[5],
					raw[8] | (raw[9] << 8), raw[6] | (raw[7] << 8));
				break;

			case ENA_ADMIN_DESTROY_CQ:
			case ENA_ADMIN_DESTROY_SQ:
				TRACE_ALWAYS("       %s\n",
					opcode == ENA_ADMIN_DESTROY_CQ ? "DESTROY_CQ"
						: "DESTROY_SQ");
				break;

			case ENA_ADMIN_GET_STATS:
				TRACE_ALWAYS("       GET_STATS type %u scope %u\n",
					raw[16], raw[17]);
				break;

			default:
				TRACE_ALWAYS("       opcode %u unrecognised\n", opcode);
				break;
		}
	}

	TRACE_ALWAYS("--- end admin command stream ---\n");
}


/*!	Creates one queue pair at the given depth. */
static int
ena_create_queue_pair(ena_haiku_device* device, uint16 txDepth,
	uint16 rxDepth, uint32 msixVector)
{
	struct ena_com_dev* comDev = &device->comDev;
	struct ena_com_create_io_ctx context;

	/* TODO: one pair only. Scaling out means a vector and a ring per pair,
	   plus RSS configuration to spread receive across them. */
	memset(&context, 0, sizeof(context));
	context.direction = ENA_COM_IO_QUEUE_DIRECTION_RX;
	context.qid = ENA_RX_QUEUE_ID;
	context.mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
	context.msix_vector = msixVector;
	context.queue_size = rxDepth;
	context.numa_node = 0;

	int result = ena_com_create_io_queue(comDev, &context);
	if (result != ENA_COM_OK) {
		ERROR("cannot create the receive queue at depth %u: %d\n", rxDepth,
			result);
		return result;
	}

	result = ena_com_get_io_handlers(comDev, ENA_RX_QUEUE_ID,
		&device->rxSubmissionQueue, &device->rxCompletionQueue);
	if (result != ENA_COM_OK) {
		ERROR("cannot get receive queue handlers: %d\n", result);
		ena_com_destroy_io_queue(comDev, ENA_RX_QUEUE_ID);
		return result;
	}

	memset(&context, 0, sizeof(context));
	context.direction = ENA_COM_IO_QUEUE_DIRECTION_TX;
	context.qid = ENA_TX_QUEUE_ID;
	context.mem_queue_type = comDev->tx_mem_queue_type;
	context.msix_vector = msixVector;
	context.queue_size = txDepth;
	context.numa_node = 0;

	TRACE_ALWAYS("creating queue pair: tx depth %u, rx depth %u, msix vector %"
		B_PRIu32
		", tx placement %s, tx max header %" B_PRIu32 "\n", txDepth, rxDepth,
		msixVector,
		comDev->tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_HOST
			? "host" : "device", comDev->tx_max_header_size);
	TRACE_ALWAYS("cdesc sizes: tx %" B_PRIuSIZE " bytes (%" B_PRIuSIZE
		" words), rx %" B_PRIuSIZE " bytes (%" B_PRIuSIZE " words)\n",
		sizeof(struct ena_eth_io_tx_cdesc),
		sizeof(struct ena_eth_io_tx_cdesc) / 4,
		sizeof(struct ena_eth_io_rx_cdesc_base),
		sizeof(struct ena_eth_io_rx_cdesc_base) / 4);

	result = ena_com_create_io_queue(comDev, &context);
	if (result != ENA_COM_OK) {
		ERROR("cannot create the transmit queue at depth %u: %d\n", txDepth,
			result);
		ena_com_destroy_io_queue(comDev, ENA_RX_QUEUE_ID);
		return result;
	}

	result = ena_com_get_io_handlers(comDev, ENA_TX_QUEUE_ID,
		&device->txSubmissionQueue, &device->txCompletionQueue);
	if (result != ENA_COM_OK) {
		ERROR("cannot get transmit queue handlers: %d\n", result);
		ena_com_destroy_io_queue(comDev, ENA_TX_QUEUE_ID);
		ena_com_destroy_io_queue(comDev, ENA_RX_QUEUE_ID);
		return result;
	}

	return ENA_COM_OK;
}


/*!	Creates the single TX/RX queue pair, halving the ring depth and retrying
	when the device refuses creation.

	A device may accept every earlier admin command and then reject queue
	creation because the requested depth is too large for the negotiated
	configuration -- LLQ entry size, instance type, or transient resource
	pressure. The reference drivers handle this by backing the ring size off
	rather than failing the whole attach: ena_netdev.c's
	create_queues_with_size_backoff() halves the depth on failure and retries
	down to ENA_MIN_RING_SIZE. This mirrors that.

	The backoff is self-contained. ena_create_queue_pair() destroys whatever it
	created on any non-OK return, so no io queue survives a failed attempt to
	leak into the next one; the descriptor rings are owned by ena-com and freed
	with the queue. The packet-buffer pools (ena_setup_buffers()) are sized from
	the ring size afterwards, so they pick up whatever depth this settles on, and
	the receive refill batch is re-derived here for the same reason.

	The reduced depth is not sticky across a device reset: ena_init_device()
	re-runs ena_calculate_ring_sizes(), which restores the requested size before
	this is reached, so a reset always starts from the full depth and backs off
	again only if the device still refuses.
*/
static status_t
ena_setup_io_queues(ena_haiku_device* device)
{
	while (true) {
		int result = ena_create_queue_pair(device, device->txRingSize,
			device->rxRingSize, ENA_IO_VECTOR_IDX);
		if (result == ENA_COM_OK) {
			device->ioVector = ENA_IO_VECTOR_IDX;
			return B_OK;
		}

		ena_dump_admin_command_stream(device);

		/* Give up once neither ring can shrink any further: at the minimum
		   depth a smaller ring is not an option, so the failure is real. */
		uint16 currentTx = device->txRingSize;
		uint16 currentRx = device->rxRingSize;
		if (currentTx <= ENA_MIN_RING_SIZE && currentRx <= ENA_MIN_RING_SIZE) {
			ERROR("queue creation still failing at the minimum ring size "
				"(%u tx, %u rx): %d\n", currentTx, currentRx, result);
			return ena_translate_error(result);
		}

		/* Halve each ring that is still above the floor, clamped to it. */
		uint16 newTx = currentTx > ENA_MIN_RING_SIZE
			? (uint16)(currentTx / 2) : currentTx;
		uint16 newRx = currentRx > ENA_MIN_RING_SIZE
			? (uint16)(currentRx / 2) : currentRx;
		if (newTx < ENA_MIN_RING_SIZE)
			newTx = ENA_MIN_RING_SIZE;
		if (newRx < ENA_MIN_RING_SIZE)
			newRx = ENA_MIN_RING_SIZE;

		TRACE_ALWAYS("queue creation failed (%d) at %u tx / %u rx; backing off "
			"to %u tx / %u rx\n", result, currentTx, currentRx, newTx, newRx);

		device->txRingSize = newTx;
		device->rxRingSize = newRx;

		/* The refill batch is a fraction of the ring; keep it in step with the
		   depth we just chose so a small ring still refills. */
		ena_update_rx_refill_threshold(device);
	}
}


/*!	Destroys the queue pair and forgets its handlers.

	Shared with the ena_init_device() unwind, so it has to tolerate a pair that
	was never created, and it clears the handlers so that a second call is a no
	operation.
*/
static void
ena_release_io_queues(ena_haiku_device* device)
{
	if (device->rxSubmissionQueue != NULL)
		ena_com_destroy_io_queue(&device->comDev, ENA_RX_QUEUE_ID);
	if (device->txSubmissionQueue != NULL)
		ena_com_destroy_io_queue(&device->comDev, ENA_TX_QUEUE_ID);
	device->rxSubmissionQueue = NULL;
	device->rxCompletionQueue = NULL;
	device->txSubmissionQueue = NULL;
	device->txCompletionQueue = NULL;
}


/*!	Allocates one contiguous DMA area of \a count packet slots.

	Carving a single contiguous area means each slot's physical address is a
	fixed offset from the first, so they are resolved once here rather than on
	every refill.
*/
static status_t
ena_allocate_buffer_area(ena_haiku_device* device, const char* name,
	uint16 count, area_id* _area, addr_t* _base, phys_addr_t* _physicalBase)
{
	size_t areaSize = ROUNDUP((size_t)count * ENA_PACKET_BUFFER_SIZE,
		B_PAGE_SIZE);

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.alignment = B_PAGE_SIZE;
	if (device->dmaWidth < (sizeof(phys_addr_t) * 8))
		physicalRestrictions.high_address = (phys_addr_t)1 << device->dmaWidth;

	void* base = NULL;
	area_id area = create_area_etc(B_SYSTEM_TEAM, name, areaSize, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &virtualRestrictions,
		&physicalRestrictions, &base);
	if (area < B_OK)
		return area;

	physical_entry entry;
	status_t status = get_memory_map(base, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK) {
		delete_area(area);
		return status;
	}

	*_area = area;
	*_base = (addr_t)base;
	*_physicalBase = entry.address;

	return B_OK;
}


/*!	Allocates the bounce buffers and the transmit request-id pool. */
static status_t
ena_setup_buffers(ena_haiku_device* device)
{
	device->rxBuffers = (ena_packet_buffer*)calloc(device->rxRingSize,
		sizeof(ena_packet_buffer));
	device->txBuffers = (ena_tx_buffer*)calloc(device->txRingSize,
		sizeof(ena_tx_buffer));
	device->txFreeIds = (uint16*)calloc(device->txRingSize, sizeof(uint16));
	if (device->rxBuffers == NULL || device->txBuffers == NULL
			|| device->txFreeIds == NULL) {
		return B_NO_MEMORY;
	}

	addr_t base;
	phys_addr_t physicalBase;
	status_t status = ena_allocate_buffer_area(device, "ena rx buffers",
		device->rxRingSize, &device->rxBufferArea, &base, &physicalBase);
	if (status != B_OK)
		return status;

	for (uint16 i = 0; i < device->rxRingSize; i++) {
		device->rxBuffers[i].data
			= (void*)(base + (size_t)i * ENA_PACKET_BUFFER_SIZE);
		device->rxBuffers[i].physicalAddress
			= physicalBase + (phys_addr_t)i * ENA_PACKET_BUFFER_SIZE;
		device->rxBuffers[i].index = i;
	}

	status = ena_allocate_buffer_area(device, "ena tx buffers",
		device->txRingSize, &device->txBufferArea, &base, &physicalBase);
	if (status != B_OK)
		return status;

	for (uint16 i = 0; i < device->txRingSize; i++) {
		device->txBuffers[i].slot.data
			= (void*)(base + (size_t)i * ENA_PACKET_BUFFER_SIZE);
		device->txBuffers[i].slot.physicalAddress
			= physicalBase + (phys_addr_t)i * ENA_PACKET_BUFFER_SIZE;
		device->txBuffers[i].slot.index = i;

		/* Every transmit request id starts out available. */
		device->txFreeIds[i] = i;
	}
	device->txFreeCount = device->txRingSize;

	return B_OK;
}


/*!	Releases everything ena_setup_buffers() allocated.

	Shared with the ena_init_device() unwind, so it has to cope with an
	allocation that only got part way. Every field is reset as well as released:
	this runs once per close, not once per lifetime, and the next open starts
	from these values.
*/
static void
ena_release_buffers(ena_haiku_device* device)
{
	/* The net_buffers of packets that were still in flight. Each one is owned by
	   the driver from ena_send() until ena_reclaim_transmitted() sees its
	   completion, and a reset destroys the queues underneath them: the
	   completions are never delivered, so nothing else will ever free these.
	   Leaking them is unbounded in the number of resets, and on an instance whose
	   NIC is wedging repeatedly that is the failure mode that turns a recoverable
	   NIC into an out-of-memory kernel. The reference driver frees them in
	   ena_free_tx_bufs() for the same reason, and warns once per ring because an
	   uncompleted packet at teardown is worth knowing about.

	   Runs before the areas go away, since the slot each entry bounced through
	   lives in txBufferArea. Callers already hold txLock (the reset path) or have
	   destroyed it along with the rest of the device (ena_uninit_device()), so
	   this deliberately takes no lock of its own. */
	if (device->txBuffers != NULL) {
		uint16 outstanding = 0;
		for (uint16 i = 0; i < device->txRingSize; i++) {
			if (device->txBuffers[i].buffer == NULL)
				continue;

			sBufferModule->free(device->txBuffers[i].buffer);
			device->txBuffers[i].buffer = NULL;
			device->txBuffers[i].descriptors = 0;
			device->txBuffers[i].segments = 0;
			outstanding++;
		}

		if (outstanding > 0) {
			TRACE_ALWAYS("released %u uncompleted transmit buffer(s)\n",
				outstanding);
		}
	}

	if (device->rxBufferArea >= 0) {
		delete_area(device->rxBufferArea);
		device->rxBufferArea = -1;
	}
	if (device->txBufferArea >= 0) {
		delete_area(device->txBufferArea);
		device->txBufferArea = -1;
	}
	free(device->rxBuffers);
	free(device->txBuffers);
	free(device->txFreeIds);
	device->rxBuffers = NULL;
	device->txBuffers = NULL;
	device->txFreeIds = NULL;
	device->txFreeCount = 0;
	device->rxNextToFill = 0;
	/* Both halves of the receive ring's position, together: a surviving
	   rxPendingRefill would make the next refill post descriptors against a
	   ring that no longer exists in the form it was counted for. */
	device->rxPendingRefill = 0;
}


/*!	Posts up to \a count receive descriptors and rings the doorbell once.

	Returns how many were actually posted, which can be fewer than asked for --
	the submission queue may be full, and the caller has to know that so the
	remainder stays owed rather than being forgotten. One doorbell write covers
	the whole batch, which is the entire reason batching exists; the reference
	driver rings it in exactly the same place, once, at the end
	(ena.c:1163-1164).

	Must be called with rxLock held.
*/
static uint16
ena_post_receive_descriptors(ena_haiku_device* device, uint16 count)
{
	uint16 posted = 0;

	for (uint16 i = 0; i < count; i++) {
		/* Free entries minus one: ENA treats a completely full submission
		   queue as an error rather than as full. */
		if (ena_com_free_q_entries(device->rxSubmissionQueue) < 1)
			break;

		ena_packet_buffer* buffer = &device->rxBuffers[
			(device->rxNextToFill + i) % device->rxRingSize];

		struct ena_com_buf comBuffer;
		comBuffer.paddr = buffer->physicalAddress;
		comBuffer.len = ENA_PACKET_BUFFER_SIZE;

		int result = ena_com_add_single_rx_desc(device->rxSubmissionQueue,
			&comBuffer, buffer->index);
		if (result != ENA_COM_OK)
			break;

		posted++;
	}

	if (posted > 0) {
		device->rxNextToFill = (device->rxNextToFill + posted)
			% device->rxRingSize;
		ena_com_write_sq_doorbell(device->rxSubmissionQueue);
	}

	return posted;
}


/*!	Gives \a count drained descriptors back to the device, in batches.

	This is the normal datapath return path, and it deliberately does *not* post
	immediately: each post costs a doorbell write, and at a jumbo MTU a single
	frame drains up to five descriptors, so posting per frame would be five
	register writes per packet. Descriptors are accumulated instead and flushed
	when a batch's worth has built up, or when \a force says the caller cannot
	afford to leave any owed.

	Holding descriptors back is safe against starving the ring -- see the note on
	ENA_RX_REFILL_DIVISOR in ena.h -- but it does depend on the count surviving
	only as long as the ring it describes. Every place that reallocates the
	receive buffers resets rxPendingRefill along with rxNextToFill.

	Must be called with rxLock held.
*/
static void
ena_return_receive_descriptors(ena_haiku_device* device, uint16 count,
	bool force = false)
{
	device->rxPendingRefill += count;

	/* The ring cannot owe more descriptors than it has. Both callers that can
	   pass a device-derived count -- the descriptor-count and stranded-count
	   paths in ena_receive() -- take it from a completion descriptor or from a
	   u16 subtraction of two device-advanced counters, so neither is trustworthy
	   on its own. An inflated count could not actually make
	   ena_post_receive_descriptors() hand the device a descriptor it already
	   owns, because ena_com_free_q_entries() bounds that independently, but it
	   would leave a permanent fictional debt that defeats the batching for the
	   life of the ring. */
	if (device->rxPendingRefill > device->rxRingSize) {
		ERROR("receive refill debt of %u exceeds the %u entry ring; clamping\n",
			device->rxPendingRefill, device->rxRingSize);
		device->rxPendingRefill = device->rxRingSize;
	}

	if (!force && device->rxPendingRefill < device->rxRefillThreshold)
		return;

	/* Only what was actually posted is no longer owed. A partial post leaves
	   the rest pending, and the next frame's return -- or the next forced
	   flush -- picks it up. */
	uint16 posted = ena_post_receive_descriptors(device,
		device->rxPendingRefill);
	device->rxPendingRefill -= posted;
}


/*!	Fills the receive ring from empty, after the buffers have been (re)created.

	Used by the two paths that own the whole ring: the first open, and the end of
	a reset. Anything left over stays owed, exactly as during normal operation,
	rather than being silently dropped.

	Must be called with rxLock held.
*/
static void
ena_refill_receive_ring(ena_haiku_device* device)
{
	device->rxNextToFill = 0;
	device->rxPendingRefill = 0;

	/* One short of the ring, not the whole ring: ena_com_free_q_entries() is
	   q_depth - 1 - outstanding, so the last entry can never be posted and
	   asking for it would report a phantom shortfall on every open. The
	   reference asks for ring_size - 1 for the same reason (ena.c:1483). */
	const uint16 wanted = (uint16)(device->rxRingSize - 1);
	ena_return_receive_descriptors(device, wanted, true);

	if (device->rxPendingRefill != 0) {
		TRACE_ALWAYS("receive ring only partly filled: %u of %u descriptors "
			"still owed\n", device->rxPendingRefill, wanted);
	}
}


//	#pragma mark - device module API



//	#pragma mark - device watchdog


/* Defined below, next to ena_init_device(), because that is where the sequence it
   performs belongs conceptually. The watchdog reset is its second caller. */
static status_t	ena_device_bringup(ena_haiku_device* device);

/* Defined with the transmit path; the missing-TX-completion check reclaims
   completed descriptors before scanning for outstanding ones. */
static void	ena_reclaim_transmitted(ena_haiku_device* device);


/*!	Tears the device down and brings it back, in the one order that is safe.

	Ordering here is not stylistic. Three things in particular:

	- \a ena_com_dev_reset() runs **before** the descriptor rings are freed. After
	  ena_com_set_admin_running_state(false), ena_com_submit_admin_cmd() returns
	  immediately, so the DESTROY_SQ/DESTROY_CQ commands inside
	  ena_com_destroy_io_queue() never reach the device -- while the free happens
	  regardless. Freeing first would hand rings back to the VM while the device
	  still holds their addresses and has not been told to stop, which is silent,
	  delayed corruption of whatever gets that memory next. FreeBSD resets in
	  ena_down() before destroying queues, for exactly this reason.

	- The frees happen with \a txLock and \a rxLock held. The resetting flag alone
	  cannot close the window: a receiver already inside ena_com_rx_pkt() when the
	  rings go away dereferences a null page, and the net stack's reader retries
	  every 10 ms, so there is no window to be lucky in.

	- Bring-up is ena_device_bringup(), the same function attach uses, so the
	  command sequence cannot drift between the two paths.
*/
static status_t
ena_watchdog_reset(ena_haiku_device* device,
	enum ena_regs_reset_reason_types reason)
{
	MutexLocker resetLocker(device->resetLock);

	/* Losing a race with teardown is not a failure; it means there is nothing
	   left worth resetting. */
	if (device->watchdogExiting || device->deviceDead)
		return B_CANCELED;

	const bigtime_t startedAt = system_time();
	atomic_add(&device->resetCount, 1);
	TRACE_ALWAYS("resetting the device, reason %d (reset #%" B_PRId32 ")\n",
		(int)reason, device->resetCount);

	/* Publish the flag before anything is dismantled, so a datapath call that
	   takes txLock/rxLock after this point sees it and returns instead of
	   touching a queue that is about to disappear. */
	device->resetting = true;

#ifdef ENA_DEBUG_FAULT_INJECTION
	/* Mode 1 is "one timeout", and this is the point at which that timeout has
	   happened, so it is the right place to clear it. Doing it in the keep-alive
	   handler instead -- which is what the first version did -- cleared the flag
	   on the next event and so never produced a timeout at all. */
	if (device->suppressKeepAlive == 1) {
		device->suppressKeepAlive = 0;
		TRACE_ALWAYS("fault injection: single-timeout request satisfied, "
			"suppression cleared\n");
	}
#endif

	/* Tell the stack the link is gone. This is the single most consequential
	   line here: ethernet_link_checker() polls ETHER_GET_LINK_STATE every second
	   and a cleared IFF_LINK makes AutoconfigLooper delete the DHCP client and
	   renegotiate on the way back up. That is measured rather than designed
	   around -- see docs/watchdog-design.md, criterion B. */
	device->linkUp = false;

	/* Wake everyone who is blocked, with a count: release_sem() wakes exactly
	   one waiter, and there can be several. They re-check under the locks. */
	if (device->rxReady >= 0)
		release_sem_etc(device->rxReady, 8, B_DO_NOT_RESCHEDULE);
	if (device->txCompleted >= 0)
		release_sem_etc(device->txCompleted, 8, B_DO_NOT_RESCHEDULE);

	/* --- teardown ------------------------------------------------------- */

	/* First, and before the rings are touched: stop the device dead. This is a
	   register write, not an admin command, so it works regardless of admin
	   state. */
	int result = ena_com_dev_reset(&device->comDev, reason);
	if (result != ENA_COM_OK) {
		/* Worth logging loudly but not worth aborting: the rest of the teardown
		   is still the best available way back to a known state. */
		ERROR("device reset register write failed: %d\n", result);
	}

	ena_com_set_admin_running_state(&device->comDev, false);

	if (device->ioIrqInstalled) {
		remove_io_interrupt_handler(device->ioIrq, ena_io_interrupt, device);
		device->ioIrqInstalled = false;
	}
	if (device->managementIrqInstalled) {
		remove_io_interrupt_handler(device->managementIrq,
			ena_management_interrupt, device);
		device->managementIrqInstalled = false;
	}

	/* The rings and the bounce buffers, with the datapath excluded. */
	{
		MutexLocker txLocker(device->txLock);
		MutexLocker rxLocker(device->rxLock);

		ena_release_io_queues(device);
		ena_release_buffers(device);
	}

#ifdef ENA_DEBUG_FAULT_INJECTION
	/* Debug only: widen the reset window so a concurrent teardown can be aimed at
	   it.
	   A reset takes 27-84 ms on this hardware, which is far too narrow to hit
	   from a shell -- the torture campaign could not test `ifconfig down` landing
	   *inside* a reset, so resetLock and the `resetting` flag guarding against a
	   concurrent ena_uninit_device() were verified by code structure only, which
	   is the weakest result of the whole campaign (docs/watchdog-design.md
	   section 8).
	   This is deliberately the widest point: the rings and bounce buffers are
	   gone, admin is about to be torn down, and the device is at its least
	   consistent. If the guards are wrong, this is where it shows. */
	int32 holdMs = atomic_get(&device->holdResetMs);
	if (holdMs > 0) {
		TRACE_ALWAYS("fault injection: holding the reset open for %" B_PRId32
			" ms -- teardown may be raced now\n", holdMs);
		snooze((bigtime_t)holdMs * 1000);
		TRACE_ALWAYS("fault injection: reset hold over, continuing\n");
	}
#endif

	ena_com_rss_destroy(&device->comDev);
	ena_com_abort_admin_commands(&device->comDev);
	ena_com_wait_for_abort_completion(&device->comDev);
	ena_com_admin_destroy(&device->comDev);
	ena_com_mmio_reg_read_request_destroy(&device->comDev);

	/* One contiguous page per reset cycle if this is forgotten. comDev outlives
	   the reset, so the bring-up allocates a fresh host_info over the old
	   pointer. */
	ena_com_delete_host_info(&device->comDev);

	if (device->msixConfigured) {
		device->pci->unconfigure_msi(device->pciDevice);
		device->msixConfigured = false;
		device->msixEnabled = false;
	}

	device->running = false;

	/* Zero the interrupt counters so "first management/io interrupt delivered"
	   prints again for the *new* vectors. Without this the reset is
	   indistinguishable in the log from a reset whose IO vector was never armed --
	   which is the failure mode that looks exactly like success. Criterion A in
	   docs/watchdog-design.md depends on this line. */
	atomic_set(&device->managementInterrupts, 0);
	atomic_set(&device->ioInterrupts, 0);

	/* --- bring-up ------------------------------------------------------- */

	/* Snapshot the identity the device advertised before this reset, so the
	   post-reset validation below can catch a device that silently comes back
	   describing itself differently -- a changed MAC would strand the stack's
	   ARP/neighbour state and a shrunken max_mtu would leave frameSize describing
	   a frame the device will now reject (docs/watchdog-design.md gap 7). This is
	   diagnostic: it logs, it does not fail the reset, because there is nothing
	   better to fall back to than the device the hardware now presents. */
	uint8 previousMac[ETHER_ADDRESS_LENGTH];
	memcpy(previousMac, device->macAddress, ETHER_ADDRESS_LENGTH);
	const uint32 previousMaxMtu = device->maxSupportedMtu;
	const uint32 previousFrameSize = device->frameSize;

	status_t status = ena_device_bringup(device);
	if (status != B_OK) {
		/* Leave the device inert rather than half-built, and stop the watchdog
		   from trying again forever. An operator can still see this in the
		   syslog; the alternative is a reset loop that makes the instance worse
		   than the wedged NIC it was trying to fix. */
		ERROR("reset failed to bring the device back: %s -- device is now "
			"inert and the watchdog is disabled\n", strerror(status));
		device->deviceDead = true;
		device->resetting = false;
		return status;
	}

	/* The receive ring belongs to the reset, not to ena_open(): the buffers were
	   freed and reallocated above, so the descriptors have to be reposted. Only
	   if somebody actually has the device open -- otherwise ena_open() will do
	   it, and posting now would race with it. */
	if (atomic_get(&device->openCount) > 0) {
		MutexLocker rxLocker(device->rxLock);
		ena_refill_receive_ring(device);
	}

	device->resetting = false;

	/* Post-reset parameter validation (gap 7). Loud, because a device that comes
	   back with a different identity is a rare and consequential event and there
	   is no other place it becomes visible. */
	if (memcmp(previousMac, device->macAddress, ETHER_ADDRESS_LENGTH) != 0) {
		ERROR("device MAC changed across the reset "
			"(%02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x); "
			"the stack's neighbour state is now stale\n",
			previousMac[0], previousMac[1], previousMac[2], previousMac[3],
			previousMac[4], previousMac[5], device->macAddress[0],
			device->macAddress[1], device->macAddress[2], device->macAddress[3],
			device->macAddress[4], device->macAddress[5]);
	}
	if (device->maxSupportedMtu < previousMaxMtu
		|| device->frameSize != previousFrameSize) {
		ERROR("device MTU changed across the reset (max %" B_PRIu32 " -> %"
			B_PRIu32 ", frame size %" B_PRIu32 " -> %" B_PRIu32 ")\n",
			previousMaxMtu, device->maxSupportedMtu, previousFrameSize,
			device->frameSize);
	}

	/* Optimistic, exactly as at attach: the device does not send a link event
	   for a link that is already up, and on EC2 it always is. */
	device->linkUp = true;

	TRACE_ALWAYS("device reset completed in %" B_PRId64 " ms (reset #%" B_PRId32
		"); interrupts since: %" B_PRId32 " management, %" B_PRId32 " io\n",
		(system_time() - startedAt) / 1000, device->resetCount,
		device->managementInterrupts, device->ioInterrupts);

	return B_OK;
}


/*!	Acts on a reset the device asked for, deferred here from an AENQ handler.

	FATAL_ERROR and DEVICE_REQUEST_RESET are recorded in interrupt context and
	drained here because the reset itself blocks. There is no false-positive
	question to weigh -- the device asked -- so this runs ahead of every
	heuristic check below. Returns true when a reset was performed, so the caller
	skips the rest of the tick rather than re-examining a device it just rebuilt.
*/
static bool
ena_watchdog_check_reset_request(ena_haiku_device* device)
{
	if (atomic_get(&device->resetRequest) == 0)
		return false;

	enum ena_regs_reset_reason_types reason
		= (enum ena_regs_reset_reason_types)atomic_get(&device->resetRequestReason);

	/* Cleared before acting, not after: the reset blocks for tens of
	   milliseconds, and a fresh request arriving in that window must set the flag
	   again and be handled on the next tick rather than being swallowed by a
	   clear that runs after the reset returns. */
	atomic_set(&device->resetRequest, 0);

	switch (reason) {
		case ENA_REGS_RESET_GENERIC:
			device->fatalErrorResets++;
			break;
		case ENA_REGS_RESET_DEVICE_REQUEST:
			device->deviceRequestResets++;
			break;
		default:
			break;
	}

	TRACE_ALWAYS("acting on a device-requested reset, reason %d\n", (int)reason);
	ena_watchdog_reset(device, reason);
	return true;
}


/*!	Detects a wedged admin queue (docs/watchdog-design.md gap 1).

	The HAL clears running_state only after an admin command has actually failed
	or timed out (ena_com.c) -- never speculatively -- so unlike the keep-alive
	deadline there is no marginal-threshold question here: a cleared state is a
	command that already did not complete. This is one of the three wedges the
	keep-alive check cannot see, because the device keeps emitting keep-alives
	while its admin path is dead. Returns true when a reset was performed.
*/
static bool
ena_watchdog_check_admin_state(ena_haiku_device* device)
{
	if (ena_com_get_admin_running_state(&device->comDev))
		return false;

	ERROR("admin queue wedged: the HAL cleared running_state after a command "
		"timeout, so every later admin command would fail silently; resetting\n");
	device->adminWedgeResets++;
	ena_watchdog_reset(device, ENA_REGS_RESET_ADMIN_TO);
	return true;
}


/*!	The original keep-alive liveness check, unchanged but now one of several.

	\a moving is whether the datapath advanced in the interval just ended.
	Returns true when a reset was performed. A device that asked (via a hardware
	hint) not to be keep-alive-watchdogged is skipped here but still covered by
	the admin, fatal and transmit checks -- honouring the hint can only reduce
	resets, which is the conservative direction.
*/
static bool
ena_watchdog_check_keep_alive(ena_haiku_device* device, bool moving)
{
	if (device->hwHintNoKeepAliveTimeout) {
		device->keepAliveMisses = 0;
		return false;
	}

	const bigtime_t last = atomic_get64(&device->lastKeepAlive);
	const bigtime_t age = system_time() - last;
	if (age <= ENA_KEEP_ALIVE_TIMEOUT_US) {
		if (device->keepAliveMisses != 0) {
			/* Logged, because this is the line that says a reset was
			   correctly *not* performed. Without it the fix is invisible
			   when it works, and a fix that is invisible when it works is
			   indistinguishable from a broken watchdog. */
			TRACE_ALWAYS("keep-alive recovered after %" B_PRIu32 " missed "
				"deadline(s); no reset\n", device->keepAliveMisses);
			device->keepAliveMisses = 0;
		}
		return false;
	}

	/* If a keep-alive is sitting unconsumed in the AENQ then the device is
	   alive and it is our interrupt that went missing. Distinguishing the
	   two costs nothing and is the difference between blaming the device and
	   blaming ourselves. */
	enum ena_regs_reset_reason_types reason = ENA_REGS_RESET_KEEP_ALIVE_TO;
	if (ena_com_aenq_has_keep_alive(&device->comDev))
		reason = ENA_REGS_RESET_MISSING_ADMIN_INTERRUPT;

	device->keepAliveMisses++;

	/* How many misses this device has to accumulate before it is reset. A
	   device that is still moving frames has demonstrably not stopped, so a
	   late keep-alive earns more patience -- but a bounded amount, because a
	   device that moves frames while its management path is dead is exactly
	   the partial wedge the watchdog exists to catch. See
	   ENA_KEEP_ALIVE_MISSES_WITH_TRAFFIC. */
	const uint32 required = moving
		? ENA_KEEP_ALIVE_MISSES_WITH_TRAFFIC
		: ENA_KEEP_ALIVE_MISSES_BEFORE_RESET;

	/* One missed deadline is not evidence of a dead device -- measured, see
	   ENA_KEEP_ALIVE_MISSES_BEFORE_RESET. Say so and look again next tick;
	   the run has to continue for the device to be reset. */
	if (device->keepAliveMisses < required) {
		TRACE_ALWAYS("keep-alive deadline missed (%" B_PRId64 " ms since the "
			"last event, limit %d ms, reason %s); miss %" B_PRIu32 " of %"
			B_PRIu32 "%s, not resetting yet\n", age / 1000,
			ENA_KEEP_ALIVE_TIMEOUT_US / 1000,
			reason == ENA_REGS_RESET_MISSING_ADMIN_INTERRUPT
				? "missing admin interrupt" : "keep-alive timeout",
			device->keepAliveMisses, required,
			moving ? " (datapath still moving)" : " (datapath idle)");
		return false;
	}

	ERROR("keep-alive watchdog timeout: %" B_PRId64 " ms since the last "
		"event (limit %d ms), reason %s, after %" B_PRIu32 " consecutive "
		"missed deadlines%s\n", age / 1000,
		ENA_KEEP_ALIVE_TIMEOUT_US / 1000,
		reason == ENA_REGS_RESET_MISSING_ADMIN_INTERRUPT
			? "missing admin interrupt" : "keep-alive timeout",
		device->keepAliveMisses,
		moving ? " (datapath still moving -- resetting anyway)" : "");

	device->keepAliveMisses = 0;
	ena_watchdog_reset(device, reason);
	return true;
}


/*!	Detects a wedged transmit path (docs/watchdog-design.md gap 4).

	One lost completion leaves a frame outstanding forever, and the keep-alive
	check cannot see it: the device keeps emitting keep-alives while transmit is
	dead. So the oldest still-outstanding descriptor is timed here instead.

	Deliberately conservative on two independent axes, because the cost of a false
	positive on a console-less instance is a reset of a healthy NIC (§7):

	- The lock is a *trylock*. If the transmit path is holding txLock the datapath
	  is active -- which is itself evidence of progress -- so the tick is skipped.
	  A genuinely wedged transmit releases the lock (ena_send() drops it while
	  blocked on txCompleted), so the wedge is exactly the case the trylock wins.
	- A run of ENA_MISSING_TX_CHECKS_BEFORE_RESET consecutive ticks over the
	  deadline is required, mirroring the keep-alive consecutive-miss guard; a
	  transient backlog clears on the next tick and never reaches the reset.

	The per-frame deadline and the count both take the device's hardware hint when
	one has been received, and fall back to compile-time defaults that are
	OWED-TUNING against real Graviton transmit backpressure. Returns true when a
	reset was performed.
*/
static bool
ena_watchdog_check_missing_tx(ena_haiku_device* device)
{
	bigtime_t timeoutUs = ENA_MISSING_TX_COMPLETION_TIMEOUT_US;
	uint32 threshold = ENA_MISSING_TX_COMPLETION_THRESHOLD;
	if (device->hwHintsReceived) {
		if (device->hwHintTxCompletionTimeoutMs != 0)
			timeoutUs = (bigtime_t)device->hwHintTxCompletionTimeoutMs * 1000;
		if (device->hwHintTxCompletionThreshold != 0)
			threshold = device->hwHintTxCompletionThreshold;
	}

	if (mutex_trylock(&device->txLock) != B_OK)
		return false;

	/* Reclaim first, then scan. On a receive-heavy or idle-transmit path nothing
	   calls ena_send(), so a frame the device has *already completed* keeps its
	   buffer set until the next transmit reclaims it (ena_reclaim_transmitted()
	   runs only from ena_send() -- see the ownership note in ena_io_interrupt()).
	   Without this, a completed-but-unreclaimed frame would read as outstanding
	   and, past the deadline, reset a perfectly healthy device -- precisely the
	   false positive §7 warns against. Doing the reclaim here also frees those
	   net_buffers within a second on an otherwise-idle transmit path, which is a
	   small correctness win in its own right. Safe under txLock: the only other
	   caller of a reset is this same (single) watchdog thread, and ena_send() is
	   excluded by the lock. */
	ena_reclaim_transmitted(device);

	uint32 overdue = 0;
	bigtime_t oldest = 0;
	const bigtime_t now = system_time();
	if (device->txBuffers != NULL) {
		for (uint16 i = 0; i < device->txRingSize; i++) {
			ena_tx_buffer* entry = &device->txBuffers[i];
			if (entry->buffer == NULL)
				continue;
			const bigtime_t outstanding = now - entry->submittedAt;
			if (outstanding > timeoutUs) {
				overdue++;
				if (outstanding > oldest)
					oldest = outstanding;
			}
		}
	}
	mutex_unlock(&device->txLock);

	if (overdue < threshold) {
		if (device->missingTxChecks != 0) {
			TRACE_ALWAYS("transmit completions recovered after %" B_PRIu32
				" check(s) over the deadline; no reset\n",
				device->missingTxChecks);
			device->missingTxChecks = 0;
		}
		return false;
	}

	device->missingTxChecks++;
	if (device->missingTxChecks < ENA_MISSING_TX_CHECKS_BEFORE_RESET) {
		TRACE_ALWAYS("missing transmit completions: %" B_PRIu32 " descriptor(s) "
			"over %" B_PRId64 " ms (oldest %" B_PRId64 " ms); check %" B_PRIu32
			" of %d, not resetting yet\n", overdue, timeoutUs / 1000,
			oldest / 1000, device->missingTxChecks,
			ENA_MISSING_TX_CHECKS_BEFORE_RESET);
		return false;
	}

	ERROR("transmit path wedged: %" B_PRIu32 " descriptor(s) over %" B_PRId64
		" ms (oldest %" B_PRId64 " ms) across %" B_PRIu32 " consecutive checks; "
		"resetting\n", overdue, timeoutUs / 1000, oldest / 1000,
		device->missingTxChecks);
	device->missingTxChecks = 0;
	device->missingTxResets++;
	ena_watchdog_reset(device, ENA_REGS_RESET_MISS_TX_CMPL);
	return true;
}


/*!	Flags a suspected receive stall (docs/watchdog-design.md gap 5).

	DETECT-AND-LOG ONLY. A refill deadlock leaves descriptors owed to the device
	(rxPendingRefill > 0) making no progress. The trap is that "descriptors owed
	and not shrinking, with no receive progress" is *also* what a perfectly idle
	link looks like -- the ring is armed and simply waiting -- so counting on that
	alone over-fires the moment a booted instance goes quiet (hardware-confirmed,
	#211). The discriminator is the device's own rx-drop counter: a genuine
	deadlock strands the free-buffer pool, so inbound frames are dropped and
	hwRxDrops climbs, whereas an idle link drops nothing because nothing arrives.
	So a tick is only counted when drops are still climbing -- evidence the device
	has traffic it cannot place, not merely that the ring is quiet. Even then this
	only counts and logs: the reset threshold is OWED until it can be measured on
	hardware, and an over-eager receive reset on a console-less instance is the
	failure §7 exists to prevent.

	hwRxDrops is refreshed once per keep-alive, and the keep-alive check runs
	first and short-circuits the tick when the device has gone quiet, so this code
	only sees a freshly reported drop count. rxPendingRefill and hwRxDrops are read
	without rxLock, on the same license as watchdogLastTraffic: the question is
	only "did this change", and a torn read costs at most one tick. \a moving
	covers the whole datapath, so a stall while transmit is still busy is not
	flagged -- a known limitation of the log-only heuristic, acceptable because it
	never triggers an action.
*/
static void
ena_watchdog_check_rx_stall(ena_haiku_device* device, bool moving)
{
	const uint16 pending = device->rxPendingRefill;
	const uint64 drops = device->hwRxDrops;
	const uint64 dropDelta = drops > device->rxStallLastDrops
		? drops - device->rxStallLastDrops : 0;

	/* Any of these ends the run: nothing owed, the datapath advanced, the owed
	   count is shrinking (refill is working), or -- the key idle guard -- the
	   device is not dropping inbound frames, meaning either nothing is arriving
	   (idle) or everything arriving is being placed (healthy). Only when frames
	   are being dropped while our descriptors stay owed is this a suspected
	   deadlock rather than a quiet link. */
	if (pending == 0 || moving || pending < device->rxStallLastPending
		|| dropDelta == 0) {
		device->rxStallChecks = 0;
		device->rxStallLastPending = pending;
		device->rxStallLastDrops = drops;
		return;
	}

	device->rxStallLastPending = pending;
	device->rxStallLastDrops = drops;
	device->rxStallChecks++;
	if (device->rxStallChecks == ENA_RX_STALL_CHECKS_BEFORE_LOG) {
		device->rxStallDetections++;
		ERROR("suspected receive stall: %u descriptor(s) owed to the device, "
			"%" B_PRIu64 " inbound frame(s) dropped this interval, and no "
			"receive progress for %" B_PRIu32 " checks -- NOT resetting "
			"(gap 5 is detect-only; the reset threshold is owed hardware "
			"tuning)\n", pending, dropDelta, device->rxStallChecks);
	}
}


/*!	The watchdog. One second of sleep, the checks, and a reset if one is due.

	A thread rather than add_timer(), because add_timer() fires in interrupt
	context and a reset issues admin commands and blocks. This thread *is* the
	deferred context FreeBSD gets from its taskqueue.

	The checks run in priority order: a reset the device asked for, then a wedged
	admin queue, then keep-alive liveness, then a wedged transmit path, then a
	suspected receive stall. The first four can reset and short-circuit the tick;
	the last only logs. The three that are not keep-alive exist because the
	keep-alive check is blind to them by construction -- the device keeps sending
	keep-alives while a specific path of it is dead (docs/watchdog-design.md §9).
*/
static int32
ena_watchdog(void* arg)
{
	ena_haiku_device* device = (ena_haiku_device*)arg;

	while (true) {
		/* A semaphore rather than snooze(), so teardown wakes us immediately
		   instead of waiting out the remaining second. */
		acquire_sem_etc(device->watchdogWake, 1, B_RELATIVE_TIMEOUT,
			ENA_WATCHDOG_INTERVAL_US);

		if (device->watchdogExiting)
			break;

		/* Do not even look before the device is running, or while a reset is in
		   flight. Double entry is prevented here, by construction, rather than
		   by a flag test at the trigger. */
		if (!device->watchdogActive || !device->running || device->resetting
			|| device->deviceDead) {
			/* Not being watched, so any run of misses recorded before this is
			   over: carrying one across a down/up or a reset would let two
			   unrelated samples add up to a reset. Every consecutive-run counter
			   is cleared here for the same reason. */
			device->keepAliveMisses = 0;
			device->missingTxChecks = 0;
			device->rxStallChecks = 0;
			continue;
		}

		/* Device-authoritative reset triggers first: no heuristic to second-guess. */
		if (ena_watchdog_check_reset_request(device))
			continue;

		/* A wedged admin queue the keep-alive check cannot see. */
		if (ena_watchdog_check_admin_state(device))
			continue;

		/* Did the datapath move at all since the last check? Sampled every tick,
		   before the age test, so it describes the interval just ended whether or
		   not a deadline was missed during it. Shared by the keep-alive and
		   receive-stall checks. */
		const uint64 traffic = device->rxFrames + device->txFrames;
		const bool moving = traffic != device->watchdogLastTraffic;
		device->watchdogLastTraffic = traffic;

		if (ena_watchdog_check_keep_alive(device, moving))
			continue;

		/* Datapath-progress checks the keep-alive liveness test is blind to. */
		if (ena_watchdog_check_missing_tx(device))
			continue;

		ena_watchdog_check_rx_stall(device, moving);
	}

	return 0;
}


static void
ena_watchdog_start(ena_haiku_device* device)
{
	if (!device->watchdogActive) {
		TRACE_ALWAYS("watchdog not started: the device did not grant "
			"keep-alive events\n");
		return;
	}

	device->watchdogExiting = false;
	atomic_set64(&device->lastKeepAlive, system_time());

	device->watchdogWake = create_sem(0, "ena watchdog");
	if (device->watchdogWake < B_OK) {
		ERROR("cannot create the watchdog semaphore: %s\n",
			strerror(device->watchdogWake));
		return;
	}

	/* Above normal: on a busy system a watchdog that is scheduled late measures
	   scheduler latency rather than device health. */
	device->watchdogThread = spawn_kernel_thread(ena_watchdog, "ena watchdog",
		B_URGENT_DISPLAY_PRIORITY, device);
	if (device->watchdogThread < B_OK) {
		ERROR("cannot spawn the watchdog thread: %s\n",
			strerror(device->watchdogThread));
		delete_sem(device->watchdogWake);
		device->watchdogWake = -1;
		return;
	}

	resume_thread(device->watchdogThread);
	TRACE_ALWAYS("watchdog running: %d ms keep-alive timeout, %d ms cadence\n",
		ENA_KEEP_ALIVE_TIMEOUT_US / 1000, ENA_WATCHDOG_INTERVAL_US / 1000);
}


static void
ena_watchdog_stop(ena_haiku_device* device)
{
	if (device->watchdogThread < B_OK)
		return;

	/* Ask, wake, then wait. The thread tests watchdogExiting after taking
	   resetLock, so a reset already in flight finishes and the join is bounded
	   by the admin command timeouts rather than being open-ended. */
	device->watchdogExiting = true;
	if (device->watchdogWake >= B_OK)
		release_sem(device->watchdogWake);

	status_t exitValue;
	wait_for_thread(device->watchdogThread, &exitValue);
	device->watchdogThread = -1;

	if (device->watchdogWake >= B_OK) {
		delete_sem(device->watchdogWake);
		device->watchdogWake = -1;
	}
}


/*!	Brings the device itself up: everything from the admin queue to the buffers.

	Called by both \a ena_init_device() at attach and by the watchdog reset. It
	exists as one function on purpose. The order it performs -- host attributes,
	device attributes, LLQ placement, ring sizes, MSI-X, interrupt handlers,
	interrupt moderation, RSS, queue pairs, RSS flush, and MTU *last* -- is a
	conjunction the device validates as a whole, and finding it cost a week (see
	FINDINGS.md). Two copies of that sequence would drift, and the drift would
	present as CREATE_CQ failing again months later for no visible reason.

	Deliberately does **not** touch anything per-open or per-lifetime: not the
	mutexes, not the semaphores, not openCount, not the BAR mappings, and not
	linkUp. Those belong to the caller. A reset re-runs this and nothing else.
*/
static status_t
ena_device_bringup(ena_haiku_device* device)
{
	struct ena_com_dev_get_features_ctx features;
	memset(&features, 0, sizeof(features));

	status_t status;

	status = ena_device_init(device, &features);
	if (status != B_OK)
		return status;

	memcpy(device->macAddress, features.dev_attr.mac_addr,
		ETHER_ADDRESS_LENGTH);
	device->maxSupportedMtu = features.dev_attr.max_mtu;

	TRACE_ALWAYS("MAC %02x:%02x:%02x:%02x:%02x:%02x, device MTU limit %"
		B_PRIu32 "\n",
		device->macAddress[0], device->macAddress[1], device->macAddress[2],
		device->macAddress[3], device->macAddress[4], device->macAddress[5],
		device->maxSupportedMtu);

	/* Read once and kept, because the receive path needs it per frame and
	   because it had never been looked at: the descriptor was fetched at
	   bring-up and discarded. Logged for the same reason -- on c7g the device
	   reports rx_supported 0x7 and rx_enabled 0x0, i.e. it can verify the IPv4
	   header checksum and both L4 checksums and is doing none of them, which is
	   not a thing anyone could have known from this driver's output. */
	device->offloadRxSupported = features.offload.rx_supported;
	device->offloadRxEnabled = features.offload.rx_enabled;

	TRACE_ALWAYS("offloads: tx %#" B_PRIx32 ", rx supported %#" B_PRIx32
		", rx enabled %#" B_PRIx32 "\n", features.offload.tx,
		features.offload.rx_supported, features.offload.rx_enabled);

	ena_configure_placement_policy(device, &features.llq);
	ena_report_offload_capabilities(device, &features);
	ena_calculate_ring_sizes(device, &features);
	if (device->txRingSize < ENA_MIN_RING_SIZE
		|| device->rxRingSize < ENA_MIN_RING_SIZE) {
		ERROR("device offers unusably short rings (%u tx, %u rx)\n",
			device->txRingSize, device->rxRingSize);
		status = B_NOT_SUPPORTED;
		return status;
	}

	/* After the ring sizes, because the receive refill batch is a fraction of
	   the ring; before SET_FEATURE(MTU) below, which is the whole point. */
	status = ena_calculate_frame_limits(device, &features);
	if (status != B_OK)
		return status;

	status = ena_enable_msix(device);
	if (status != B_OK)
		return status;

	status = install_io_interrupt_handler(device->managementIrq,
		ena_management_interrupt, device, 0);
	if (status != B_OK) {
		ERROR("cannot install the management interrupt handler: %s\n",
			strerror(status));
		return status;
	}
	device->managementIrqInstalled = true;

	status = install_io_interrupt_handler(device->ioIrq, ena_io_interrupt,
		device, 0);
	if (status != B_OK) {
		ERROR("cannot install the io interrupt handler: %s\n",
			strerror(status));
		return status;
	}
	device->ioIrqInstalled = true;

	/* The reference drivers initialise interrupt moderation here, while still
	   polling, and the device is evidently particular about how much of its
	   configuration exists before queues are created. */
	if (ena_com_init_interrupt_moderation(&device->comDev) != ENA_COM_OK)
		TRACE_ALWAYS("interrupt moderation unavailable; continuing\n");

	/* Probe the moderation feature the same way the offload and header-length
	   caps are probed -- from what the device advertises, not from an assumption.
	   Adaptive moderation (#108) is gated on this: the interval it programs is in
	   device ticks, and the tick length is only known when the device reported the
	   moderation feature and hence a delay resolution. */
	device->moderationSupported
		= ena_com_interrupt_moderation_supported(&device->comDev);
	TRACE_ALWAYS("interrupt moderation feature %s\n",
		device->moderationSupported ? "advertised" : "not advertised");

	/* Logged because the moderation intervals are written into the register as
	   raw ticks and this is the only thing that says what a tick is worth. It had
	   been inferred from an interrupt-rate ceiling and never read; an inference
	   with a good fit is still not a measurement, and the whole moderation sweep
	   is denominated in this number. */
	TRACE_ALWAYS("interrupt delay resolution %u; rx interval %" B_PRId32
		", tx interval %d\n", device->comDev.intr_delay_resolution,
		atomic_get(&device->rxIrqInterval), ENA_TX_IRQ_INTERVAL);

	/* Interrupts are live now, so the admin queue no longer has to be
	   polled and asynchronous events can start arriving. */
	ena_com_set_admin_polling_mode(&device->comDev, false);

	/* Belt and braces, and deliberately not a substitute for working
	   interrupts: with auto-polling the HAL notices an admin completion that
	   arrived without an interrupt and finishes the command by polling instead
	   of failing it. Admin commands only happen during setup and
	   reconfiguration, so the cost is irrelevant, and it keeps a
	   misconfigured or undelivered management vector from making the device
	   unusable outright. The datapath still depends on real interrupts. */
	ena_com_set_admin_auto_polling_mode(&device->comDev, true);

	device->running = true;
	ena_com_admin_aenq_enable(&device->comDev);

	status = ena_prepare_rss(device);
	if (status != B_OK)
		return status;

	status = ena_setup_io_queues(device);
	if (status != B_OK)
		return status;

	/* Only now that the queues exist can the indirection table be translated
	   into device queue indices. */
	ena_flush_rss(device);

	/* MTU last, which is where Amazon's drivers put it -- their SET_FEATURE(MTU)
	   is the final admin command, after all four queue pairs exist, where ours
	   used to be the command immediately before CREATE_CQ. The maintainers'
	   answer on amzn/amzn-drivers#381 was that CREATE_CQ can be refused when
	   host features provided earlier via SET_FEATURE are incompatible with the
	   instance type, and this was the last SET_FEATURE whose position or value
	   still differed from the reference.

	   Only the *position* is load-bearing, and it is what stays. The value is
	   still frameSize rather than a constant, and it still has to agree with
	   what the receive path can accept -- telling the device a larger MTU than
	   we can reassemble leaves a window where a peer that ignores our advertised
	   MTU puts a frame on the wire that the device accepts and we drop. What has
	   changed is that frameSize is no longer pinned to 1500: the receive path
	   reassembles a frame from a chain of descriptors now, and
	   ena_calculate_frame_limits() derives frameSize from the device's own
	   max_mtu bounded by how much that chain can carry, so the two agree by
	   construction rather than by both being 1500. */
	{
		const uint32 deviceMtu = device->frameSize;
		int mtuResult = ena_com_set_dev_mtu(&device->comDev, deviceMtu);
		TRACE_ALWAYS("set device MTU %" B_PRIu32 " after queue creation "
			"(matching what we report to the stack): %s\n", deviceMtu,
			mtuResult == ENA_COM_OK ? "ok" : "FAILED");
		if (mtuResult != ENA_COM_OK) {
			status = ena_translate_error(mtuResult);
			return status;
		}
	}

	/* After the queues, because the device may have forced a smaller depth
	   than we asked for and the buffer pools are sized from it. */
	status = ena_setup_buffers(device);
	if (status != B_OK) {
		ERROR("cannot allocate packet buffers: %s\n", strerror(status));
		return status;
	}

	/* Arm the io vector. A completion queue created by ena_com_create_io_queue()
	   starts masked, and the re-arm now happens at the end of a drain -- which
	   nothing will reach until an interrupt has woken a reader. Without this first
	   unmask the first interrupt never comes and therefore neither does the
	   re-arm. The interface would then look perfectly healthy in every log line
	   and never receive another frame, which is the hardest possible failure to
	   spot in a reset.

	   Forced, because irqArmed describes the vector belonging to the queue that
	   has just been destroyed and recreated: whatever it says, this one is
	   masked. */
	ena_rearm_io_interrupt(device, true);

	/* Seed the watchdog deadline: the device has not sent a keep-alive yet, and
	   a zero (or stale) timestamp here means the watchdog would fire six seconds
	   from now against a device that is perfectly healthy. */
	atomic_set64(&device->lastKeepAlive, system_time());

	return B_OK;
}

static status_t
ena_init_device(void* _info, void** _cookie)
{
	CALLED();
	ena_haiku_device* device = (ena_haiku_device*)_info;

	/* First thing in the log, so every boot is attributable to a build. See the
	   comment on ENA_BUILD_TAG in ena.h for why this is not decoration. The
	   revision comes from get_haiku_revision() -- the live value stamped into
	   the running system (a DeBeOS-native debeos-r<n>-g<sha> for source builds,
	   or the pinned hrev of a bake), not the frozen ENA_HAIKU_REVISION constant,
	   which is now used only for the numeric device telemetry field (#201). */
	TRACE_ALWAYS("driver build %s, compiled %s (%s)\n", ENA_BUILD_TAG,
		ENA_BUILD_STAMP, get_haiku_revision());

	device_node* parent = sDeviceManager->get_parent_node(device->node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->pci,
		(void**)&device->pciDevice);
	device->pci->get_pci_info(device->pciDevice, &device->pciInfo);
	sDeviceManager->put_node(parent);

	ena_init_aenq_handlers();

	/* Until the device tells us how wide its DMA addressing is, assume the
	   narrowest width ENA is allowed to have. ena_dma_alloc() bounds
	   allocations by this field, and the admin-queue setup below allocates
	   before we can ask. Guessing high here would risk handing the device an
	   address it cannot reach, which corrupts unrelated memory rather than
	   failing. */
	device->dmaWidth = 32;

	/* Enable memory space and bus mastering. */
	uint16 command = device->pci->read_pci_config(device->pciDevice,
		PCI_command, 2);
	command |= PCI_command_master | PCI_command_memory;
	device->pci->write_pci_config(device->pciDevice, PCI_command, 2, command);

	status_t status = ena_map_bar(device, ENA_REGISTER_BAR, "ena registers",
		&device->registerArea, &device->bus.reg_bar, &device->bus.reg_bar_size);
	if (status != B_OK) {
		ERROR("cannot map the register BAR: %s\n", strerror(status));
		return status;
	}

	/* ena-com uses reg_bar as a base that it adds to every register offset
	   before handing the result to ENA_REG_READ32/WRITE32. Our accessors
	   already add the mapped BAR address, so this base has to stay zero --
	   exactly as Amazon's own bus_space based driver leaves it. */
	device->comDev.reg_bar = NULL;

	/* Map the Low Latency Queue push window. Unlike reg_bar, ena-com
	   dereferences mem_bar directly to push transmit headers into device
	   memory, so this one really is the mapped virtual address.

	   This is not optional in practice. Amazon's drivers do fall back to
	   host-memory placement when this BAR is absent, but nothing exercises
	   that path on current hardware -- every real driver maps the BAR and
	   configures LLQ -- and a device that has never been told its placement
	   policy rejects queue creation outright. */
	phys_addr_t memoryPhysical = 0;
	device->comDev.tx_mem_queue_type = ENA_ADMIN_PLACEMENT_POLICY_HOST;
	if (ena_map_bar(device, ENA_MEMORY_BAR, "ena llq window",
			&device->memoryArea, &device->bus.mem_bar,
			&device->bus.mem_bar_size, &memoryPhysical) == B_OK) {
		device->comDev.mem_bar = (void*)device->bus.mem_bar;

		/* Write combining matters here: this window is written with a stream
		   of 64-bit stores per packet. Failure is not fatal, only slower. */
		if (vm_set_area_memory_type(device->memoryArea, memoryPhysical,
				B_WRITE_COMBINING_MEMORY) != B_OK) {
			TRACE_ALWAYS("could not enable write combining on the LLQ "
				"window\n");
		}
	} else {
		TRACE_ALWAYS("no LLQ window; falling back to host placement\n");
		device->comDev.mem_bar = NULL;
	}

	device->comDev.bus = &device->bus;
	device->comDev.dmadev = device;
	device->comDev.net_device = device;
	device->comDev.ena_min_poll_delay_us = ENA_MIN_POLL_DELAY_US;

	status = ena_device_bringup(device);
	if (status != B_OK)
		goto err_device;


	mutex_init(&device->txLock, "ena tx");
	mutex_init(&device->rxLock, "ena rx");
	device->rxReady = -1;
	device->txCompleted = -1;

	/* ENA reports link state only through asynchronous events, and does not
	   send one for a link that is already up when we attach. On EC2 the link
	   is always up, so start optimistic and let an event correct us. */
	device->linkUp = true;

	mutex_init(&device->resetLock, "ena reset");
	ena_watchdog_start(device);

	TRACE_ALWAYS("attached [build " ENA_BUILD_STAMP "]; interrupts so far: %"
		B_PRId32 " management, %" B_PRId32 " io%s\n",
		device->managementInterrupts, device->ioInterrupts,
		device->managementInterrupts == 0
			? " (management vector never fired -- admin queue is polling)"
			: "");

	*_cookie = device;
	return B_OK;

	/* The device manager does not call UninitDevice for a device whose
	   InitDevice failed, so this is the only chance to give any of it back --
	   and since InitDevice is reference counted per open(), an ifconfig up that
	   fails here will be tried again. It therefore has to undo everything
	   above, in the same order as ena_uninit_device(), and leave every handle
	   at the value ena_init_driver() set so a later teardown is harmless. */
err_device:
	ena_release_io_queues(device);

	if (device->ioIrqInstalled) {
		remove_io_interrupt_handler(device->ioIrq, ena_io_interrupt, device);
		device->ioIrqInstalled = false;
	}
	if (device->managementIrqInstalled) {
		remove_io_interrupt_handler(device->managementIrq,
			ena_management_interrupt, device);
		device->managementIrqInstalled = false;
	}

	ena_com_rss_destroy(&device->comDev);

	/* Before the buffer areas, which the device can still DMA into; see the
	   comment on ena_destroy_device(). */
	ena_destroy_device(device);

	/* Keyed on msixConfigured, not msixEnabled: configure_msix() may have
	   succeeded while enable_msix() failed, and unconfigure_msi() is what
	   releases the vectors in either case. */
	if (device->msixConfigured) {
		device->pci->unconfigure_msi(device->pciDevice);
		device->msixConfigured = false;
		device->msixEnabled = false;
	}

	ena_release_buffers(device);

	/* Reached only by falling through from err_device now: the BAR-mapping
	   failures that used to jump here live inside ena_device_bringup() since the
	   split, and return rather than goto. The unmapping still has to happen, so
	   the code stays and only the label goes. */
	if (device->memoryArea >= 0) {
		delete_area(device->memoryArea);
		device->memoryArea = -1;
	}
	if (device->registerArea >= 0) {
		delete_area(device->registerArea);
		device->registerArea = -1;
	}
	return status;
}


static void
ena_uninit_device(void* _cookie)
{
	CALLED();
	ena_haiku_device* device = (ena_haiku_device*)_cookie;

	/* Before anything is dismantled: the watchdog must not be mid-reset while the
	   device is being torn down under it. Stopping it takes resetLock into
	   account, so a reset in flight completes first. */
	ena_watchdog_stop(device);
	mutex_destroy(&device->resetLock);

	device->running = false;

	/* Remove the interrupt handlers *before* freeing the queues they read, not
	   after (docs/watchdog-design.md gap 9). An io interrupt landing between the
	   free and the removal would run ena_io_interrupt()/ena_management_interrupt()
	   against a torn-down queue -- a latent use-after-free. This is the order both
	   the reference driver and ena_watchdog_reset() in this same file already use;
	   ena_uninit_device() was the one path that had it inverted. */
	if (device->ioIrqInstalled) {
		remove_io_interrupt_handler(device->ioIrq, ena_io_interrupt, device);
		device->ioIrqInstalled = false;
	}
	if (device->managementIrqInstalled) {
		remove_io_interrupt_handler(device->managementIrq,
			ena_management_interrupt, device);
		device->managementIrqInstalled = false;
	}

	ena_release_io_queues(device);

	ena_com_rss_destroy(&device->comDev);

	/* Stop the device before anything it can DMA into goes away. */
	ena_destroy_device(device);

	/* There is no disable_msix(); unconfigure_msi() tries MSI-X first and
	   handles it, which is the only teardown path the bus manager exposes. */
	/* Keyed on msixConfigured, not msixEnabled: configure_msix() may have
	   succeeded while enable_msix() failed, and unconfigure_msi() is what
	   releases the vectors in either case. */
	if (device->msixConfigured) {
		device->pci->unconfigure_msi(device->pciDevice);
		device->msixConfigured = false;
		device->msixEnabled = false;
	}

	mutex_destroy(&device->txLock);
	mutex_destroy(&device->rxLock);

	ena_release_buffers(device);

	if (device->memoryArea >= 0) {
		delete_area(device->memoryArea);
		device->memoryArea = -1;
	}
	if (device->registerArea >= 0) {
		delete_area(device->registerArea);
		device->registerArea = -1;
	}
}


static status_t
ena_open(void* _info, const char* path, int openMode, void** _cookie)
{
	CALLED();
	ena_haiku_device* device = (ena_haiku_device*)_info;

	device->nonBlocking = (openMode & O_NONBLOCK) != 0;

	/* Held across the whole of the first-opener setup below, which is what the
	   reference driver's global sx lock does around ena_up() and
	   ena_reset_task(). Two things need it:

	   - The setup touches the queues (ena_refill_receive_ring() and the unmask)
	     with no lock at all, and the reset path frees them and leaves the
	     pointers NULL (ena_release_io_queues()). An `ifconfig up` landing inside
	     a reset therefore dereferenced a null queue in the kernel. Testing the
	     flags without the lock would not fix it: the window between the test and
	     the refill is exactly the window the reset needs.

	   - It settles who posts the receive descriptors. The reset reposts them only
	     if openCount is already above zero, on the stated assumption that
	     ena_open() will do it otherwise (see ena_watchdog_reset()); that hand-off
	     is only sound if the increment and the refill cannot interleave with the
	     reset's own test, which the lock is what guarantees.

	   Order is resetLock before rxLock, the same direction the reset path takes
	   them, and nothing here sleeps: the refill posts descriptors and returns, so
	   the reset waits at worst the length of this function. */
	MutexLocker resetLocker(device->resetLock);

	/* No reset can be in flight while resetLock is held, so `resetting` is only
	   tested to keep this guard correct if some later path comes to set it
	   elsewhere; `deviceDead` is the real case. A reset that failed to bring the
	   device back leaves it inert with its queues destroyed, and the honest
	   answer to `ifconfig up` then is that the device is not there. */
	if (device->resetting || device->deviceDead)
		return B_DEV_NOT_READY;

	/* Everything below is per-device state, not per-open, so only the first
	   opener may set it up. A second open used to create a fresh pair of
	   semaphores -- leaking the first pair and leaving the earlier opener's
	   ena_close() to delete the newcomer's -- and, worse, reset rxNextToFill to
	   zero while the device still owned the whole ring. The refill then posted
	   nothing (no free entries), so the index pointed at slot 0 while the device
	   held 0..rxRingSize-2, and the next per-packet refill handed the device
	   descriptors it already owned: two descriptors sharing one req_id and one
	   2 KB buffer, which duplicates frames and lets the stack read a buffer
	   while DMA is still writing it. */
	if (atomic_add(&device->openCount, 1) != 0) {
		*_cookie = device;
		return B_OK;
	}

	device->rxReady = create_sem(0, "ena rx ready");
	device->txCompleted = create_sem(0, "ena tx completed");
	if (device->rxReady < B_OK || device->txCompleted < B_OK) {
		delete_sem(device->rxReady);
		delete_sem(device->txCompleted);
		device->rxReady = device->txCompleted = -1;
		atomic_add(&device->openCount, -1);
		return B_NO_MORE_SEMS;
	}

	/* Post every receive descriptor before the first interrupt can arrive. Under
	   rxLock, as the reset path does it: a second opener returns above without
	   waiting for this, so it can already be inside ena_receive() touching the
	   same submission queue. */
	{
		MutexLocker rxLocker(device->rxLock);
		ena_refill_receive_ring(device);
	}

	/* Third and last seeding point for the watchdog deadline (the others are
	   thread start and the end of a reset). The interface can have been down long
	   enough for the timestamp to go stale while nothing was watching it, and a
	   stale timestamp here is a reset six seconds after ifconfig up. */
	atomic_set64(&device->lastKeepAlive, system_time());

	/* Arm the io vector; it starts masked. Forced for the same reason as in the
	   reset path: this queue is newly created, so irqArmed cannot be describing
	   it. */
	ena_rearm_io_interrupt(device, true);

	*_cookie = device;
	return B_OK;
}


static status_t
ena_close(void* cookie)
{
	CALLED();
	ena_haiku_device* device = (ena_haiku_device*)cookie;

	/* Mirror of ena_open(): only the last closer tears the semaphores down, or
	   one process closing would strand every other reader in acquire_sem(). */
	if (atomic_add(&device->openCount, -1) != 1)
		return B_OK;

	sem_id rxReady = device->rxReady;
	sem_id txCompleted = device->txCompleted;
	device->rxReady = device->txCompleted = -1;

	delete_sem(rxReady);
	delete_sem(txCompleted);

	return B_OK;
}


static status_t
ena_free(void* cookie)
{
	return B_OK;
}


//	#pragma mark - transmit and receive


/*!	Reclaims completed transmit descriptors and frees their net_buffers.

	Must be called with txLock held.
*/
static void
ena_reclaim_transmitted(ena_haiku_device* device)
{
	/* Descriptors completed but not yet acknowledged, and how many packets that
	   is. Acknowledging is a plain `next_to_comp += n` inside ena-com with no
	   register write, so batching it in ENA_TX_COMMIT-sized groups is about not
	   touching the submission queue's shared state once per packet rather than
	   about bus traffic. This mirrors ena_tx_cleanup() (ena_datapath.c:299-317),
	   including the remainder flush after the loop -- which every `break` below
	   falls through to, so nothing is left unacknowledged on an error exit. */
	uint16 pendingDescriptors = 0;
	uint16 completed = 0;

	while (true) {
		uint16 requestId = 0;
		if (ena_com_tx_comp_req_id_get(device->txCompletionQueue, &requestId)
				!= ENA_COM_OK) {
			break;
		}

		if (requestId >= device->txRingSize) {
			ERROR("device returned an out-of-range transmit request id %u\n",
				requestId);
			break;
		}

		ena_tx_buffer* entry = &device->txBuffers[requestId];
		if (entry->buffer == NULL) {
			/* Not outstanding. Pushing it back would put the same id on the
			   free stack twice and, repeated, run txFreeCount past the end of
			   the array. This is also what a completion naming one of a
			   multi-slot packet's *secondary* slots looks like, since only the
			   primary carries the net_buffer. */
			ERROR("device completed transmit request id %u that was not in "
				"use\n", requestId);
			break;
		}

		sBufferModule->free(entry->buffer);
		entry->buffer = NULL;

		/* Give back every bounce slot the frame was copied into, not just the
		   one the device echoed. segments is set under txLock in ena_send()
		   together with buffer, so a non-NULL buffer implies a valid chain --
		   but this is the one place where getting it wrong overruns txFreeIds,
		   so it is checked rather than assumed. */
		if (entry->segments == 0
			|| entry->segments > ENA_MAX_PACKET_DESCRIPTORS) {
			ERROR("transmit request id %u has an impossible segment count %u; "
				"reclaiming it alone\n", requestId, entry->segments);
			entry->segments = 1;
			entry->segmentIds[0] = requestId;
		}

		for (uint16 i = 0; i < entry->segments; i++) {
			const uint16 slot = entry->segmentIds[i];
			if (slot >= device->txRingSize) {
				ERROR("transmit request id %u names an out-of-range slot %u; "
					"leaking it rather than corrupting the free stack\n",
					requestId, slot);
				continue;
			}
			device->txFreeIds[device->txFreeCount++] = slot;
		}
		entry->segments = 0;

		/* Whatever ena_com_prepare_tx() reported this packet occupied,
		   including a meta descriptor if it emitted one. */
		pendingDescriptors += entry->descriptors;
		entry->descriptors = 0;

		if (++completed >= ENA_TX_COMMIT) {
			ena_com_comp_ack(device->txSubmissionQueue, pendingDescriptors);
			pendingDescriptors = 0;
			completed = 0;
		}
	}

	if (pendingDescriptors > 0)
		ena_com_comp_ack(device->txSubmissionQueue, pendingDescriptors);

	/* The loop above leaves only when the completion queue reads empty, so this
	   is the transmit side's end-of-drain and the mirror of the re-arm in
	   ena_receive(). It earns its keep on a transmit-heavy workload, where the
	   receive reader can be sitting idle with nothing to wake it; under receive
	   load the receive side gets there first and this is a no-op.

	   The early `break`s above are error paths that may leave the queue
	   non-empty, so re-arming here can cost one spurious interrupt in a case that
	   has already logged a device protocol violation. Not worth a second exit
	   path to avoid. */
	ena_rearm_io_interrupt(device, false);
}


static status_t
ena_send(ena_haiku_device* device, net_buffer* buffer)
{
	MutexLocker locker(device->txLock);

	/* Checked here, under the lock, and not before it. The reset holds txLock
	   across the frees, so anything that gets this far is guaranteed that
	   txBuffers, txFreeIds and the submission queue still exist. A bare flag test
	   outside the lock would be check-then-act and the window is a
	   use-after-free, not a lost packet. */
	if (device->resetting || device->deviceDead)
		return B_DEV_NOT_READY;

	ena_reclaim_transmitted(device);

	/* Worked out before the wait loop, because it is what the loop has to wait
	   for. A frame longer than one bounce slot is copied into a chain of them,
	   one submission-queue descriptor each.

	   The size check is a backstop: ethernet_send_data() already refuses
	   anything above the frame size we reported through ETHER_GETFRAMESIZE. It
	   stays because the descriptor arithmetic below is only bounded if this is,
	   and because a driver that trusts its caller for a bound on a memcpy length
	   is one refactor away from a heap overrun. */
	const size_t size = buffer->size;
	if (size == 0 || size > device->maxFrameSize) {
		ERROR("refusing a %" B_PRIuSIZE " byte frame; the limit is %" B_PRIu32
			"\n", size, device->maxFrameSize);
		return B_BAD_VALUE;
	}

	const uint16 segments = (uint16)((size + ENA_PACKET_BUFFER_SIZE - 1)
		/ ENA_PACKET_BUFFER_SIZE);
	if (segments > device->txMaxDescriptors) {
		/* Unreachable while maxFrameSize is derived from txMaxDescriptors in
		   ena_calculate_frame_limits(); here so that it stays unreachable. */
		ERROR("a %" B_PRIuSIZE " byte frame needs %u descriptors, more than the "
			"%u this device allows\n", size, segments,
			device->txMaxDescriptors);
		return B_BAD_VALUE;
	}

	/* The submission queue runs out before the request-id pool does: it
	   refuses at q_depth - 1 outstanding, and ena_com_prepare_tx() wants room
	   for the packet's descriptors plus a possible meta descriptor -- which is
	   exactly the `num_bufs + 1` it checks for itself (ena_eth_com.c:457).
	   Waiting on txFreeCount alone would never block, and we would drop frames
	   instead. */
	while (device->txFreeCount < segments
			|| !ena_com_sq_have_enough_space(device->txSubmissionQueue,
				(uint16)(segments + 1))) {
		locker.Unlock();

		if (device->nonBlocking)
			return B_WOULD_BLOCK;

		status_t status = acquire_sem(device->txCompleted);
		if (status != B_OK)
			return status;

		locker.Lock();

		/* The lock was dropped while blocked on txCompleted, so a reset may have
		   run and freed the TX ring -- and it is the reset that woke us. Re-check
		   before ena_reclaim_transmitted(), which would touch the freed ring.
		   This mirrors the equivalent guard on the receive path. */
		if (device->resetting || device->deviceDead)
			return B_DEV_NOT_READY;

		ena_reclaim_transmitted(device);
	}

	/* Claim one slot per segment. A slot and a request id are the same
	   resource -- txBuffers is indexed by request id -- so the chain is drawn
	   from the same stack, and the first id claimed becomes the request id the
	   device echoes back. The rest exist only as bounce storage and are
	   deliberately left with a NULL buffer, so ena_reclaim_transmitted() and
	   ena_release_buffers() ignore them; the chain is recorded on the primary
	   entry and released as a whole on completion. */
	uint16 slotIds[ENA_MAX_PACKET_DESCRIPTORS];
	for (uint16 i = 0; i < segments; i++)
		slotIds[i] = device->txFreeIds[--device->txFreeCount];

	const uint16 requestId = slotIds[0];
	ena_tx_buffer* entry = &device->txBuffers[requestId];

	/* Each descriptor's buffer has to be physically contiguous, and a
	   net_buffer's storage is neither contiguous nor addressed by physical
	   address through any interface the buffer module offers -- get_memory_map
	   is a NULL entry in net_buffer_module_info -- so the frame is copied. See
	   the note above ena_receive() for why that copy is still here. */
	struct ena_com_buf comBuffers[ENA_MAX_PACKET_DESCRIPTORS];
	size_t copied = 0;
	for (uint16 i = 0; i < segments; i++) {
		ena_packet_buffer* slot = &device->txBuffers[slotIds[i]].slot;
		const size_t chunk = min_c(size - copied,
			(size_t)ENA_PACKET_BUFFER_SIZE);

		if (sBufferModule->read(buffer, copied, slot->data, chunk) != B_OK) {
			for (uint16 j = 0; j < segments; j++)
				device->txFreeIds[device->txFreeCount++] = slotIds[j];
			return B_BAD_DATA;
		}

		comBuffers[i].paddr = slot->physicalAddress;
		comBuffers[i].len = (uint16)chunk;
		copied += chunk;
	}

	struct ena_com_tx_ctx context;
	memset(&context, 0, sizeof(context));
	context.req_id = requestId;

	/* In LLQ mode the leading bytes of the frame are pushed straight into the
	   device's memory window rather than fetched by DMA, so the descriptors only
	   cover whatever is left over. A short frame can be entirely header, in
	   which case there is no buffer descriptor at all.

	   The header must be one contiguous run, which is why it is taken from the
	   first segment and capped at that segment's length: tx_max_header_size is
	   bounded by the LLQ ring entry size (96 or 224 bytes in practice, and
	   ena-com clamps its own copy to 256), so this cap never actually bites --
	   but it is what makes the pointer arithmetic below true by construction
	   rather than by knowing that number. */
	uint16 headerLength = 0;
	if (device->comDev.tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
		headerLength = (uint16)min_c(
			(size_t)device->txSubmissionQueue->tx_max_header_size,
			(size_t)comBuffers[0].len);
		context.push_header = device->txBuffers[requestId].slot.data;
		context.header_len = headerLength;
	}

	/* Ahead of ena_com_is_doorbell_needed() below, which counts the meta
	   descriptor this may add, and ahead of the descriptor adjustment further
	   down, which moves comBuffers[0] past the pushed header. */
	if ((buffer->buffer_flags & NET_BUFFER_L4_CHECKSUM_NEEDED) != 0) {
		if (ena_prepare_tx_checksum(device, &context,
				(const uint8*)device->txBuffers[requestId].slot.data, size,
				headerLength)) {
			device->txChecksumOffloaded++;
		} else {
			/* The frame's checksum was never computed and this device will not
			   compute it either, so there is nothing to send: on the wire it
			   would be silently discarded by the peer, which is strictly worse
			   than a drop the sender can see. Only reachable if something above
			   asked for offload on a frame the negotiation does not cover, so
			   it is loud. */
			device->txChecksumRejected++;
			if (device->txChecksumRejected <= 8) {
				ERROR("a %" B_PRIuSIZE " byte frame asked for transmit checksum "
					"offload this device cannot do; dropping it (%" B_PRIu64
					" so far)\n", size, device->txChecksumRejected);
			}
			for (uint16 j = 0; j < segments; j++)
				device->txFreeIds[device->txFreeCount++] = slotIds[j];
			return B_NOT_SUPPORTED;
		}
	}

	/* Skip the pushed header in the first descriptor. It can consume that
	   descriptor entirely, which only happens for a single-segment frame that is
	   all header -- a longer frame's first segment is a whole 2048 byte slot and
	   the header is far smaller. */
	uint16 firstBuffer = 0;
	if (headerLength > 0) {
		comBuffers[0].paddr += headerLength;
		comBuffers[0].len = (uint16)(comBuffers[0].len - headerLength);
		if (comBuffers[0].len == 0)
			firstBuffer = 1;
	}

	if (segments > firstBuffer) {
		context.ena_bufs = &comBuffers[firstBuffer];
		context.num_bufs = (uint16)(segments - firstBuffer);
	}

	/* Before ena_com_prepare_tx(), and using this packet's context, exactly as
	   the reference does (ena_datapath.c:1030-1036): the question it answers is
	   "will the descriptors I am about to write still fit in the device's
	   remaining LLQ burst?", so asking after writing them would be too late.

	   It can only ever return true in LLQ mode -- it is `false` outright for host
	   placement -- and while we ring the doorbell for every frame below, a
	   doorbell resets the burst allowance anyway, so today this fires only for a
	   frame that needs more LLQ entries than a whole burst. It is here because it
	   is the piece that has to be in the right place for doorbell coalescing to
	   become a one-line change once the stack can hand us more than one frame per
	   call; see the comment after the doorbell. */
	if (ena_com_is_doorbell_needed(device->txSubmissionQueue, &context))
		ena_com_write_sq_doorbell(device->txSubmissionQueue);

	int descriptors = 0;
	int result = ena_com_prepare_tx(device->txSubmissionQueue, &context,
		&descriptors);
	if (result != ENA_COM_OK) {
		for (uint16 j = 0; j < segments; j++)
			device->txFreeIds[device->txFreeCount++] = slotIds[j];
		ERROR("cannot prepare a transmit descriptor: %d\n", result);
		return ena_translate_error(result);
	}

	entry->buffer = buffer;
	entry->descriptors = (uint16)descriptors;
	entry->segments = segments;
	/* Stamp the submission time so the watchdog can time the oldest outstanding
	   transmit. Set under txLock with buffer/segments, so a non-NULL buffer always
	   carries a valid timestamp; ena_reclaim_transmitted() clears buffer, which is
	   what makes this entry invisible to the missing-completion scan again. */
	entry->submittedAt = system_time();
	for (uint16 i = 0; i < segments; i++)
		entry->segmentIds[i] = slotIds[i];

	/* One doorbell per frame, and -- measured, not assumed -- there is nothing
	   here to amortise on this device.

	   In LLQ placement the device grants a burst of
	   llq_info.max_entries_in_tx_burst ring entries and a doorbell is what
	   refills the allowance. This device grants **two**, and one jumbo frame
	   consumes both: ena_com_is_doorbell_needed() wants
	   1 + ceil((num_bufs - descs_num_before_header) / descs_per_entry) entries,
	   which for a 9 KB frame in five bounce slots is 2. The counters below
	   confirm it on hardware -- 99.94 % of transmit frames leave the allowance
	   at zero -- so a deferred doorbell is forced by the very next frame and the
	   achievable coalescing ratio is 1:1. See
	   graviton/docs/ena-tx-offload.md section 5.

	   Two things anyone reconsidering this needs, because neither is visible from
	   here. First, deferring does not merely risk a frame going unsent: the wait
	   loop above blocks on txCompleted with no timeout, and completions only
	   arrive for descriptors the device has been told about, so a batch that
	   fills the ring with un-rung descriptors waits on itself forever. Any
	   implementation must ring before that wait, not after it. Second, the
	   reference's guard for the several early returns above,
	   ena_com_used_q_entries(), does not exist in the vendored
	   ena_freebsd_2.8.4 HAL and cannot be added to it -- and ena_com_io_sq keeps
	   no record of the last value written to db_addr, so a caller has to track
	   its own. */

	/* Read *before* the doorbell, because the doorbell is what refills the
	   allowance. This is the measurement that decides whether the batched entry
	   point is worth building at all: if one frame leaves zero entries, the very
	   next frame is forced to ring anyway and there is nothing to amortise. */
	const uint16 burstLeft
		= device->txSubmissionQueue->entries_in_tx_burst_left;
	if (burstLeft < device->txBurstLeftMin)
		device->txBurstLeftMin = burstLeft;
	if (burstLeft == 0)
		device->txBurstExhausted++;
	device->txFrames++;
	device->txBytes += size;

	ena_com_write_sq_doorbell(device->txSubmissionQueue);
	device->txDoorbells++;

	/* Debug knob only; zero unless driver settings asked otherwise. Writing the
	   same tail again tells the device nothing new, so this buys nothing and
	   costs exactly one MMIO write each -- which is the point: it prices a
	   doorbell without having to build the batched entry point first. */
	for (int32 i = 0; i < device->txExtraDoorbells; i++) {
		ena_com_write_sq_doorbell(device->txSubmissionQueue);
		device->txDoorbells++;
	}

	if ((device->txFrames % 100000) == 0) {
		TRACE_ALWAYS("tx: %" B_PRIu64 " frames, %" B_PRIu64 " doorbells "
			"(+%" B_PRId32 " forced per frame), burst left min %u, exhausted %"
			B_PRIu64 ", csum offloaded %" B_PRIu64 " rejected %" B_PRIu64
			" (%u entries per burst, %u descs before header, %u descs per "
			"entry; this frame %" B_PRIuSIZE " bytes in %d descriptors)\n",
			device->txFrames, device->txDoorbells, device->txExtraDoorbells,
			device->txBurstLeftMin, device->txBurstExhausted,
			device->txChecksumOffloaded, device->txChecksumRejected,
			device->comDev.llq_info.max_entries_in_tx_burst,
			device->comDev.llq_info.descs_num_before_header,
			device->comDev.llq_info.descs_per_entry,
			size, descriptors);
	}

	return B_OK;
}


/*!	Takes one frame off the receive ring, reassembling it if it was split.

	A frame larger than one ENA_PACKET_BUFFER_SIZE buffer arrives as a chain of
	completion descriptors, and the device -- not the driver -- decides how
	finely to split it: all it is obliged to respect is the length of each
	descriptor we posted. So this walks however many descriptors the completion
	reports and appends each one's bytes to a single net_buffer.

	Everything the chain says about itself comes out of device-written
	descriptors, so all of it is treated as untrusted and the *whole* chain is
	validated before a single byte is copied. Validating as we go would leave a
	half-built net_buffer to unwind on the failure of the last descriptor, and
	the failure mode of getting it wrong is a copy past the end of a buffer
	area, so the two passes are worth their cost.

	The bytes are still copied. Handing the receive buffer itself up the stack
	instead would need net_buffer to be able to take ownership of driver-owned
	memory with a release callback, and net_buffer_module_info has no such
	entry point (its get_memory_map slot is a literal NULL); the alternative,
	allocating a fresh DMA-capable buffer per frame and posting that, replaces a
	copy with an allocation on every packet. Neither is a change to this
	function -- see the report accompanying this work.
*/
static status_t
ena_receive(ena_haiku_device* device, net_buffer** _buffer)
{
	struct ena_com_rx_buf_info bufferInfo[ENA_MAX_PACKET_DESCRIPTORS];
	struct ena_com_rx_ctx context;

	/* Whether this call has already re-armed the vector for the drain it is in,
	   and therefore whether an empty ring means "look again" or "sleep". */
	bool rearmed = false;

	MutexLocker locker(device->rxLock);

	/* Same reasoning as the transmit side: under the lock, because the reset
	   frees the completion queue's descriptor ring and ena_com_rx_pkt() reads it
	   with no NULL check. */
	if (device->resetting || device->deviceDead)
		return B_DEV_NOT_READY;

	while (true) {
		memset(&context, 0, sizeof(context));
		context.ena_bufs = bufferInfo;
		/* Bounded by the size of bufferInfo above, and set at bring-up from the
		   device's own per-packet descriptor limit. ena_com_rx_pkt() enforces it
		   -- a completion naming more descriptors than this is refused with
		   ENA_COM_NO_SPACE before it writes anything into bufferInfo -- so this
		   assignment is what keeps the array from being overrun. */
		context.max_bufs = device->rxMaxDescriptors;

		/* ena_com_rx_pkt() returns 0 on success and reports how many
		   descriptors it consumed in context.descs; every failure is a small
		   positive ENA_COM_* code. Testing the return value for a count means
		   never seeing a packet, and reading ena_bufs on the error path where
		   the HAL never wrote it. */
		int result = ena_com_rx_pkt(device->rxCompletionQueue,
			device->rxSubmissionQueue, &context);
		if (result != ENA_COM_OK) {
			/* Every error return leaves the completion descriptors consumed --
			   ena_com_cdesc_rx_pkt_get() advances io_cq->head before any of the
			   checks that can fail -- but skips the io_sq->next_to_comp update
			   that hands the matching receive buffers back to us, and does not
			   say how many they were: ena_rx_ctx::descs is written on success
			   only. The difference between the two counters is exactly that
			   number, since one receive descriptor produces one completion
			   descriptor, so acknowledge and repost it. Otherwise
			   ena_com_free_q_entries() counts those buffers as in flight for
			   good, and since it is what gates the refill the ring shrinks
			   permanently on every error with nothing to grow it back.

			   NO_SPACE now means the device split a frame more finely than
			   rxMaxDescriptors allows, which is what ena_calculate_frame_limits()
			   sizes the MTU to prevent; it used to be the ordinary consequence of
			   any frame spanning more than one descriptor, because max_bufs was 1.

			   One thing this deliberately does not do, and the reference does:
			   treat the error as a reason to reset the device. FreeBSD maps
			   NO_SPACE to ENA_REGS_RESET_TOO_MANY_RX_DESCS, FAULT to
			   RX_DESCRIPTOR_MALFORMED and anything else to INV_RX_REQ_ID, and
			   resets in every case (ena_datapath.c:612-626). It has a reason to:
			   after a FAULT, ena-com's partial-packet accumulator
			   (io_cq->cur_rx_pkt_cdesc_count and cur_rx_pkt_cdesc_start_idx) is
			   left describing a packet whose completion descriptors we have just
			   acknowledged, so the next call reads from a stale start index.
			   Reclaiming and carrying on is what this driver has shipped with and
			   keeps a recoverable link recoverable; escalating to a reset is a
			   separate decision about which failure is worse on a console-less
			   instance, and is not made here. */
			const uint16 stranded = (uint16)(device->rxCompletionQueue->head
				- device->rxSubmissionQueue->next_to_comp);

			ERROR("receive failed: %d, reclaiming %u descriptor(s)\n", result,
				stranded);

			if (stranded > 0) {
				ena_com_comp_ack(device->rxSubmissionQueue, stranded);
				/* Forced rather than batched: this is error recovery, and the
				   number of descriptors involved may well be under the batch
				   threshold, so waiting for a batch to fill would mean waiting
				   for traffic that the missing descriptors are part of carrying. */
				ena_return_receive_descriptors(device, stranded, true);
			}

			return ena_translate_error(result);
		}

		if (context.descs > 0)
			break;

		/* The ring has read back empty, so this is the end of the drain and the
		   point the reference driver re-arms at. Done while rxLock is still held:
		   a reset takes rxLock before freeing the queues, so holding it is what
		   stops txCompletionQueue being freed underneath the unmask.

		   Then go round once more before sleeping rather than blocking straight
		   away. A completion posted between the read above and the unmask landed
		   while the vector was masked, and whether the device replays it on
		   unmask is the device's business -- not something worth betting a
		   stalled flow on. Re-running the loop closes that window using the same
		   read, with the same error handling: a second copy of the ena_com_rx_pkt()
		   call would have to repeat the stranded-descriptor reclaim below or leak
		   the ring away one error at a time. Anything arriving after the unmask
		   raises an interrupt, and rxReady counts, so nothing is lost either
		   side of it. */
		if (!rearmed) {
			device->rxDrainCycles++;
			/* Choose the interval before the re-arm, so an interval the packet
			   rate has just moved is the one this same unmask programs. */
			ena_adaptive_moderation_sample(device);
			ena_rearm_io_interrupt(device, false);
			rearmed = true;
			continue;
		}
		rearmed = false;

		locker.Unlock();

		if (device->nonBlocking)
			return B_WOULD_BLOCK;

		status_t status = acquire_sem(device->rxReady);
		if (status != B_OK)
			return status;

		/* Collapse the backlog: one interrupt can cover many frames. */
		int32 count = 0;
		if (get_sem_count(device->rxReady, &count) == B_OK && count > 0)
			acquire_sem_etc(device->rxReady, count, B_RELATIVE_TIMEOUT, 0);

		locker.Lock();

		/* The lock was dropped while blocked, so a reset may have run in the
		   meantime -- and it is the reset that woke us. Re-check before going back
		   round into ena_com_rx_pkt(), which is the call that would touch the
		   freed ring. */
		if (device->resetting || device->deviceDead)
			return B_DEV_NOT_READY;
	}

	/* How many descriptors this frame occupied, and therefore how many are owed
	   back to the device however this function goes on to end. ena_com_rx_pkt()
	   has already advanced the submission queue's next_to_comp by exactly this
	   many on the success path, so unlike the error path above there is no
	   ena_com_comp_ack() to pair with the repost. */
	const uint16 descriptors = context.descs;

	/* ena_com_rx_pkt() refuses a completion with more descriptors than
	   max_bufs, so this cannot fire -- but bufferInfo is a fixed stack array
	   being indexed by a count the device chose, and that is not a place to rely
	   on a check made in another file. */
	if (descriptors > ENA_MAX_PACKET_DESCRIPTORS) {
		ERROR("device reported a frame spanning %u descriptors, more than the "
			"%d this driver can hold\n", descriptors,
			ENA_MAX_PACKET_DESCRIPTORS);
		ena_return_receive_descriptors(device, descriptors, true);
		return B_IO_ERROR;
	}

	/* First pass: validate the whole chain, copy nothing.

	   Every field here comes out of a device-written completion descriptor and
	   is untrusted. An oversized length reads past the end of a slot and, for
	   the last slot in the area, past the area itself; an out-of-range request id
	   indexes rxBuffers out of bounds. Only the *first* descriptor carries
	   pkt_offset -- ena_com_rx_pkt() takes it from the first completion
	   descriptor alone (ena_eth_com.c:634) and the reference applies it only to
	   the head segment (ena_datapath.c:456) -- so every later segment starts at
	   the beginning of its buffer.

	   No check on the total: a chain of validated segments is bounded by
	   ENA_MAX_PACKET_DESCRIPTORS * ENA_PACKET_BUFFER_SIZE whatever the device
	   claims, so nothing here depends on it, and rejecting a frame merely for
	   exceeding maxFrameSize would drop a legitimately VLAN-tagged frame four
	   bytes over the MTU we asked for. */
	size_t total = 0;
	for (uint16 i = 0; i < descriptors; i++) {
		if (bufferInfo[i].req_id >= device->rxRingSize) {
			ERROR("device returned an out-of-range receive request id %u in "
				"descriptor %u of %u\n", bufferInfo[i].req_id, i, descriptors);
			ena_return_receive_descriptors(device, descriptors, true);
			return B_IO_ERROR;
		}

		const uint16 offset = (i == 0) ? context.pkt_offset : 0;
		if (offset >= ENA_PACKET_BUFFER_SIZE
			|| bufferInfo[i].len > ENA_PACKET_BUFFER_SIZE - offset) {
			ERROR("device reported a %u byte segment at offset %u in descriptor "
				"%u of %u, which does not fit a %d byte buffer\n",
				bufferInfo[i].len, offset, i, descriptors,
				ENA_PACKET_BUFFER_SIZE);
			ena_return_receive_descriptors(device, descriptors, true);
			return B_IO_ERROR;
		}

		total += bufferInfo[i].len;
	}

	if (total == 0) {
		ERROR("device reported an empty frame across %u descriptor(s)\n",
			descriptors);
		ena_return_receive_descriptors(device, descriptors, true);
		return B_IO_ERROR;
	}

	net_buffer* buffer = sBufferModule->create(0);
	if (buffer == NULL) {
		/* Give the descriptors back rather than losing them. */
		ena_return_receive_descriptors(device, descriptors, true);
		return B_NO_MEMORY;
	}

	/* Second pass: copy. Every segment has been bounds-checked above, so this
	   loop cannot read out of range; the only thing that can fail is the
	   allocation append() does to grow the buffer. */
	status_t status = B_OK;
	for (uint16 i = 0; i < descriptors && status == B_OK; i++) {
		if (bufferInfo[i].len == 0)
			continue;

		ena_packet_buffer* slot = &device->rxBuffers[bufferInfo[i].req_id];
		const uint16 offset = (i == 0) ? context.pkt_offset : 0;

		status = sBufferModule->append(buffer, (uint8*)slot->data + offset,
			bufferInfo[i].len);
	}

	/* The device is done with every slot the moment its bytes have been copied
	   out, so the whole chain goes back here -- batched, because at a jumbo MTU
	   this is up to five descriptors per frame and each immediate post would be
	   its own doorbell write. */
	ena_return_receive_descriptors(device, descriptors);

	if (status != B_OK) {
		sBufferModule->free(buffer);
		return status;
	}

	/* Pass on what the device already checked so the stack need not redo it.

	   NET_BUFFER_L4_CHECKSUM_VALID tells tcp.cpp:718 and udp.cpp:849 to skip
	   their own verification, so the test must be "did the device check this",
	   not "could it have". l4_csum_checked is exactly that bit, and the device
	   sets it: measured at 983 of the first 1000 frames on c7g. So this branch is
	   both correct and load bearing -- it is why receive computes no TCP checksum
	   at all today.

	   There is deliberately no L3 equivalent. The receive descriptor carries
	   l3_csum_err with no l3_csum_checked beside it, so there is no way to
	   distinguish "the header checksum was verified and was good" from "the
	   device never looked", and l3_csum_err reads 0 in both cases. This used to
	   set NET_BUFFER_L3_CHECKSUM_VALID on `l3_proto == IPV4 && !l3_csum_err`,
	   which reads as though it closes that gap and does not: l3_proto says the
	   device *parsed* the frame as IPv4, which it does for steering regardless of
	   whether it validated anything. The result was that every received IPv4
	   frame arrived stamped as verified by nobody, and ipv4.cpp:1771 skipped the
	   check on the strength of it -- so a corrupted total length, protocol or
	   address was parsed rather than dropped.

	   rx_enabled cannot be used as a stand-in for the missing bit either: it
	   reads 0 on c7g while the device demonstrably is checking L4, so it does not
	   describe what the device is doing.

	   Not claiming it is therefore the only honest option, and it is nearly free:
	   the IPv4 header is 20 bytes against a 9001 byte frame, so verifying it in
	   software costs about 0.2% of the bytes the frame already costs. This is
	   also precisely what Linux's ena driver does -- ena_rx_checksum() sets
	   CHECKSUM_UNNECESSARY only from the L4 branch, never from L3, and Linux's IP
	   stack always verifies the header itself. */
	if (context.l4_csum_checked && !context.l4_csum_err)
		buffer->buffer_flags |= NET_BUFFER_L4_CHECKSUM_VALID;

	device->rxFrames++;
	device->rxBytes += buffer->size;
	if (context.l4_csum_checked)
		device->rxL4CsumChecked++;
	if (context.l4_csum_err)
		device->rxL4CsumErrors++;
	if (context.l3_proto == ENA_ETH_IO_L3_PROTO_IPV4)
		device->rxL3Ipv4Frames++;
	if (context.l3_csum_err)
		device->rxL3CsumErrors++;

	/* Once per power-of-four-ish milestone rather than on a timer, so a short
	   run reports early and a long one does not flood: the question these answer
	   is settled by the first few thousand frames. */
	if (device->rxFrames == 1000 || device->rxFrames == 50000
			|| device->rxFrames == 500000) {
		TRACE_ALWAYS("rx offload observed after %" B_PRIu64 " frames: "
			"l4_csum_checked %" B_PRIu64 ", l4_csum_err %" B_PRIu64 ", "
			"l3_proto==ipv4 %" B_PRIu64 ", l3_csum_err %" B_PRIu64 " "
			"(rx_supported %#" B_PRIx32 ", rx_enabled %#" B_PRIx32 ")\n",
			device->rxFrames, device->rxL4CsumChecked, device->rxL4CsumErrors,
			device->rxL3Ipv4Frames, device->rxL3CsumErrors,
			device->offloadRxSupported, device->offloadRxEnabled);
	}

	*_buffer = buffer;
	return B_OK;
}


static status_t
ena_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	ena_haiku_device* device = (ena_haiku_device*)cookie;

	switch (op) {
#ifdef ENA_DEBUG_FAULT_INJECTION
		case ENA_IOCTL_SUPPRESS_KEEP_ALIVE:
		{
			int32 mode = 0;
			if (length != sizeof(mode))
				return B_BAD_VALUE;
			if (user_memcpy(&mode, buffer, sizeof(mode)) != B_OK)
				return B_BAD_ADDRESS;

			device->suppressKeepAlive = mode;
			TRACE_ALWAYS("fault injection: keep-alive suppression = %" B_PRId32
				"\n", mode);
			return B_OK;
		}

		case ENA_IOCTL_HOLD_RESET:
		{
			int32 milliseconds = 0;
			if (length != sizeof(milliseconds))
				return B_BAD_VALUE;
			if (user_memcpy(&milliseconds, buffer, sizeof(milliseconds)) != B_OK)
				return B_BAD_ADDRESS;
			if (milliseconds < 0 || milliseconds > ENA_MAX_RESET_HOLD_MS)
				return B_BAD_VALUE;

			atomic_set(&device->holdResetMs, milliseconds);
			TRACE_ALWAYS("fault injection: reset hold = %" B_PRId32 " ms\n",
				milliseconds);
			return B_OK;
		}
#endif

		case ETHER_INIT:
			return B_OK;

		case ETHER_GETADDR:
			return user_memcpy(buffer, device->macAddress,
				ETHER_ADDRESS_LENGTH);

		case ETHER_GETFRAMESIZE:
		{
			/* The ethernet device layer derives the interface MTU by
			   subtracting the header length from this, so report the frame
			   size rather than the MTU. It clamps whatever we say to
			   ETHER_MAX_JUMBO_FRAME_SIZE and, if the driver does not do
			   net_buffer I/O, all the way back to ETHER_MAX_FRAME_SIZE; we do,
			   and ena_calculate_frame_limits() has already applied the same
			   ceiling, so what it uses is what we asked for. */
			uint32 frameSize = device->maxFrameSize;
			if (length != sizeof(frameSize))
				return B_BAD_VALUE;
			return user_memcpy(buffer, &frameSize, sizeof(frameSize));
		}

		case ETHER_GET_TX_CHECKSUM_OFFLOAD:
		{
			/* Answered from a value settled once at bring-up, so a reset cannot
			   change it under the stack: the stack reads this when the interface
			   comes up and then stops computing checksums for it. */
			uint32 offload = device->txChecksumOffload;
			if (length != sizeof(offload))
				return B_BAD_VALUE;
			return user_memcpy(buffer, &offload, sizeof(offload));
		}

		case ENA_IOCTL_TX_EXTRA_DOORBELLS:
		{
			int32 value;
			if (length != sizeof(value))
				return B_BAD_VALUE;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			if (value < 0 || value > ENA_MAX_EXTRA_DOORBELLS)
				return B_BAD_VALUE;
			/* Plain store, no lock: the transmit path only reads it, and a
			   frame that straddles the change is measured under whichever value
			   it happens to see -- which over a multi-second run is one frame in
			   millions. */
			device->txExtraDoorbells = value;
			TRACE_ALWAYS("tx_extra_doorbells now %" B_PRId32 " (at %" B_PRIu64
				" frames, %" B_PRIu64 " doorbells)\n", value, device->txFrames,
				device->txDoorbells);
			return B_OK;
		}

		case ENA_IOCTL_REARM_MODE:
		{
			int32 value;
			if (length != sizeof(value))
				return B_BAD_VALUE;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			if (value != ENA_REARM_IN_HANDLER
				&& value != ENA_REARM_AFTER_DRAIN) {
				return B_BAD_VALUE;
			}

			/* Switching arms mid-flight is safe in both directions, and it has to
			   be, because interleaving the two inside one boot is the whole point
			   of the knob:

			   - to ENA_REARM_IN_HANDLER: the next interrupt re-arms from the
			     handler, whatever the drains do.
			   - to ENA_REARM_AFTER_DRAIN: the vector is either armed now (so the
			     next interrupt masks it and a drain re-arms it) or a drain is
			     already on its way to re-arming it.

			   Neither ordering can leave it masked with nobody due to re-arm, so
			   no quiescing is needed. */
			atomic_set(&device->rearmMode, value);
			TRACE_ALWAYS("rearm mode now %" B_PRId32 " (%s) at %" B_PRId32
				" io interrupts, %" B_PRIu64 " rx frames\n", value,
				value == ENA_REARM_IN_HANDLER ? "in handler" : "after drain",
				device->ioInterrupts, device->rxFrames);
			return B_OK;
		}

		case ENA_IOCTL_RX_MODERATION:
		{
			int32 value;
			if (length != sizeof(value))
				return B_BAD_VALUE;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			/* Rejected rather than masked. The register field would silently keep
			   the low 15 bits of anything larger, so an out-of-range sweep point
			   would come back as a plausible number for an interval nobody asked
			   for -- the exact shape of error this instrument exists to rule out. */
			if (value < 0 || value > ENA_MAX_IRQ_INTERVAL)
				return B_BAD_VALUE;

			/* No quiescing needed, and nothing to undo if the write races a
			   re-arm: whichever value a concurrent re-arm reads is one of the two
			   the caller asked for, and the next re-arm after this returns uses
			   the new one. The stats snapshot reports the interval alongside the
			   counters, so a sample taken across the boundary identifies itself as
			   such rather than being silently mis-attributed. */
			atomic_set(&device->rxIrqInterval, value);
			TRACE_ALWAYS("rx moderation interval now %" B_PRId32 " ticks "
				"(resolution %u) at %" B_PRId32 " io interrupts, %" B_PRIu64
				" rx frames\n", value, device->comDev.intr_delay_resolution,
				device->ioInterrupts, device->rxFrames);
			return B_OK;
		}

		case ENA_IOCTL_RX_ADAPTIVE_MODERATION:
		{
			int32 value;
			if (length != sizeof(value))
				return B_BAD_VALUE;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			if (value != 0 && value != 1)
				return B_BAD_VALUE;
			/* Refused rather than silently accepted when the device never
			   advertised the feature: the interval is in ticks of the device's
			   delay resolution, and without the feature that resolution is
			   unknown, so a control loop would be programming an interval of
			   unknown length. */
			if (value == 1 && !device->moderationSupported)
				return B_NOT_SUPPORTED;

			/* No quiescing needed, same as the manual knob above: the sample path
			   reads these words without a shared lock and any straddling read is
			   benign. Clear the window before enabling so the first decision is
			   taken over traffic seen after the switch, not across it; restore the
			   compile-time default on disable so the manual instrument resumes
			   from a known point rather than the last window's leftover. */
			device->moderationWindowStart = 0;
			if (value == 0)
				atomic_set(&device->rxIrqInterval, ENA_RX_IRQ_INTERVAL);
			atomic_set(&device->rxAdaptive, value);
			TRACE_ALWAYS("adaptive rx moderation %s at %" B_PRId32
				" io interrupts, %" B_PRIu64 " rx frames\n",
				value ? "enabled" : "disabled", device->ioInterrupts,
				device->rxFrames);
			return B_OK;
		}

		case ENA_IOCTL_GET_IRQ_STATS:
		{
			struct ena_irq_stats stats;
			if (length != sizeof(stats))
				return B_BAD_VALUE;

			/* Read without either datapath lock. Every field here is written by
			   one side only and none of them gates anything, so the worst a
			   snapshot straddling an update can be is one frame out over a run of
			   millions -- whereas taking rxLock would serialise a diagnostic
			   against the path it is measuring, which is the one thing an
			   instrument for interrupt cadence must not do. */
			stats.ioInterrupts = (uint64)(uint32)atomic_get(
				&device->ioInterrupts);
			stats.irqArms = (uint64)(uint32)atomic_get(&device->irqArms);
			stats.rxFrames = device->rxFrames;
			stats.rxDrainCycles = device->rxDrainCycles;
			stats.txFrames = device->txFrames;
			stats.resetCount = (uint64)(uint32)atomic_get(&device->resetCount);
			stats.rxIrqInterval = (uint64)(uint32)atomic_get(
				&device->rxIrqInterval);
			stats.intrDelayResolution = device->comDev.intr_delay_resolution;
			stats.rearmMode = (uint64)(uint32)atomic_get(&device->rearmMode);

			return user_memcpy(buffer, &stats, sizeof(stats));
		}

		case ENA_IOCTL_GET_ENI_STATS:
		{
			struct ena_eni_stats stats;
			if (length != sizeof(stats))
				return B_BAD_VALUE;

			/* Read without either datapath lock, same as ENA_IOCTL_GET_IRQ_STATS:
			   each field is written by one side only and none of them gates
			   anything, so the worst a snapshot straddling an update can be is one
			   frame stale -- and taking rxLock or txLock here would serialise a
			   diagnostic against the very path it is meant to observe. */
			stats.hwRxDrops = device->hwRxDrops;
			stats.hwTxDrops = device->hwTxDrops;
			stats.rxPackets = device->rxFrames;
			stats.rxBytes = device->rxBytes;
			stats.txPackets = device->txFrames;
			stats.txBytes = device->txBytes;
			stats.rxDrainCycles = device->rxDrainCycles;
			stats.rxL4CsumChecked = device->rxL4CsumChecked;
			stats.rxL4CsumErrors = device->rxL4CsumErrors;
			stats.rxL3Ipv4Frames = device->rxL3Ipv4Frames;
			stats.rxL3CsumErrors = device->rxL3CsumErrors;
			stats.txChecksumOffloaded = device->txChecksumOffloaded;
			stats.txChecksumRejected = device->txChecksumRejected;
			stats.txDoorbells = device->txDoorbells;
			stats.txBurstExhausted = device->txBurstExhausted;
			stats.resetCount = (uint64)(uint32)atomic_get(&device->resetCount);
			stats.adminWedgeResets = device->adminWedgeResets;
			stats.fatalErrorResets = device->fatalErrorResets;
			stats.deviceRequestResets = device->deviceRequestResets;
			stats.missingTxResets = device->missingTxResets;
			stats.rxStallDetections = device->rxStallDetections;
			stats.linkUp = device->linkUp ? 1 : 0;
			stats.mtu = device->frameSize;

			return user_memcpy(buffer, &stats, sizeof(stats));
		}

		case ETHER_NONBLOCK:
		{
			int32 value;
			if (length != sizeof(value))
				return B_BAD_VALUE;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			device->nonBlocking = value == 0;
			return B_OK;
		}

		case ETHER_GET_LINK_STATE:
		{
			ether_link_state_t state;
			state.media = IFM_ETHER | IFM_FULL_DUPLEX
				| (device->linkUp ? IFM_ACTIVE : 0);
			/* ENA does not report a link rate; instances are at least
			   10 Gb/s worth of shared bandwidth. */
			state.speed = 10000000000ULL;
			state.quality = 1000;
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case ETHER_SETPROMISC:
		{
			int32 value;
			if (length != sizeof(value))
				return B_BAD_VALUE;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			/* ENA has no promiscuous mode: the hypervisor only ever delivers
			   frames addressed to this interface. Accept the request when it
			   asks for the behaviour we already have, so that tcpdump style
			   callers do not fail outright. */
			device->promiscuous = value != 0;
			return value != 0 ? B_NOT_SUPPORTED : B_OK;
		}

		case ETHER_ADDMULTI:
		case ETHER_REMMULTI:
			/* Likewise filtered upstream of us; nothing to program. */
			return B_OK;

		case ETHER_SEND_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return ena_send(device, (net_buffer*)buffer);

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return ena_receive(device, (net_buffer**)buffer);

		default:
			break;
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver module API


static float
ena_supports_device(device_node* parent)
{
	const char* bus;
	uint16 vendorId;
	uint16 baseClass;
	uint16 subClass;

	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
				!= B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_VENDOR_ID,
				&vendorId, false) != B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_TYPE, &baseClass,
				false) != B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_SUB_TYPE, &subClass,
				false) != B_OK) {
		return -1.0f;
	}

	if (strcmp(bus, "pci") != 0)
		return 0.0f;

	/* Match on vendor and class rather than a device id list: Amazon ships
	   several ids for the physical and virtual function and for LLQ-capable
	   variants, and they all speak the same interface. */
	if (vendorId != ENA_PCI_VENDOR_AMAZON)
		return 0.0f;
	if (baseClass != PCI_network || subClass != PCI_ethernet)
		return 0.0f;

	TRACE_ALWAYS("found an ENA device\n");
	return 0.8f;
}


static status_t
ena_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "AWS Elastic Network Adapter" } },
		{ NULL }
	};

	return sDeviceManager->register_node(parent, ENA_DRIVER_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
ena_init_driver(device_node* node, void** cookie)
{
	CALLED();

	ena_haiku_device* device = (ena_haiku_device*)calloc(1,
		sizeof(ena_haiku_device));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	device->registerArea = -1;
	device->memoryArea = -1;
	device->rxBufferArea = -1;
	device->txBufferArea = -1;
	device->rxReady = -1;
	device->txCompleted = -1;

	/* Not the calloc default: 0 is ENA_REARM_IN_HANDLER, the behaviour being
	   replaced. Set explicitly so that forgetting to set it cannot quietly ship
	   the old cadence. */
	device->rearmMode = ENA_REARM_AFTER_DRAIN;

	/* Also not the calloc default, and for a sharper reason than the above: zero
	   in this field does not mean "unset", it means "interrupt on every
	   completion". Failing to set it here would not ship a stale default, it would
	   ship no moderation at all. */
	device->rxIrqInterval = ENA_RX_IRQ_INTERVAL;

	*cookie = device;
	return B_OK;
}


static void
ena_uninit_driver(void* _cookie)
{
	CALLED();
	free(_cookie);
}


static status_t
ena_register_child_devices(void* _cookie)
{
	CALLED();
	ena_haiku_device* device = (ena_haiku_device*)_cookie;

	int32 id = sDeviceManager->create_id(ENA_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "net/ena/%" B_PRId32, id);

	return sDeviceManager->publish_device(device->node, name,
		ENA_DEVICE_MODULE_NAME);
}


//	#pragma mark -


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ NET_BUFFER_MODULE_NAME, (module_info**)&sBufferModule },
	{}
};


static struct device_module_info sEnaDevice = {
	{
		ENA_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	ena_init_device,
	ena_uninit_device,
	NULL,	// remove

	ena_open,
	ena_close,
	ena_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	ena_ioctl,

	NULL,	// select
	NULL,	// deselect
};


static struct driver_module_info sEnaDriver = {
	{
		ENA_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	ena_supports_device,
	ena_register_device,
	ena_init_driver,
	ena_uninit_driver,
	ena_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sEnaDriver,
	(module_info*)&sEnaDevice,
	NULL
};
