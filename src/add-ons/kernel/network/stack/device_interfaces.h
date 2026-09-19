/*
 * Copyright 2006-2017, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */
#ifndef DEVICE_INTERFACES_H
#define DEVICE_INTERFACES_H


#include <net_datalink.h>
#include <net_stack.h>

#include <util/DoublyLinkedList.h>

#include "utility.h"


struct net_device_handler : DoublyLinkedListLinkImpl<net_device_handler> {
	net_receive_func	func;
	int32				type;
	void*				cookie;
};

typedef DoublyLinkedList<net_device_handler> DeviceHandlerList;

typedef DoublyLinkedList<net_device_monitor,
	DoublyLinkedListCLink<net_device_monitor> > DeviceMonitorList;

#define NET_STACK_MAX_RX_QUEUES		8	// D9
#define NET_STACK_RX_QUEUE_FIFO_LIMIT	(256 * 1024)	// D28


struct net_device_interface;


// Per-queue receive context for a multiqueue interface. Queue 0 does *not* use
// one of these -- it keeps the legacy fields in net_device_interface below, so
// that a single-queue interface runs literally the same code it always has.
// These describe queues 1..receive_queue_count-1 only.
struct net_device_interface_queue {
	net_device_interface*	interface;
	uint32				index;			// 1..receive_queue_count-1
	thread_id			reader_thread;
	thread_id			consumer_thread;
	net_fifo			receive_queue;
	net_fifo_watermark	receive_queue_diagnostics;
	uint64				receive_deframe_dropped;
	uint64				receive_enqueue_dropped;

	// Stop signal for this queue's consumer, checked in its loop guard. down()
	// does not drop ref_count (it is not the final put), so the consumer needs
	// its own flag to exit before it can loop back and touch a destroyed fifo;
	// deleting the fifo's notify sem alone only wakes a currently-blocked one.
	// Accessed with atomics (set by teardown, read by the consumer).
	int32				stopping;
};


// Lock order (see D40):
//   sLock (module list)  ->  receive_lock  ->  receive_handlers_lock(write)
//   consumer (multiqueue): receive_handlers_lock(read) -> [datalink/domain/TCP]
//   consumer (single-queue): receive_lock -> [datalink/domain/TCP]
//   reader: monitor_lock | fifo->lock  (leaf locks, never nested with the above)
struct net_device_interface : DoublyLinkedListLinkImpl<net_device_interface> {
	struct net_device*	device;
	thread_id			reader_thread;
	uint32				up_count;
		// a device can be brought up by more than one interface
	int32				ref_count;
	bool				busy;

	net_deframe_func	deframe_func;
	int32				deframe_ref_count;

	int32				monitor_count;
	recursive_lock		monitor_lock;
	DeviceMonitorList	monitor_funcs;

	DeviceHandlerList	receive_funcs;
	recursive_lock		receive_lock;

	thread_id			consumer_thread;
	net_fifo			receive_queue;

	// Receive-drop attribution. net_device::stats.receive.dropped is a single
	// counter incremented from two unrelated failures in the reader thread, so
	// on its own it cannot say which one is happening. These split it; their sum
	// is the aggregate, which keeps its existing meaning for every reader.
	uint64				receive_deframe_dropped;
	uint64				receive_enqueue_dropped;

	// Occupancy instrumentation for receive_queue. See utility.h; this is what
	// lets the fifo limit being reached be shown rather than inferred from a
	// drop count.
	net_fifo_watermark	receive_queue_diagnostics;

	// Multiqueue receive (all zero/NULL for a single-queue device).
	// receive_queue_count is the number of queues being drained (m); queues[]
	// holds queues 1..m-1 -- queue 0 lives in the legacy fields above so that
	// the single-queue path is not merely equivalent but the same code.
	uint32				receive_queue_count;
	net_device_interface_queue*	queues;		// array of count-1, or NULL

	// Serializes handler-list mutation against multiqueue dispatch; see D30.
	// Mutators take receive_lock then this (write); multiqueue consumers take
	// only this (read). The single-queue consumer never touches it and keeps
	// its receive_lock exclusion unchanged.
	rw_lock				receive_handlers_lock;

	// Sojourn-time queue discipline for receive_queue. Bounds the queue by the
	// time a frame spends in it rather than only by bytes, so a fast link fed
	// past the consumer's drain rate does not build the standing multi-millisecond
	// queue a pure byte cap allows. See utility.h.
	net_fifo_codel		receive_queue_codel;
};

typedef DoublyLinkedList<net_device_interface> DeviceInterfaceList;


// device interfaces
net_device_interface* acquire_device_interface(net_device_interface* interface);
void get_device_interface_address(net_device_interface* interface,
	sockaddr* address);
uint32 count_device_interfaces();
status_t list_device_interfaces(void* buffer, size_t* _bufferSize);
void put_device_interface(struct net_device_interface* interface);
struct net_device_interface* get_device_interface(uint32 index);
struct net_device_interface* get_device_interface(const char* name,
	bool create = true);
void device_interface_monitor_receive(net_device_interface* interface,
	net_buffer* buffer);
status_t up_device_interface(net_device_interface* interface);
void down_device_interface(net_device_interface* interface);

// devices
status_t unregister_device_deframer(net_device* device);
status_t register_device_deframer(net_device* device,
	net_deframe_func deframeFunc);
status_t register_domain_device_handler(struct net_device* device, int32 type,
	struct net_domain* domain);
status_t register_device_handler(struct net_device* device, int32 type,
	net_receive_func receiveFunc, void* cookie);
status_t unregister_device_handler(struct net_device* device, int32 type);
status_t register_device_monitor(struct net_device* device,
	struct net_device_monitor* monitor);
status_t unregister_device_monitor(struct net_device* device,
	struct net_device_monitor* monitor);
status_t device_link_changed(net_device* device);
status_t device_removed(net_device* device);
status_t device_enqueue_buffer(net_device* device, net_buffer* buffer);
void dump_receive_queue_diagnostics(net_device_interface* interface);

status_t init_device_interfaces();
status_t uninit_device_interfaces();


#endif	// DEVICE_INTERFACES_H
