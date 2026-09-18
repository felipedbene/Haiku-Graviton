/*
 * Copyright 2006-2017, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "device_interfaces.h"
#include "domains.h"
#include "interfaces.h"
#include "stack_private.h"
#include "utility.h"

#include <net_device.h>

#include <kscheduler.h>
#include <lock.h>
#include <smp.h>
#include <util/AutoLock.h>

#include <KernelExport.h>

#include <net/if_dl.h>
#include <netinet/in.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


//#define TRACE_DEVICE_INTERFACES
#ifdef TRACE_DEVICE_INTERFACES
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


static mutex sLock;
static DeviceInterfaceList sInterfaces;
static uint32 sDeviceIndex;


/*!	The body shared by the queue-0 reader (device_reader_thread) and the
	per-queue readers (device_queue_reader_thread). It reads as many packets as
	available from one receive queue, deframes them, and puts them into that
	queue's fifo.

	\a queue is the queue index: 0 uses the legacy single-queue receive_data()
	entry (so the compiled single-queue path is unchanged), any higher index
	uses receive_data_queue(). The fifo, its watermark, and the drop counters
	are passed in so each queue owns its own -- the drop counters are bumped
	without atomics, which is safe only because exactly one reader touches each.
*/
static status_t
reader_loop(net_device_interface* interface, uint32 queue, net_fifo* fifo,
	net_fifo_watermark* diagnostics, uint64* deframeDropped,
	uint64* enqueueDropped)
{
	net_device* device = interface->device;
	status_t status = B_OK;

	while ((device->flags & IFF_UP) != 0) {
		net_buffer* buffer;
		if (queue == 0)
			status = device->module->receive_data(device, &buffer);
		else
			status = device->module->receive_data_queue(device, queue, &buffer);
		if (status == B_OK) {
			// feed device monitors
			if (atomic_get(&interface->monitor_count) > 0)
				device_interface_monitor_receive(interface, buffer);

			ASSERT(buffer->interface_address == NULL);

			if (interface->deframe_func(interface->device, buffer) != B_OK) {
				gNetBufferModule.free(buffer);
				atomic_add((int32*)&device->stats.receive.dropped, 1);
				(*deframeDropped)++;
				continue;
			}

			const size_t packetSize = buffer->size;
			status = fifo_enqueue_buffer_tracked(fifo, buffer, diagnostics);
			if (status == B_OK) {
				atomic_add((int32*)&device->stats.receive.packets, 1);
				atomic_add64((int64*)&device->stats.receive.bytes, packetSize);
			} else {
				gNetBufferModule.free(buffer);
				atomic_add((int32*)&device->stats.receive.dropped, 1);
				(*enqueueDropped)++;
			}
		} else if (status == B_DEVICE_NOT_FOUND) {
			device_removed(device);
			return status;
		} else {
			atomic_add((int32*)&device->stats.receive.errors, 1);

			// In case of error, give the other threads some
			// time to run since this is a high priority time thread.
			snooze(10000);
		}
	}

	return status;
}


/*!	A service thread for each device interface. It just reads as many packets
	as available, deframes them, and puts them into the receive queue of the
	device interface.
*/
static status_t
device_reader_thread(void* _interface)
{
	net_device_interface* interface = (net_device_interface*)_interface;
	return reader_loop(interface, 0, &interface->receive_queue,
		&interface->receive_queue_diagnostics,
		&interface->receive_deframe_dropped,
		&interface->receive_enqueue_dropped);
}


static status_t
device_queue_reader_thread(void* _queue)
{
	net_device_interface_queue* queue = (net_device_interface_queue*)_queue;
	return reader_loop(queue->interface, queue->index, &queue->receive_queue,
		&queue->receive_queue_diagnostics, &queue->receive_deframe_dropped,
		&queue->receive_enqueue_dropped);
}


/*!	The dispatch body shared by the queue-0 consumer (device_consumer_thread)
	and the per-queue consumers (device_queue_consumer_thread). It drains one
	fifo and hands each buffer to the first matching receive handler.

	The single-queue consumer (\a multiqueue false) keeps the recursive
	receive_lock across the handler walk exactly as it always has. A multiqueue
	consumer instead takes only the receive_handlers_lock for read, so N
	consumers do not serialize on receive_lock; mutators exclude both by taking
	receive_lock and the write lock together (D30).
*/
static status_t
consumer_loop(net_device_interface* interface, net_fifo* fifo,
	net_fifo_watermark* diagnostics, bool multiqueue, int32* stopping)
{
	net_device* device = interface->device;
	net_buffer* buffer;

	// \a stopping is the per-queue stop flag for an extra queue's consumer, or
	// NULL for the queue-0 consumer (which exits on ref_count reaching 0 during
	// the final put). An extra consumer must not rely on ref_count -- down()
	// tears its fifo down without dropping ref_count -- so it exits as soon as
	// the flag is set, before it can loop back to lock a destroyed fifo.
	while ((stopping == NULL || atomic_get(stopping) == 0)
			&& atomic_get(&interface->ref_count) > 0) {
		ssize_t status = fifo_dequeue_buffer_tracked(fifo, 0,
			B_INFINITE_TIMEOUT, &buffer, diagnostics);
		if (status != B_OK) {
			if (status == B_INTERRUPTED)
				continue;
			break;
		}

		if (buffer->interface_address != NULL) {
			// If the interface is already specified, this buffer was
			// delivered locally.
			if (buffer->interface_address->domain->module->receive_data(buffer)
					== B_OK)
				buffer = NULL;
		} else {
			sockaddr_dl& linkAddress = *(sockaddr_dl*)buffer->source;
			int32 genericType = buffer->type;
			int32 specificType = B_NET_FRAME_TYPE(linkAddress.sdl_type,
				ntohs(linkAddress.sdl_e_type));

			buffer->index = interface->device->index;

			// Find handler for this packet
			RecursiveLocker recursiveLocker;
			ReadLocker readLocker;
			if (multiqueue)
				readLocker.SetTo(interface->receive_handlers_lock, false);
			else
				recursiveLocker.SetTo(interface->receive_lock, false);

			DeviceHandlerList::Iterator iterator
				= interface->receive_funcs.GetIterator();
			while (buffer != NULL && iterator.HasNext()) {
				net_device_handler* handler = iterator.Next();

				// If the handler returns B_OK, it consumed the buffer - first
				// handler wins.
				if ((handler->type == genericType
						|| handler->type == specificType)
					&& handler->func(handler->cookie, device, buffer) == B_OK)
					buffer = NULL;
			}
		}

		if (buffer != NULL)
			gNetBufferModule.free(buffer);
	}

	return B_OK;
}


static status_t
device_consumer_thread(void* _interface)
{
	net_device_interface* interface = (net_device_interface*)_interface;
	return consumer_loop(interface, &interface->receive_queue,
		&interface->receive_queue_diagnostics, false, NULL);
}


static status_t
device_queue_consumer_thread(void* _queue)
{
	net_device_interface_queue* queue = (net_device_interface_queue*)_queue;
	return consumer_loop(queue->interface, &queue->receive_queue,
		&queue->receive_queue_diagnostics, true, &queue->stopping);
}


/*!	The domain's device receive handler - this will inject the net_buffers into
	the protocol layer (the domain's registered receive handler).
*/
static status_t
domain_receive_adapter(void* cookie, net_device* device, net_buffer* buffer)
{
	net_domain_private* domain = (net_domain_private*)cookie;

	return domain->module->receive_data(buffer);
}


static net_device_interface*
find_device_interface(const char* name)
{
	ASSERT_LOCKED_MUTEX(&sLock);
	DeviceInterfaceList::Iterator iterator = sInterfaces.GetIterator();

	while (net_device_interface* interface = iterator.Next()) {
		if (!strcmp(interface->device->name, name))
			return interface;
	}

	return NULL;
}


static net_device_interface*
allocate_device_interface(net_device* device, net_device_module_info* module)
{
	net_device_interface* interface = new(std::nothrow) net_device_interface;
	if (interface == NULL)
		return NULL;

	recursive_lock_init(&interface->receive_lock, "device interface receive");
	recursive_lock_init(&interface->monitor_lock, "device interface monitors");
	rw_lock_init(&interface->receive_handlers_lock,
		"device interface receive handlers");

	char name[128];
	snprintf(name, sizeof(name), "%s receive queue", device->name);

	if (init_fifo(&interface->receive_queue, name, 16 * 1024 * 1024) < B_OK)
		goto error1;

	interface->receive_deframe_dropped = 0;
	interface->receive_enqueue_dropped = 0;
	init_fifo_watermark(&interface->receive_queue_diagnostics,
		interface->receive_queue.max_bytes);

	interface->device = device;
	interface->up_count = 0;
	interface->ref_count = 1;
	interface->busy = false;
	interface->monitor_count = 0;
	interface->deframe_func = NULL;
	interface->deframe_ref_count = 0;
	interface->receive_queue_count = 1;
	interface->queues = NULL;

	snprintf(name, sizeof(name), "%s consumer", device->name);

	interface->reader_thread   = -1;
	interface->consumer_thread = spawn_kernel_thread(device_consumer_thread,
		name, B_DISPLAY_PRIORITY, interface);
	if (interface->consumer_thread < B_OK)
		goto error2;
	resume_thread(interface->consumer_thread);

	// TODO: proper interface index allocation
	device->index = ++sDeviceIndex;
	device->module = module;

	sInterfaces.Add(interface);
	return interface;

error2:
	uninit_fifo(&interface->receive_queue);
error1:
	rw_lock_destroy(&interface->receive_handlers_lock);
	recursive_lock_destroy(&interface->receive_lock);
	recursive_lock_destroy(&interface->monitor_lock);
	delete interface;

	return NULL;
}


static void
notify_device_monitors(net_device_interface* interface, int32 event)
{
	RecursiveLocker locker(interface->monitor_lock);

	DeviceMonitorList::Iterator iterator
		= interface->monitor_funcs.GetIterator();
	while (net_device_monitor* monitor = iterator.Next()) {
		// it's safe for the "current" item to remove itself.
		monitor->event(monitor, event);
	}
}


#if ENABLE_DEBUGGER_COMMANDS


static int
dump_device_interface(int argc, char** argv)
{
	if (argc != 2) {
		kprintf("usage: %s [address]\n", argv[0]);
		return 0;
	}

	net_device_interface* interface
		= (net_device_interface*)parse_expression(argv[1]);

	kprintf("device:            %p\n", interface->device);
	kprintf("reader_thread:     %" B_PRId32 "\n", interface->reader_thread);
	kprintf("up_count:          %" B_PRIu32 "\n", interface->up_count);
	kprintf("ref_count:         %" B_PRId32 "\n", interface->ref_count);
	kprintf("deframe_func:      %p\n", interface->deframe_func);
	kprintf("deframe_ref_count: %" B_PRId32 "\n", interface->ref_count);
	kprintf("consumer_thread:   %" B_PRId32 "\n", interface->consumer_thread);

	kprintf("monitor_count:     %" B_PRId32 "\n", interface->monitor_count);
	kprintf("monitor_lock:      %p\n", &interface->monitor_lock);
	kprintf("monitor_funcs:\n");
	DeviceMonitorList::Iterator monitorIterator
		= interface->monitor_funcs.GetIterator();
	while (monitorIterator.HasNext())
		kprintf("  %p\n", monitorIterator.Next());

	kprintf("receive_lock:      %p\n", &interface->receive_lock);
	kprintf("receive_queue:     %p\n", &interface->receive_queue);
	kprintf("  limit/current:   %" B_PRIuSIZE " / %" B_PRIuSIZE " bytes, %"
		B_PRIu32 " packets\n", interface->receive_queue.max_bytes,
		interface->receive_queue.current_bytes,
		interface->receive_queue_diagnostics.current_packets);
	kprintf("  peak:            %" B_PRIuSIZE " bytes, %" B_PRIu32 " packets\n",
		interface->receive_queue_diagnostics.peak_bytes,
		interface->receive_queue_diagnostics.peak_packets);
	kprintf("  dropped:         %" B_PRIu64 " deframe, %" B_PRIu64 " enqueue\n",
		interface->receive_deframe_dropped, interface->receive_enqueue_dropped);
	kprintf("  enqueue failed:  %" B_PRIu64 " (%" B_PRIu64 " ENOBUFS, %" B_PRIu64
		" other)\n", interface->receive_queue_diagnostics.fail_total,
		interface->receive_queue_diagnostics.fail_nobufs,
		interface->receive_queue_diagnostics.fail_other);

	kprintf("receive_queue_cnt: %" B_PRIu32 "\n",
		interface->receive_queue_count);
	for (uint32 i = 1; i < interface->receive_queue_count; i++) {
		net_device_interface_queue* queue = &interface->queues[i - 1];
		kprintf("queue %" B_PRIu32 ": reader %" B_PRId32 " consumer %" B_PRId32
			"\n", i, queue->reader_thread, queue->consumer_thread);
		kprintf("  limit/current:   %" B_PRIuSIZE " / %" B_PRIuSIZE " bytes, %"
			B_PRIu32 " packets\n", queue->receive_queue.max_bytes,
			queue->receive_queue.current_bytes,
			queue->receive_queue_diagnostics.current_packets);
		kprintf("  peak:            %" B_PRIuSIZE " bytes, %" B_PRIu32
			" packets\n", queue->receive_queue_diagnostics.peak_bytes,
			queue->receive_queue_diagnostics.peak_packets);
		kprintf("  dropped:         %" B_PRIu64 " deframe, %" B_PRIu64
			" enqueue\n", queue->receive_deframe_dropped,
			queue->receive_enqueue_dropped);
	}

	kprintf("receive_funcs:\n");
	DeviceHandlerList::Iterator handlerIterator
		= interface->receive_funcs.GetIterator();
	while (handlerIterator.HasNext())
		kprintf("  %p\n", handlerIterator.Next());

	return 0;
}


static int
dump_device_interfaces(int argc, char** argv)
{
	DeviceInterfaceList::Iterator iterator = sInterfaces.GetIterator();
	while (net_device_interface* interface = iterator.Next()) {
		kprintf("  %p  %s\n", interface, interface->device->name);
	}

	return 0;
}


#endif	// ENABLE_DEBUGGER_COMMANDS


//	#pragma mark - device interfaces


net_device_interface*
acquire_device_interface(net_device_interface* interface)
{
	if (interface == NULL || atomic_add(&interface->ref_count, 1) == 0)
		return NULL;

	return interface;
}


void
get_device_interface_address(net_device_interface* interface,
	sockaddr* _address)
{
	sockaddr_dl &address = *(sockaddr_dl*)_address;

	address.sdl_family = AF_LINK;
	address.sdl_index = interface->device->index;
	address.sdl_type = interface->device->type;
	address.sdl_nlen = strlen(interface->device->name);
	address.sdl_slen = 0;
	memcpy(address.sdl_data, interface->device->name, address.sdl_nlen);

	address.sdl_alen = interface->device->address.length;
	memcpy(LLADDR(&address), interface->device->address.data, address.sdl_alen);

	address.sdl_len = sizeof(sockaddr_dl) - sizeof(address.sdl_data)
		+ address.sdl_nlen + address.sdl_alen;
}


uint32
count_device_interfaces()
{
	MutexLocker locker(sLock);

	DeviceInterfaceList::Iterator iterator = sInterfaces.GetIterator();
	uint32 count = 0;

	while (iterator.HasNext()) {
		iterator.Next();
		count++;
	}

	return count;
}


/*!	Dumps a list of all interfaces into the supplied userland buffer.
	If the interfaces don't fit into the buffer, an error (\c ENOBUFS) is
	returned.
*/
status_t
list_device_interfaces(void* _buffer, size_t* bufferSize)
{
	MutexLocker locker(sLock);

	DeviceInterfaceList::Iterator iterator = sInterfaces.GetIterator();
	UserBuffer buffer(_buffer, *bufferSize);

	while (net_device_interface* interface = iterator.Next()) {
		buffer.Push(interface->device->name, IF_NAMESIZE);

		sockaddr_storage address;
		get_device_interface_address(interface, (sockaddr*)&address);

		buffer.Push(&address, address.ss_len);
		if (IF_NAMESIZE + address.ss_len < (int)sizeof(ifreq))
			buffer.Pad(sizeof(ifreq) - IF_NAMESIZE - address.ss_len);

		if (buffer.Status() != B_OK)
			return buffer.Status();
	}

	*bufferSize = buffer.BytesConsumed();
	return B_OK;
}


/*!	Releases the reference for the interface. When all references are
	released, the interface is removed.
*/
void
put_device_interface(struct net_device_interface* interface)
{
	if (interface == NULL)
		return;

	if (atomic_add(&interface->ref_count, -1) != 1)
		return;

	// Indicate we are in the process of destroying this interface
	// by setting its ref_count to 0.
	interface->ref_count = 0;

	MutexLocker locker(sLock);
	sInterfaces.Remove(interface);
	locker.Unlock();

	uninit_fifo(&interface->receive_queue);
	wait_for_thread(interface->consumer_thread, NULL);

	net_device* device = interface->device;
	const char* moduleName = device->module->info.name;

	device->module->uninit_device(device);
	put_module(moduleName);

	rw_lock_destroy(&interface->receive_handlers_lock);
	recursive_lock_destroy(&interface->monitor_lock);
	recursive_lock_destroy(&interface->receive_lock);
	delete interface;
}


/*!	Finds an interface by the specified index and acquires a reference to it.
*/
struct net_device_interface*
get_device_interface(uint32 index)
{
	MutexLocker locker(sLock);

	// TODO: maintain an array of all device interfaces instead
	DeviceInterfaceList::Iterator iterator = sInterfaces.GetIterator();
	while (net_device_interface* interface = iterator.Next()) {
		if (interface->device->index == index) {
			if (interface->busy)
				break;

			if (atomic_add(&interface->ref_count, 1) != 0)
				return interface;
		}
	}

	return NULL;
}


/*!	Finds an interface by the specified name and grabs a reference to it.
	If the interface does not yet exist, a new one is created.
*/
struct net_device_interface*
get_device_interface(const char* name, bool create)
{
	MutexLocker locker(sLock);

	net_device_interface* interface = find_device_interface(name);
	if (interface != NULL) {
		if (interface->busy)
			return NULL;

		if (atomic_add(&interface->ref_count, 1) != 0)
			return interface;

		// try to recreate interface - it just got removed
	}

	if (!create)
		return NULL;

	void* cookie = open_module_list("network/devices");
	if (cookie == NULL)
		return NULL;

	while (true) {
		char moduleName[B_FILE_NAME_LENGTH];
		size_t length = sizeof(moduleName);
		if (read_next_module_name(cookie, moduleName, &length) != B_OK)
			break;

		TRACE(("get_device_interface: ask \"%s\" for %s\n", moduleName, name));

		net_device_module_info* module;
		if (get_module(moduleName, (module_info**)&module) == B_OK) {
			net_device* device;
			status_t status = module->init_device(name, &device);
			if (status == B_OK) {
				interface = allocate_device_interface(device, module);
				if (interface != NULL) {
					close_module_list(cookie);
					return interface;
				}

				module->uninit_device(device);
			}
			put_module(moduleName);
		}
	}

	close_module_list(cookie);

	return NULL;
}


/*!	Feeds the device monitors of the \a interface with the specified \a buffer.
	You might want to check interface::monitor_count before calling this
	function for optimization.
*/
void
device_interface_monitor_receive(net_device_interface* interface,
	net_buffer* buffer)
{
	RecursiveLocker locker(interface->monitor_lock);

	DeviceMonitorList::Iterator iterator
		= interface->monitor_funcs.GetIterator();
	while (iterator.HasNext()) {
		net_device_monitor* monitor = iterator.Next();
		monitor->receive(monitor, buffer);
	}
}


/*!	Tears down the extra receive queues (indices 1..receive_queue_count-1) of a
	multiqueue interface, freeing the array and restoring queue 0's fifo to the
	single-queue budget. A no-op for a single-queue interface (queues == NULL).

	Every extra queue must have a live fifo and thread ids that are either -1 or
	a resumed thread (never a still-suspended one, which would never exit and so
	could not be joined). Callers on the up() unwind path resume what they
	spawned before calling here; on down() the threads are already running and
	stop because IFF_UP is cleared / the driver fd is closed (readers) and the
	fifo notify sem is deleted below (consumers).
*/
static void
teardown_extra_receive_queues(net_device_interface* interface)
{
	if (interface->queues == NULL) {
		interface->receive_queue_count = 1;
		return;
	}

	const uint32 count = interface->receive_queue_count;

	// Tell every extra consumer to stop *before* destroying any fifo. down()
	// does not drop ref_count (it is not the final put), so a consumer that is
	// mid-dispatch would otherwise loop back and re-lock a fifo mutex we are
	// about to destroy. With the flag set first, such a consumer exits at the
	// top of its loop; the fifo's sem-delete below only handles the separate
	// case of a consumer already blocked in dequeue.
	for (uint32 i = 1; i < count; i++)
		atomic_set(&interface->queues[i - 1].stopping, 1);

	// Readers exit on their own (IFF_UP clear or fd closed); join them. We may
	// be one of them only for queue 0, never for an extra queue, but guard
	// anyway.
	thread_id self = find_thread(NULL);
	for (uint32 i = 1; i < count; i++) {
		net_device_interface_queue* queue = &interface->queues[i - 1];
		if (queue->reader_thread >= 0 && queue->reader_thread != self)
			wait_for_thread(queue->reader_thread, NULL);
	}

	// Deleting each fifo's notify sem wakes a consumer blocked in dequeue so it
	// returns an error and exits; a consumer that was instead mid-dispatch has
	// already seen the stopping flag above. Joining the consumer here is only
	// deadlock-free because no receive-dispatch path acquires receive_lock (the
	// caller holds it): a multiqueue consumer takes receive_handlers_lock(read)
	// for the handler walk and nothing else. A future handler that took
	// receive_lock would deadlock here -- keep that invariant.
	for (uint32 i = 1; i < count; i++) {
		net_device_interface_queue* queue = &interface->queues[i - 1];
		uninit_fifo(&queue->receive_queue);
		if (queue->consumer_thread >= 0 && queue->consumer_thread != self)
			wait_for_thread(queue->consumer_thread, NULL);
	}

	delete[] interface->queues;
	interface->queues = NULL;
	interface->receive_queue_count = 1;

	set_fifo_max_bytes(&interface->receive_queue, 16 * 1024 * 1024);
	interface->receive_queue_diagnostics.limit_bytes = 16 * 1024 * 1024;
}


status_t
up_device_interface(net_device_interface* interface)
{
	net_device* device = interface->device;

	RecursiveLocker locker(interface->receive_lock);

	if (interface->up_count != 0) {
		interface->up_count++;
		return B_OK;
	}

	status_t status = device->module->up(device);
	if (status != B_OK)
		return status;

	// Decide how many receive queues to actually drain. A device that does not
	// answer the multiqueue contract (receive_data_queue == NULL) or reports at
	// most one queue keeps the single-queue path below, textually unchanged.
	uint32 m = 1;
	if (device->module->receive_data_queue != NULL
		&& device->rx_queue_count > 1) {
		m = device->rx_queue_count;
		if (m > NET_STACK_MAX_RX_QUEUES)
			m = NET_STACK_MAX_RX_QUEUES;
		uint32 cpus = (uint32)smp_get_num_cpus();
		if (m > cpus)
			m = cpus;
		if (m > 1 && device->module->set_rx_queue_count(device, m) != B_OK)
			m = 1;
	}

	// Allocate and initialize the extra queues (1..m-1); queue 0 keeps the
	// legacy fields. Any allocation failure here falls back to a single queue
	// -- a working single-queue interface beats a failed "ifconfig up".
	if (m > 1) {
		interface->queues
			= new(std::nothrow) net_device_interface_queue[m - 1];
		if (interface->queues == NULL) {
			m = 1;
		} else {
			uint32 built = 0;
			for (uint32 i = 1; i < m; i++) {
				net_device_interface_queue* queue = &interface->queues[i - 1];
				queue->interface = interface;
				queue->index = i;
				queue->reader_thread = -1;
				queue->consumer_thread = -1;
				queue->receive_deframe_dropped = 0;
				queue->receive_enqueue_dropped = 0;
				queue->stopping = 0;

				char name[128];
				snprintf(name, sizeof(name), "%s receive queue %" B_PRIu32,
					device->name, i);
				if (init_fifo(&queue->receive_queue, name,
						NET_STACK_RX_QUEUE_FIFO_LIMIT) < B_OK)
					break;
				init_fifo_watermark(&queue->receive_queue_diagnostics,
					queue->receive_queue.max_bytes);
				built++;
			}

			if (built != m - 1) {
				// A fifo failed to init; no threads exist yet, so just drop the
				// ones built and fall back to one queue.
				for (uint32 i = 0; i < built; i++)
					uninit_fifo(&interface->queues[i].receive_queue);
				delete[] interface->queues;
				interface->queues = NULL;
				device->module->set_rx_queue_count(device, 1);
				m = 1;
			}
		}
	}

	interface->receive_queue_count = m;

	if (m > 1) {
		// Queue 0 now shares the buffering budget with the extra queues, so
		// shrink its fifo to the per-queue limit (D28); teardown restores it.
		set_fifo_max_bytes(&interface->receive_queue,
			NET_STACK_RX_QUEUE_FIFO_LIMIT);
		interface->receive_queue_diagnostics.limit_bytes
			= NET_STACK_RX_QUEUE_FIFO_LIMIT;
	}

	if (device->module->receive_data != NULL) {
		// give the thread a nice name
		char name[B_OS_NAME_LENGTH];
		snprintf(name, sizeof(name), "%s reader", device->name);

		interface->reader_thread = spawn_kernel_thread(device_reader_thread,
			name, B_REAL_TIME_DISPLAY_PRIORITY - 10, interface);
		if (interface->reader_thread < B_OK) {
			status = interface->reader_thread;
			if (m > 1) {
				teardown_extra_receive_queues(interface);
				device->module->set_rx_queue_count(device, 1);
			}
			return status;
		}
	}

	if (m > 1) {
		// Spawn a reader and consumer for each extra queue, suspended so they
		// can be pinned before they run.
		bool ok = true;
		for (uint32 i = 1; i < m && ok; i++) {
			net_device_interface_queue* queue = &interface->queues[i - 1];
			char name[B_OS_NAME_LENGTH];

			snprintf(name, sizeof(name), "%s reader %" B_PRIu32,
				device->name, i);
			queue->reader_thread = spawn_kernel_thread(
				device_queue_reader_thread, name,
				B_REAL_TIME_DISPLAY_PRIORITY - 10, queue);
			if (queue->reader_thread < B_OK) {
				queue->reader_thread = -1;
				ok = false;
				break;
			}

			snprintf(name, sizeof(name), "%s consumer %" B_PRIu32,
				device->name, i);
			queue->consumer_thread = spawn_kernel_thread(
				device_queue_consumer_thread, name, B_DISPLAY_PRIORITY, queue);
			if (queue->consumer_thread < B_OK) {
				queue->consumer_thread = -1;
				ok = false;
				break;
			}
		}

		if (!ok) {
			// A per-queue thread failed to spawn. Resume whatever we spawned so
			// it can exit -- IFF_UP is still clear, so a resumed reader returns
			// at once, and teardown's fifo-sem delete releases each consumer --
			// then fall back to a single queue.
			for (uint32 i = 1; i < m; i++) {
				net_device_interface_queue* queue = &interface->queues[i - 1];
				if (queue->reader_thread >= 0)
					resume_thread(queue->reader_thread);
				if (queue->consumer_thread >= 0)
					resume_thread(queue->consumer_thread);
			}
			teardown_extra_receive_queues(interface);
			device->module->set_rx_queue_count(device, 1);
			m = 1;
		}
	}

	if (m > 1) {
		// Pin each queue's threads to the CPU its interrupt targets, so the
		// interrupt, reader and consumer for a flow all land on one CPU.
		// Best-effort (D7b): on any error the thread simply runs unpinned.
		int32 cpuCount = smp_get_num_cpus();
		int32 cpu0 = device->module->get_rx_queue_cpu != NULL
			? device->module->get_rx_queue_cpu(device, 0) : -1;
		if (cpu0 < 0)
			cpu0 = 0;
		if (interface->reader_thread >= 0)
			scheduler_pin_thread_to_cpu(interface->reader_thread, cpu0);

		for (uint32 i = 1; i < m; i++) {
			net_device_interface_queue* queue = &interface->queues[i - 1];
			int32 cpu = device->module->get_rx_queue_cpu != NULL
				? device->module->get_rx_queue_cpu(device, i) : -1;
			if (cpu < 0)
				cpu = (int32)(i % (uint32)cpuCount);
			scheduler_pin_thread_to_cpu(queue->reader_thread, cpu);
			scheduler_pin_thread_to_cpu(queue->consumer_thread, cpu);
		}
	}

	device->flags |= IFF_UP;

	if (device->module->receive_data != NULL)
		resume_thread(interface->reader_thread);

	if (m > 1) {
		for (uint32 i = 1; i < m; i++) {
			net_device_interface_queue* queue = &interface->queues[i - 1];
			resume_thread(queue->reader_thread);
			resume_thread(queue->consumer_thread);
		}
	}

	interface->up_count = 1;
	return B_OK;
}


void
down_device_interface(net_device_interface* interface)
{
	// Receive lock must be held when calling down_device_interface.
	// Known callers are `interface_protocol_down' which gets
	// here via one of the following paths:
	//
	// - Interface::Control()
	//    Interface::SetDown()
	//     interface_protocol_down()
	//
	// - datalink_control()
	//    remove_interface()
	//     Interface::SetDown() etc.
	//
	// - device_removed()
	//    interface_removed_device_interface()
	//     remove_interface() etc.

	net_device* device = interface->device;

	device->flags &= ~IFF_UP;
	device->module->down(device);

	notify_device_monitors(interface, B_DEVICE_GOING_DOWN);

	if (device->module->receive_data != NULL) {
		thread_id readerThread = interface->reader_thread;

		// make sure the reader thread is gone before shutting down the interface
		// (note that we may be the reader thread)
		status_t status;
		wait_for_thread(readerThread, &status);
	}

	// Stop and free the extra receive queues (multiqueue only). down() above
	// closed the driver fd, so each extra reader's in-flight receive returns an
	// error and, with IFF_UP cleared, the reader exits; teardown then joins the
	// readers, deletes each extra fifo (releasing its consumer), and restores
	// queue 0's fifo to the single-queue budget. No-op for a single queue.
	teardown_extra_receive_queues(interface);
}


//	#pragma mark - devices stack API


/*!	Unregisters a previously registered deframer function. */
status_t
unregister_device_deframer(net_device* device)
{
	MutexLocker locker(sLock);

	// find device interface for this device
	net_device_interface* interface = find_device_interface(device->name);
	if (interface == NULL)
		return B_DEVICE_NOT_FOUND;

	RecursiveLocker _(interface->receive_lock);
	WriteLocker handlersLocker(interface->receive_handlers_lock);

	if (--interface->deframe_ref_count == 0)
		interface->deframe_func = NULL;

	return B_OK;
}


/*!	Registers the deframer function for the specified \a device.
	Note, however, that right now, you can only register one single
	deframer function per device.

	If the need arises, we might want to lift that limitation at a
	later time (which would require a slight API change, though).
*/
status_t
register_device_deframer(net_device* device, net_deframe_func deframeFunc)
{
	MutexLocker locker(sLock);

	// find device interface for this device
	net_device_interface* interface = find_device_interface(device->name);
	if (interface == NULL)
		return B_DEVICE_NOT_FOUND;

	RecursiveLocker _(interface->receive_lock);
	WriteLocker handlersLocker(interface->receive_handlers_lock);

	if (interface->deframe_func != NULL
		&& interface->deframe_func != deframeFunc)
		return B_ERROR;

	interface->deframe_func = deframeFunc;
	interface->deframe_ref_count++;
	return B_OK;
}


/*!	Registers a domain to receive net_buffers from the specified \a device. */
status_t
register_domain_device_handler(struct net_device* device, int32 type,
	struct net_domain* _domain)
{
	net_domain_private* domain = (net_domain_private*)_domain;
	if (domain->module == NULL || domain->module->receive_data == NULL)
		return B_BAD_VALUE;

	return register_device_handler(device, type, &domain_receive_adapter,
		domain);
}


/*!	Registers a receiving function callback for the specified \a device. */
status_t
register_device_handler(struct net_device* device, int32 type,
	net_receive_func receiveFunc, void* cookie)
{
	MutexLocker locker(sLock);

	// find device interface for this device
	net_device_interface* interface = find_device_interface(device->name);
	if (interface == NULL)
		return B_DEVICE_NOT_FOUND;

	RecursiveLocker _(interface->receive_lock);
	WriteLocker handlersLocker(interface->receive_handlers_lock);

	// see if such a handler already for this device

	DeviceHandlerList::Iterator iterator
		= interface->receive_funcs.GetIterator();
	while (net_device_handler* handler = iterator.Next()) {
		if (handler->type == type)
			return B_ERROR;
	}

	// Add new handler

	net_device_handler* handler = new(std::nothrow) net_device_handler;
	if (handler == NULL)
		return B_NO_MEMORY;

	handler->func = receiveFunc;
	handler->type = type;
	handler->cookie = cookie;
	interface->receive_funcs.Add(handler);
	return B_OK;
}


/*!	Unregisters a previously registered device handler. */
status_t
unregister_device_handler(struct net_device* device, int32 type)
{
	MutexLocker locker(sLock);

	// find device interface for this device
	net_device_interface* interface = find_device_interface(device->name);
	if (interface == NULL)
		return B_DEVICE_NOT_FOUND;

	RecursiveLocker _(interface->receive_lock);
	WriteLocker handlersLocker(interface->receive_handlers_lock);

	// search for the handler

	DeviceHandlerList::Iterator iterator
		= interface->receive_funcs.GetIterator();
	while (net_device_handler* handler = iterator.Next()) {
		if (handler->type == type) {
			// found it
			iterator.Remove();
			delete handler;
			return B_OK;
		}
	}

	return B_BAD_VALUE;
}


/*!	Registers a device monitor for the specified device. */
status_t
register_device_monitor(net_device* device, net_device_monitor* monitor)
{
	if (monitor->receive == NULL || monitor->event == NULL)
		return B_BAD_VALUE;

	MutexLocker locker(sLock);

	// find device interface for this device
	net_device_interface* interface = find_device_interface(device->name);
	if (interface == NULL)
		return B_DEVICE_NOT_FOUND;

	RecursiveLocker monitorLocker(interface->monitor_lock);
	interface->monitor_funcs.Add(monitor);
	atomic_add(&interface->monitor_count, 1);

	return B_OK;
}


/*!	Unregisters a previously registered device monitor. */
status_t
unregister_device_monitor(net_device* device, net_device_monitor* monitor)
{
	MutexLocker locker(sLock);

	// find device interface for this device
	net_device_interface* interface = find_device_interface(device->name);
	if (interface == NULL)
		return B_DEVICE_NOT_FOUND;

	RecursiveLocker monitorLocker(interface->monitor_lock);

	// search for the monitor

	DeviceMonitorList::Iterator iterator
		= interface->monitor_funcs.GetIterator();
	while (iterator.HasNext()) {
		if (iterator.Next() == monitor) {
			iterator.Remove();
			atomic_add(&interface->monitor_count, -1);
			return B_OK;
		}
	}

	return B_BAD_VALUE;
}


/*!	This function is called by device modules in case their link
	state changed, ie. if an ethernet cable was plugged in or
	removed.
*/
status_t
device_link_changed(net_device* device)
{
	notify_link_changed(device);
	return B_OK;
}


/*!	This function is called by device modules once their device got
	physically removed, ie. a USB networking card is unplugged.
*/
status_t
device_removed(net_device* device)
{
	MutexLocker locker(sLock);

	net_device_interface* interface = find_device_interface(device->name);
	if (interface == NULL)
		return B_DEVICE_NOT_FOUND;
	if (interface->busy)
		return B_BUSY;

	// Acquire a reference to the device interface being removed
	// so our put_() will (eventually) do the final cleanup
	atomic_add(&interface->ref_count, 1);
	interface->busy = true;
	locker.Unlock();

	// Propagate the loss of the device throughout the stack.

	interface_removed_device_interface(interface);
	notify_device_monitors(interface, B_DEVICE_BEING_REMOVED);

	// By now all of the monitors must have removed themselves. If they
	// didn't, they'll probably wait forever to be callback'ed again.
	RecursiveLocker monitorLocker(interface->monitor_lock);
	interface->monitor_funcs.RemoveAll();
	monitorLocker.Unlock();

	// All of the readers should be gone as well since we are out of
	// interfaces and put_domain_datalink_protocols() is called for
	// each delete_interface().

	put_device_interface(interface);

	return B_OK;
}


status_t
device_enqueue_buffer(net_device* device, net_buffer* buffer)
{
	net_device_interface* interface = get_device_interface(device->index);
	if (interface == NULL)
		return B_DEVICE_NOT_FOUND;

	status_t status = interface->deframe_func(interface->device, buffer);
	if (status != B_OK) {
		gNetBufferModule.free(buffer);
		return status;
	}

	status = fifo_enqueue_buffer_tracked(&interface->receive_queue, buffer,
		&interface->receive_queue_diagnostics);

	put_device_interface(interface);
	return status;
}


/*!	Prints the receive queue drop attribution and occupancy for \a interface.

	Every line carries the instrumentation version, so a measurement can never be
	attributed to the wrong build, and a single prefix so the whole block can be
	pulled out of the log with one grep.

	This is deliberately a few lines emitted on demand rather than anything
	per-frame: on this platform the serial console does not use its FIFO, so a
	line of output costs on the order of a millisecond and a per-frame print would
	create the very stalls it was meant to observe.
*/
void
dump_receive_queue_diagnostics(net_device_interface* interface)
{
	net_fifo_watermark diagnostics;
	snapshot_fifo_watermark(&interface->receive_queue,
		&interface->receive_queue_diagnostics, &diagnostics, true);

	const char* name = interface->device->name;
	const uint64 deframeDropped = interface->receive_deframe_dropped;
	const uint64 enqueueDropped = interface->receive_enqueue_dropped;

	dprintf(NET_RX_DIAG_VERSION " %s drops total=%" B_PRIu64 " deframe=%" B_PRIu64
		" enqueue=%" B_PRIu64 "\n", name, deframeDropped + enqueueDropped,
		deframeDropped, enqueueDropped);

	dprintf(NET_RX_DIAG_VERSION " %s enqfail total=%" B_PRIu64 " nobufs=%" B_PRIu64
		" other=%" B_PRIu64 " depth=%" B_PRIuSIZE "..%" B_PRIuSIZE "\n", name,
		diagnostics.fail_total, diagnostics.fail_nobufs, diagnostics.fail_other,
		diagnostics.fail_bytes_min == (size_t)-1 ? 0 : diagnostics.fail_bytes_min,
		diagnostics.fail_bytes_max);

	dprintf(NET_RX_DIAG_VERSION " %s queue limit=%" B_PRIuSIZE " cur=%" B_PRIuSIZE
		" curpkts=%" B_PRIu32 " peak=%" B_PRIuSIZE " peakpkts=%" B_PRIu32
		" enq=%" B_PRIu64 " deq=%" B_PRIu64 "\n", name, diagnostics.limit_bytes,
		diagnostics.current_bytes, diagnostics.current_packets,
		diagnostics.peak_bytes, diagnostics.peak_packets, diagnostics.enqueued,
		diagnostics.dequeued);

	// Extra receive queues of a multiqueue interface (queue 0 is the block
	// above). Each line names the queue so it can be told apart in the log.
	for (uint32 i = 1; i < interface->receive_queue_count; i++) {
		net_device_interface_queue* queue = &interface->queues[i - 1];
		net_fifo_watermark queueDiag;
		snapshot_fifo_watermark(&queue->receive_queue,
			&queue->receive_queue_diagnostics, &queueDiag, true);

		dprintf(NET_RX_DIAG_VERSION " %s.q%" B_PRIu32 " drops total=%" B_PRIu64
			" deframe=%" B_PRIu64 " enqueue=%" B_PRIu64 "\n", name, i,
			queue->receive_deframe_dropped + queue->receive_enqueue_dropped,
			queue->receive_deframe_dropped, queue->receive_enqueue_dropped);

		dprintf(NET_RX_DIAG_VERSION " %s.q%" B_PRIu32 " queue limit=%" B_PRIuSIZE
			" cur=%" B_PRIuSIZE " curpkts=%" B_PRIu32 " peak=%" B_PRIuSIZE
			" peakpkts=%" B_PRIu32 " enq=%" B_PRIu64 " deq=%" B_PRIu64 "\n",
			name, i, queueDiag.limit_bytes, queueDiag.current_bytes,
			queueDiag.current_packets, queueDiag.peak_bytes,
			queueDiag.peak_packets, queueDiag.enqueued, queueDiag.dequeued);
	}

	// receive.errors is a third and separate bucket - receive_data() failing in
	// the driver - and is deliberately not folded into either of the above.
	dprintf(NET_RX_DIAG_VERSION " %s packets=%" B_PRIu32 " errors=%" B_PRIu32 "\n",
		name, interface->device->stats.receive.packets,
		interface->device->stats.receive.errors);
}


//	#pragma mark -


status_t
init_device_interfaces()
{
	mutex_init(&sLock, "net device interfaces");

	new (&sInterfaces) DeviceInterfaceList;
		// static C++ objects are not initialized in the module startup

	dprintf("network/stack: " NET_RX_DIAG_VERSION " receive drop attribution "
		"active\n");
		// Announce the instrumentation build. A hot-swapped module that did not
		// load looks exactly like a change that had no effect, so every number
		// taken from this build is gated on this line appearing.

#if ENABLE_DEBUGGER_COMMANDS
	add_debugger_command("net_device_interface", &dump_device_interface,
		"Dump the given network device interface");
	add_debugger_command("net_device_interfaces", &dump_device_interfaces,
		"Dump network device interfaces");
#endif
	return B_OK;
}


status_t
uninit_device_interfaces()
{
#if ENABLE_DEBUGGER_COMMANDS
	remove_debugger_command("net_device_interface", &dump_device_interface);
	remove_debugger_command("net_device_interfaces", &dump_device_interfaces);
#endif

	mutex_destroy(&sLock);
	return B_OK;
}

