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
   directory before building, which is the habit to keep. */
#define ENA_BUILD_TAG		"wd-asid"
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

/* v1 receives each frame into one descriptor, so the buffer has to hold a
   whole frame. EC2 links allow a 9001 byte MTU, but jumbo frames need
   multi-descriptor receive; we ask the device for 1500 to match these buffers
   and leave jumbo for later. */
#define ENA_FRAME_SIZE		1500
#define ENA_PACKET_BUFFER_SIZE	2048

/* Watchdog cadence and timeout, both matching Linux and FreeBSD exactly: a
   one-second timer against a six-second keep-alive deadline
   (ena_netdev.h:130 ENA_DEVICE_KALIVE_TIMEOUT, ena.h:173
   ENA_DEFAULT_KEEP_ALIVE_TO). The device can also ask not to be watchdogged at
   all via a hardware hint; we do not read hints yet, which is recorded as a
   limitation in docs/watchdog-design.md rather than pretended away. */
#define ENA_WATCHDOG_INTERVAL_US	1000000
#define ENA_KEEP_ALIVE_TIMEOUT_US	6000000

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
   Argument: 0 = off, 1 = suppress a single keep-alive, 2 = suppress until
   cleared (for the repeated-reset scenario). */
#define ENA_IOCTL_SUPPRESS_KEEP_ALIVE	9800
#endif

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
	   number can be acknowledged on completion. */
	uint16		descriptors;
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

#ifdef ENA_DEBUG_FAULT_INJECTION
	/* Debug only, compiled out by default: makes the keep-alive handler stop
	   updating lastKeepAlive so the watchdog observes a dead device while the
	   device is in fact healthy. 0 = off, 1 = suppress one timeout,
	   2 = suppress until cleared. */
	int32				suppressKeepAlive;
#endif

	uint8				macAddress[ETHER_ADDRESS_LENGTH];
	uint32				frameSize;
	uint32				maxSupportedMtu;
	bool				linkUp;
	bool				nonBlocking;
	bool				promiscuous;
	bool				running;

	uint32				multicastCount;
	ether_address_t			multicast[ENA_MAX_MULTICAST];
};


#endif	/* ENA_H */
