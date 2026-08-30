/*
 * Copyright 2007, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ETHER_DRIVER_H
#define _ETHER_DRIVER_H

/*! Standard ethernet driver interface */


#include <Drivers.h>


/* ioctl() opcodes a driver should support */
enum {
	ETHER_GETADDR = B_DEVICE_OP_CODES_END,
		/* get ethernet address (required) */
	ETHER_INIT,								/* (obsolete) */
	ETHER_NONBLOCK,							/* change non blocking mode (int *) */
	ETHER_ADDMULTI,							/* add multicast address */
	ETHER_REMMULTI,							/* remove multicast address */
	ETHER_SETPROMISC,						/* set promiscuous mode (int *) */
	ETHER_GETFRAMESIZE,						/* get frame size (required) (int *) */
	ETHER_SET_LINK_STATE_SEM,
		/* pass over a semaphore to release on link state changes (sem_id *) */
	ETHER_GET_LINK_STATE,
		/* get line speed, quality, duplex mode, etc. (ether_link_state_t *) */

	ETHER_SEND_NET_BUFFER,					/* send a net_buffer */
	ETHER_RECEIVE_NET_BUFFER,				/* receive a net_buffer */

	ETHER_GET_TX_CHECKSUM_OFFLOAD,
		/* which checksums the device will compute on transmit (uint32 *),
		   as net_device_tx_checksum bits -- see net_device.h. Optional: a
		   driver that does not implement it offloads nothing, which is the
		   safe answer and the one every existing driver gives by failing the
		   call. Append new opcodes here and nowhere else: the values are
		   positional and shared with out-of-tree drivers. */

	ETHER_GET_RX_QUEUE_COUNT,
		/* uint32*: receive queues the driver created (out). Optional; a
		   driver that fails the call is single-queue. */
	ETHER_SET_RX_QUEUE_COUNT,
		/* uint32*: how many receive queues the caller will drain (in).
		   The driver re-spreads its receive steering (RSS) across exactly
		   that many and confirms with B_OK. */
	ETHER_GET_RX_QUEUE_CPU,
		/* ether_queue_cpu_args*: CPU the queue's interrupt targets. */
	ETHER_RECEIVE_NET_BUFFER_QUEUE,
		/* ether_receive_queue_args*: as ETHER_RECEIVE_NET_BUFFER, from one
		   specific queue. */
};


/* ETHER_GETADDR - MAC address */
typedef struct ether_address {
	uint8	ebyte[6];
} ether_address_t;

/* ETHER_GETLINKSTATE */
typedef struct ether_link_state {
	uint32	media;		/* as specified in net/if_media.h */
	uint32  quality;	/* in one tenth of a percent */
	uint64	speed;		/* in bit/s */
} ether_link_state_t;

/* ETHER_GET_RX_QUEUE_CPU */
typedef struct ether_queue_cpu_args {
	uint32	queue;		/* in */
	int32	cpu;		/* out; negative = unknown */
} ether_queue_cpu_args;

/* ETHER_RECEIVE_NET_BUFFER_QUEUE. buffer is a struct net_buffer* (kernel
   address), typed void* here because this header does not know net_buffer --
   exactly as ETHER_RECEIVE_NET_BUFFER already passes one untyped. Explicit
   padding keeps one layout on every architecture. */
typedef struct ether_receive_queue_args {
	uint32	queue;		/* in */
	uint32	_reserved;
	void*	buffer;		/* out */
} ether_receive_queue_args;

#endif	/* _ETHER_DRIVER_H */
