/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NET_UTILITIES_H
#define NET_UTILITIES_H


#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include <net_buffer.h>
#include <net_datalink.h>


/*!	An IPv6 socket names an IPv4 peer with an IPv4-mapped address,
	::ffff:a.b.c.d (RFC 4291 section 2.5.5.2, RFC 3493 section 3.7). A
	dual-stack socket (IPV6_V6ONLY off) that carries IPv4 traffic keeps its
	addresses in plain IPv4 form inside the stack, so that the IPv4 domain can
	route, checksum and demultiplex them, and only presents them in the mapped
	form to the application. These convert between the two.
*/
static inline bool
is_ipv4_mapped_address(const sockaddr* address)
{
	return address != NULL && address->sa_family == AF_INET6
		&& address->sa_len >= sizeof(sockaddr_in6)
		&& IN6_IS_ADDR_V4MAPPED(&((const sockaddr_in6*)address)->sin6_addr);
}


static inline void
ipv4_address_from_mapped(const sockaddr* mapped, sockaddr_in* address)
{
	const sockaddr_in6* source = (const sockaddr_in6*)mapped;

	memset(address, 0, sizeof(sockaddr_in));
	address->sin_len = sizeof(sockaddr_in);
	address->sin_family = AF_INET;
	address->sin_port = source->sin6_port;
	memcpy(&address->sin_addr, &source->sin6_addr.s6_addr[12],
		sizeof(in_addr));
}


static inline void
mapped_address_from_ipv4(const sockaddr* ipv4, sockaddr_in6* mapped)
{
	const sockaddr_in* source = (const sockaddr_in*)ipv4;

	memset(mapped, 0, sizeof(sockaddr_in6));
	mapped->sin6_len = sizeof(sockaddr_in6);
	mapped->sin6_family = AF_INET6;
	mapped->sin6_port = source->sin_port;
	mapped->sin6_addr.s6_addr[10] = 0xff;
	mapped->sin6_addr.s6_addr[11] = 0xff;
	memcpy(&mapped->sin6_addr.s6_addr[12], &source->sin_addr,
		sizeof(in_addr));
}


class Checksum {
public:
	struct BufferHelper {
		BufferHelper(net_buffer* _buffer, net_buffer_module_info* _bufferModule)
			:
			buffer(_buffer),
			bufferModule(_bufferModule)
		{
		}
		
		net_buffer* buffer;
		net_buffer_module_info* bufferModule;
	};

	Checksum()
		:
		fSum(0)
	{
	}

	inline Checksum& operator<<(uint8 value)
	{
#if B_HOST_IS_LENDIAN
		fSum += value;
#else
		fSum += (uint16)value << 8;
#endif
		return *this;
	}

	inline Checksum& operator<<(uint16 value)
	{
		fSum += value;
		return *this;
	}

	inline Checksum& operator<<(uint32 value)
	{
		fSum += (value & 0xffff) + (value >> 16);
		return *this;
	}

	inline Checksum& operator<<(const BufferHelper& bufferHelper)
	{
		net_buffer* buffer = bufferHelper.buffer;
		fSum += bufferHelper.bufferModule->checksum(buffer, 0, buffer->size,
			false);
		return *this;
	}

	//! The folded sum, *not* complemented -- what a partial checksum carries.
	inline uint16 Sum()
	{
		while (fSum >> 16) {
			fSum = (fSum & 0xffff) + (fSum >> 16);
		}
		return (uint16)fSum;
	}

	inline operator uint16()
	{
		return (uint16)(Sum() ^ 0xFFFF);
	}

	static uint16 PseudoHeader(net_address_module_info* addressModule,
		net_buffer_module_info* bufferModule, net_buffer* buffer,
		uint16 protocol);
	static uint16 PartialPseudoHeader(net_address_module_info* addressModule,
		net_buffer* buffer, uint16 protocol);

private:
	uint32 fSum;
};


inline uint16
Checksum::PseudoHeader(net_address_module_info* addressModule,
	net_buffer_module_info* bufferModule, net_buffer* buffer, uint16 protocol)
{
	Checksum checksum;
	addressModule->checksum_address(&checksum, buffer->source);
	addressModule->checksum_address(&checksum, buffer->destination);
	checksum << (uint16)htons(protocol) << (uint16)htons(buffer->size)
		<< Checksum::BufferHelper(buffer, bufferModule);
	return checksum;
}


/*!	The pseudo-header half of a layer-4 checksum, and nothing else.

	Stored raw in the packet's checksum field it is a *partial* checksum: a
	device asked to finish the job sums the remaining bytes -- which includes
	this field -- folds and complements. This is the same convention as Linux's
	CHECKSUM_PARTIAL and as what ENA calls l4_csum_partial.

	Unlike PseudoHeader() this never walks the payload, so it costs a handful of
	adds regardless of how large the segment is. That difference is the entire
	point of transmit checksum offload.
*/
inline uint16
Checksum::PartialPseudoHeader(net_address_module_info* addressModule,
	net_buffer* buffer, uint16 protocol)
{
	Checksum checksum;
	addressModule->checksum_address(&checksum, buffer->source);
	addressModule->checksum_address(&checksum, buffer->destination);
	checksum << (uint16)htons(protocol) << (uint16)htons(buffer->size);
	return checksum.Sum();
}


/*!	Helper class that prints an address (and optionally a port) into a buffer
	that is automatically freed at end of scope.
*/
class AddressString {
public:
	AddressString(net_domain* domain, const sockaddr* address,
		bool printPort = false)
		:
		fBuffer(NULL)
	{
		domain->address_module->print_address(address, &fBuffer, printPort);
	}

	AddressString(net_domain* domain, const sockaddr& address,
		bool printPort = false)
		:
		fBuffer(NULL)
	{
		domain->address_module->print_address(&address, &fBuffer, printPort);
	}

	AddressString(net_address_module_info* address_module,
		const sockaddr* address, bool printPort = false)
		:
		fBuffer(NULL)
	{
		address_module->print_address(address, &fBuffer, printPort);
	}

	~AddressString()
	{
		free(fBuffer);
	}

	const char* Data() const
	{
		return fBuffer;
	}

private:
	char* fBuffer;
};


#endif	// NET_UTILITIES_H
