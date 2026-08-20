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
 * Scope of this version: one TX/RX queue pair, descriptors in host memory
 * (no Low Latency Queue push mode), one receive descriptor per frame and
 * therefore a 1500 byte MTU. That is enough to make an instance reachable;
 * multiple queues with RSS, LLQ and jumbo frames are each separate changes.
 */


#include "ena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <net/if_media.h>

#include <kernel.h>
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
	/* The device expects someone to notice these; there is nothing to do
	   until we implement a watchdog that resets the device when they stop
	   arriving. */
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
	sAenqHandlers.handlers[ENA_ADMIN_KEEP_ALIVE] = ena_aenq_keep_alive;
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


static int32
ena_io_interrupt(void* arg)
{
	ena_haiku_device* device = (ena_haiku_device*)arg;

	if (atomic_add(&device->ioInterrupts, 1) == 0)
		TRACE_ALWAYS("first io interrupt delivered\n");

	/* Both directions share this vector, so wake both waiters and let them
	   find out whether there was anything for them. */
	if (device->rxReady >= 0)
		release_sem_etc(device->rxReady, 1, B_DO_NOT_RESCHEDULE);
	if (device->txCompleted >= 0)
		release_sem_etc(device->txCompleted, 1, B_DO_NOT_RESCHEDULE);

	/* ENA masks a vector when it raises it, so it must be re-armed or this is
	   the last interrupt we ever see. Each completion queue carries its own
	   unmask register offset, and both of Amazon's drivers re-arm a queue pair
	   through its *transmit* CQ -- so this must be txCompletionQueue even
	   though receive is what we mostly care about. */
	if (device->txCompletionQueue != NULL) {
		struct ena_eth_io_intr_reg interruptRegister;
		ena_com_update_intr_reg(&interruptRegister, 0, 0, true, true);
		ena_com_unmask_intr(device->txCompletionQueue, &interruptRegister);
	}

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
			| BIT(ENA_ADMIN_KEEP_ALIVE);
		groups &= features->aenq.supported_groups;

		TRACE_ALWAYS("configuring AENQ groups %#" B_PRIx32 "\n", groups);

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


/*!	Creates the single TX/RX queue pair.

	NOTE: the device currently rejects CREATE_CQ with
	ENA_ADMIN_UNKNOWN_ERROR (6) and no extended status, and probing has ruled
	out the obvious suspects -- it fails identically for every ring depth from
	256 down to 16, for both directions, and for both a dedicated MSI-X vector
	and the management one. The command matches Amazon's byte for byte as far
	as can be told from the sources. This is the open question; see HANDOFF.md.
*/
static status_t
ena_setup_io_queues(ena_haiku_device* device)
{
	int result = ena_create_queue_pair(device, device->txRingSize,
		device->rxRingSize, ENA_IO_VECTOR_IDX);
	if (result != ENA_COM_OK) {
		ena_dump_admin_command_stream(device);
		return ena_translate_error(result);
	}

	device->ioVector = ENA_IO_VECTOR_IDX;

	return B_OK;
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
}


/*!	Hands every receive descriptor to the device and rings the doorbell. */
static status_t
ena_refill_receive_ring(ena_haiku_device* device, uint16 count)
{
	uint16 refilled = 0;

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

		refilled++;
	}

	if (refilled > 0) {
		device->rxNextToFill = (device->rxNextToFill + refilled)
			% device->rxRingSize;
		ena_com_write_sq_doorbell(device->rxSubmissionQueue);
	}

	return refilled > 0 ? B_OK : B_ERROR;
}


//	#pragma mark - device module API


static status_t
ena_init_device(void* _info, void** _cookie)
{
	CALLED();
	ena_haiku_device* device = (ena_haiku_device*)_info;

	/* First thing in the log, so every boot is attributable to a build. See the
	   comment on ENA_BUILD_TAG in ena.h for why this is not decoration. */
	TRACE_ALWAYS("driver build %s, compiled %s (hrev%d)\n", ENA_BUILD_TAG,
		ENA_BUILD_STAMP, ENA_HAIKU_REVISION);

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

	struct ena_com_dev_get_features_ctx features;
	memset(&features, 0, sizeof(features));

	status = ena_device_init(device, &features);
	if (status != B_OK)
		goto err_unmap;

	memcpy(device->macAddress, features.dev_attr.mac_addr,
		ETHER_ADDRESS_LENGTH);
	device->maxSupportedMtu = features.dev_attr.max_mtu;
	device->frameSize = min_c((uint32)ENA_FRAME_SIZE, device->maxSupportedMtu);

	TRACE_ALWAYS("MAC %02x:%02x:%02x:%02x:%02x:%02x, device MTU limit %"
		B_PRIu32 ", using %" B_PRIu32 "\n",
		device->macAddress[0], device->macAddress[1], device->macAddress[2],
		device->macAddress[3], device->macAddress[4], device->macAddress[5],
		device->maxSupportedMtu, device->frameSize);

	ena_configure_placement_policy(device, &features.llq);
	ena_calculate_ring_sizes(device, &features);
	if (device->txRingSize < ENA_MIN_RING_SIZE
		|| device->rxRingSize < ENA_MIN_RING_SIZE) {
		ERROR("device offers unusably short rings (%u tx, %u rx)\n",
			device->txRingSize, device->rxRingSize);
		status = B_NOT_SUPPORTED;
		goto err_device;
	}

	status = ena_enable_msix(device);
	if (status != B_OK)
		goto err_device;

	status = install_io_interrupt_handler(device->managementIrq,
		ena_management_interrupt, device, 0);
	if (status != B_OK) {
		ERROR("cannot install the management interrupt handler: %s\n",
			strerror(status));
		goto err_device;
	}
	device->managementIrqInstalled = true;

	status = install_io_interrupt_handler(device->ioIrq, ena_io_interrupt,
		device, 0);
	if (status != B_OK) {
		ERROR("cannot install the io interrupt handler: %s\n",
			strerror(status));
		goto err_device;
	}
	device->ioIrqInstalled = true;

	/* The reference drivers initialise interrupt moderation here, while still
	   polling, and the device is evidently particular about how much of its
	   configuration exists before queues are created. */
	if (ena_com_init_interrupt_moderation(&device->comDev) != ENA_COM_OK)
		TRACE_ALWAYS("interrupt moderation unavailable; continuing\n");

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
		goto err_device;

	status = ena_setup_io_queues(device);
	if (status != B_OK)
		goto err_device;

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
	   frameSize, not the reference's 9001: what the device is told here has to
	   agree with what the receive path can actually accept, and ours posts one
	   2048 byte buffer per frame with max_bufs = 1. Telling the device 9001
	   while advertising 1500 to the stack left a window where a peer that
	   ignored our advertised MTU could put a frame on the wire that the device
	   would accept and we could not reassemble. Nothing on an EC2 link does
	   that, so it never bit -- but it was an inconsistency held in place only by
	   the good manners of the other end, which is not a property to depend on.

	   Raising this again is a prerequisite for jumbo frames, and it is a
	   two-part change: multi-descriptor receive first, then this value. */
	{
		const uint32 deviceMtu = device->frameSize;
		int mtuResult = ena_com_set_dev_mtu(&device->comDev, deviceMtu);
		TRACE_ALWAYS("set device MTU %" B_PRIu32 " after queue creation "
			"(matching what we report to the stack): %s\n", deviceMtu,
			mtuResult == ENA_COM_OK ? "ok" : "FAILED");
		if (mtuResult != ENA_COM_OK) {
			status = ena_translate_error(mtuResult);
			goto err_device;
		}
	}

	/* After the queues, because the device may have forced a smaller depth
	   than we asked for and the buffer pools are sized from it. */
	status = ena_setup_buffers(device);
	if (status != B_OK) {
		ERROR("cannot allocate packet buffers: %s\n", strerror(status));
		goto err_device;
	}

	mutex_init(&device->txLock, "ena tx");
	mutex_init(&device->rxLock, "ena rx");
	device->rxReady = -1;
	device->txCompleted = -1;

	/* ENA reports link state only through asynchronous events, and does not
	   send one for a link that is already up when we attach. On EC2 the link
	   is always up, so start optimistic and let an event correct us. */
	device->linkUp = true;

	TRACE_ALWAYS("attached; interrupts so far: %" B_PRId32 " management, %"
		B_PRId32 " io%s\n", device->managementInterrupts,
		device->ioInterrupts,
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

	if (device->msixEnabled) {
		device->pci->unconfigure_msi(device->pciDevice);
		device->msixEnabled = false;
	}

	ena_release_buffers(device);
err_unmap:
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

	device->running = false;

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

	/* Stop the device before anything it can DMA into goes away. */
	ena_destroy_device(device);

	/* There is no disable_msix(); unconfigure_msi() tries MSI-X first and
	   handles it, which is the only teardown path the bus manager exposes. */
	if (device->msixEnabled) {
		device->pci->unconfigure_msi(device->pciDevice);
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

	device->rxReady = create_sem(0, "ena rx ready");
	device->txCompleted = create_sem(0, "ena tx completed");
	if (device->rxReady < B_OK || device->txCompleted < B_OK) {
		delete_sem(device->rxReady);
		delete_sem(device->txCompleted);
		device->rxReady = device->txCompleted = -1;
		return B_NO_MORE_SEMS;
	}

	/* Post every receive descriptor before the first interrupt can arrive. */
	device->rxNextToFill = 0;
	ena_refill_receive_ring(device, device->rxRingSize);

	/* Arm the io vector; it starts masked. Through the transmit CQ, for the
	   reason given in ena_io_interrupt(). */
	struct ena_eth_io_intr_reg interruptRegister;
	ena_com_update_intr_reg(&interruptRegister, 0, 0, true, true);
	ena_com_unmask_intr(device->txCompletionQueue, &interruptRegister);

	*_cookie = device;
	return B_OK;
}


static status_t
ena_close(void* cookie)
{
	CALLED();
	ena_haiku_device* device = (ena_haiku_device*)cookie;

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
			   the array. */
			ERROR("device completed transmit request id %u that was not in "
				"use\n", requestId);
			break;
		}

		sBufferModule->free(entry->buffer);
		entry->buffer = NULL;

		device->txFreeIds[device->txFreeCount++] = requestId;

		/* Acknowledge exactly what this packet occupied; it is only 1 while we
		   emit no meta descriptor. */
		ena_com_comp_ack(device->txSubmissionQueue, entry->descriptors);
		entry->descriptors = 0;
	}
}


static status_t
ena_send(ena_haiku_device* device, net_buffer* buffer)
{
	MutexLocker locker(device->txLock);

	ena_reclaim_transmitted(device);

	/* The submission queue runs out before the request-id pool does: it
	   refuses at q_depth - 1 outstanding, and ena_com_prepare_tx() wants room
	   for the packet plus a possible meta descriptor. Waiting on txFreeCount
	   alone would never block, and we would drop frames instead. */
	while (device->txFreeCount == 0
			|| !ena_com_sq_have_enough_space(device->txSubmissionQueue, 2)) {
		locker.Unlock();

		if (device->nonBlocking)
			return B_WOULD_BLOCK;

		status_t status = acquire_sem(device->txCompleted);
		if (status != B_OK)
			return status;

		locker.Lock();
		ena_reclaim_transmitted(device);
	}

	uint16 requestId = device->txFreeIds[--device->txFreeCount];
	ena_tx_buffer* entry = &device->txBuffers[requestId];

	/* The frame has to be in one physically contiguous piece to go out in a
	   single descriptor, and a net_buffer is not, so it is copied into this
	   request id's bounce slot. TODO: map the net_buffer's own pages and build
	   a multi-descriptor request instead of copying. */
	size_t size = buffer->size;
	if (size > ENA_PACKET_BUFFER_SIZE) {
		device->txFreeIds[device->txFreeCount++] = requestId;
		ERROR("refusing a %" B_PRIuSIZE " byte frame; the limit is %d\n", size,
			ENA_PACKET_BUFFER_SIZE);
		return B_BAD_VALUE;
	}

	if (sBufferModule->read(buffer, 0, entry->slot.data, size) != B_OK) {
		device->txFreeIds[device->txFreeCount++] = requestId;
		return B_BAD_DATA;
	}

	struct ena_com_tx_ctx context;
	memset(&context, 0, sizeof(context));
	context.req_id = requestId;

	/* In LLQ mode the leading bytes of the frame are pushed straight into the
	   device's memory window rather than fetched by DMA, so the descriptor only
	   covers whatever is left over. A short frame can be entirely header, in
	   which case there is no buffer descriptor at all. */
	uint16 headerLength = 0;
	if (device->comDev.tx_mem_queue_type == ENA_ADMIN_PLACEMENT_POLICY_DEV) {
		headerLength = (uint16)min_c((size_t)size,
			(size_t)device->txSubmissionQueue->tx_max_header_size);
		context.push_header = entry->slot.data;
		context.header_len = headerLength;
	}

	struct ena_com_buf comBuffer;
	if (size > headerLength) {
		comBuffer.paddr = entry->slot.physicalAddress + headerLength;
		comBuffer.len = (uint16)(size - headerLength);
		context.ena_bufs = &comBuffer;
		context.num_bufs = 1;
	}

	int descriptors = 0;
	int result = ena_com_prepare_tx(device->txSubmissionQueue, &context,
		&descriptors);
	if (result != ENA_COM_OK) {
		device->txFreeIds[device->txFreeCount++] = requestId;
		ERROR("cannot prepare a transmit descriptor: %d\n", result);
		return ena_translate_error(result);
	}

	entry->buffer = buffer;
	entry->descriptors = (uint16)descriptors;
	ena_com_write_sq_doorbell(device->txSubmissionQueue);

	return B_OK;
}


static status_t
ena_receive(ena_haiku_device* device, net_buffer** _buffer)
{
	struct ena_com_rx_buf_info bufferInfo[2];
	struct ena_com_rx_ctx context;

	MutexLocker locker(device->rxLock);

	while (true) {
		memset(&context, 0, sizeof(context));
		context.ena_bufs = bufferInfo;
		context.max_bufs = 1;

		/* ena_com_rx_pkt() returns 0 on success and reports how many
		   descriptors it consumed in context.descs; every failure is a small
		   positive ENA_COM_* code. Testing the return value for a count means
		   never seeing a packet, and reading ena_bufs on the error path where
		   the HAL never wrote it. */
		int result = ena_com_rx_pkt(device->rxCompletionQueue,
			device->rxSubmissionQueue, &context);
		if (result != ENA_COM_OK) {
			ERROR("receive failed: %d\n", result);
			return ena_translate_error(result);
		}

		if (context.descs > 0)
			break;

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
	}

	uint16 requestId = bufferInfo[0].req_id;
	if (requestId >= device->rxRingSize) {
		ERROR("device returned an out-of-range receive request id %u\n",
			requestId);
		return B_IO_ERROR;
	}

	ena_packet_buffer* slot = &device->rxBuffers[requestId];
	uint16 length = bufferInfo[0].len;

	/* Both of these come straight out of a completion descriptor, so treat
	   them as untrusted: an oversized length would read past this slot and,
	   for the last slot, past the area. */
	if (context.pkt_offset >= ENA_PACKET_BUFFER_SIZE
		|| length > ENA_PACKET_BUFFER_SIZE - context.pkt_offset) {
		ERROR("device reported a %u byte frame at offset %u, which does not "
			"fit a %d byte buffer\n", length, context.pkt_offset,
			ENA_PACKET_BUFFER_SIZE);
		ena_refill_receive_ring(device, 1);
		return B_IO_ERROR;
	}

	net_buffer* buffer = sBufferModule->create(0);
	if (buffer == NULL) {
		/* Give the descriptor back rather than losing it. */
		ena_refill_receive_ring(device, 1);
		return B_NO_MEMORY;
	}

	status_t status = sBufferModule->append(buffer,
		(uint8*)slot->data + context.pkt_offset, length);

	/* The device is done with the slot the moment we have copied out of it. */
	ena_refill_receive_ring(device, 1);

	if (status != B_OK) {
		sBufferModule->free(buffer);
		return status;
	}

	/* Pass on what the device already checked so the stack need not redo it. */
	if (context.l4_csum_checked && !context.l4_csum_err)
		buffer->buffer_flags |= NET_BUFFER_L4_CHECKSUM_VALID;
	/* l3_csum_err is also zero when the device never looked, so the protocol
	   has to be confirmed before claiming the checksum was verified. */
	if (context.l3_proto == ENA_ETH_IO_L3_PROTO_IPV4 && !context.l3_csum_err)
		buffer->buffer_flags |= NET_BUFFER_L3_CHECKSUM_VALID;

	*_buffer = buffer;
	return B_OK;
}


static status_t
ena_ioctl(void* cookie, uint32 op, void* buffer, size_t length)
{
	ena_haiku_device* device = (ena_haiku_device*)cookie;

	switch (op) {
		case ETHER_INIT:
			return B_OK;

		case ETHER_GETADDR:
			return user_memcpy(buffer, device->macAddress,
				ETHER_ADDRESS_LENGTH);

		case ETHER_GETFRAMESIZE:
		{
			/* The ethernet device layer derives the interface MTU by
			   subtracting the header length from this, so report the frame
			   size. frameSize itself is the L3 MTU we gave the device. */
			uint32 frameSize = device->frameSize + ETHER_HEADER_LENGTH;
			if (length != sizeof(frameSize))
				return B_BAD_VALUE;
			return user_memcpy(buffer, &frameSize, sizeof(frameSize));
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
