/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NET_BUFFER_H
#define NET_BUFFER_H


#include <sys/socket.h>

#include <module.h>

#include <util/list.h>


#define NET_BUFFER_MODULE_NAME "network/stack/buffer/v1"


enum net_buffer_flags {
	// Receive: this checksum has already been verified -- by hardware, or by a
	// layer below -- and the protocol may skip verifying it again.
	NET_BUFFER_L3_CHECKSUM_VALID	= (1 << 0),
	NET_BUFFER_L4_CHECKSUM_VALID	= (1 << 1),

	// Transmit: this checksum is deliberately *unfinished* and the device is
	// expected to complete it. The field in the packet holds the folded,
	// uncomplemented one's-complement sum of the pseudo-header only -- the same
	// convention as Linux's CHECKSUM_PARTIAL -- so a device that sums the
	// remaining bytes, folds and complements produces the right answer.
	//
	// Only ever set for a device that advertised the corresponding bit in
	// net_device::tx_checksum_offload (see net_device.h), because a packet that
	// reaches the wire with one of these still set is a packet the peer will
	// discard. Anything that redirects such a buffer away from that device --
	// fragmentation, loopback re-injection -- has to finish the sum in software
	// and swap the flag for the _VALID one above.
	NET_BUFFER_L3_CHECKSUM_NEEDED	= (1 << 2),
	NET_BUFFER_L4_CHECKSUM_NEEDED	= (1 << 3),
};


typedef struct net_buffer {
	struct list_link		link;

	struct sockaddr*		source;
	struct sockaddr*		destination;
	struct net_interface_address* interface_address;
	union {
		struct {
			uint16			start;
			uint16			end;
		}					fragment;
		uint32				sequence;
		uint32				offset;
		uint32				index;
		int32				type;
	};
	uint32					msg_flags;
	uint32					size;
	uint8					protocol;
	uint16					buffer_flags;

	// Wall-clock time (system_time(), microseconds) at which the device reader
	// thread put this buffer onto a device interface's receive FIFO. The
	// consumer thread reads it back to compute how long the buffer sat in that
	// queue -- its sojourn -- which is the quantity the receive FIFO's queue
	// discipline bounds (see net_fifo_codel in the stack add-on). Zero means the
	// buffer was never enqueued on such a FIFO and carries no sojourn.
	//
	// Appended at the end of the structure on purpose: every field above keeps
	// its offset, so a module built against an older copy of this header still
	// reads all of them correctly, and only the allocating module (the stack
	// add-on's net_buffer.cpp) needs to know that the object grew.
	bigtime_t				receive_enqueue_time;
} net_buffer;

struct ancillary_data_container;

struct net_buffer_module_info {
	module_info info;

	net_buffer*		(*create)(size_t headerSpace);
	void 			(*free)(net_buffer* buffer);

	net_buffer*		(*duplicate)(net_buffer* from);
	net_buffer*		(*clone)(net_buffer* from, bool shareFreeSpace);
	net_buffer*		(*split)(net_buffer* from, uint32 offset);
	status_t		(*merge)(net_buffer* buffer, net_buffer* with, bool after);

	status_t		(*prepend_size)(net_buffer* buffer, size_t size,
						void** _contiguousBuffer);
	status_t		(*prepend)(net_buffer* buffer, const void* data,
						size_t bytes);
	status_t		(*append_size)(net_buffer* buffer, size_t size,
						void** _contiguousBuffer);
	status_t		(*append)(net_buffer* buffer, const void* data,
						size_t bytes);
	status_t		(*insert)(net_buffer* buffer, uint32 offset,
						const void* data, size_t bytes, uint32 flags);
	status_t		(*remove)(net_buffer* buffer, uint32 offset,
						size_t bytes);
	status_t		(*remove_header)(net_buffer* buffer, size_t bytes);
	status_t		(*remove_trailer)(net_buffer* buffer, size_t bytes);
	status_t		(*trim)(net_buffer* buffer, size_t newSize);
	status_t		(*append_cloned)(net_buffer* buffer, net_buffer* source,
						uint32 offset, size_t bytes);

	status_t		(*associate_data)(net_buffer* buffer, void* data);

	void			(*set_ancillary_data)(net_buffer* buffer,
						struct ancillary_data_container* container);
	struct ancillary_data_container* (*get_ancillary_data)(net_buffer* buffer);
	void*			(*transfer_ancillary_data)(net_buffer* from,
						net_buffer* to);

	status_t		(*store_header)(net_buffer* buffer);
	ssize_t			(*stored_header_length)(net_buffer* buffer);
	status_t		(*restore_header)(net_buffer* buffer, uint32 offset,
						void* data, size_t bytes);
	status_t		(*append_restored_header)(net_buffer* buffer,
						net_buffer* source, uint32 offset, size_t bytes);

	status_t		(*direct_access)(net_buffer* buffer, uint32 offset,
						size_t bytes, void** _data);
	status_t 		(*read)(net_buffer* buffer, size_t offset, void* data,
						size_t bytes);
	status_t		(*write)(net_buffer* buffer, size_t offset,
						const void* data, size_t bytes);

	int32			(*checksum)(net_buffer* buffer, uint32 offset, size_t bytes,
						bool finalize);
	status_t		(*get_memory_map)(net_buffer* buffer,
						struct iovec* iovecs, uint32 vecCount);
	uint32 			(*get_iovecs)(net_buffer* buffer,
						struct iovec* iovecs, uint32 vecCount);
	uint32 			(*count_iovecs)(net_buffer* buffer);

	void			(*swap_addresses)(net_buffer* buffer);

	void			(*dump)(net_buffer* buffer);
};


#endif	// NET_BUFFER_H
