/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NET_DEVICE_H
#define NET_DEVICE_H


#include <net/if.h>

#include <module.h>


typedef struct net_buffer net_buffer;


struct net_hardware_address {
	uint8	data[64];
	uint8	length;
};


// What a device is willing to compute on transmit, advertised in
// net_device::tx_checksum_offload. A protocol that sees its bit here may leave
// the layer-4 checksum unfinished and set NET_BUFFER_L4_CHECKSUM_NEEDED on the
// buffer instead (see net_buffer.h for the exact convention). Split by address
// family because hardware routinely supports one and not the other -- the ENA
// device on Graviton advertises IPv4 only.
enum net_device_tx_checksum {
	NET_DEVICE_TX_CHECKSUM_IPV4_L4	= (1 << 0),
	NET_DEVICE_TX_CHECKSUM_IPV6_L4	= (1 << 1),
};

typedef struct net_device {
	struct net_device_module_info* module;

	char	name[IF_NAMESIZE];
	uint32	index;
	uint32	flags;		// IFF_LOOPBACK, ...
	uint32	type;		// IFT_ETHER, ...
	size_t	mtu;
	uint32	media;
	uint64	link_speed;
	uint32	link_quality;
	size_t	header_length;

	struct net_hardware_address address;

	struct ifreq_stats stats;

	// net_device_tx_checksum bits. Filled in by the device module while the
	// device is up; zero means "offload nothing", which is what every device
	// that does not know about this field gets.
	uint32	tx_checksum_offload;
} net_device;


struct net_device_module_info {
	struct module_info info;

	status_t	(*init_device)(const char* name, net_device** _device);
	status_t	(*uninit_device)(net_device* device);

	status_t	(*up)(net_device* device);
	void		(*down)(net_device* device);

	status_t	(*control)(net_device* device, int32 op, void* argument,
					size_t length);

	status_t	(*send_data)(net_device* device, net_buffer* buffer);
	status_t	(*receive_data)(net_device* device, net_buffer** _buffer);

	status_t	(*set_mtu)(net_device* device, size_t mtu);
	status_t	(*set_promiscuous)(net_device* device, bool promiscuous);
	status_t	(*set_media)(net_device* device, uint32 media);

	status_t	(*add_multicast)(net_device* device,
					const struct sockaddr* address);
	status_t	(*remove_multicast)(net_device* device,
					const struct sockaddr* address);
};


#endif	// NET_DEVICE_H
