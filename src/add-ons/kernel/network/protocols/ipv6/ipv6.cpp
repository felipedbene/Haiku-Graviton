/*
 * Copyright 2006-2011, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 *		Atis Elsts, the.kfx@gmail.com
 */


#include "ipv6_address.h"
#include "ipv6_utils.h"
#include "multicast.h"

#include <net_datalink.h>
#include <net_datalink_protocol.h>
#include <net_device.h>
#include <net_protocol.h>
#include <net_stack.h>
#include <NetBufferUtilities.h>
#include <NetUtilities.h>
#include <ProtocolUtilities.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <StackOrHeapArray.h>
#include <util/AutoLock.h>
#include <util/atomic.h>
#include <util/list.h>
#include <util/DoublyLinkedList.h>
#include <util/MultiHashTable.h>

#include <netinet6/in6.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <new>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <utility>


//#define TRACE_IPV6
#ifdef TRACE_IPV6
	#define TRACE(format, args...) \
		dprintf("IPv6 [%" B_PRIdBIGTIME "] " format "\n", system_time(), ##args)
	#define TRACE_SK(protocol, format, args...) \
		dprintf("IPv6 [%" B_PRIdBIGTIME "] %p " format "\n", system_time(), \
			protocol, ##args)
	#define TRACE_ONLY(x) x
#else
	#define TRACE(args...)
	#define TRACE_SK(args...)
	#define TRACE_ONLY(x)
#endif


#define MAX_HASH_FRAGMENTS 		64
	// slots in the fragment packet's hash
#define FRAGMENT_TIMEOUT		60000000LL
	// discard fragment after 60 seconds [RFC 2460]
#define MAX_FRAGMENT_MEMORY		(4 * 1024 * 1024)
	// what all partial datagrams may hold at once, as Linux's default
	// ip(6)frag_high_thresh; the oldest are dropped to stay below it
#define FRAGMENT_OVERHEAD		2048
	// charged per fragment on top of its data: its buffer and data block


struct IPv6Header {
	struct ip6_hdr header;

	uint8 ProtocolVersion() const { return header.ip6_vfc & IPV6_VERSION_MASK; }
	uint8 ServiceType() const { return ntohl(header.ip6_flow) >> 20;}
	uint16 PayloadLength() const { return ntohs(header.ip6_plen); }
	const in6_addr& Dst() const { return header.ip6_dst; }
	const in6_addr& Src() const { return header.ip6_src; }
	uint8 NextHeader() const { return header.ip6_nxt; }
	uint32 GetHeaderOffset(net_buffer* buffer, uint32 headerCode = ~0u,
		uint32* _nextHeaderField = NULL) const;
};


typedef DoublyLinkedList<struct net_buffer,
	DoublyLinkedListCLink<struct net_buffer> > FragmentList;


// TODO: make common fragmentation interface for both address families
struct ipv6_packet_key {
	in6_addr	source;
	in6_addr	destination;
	// We use uint32 here due to the hash function
	uint32		id;
	uint32		protocol;
};


class FragmentPacket : public DoublyLinkedListLinkImpl<FragmentPacket> {
public:
								FragmentPacket(const ipv6_packet_key& key);
								~FragmentPacket();

			status_t			AddFragment(uint16 start, uint16 end,
									net_buffer* buffer, bool lastFragment);
			void				SetHeaders(uint8* headers, uint32 length,
									uint32 nextHeaderField);
			status_t			Reassemble(net_buffer* to);

			bool				IsComplete() const;

			const ipv6_packet_key& Key() const { return fKey; }
			FragmentPacket*&	HashTableLink() { return fNext; }

			bool				StopTimer();
			void				Orphan() { fOrphaned = true; }
			void				StartTimer();
			bool				CancelTimer();

	static	void				StaleTimer(struct net_timer* timer, void* data);

private:
			FragmentPacket*		fNext;
			struct ipv6_packet_key fKey;
			uint32				fIndex;
			uint32				fEnd;
				// where the last fragment ends, once it has arrived
			FragmentList		fFragments;
			net_timer			fTimer;
			bool				fOrphaned;
				// taken out of the hash while StaleTimer() was already running
				// for it; StaleTimer() deletes it
			uint32				fMemory;
				// what this packet is charged in sFragmentMemory
			bool				fReceivedLastFragment;
			uint8*				fHeaders;
				// the first fragment's headers in front of its fragment header
			uint32				fHeadersLength;
			uint32				fNextHeaderField;
				// offset in fHeaders of the byte that names the fragment header
};


struct FragmentHashDefinition {
	typedef ipv6_packet_key KeyType;
	typedef FragmentPacket ValueType;

	size_t HashKey(const KeyType& key) const
	{
		return jenkins_hashword((const uint32*)&key,
			sizeof(ipv6_packet_key) / sizeof(uint32), 0);
	}

	size_t Hash(ValueType* value) const
	{
		return HashKey(value->Key());
	}

	bool Compare(const KeyType& key, ValueType* value) const
	{
		const ipv6_packet_key& packetKey = value->Key();

		return packetKey.id == key.id
			&& packetKey.source == key.source
			&& packetKey.destination == key.destination
			&& packetKey.protocol == key.protocol;
	}

	ValueType*& GetLink(ValueType* value) const
	{
		return value->HashTableLink();
	}
};


typedef BOpenHashTable<FragmentHashDefinition, false, true> FragmentTable;


class RawSocket
	: public DoublyLinkedListLinkImpl<RawSocket>, public DatagramSocket<> {
public:
							RawSocket(net_socket* socket);
};


typedef DoublyLinkedList<RawSocket> RawSocketList;

typedef MulticastGroupInterface<IPv6Multicast> IPv6GroupInterface;
typedef MulticastFilter<IPv6Multicast> IPv6MulticastFilter;

struct MulticastStateHash {
	typedef std::pair<const in6_addr*, uint32> KeyType;
	typedef IPv6GroupInterface ValueType;

	size_t HashKey(const KeyType &key) const;
	size_t Hash(ValueType* value) const
		{ return HashKey(std::make_pair(&value->Address(),
			value->Interface()->index)); }
	bool Compare(const KeyType &key, ValueType* value) const
		{ return value->Interface()->index == key.second
			&& value->Address() == *key.first; }
	bool CompareValues(ValueType* value1, ValueType* value2) const
		{ return value1->Interface()->index == value2->Interface()->index
			&& value1->Address() == value2->Address(); }
	ValueType*& GetLink(ValueType* value) const { return value->MulticastGroupsHashLink(); }
};


struct ipv6_protocol : net_protocol {
	ipv6_protocol()
		:
		raw(NULL),
		multicast_filter(this)
	{
	}

	~ipv6_protocol()
	{
		delete raw;
	}

	RawSocket	*raw;
	uint8		service_type;
	uint8		time_to_live;
	uint8		multicast_time_to_live;
	uint8		receive_hoplimit;
	uint8		receive_pktinfo;
	uint32		multicast_interface;
		// IPV6_MULTICAST_IF interface index, 0 for the default selection
	bool		v6only;
		// IPV6_V6ONLY; while it is off, the socket also carries IPv4
	net_protocol* ipv4;
		// the IPv4 protocol that carries this socket's IPv4 traffic, created
		// the first time it has any (see ipv4_protocol_for())

	IPv6MulticastFilter multicast_filter;
};


static const int kDefaultTTL = IPV6_DEFHLIM;
static const int kDefaultMulticastTTL = 1;
static const bool kDefaultV6Only = false;
	// as Linux (net.ipv6.bindv6only=0): a socket bound to :: also accepts IPv4
	// unless the application asks for IPV6_V6ONLY

#define NET_IPV4_MODULE_NAME "network/protocols/ipv4/v1"

// Explicit Congestion Notification codepoints (RFC 3168). In IPv6 the ECN bits
// are the low 2 bits of the 8-bit traffic class, which occupies bits 20..27 of
// the host-order flow word (ip6_flow); ECN therefore sits at host-order bits
// 20..21. The mirror of the IPv4 ToS translate point lives in this file.
static const uint8 kIPECNMask = 0x03;
static const uint8 kIPECNNotECT = 0x00;
static const uint8 kIPECNECT0 = 0x02;
static const uint8 kIPECNCE = 0x03;
static const int kIPv6ECNFlowShift = 20;


extern net_protocol_module_info gIPv6Module;
	// we need this in ipv6_std_ops() for registering the AF_INET6 domain

net_stack_module_info* gStackModule;
net_buffer_module_info* gBufferModule;

static struct net_domain* sDomain;
static net_datalink_module_info* sDatalinkModule;
static net_socket_module_info* sSocketModule;
static RawSocketList sRawSockets;
static mutex sRawSocketsLock;
static mutex sFragmentLock;
static FragmentTable sFragmentHash;
typedef DoublyLinkedList<FragmentPacket> FragmentPacketList;
static FragmentPacketList sFragmentPackets;
	// every packet in sFragmentHash, oldest first
static int64 sFragmentMemory;
	// what all of them hold, see FRAGMENT_OVERHEAD
static int32 sFragmentID;
static mutex sMulticastGroupsLock;

typedef MultiHashTable<MulticastStateHash> MulticastState;
static MulticastState* sMulticastState;

static net_protocol_module_info* sReceivingProtocol[256];
static mutex sReceivingProtocolLock;


/*!	Returns whether a header of \a type can be walked over to find the one
	after it. ESP cannot: everything after its SPI and sequence number is
	encrypted, including the next header field.
*/
static bool
is_extension_header(uint8 type)
{
	switch (type) {
		case IPPROTO_HOPOPTS:
		case IPPROTO_ROUTING:
		case IPPROTO_DSTOPTS:
		case IPPROTO_FRAGMENT:
		case IPPROTO_AH:
			return true;
		default:
			return false;
	}
}


/*!	Returns how many bytes the extension header of \a type, starting with
	\a extensionHeader, takes up. Never 0 for an extension header.
*/
static uint32
extension_header_length(uint8 type, const ip6_ext& extensionHeader)
{
	switch (type) {
		case IPPROTO_HOPOPTS:
		case IPPROTO_ROUTING:
		case IPPROTO_DSTOPTS:
			// in 8-octet units, not counting the first 8 (RFC 8200, 4.3)
			return (extensionHeader.ip6e_len + 1) * 8;
		case IPPROTO_FRAGMENT:
			// fixed size; the byte in the length position is reserved
			return sizeof(ip6_frag);
		case IPPROTO_AH:
			// in 4-octet units, not counting the first 2 (RFC 4302, 2.2)
			return (extensionHeader.ip6e_len + 2) * 4;
		default:
			return 0;
	}
}


/*!	Walks the extension headers that follow the fixed header.

	With \a headerCode left at its default, returns the offset of the
	transport header and sets the buffer's \c protocol to its type. Otherwise
	returns the offset of the first header of type \a headerCode, or 0 if there
	is none.

	Returns 0 as well if the chain runs past the end of the packet. The walk
	always moves past each header it reads, so it ends, whatever the packet
	holds.

	If \a _nextHeaderField is given, it is set to the offset of the byte that
	names the type of the header whose offset is returned: the fixed header's
	\c ip6_nxt, or the \c ip6e_nxt of the extension header before it.
*/
uint32
IPv6Header::GetHeaderOffset(net_buffer* buffer, uint32 headerCode,
	uint32* _nextHeaderField) const
{
	uint32 offset = sizeof(struct ip6_hdr);
	uint32 nextHeaderField = offsetof(struct ip6_hdr, ip6_nxt);
	uint8 next = header.ip6_nxt;

	while (next != headerCode) {
		if (!is_extension_header(next)) {
			// this is where the chain ends
			break;
		}

		struct ip6_ext extensionHeader;
		if (gBufferModule->read(buffer, offset, &extensionHeader,
				sizeof(ip6_ext)) != B_OK)
			return 0;

		uint32 length = extension_header_length(next, extensionHeader);
		if (offset + length > buffer->size)
			return 0;

		nextHeaderField = offset + offsetof(struct ip6_ext, ip6e_nxt);
		next = extensionHeader.ip6e_nxt;
		offset += length;
	}

	if (_nextHeaderField != NULL)
		*_nextHeaderField = nextHeaderField;

	// were we looking for a specific header?
	if (headerCode != ~0u) {
		if (next == headerCode) {
			// found the specific header
			return offset;
		}
		// return 0 if fragement header is not present
		return 0;
	}

	// the general transport layer header case
	buffer->protocol = next;
	return offset;
}


RawSocket::RawSocket(net_socket* socket)
	:
	DatagramSocket<>("ipv6 raw socket", socket)
{
}


//	#pragma mark -


FragmentPacket::FragmentPacket(const ipv6_packet_key &key)
	:
	fKey(key),
	fEnd(0),
	fReceivedLastFragment(false),
	fHeaders(NULL),
	fHeadersLength(0),
	fNextHeaderField(0)
{
	fOrphaned = false;
	fMemory = 0;
	gStackModule->init_timer(&fTimer, FragmentPacket::StaleTimer, this);
}


FragmentPacket::~FragmentPacket()
{
	// cancel the kill timer
	gStackModule->set_timer(&fTimer, -1);

	// delete all fragments
	net_buffer* buffer;
	while ((buffer = fFragments.RemoveHead()) != NULL) {
		gBufferModule->free(buffer);
	}

	free(fHeaders);

	atomic_add64(&sFragmentMemory, -(int64)fMemory);
}


status_t
FragmentPacket::AddFragment(uint16 start, uint16 end, net_buffer* buffer,
	bool lastFragment)
{
	if (start >= end) {
		// invalid fragment
		return B_BAD_DATA;
	}

	// Once the end of the datagram is known, nothing may lie past it, and the
	// last fragment may not end before data already received (RFC 8200 4.5,
	// RFC 791). Such a fragment is dropped; it would otherwise be delivered
	// with the data around it, or keep the datagram from ever completing.
	uint16 dataEnd = fFragments.IsEmpty() ? 0 : fFragments.Last()->fragment.end;
		// the fragments are kept sorted and without overlaps
	if (lastFragment) {
		if (fReceivedLastFragment ? end != fEnd : end < dataEnd)
			return B_BAD_DATA;
	} else if (fReceivedLastFragment && end > fEnd)
		return B_BAD_DATA;

	// The end is only recorded once the fragment is stored below: one whose
	// data is all held already is freed, and would leave a complete-looking
	// datagram with no buffer of this call to reassemble into.
	const uint16 datagramEnd = end;

	// Search for a position in the list to insert the fragment

	FragmentList::ReverseIterator iterator = fFragments.GetReverseIterator();
	net_buffer* previous = NULL;
	net_buffer* next = NULL;
	while ((previous = iterator.Next()) != NULL) {
		if (previous->fragment.start <= start) {
			// The new fragment can be inserted after this one
			break;
		}

		next = previous;
	}

	// See if we already have the fragment's data

	if (previous != NULL && previous->fragment.start <= start
		&& previous->fragment.end >= end) {
		// we do, so we can just drop this fragment
		gBufferModule->free(buffer);
		return B_OK;
	}

	// Charged until the packet goes, whatever merging makes of the buffer.
	const uint32 charge = buffer->size + FRAGMENT_OVERHEAD;
	fMemory += charge;
	atomic_add64(&sFragmentMemory, charge);

	fIndex = buffer->index;
		// adopt the buffer's device index

	TRACE("    previous: %p, next: %p", previous, next);

	// If we have parts of the data already, truncate as needed

	if (previous != NULL && previous->fragment.end > start) {
		TRACE("    remove header %d bytes", previous->fragment.end - start);
		gBufferModule->remove_header(buffer, previous->fragment.end - start);
		start = previous->fragment.end;
	}
	if (next != NULL && end > next->fragment.start) {
		TRACE("    remove trailer %d bytes", end - next->fragment.start);
		gBufferModule->remove_trailer(buffer, end - next->fragment.start);
		end = next->fragment.start;
	}

	// Now try if we can already merge the fragments together

	// We will always keep the last buffer received, so that we can still
	// report an error (in which case we're not responsible for freeing it)

	if (previous != NULL && previous->fragment.end == start) {
		fFragments.Remove(previous);

		buffer->fragment.start = previous->fragment.start;
		buffer->fragment.end = end;

		status_t status = gBufferModule->merge(buffer, previous, false);
		TRACE("    merge previous: %s", strerror(status));
		if (status != B_OK) {
			fFragments.InsertBefore(next, previous);
			fMemory -= charge;
			atomic_add64(&sFragmentMemory, -(int64)charge);
			return status;
		}

		fFragments.InsertBefore(next, buffer);
		if (lastFragment) {
			fReceivedLastFragment = true;
			fEnd = datagramEnd;
		}

		return B_OK;
	} else if (next != NULL && next->fragment.start == end) {
		net_buffer* afterNext = (net_buffer*)next->link.next;
		fFragments.Remove(next);

		buffer->fragment.start = start;
		buffer->fragment.end = next->fragment.end;

		status_t status = gBufferModule->merge(buffer, next, true);
		TRACE("    merge next: %s", strerror(status));
		if (status != B_OK) {
			// Insert "next" at its previous position
			fFragments.InsertBefore(afterNext, next);
			fMemory -= charge;
			atomic_add64(&sFragmentMemory, -(int64)charge);
			return status;
		}

		fFragments.InsertBefore(afterNext, buffer);
		if (lastFragment) {
			fReceivedLastFragment = true;
			fEnd = datagramEnd;
		}

		return B_OK;
	}

	// We couldn't merge the fragments, so we need to add it as is

	TRACE("    new fragment: %p, bytes %d-%d", buffer, start, end);

	buffer->fragment.start = start;
	buffer->fragment.end = end;
	fFragments.InsertBefore(next, buffer);
	if (lastFragment) {
		fReceivedLastFragment = true;
		fEnd = datagramEnd;
	}

	return B_OK;
}


/*!	Takes over \a headers, a malloc()ed copy of the first fragment's headers
	that come before its fragment header. They become the headers of the
	reassembled packet (RFC 8200, 4.5). \a nextHeaderField is the offset of
	the byte in them that names the fragment header. A copy from a repeated
	first fragment is freed.
*/
void
FragmentPacket::SetHeaders(uint8* headers, uint32 length,
	uint32 nextHeaderField)
{
	if (fHeaders != NULL) {
		free(headers);
		return;
	}

	fHeaders = headers;
	fHeadersLength = length;
	fNextHeaderField = nextHeaderField;

	// held as long as the fragments, so charged with them; the destructor
	// gives fMemory back
	fMemory += length;
	atomic_add64(&sFragmentMemory, length);
}


/*!	Returns whether the fragments cover every byte from 0 to the end of the
	last fragment. Counting the bytes received is not enough: a fragment past
	the end used to make up for a hole.
*/
bool
FragmentPacket::IsComplete() const
{
	if (!fReceivedLastFragment)
		return false;

	uint32 covered = 0;
	FragmentList::ConstIterator iterator = fFragments.GetIterator();
	while (net_buffer* fragment = iterator.Next()) {
		if (fragment->fragment.start != covered)
			return false;
		covered = fragment->fragment.end;
	}
	return covered == fEnd;
}


/*!	Reassembles the fragments to the specified buffer \a to.
	This buffer must have been added via AddFragment() before.
	The fragments hold only their fragment data. The headers kept by
	SetHeaders() are put in front of it, with the fragment header left out.
*/
status_t
FragmentPacket::Reassemble(net_buffer* to)
{
	if (!IsComplete())
		return B_ERROR;

	net_buffer* buffer = NULL;

	net_buffer* fragment;
	while ((fragment = fFragments.RemoveHead()) != NULL) {
		if (buffer != NULL) {
			status_t status;
			if (to == fragment) {
				status = gBufferModule->merge(fragment, buffer, false);
				buffer = fragment;
			} else
				status = gBufferModule->merge(buffer, fragment, true);
			if (status != B_OK)
				return status;
		} else
			buffer = fragment;
	}

	if (buffer != to)
		panic("ipv6 packet reassembly did not work correctly.");

	to->index = fIndex;
		// reset the buffer's device index
	to->buffer_flags &= ~NET_BUFFER_L4_CHECKSUM_VALID;
		// That came with the fragment that is now the buffer, and said
		// nothing about the datagram: the layer-4 checksum has to be checked.

	// The data from offset 0 on is all there, so the first fragment arrived
	// and its headers were kept.
	if (fHeaders == NULL)
		return B_ERROR;

	uint32 payloadLength = fHeadersLength - sizeof(ip6_hdr) + to->size;
	if (payloadLength > IPV6_MAXPACKET)
		return B_BAD_DATA;

	ip6_hdr* header = (ip6_hdr*)fHeaders;
	header->ip6_plen = htons(payloadLength);
	fHeaders[fNextHeaderField] = fKey.protocol;
		// what followed the fragment header now follows the header before it

	return gBufferModule->prepend(to, fHeaders, fHeadersLength);
}


/*!	Makes sure StaleTimer() will not run for this packet. Must be called with
	sFragmentLock held, after the packet has been taken out of the hash.
	Returns \c false if that is too late: the timer has fired, and its hook is
	waiting for sFragmentLock. The packet must then not be deleted, but handed
	to the hook with Orphan().

	set_timer(-1), which the destructor calls, is not enough: it only takes a
	timer off the list of scheduled ones, and does not wait for a hook that
	has already fired and is waiting for the lock.
	wait_for_timer() cannot be used either, as the hook needs sFragmentLock.
*/
bool
FragmentPacket::StopTimer()
{
	gStackModule->cancel_timer(&fTimer);
	return !gStackModule->is_timer_running(&fTimer);
		// The timer thread marks the timer running in the same critical
		// section in which it takes it off the list, so once cancel_timer()
		// has had the timer lock, a hook that will run is visible here. It
		// cannot have finished, since it needs sFragmentLock first.
}


/*!	Gives the datagram FRAGMENT_TIMEOUT from its first fragment to complete
	(RFC 8200 4.5, RFC 791). Later fragments do not restart it, so a sender
	cannot keep a partial datagram alive by trickling fragments into it.
*/
void
FragmentPacket::StartTimer()
{
	gStackModule->set_timer(&fTimer, FRAGMENT_TIMEOUT);
}


/*!	Cancels the timer. Returns \c false if it has already fired, in which case
	StaleTimer() is about to free the packet.
*/
bool
FragmentPacket::CancelTimer()
{
	return gStackModule->cancel_timer(&fTimer);
}


/*static*/ void
FragmentPacket::StaleTimer(struct net_timer* timer, void* data)
{
	FragmentPacket* packet = (FragmentPacket*)data;
	TRACE("Assembling FragmentPacket %p timed out!", packet);

	MutexLocker locker(&sFragmentLock);
	if (packet->fOrphaned) {
		// It was completed while this hook waited for the lock, and is no
		// longer in the hash; whoever completed it left it to be freed here.
		locker.Unlock();
		delete packet;
		return;
	}
	sFragmentHash.Remove(packet);
	sFragmentPackets.Remove(packet);
	locker.Unlock();

	if (!packet->fFragments.IsEmpty()) {
		// Send error: fragment reassembly time exceeded
		sDomain->module->error_reply(NULL, packet->fFragments.First(),
			B_NET_ERROR_REASSEMBLY_TIME_EXCEEDED, NULL);
	}

	delete packet;
}


//	#pragma mark -


size_t
MulticastStateHash::HashKey(const KeyType &key) const
{
	size_t result = 0;
	result = jenkins_hashword((const uint32*)key.first,
		sizeof(in6_addr) / sizeof(uint32), result);
	result = jenkins_hashword(&key.second, 1, result);
	return result;
}


//	#pragma mark -


static inline void
dump_ipv6_header(IPv6Header &header)
{
#ifdef TRACE_IPV6
	char addrbuf[INET6_ADDRSTRLEN];
	dprintf("  version: %d\n", header.ProtocolVersion() >> 4);
	dprintf("  service_type: %d\n", header.ServiceType());
	dprintf("  payload_length: %d\n", header.PayloadLength());
	dprintf("  next_header: %d\n", header.NextHeader());
	dprintf("  hop_limit: %d\n", header.header.ip6_hops);
	dprintf("  source: %s\n", ip6_sprintf(&header.header.ip6_src, addrbuf));
	dprintf("  destination: %s\n",
		ip6_sprintf(&header.header.ip6_dst, addrbuf));
#endif
}


/*!	Drops the oldest partial datagrams, other than \a keep, until \a charge
	more bytes fit under MAX_FRAGMENT_MEMORY. Returns whether they do.
	Must be called with sFragmentLock held.
*/
static bool
make_room_for_fragment(FragmentPacket* keep, uint32 charge)
{
	FragmentPacketList::Iterator iterator = sFragmentPackets.GetIterator();
	while (atomic_get64(&sFragmentMemory) + charge > MAX_FRAGMENT_MEMORY) {
		FragmentPacket* oldest = iterator.Next();
		if (oldest == NULL)
			return false;
		if (oldest == keep || !oldest->CancelTimer()) {
			// One whose timer has fired is StaleTimer()'s to free; it is
			// waiting for sFragmentLock.
			continue;
		}

		TRACE("  dropping FragmentPacket %p to make room", oldest);
		iterator.Remove();
		sFragmentHash.Remove(oldest);
		delete oldest;
	}
	return true;
}


/*!	Attempts to re-assemble fragmented packets.
	\return B_OK if everything went well; if it could reassemble the packet,
		\a _buffer will point to its buffer, otherwise, it will be \c NULL.
	\return various error codes if something went wrong (mostly B_NO_MEMORY)
*/
static status_t
reassemble_fragments(const IPv6Header &header, net_buffer** _buffer,
	uint32 offset, uint32 nextHeaderField)
{
	net_buffer* buffer = *_buffer;
	status_t status;

	ip6_frag fragmentHeader;
	status = gBufferModule->read(buffer, offset, &fragmentHeader,
		sizeof(ip6_frag));

	if (status != B_OK)
		return status;

	// The fragment's data is what follows its fragment header; the headers in
	// front of that are the same in every fragment (RFC 8200, 4.5). The
	// buffer has already been trimmed to the packet's length.
	uint32 dataOffset = offset + sizeof(ip6_frag);
	if (dataOffset > buffer->size)
		return B_BAD_DATA;

	uint32 start = ntohs(fragmentHeader.ip6f_offlg & IP6F_OFF_MASK);
	uint32 end = start + buffer->size - dataOffset;
	bool lastFragment = (fragmentHeader.ip6f_offlg & IP6F_MORE_FRAG) == 0;

	// Discarded as RFC 8200 4.5 says: a packet that would reassemble to more
	// than the maximum payload, and a fragment other than the last one whose
	// length is not a multiple of 8, so the next could not start where it ends
	if (end > IPV6_MAXPACKET || (!lastFragment && (end - start) % 8 != 0))
		return B_BAD_DATA;

	// \a header may point into the buffer's data, so it is read before the
	// headers are removed
	struct ipv6_packet_key key;
	memcpy(&key.source, &header.Src(), sizeof(in6_addr));
	memcpy(&key.destination, &header.Dst(), sizeof(in6_addr));
	key.id = fragmentHeader.ip6f_ident;
	key.protocol = fragmentHeader.ip6f_nxt;

	uint8* headers = NULL;
	if (start == 0) {
		headers = (uint8*)malloc(offset);
		if (headers == NULL)
			return B_NO_MEMORY;

		status = gBufferModule->read(buffer, 0, headers, offset);
		if (status != B_OK) {
			free(headers);
			return status;
		}
	}
	MemoryDeleter headersDeleter(headers);

	// Every fragment, the first one included, keeps only its data, so that a
	// fragment's buffer offsets are its offsets in the reassembled data. That
	// is what AddFragment() trims overlaps by.
	status = gBufferModule->remove_header(buffer, dataOffset);
	if (status != B_OK)
		return status;

	// TODO: Make locking finer grained.
	MutexLocker locker(&sFragmentLock);

	FragmentPacket* packet = sFragmentHash.Lookup(key);
	if (packet == NULL) {
		// New fragment packet
		packet = new (std::nothrow) FragmentPacket(key);
		if (packet == NULL)
			return B_NO_MEMORY;

		// add packet to hash
		status = sFragmentHash.Insert(packet);
		if (status != B_OK) {
			delete packet;
			return status;
		}
		sFragmentPackets.Add(packet);
		packet->StartTimer();
	}

	if (!make_room_for_fragment(packet, buffer->size + FRAGMENT_OVERHEAD
			+ (headers != NULL ? offset : 0)))
		return B_NO_MEMORY;

	TRACE("   Received IPv6 %sfragment of size %" B_PRIu32 ", offset %" B_PRIu32
		".", lastFragment ? "last ": "", end - start, start);

	if (headers != NULL)
		packet->SetHeaders((uint8*)headersDeleter.Detach(), offset,
			nextHeaderField);

	status = packet->AddFragment(start, end, buffer, lastFragment);
	if (status != B_OK)
		return status;

	if (packet->IsComplete()) {
		sFragmentHash.Remove(packet);
		sFragmentPackets.Remove(packet);
			// no matter if reassembling succeeds, we won't need this packet
			// anymore

		status = packet->Reassemble(buffer);
		if (packet->StopTimer())
			delete packet;
		else
			packet->Orphan();

		// _buffer does not change
		return status;
	}

	// This indicates that the packet is not yet complete
	*_buffer = NULL;
	return B_OK;
}


/*!	Finishes a layer-4 checksum that a device was going to be asked to complete.
	No device completes one across fragments, and only the first fragment
	carries the layer-4 header to put it in. The field already holds the
	pseudo-header sum, so one pass over the layer-4 bytes completes it.
	Mirrors the IPv4 module's finish_l4_checksum().
*/
static status_t
finish_l4_checksum(net_buffer* buffer, uint32 headersLength, uint8 protocol)
{
	uint32 fieldOffset;
	switch (protocol) {
		case IPPROTO_TCP:
			fieldOffset = 16;
			break;
		case IPPROTO_UDP:
			fieldOffset = 6;
			break;
		default:
			// nothing else ever asks for offload
			return B_NOT_SUPPORTED;
	}

	if (buffer->size < headersLength + fieldOffset + sizeof(uint16))
		return B_BAD_VALUE;

	int32 sum = gBufferModule->checksum(buffer, headersLength,
		buffer->size - headersLength, false);
	if (sum < 0)
		return sum;

	uint16 result = (uint16)~(uint16)sum;
	if (protocol == IPPROTO_UDP && result == 0)
		result = 0xffff;
			// zero means "no checksum", which IPv6 does not allow for UDP

	status_t status = gBufferModule->write(buffer, headersLength + fieldOffset,
		&result, sizeof(result));
	if (status != B_OK)
		return status;

	buffer->buffer_flags &= ~NET_BUFFER_L4_CHECKSUM_NEEDED;
	buffer->buffer_flags |= NET_BUFFER_L4_CHECKSUM_VALID;
	return B_OK;
}


/*!	Fragments the incoming buffer and send all fragments via the specified
	\a route.

	Each fragment gets its own copy of the headers. They used to be written
	through a clone of the buffer, which shares its data, so setting the next
	header to "fragment" changed the original's too: every fragment header then
	named itself as the next header, and the last fragment was the whole
	original packet with more data after it (#965).
*/
static status_t
send_fragments(ipv6_protocol* protocol, struct net_route* route,
	net_buffer* buffer, uint32 mtu)
{
	TRACE_SK(protocol, "SendFragments(%" B_PRIu32 " bytes, mtu %" B_PRIu32 ")",
		buffer->size, mtu);

	NetBufferHeaderReader<IPv6Header> originalHeader(buffer);
	if (originalHeader.Status() != B_OK)
		return originalHeader.Status();

	// TODO: currently FragHeader goes always as the last one, but in theory
	// ext. headers like AuthHeader and DestOptions should go after it.
	uint32 nextHeaderField;
	uint32 headersLength = originalHeader->GetHeaderOffset(buffer, ~0u,
		&nextHeaderField);
	if (headersLength == 0)
		return B_BAD_DATA;
	uint8 transportProtocol = buffer->protocol;
		// set by GetHeaderOffset()

	// Adapt MTU to be a multiple of 8 (fragment offsets can only be specified
	// this way)
	if (mtu < headersLength + sizeof(ip6_frag) + 8)
		return EMSGSIZE;
	mtu -= headersLength + sizeof(ip6_frag);
	mtu &= ~7;

	status_t status;
	if ((buffer->buffer_flags & NET_BUFFER_L4_CHECKSUM_NEEDED) != 0) {
		// before the split, which copies the flags into every fragment
		status = finish_l4_checksum(buffer, headersLength, transportProtocol);
		if (status != B_OK)
			return status;
	}

	// The headers every fragment starts with: the original ones, with a
	// fragment header after them.
	uint32 fragmentHeadersLength = headersLength + sizeof(ip6_frag);
	BStackOrHeapArray<uint8, 128> headers(fragmentHeadersLength);
	if (!headers.IsValid())
		return B_NO_MEMORY;
	status = gBufferModule->read(buffer, 0, headers, headersLength);
	if (status != B_OK)
		return status;

	headers[nextHeaderField] = IPPROTO_FRAGMENT;
	ip6_hdr* header = (ip6_hdr*)&headers[0];
	ip6_frag* fragmentHeader = (ip6_frag*)&headers[headersLength];
	fragmentHeader->ip6f_nxt = transportProtocol;
	fragmentHeader->ip6f_reserved = 0;
	fragmentHeader->ip6f_ident = htonl(atomic_add(&sFragmentID, 1));
		// one identification for all fragments of the packet

	status = gBufferModule->remove_header(buffer, headersLength);
	if (status != B_OK)
		return status;

	uint32 bytesLeft = buffer->size;
	uint32 fragmentOffset = 0;

	TRACE("  adjusted MTU to %" B_PRIu32 " bytesLeft %" B_PRIu32, mtu,
		bytesLeft);

	while (bytesLeft > 0) {
		uint32 fragmentLength = min_c(bytesLeft, mtu);
		bytesLeft -= fragmentLength;
		bool lastFragment = bytesLeft == 0;

		header->ip6_plen = htons(fragmentHeadersLength - sizeof(ip6_hdr)
			+ fragmentLength);
		fragmentHeader->ip6f_offlg = htons(fragmentOffset) & IP6F_OFF_MASK;
		if (!lastFragment)
			fragmentHeader->ip6f_offlg |= IP6F_MORE_FRAG;

		TRACE("  send fragment of %" B_PRIu32 " bytes (%" B_PRIu32
			" bytes left)", fragmentLength, bytesLeft);

		net_buffer* fragmentBuffer;
		if (!lastFragment) {
			fragmentBuffer = gBufferModule->split(buffer, fragmentLength);
			fragmentOffset += fragmentLength;
		} else
			fragmentBuffer = buffer;

		if (fragmentBuffer == NULL) {
			status = B_NO_MEMORY;
			break;
		}

		status = gBufferModule->prepend(fragmentBuffer, headers,
			fragmentHeadersLength);

		// send fragment
		if (status == B_OK)
			status = sDatalinkModule->send_routed_data(route, fragmentBuffer);

		if (lastFragment) {
			// we don't own the last buffer, so we don't have to free it
			break;
		}

		if (status != B_OK) {
			gBufferModule->free(fragmentBuffer);
			break;
		}
	}

	return status;
}


static bool
deliver_multicast(net_protocol_module_info* module, net_buffer* buffer,
	bool deliverToRaw)
{
	TRACE("deliver_multicast(%p [%" B_PRIu32 " bytes])", buffer, buffer->size);
	if (module->deliver_data == NULL)
		return false;

	MutexLocker _(sMulticastGroupsLock);

	sockaddr_in6* multicastAddr = (sockaddr_in6*)buffer->destination;

	uint32 index = buffer->index;
	if (buffer->interface_address != NULL)
		index = buffer->interface_address->interface->index;

	MulticastState::ValueIterator it = sMulticastState->Lookup(std::make_pair(
		&multicastAddr->sin6_addr, index));

	size_t count = 0;

	while (it.HasNext()) {
		IPv6GroupInterface* state = it.Next();

		ipv6_protocol* ipProtocol = state->Parent()->Socket();
		// There is no socket for Neighbor Discovery Protocol
		if (ipProtocol->socket == NULL)
			continue;
		if (deliverToRaw && (ipProtocol->raw == NULL
				|| ipProtocol->socket->protocol != buffer->protocol))
			continue;

		if (state->FilterAccepts(buffer)) {
			net_protocol* protocol = ipProtocol;
			if (protocol->module != module) {
				// as multicast filters are installed with an IPv6 protocol
				// reference, we need to go and find the appropriate instance
				// related to the 'receiving protocol' with module 'module'.
				protocol = ipProtocol->socket->first_protocol;

				while (protocol != NULL && protocol->module != module)
					protocol = protocol->next;
			}

			if (protocol != NULL) {
				module->deliver_data(protocol, buffer);
				count++;
			}
		}
	}

	return count > 0;
}


static void
raw_receive_data(net_buffer* buffer)
{
	MutexLocker locker(sRawSocketsLock);

	if (sRawSockets.IsEmpty())
		return;

	TRACE("RawReceiveData(%i)", buffer->protocol);

	if ((buffer->msg_flags & MSG_MCAST) != 0) {
		deliver_multicast(&gIPv6Module, buffer, true);
	} else {
		RawSocketList::Iterator iterator = sRawSockets.GetIterator();

		while (iterator.HasNext()) {
			RawSocket* raw = iterator.Next();

			if (raw->Socket()->protocol == buffer->protocol)
				raw->EnqueueClone(buffer);
		}
	}
}


static inline sockaddr*
fill_sockaddr_in6(sockaddr_in6* target, const in6_addr &address)
{
	target->sin6_family = AF_INET6;
	target->sin6_len = sizeof(sockaddr_in6);
	target->sin6_port = 0;
	target->sin6_flowinfo = 0;
	memcpy(target->sin6_addr.s6_addr, address.s6_addr, sizeof(in6_addr));
	target->sin6_scope_id = 0;
	return (sockaddr*)target;
}


status_t
IPv6Multicast::JoinGroup(IPv6GroupInterface* state)
{
	MutexLocker _(sMulticastGroupsLock);

	sockaddr_in6 groupAddr;
	status_t status = sDatalinkModule->join_multicast(state->Interface(),
		sDomain, fill_sockaddr_in6(&groupAddr, state->Address()));
	if (status != B_OK)
		return status;

	sMulticastState->Insert(state);
	return B_OK;
}


status_t
IPv6Multicast::LeaveGroup(IPv6GroupInterface* state)
{
	MutexLocker _(sMulticastGroupsLock);

	sMulticastState->Remove(state);

	sockaddr_in6 groupAddr;
	return sDatalinkModule->leave_multicast(state->Interface(), sDomain,
		fill_sockaddr_in6(&groupAddr, state->Address()));
}


static net_protocol_module_info*
receiving_protocol(uint8 protocol)
{
	net_protocol_module_info* module = sReceivingProtocol[protocol];
	if (module != NULL)
		return module;

	MutexLocker locker(sReceivingProtocolLock);

	module = sReceivingProtocol[protocol];
	if (module != NULL)
		return module;

	if (gStackModule->get_domain_receiving_protocol(sDomain, protocol,
			&module) == B_OK)
		sReceivingProtocol[protocol] = module;

	return module;
}


static status_t
ipv6_delta_group(IPv6GroupInterface* group, int option,
	net_interface* interface, const in6_addr* sourceAddr)
{
	switch (option) {
		case IPV6_JOIN_GROUP:
			return group->Add();
		case IPV6_LEAVE_GROUP:
			return group->Drop();
	}

	return B_ERROR;
}


static status_t
ipv6_delta_membership(ipv6_protocol* protocol, int option,
	net_interface* interface, const in6_addr* groupAddr,
	const in6_addr* sourceAddr)
{
	IPv6MulticastFilter &filter = protocol->multicast_filter;
	IPv6GroupInterface* state = NULL;
	status_t status = B_OK;

	switch (option) {
		// TODO: support more options
		case IPV6_JOIN_GROUP:
			status = filter.GetState(*groupAddr, interface, state, true);
			break;

		case IPV6_LEAVE_GROUP:
			filter.GetState(*groupAddr, interface, state, false);
			if (state == NULL)
				return EADDRNOTAVAIL;
			break;
	}

	if (status != B_OK)
		return status;

	status = ipv6_delta_group(state, option, interface, sourceAddr);
	filter.ReturnState(state);
	return status;
}


static status_t
ipv6_delta_membership(ipv6_protocol* protocol, int option,
	uint32 interfaceIndex, in6_addr* groupAddr, in6_addr* sourceAddr)
{
	net_interface* interface;

	// TODO: can the interface be unspecified?
	interface = sDatalinkModule->get_interface(sDomain, interfaceIndex);

	if (interface == NULL)
		return B_DEVICE_NOT_FOUND;

	return ipv6_delta_membership(protocol, option, interface,
		groupAddr, sourceAddr);
}


static status_t
get_int_option(void* target, size_t length, int value)
{
	if (length != sizeof(int))
		return B_BAD_VALUE;

	return user_memcpy(target, &value, sizeof(int));
}


template<typename Type> static status_t
set_int_option(Type &target, const void* _value, size_t length)
{
	int value;

	if (length != sizeof(int))
		return B_BAD_VALUE;

	if (user_memcpy(&value, _value, sizeof(int)) != B_OK)
		return B_BAD_ADDRESS;

	target = value;
	return B_OK;
}


/*!	Returns the IPv4 protocol that carries the IPv4 traffic of this dual-stack
	socket, creating it the first time. It is an ordinary IPv4 protocol
	instance bound to the same net_socket, so an IPv4 datagram or segment of
	an IPv6 socket gets the same header, fragmentation and SO_BROADCAST
	treatment as one of an IPv4 socket, and IPPROTO_IP options set on the IPv6
	socket apply to it (see ipv6_setsockopt()).
	Senders of one socket can race to create it; the loser frees its copy.
*/
static net_protocol*
ipv4_protocol_for(ipv6_protocol* protocol)
{
	net_protocol* ipv4 = atomic_pointer_get(&protocol->ipv4);
	if (ipv4 != NULL)
		return ipv4;

	net_protocol_module_info* module;
	if (get_module(NET_IPV4_MODULE_NAME, (module_info**)&module) != B_OK)
		return NULL;

	ipv4 = module->init_protocol(protocol->socket);
	if (ipv4 == NULL) {
		put_module(NET_IPV4_MODULE_NAME);
		return NULL;
	}

	ipv4->module = module;
	ipv4->socket = protocol->socket;
	ipv4->next = NULL;

	net_protocol* existing = atomic_pointer_test_and_set(&protocol->ipv4,
		ipv4, (net_protocol*)NULL);
	if (existing != NULL) {
		module->uninit_protocol(ipv4);
		put_module(NET_IPV4_MODULE_NAME);
		return existing;
	}

	return ipv4;
}


/*!	The IPv4 options a dual-stack IPv6 socket accepts on behalf of its IPv4
	traffic. Group membership and the multicast options stay refused: the
	IPv4 multicast filter delivers to the protocol it is attached to, which
	here is not part of the socket's protocol chain.
*/
static bool
is_ipv4_traffic_option(int option)
{
	return option == IP_TOS || option == IP_TTL || option == IP_DONTFRAG;
}


//	#pragma mark -


net_protocol*
ipv6_init_protocol(net_socket* socket)
{
	ipv6_protocol* protocol = new (std::nothrow) ipv6_protocol();
	if (protocol == NULL)
		return NULL;

	protocol->raw = NULL;
	protocol->service_type = 0;
	protocol->time_to_live = kDefaultTTL;
	protocol->multicast_time_to_live = kDefaultMulticastTTL;
	protocol->receive_hoplimit = 0;
	protocol->receive_pktinfo = 0;
	protocol->multicast_interface = 0;
	protocol->v6only = kDefaultV6Only;
	protocol->ipv4 = NULL;
	return protocol;
}


status_t
ipv6_uninit_protocol(net_protocol* _protocol)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	if (protocol->ipv4 != NULL) {
		protocol->ipv4->module->uninit_protocol(protocol->ipv4);
		put_module(NET_IPV4_MODULE_NAME);
	}

	delete protocol;
	return B_OK;
}


/*!	Since open() is only called on the top level protocol, when we get here
	it means we are on a SOCK_RAW socket.
*/
status_t
ipv6_open(net_protocol* _protocol)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	RawSocket* raw = new (std::nothrow) RawSocket(protocol->socket);
	if (raw == NULL)
		return B_NO_MEMORY;

	status_t status = raw->InitCheck();
	if (status != B_OK) {
		delete raw;
		return status;
	}

	TRACE_SK(protocol, "Open()");

	protocol->raw = raw;

	MutexLocker locker(sRawSocketsLock);
	sRawSockets.Add(raw);
	return B_OK;
}


status_t
ipv6_close(net_protocol* _protocol)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	RawSocket* raw = protocol->raw;
	if (raw == NULL)
		return B_ERROR;

	TRACE_SK(protocol, "Close()");

	MutexLocker locker(sRawSocketsLock);
	sRawSockets.Remove(raw);
	delete raw;
	protocol->raw = NULL;

	return B_OK;
}


status_t
ipv6_free(net_protocol* protocol)
{
	return B_OK;
}


status_t
ipv6_connect(net_protocol* _protocol, const struct sockaddr* address)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	RawSocket* raw = protocol->raw;
	if (raw == NULL)
		return B_ERROR;
	if (address->sa_len != sizeof(struct sockaddr_in6))
		return B_BAD_VALUE;
	if (address->sa_family != AF_INET6)
		return EAFNOSUPPORT;

	memcpy(&protocol->socket->peer, address, sizeof(struct sockaddr_in6));
	sSocketModule->set_connected(protocol->socket);

	return B_OK;
}


status_t
ipv6_accept(net_protocol* protocol, struct net_socket** _acceptedSocket)
{
	return EOPNOTSUPP;
}


status_t
ipv6_control(net_protocol* _protocol, int level, int option, void* value,
	size_t* _length)
{
	if ((level & LEVEL_MASK) != IPPROTO_IPV6)
		return sDatalinkModule->control(sDomain, option, value, _length);

	return B_BAD_VALUE;
}


status_t
ipv6_getsockopt(net_protocol* _protocol, int level, int option, void* value,
	int* _length)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	if (level == IPPROTO_IPV6) {
		// TODO: support more of these options

		if (option == IPV6_MULTICAST_HOPS) {
			return get_int_option(value, *_length,
				protocol->multicast_time_to_live);
		}
		if (option == IPV6_MULTICAST_IF) {
			return get_int_option(value, *_length,
				protocol->multicast_interface);
		}
		if (option == IPV6_MULTICAST_LOOP)
			return EOPNOTSUPP;
		if (option == IPV6_UNICAST_HOPS)
			return get_int_option(value, *_length, protocol->time_to_live);
		if (option == IPV6_V6ONLY)
			return get_int_option(value, *_length, protocol->v6only ? 1 : 0);
		if (option == IPV6_RECVPKTINFO)
			return get_int_option(value, *_length, protocol->receive_pktinfo);
		if (option == IPV6_RECVHOPLIMIT)
			return get_int_option(value, *_length, protocol->receive_hoplimit);
		if (option == IPV6_JOIN_GROUP
			|| option == IPV6_LEAVE_GROUP)
			return EOPNOTSUPP;

		dprintf("IPv6::getsockopt(): get unknown option: %d\n", option);
		return ENOPROTOOPT;
	}

	if (level == IPPROTO_IP && is_ipv4_traffic_option(option)) {
		net_protocol* ipv4 = ipv4_protocol_for(protocol);
		if (ipv4 == NULL)
			return B_NO_MEMORY;
		return ipv4->module->getsockopt(ipv4, level, option, value, _length);
	}

	return sSocketModule->get_option(protocol->socket, level, option, value,
		_length);
}


status_t
ipv6_setsockopt(net_protocol* _protocol, int level, int option,
	const void* value, int length)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	if (level == IPPROTO_IPV6) {
		// TODO: support more of these options

		if (option == IPV6_MULTICAST_IF) {
			// RFC 3493 section 5.2 and POSIX define the argument as an
			// interface index, with 0 for the default selection. It used to be
			// taken as an in6_addr, which no portable caller passes: libuv
			// hands in sin6_scope_id, and so got EINVAL every time.
			unsigned int index;
			if (length != sizeof(index))
				return B_BAD_VALUE;
			if (user_memcpy(&index, value, sizeof(index)) != B_OK)
				return B_BAD_ADDRESS;

			if (index != 0) {
				net_interface* interface
					= sDatalinkModule->get_interface(sDomain, index);
				if (interface == NULL)
					return EADDRNOTAVAIL;
				sDatalinkModule->put_interface(interface);
			}

			protocol->multicast_interface = index;
			return B_OK;
		}
		if (option == IPV6_MULTICAST_HOPS) {
			return set_int_option(protocol->multicast_time_to_live,
				value, length);
		}
		if (option == IPV6_MULTICAST_LOOP)
			return EOPNOTSUPP;
		if (option == IPV6_UNICAST_HOPS)
			return set_int_option(protocol->time_to_live, value, length);
		if (option == IPV6_V6ONLY) {
			// TCP and UDP refuse a change once the socket is bound, as Linux
			// and FreeBSD do: the binding was checked against the value it
			// had then.
			int v6only;
			status_t status = set_int_option(v6only, value, length);
			if (status == B_OK)
				protocol->v6only = v6only != 0;
			return status;
		}
		if (option == IPV6_RECVPKTINFO)
			return set_int_option(protocol->receive_pktinfo, value, length);
		if (option == IPV6_RECVHOPLIMIT)
			return set_int_option(protocol->receive_hoplimit, value, length);
		if (option == IPV6_JOIN_GROUP || option == IPV6_LEAVE_GROUP) {
			ipv6_mreq mreq;
			if (length != sizeof(ipv6_mreq))
				return B_BAD_VALUE;
			if (user_memcpy(&mreq, value, sizeof(ipv6_mreq)) != B_OK)
				return B_BAD_ADDRESS;

			return ipv6_delta_membership(protocol, option,
				mreq.ipv6mr_interface, &mreq.ipv6mr_multiaddr, NULL);
		}

		dprintf("IPv6::setsockopt(): set unknown option: %d\n", option);
		return ENOPROTOOPT;
	}

	if (level == IPPROTO_IP && is_ipv4_traffic_option(option)) {
		net_protocol* ipv4 = ipv4_protocol_for(protocol);
		if (ipv4 == NULL)
			return B_NO_MEMORY;
		return ipv4->module->setsockopt(ipv4, level, option, value, length);
	}

	return sSocketModule->set_option(protocol->socket, level, option,
		value, length);
}


status_t
ipv6_bind(net_protocol* _protocol, const sockaddr* _address)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	if (_address->sa_family == AF_INET && !protocol->v6only
		&& protocol->socket->first_protocol != protocol) {
		// A dual-stack socket taking on IPv4 traffic: the transport above us
		// has already turned the mapped address into the IPv4 one.
		net_protocol* ipv4 = ipv4_protocol_for(protocol);
		if (ipv4 == NULL)
			return B_NO_MEMORY;
		return ipv4->module->bind(ipv4, _address);
	}
	if (_address->sa_family != AF_INET6)
		return EAFNOSUPPORT;

	const sockaddr_in6* address = (const sockaddr_in6*)_address;

	// only INADDR_ANY and addresses of local interfaces are accepted:
	if (IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr)
		|| IN6_IS_ADDR_MULTICAST(&address->sin6_addr)
		|| sDatalinkModule->is_local_address(sDomain, _address, NULL, NULL)) {
		memcpy(&protocol->socket->address, address, sizeof(sockaddr_in6));
		protocol->socket->address.ss_len = sizeof(sockaddr_in6);
			// explicitly set length, as our callers can't be trusted to
			// always provide the correct length!
		return B_OK;
	}

	return EADDRNOTAVAIL;
		// address is unknown on this host; ipv4_bind() answers the same
}


status_t
ipv6_unbind(net_protocol* protocol, struct sockaddr* address)
{
	// nothing to do here
	return B_OK;
}


status_t
ipv6_listen(net_protocol* protocol, int count)
{
	return EOPNOTSUPP;
}


status_t
ipv6_shutdown(net_protocol* protocol, int direction)
{
	return EOPNOTSUPP;
}


static uint8
ip6_select_hoplimit(net_protocol* _protocol, net_buffer* buffer)
{
	// TODO: the precedence should be as follows:
	// 1. Hoplimit value specified via ioctl.
	// 2. (If the outgoing interface is detected) the current
	//     hop limit of the interface specified by router advertisement.
	// 3. The system default hoplimit.

	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	const bool isMulticast = buffer->msg_flags & MSG_MCAST;

	if (protocol) {
		return isMulticast ? protocol->multicast_time_to_live
			: protocol->time_to_live;
	}
	return isMulticast ? kDefaultMulticastTTL : kDefaultTTL;
}


status_t
ipv6_send_routed_data(net_protocol* _protocol, struct net_route* route,
	net_buffer* buffer)
{
	if (route == NULL)
		return B_BAD_VALUE;

	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	if (route->interface_address->domain != sDomain) {
		// IPv4 traffic of a dual-stack socket: the transport has routed it in
		// the IPv4 domain, with IPv4 addresses.
		if (protocol == NULL || protocol->v6only
			|| route->interface_address->domain->family != AF_INET)
			return EAFNOSUPPORT;

		net_protocol* ipv4 = ipv4_protocol_for(protocol);
		if (ipv4 == NULL)
			return B_NO_MEMORY;
		return ipv4->module->send_routed_data(ipv4, route, buffer);
	}

	net_interface* interface = route->interface_address->interface;
	uint8 protocolNumber;
	if (protocol != NULL && protocol->socket != NULL)
		protocolNumber = protocol->socket->protocol;
	else
		protocolNumber = buffer->protocol;

	TRACE_SK(protocol, "SendRoutedData(%p, %p [%" B_PRIu32 " bytes])", route,
		buffer, buffer->size);

	sockaddr_in6& source = *(sockaddr_in6*)buffer->source;
	sockaddr_in6& destination = *(sockaddr_in6*)buffer->destination;

	buffer->msg_flags &= ~(MSG_BCAST | MSG_MCAST);

	if (IN6_IS_ADDR_UNSPECIFIED(&destination.sin6_addr))
		return EDESTADDRREQ;

	// ::1 must not leave the host (RFC 4291, 2.5.3): only the loopback, or an
	// address of this host, takes it; EINVAL otherwise, as ipv4 answers.
	if (IN6_IS_ADDR_LOOPBACK(&source.sin6_addr)
		&& (interface->device->flags & IFF_LOOPBACK) == 0
		&& (route->flags & RTF_LOCAL) == 0)
		return B_BAD_VALUE;

	if (IN6_IS_ADDR_MULTICAST(&destination.sin6_addr))
		buffer->msg_flags |= MSG_MCAST;

	// The 16-bit length field counts the payload alone (RFC 8200, section 3),
	// so a packet may be 65535 bytes plus this header. Limiting the whole
	// packet to 65535 bytes, as IPv4 has to, refused every full-sized TCP
	// segment on loopback: its MTU of 65536 makes each one a 65536-byte
	// packet.
	if (buffer->size > IPV6_MAXPACKET)
		return EMSGSIZE;

	uint16 dataLength = buffer->size;

	// Add IPv6 header

	NetBufferPrepend<ip6_hdr> header(buffer);
	if (header.Status() != B_OK)
		return header.Status();

	uint32 flowinfo = 0;
		// TODO: fill in the flow id from somewhere
	if (protocol) {
		// fill in traffic class
		flowinfo |= htonl(protocol->service_type << 20);
	}
	// set lower 28 bits
	header->ip6_flow = htonl(flowinfo) & IPV6_FLOWINFO_MASK;
	// Stage C (RFC 3168): TCP marks individual data segments ECT(0) via a
	// per-buffer flag; OR it into the low 2 ECN bits of the traffic class so the
	// path/peer can mark CE. Retransmits, pure ACKs and SYNs leave the flag clear
	// and stay Not-ECT. Operate directly on the network-order flow word so this
	// is correct regardless of how the traffic class above was assembled.
	if ((buffer->buffer_flags & NET_BUFFER_ECN_ECT0) != 0) {
		header->ip6_flow = (header->ip6_flow
				& ~htonl((uint32)kIPECNMask << kIPv6ECNFlowShift))
			| htonl((uint32)kIPECNECT0 << kIPv6ECNFlowShift);
	}
	// set upper 4 bits
	header->ip6_vfc |= IPV6_VERSION;
	header->ip6_plen = htons(dataLength);
	header->ip6_nxt = protocolNumber;
	header->ip6_hlim = ip6_select_hoplimit(protocol, buffer);
	memcpy(&header->ip6_src, &source.sin6_addr, sizeof(in6_addr));
	memcpy(&header->ip6_dst, &destination.sin6_addr, sizeof(in6_addr));

	header.Sync();

	// write the checksum for ICMPv6 sockets
	if (protocolNumber == IPPROTO_ICMPV6
		&& dataLength >= sizeof(struct icmp6_hdr)) {
		NetBufferField<uint16, sizeof(ip6_hdr)
			+ offsetof(icmp6_hdr, icmp6_cksum)>
			icmpChecksum(buffer);
		// first make sure the existing checksum is zero
		*icmpChecksum = 0;
		icmpChecksum.Sync();

		uint16 checksum = gBufferModule->checksum(buffer, sizeof(ip6_hdr),
			buffer->size - sizeof(ip6_hdr), false);
		checksum = ipv6_checksum(&header->ip6_src,
			&header->ip6_dst, dataLength, protocolNumber,
			checksum);
		*icmpChecksum = checksum;
	}

	char addrbuf[INET6_ADDRSTRLEN];
	ip6_sprintf(&destination.sin6_addr, addrbuf);
	TRACE_SK(protocol, "  SendRoutedData(): destination: %s", addrbuf);

	uint32 mtu = route->mtu ? route->mtu : interface->device->mtu;
	if (buffer->size > mtu) {
		// we need to fragment the packet
		return send_fragments(protocol, route, buffer, mtu);
	}

	return sDatalinkModule->send_routed_data(route, buffer);
}


status_t
ipv6_send_data(net_protocol* _protocol, net_buffer* buffer)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	TRACE_SK(protocol, "SendData(%p [%" B_PRIu32 " bytes])", buffer,
		buffer->size);

	sockaddr_in6* destination = (sockaddr_in6*)buffer->destination;

	// handle IPV6_MULTICAST_IF
	if (IN6_IS_ADDR_MULTICAST(&destination->sin6_addr)
		&& protocol->multicast_interface != 0) {
		net_interface* interface = sDatalinkModule->get_interface(sDomain,
			protocol->multicast_interface);
		if (interface == NULL || (interface->flags & IFF_UP) == 0) {
			sDatalinkModule->put_interface(interface);
			return EADDRNOTAVAIL;
		}

		// send from the interface's first IPv6 address
		net_interface_address* address = NULL;
		while (sDatalinkModule->get_next_interface_address(interface,
				&address)) {
			if (address->domain == sDomain && address->local != NULL)
				break;
		}
		sDatalinkModule->put_interface(interface);
		if (address == NULL)
			return EADDRNOTAVAIL;

		sDatalinkModule->put_interface_address(buffer->interface_address);
		buffer->interface_address = address;
			// the buffer takes over ownership of the address

		net_route* route = sDatalinkModule->get_route(sDomain, address->local);
		if (route == NULL)
			return ENETUNREACH;

		return sDatalinkModule->send_routed_data(route, buffer);
	}

	return sDatalinkModule->send_data(protocol, sDomain, buffer);
}


ssize_t
ipv6_send_avail(net_protocol* protocol)
{
	return B_ERROR;
}


status_t
ipv6_read_data(net_protocol* _protocol, size_t numBytes, uint32 flags,
	net_buffer** _buffer)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	RawSocket* raw = protocol->raw;
	if (raw == NULL)
		return B_ERROR;

	TRACE_SK(protocol, "ReadData(%" B_PRIuSIZE ", 0x%" B_PRIu32 ")", numBytes,
		flags);

	return raw->Dequeue(flags, _buffer);
}


ssize_t
ipv6_read_avail(net_protocol* _protocol)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	RawSocket* raw = protocol->raw;
	if (raw == NULL)
		return B_ERROR;

	return raw->AvailableData();
}


bool
ipv6_message_queued(net_protocol* _protocol)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	RawSocket* raw = protocol->raw;
	if (raw == NULL)
		return false;

	return raw->MessageQueued();
}


struct net_domain*
ipv6_get_domain(net_protocol* protocol)
{
	return sDomain;
}


size_t
ipv6_get_mtu(net_protocol* protocol, const struct sockaddr* address)
{
	if (address->sa_family == AF_INET) {
		// the IPv4 path of a dual-stack socket
		net_domain* domain = gStackModule->get_domain(AF_INET);
		if (domain == NULL)
			return 0;
		return domain->module->get_mtu(NULL, address);
	}

	net_route* route = sDatalinkModule->get_route(sDomain, address);
	if (route == NULL)
		return 0;

	size_t mtu;
	if (route->mtu != 0)
		mtu = route->mtu;
	else
		mtu = route->interface_address->interface->device->mtu;

	sDatalinkModule->put_route(sDomain, route);
	// TODO: what about extension headers?
	// this function probably shoud be changed in calling places, not here
	return mtu - sizeof(ip6_hdr);
}


status_t
ipv6_receive_data(net_buffer* buffer)
{
	TRACE("ReceiveData(%p [%" B_PRIu32 " bytes])", buffer, buffer->size);

	NetBufferHeaderReader<IPv6Header> bufferHeader(buffer);
	if (bufferHeader.Status() != B_OK)
		return bufferHeader.Status();

	IPv6Header &header = bufferHeader.Data();
	// dump_ipv6_header(header);

	if (header.ProtocolVersion() != IPV6_VERSION)
		return B_BAD_TYPE;

	// Capture the inbound ECN codepoint (low 2 bits of the traffic class) before
	// the IPv6 header is stripped; it is realised onto the L4 side-channel at the
	// single translate point below.
	uint8 ecnCodepoint = header.ServiceType() & kIPECNMask;

	// kept for IPV6_RECVHOPLIMIT, for the same reason
	buffer->hop_limit = header.header.ip6_hlim;

	// Not a uint16: the payload length alone may be 65535, and with the fixed
	// header a packet can reach 65575 bytes. On loopback (MTU 65536) every
	// full-sized TCP segment is a 65536-byte packet, which a 16-bit sum
	// wrapped to 0 and the trim() below then emptied, so it was dropped.
	uint32 packetLength = header.PayloadLength() + sizeof(ip6_hdr);
	if (packetLength > buffer->size)
		return B_BAD_DATA;

	// lower layers notion of Broadcast or Multicast have no relevance to us
	buffer->msg_flags &= ~(MSG_BCAST | MSG_MCAST);

	sockaddr_in6 destination;
	fill_sockaddr_in6(&destination, header.Dst());

	if (IN6_IS_ADDR_MULTICAST(&destination.sin6_addr)) {
		buffer->msg_flags |= MSG_MCAST;
	} else {
		uint32 matchedAddressType = 0;

		// test if the packet is really for us
		if (!sDatalinkModule->is_local_address(sDomain, (sockaddr*)&destination,
				&buffer->interface_address, &matchedAddressType)
			&& !sDatalinkModule->is_local_link_address(sDomain, true,
				buffer->destination, &buffer->interface_address)) {

			char srcbuf[INET6_ADDRSTRLEN];
			char dstbuf[INET6_ADDRSTRLEN];
			ip6_sprintf(&header.Src(), srcbuf);
			ip6_sprintf(&header.Dst(), dstbuf);
			TRACE("  ipv6_receive_data(): packet was not for us %s -> %s",
				srcbuf, dstbuf);

			// TODO: Send ICMPv6 error: Host unreachable
			return B_ERROR;
		}

		// copy over special address types (MSG_BCAST or MSG_MCAST):
		buffer->msg_flags |= matchedAddressType;
	}

	// set net_buffer's source/destination address
	fill_sockaddr_in6((struct sockaddr_in6*)buffer->source, header.Src());
	memcpy(buffer->destination, &destination, sizeof(sockaddr_in6));

	// remove any trailing/padding data
	status_t status = gBufferModule->trim(buffer, packetLength);
	if (status != B_OK)
		return status;

	// check for fragmentation
	uint32 nextHeaderField;
	uint32 fragmentHeaderOffset = header.GetHeaderOffset(buffer,
		IPPROTO_FRAGMENT, &nextHeaderField);

	if (fragmentHeaderOffset != 0) {
		// this is a fragment
		TRACE("  ipv6_receive_data(): Found a Fragment!");
		status = reassemble_fragments(header, &buffer, fragmentHeaderOffset,
			nextHeaderField);
		TRACE("  ipv6_receive_data():  -> %s", strerror(status));
		if (status != B_OK)
			return status;

		if (buffer == NULL) {
			// buffer was put into fragment packet
			TRACE("  ipv6_receive_data(): Not yet assembled.");
			return B_OK;
		}
	}

	// Since the buffer might hold a reassembled packet now, the transport
	// header is looked for in that one: bufferHeader and header must not be
	// used anymore from here on.
	uint32 transportHeaderOffset;
	{
		NetBufferHeaderReader<IPv6Header> packetHeader(buffer);
		if (packetHeader.Status() != B_OK)
			return packetHeader.Status();

		if (fragmentHeaderOffset != 0
			&& packetHeader->GetHeaderOffset(buffer, IPPROTO_FRAGMENT) != 0) {
			// a fragment of a fragment; not reassembled again
			return B_BAD_DATA;
		}

		// get the transport protocol and transport header offset
		transportHeaderOffset = packetHeader->GetHeaderOffset(buffer);
	}
	if (transportHeaderOffset == 0) {
		// the extension headers run past the end of the packet
		return B_BAD_DATA;
	}
	uint8 protocol = buffer->protocol;

	// Single ECN translate point (RFC 3168), mirroring the IPv4 receive path.
	// Two inputs converge here:
	//   * A CE the path or peer already set in the traffic class -> echo it as a
	//     mark for TCP.
	//   * A NET_BUFFER_ECN_CE_MARK request from the ingress AQM (the receive
	//     FIFO's CoDel discipline, which cannot see L3 and so defers the
	//     mark-vs-drop choice to here): if the packet is ECN-capable (ECT),
	//     promote it to CE -- a mark, not a loss; if it is Not-ECT, the AQM's
	//     decision stands as an actual drop, taken here rather than in the FIFO.
	if ((buffer->buffer_flags & NET_BUFFER_ECN_CE_MARK) != 0
		&& ecnCodepoint == kIPECNNotECT) {
		// Non-ECN flow the AQM chose to shed: drop it (as the FIFO would have).
		gBufferModule->free(buffer);
		return B_OK;
	}
	if (ecnCodepoint == kIPECNCE
		|| (buffer->buffer_flags & NET_BUFFER_ECN_CE_MARK) != 0) {
		buffer->buffer_flags |= NET_BUFFER_ECN_CE;
	}

	// tell the buffer to preserve removed ipv6 header - may need it later
	gBufferModule->store_header(buffer);

	// remove ipv6 headers for now
	gBufferModule->remove_header(buffer, transportHeaderOffset);

	// deliver the data to raw sockets
	raw_receive_data(buffer);

	net_protocol_module_info* module = receiving_protocol(protocol);
	if (module == NULL) {
		// no handler for this packet
		return EAFNOSUPPORT;
	}

	if ((buffer->msg_flags & MSG_MCAST) != 0) {
		// Unfortunately historical reasons dictate that the IP multicast
		// model be a little different from the unicast one. We deliver
		// this frame directly to all sockets registered with interest
		// for this multicast group.
		deliver_multicast(module, buffer, false);
		if (protocol != IPPROTO_ICMPV6) {
			// ICMPv6 will still be passed to receive_data below,
			// as it might be a neighbor solicitation; otherwise,
			// we are done.
			gBufferModule->free(buffer);
			return B_OK;
		}
	}

	return module->receive_data(buffer);
}


status_t
ipv6_deliver_data(net_protocol* _protocol, net_buffer* buffer)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;

	if (protocol->raw == NULL)
		return B_ERROR;

	return protocol->raw->EnqueueClone(buffer);
}


status_t
ipv6_error_received(net_error error, net_error_data* errorData, net_buffer* buffer)
{
	TRACE("  ipv6_error_received(error %d, buffer %p [%" B_PRIu32 " bytes])",
		(int)error, buffer, buffer->size);

	NetBufferHeaderReader<IPv6Header> bufferHeader(buffer);
	if (bufferHeader.Status() != B_OK)
		return bufferHeader.Status();

	IPv6Header& header = bufferHeader.Data();
	TRACE_ONLY(dump_ipv6_header(header));

	// We do not check the packet length, as we usually only get a part of it
	if (header.ProtocolVersion() != IPV6_VERSION)
		return B_BAD_DATA;

	// Restore addresses of the original buffer

	// lower layers notion of broadcast or multicast have no relevance to us
	// TODO: they actually have when deciding whether to send an ICMP error
	buffer->msg_flags &= ~(MSG_BCAST | MSG_MCAST);

	fill_sockaddr_in6((struct sockaddr_in6*)buffer->source, header.Src());
	fill_sockaddr_in6((struct sockaddr_in6*)buffer->destination,
		header.Dst());

	// test if the packet is really from us
	if (!sDatalinkModule->is_local_address(sDomain, buffer->source, NULL,
			NULL)) {
		TRACE("  ipv6_error_received(): packet was not for us");
		return B_ERROR;
	}

	if (error == B_NET_ERROR_MESSAGE_SIZE) {
		if (errorData != NULL)
			errorData->mtu -= sizeof(IPv6Header);
	} else if (error == B_NET_ERROR_REDIRECT_HOST) {
		// TODO: Update the routing table!
	}

	buffer->protocol = header.NextHeader();

	bufferHeader.Remove(sizeof(IPv6Header));

	net_protocol_module_info* protocol = receiving_protocol(buffer->protocol);
	if (protocol == NULL)
		return B_ERROR;

	// propagate error
	return protocol->error_received(error, errorData, buffer);
}


status_t
ipv6_error_reply(net_protocol* protocol, net_buffer* cause, net_error error,
	net_error_data* errorData)
{
	// Directly obtain the ICMPv6 protocol module
	net_protocol_module_info* icmp = receiving_protocol(IPPROTO_ICMPV6);
	if (icmp == NULL)
		return B_ERROR;

	return icmp->error_reply(protocol, cause, error, errorData);
}


ssize_t
ipv6_process_ancillary_data_no_container(net_protocol* _protocol,
	net_buffer* buffer, void* msgControl, size_t msgControlLen,
	int* _messageFlags)
{
	ipv6_protocol* protocol = (ipv6_protocol*)_protocol;
	ssize_t bytesWritten = 0;

	// An IPv4 datagram on a dual-stack socket has no hop limit to report:
	// its header is an IPv4 one. Linux and the BSDs send no IPV6_HOPLIMIT for
	// it either.
	if (protocol->receive_hoplimit != 0
		&& buffer->destination->sa_family == AF_INET6) {
		TRACE("receive_hoplimit");

		if (msgControlLen < CMSG_SPACE(sizeof(int))) {
			// The datagram is still returned, without the control messages
			// that do not fit (POSIX, as on Linux and the BSDs).
			*_messageFlags |= MSG_CTRUNC;
			return bytesWritten;
		}

		// Taken from the buffer's metadata, which every clone and split
		// carries, rather than from the stored header: a queued datagram is
		// a clone, and a clone never has the header stored.
		int hopLimit = buffer->hop_limit;

		cmsghdr* messageHeader = (cmsghdr*)((char*)msgControl + bytesWritten);
		messageHeader->cmsg_len = CMSG_LEN(sizeof(int));
		messageHeader->cmsg_level = IPPROTO_IPV6;
		messageHeader->cmsg_type = IPV6_HOPLIMIT;

		memcpy(CMSG_DATA(messageHeader), &hopLimit, sizeof(int));

		bytesWritten += CMSG_SPACE(sizeof(int));
		msgControlLen -= CMSG_SPACE(sizeof(int));
	}

	if (protocol->receive_pktinfo != 0) {
		TRACE("receive_pktinfo");

		if (msgControlLen < CMSG_SPACE(sizeof(struct in6_pktinfo))) {
			*_messageFlags |= MSG_CTRUNC;
			return bytesWritten;
		}

		cmsghdr* messageHeader = (cmsghdr*)((char*)msgControl + bytesWritten);
		messageHeader->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));
		messageHeader->cmsg_level = IPPROTO_IPV6;
		messageHeader->cmsg_type = IPV6_PKTINFO;

		struct in6_pktinfo pi;
		if (buffer->destination->sa_family == AF_INET) {
			// an IPv4 datagram on a dual-stack socket
			sockaddr_in6 mapped;
			mapped_address_from_ipv4(buffer->destination, &mapped);
			memcpy(&pi.ipi6_addr, &mapped.sin6_addr, sizeof(struct in6_addr));
		} else {
			memcpy(&pi.ipi6_addr,
				&((struct sockaddr_in6*)buffer->destination)->sin6_addr,
				sizeof(struct in6_addr));
		}
		if (buffer->interface_address != NULL
			&& buffer->interface_address->interface != NULL)
			pi.ipi6_ifindex = buffer->interface_address->interface->index;
		else
			pi.ipi6_ifindex = 0;
		memcpy(CMSG_DATA(messageHeader), &pi, sizeof(struct in6_pktinfo));

		bytesWritten += CMSG_SPACE(sizeof(struct in6_pktinfo));
		msgControlLen -= CMSG_SPACE(sizeof(struct in6_pktinfo));
	}

	return bytesWritten;
}


//	#pragma mark -


status_t
init_ipv6()
{
	mutex_init(&sRawSocketsLock, "raw sockets");
	mutex_init(&sFragmentLock, "IPv4 Fragments");
	mutex_init(&sMulticastGroupsLock, "IPv6 multicast groups");
	mutex_init(&sReceivingProtocolLock, "IPv6 receiving protocols");

	status_t status;

	sMulticastState = new MulticastState();
	if (sMulticastState == NULL) {
		status = B_NO_MEMORY;
		goto err1;
	}

	status = sMulticastState->Init();
	if (status != B_OK)
		goto err2;

	new (&sFragmentHash) FragmentTable();
	new (&sFragmentPackets) FragmentPacketList;
	sFragmentMemory = 0;
	status = sFragmentHash.Init(256);
	if (status != B_OK)
		goto err3;

	new (&sRawSockets) RawSocketList;
		// static initializers do not work in the kernel,
		// so we have to do it here, manually
		// TODO: for modules, this shouldn't be required

	status = gStackModule->register_domain_protocols(AF_INET6, SOCK_RAW, 0,
		NET_IPV6_MODULE_NAME, NULL);
	if (status != B_OK)
		goto err3;

	status = gStackModule->register_domain(AF_INET6, "internet6", &gIPv6Module,
		&gIPv6AddressModule, &sDomain);
	if (status != B_OK)
		goto err3;

	TRACE("init_ipv6: OK");
	return B_OK;

err3:
	sFragmentHash.~FragmentTable();
err2:
	delete sMulticastState;
err1:
	mutex_destroy(&sReceivingProtocolLock);
	mutex_destroy(&sMulticastGroupsLock);
	mutex_destroy(&sFragmentLock);
	mutex_destroy(&sRawSocketsLock);
	TRACE("init_ipv6: error %s", strerror(status));
	return status;
}


status_t
uninit_ipv6()
{
	mutex_lock(&sReceivingProtocolLock);

	// put all the domain receiving protocols we gathered so far
	for (uint32 i = 0; i < 256; i++) {
		if (sReceivingProtocol[i] != NULL)
			gStackModule->put_domain_receiving_protocol(sDomain, i);
	}

	sFragmentHash.~FragmentTable();
	delete sMulticastState;

	gStackModule->unregister_domain(sDomain);
	mutex_unlock(&sReceivingProtocolLock);

	mutex_destroy(&sMulticastGroupsLock);
	mutex_destroy(&sFragmentLock);
	mutex_destroy(&sRawSocketsLock);
	mutex_destroy(&sReceivingProtocolLock);

	return B_OK;
}


static status_t
ipv6_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			return init_ipv6();
		case B_MODULE_UNINIT:
			return uninit_ipv6();
		default:
			return B_ERROR;
	}
}


net_protocol_module_info gIPv6Module = {
	{
		NET_IPV6_MODULE_NAME,
		0,
		ipv6_std_ops
	},
	NET_PROTOCOL_ATOMIC_MESSAGES | NET_PROTOCOL_HAS_MESSAGE_QUEUED,

	ipv6_init_protocol,
	ipv6_uninit_protocol,
	ipv6_open,
	ipv6_close,
	ipv6_free,
	ipv6_connect,
	ipv6_accept,
	ipv6_control,
	ipv6_getsockopt,
	ipv6_setsockopt,
	ipv6_bind,
	ipv6_unbind,
	ipv6_listen,
	ipv6_shutdown,
	ipv6_send_data,
	ipv6_send_routed_data,
	ipv6_send_avail,
	ipv6_read_data,
	ipv6_read_avail,
	ipv6_get_domain,
	ipv6_get_mtu,
	ipv6_receive_data,
	ipv6_deliver_data,
	ipv6_error_received,
	ipv6_error_reply,
	NULL,		// add_ancillary_data()
	NULL,		// process_ancillary_data()
	ipv6_process_ancillary_data_no_container,
	NULL,		// send_data_no_buffer()
	NULL,		// read_data_no_buffer()
	ipv6_message_queued
};

module_dependency module_dependencies[] = {
	{NET_STACK_MODULE_NAME, (module_info**)&gStackModule},
	{NET_BUFFER_MODULE_NAME, (module_info**)&gBufferModule},
	{NET_DATALINK_MODULE_NAME, (module_info**)&sDatalinkModule},
	{NET_SOCKET_MODULE_NAME, (module_info**)&sSocketModule},
	{}
};

module_info* modules[] = {
	(module_info*)&gIPv6Module,
	NULL
};
