/*
 * Copyright 2006-2009, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 *		Hugo Santos, hugosantos@gmail.com
 */


#include "EndpointManager.h"

#include <new>
#include <unistd.h>

#include <KernelExport.h>

#include <NetUtilities.h>
#include <tracing.h>

#include "TCPEndpoint.h"


//#define TRACE_ENDPOINT_MANAGER
#ifdef TRACE_ENDPOINT_MANAGER
#	define TRACE(x) dprintf x
#else
#	define TRACE(x)
#endif

#if TCP_TRACING
#	define ENDPOINT_TRACING
#endif
#ifdef ENDPOINT_TRACING
namespace EndpointTracing {

class Bind : public AbstractTraceEntry {
public:
	Bind(TCPEndpoint* endpoint, ConstSocketAddress& address, bool ephemeral)
		:
		fEndpoint(endpoint),
		fEphemeral(ephemeral)
	{
		address.AsString(fAddress, sizeof(fAddress), true);
		Initialized();
	}

	Bind(TCPEndpoint* endpoint, SocketAddress& address, bool ephemeral)
		:
		fEndpoint(endpoint),
		fEphemeral(ephemeral)
	{
		address.AsString(fAddress, sizeof(fAddress), true);
		Initialized();
	}

	virtual void AddDump(TraceOutput& out)
	{
		out.Print("tcp:e:%p bind%s address %s", fEndpoint,
			fEphemeral ? " ephemeral" : "", fAddress);
	}

protected:
	TCPEndpoint*	fEndpoint;
	char			fAddress[32];
	bool			fEphemeral;
};

class Connect : public AbstractTraceEntry {
public:
	Connect(TCPEndpoint* endpoint)
		:
		fEndpoint(endpoint)
	{
		endpoint->LocalAddress().AsString(fLocal, sizeof(fLocal), true);
		endpoint->PeerAddress().AsString(fPeer, sizeof(fPeer), true);
		Initialized();
	}

	virtual void AddDump(TraceOutput& out)
	{
		out.Print("tcp:e:%p connect local %s, peer %s", fEndpoint, fLocal,
			fPeer);
	}

protected:
	TCPEndpoint*	fEndpoint;
	char			fLocal[32];
	char			fPeer[32];
};

class Unbind : public AbstractTraceEntry {
public:
	Unbind(TCPEndpoint* endpoint)
		:
		fEndpoint(endpoint)
	{
		//fStackTrace = capture_tracing_stack_trace(10, 0, false);

		endpoint->LocalAddress().AsString(fLocal, sizeof(fLocal), true);
		endpoint->PeerAddress().AsString(fPeer, sizeof(fPeer), true);
		Initialized();
	}

#if 0
	virtual void DumpStackTrace(TraceOutput& out)
	{
		out.PrintStackTrace(fStackTrace);
	}
#endif

	virtual void AddDump(TraceOutput& out)
	{
		out.Print("tcp:e:%p unbind, local %s, peer %s", fEndpoint, fLocal,
			fPeer);
	}

protected:
	TCPEndpoint*	fEndpoint;
	//tracing_stack_trace* fStackTrace;
	char			fLocal[32];
	char			fPeer[32];
};

}	// namespace EndpointTracing

#	define T(x)	new(std::nothrow) EndpointTracing::x
#else
#	define T(x)
#endif	// ENDPOINT_TRACING


static const uint16 kLastReservedPort = 1023;
static const uint16 kFirstEphemeralPort = 40000;


/*!	Holds a manager's lock for a bind and, when the bind must also respect the
	bindings of the other address family (see _DualStackCounterpart()), that
	manager's lock for reading. The IPv4 manager's lock is always taken
	first, so that binds in the two families cannot deadlock each other.
*/
class EndpointManager::BindLocker {
public:
	BindLocker(EndpointManager* manager, EndpointManager* counterpart)
		:
		fManager(manager),
		fCounterpart(counterpart),
		fLocked(false)
	{
		Lock();
	}

	~BindLocker()
	{
		Unlock();
	}

	void Lock()
	{
		bool counterpartFirst = fCounterpart != NULL
			&& fCounterpart->Domain()->family == AF_INET;
		if (counterpartFirst)
			rw_lock_read_lock(&fCounterpart->fLock);
		rw_lock_write_lock(&fManager->fLock);
		if (fCounterpart != NULL && !counterpartFirst)
			rw_lock_read_lock(&fCounterpart->fLock);
		fLocked = true;
	}

	void Unlock()
	{
		if (!fLocked)
			return;
		if (fCounterpart != NULL)
			rw_lock_read_unlock(&fCounterpart->fLock);
		rw_lock_write_unlock(&fManager->fLock);
		fLocked = false;
	}

private:
	EndpointManager*	fManager;
	EndpointManager*	fCounterpart;
	bool				fLocked;
};


ConnectionHashDefinition::ConnectionHashDefinition(EndpointManager* manager)
	:
	fManager(manager)
{
}


size_t
ConnectionHashDefinition::HashKey(const KeyType& key) const
{
	return ConstSocketAddress(fManager->AddressModule(),
		key.first).HashPair(key.second);
}


size_t
ConnectionHashDefinition::Hash(TCPEndpoint* endpoint) const
{
	return endpoint->LocalAddress().HashPair(*endpoint->PeerAddress());
}


bool
ConnectionHashDefinition::Compare(const KeyType& key,
	TCPEndpoint* endpoint) const
{
	return endpoint->LocalAddress().EqualTo(key.first, true)
		&& endpoint->PeerAddress().EqualTo(key.second, true);
}


TCPEndpoint*&
ConnectionHashDefinition::GetLink(TCPEndpoint* endpoint) const
{
	return endpoint->fConnectionHashLink;
}


//	#pragma mark -


size_t
EndpointHashDefinition::HashKey(uint16 port) const
{
	return port;
}


size_t
EndpointHashDefinition::Hash(TCPEndpoint* endpoint) const
{
	return endpoint->LocalAddress().Port();
}


bool
EndpointHashDefinition::Compare(uint16 port, TCPEndpoint* endpoint) const
{
	return endpoint->LocalAddress().Port() == port;
}


bool
EndpointHashDefinition::CompareValues(TCPEndpoint* first,
	TCPEndpoint* second) const
{
	return first->LocalAddress().Port() == second->LocalAddress().Port();
}


TCPEndpoint*&
EndpointHashDefinition::GetLink(TCPEndpoint* endpoint) const
{
	return endpoint->fEndpointHashLink;
}


//	#pragma mark -


EndpointManager::EndpointManager(net_domain* domain)
	:
	fDomain(domain),
	fConnectionHash(this),
	fLastPort(kFirstEphemeralPort)
{
	rw_lock_init(&fLock, "TCP endpoint manager");
}


EndpointManager::~EndpointManager()
{
	rw_lock_destroy(&fLock);
}


status_t
EndpointManager::Init()
{
	status_t status = fConnectionHash.Init();
	if (status == B_OK)
		status = fEndpointHash.Init();

	return status;
}


//	#pragma mark - connections


/*!	Returns the endpoint matching the connection.
	You must hold the manager's lock when calling this method (either read or
	write).
*/
TCPEndpoint*
EndpointManager::_LookupConnection(const sockaddr* local, const sockaddr* peer)
{
	return fConnectionHash.Lookup(std::make_pair(local, peer));
}


status_t
EndpointManager::SetConnection(TCPEndpoint* endpoint, const sockaddr* _local,
	const sockaddr* peer, const sockaddr* interfaceLocal)
{
	TRACE(("EndpointManager::SetConnection(%p)\n", endpoint));

	WriteLocker _(fLock);

	SocketAddressStorage local(AddressModule());
	local.SetTo(_local);

	if (local.IsEmpty(false)) {
		uint16 port = local.Port();
		local.SetTo(interfaceLocal);
		local.SetPort(port);
	}

	// We want to create a connection for (local, peer), so check to make sure
	// that this pair is not already in use by an existing connection -- by
	// some *other* endpoint, that is. Connect() only gets here from CLOSED or
	// LISTEN, and a listener is registered against an empty peer, so the one
	// endpoint this lookup could find and must not refuse is this one: its own
	// registration from the connection that just ended. Counting that as a
	// conflict made connect() to the same peer again fail with EADDRINUSE for
	// the rest of the socket's life -- the reconnect-after-reset sequence --
	// even though nothing else holds the tuple and the removal below is
	// already there to replace the stale entry.
	TCPEndpoint* other = _LookupConnection(*local, peer);
	if (other != NULL && other != endpoint)
		return EADDRINUSE;

	endpoint->LocalAddress().SetTo(*local);
	endpoint->PeerAddress().SetTo(peer);
	T(Connect(endpoint));

	// BOpenHashTable doesn't support inserting duplicate objects. Since
	// BOpenHashTable is a chained hash table where the items are required to
	// be intrusive linked list nodes, inserting the same object twice will
	// create a cycle in the linked list, which is not handled currently.
	//
	// We need to makes sure to remove any existing copy of this endpoint
	// object from the table in order to handle calling connect() on a closed
	// socket to connect to a different remote (address, port) than it was
	// originally used for.
	//
	// We use RemoveUnchecked here because we don't want the hash table to
	// resize itself after this removal when we are planning to just add
	// another.
	fConnectionHash.RemoveUnchecked(endpoint);

	fConnectionHash.Insert(endpoint);
	return B_OK;
}


status_t
EndpointManager::SetPassive(TCPEndpoint* endpoint)
{
	SocketAddressStorage local(AddressModule());
	local.SetToEmpty();

	EndpointManager* counterpart = endpoint->IsBound()
		? NULL : _DualStackCounterpart(endpoint, *local);
	BindLocker _(this, counterpart);

	if (!endpoint->IsBound()) {
		// if the socket is unbound first bind it to ephemeral
		status_t status = _BindToEphemeral(endpoint, *local, counterpart);
		if (status < B_OK)
			return status;
	}

	SocketAddressStorage passive(AddressModule());
	passive.SetToEmpty();

	if (_LookupConnection(*endpoint->LocalAddress(), *passive))
		return EADDRINUSE;

	endpoint->PeerAddress().SetTo(*passive);
	fConnectionHash.Insert(endpoint);
	return B_OK;
}


TCPEndpoint*
EndpointManager::FindConnection(sockaddr* local, sockaddr* peer)
{
	ReadLocker _(fLock);

	TCPEndpoint *endpoint = _LookupConnection(local, peer);
	if (endpoint != NULL) {
		TRACE(("TCP: Received packet corresponds to explicit endpoint %p\n",
			endpoint));
		if (gSocketModule->acquire_socket(endpoint->socket))
			return endpoint;
	}

	// no explicit endpoint exists, check for wildcard endpoints

	SocketAddressStorage wildcard(AddressModule());
	wildcard.SetToEmpty();

	endpoint = _LookupConnection(local, *wildcard);
	if (endpoint != NULL) {
		TRACE(("TCP: Received packet corresponds to wildcard endpoint %p\n",
			endpoint));
		if (gSocketModule->acquire_socket(endpoint->socket))
			return endpoint;
	}

	SocketAddressStorage localWildcard(AddressModule());
	localWildcard.SetToEmpty();
	localWildcard.SetPort(AddressModule()->get_port(local));

	endpoint = _LookupConnection(*localWildcard, *wildcard);
	if (endpoint != NULL) {
		TRACE(("TCP: Received packet corresponds to local wildcard endpoint "
			"%p\n", endpoint));
		if (gSocketModule->acquire_socket(endpoint->socket))
			return endpoint;
	}

	// no matching endpoint exists
	TRACE(("TCP: no matching endpoint!\n"));

	return NULL;
}


/*!	Returns the IPv6 endpoint listening on \a port of :: that also takes IPv4
	connections (IPV6_V6ONLY off), if there is one, with a reference to its
	socket. This manager must be the IPv6 one; it is asked for an IPv4 segment
	that no IPv4 endpoint wants.
	Only a wildcard listener qualifies: one bound to a specific IPv6 address
	cannot be reached by an IPv4 address, and one bound to a mapped address
	lives in the IPv4 manager.
*/
TCPEndpoint*
EndpointManager::FindDualStackListener(uint16 port)
{
	ReadLocker _(fLock);

	SocketAddressStorage local(AddressModule());
	local.SetToEmpty();
	local.SetPort(port);

	SocketAddressStorage peer(AddressModule());
	peer.SetToEmpty();

	TCPEndpoint* endpoint = _LookupConnection(*local, *peer);
	if (endpoint == NULL || endpoint->State() != LISTEN
		|| is_ipv6_only(endpoint->next)
		|| !gSocketModule->acquire_socket(endpoint->socket))
		return NULL;

	TRACE(("TCP: IPv4 segment corresponds to dual-stack listener %p\n",
		endpoint));
	return endpoint;
}


//	#pragma mark - endpoints


status_t
EndpointManager::Bind(TCPEndpoint* endpoint, const sockaddr* address)
{
	// check the family
	if (!AddressModule()->is_same_family(address))
		return EAFNOSUPPORT;

	EndpointManager* counterpart = _DualStackCounterpart(endpoint, address);
	BindLocker locker(this, counterpart);

	if (AddressModule()->get_port(address) == 0)
		return _BindToEphemeral(endpoint, address, counterpart);

	return _BindToAddress(locker, endpoint, address, counterpart);
}


status_t
EndpointManager::BindChild(TCPEndpoint* endpoint, const sockaddr* address)
{
	WriteLocker _(fLock);
	return _Bind(endpoint, address);
}


/*!	Returns the manager of the other address family whose bindings a bind of
	\a endpoint to \a address here conflicts with, if any. An IPv6 socket
	bound to :: that does not refuse IPv4 (IPV6_V6ONLY off) holds its port
	for IPv4 as well, as on Linux: so any IPv4 bind has to look at the IPv6
	manager, and such an IPv6 bind at the IPv4 one. A socket bound to a
	specific IPv6 address, or with IPV6_V6ONLY on, shares nothing with IPv4.
*/
EndpointManager*
EndpointManager::_DualStackCounterpart(TCPEndpoint* endpoint,
	const sockaddr* address) const
{
	switch (Domain()->family) {
		case AF_INET:
			return existing_endpoint_manager(AF_INET6);
		case AF_INET6:
			if (AddressModule()->is_empty_address(address, false)
				&& !is_ipv6_only(endpoint->next))
				return existing_endpoint_manager(AF_INET);
			break;
	}

	return NULL;
}


/*!	Applies the rules of _BindToAddress() to the users of \a port in the other
	family's manager \a counterpart: there an IPv6 socket bound to :: that
	takes IPv4 counts as bound to INADDR_ANY, and when this bind is such a
	socket, it counts as a bind to INADDR_ANY. So, as within one family, a
	wildcard binding conflicts with any binding of the port made before it,
	and a specific binding only with a wildcard one.
	A closing local connection of the other family is not waited for, as
	_BindToAddress() does for its own: that would mean dropping both locks.
	You must hold both managers' locks.
*/
status_t
EndpointManager::_CheckDualStackConflict(EndpointManager* counterpart,
	TCPEndpoint* endpoint, uint16 port)
{
	EndpointTable::ValueIterator portUsers
		= counterpart->fEndpointHash.Lookup(port);
	while (portUsers.HasNext()) {
		TCPEndpoint* user = portUsers.Next();

		// Only a wildcard user can conflict: an IPv4 user bound to a specific
		// address was there before this wildcard bind, and an IPv6 one cannot
		// be reached by IPv4 at all.
		if (!user->LocalAddress().IsEmpty(false))
			continue;
		if (counterpart->Domain()->family == AF_INET6
			&& is_ipv6_only(user->next))
			continue;

		if ((endpoint->socket->options & SO_REUSEADDR) == 0)
			return EADDRINUSE;

		tcp_state userState = user->State();
		if (userState != TIME_WAIT && userState != CLOSED)
			return EADDRINUSE;
	}

	return B_OK;
}


/*! You must have fLock write locked when calling this method. */
status_t
EndpointManager::_BindToAddress(BindLocker& locker, TCPEndpoint* endpoint,
	const sockaddr* _address, EndpointManager* counterpart)
{
	ConstSocketAddress address(AddressModule(), _address);
	uint16 port = address.Port();

	TRACE(("EndpointManager::BindToAddress(%p)\n", endpoint));
	T(Bind(endpoint, address, false));

	// TODO: this check follows very typical UNIX semantics
	// and generally should be improved.
	if (ntohs(port) <= kLastReservedPort && geteuid() != 0)
		return B_PERMISSION_DENIED;

	bool retrying = false;
	int32 retry = 0;
	do {
		EndpointTable::ValueIterator portUsers = fEndpointHash.Lookup(port);
		retry = false;

		while (portUsers.HasNext()) {
			TCPEndpoint* user = portUsers.Next();

			if (user->LocalAddress().IsEmpty(false)
				|| address.EqualTo(*user->LocalAddress(), false)) {
				// Check if this belongs to a local connection

				// Note, while we hold our lock, the endpoint cannot go away,
				// it can only change its state - IsLocal() is safe to be used
				// without having the endpoint locked.
				tcp_state userState = user->State();
				if (user->IsLocal()
					&& (userState > ESTABLISHED || userState == CLOSED)) {
					// This is a closing local connection - wait until it's
					// gone away for real
					locker.Unlock();
					snooze(10000);
					locker.Lock();
						// TODO: make this better
					if (!retrying) {
						retrying = true;
						retry = 5;
					}
					break;
				}

				if ((endpoint->socket->options & SO_REUSEADDR) == 0)
					return EADDRINUSE;

				if (userState != TIME_WAIT && userState != CLOSED)
					return EADDRINUSE;
			}
		}
	} while (retry-- > 0);

	if (counterpart != NULL) {
		status_t status = _CheckDualStackConflict(counterpart, endpoint, port);
		if (status != B_OK)
			return status;
	}

	return _Bind(endpoint, *address);
}


/*!	You must have fLock write locked, and \a counterpart's lock if it is
	given, when calling this method. A port in use in \a counterpart at all
	is skipped: a stricter test than _CheckDualStackConflict(), which costs
	nothing when choosing a free port.
*/
status_t
EndpointManager::_BindToEphemeral(TCPEndpoint* endpoint,
	const sockaddr* address, EndpointManager* counterpart)
{
	TRACE(("EndpointManager::BindToEphemeral(%p)\n", endpoint));

	uint32 max = fLastPort + 65536;

	for (int32 i = 1; i < 5; i++) {
		// try to retrieve a more or less random port
		uint32 step = i == 4 ? 1 : (system_time() & 0x1f) + 1;
		uint32 counter = fLastPort + step;

		while (counter < max) {
			uint16 port = counter & 0xffff;
			if (port <= kLastReservedPort)
				port += kLastReservedPort;

			fLastPort = port;
			port = htons(port);

			if (!fEndpointHash.Lookup(port).HasNext()
				&& (counterpart == NULL
					|| !counterpart->fEndpointHash.Lookup(port).HasNext())) {
				// found a port
				SocketAddressStorage newAddress(AddressModule());
				newAddress.SetTo(address);
				newAddress.SetPort(port);

				TRACE(("   EndpointManager::BindToEphemeral(%p) -> %s\n",
					endpoint, AddressString(Domain(), *newAddress,
					true).Data()));
				T(Bind(endpoint, newAddress, true));

				return _Bind(endpoint, *newAddress);
			}

			counter += step;
		}
	}

	// could not find a port!
	return EADDRINUSE;
}


status_t
EndpointManager::_Bind(TCPEndpoint* endpoint, const sockaddr* address)
{
	// Thus far we have checked if the Bind() is allowed

	status_t status = endpoint->next->module->bind(endpoint->next, address);
	if (status < B_OK)
		return status;

	fEndpointHash.Insert(endpoint);

	return B_OK;
}


status_t
EndpointManager::Unbind(TCPEndpoint* endpoint)
{
	TRACE(("EndpointManager::Unbind(%p)\n", endpoint));
	T(Unbind(endpoint));

	if (endpoint == NULL || !endpoint->IsBound()) {
		TRACE(("  endpoint is unbound.\n"));
		return B_BAD_VALUE;
	}

	WriteLocker _(fLock);

	if (!fEndpointHash.Remove(endpoint))
		panic("bound endpoint %p not in hash!", endpoint);

	fConnectionHash.Remove(endpoint);

	(*endpoint->LocalAddress())->sa_len = 0;

	return B_OK;
}


status_t
EndpointManager::ReplyWithReset(tcp_segment_header& segment, net_buffer* buffer)
{
	TRACE(("TCP: Sending RST...\n"));

	net_buffer* reply = gBufferModule->create(512);
	if (reply == NULL)
		return B_NO_MEMORY;

	AddressModule()->set_to(reply->source, buffer->destination);
	AddressModule()->set_to(reply->destination, buffer->source);

	tcp_segment_header outSegment(TCP_FLAG_RESET);
	outSegment.sequence = 0;
	outSegment.acknowledge = 0;
	outSegment.advertised_window = 0;
	outSegment.urgent_offset = 0;

	if ((segment.flags & TCP_FLAG_ACKNOWLEDGE) == 0) {
		outSegment.flags |= TCP_FLAG_ACKNOWLEDGE;
		outSegment.acknowledge = segment.sequence + buffer->size;
		// TODO: Confirm:
		if ((segment.flags & (TCP_FLAG_SYNCHRONIZE | TCP_FLAG_FINISH)) != 0)
			outSegment.acknowledge++;
	} else
		outSegment.sequence = segment.acknowledge;

	status_t status = add_tcp_header(AddressModule(), outSegment, reply);
	if (status == B_OK)
		status = Domain()->module->send_data(NULL, reply);

	if (status != B_OK)
		gBufferModule->free(reply);

	return status;
}


void
EndpointManager::Dump() const
{
	kprintf("-------- TCP Domain %p ---------\n", this);
	kprintf("%10s %21s %21s %8s %8s %12s\n", "address", "local", "peer",
		"recv-q", "send-q", "state");

	ConnectionTable::Iterator iterator = fConnectionHash.GetIterator();

	while (iterator.HasNext()) {
		TCPEndpoint *endpoint = iterator.Next();

		char localBuf[64], peerBuf[64];
		endpoint->LocalAddress().AsString(localBuf, sizeof(localBuf), true);
		endpoint->PeerAddress().AsString(peerBuf, sizeof(peerBuf), true);

		kprintf("%p %21s %21s %8lu %8lu %12s\n", endpoint, localBuf, peerBuf,
			endpoint->fReceiveQueue.Available(), endpoint->fSendQueue.Used(),
			name_for_state(endpoint->State()));
	}
}

