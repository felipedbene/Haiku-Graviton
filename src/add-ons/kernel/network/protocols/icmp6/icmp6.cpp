/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	RFC 4443 defines ICMPv6, including when an error message must not be
	sent (section 2.4).
*/


#include <net_datalink.h>
#include <net_protocol.h>
#include <net_stack.h>
#include <net_datalink_protocol.h>
#include <NetUtilities.h>
#include <NetBufferUtilities.h>

#include <KernelExport.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <util/list.h>

#include <algorithm>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <netinet/ip6.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include <ipv6_datagram/ndp.h>

#include "ErrorRateLimit.h"


//#define TRACE_ICMP6
#ifdef TRACE_ICMP6
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


typedef NetBufferField<uint16, offsetof(icmp6_hdr, icmp6_cksum)> ICMP6ChecksumField;


net_buffer_module_info *gBufferModule;
static net_stack_module_info *sStackModule;
static net_ndp_module_info *sIPv6NDPModule;
static net_datalink_module_info *sDatalinkModule;

// needed for IN6_IS_ADDR_UNSPECIFIED() macro
const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;


// The quoted invoking packet must leave the error within the minimum IPv6
// MTU (RFC 4443, section 2.4 (c)), so that it is never fragmented.
static const size_t kMaxQuotedLength = IPV6_MMTU - sizeof(ip6_hdr)
	- sizeof(icmp6_hdr);

static mutex sErrorRateLock = MUTEX_INITIALIZER("icmp6 error rate");
static ErrorRateLimit sErrorRateLimit;


static net_error
icmp6_to_net_error(uint8 type, uint8 code)
{
	switch (type) {
		case ICMP6_DST_UNREACH:
			switch (code) {
				case ICMP6_DST_UNREACH_NOROUTE:
					return B_NET_ERROR_UNREACH_NET;
				case ICMP6_DST_UNREACH_ADMIN:
					return B_NET_ERROR_UNREACH_HOST_PROHIBITED;
				case ICMP6_DST_UNREACH_BEYONDSCOPE:
				case ICMP6_DST_UNREACH_ADDR:
					return B_NET_ERROR_UNREACH_HOST;
				case ICMP6_DST_UNREACH_NOPORT:
					return B_NET_ERROR_UNREACH_PORT;
				case 5:
					// source address failed ingress/egress policy
					return B_NET_ERROR_UNREACH_FILTER_PROHIBITED;
				case 6:
					// reject route to destination
					return B_NET_ERROR_UNREACH_NET_PROHIBITED;
			}
			break;

		case ICMP6_TIME_EXCEEDED:
			switch (code) {
				case ICMP6_TIME_EXCEED_TRANSIT:
					return B_NET_ERROR_TRANSIT_TIME_EXCEEDED;
				case ICMP6_TIME_EXCEED_REASSEMBLY:
					return B_NET_ERROR_REASSEMBLY_TIME_EXCEEDED;
			}
			break;

		case ICMP6_PARAM_PROB:
			// An unrecognized next header is what IPv4 reports as an
			// unreachable protocol.
			if (code == ICMP6_PARAMPROB_NEXTHEADER)
				return B_NET_ERROR_UNREACH_PROTOCOL;
			return B_NET_ERROR_PARAMETER_PROBLEM;

		case ICMP6_PACKET_TOO_BIG:
			return B_NET_ERROR_MESSAGE_SIZE;

		default:
			break;
	}

	return (net_error)0;
}


/*!	Maps \a error to the ICMPv6 type and code that report it. Returns \c false
	for an error ICMPv6 has no message for, such as a redirect, which only a
	router sends, or a source quench, which RFC 4443 dropped.
*/
static bool
net_error_to_icmp6(net_error error, uint8& type, uint8& code)
{
	switch (error) {
		case B_NET_ERROR_UNREACH_NET:
		case B_NET_ERROR_UNREACH_NET_UNKNOWN:
			type = ICMP6_DST_UNREACH;
			code = ICMP6_DST_UNREACH_NOROUTE;
			return true;
		case B_NET_ERROR_UNREACH_HOST:
		case B_NET_ERROR_UNREACH_HOST_UNKNOWN:
			type = ICMP6_DST_UNREACH;
			code = ICMP6_DST_UNREACH_ADDR;
			return true;
		case B_NET_ERROR_UNREACH_NET_PROHIBITED:
		case B_NET_ERROR_UNREACH_HOST_PROHIBITED:
		case B_NET_ERROR_UNREACH_FILTER_PROHIBITED:
			type = ICMP6_DST_UNREACH;
			code = ICMP6_DST_UNREACH_ADMIN;
			return true;
		case B_NET_ERROR_UNREACH_PORT:
			type = ICMP6_DST_UNREACH;
			code = ICMP6_DST_UNREACH_NOPORT;
			return true;
		case B_NET_ERROR_UNREACH_PROTOCOL:
			type = ICMP6_PARAM_PROB;
			code = ICMP6_PARAMPROB_NEXTHEADER;
			return true;
		case B_NET_ERROR_MESSAGE_SIZE:
			type = ICMP6_PACKET_TOO_BIG;
			code = 0;
			return true;
		case B_NET_ERROR_TRANSIT_TIME_EXCEEDED:
			type = ICMP6_TIME_EXCEEDED;
			code = ICMP6_TIME_EXCEED_TRANSIT;
			return true;
		case B_NET_ERROR_REASSEMBLY_TIME_EXCEEDED:
			type = ICMP6_TIME_EXCEEDED;
			code = ICMP6_TIME_EXCEED_REASSEMBLY;
			return true;
		case B_NET_ERROR_PARAMETER_PROBLEM:
			type = ICMP6_PARAM_PROB;
			code = ICMP6_PARAMPROB_HEADER;
			return true;

		default:
			return false;
	}
}


/*!	Walks the extension headers of the invoking packet in \a buffer, which
	starts with \a header, and decides whether RFC 4443 lets an error be sent
	for it. It may not for an ICMPv6 error message (section 2.4 (e.1)). A
	non-initial fragment does not show which upper-layer protocol it belongs
	to, so like IPv4 (RFC 1122, 3.2.2) no error is sent for one. A
	reassembly timeout is only reported for a packet whose first fragment
	arrived (RFC 8200, section 4.5).
	\a _nextHeaderOffset is set to the offset of the next header field that
	names the upper-layer protocol, which an unrecognized next header error
	points to.
*/
static bool
is_error_allowed(net_buffer* buffer, const ip6_hdr& header, net_error error,
	size_t& _nextHeaderOffset)
{
	size_t offset = sizeof(ip6_hdr);
	uint8 next = header.ip6_nxt;
	bool fragment = false;
	_nextHeaderOffset = offsetof(ip6_hdr, ip6_nxt);

	// Bounded, so that a forged chain cannot keep us walking; a chain longer
	// than that is not one this node sends errors for.
	bool done = false;
	for (int32 i = 0; i < 16 && !done; i++) {
		if (next == IPPROTO_FRAGMENT) {
			ip6_frag fragmentHeader;
			if (gBufferModule->restore_header(buffer, offset, &fragmentHeader,
					sizeof(ip6_frag)) != B_OK
				|| (fragmentHeader.ip6f_offlg & IP6F_OFF_MASK) != 0)
				return false;

			fragment = true;
			next = fragmentHeader.ip6f_nxt;
			_nextHeaderOffset = offset;
			offset += sizeof(ip6_frag);
		} else if (next == IPPROTO_HOPOPTS || next == IPPROTO_ROUTING
			|| next == IPPROTO_DSTOPTS || next == IPPROTO_AH) {
			uint8 extension[2];
			if (gBufferModule->restore_header(buffer, offset, extension,
					sizeof(extension)) != B_OK)
				return false;

			_nextHeaderOffset = offset;
			if (next == IPPROTO_AH)
				offset += (extension[1] + 2) * 4;
			else
				offset += (extension[1] + 1) * 8;
			next = extension[0];
		} else
			done = true;
	}
	if (!done)
		return false;

	if (error == B_NET_ERROR_REASSEMBLY_TIME_EXCEEDED && !fragment)
		return false;

	if (next == IPPROTO_ICMPV6) {
		uint8 type;
		if (gBufferModule->restore_header(buffer, offset, &type, 1) != B_OK
			|| (type & ICMP6_INFOMSG_MASK) == 0)
			return false;
	}

	return true;
}


static bool
error_rate_allows()
{
	MutexLocker locker(sErrorRateLock);
	return sErrorRateLimit.Take(system_time());
}


net_protocol *
icmp6_init_protocol(net_socket *socket)
{
	net_protocol *protocol = new (std::nothrow) net_protocol;
	if (protocol == NULL)
		return NULL;

	return protocol;
}


status_t
icmp6_uninit_protocol(net_protocol *protocol)
{
	delete protocol;
	return B_OK;
}


status_t
icmp6_open(net_protocol *protocol)
{
	return B_OK;
}


status_t
icmp6_close(net_protocol *protocol)
{
	return B_OK;
}


status_t
icmp6_free(net_protocol *protocol)
{
	return B_OK;
}


status_t
icmp6_connect(net_protocol *protocol, const struct sockaddr *address)
{
	return B_ERROR;
}


status_t
icmp6_accept(net_protocol *protocol, struct net_socket **_acceptedSocket)
{
	return EOPNOTSUPP;
}


status_t
icmp6_control(net_protocol *protocol, int level, int option, void *value,
	size_t *_length)
{
	return protocol->next->module->control(protocol->next, level, option,
		value, _length);
}


status_t
icmp6_getsockopt(net_protocol *protocol, int level, int option,
	void *value, int *length)
{
	return protocol->next->module->getsockopt(protocol->next, level, option,
		value, length);
}


status_t
icmp6_setsockopt(net_protocol *protocol, int level, int option,
	const void *value, int length)
{
	return protocol->next->module->setsockopt(protocol->next, level, option,
		value, length);
}


status_t
icmp6_bind(net_protocol *protocol, const struct sockaddr *address)
{
	return B_ERROR;
}


status_t
icmp6_unbind(net_protocol *protocol, struct sockaddr *address)
{
	return B_ERROR;
}


status_t
icmp6_listen(net_protocol *protocol, int count)
{
	return EOPNOTSUPP;
}


status_t
icmp6_shutdown(net_protocol *protocol, int direction)
{
	return EOPNOTSUPP;
}


status_t
icmp6_send_data(net_protocol *protocol, net_buffer *buffer)
{
	return protocol->next->module->send_data(protocol->next, buffer);
}


status_t
icmp6_send_routed_data(net_protocol *protocol, struct net_route *route,
	net_buffer *buffer)
{
	return protocol->next->module->send_routed_data(protocol->next, route, buffer);
}


ssize_t
icmp6_send_avail(net_protocol *protocol)
{
	return B_ERROR;
}


status_t
icmp6_read_data(net_protocol *protocol, size_t numBytes, uint32 flags,
	net_buffer **_buffer)
{
	return B_ERROR;
}


ssize_t
icmp6_read_avail(net_protocol *protocol)
{
	return B_ERROR;
}


struct net_domain *
icmp6_get_domain(net_protocol *protocol)
{
	return protocol->next->module->get_domain(protocol->next);
}


size_t
icmp6_get_mtu(net_protocol *protocol, const struct sockaddr *address)
{
	return protocol->next->module->get_mtu(protocol->next, address);
}


static net_domain*
get_domain(struct net_buffer* buffer)
{
	net_domain* domain;
	if (buffer->interface_address != NULL)
		domain = buffer->interface_address->domain;
	else
		domain = sStackModule->get_domain(buffer->source->sa_family);

	if (domain == NULL || domain->module == NULL)
		return NULL;

	return domain;
}


status_t
icmp6_receive_data(net_buffer *buffer)
{
	TRACE(("ICMPv6 received some data, buffer length %" B_PRIu32 "\n",
		buffer->size));

	net_domain* domain = get_domain(buffer);
	if (domain == NULL)
		return B_ERROR;

	NetBufferHeaderReader<icmp6_hdr> bufferHeader(buffer);
	if (bufferHeader.Status() < B_OK)
		return bufferHeader.Status();

	icmp6_hdr &header = bufferHeader.Data();

	TRACE(("  got type %u, code %u, checksum 0x%x\n", header.icmp6_type,
			header.icmp6_code, header.icmp6_cksum));

	net_address_module_info* addressModule = domain->address_module;

	// compute and check the checksum
 	if (Checksum::PseudoHeader(addressModule, gBufferModule, buffer,
 			IPPROTO_ICMPV6) != 0)
 		return B_BAD_DATA;

	switch (header.icmp6_type) {
		case ICMP6_ECHO_REPLY:
			break;

		case ICMP6_ECHO_REQUEST:
		{
			if (buffer->interface_address != NULL) {
				// We only reply to echo requests of our local interface; we
				// don't reply to broadcast requests
				if (!domain->address_module->equal_addresses(
						buffer->interface_address->local, buffer->destination))
					break;
			}

			net_buffer *reply = gBufferModule->duplicate(buffer);
			if (reply == NULL)
				return B_NO_MEMORY;

			gBufferModule->swap_addresses(reply);

			// There already is an ICMP header, and we'll reuse it
			NetBufferHeaderReader<icmp6_hdr> header(reply);

			header->icmp6_type = ICMP6_ECHO_REPLY;
			header->icmp6_code = 0;
			header->icmp6_cksum = 0;

			header.Sync();

			*ICMP6ChecksumField(reply) = Checksum::PseudoHeader(addressModule,
				gBufferModule, buffer, IPPROTO_ICMPV6);

			status_t status = domain->module->send_data(NULL, reply);
			if (status < B_OK) {
				gBufferModule->free(reply);
				return status;
			}
			break;
		}

		case ICMP6_DST_UNREACH:
		case ICMP6_PACKET_TOO_BIG:
		case ICMP6_TIME_EXCEEDED:
		case ICMP6_PARAM_PROB:
		{
			net_domain* domain = get_domain(buffer);
			if (domain == NULL)
				break;

			net_error error = icmp6_to_net_error(header.icmp6_type,
				header.icmp6_code);
			if (error == 0)
				break;

			net_error_data dataStorage = {};
			net_error_data* data = NULL;
			if (error == B_NET_ERROR_MESSAGE_SIZE) {
				data = &dataStorage;
				data->mtu = ntohl(header.icmp6_mtu);

				// IPv6 minimum fragment size is 1280 bytes, so if the "next MTU"
				// is smaller than that, we can be sure it's invalid.
				if (data->mtu < 1280)
					data = NULL;
			}

			// Deliver the error to the domain protocol which will
			// propagate the error to the upper protocols
			bufferHeader.Remove();
			return domain->module->error_received(error, data, buffer);
		}

		default:
			// unrecognized messages go to neighbor discovery protocol handler
			return sIPv6NDPModule->receive_data(buffer);
	}

	gBufferModule->free(buffer);
	return B_OK;
}


status_t
icmp6_error_received(net_error code, net_error_data* errorData, net_buffer* data)
{
 	return B_ERROR;
}


/*!	Sends an ICMPv6 error message to the source of the \a buffer causing the
	error. The \a buffer stays the caller's; it must start with the IPv6 header
	of the invoking packet, possibly as a stored header.
*/
status_t
icmp6_error_reply(net_protocol* protocol, net_buffer* buffer, net_error error,
	net_error_data* errorData)
{
	uint8 icmpType;
	uint8 icmpCode;
	if (!net_error_to_icmp6(error, icmpType, icmpCode))
		return B_BAD_VALUE;

	TRACE(("icmp6_error_reply(error %d): type %u, code %u\n", (int)error,
		icmpType, icmpCode));

	// A Packet Too Big without the MTU to report is no use to its receiver.
	if (error == B_NET_ERROR_MESSAGE_SIZE && errorData == NULL)
		return B_BAD_VALUE;

	// The error's addresses are the ones the receive path recorded for the
	// invoking packet, not bytes of the quote: on the reassembly timeout path
	// the quote is fragment data a sender chose.
	if (buffer->source->sa_family != AF_INET6
		|| buffer->destination->sa_family != AF_INET6)
		return B_BAD_VALUE;
	const sockaddr_in6& invokingSource = *(sockaddr_in6*)buffer->source;
	const sockaddr_in6& invokingDestination
		= *(sockaddr_in6*)buffer->destination;

	ip6_hdr header;
	if (gBufferModule->restore_header(buffer, 0, &header, sizeof(ip6_hdr))
			!= B_OK
		|| (header.ip6_vfc & IPV6_VERSION_MASK) != IPV6_VERSION
		|| memcmp(&header.ip6_src, &invokingSource.sin6_addr,
			sizeof(in6_addr)) != 0
		|| memcmp(&header.ip6_dst, &invokingDestination.sin6_addr,
			sizeof(in6_addr)) != 0)
		return B_BAD_VALUE;

	// RFC 4443, section 2.4 (e): no error for a packet whose source does not
	// identify a single node,
	if (IN6_IS_ADDR_UNSPECIFIED(&invokingSource.sin6_addr)
		|| IN6_IS_ADDR_MULTICAST(&invokingSource.sin6_addr))
		return B_ERROR;

	// nor for one to a multicast address, except for a Packet Too Big, which
	// Path MTU Discovery of multicast needs (RFC 4443 also exempts a
	// Parameter Problem about an unrecognized option, which no caller here
	// raises). This node joins no anycast addresses, so any other destination
	// that reached a transport protocol is one of its unicast addresses.
	bool multicast = IN6_IS_ADDR_MULTICAST(&invokingDestination.sin6_addr)
		|| (buffer->msg_flags & (MSG_BCAST | MSG_MCAST)) != 0;
	if (multicast && error != B_NET_ERROR_MESSAGE_SIZE)
		return B_ERROR;

	net_domain* domain = get_domain(buffer);
	if (domain == NULL)
		return B_ERROR;

	// The error is sent from the address the invoking packet went to, so that
	// must be one of ours. A multicast one is not a source the error can have:
	// such a Packet Too Big goes out from the address the route selects.
	if (!multicast && !sDatalinkModule->is_local_address(domain,
			(const sockaddr*)&invokingDestination, NULL, NULL))
		return B_ERROR;

	size_t nextHeaderOffset;
	if (!is_error_allowed(buffer, header, error, nextHeaderOffset))
		return B_ERROR;

	// Checked last, so that only an error actually sent costs credit. Like
	// Linux, an error to this host itself is not limited: it never reaches a
	// link, and a local client relies on it to fail fast, which a remote
	// scan draining the bucket must not undo.
	bool toThisHost = sDatalinkModule->is_local_address(domain,
		(const sockaddr*)&invokingSource, NULL, NULL);
	if (!toThisHost && !error_rate_allows())
		return B_WOULD_BLOCK;

	// The invoking packet: its stored IPv6 (and extension) headers, if the
	// receive path removed them, and whatever follows.
	ssize_t storedLength = gBufferModule->stored_header_length(buffer);
	if (storedLength < 0)
		storedLength = 0;
	size_t quotedLength = std::min((size_t)storedLength + buffer->size,
		kMaxQuotedLength);

	net_buffer* reply = gBufferModule->create(256);
	if (reply == NULL)
		return B_NO_MEMORY;

	// The ports the transport protocol may have filled in mean nothing here;
	// the scope is kept, as a link-local peer needs it.
	sockaddr_in6* source = (sockaddr_in6*)reply->source;
	sockaddr_in6* destination = (sockaddr_in6*)reply->destination;
	memset(source, 0, sizeof(sockaddr_in6));
	memset(destination, 0, sizeof(sockaddr_in6));
	source->sin6_len = destination->sin6_len = sizeof(sockaddr_in6);
	source->sin6_family = destination->sin6_family = AF_INET6;
	if (!multicast) {
		source->sin6_addr = invokingDestination.sin6_addr;
		source->sin6_scope_id = invokingDestination.sin6_scope_id;
	}
	destination->sin6_addr = invokingSource.sin6_addr;
	destination->sin6_scope_id = invokingSource.sin6_scope_id;

	status_t status;
	{
		NetBufferPrepend<icmp6_hdr> icmpHeader(reply);
		status = icmpHeader.Status();
		if (status == B_OK) {
			icmpHeader->icmp6_type = icmpType;
			icmpHeader->icmp6_code = icmpCode;
			icmpHeader->icmp6_cksum = 0;
			icmpHeader->icmp6_data32[0] = 0;

			if (errorData != NULL) {
				if (error == B_NET_ERROR_MESSAGE_SIZE)
					icmpHeader->icmp6_mtu = htonl(errorData->mtu);
				else if (error == B_NET_ERROR_PARAMETER_PROBLEM)
					icmpHeader->icmp6_pptr = htonl(errorData->error_offset);
			}
			if (error == B_NET_ERROR_UNREACH_PROTOCOL)
				icmpHeader->icmp6_pptr = htonl(nextHeaderOffset);

			icmpHeader.Sync();
		}
	}

	if (status == B_OK) {
		status = gBufferModule->append_restored_header(reply, buffer, 0,
			quotedLength);
	}
	if (status == B_OK) {
		reply->protocol = IPPROTO_ICMPV6;
		*ICMP6ChecksumField(reply) = Checksum::PseudoHeader(
			domain->address_module, gBufferModule, reply, IPPROTO_ICMPV6);

		status = domain->module->send_data(NULL, reply);
	}
	if (status != B_OK)
		gBufferModule->free(reply);

	return status;
}


//	#pragma mark -


static status_t
icmp6_init()
{
	sStackModule->register_domain_protocols(AF_INET6, SOCK_DGRAM, IPPROTO_ICMPV6,
		"network/protocols/icmp6/v1",
		"network/protocols/ipv6/v1",
		NULL);

	sStackModule->register_domain_receiving_protocol(AF_INET6, IPPROTO_ICMPV6,
		"network/protocols/icmp6/v1");

	return B_OK;
}


static status_t
icmp6_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			return icmp6_init();

		case B_MODULE_UNINIT:
			return B_OK;

		default:
			return B_ERROR;
	}
}


net_protocol_module_info sICMP6Module = {
	{
		"network/protocols/icmp6/v1",
		0,
		icmp6_std_ops
	},
	NET_PROTOCOL_ATOMIC_MESSAGES,

	icmp6_init_protocol,
	icmp6_uninit_protocol,
	icmp6_open,
	icmp6_close,
	icmp6_free,
	icmp6_connect,
	icmp6_accept,
	icmp6_control,
	icmp6_getsockopt,
	icmp6_setsockopt,
	icmp6_bind,
	icmp6_unbind,
	icmp6_listen,
	icmp6_shutdown,
	icmp6_send_data,
	icmp6_send_routed_data,
	icmp6_send_avail,
	icmp6_read_data,
	icmp6_read_avail,
	icmp6_get_domain,
	icmp6_get_mtu,
	icmp6_receive_data,
	NULL,		// deliver_data
	icmp6_error_received,
	icmp6_error_reply,
	NULL,		// add_ancillary_data()
	NULL,		// process_ancillary_data()
	NULL,		// process_ancillary_data_no_container()
	NULL,		// send_data_no_buffer()
	NULL		// read_data_no_buffer()
};

module_dependency module_dependencies[] = {
	{NET_STACK_MODULE_NAME, (module_info **)&sStackModule},
	{NET_BUFFER_MODULE_NAME, (module_info **)&gBufferModule},
	{NET_DATALINK_MODULE_NAME, (module_info **)&sDatalinkModule},
	{"network/datalink_protocols/ipv6_datagram/ndp/v1",
		(module_info **)&sIPv6NDPModule},
	{}
};

module_info *modules[] = {
	(module_info *)&sICMP6Module,
	NULL
};
