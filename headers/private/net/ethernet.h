/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ETHERNET_H
#define ETHERNET_H


#include <SupportDefs.h>


#define ETHER_ADDRESS_LENGTH	6
#define ETHER_CRC_LENGTH		4
#define ETHER_HEADER_LENGTH		14

#define ETHER_MIN_FRAME_SIZE	64
#define ETHER_MAX_FRAME_SIZE	1514
	// Standard ethernet: a 1500 byte payload plus ETHER_HEADER_LENGTH. This
	// keeps its historical value on purpose -- some callers use it to size
	// queues or as the default MTU of a device that negotiates nothing, and
	// those must not grow just because jumbo frames became possible.

// Ceiling on what a device may negotiate when it advertises more than
// standard ethernet via ETHER_GETFRAMESIZE. 9001 is the jumbo MTU offered by
// AWS ENA and is the largest we are prepared to allocate for on receive; a
// fixed ceiling keeps a misreporting driver from making the stack allocate
// unbounded amounts per packet.
#define ETHER_MAX_JUMBO_MTU			9001
#define ETHER_MAX_JUMBO_FRAME_SIZE	(ETHER_MAX_JUMBO_MTU + ETHER_HEADER_LENGTH)

struct ether_header {
	uint8	destination[ETHER_ADDRESS_LENGTH];
	uint8	source[ETHER_ADDRESS_LENGTH];
	uint16	type;
} _PACKED;


// ethernet types
#define ETHER_TYPE_IP				0x0800
#define ETHER_TYPE_ARP				0x0806
#define ETHER_TYPE_IPX				0x8137
#define	ETHER_TYPE_IPV6				0x86dd
#define	ETHER_TYPE_PPPOE_DISCOVERY	0x8863	// PPPoE discovery stage
#define	ETHER_TYPE_PPPOE			0x8864	// PPPoE session stage


#endif	// ETHERNET_H
