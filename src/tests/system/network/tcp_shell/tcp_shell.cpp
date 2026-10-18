/*
 * Copyright 2006-2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "argv.h"
#include "tcp.h"
#include "pcap.h"
#include "utility.h"

#include <AutoDeleter.h>
#include <NetBufferUtilities.h>
#include <Referenceable.h>
#include <net_buffer.h>
#include <net_datalink.h>
#include <net_protocol.h>
#include <net_socket.h>
#include <net_stack.h>
#include <slab/Slab.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>

#include <KernelExport.h>
#include <Select.h>
#include <module.h>

#include <netinet/in.h>
#include <netinet/ip.h>

#include <ctype.h>
#include <errno.h>
#include <new>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


struct context {
	mutex		lock;
		// a kernel mutex rather than a BLocker: the only thing this ever
		// needed was mutual exclusion, and BLocker would make the harness
		// depend on libbe, which the host build (#686) has no equivalent of
	sem_id		wait_sem;
	struct list list;
	net_route	route;
	bool		server;
	thread_id	thread;
};

struct cmd_entry {
	const char*	name;
	void	(*func)(int argc, char **argv);
	const char*	help;
};


struct net_socket_private;
typedef DoublyLinkedList<net_socket_private> SocketList;

struct net_socket_private : net_socket,
		DoublyLinkedListLinkImpl<net_socket_private> {
	struct net_socket		*parent;
	bool					is_connected;
		// which of the parent's two lists this socket is on; socket_aborted()
		// has to remove it from the right one

	int32					ref_count;
		// What keeps the endpoint alive for as long as anyone is inside it.
		// The real stack makes net_socket_private a BWeakReferenceable and
		// socket_acquire() an increment-if-non-zero; a plain counter here
		// models the same primitive without giving this struct a vtable,
		// which would move the net_socket base off offset 0 and break the
		// C-style downcasts this harness (and the stack) do everywhere.

	team_id					owner;
	uint32					max_backlog;
	uint32					child_count;
	SocketList				pending_children;
	SocketList				connected_children;

	struct select_sync_pool	*select_pool;
	mutex					lock;
};


struct InterfaceAddress : DoublyLinkedListLinkImpl<InterfaceAddress>,
		net_interface_address, BReferenceable {
};


extern "C" status_t _add_builtin_module(module_info *info);
extern "C" status_t _get_builtin_dependencies(void);
extern bool gDebugOutputEnabled;
	// from libkernelland_emu.so

extern struct net_buffer_module_info gNetBufferModule;
	// from net_buffer.cpp
extern net_address_module_info gIPv4AddressModule;
	// from ipv4_address.cpp
extern module_info *modules[];
	// from tcp.cpp


extern struct net_protocol_module_info gDomainModule;
InterfaceAddress& gInterfaceAddress = *new InterfaceAddress();
	// Deliberately never destroyed. Every queued net_buffer holds a reference
	// to this, and a socket that close() left in TIME_WAIT keeps its queues --
	// and so its references -- until the 2MSL timer runs, which is 120 seconds
	// after the shell has exited. A static object destroyed with references
	// outstanding makes ~BReferenceable() call debugger(), so as a static this
	// aborted the process at exit in a run that had done nothing wrong: a
	// failure whose only cause was the harness looking at the wrong moment.
extern struct net_socket_module_info gNetSocketModule;
struct net_protocol_module_info *gTCPModule;
struct net_socket *gServerSocket, *gClientSocket;
static int32 sClientClosed = 0;
static int32 sServerClosed = 0;
	// see close_owned_socket()
static struct context sClientContext, sServerContext;

static int32 sPacketNumber = 1;
static double sRandomDrop = 0.0;
static std::set<uint32> sDropList;
static bigtime_t sRoundTripTime = 0;
static bool sIncreasingRoundTrip = false;
static bool sRandomRoundTrip = false;
static void (*sPacketMonitor)(net_buffer *, int32, bool) = NULL;
static int sPcapFD = -1;
static bigtime_t sStartTime;
static double sRandomReorder = 0.0;
static std::set<uint32> sReorderList;
static bool sSimultaneousConnect = false;
static bool sSimultaneousClose = false;
static bool sServerActiveClose = false;

// Spoofed inbound segments, for the RFC 5961 and RFC 9293 3.10.7.3 paths
// (#613, #614). No sequence of connect/send/drop/reorder produces one: those
// paths answer a segment no honest peer in this harness sends -- a SYN-ACK
// acknowledging something that was never sent, and an in-window SYN or reset
// that does not sit at RCV.NXT. They are what an off-path attacker puts on the
// wire, so the harness has to be able to put them there too.
//
// The bad SYN-ACK is a rewrite, the bare SYN and reset are injected copies,
// and the difference is not cosmetic. #613 wants the client to see a bogus
// SYN-ACK and nothing else while in SYN-SENT, and clobbering the honest one
// costs only a retransmit -- which is itself the thing under test, since the
// point is that the attempt survives to accept the next SYN-ACK. #614 needs an
// established connection still carrying traffic, so taking a segment off the
// wire to make room would break the stream the test is watching: the first cut
// of this ate the handshake-completing ACK one run and 1448 bytes of payload
// the next. An off-path attacker adds segments; it cannot remove them.
enum spoof_syn_acknowledge {
	SPOOF_SYN_ACKNOWLEDGE_NONE = 0,
	SPOOF_SYN_ACKNOWLEDGE_BAD_ACK,
		// a SYN-ACK acknowledging a byte that was never sent
	SPOOF_SYN_ACKNOWLEDGE_BARE_RESET
		// a bare reset, which carries no acknowledgement to validate at all
};

static int32 sSpoofBadSynAck = SPOOF_SYN_ACKNOWLEDGE_NONE;
static int32 sSpoofFlags = 0;
static int32 sSpoofOffset = 1;
static int32 sSpoofRemaining = 0;
static bool sSpoofAtServer = true;
static bool sSpoofLate = false;
	// Whether the forged copy is delivered after the honest segment instead of
	// before it. The reset at RCV.NXT - 1 is only that once the FIN it shares a
	// sequence number with has been consumed; injected ahead of that FIN the
	// same segment sits at RCV.NXT and is simply acceptable.

// What a SYN-ACK has to carry again when it is retransmitted (#830), and what
// the client's handshake-completing ACK says it understood.
//
// Nothing is forged here, unlike the two blocks above: the segment this check
// needs is one the harness already produces by itself -- the client's own
// retransmitted SYN, arriving at a server that is still in SYN-RECEIVED
// because its SYN-ACK never landed. So all the arrangement does is lose that
// SYN-ACK and compare what the server sends the second time against what it
// sent the first.
struct synchronize_acknowledge {
	uint32	sequence;
	uint32	acknowledge;
	uint16	maxSegmentSize;
	uint8	windowShift;
	bool	sackPermitted;
	bool	seen;
};

static const int32 kMaxRecordedSynAcknowledges = 2;

static bool sRecordHandshake = false;
static int32 sLoseSynAcknowledges = 0;
	// how many of the server's SYN-ACKs to lose; -1 loses all of them
static bool sLoseClientSynchronize = false;
static int32 sClientSynchronizeCount = 0;
static int32 sSynAcknowledgeCount = 0;
static int32 sSynAcknowledgeTotal = 0;
	// sSynAcknowledgeCount stops at the two that are compared; this one counts
	// every SYN-ACK, which is what a test of how many go out needs
static synchronize_acknowledge sSynAcknowledges[kMaxRecordedSynAcknowledges];
static uint32 sClientAcknowledge = 0;
static bool sClientAcknowledgeSeen = false;

static struct net_domain sDomain = {
	"ipv4",
	AF_INET,

	&gDomainModule,
	&gIPv4AddressModule
};


static bool
is_server(const sockaddr* addr)
{
	return ((sockaddr_in*)addr)->sin_port == htons(1024);
}


static uint8
tcp_segment_flags(net_buffer* buffer)
{
	NetBufferHeaderReader<tcp_header> bufferHeader(buffer);
	if (bufferHeader.Status() < B_OK)
		return bufferHeader.Status();

	tcp_header &header = bufferHeader.Data();
	return header.flags;
}


static bool
is_syn(net_buffer* buffer)
{
	return (tcp_segment_flags(buffer) & TCP_FLAG_SYNCHRONIZE) != 0;
}


static bool
is_fin(net_buffer* buffer)
{
	return (tcp_segment_flags(buffer) & TCP_FLAG_FINISH) != 0;
}


/*!	Forges a segment from \a buffer if a spoof is armed for the side that is
	about to receive it.

	Returns the forged segment, which the caller then owns, when it is to be
	delivered AFTER the honest one ("spoof ... late"); NULL otherwise, having
	either delivered it already or rewritten the honest segment in place.

	NET_BUFFER_L4_CHECKSUM_VALID is set afterwards rather than recomputing the
	checksum: tcp_receive_data() honours that flag exactly as it does for a NIC
	that checksummed in hardware, and the checksum is not what these tests are
	about.
*/
static net_buffer*
spoof_segment(net_buffer* buffer, bool server)
{
	NetBufferHeaderReader<tcp_header> bufferHeader(buffer);
	if (bufferHeader.Status() < B_OK)
		return NULL;

	tcp_header& header = bufferHeader.Data();

	if (sSpoofBadSynAck != SPOOF_SYN_ACKNOWLEDGE_NONE && !server
		&& (header.flags & (TCP_FLAG_SYNCHRONIZE | TCP_FLAG_ACKNOWLEDGE))
			== (TCP_FLAG_SYNCHRONIZE | TCP_FLAG_ACKNOWLEDGE)) {
		// The client is in SYN-SENT and this is the SYN-ACK it is waiting for.
		if (sSpoofBadSynAck == SPOOF_SYN_ACKNOWLEDGE_BARE_RESET) {
			// A bare reset: no acknowledgement, so there is nothing in it for
			// the client to validate against what it sent, and SYN-SENT has no
			// receive window to validate the sequence number against either.
			// This is the whole segment an off-path attacker needs to guess --
			// the port pair and nothing else -- so it has to be dropped
			// (RFC 9293 3.10.7.3, "Otherwise (no ACK), drop the segment and
			// return"). Rewriting the SYN-ACK rather than adding a segment
			// costs a retransmit, which is what the attempt has to survive.
			header.flags = TCP_FLAG_RESET;
			bufferHeader.Sync();

			buffer->buffer_flags |= NET_BUFFER_L4_CHECKSUM_VALID;
			sSpoofBadSynAck = SPOOF_SYN_ACKNOWLEDGE_NONE;

			puts("<**** SPOOF: bare reset, no acknowledgement, in SYN-SENT "
				"****>");
			return NULL;
		}

		// Move the acknowledgement far outside [ISS, SND.MAX] so that it
		// acknowledges a byte the client never sent, which is the segment
		// RFC 9293 3.10.7.3 is about.
		const uint32 acknowledge = header.Acknowledge() + 0x10000;
		header.acknowledge = B_HOST_TO_BENDIAN_INT32(acknowledge);
		bufferHeader.Sync();

		buffer->buffer_flags |= NET_BUFFER_L4_CHECKSUM_VALID;
		sSpoofBadSynAck = SPOOF_SYN_ACKNOWLEDGE_NONE;

		printf("<**** SPOOF: SYN-ACK acknowledging %" B_PRIu32 ", which was "
			"never sent ****>\n", acknowledge);
		return NULL;
	}

	if (sSpoofRemaining <= 0 || server != sSpoofAtServer)
		return NULL;
	if ((header.flags & TCP_FLAG_SYNCHRONIZE) != 0)
		return NULL;
		// leave the handshake alone: there is no connection to challenge yet,
		// and the states that carry one are the point of the exercise

	// In the window (the offset defaults to one byte past what the receiver is
	// waiting for) but deliberately not at RCV.NXT, which is the distinction
	// RFC 5961 turns on.
	//
	// The forged segment is a *copy*: the honest one goes on untouched behind
	// it. Rewriting in place was the first attempt and it cost the test its
	// stream, because the segment displaced was whatever happened to arrive
	// next -- the ACK that completes the handshake, or 1448 bytes the client
	// then had to resend into a hole it did not know about. An off-path
	// attacker adds a segment to the wire; it cannot take one off.
	const uint32 sequence = header.Sequence() + sSpoofOffset;
	const uint32 headerLength = header.HeaderLength();
	bufferHeader.Sync();
		// nothing was changed; this just releases the honest buffer before it
		// is duplicated

	net_buffer* spoof = gNetBufferModule.duplicate(buffer);
	if (spoof == NULL)
		return NULL;

	{
		NetBufferHeaderReader<tcp_header> spoofHeader(spoof);
		if (spoofHeader.Status() < B_OK) {
			gNetBufferModule.free(spoof);
			return NULL;
		}

		// The flags are replaced rather than added to, so what lands is the
		// bare SYN or bare reset an attacker sends, and the payload goes with
		// them -- neither carries data.
		tcp_header& forged = spoofHeader.Data();
		forged.sequence = B_HOST_TO_BENDIAN_INT32(sequence);
		forged.flags = (uint8)sSpoofFlags;
		spoofHeader.Sync();
	}

	gNetBufferModule.trim(spoof, headerLength);
	spoof->buffer_flags |= NET_BUFFER_L4_CHECKSUM_VALID;
	sSpoofRemaining--;

	printf("<**** SPOOF: bare %s at %" B_PRIu32 " (RCV.NXT + %" B_PRId32 ") to "
		"the %s, %s the honest segment ****>\n",
		(sSpoofFlags & TCP_FLAG_SYNCHRONIZE) != 0 ? "SYN" : "RST", sequence,
		sSpoofOffset, server ? "server" : "client",
		sSpoofLate ? "after" : "before");

	if (sSpoofLate)
		return spoof;

	if (sDomain.module->receive_data(spoof) < B_OK)
		gNetBufferModule.free(spoof);

	return NULL;
}


//	#pragma mark - stack


status_t
std_ops(int32, ...)
{
	return B_OK;
}


net_domain *
get_domain(int family)
{
	return &sDomain;
}


status_t
register_domain_protocols(int family, int type, int protocol, ...)
{
	return B_OK;
}


status_t
register_domain_datalink_protocols(int family, int type, ...)
{
	return B_OK;
}


static status_t
register_domain_receiving_protocol(int family, int type, const char *moduleName)
{
	return B_OK;
}


static bool
dummy_is_syscall(void)
{
	return false;
}


static bool
dummy_is_restarted_syscall(void)
{
	return false;
}


static void
dummy_store_syscall_restart_timeout(bigtime_t timeout)
{
}


static net_stack_module_info gNetStackModule = {
	{
		NET_STACK_MODULE_NAME,
		0,
		std_ops
	},
	NULL, // register_domain,
	NULL, // unregister_domain,
	get_domain,

	register_domain_protocols,
	register_domain_datalink_protocols,
	register_domain_receiving_protocol,

	NULL, // get_domain_receiving_protocol,
	NULL, // put_domain_receiving_protocol,

	NULL, // register_device_deframer,
	NULL, // unregister_device_deframer,
	NULL, // register_domain_device_handler,
	NULL, // register_device_handler,
	NULL, // unregister_device_handler,
	NULL, // register_device_monitor,
	NULL, // unregister_device_monitor,
	NULL, // device_link_changed,
	NULL, // device_removed,
	NULL, // device_enqueue_buffer,

	notify_socket,

	checksum,

	init_fifo,
	uninit_fifo,
	fifo_enqueue_buffer,
	fifo_dequeue_buffer,
	clear_fifo,
	fifo_socket_enqueue_buffer,

	init_timer,
	set_timer,
	cancel_timer,
	wait_for_timer,
	is_timer_active,
	is_timer_running,

	dummy_is_syscall,
	dummy_is_restarted_syscall,
	dummy_store_syscall_restart_timeout,
	NULL, // restore_syscall_restart_timeout

	// ancillary data is not used by TCP
};


//	#pragma mark - socket


status_t
socket_create(int family, int type, int protocol, net_socket **_socket)
{
	net_protocol* domainProtocol;

	struct net_socket_private *socket = new (std::nothrow) net_socket_private;
	if (socket == NULL)
		return B_NO_MEMORY;

	// The memset deliberately stops at the net_socket base: the two SocketList
	// members construct themselves and clearing them by hand would corrupt
	// them. But nothing then initialized net_socket_private's own scalars, so
	// parent held whatever the heap last did -- _MarkEstablished() passed it to
	// socket_connected(), which read it as a live socket and locked garbage.
	// Every connect() in this harness crashed there.
	memset(socket, 0, sizeof(net_socket));
	socket->parent = NULL;
	socket->is_connected = false;
	socket->ref_count = 1;
		// the creator's reference, handed to whoever owns the socket: the
		// shell for the two sockets it opens, the parent for a spawned child
	socket->owner = -1;
	socket->max_backlog = 0;
	socket->child_count = 0;
	socket->select_pool = NULL;

	socket->family = family;
	socket->type = type;
	socket->protocol = protocol;

	mutex_init(&socket->lock, "socket");

	// set defaults (may be overridden by the protocols)
	socket->send.buffer_size = 65535;
	socket->send.low_water_mark = 1;
	socket->send.timeout = B_INFINITE_TIMEOUT;
	socket->receive.buffer_size = 65535;
	socket->receive.low_water_mark = 1;
	socket->receive.timeout = B_INFINITE_TIMEOUT;

	socket->first_protocol = gTCPModule->init_protocol(socket);
	if (socket->first_protocol == NULL) {
		fprintf(stderr, "tcp_tester: cannot create protocol\n");
		mutex_destroy(&socket->lock);
		delete socket;
		return B_ERROR;
	}

	socket->first_info = gTCPModule;

	domainProtocol = new net_protocol;
	domainProtocol->module = &gDomainModule;
	domainProtocol->socket = socket;

	socket->first_protocol->next = domainProtocol;
	socket->first_protocol->module = gTCPModule;
	socket->first_protocol->socket = socket;

	*_socket = socket;
	return B_OK;
}


bool socket_release(net_socket* _socket);


/*!	Runs when the last reference is gone -- never directly from close().

	This is where the TCP endpoint is destroyed, so every caller that is still
	inside it has to hold a reference of its own. That is the whole point of
	the counter: close() only drops the owner's reference.
*/
void
socket_delete(net_socket *_socket)
{
	net_socket_private *socket = (net_socket_private *)_socket;

	if (socket->parent != NULL)
		panic("socket still has a parent!");

	// Children that were never accepted still carry this socket's reference;
	// unlink them and hand it back, exactly as ~net_socket_private() does.
	mutex_lock(&socket->lock);
	SocketList orphans;
	while (net_socket_private* child = socket->pending_children.RemoveHead())
		orphans.Add(child);
	while (net_socket_private* child = socket->connected_children.RemoveHead())
		orphans.Add(child);
	socket->child_count = 0;
	mutex_unlock(&socket->lock);

	while (net_socket_private* child = orphans.RemoveHead()) {
		child->parent = NULL;
		socket_release(child);
	}

	socket->first_info->uninit_protocol(socket->first_protocol);
	mutex_destroy(&socket->lock);
	delete socket;
}


int
socket_accept(net_socket *socket, struct sockaddr *address,
	socklen_t *_addressLength, net_socket **_acceptedSocket)
{
	net_socket *accepted;
	status_t status = socket->first_info->accept(socket->first_protocol,
		&accepted);
	if (status < B_OK)
		return status;

	if (address && *_addressLength > 0) {
		memcpy(address, &accepted->peer, min_c(*_addressLength,
			accepted->peer.ss_len));
		*_addressLength = accepted->peer.ss_len;
	}

	*_acceptedSocket = accepted;
	return B_OK;
}


int
socket_bind(net_socket *socket, const struct sockaddr *address, socklen_t addressLength)
{
	sockaddr empty;
	if (address == NULL) {
		// special - try to bind to an empty address, like INADDR_ANY
		memset(&empty, 0, sizeof(sockaddr));
		empty.sa_len = sizeof(sockaddr);
		empty.sa_family = socket->family;

		address = &empty;
		addressLength = sizeof(sockaddr);
	}

	if (socket->address.ss_len != 0) {
		status_t status = socket->first_info->unbind(socket->first_protocol,
			(sockaddr *)&socket->address);
		if (status < B_OK)
			return status;
	}

	memcpy(&socket->address, address, sizeof(sockaddr));

	status_t status = socket->first_info->bind(socket->first_protocol,
		(sockaddr *)address);
	if (status < B_OK) {
		// clear address again, as binding failed
		socket->address.ss_len = 0;
	}

	return status;
}


int
socket_connect(net_socket *socket, const struct sockaddr *address, socklen_t addressLength)
{
	if (address == NULL || addressLength == 0)
		return ENETUNREACH;

	if (socket->address.ss_len == 0) {
		// try to bind first
		status_t status = socket_bind(socket, NULL, 0);
		if (status < B_OK)
			return status;
	}

	return socket->first_info->connect(socket->first_protocol, address);
}


int
socket_listen(net_socket *socket, int backlog)
{
	status_t status = socket->first_info->listen(socket->first_protocol, backlog);
	if (status == B_OK)
		socket->options |= SO_ACCEPTCONN;

	return status;
}


ssize_t
socket_send(net_socket *socket, const void *data, size_t length, int flags)
{
	if (socket->peer.ss_len == 0)
		return EDESTADDRREQ;

	if (socket->address.ss_len == 0) {
		// try to bind first
		status_t status = socket_bind(socket, NULL, 0);
		if (status < B_OK)
			return status;
	}

	// TODO: useful, maybe even computed header space!
	net_buffer *buffer = gNetBufferModule.create(256);
	if (buffer == NULL)
		return ENOBUFS;

	// copy data into buffer
	if (gNetBufferModule.append(buffer, data, length) < B_OK) {
		gNetBufferModule.free(buffer);
		return ENOBUFS;
	}

	buffer->msg_flags = flags;
	memcpy(buffer->source, &socket->address, socket->address.ss_len);
	memcpy(buffer->destination, &socket->peer, socket->peer.ss_len);

	status_t status = socket->first_info->send_data(socket->first_protocol, buffer);
	if (status < B_OK) {
		gNetBufferModule.free(buffer);
		return status;
	}

	return length;
}


ssize_t
socket_recv(net_socket *socket, void *data, size_t length, int flags)
{
	net_buffer *buffer;
	ssize_t status = socket->first_info->read_data(
		socket->first_protocol, length, flags, &buffer);
	if (status < B_OK)
		return status;

	// if 0 bytes we're received, no buffer will be created
	if (buffer == NULL)
		return 0;

	ssize_t bytesReceived = buffer->size;
	gNetBufferModule.read(buffer, 0, data, bytesReceived);
	gNetBufferModule.free(buffer);

	return bytesReceived;
}


/*!	Increments the use count, but only while it is still non-zero.

	These two were stubs returning true, which meant the harness modelled no
	socket lifetime at all: EndpointManager::FindConnection() took no
	reference, so close_protocol() destroyed the endpoint under a segment that
	was already inside SegmentReceived(). It also made the harness blind to
	the defect it most needs to see -- a release that nobody owned is only
	observable if the count is real.

	Increment-if-non-zero has to be atomic for the same reason the real
	socket_acquire() uses a weak-reference upgrade: a plain increment can
	resurrect a socket whose final release has already committed to deleting
	it.
*/
bool
socket_acquire(net_socket* _socket)
{
	net_socket_private* socket = (net_socket_private*)_socket;

	int32 count = atomic_get(&socket->ref_count);
	while (count > 0) {
		const int32 found = atomic_test_and_set(&socket->ref_count, count + 1,
			count);
		if (found == count)
			return true;

		count = found;
	}

	return false;
}


bool
socket_release(net_socket* _socket)
{
	net_socket_private* socket = (net_socket_private*)_socket;

	const int32 previous = atomic_add(&socket->ref_count, -1);
	if (previous <= 0) {
		// The hazard issue 616 is about: a reference released by someone who
		// did not own it. On a real kernel this is absorbed silently whenever
		// another reference happens to be outstanding, which is why a stress
		// test that only watches for a crash cannot find it. Here it is fatal
		// on the spot.
		panic("socket %p: reference count released below zero (%" B_PRId32
			")", socket, previous - 1);
		return false;
	}

	if (previous > 1)
		return false;

	socket_delete(socket);
	return true;
}


status_t
socket_spawn_pending(net_socket *_parent, net_socket **_socket)
{
	net_socket_private *parent = (net_socket_private *)_parent;

	MutexLocker locker(parent->lock);

	// We actually accept more pending connections to compensate for those
	// that never complete, and also make sure at least a single connection
	// can always be accepted
	if (parent->child_count > 3 * parent->max_backlog / 2)
		return ENOBUFS;

	net_socket_private *socket;
	status_t status = socket_create(parent->family, parent->type, parent->protocol,
		(net_socket **)&socket);
	if (status < B_OK)
		return status;

	// inherit parent's properties
	socket->send = parent->send;
	socket->receive = parent->receive;
	socket->options = parent->options & ~SO_ACCEPTCONN;
	socket->linger = parent->linger;
	memcpy(&socket->address, &parent->address, parent->address.ss_len);
	memcpy(&socket->peer, &parent->peer, parent->peer.ss_len);

	// add to the parent's list of pending connections
	parent->pending_children.Add(socket);
	socket->parent = parent;
	parent->child_count++;

	*_socket = socket;
	return B_OK;
}


status_t
socket_dequeue_connected(net_socket *_parent, net_socket **_socket)
{
	net_socket_private *parent = (net_socket_private *)_parent;

	mutex_lock(&parent->lock);

	net_socket_private *socket = parent->connected_children.RemoveHead();
	if (socket != NULL) {
		// The child's one reference moves from the parent to the accept()ing
		// caller; acquire before unlinking so it is never momentarily zero.
		socket_acquire(socket);
		socket->parent = NULL;
		parent->child_count--;
		*_socket = socket;
	}

	mutex_unlock(&parent->lock);

	if (socket == NULL)
		return B_ENTRY_NOT_FOUND;

	socket_release(socket);
		// the parent's reference
	return B_OK;
}


ssize_t
socket_count_connected(net_socket *_parent)
{
	net_socket_private *parent = (net_socket_private *)_parent;

	MutexLocker _(parent->lock);

	return parent->connected_children.Count();
}


status_t
socket_set_max_backlog(net_socket *_socket, uint32 backlog)
{
	net_socket_private *socket = (net_socket_private *)_socket;

	// we enforce an upper limit of connections waiting to be accepted
	if (backlog > 256)
		backlog = 256;

	mutex_lock(&socket->lock);

	// first remove the pending connections, then the already connected ones as needed
	SocketList dropped;
	net_socket_private *child;
	while (socket->child_count > backlog
		&& (child = socket->pending_children.RemoveTail()) != NULL) {
		dropped.Add(child);
		socket->child_count--;
	}
	while (socket->child_count > backlog
		&& (child = socket->connected_children.RemoveTail()) != NULL) {
		dropped.Add(child);
		socket->child_count--;
	}

	socket->max_backlog = backlog;
	mutex_unlock(&socket->lock);

	// Outside the parent's lock: the last release destroys the child's
	// endpoint, which takes its own fLock. Dropping a pending child used to
	// just unlink it and leak it, and dropping a connected one deleted it
	// outright no matter who was still inside it.
	while ((child = dropped.RemoveHead()) != NULL) {
		child->parent = NULL;
		socket_release(child);
	}

	return B_OK;
}


bool
socket_has_parent(net_socket* _socket)
{
	net_socket_private* socket = (net_socket_private*)_socket;
	return socket->parent != NULL;
}


status_t
socket_connected(net_socket *socket)
{
	net_socket_private *socket_private = (net_socket_private *)socket;
	net_socket_private *parent = (net_socket_private *)socket_private->parent;
	if (parent == NULL)
		return B_BAD_VALUE;

	mutex_lock(&parent->lock);

	parent->pending_children.Remove(socket_private);
	parent->connected_children.Add(socket_private);
	socket_private->is_connected = true;

	mutex_unlock(&parent->lock);
	return B_OK;
}


/*!	Detaches an aborted child from its parent.

	TCPEndpoint::_Close() calls this for any endpoint that still has a parent,
	which is every server-side connection the harness has not dequeued yet --
	so every reset that arrives before accept() landed here, on a NULL hook,
	and took the process down.

	Like the real socket_aborted(), it also hands back the parent's reference.
	That cannot be the last one: the caller is inside the child's
	SegmentReceived(), and the lookup that got it there holds a reference of
	its own.
*/
status_t
socket_set_aborted(net_socket *_socket)
{
	net_socket_private *socket = (net_socket_private *)_socket;
	net_socket_private *parent = (net_socket_private *)socket->parent;
	if (parent == NULL)
		return B_BAD_VALUE;

	MutexLocker _(parent->lock);

	if (socket->is_connected)
		parent->connected_children.Remove(socket);
	else
		parent->pending_children.Remove(socket);

	parent->child_count--;
	socket->parent = NULL;
	socket_release(socket);

	return B_OK;
}


status_t
socket_notify(net_socket *_socket, uint8 event, int32 value)
{
	net_socket_private *socket = (net_socket_private *)_socket;

	mutex_lock(&socket->lock);

	bool notify = true;

	switch (event) {
		case B_SELECT_READ:
		{
			if ((ssize_t)socket->receive.low_water_mark > value && value >= B_OK)
				notify = false;
			break;
		}
		case B_SELECT_WRITE:
		{
			if ((ssize_t)socket->send.low_water_mark > value && value >= B_OK)
				notify = false;
			break;
		}
		case B_SELECT_ERROR:
			socket->error = value;
			break;
	}

	if (notify)
		;

	mutex_unlock(&socket->lock);
	return B_OK;
}


net_socket_module_info gNetSocketModule = {
	{
		NET_SOCKET_MODULE_NAME,
		0,
		std_ops
	},
	NULL, // open,
	NULL, // close,
	NULL, // free,

	NULL, // control,

	NULL, // read_avail,
	NULL, // send_avail,

	NULL, // send_data,
	NULL, // receive_data,

	NULL, // get_option,
	NULL, // set_option,
	NULL, // get_next_stat,

	socket_acquire,
	socket_release,

	// connections
	socket_spawn_pending,
	socket_dequeue_connected,
	socket_count_connected,
	socket_set_max_backlog,
	socket_has_parent,
	socket_connected,
	socket_set_aborted,

	// notifications
	NULL, // request_notification,
	NULL, // cancel_notification,
	socket_notify,

	// standard socket API
	NULL, // accept,
	NULL, // bind,
	NULL, // connect,
	NULL, // getpeername,
	NULL, // getsockname,
	NULL, // getsockopt,
	NULL, // listen,
	NULL, // receive,
	NULL, // send,
	NULL, // setsockopt,
	NULL, // shutdown,
	NULL, // socketpair
};


//	#pragma mark - protocol


net_protocol*
init_protocol(net_socket** _socket)
{
	net_socket *socket;
	status_t status = socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &socket);
	if (status < B_OK)
		return NULL;

	status = socket->first_info->open(socket->first_protocol);
	if (status < B_OK) {
		fprintf(stderr, "tcp_tester: cannot open client: %s\n", strerror(status));
		socket_delete(socket);
		return NULL;
	}

	*_socket = socket;
	return socket->first_protocol;
}


/*!	What close() does to a socket, and nothing more.

	It used to uninit the protocol inline -- destroying the endpoint the moment
	free() returned, regardless of who was still inside it. The real stack
	splits this the way it is split here: socket_close() runs the protocol's
	close(), socket_free() runs its free() and then drops the owner's
	reference, and the endpoint is destroyed by whichever reference turns out
	to be the last (see socket_delete()). TCPEndpoint::Free() takes a
	reference of its own for the 2MSL wait, so for a socket entering TIME_WAIT
	the release below is deliberately not the last one.
*/
bool
close_protocol(net_protocol* protocol)
{
	net_socket* socket = protocol->socket;

	gTCPModule->close(protocol);
	gTCPModule->free(protocol);
	return socket_release(socket);
}


/*!	Gives back the shell's own reference to one of its two sockets, once.

	The client can be closed either by "quit" or by the simultaneous-close path
	in the receiving thread, and the shell holds exactly one reference. Closing
	twice would be an over-release by the harness itself -- indistinguishable,
	in the report, from the stack defect this harness exists to look for.

	It also reports whether the endpoint outlived the close, which is the one
	thing a liveness test cannot see. A connection that still owes its peer a
	FIN has to take a reference of its own in TCPEndpoint::Free() and so must
	survive this release; "destroyed" there means Free() took none.
*/
void
close_owned_socket(net_socket* socket, const char* name, int32* closed)
{
	if (atomic_get_and_set(closed, 1) != 0)
		return;

	const bool destroyed = close_protocol(socket->first_protocol);
	printf("%s: close gave back the shell's reference; endpoint %s\n", name,
		destroyed ? "destroyed" : "still alive");
}


//	#pragma mark - datalink


status_t
datalink_send_data(struct net_route *route, net_buffer *buffer)
{
	struct context* context = (struct context*)route->gateway;

	buffer->interface_address = &gInterfaceAddress;
	gInterfaceAddress.AcquireReference();

	mutex_lock(&context->lock);
	list_add_item(&context->list, buffer);
	mutex_unlock(&context->lock);

	release_sem(context->wait_sem);
	return B_OK;
}


status_t
datalink_send_datagram(net_protocol *protocol, net_domain *domain,
	net_buffer *buffer)
{
	panic("called");
	return B_ERROR;
}


struct net_route *
get_route(struct net_domain *_domain, const struct sockaddr *address)
{
	if (is_server(address)) {
		// to the server
		return &sServerContext.route;
	}

	return &sClientContext.route;
}


static void
put_route(struct net_domain* _domain, net_route* route)
{
}


net_datalink_module_info gNetDatalinkModule = {
	{
		NET_DATALINK_MODULE_NAME,
		0,
		std_ops
	},

	NULL, // control
	datalink_send_data,
	datalink_send_datagram,

	NULL, // is_local_address
	NULL, // is_local_link_address

	NULL, // get_interface
	NULL, // get_interface_with_address
	NULL, // put_interface

	NULL, // get_interface_address
	NULL, // get_next_interface_address,
	NULL, // put_interface_address

	NULL, // join_multicast
	NULL, // leave_multicast

	NULL, // add_route,
	NULL, // remove_route,
	get_route,
	NULL, // get_buffer_route
	put_route,
	NULL, // register_route_info,
	NULL, // unregister_route_info,
	NULL, // update_route_info
};


//	#pragma mark - domain


status_t
domain_open(net_protocol *protocol)
{
	return B_OK;
}


status_t
domain_close(net_protocol *protocol)
{
	return B_OK;
}


status_t
domain_free(net_protocol *protocol)
{
	return B_OK;
}


status_t
domain_connect(net_protocol *protocol, const struct sockaddr *address)
{
	return B_ERROR;
}


status_t
domain_accept(net_protocol *protocol, struct net_socket **_acceptedSocket)
{
	return EOPNOTSUPP;
}


status_t
domain_control(net_protocol *protocol, int level, int option, void *value,
	size_t *_length)
{
	return B_ERROR;
}


status_t
domain_bind(net_protocol *protocol, const struct sockaddr *address)
{
	memcpy(&protocol->socket->address, address, sizeof(struct sockaddr_in));
	protocol->socket->address.ss_len = sizeof(struct sockaddr_in);
		// explicitly set length, as our callers can't be trusted to
		// always provide the correct length!
	return B_OK;
}


status_t
domain_unbind(net_protocol *protocol, struct sockaddr *address)
{
	return B_OK;
}


status_t
domain_listen(net_protocol *protocol, int count)
{
	return EOPNOTSUPP;
}


status_t
domain_shutdown(net_protocol *protocol, int direction)
{
	return EOPNOTSUPP;
}


status_t
domain_send_data(net_protocol *protocol, net_buffer *buffer)
{
	// find route
	struct net_route *route = get_route(&sDomain, (sockaddr *)&buffer->destination);
	if (route == NULL)
		return ENETUNREACH;

	return datalink_send_data(route, buffer);
}


status_t
domain_send_routed_data(net_protocol *protocol, struct net_route *route,
	net_buffer *buffer)
{
	return datalink_send_data(route, buffer);
}


ssize_t
domain_send_avail(net_protocol *protocol)
{
	return B_ERROR;
}


status_t
domain_read_data(net_protocol *protocol, size_t numBytes, uint32 flags,
	net_buffer **_buffer)
{
	return B_ERROR;
}


ssize_t
domain_read_avail(net_protocol *protocol)
{
	return B_ERROR;
}


struct net_domain *
domain_get_domain(net_protocol *protocol)
{
	return &sDomain;
}


size_t
domain_get_mtu(net_protocol *protocol, const struct sockaddr *address)
{
	return 1480;
		// 1500 ethernet - IPv4 header
}


/*!	Accepts and ignores every option.

	TCPEndpoint::_PrepareSendPath() sets IP_DONTFRAG on the layer below for
	every connection, so leaving this NULL made connect() call through a null
	pointer -- the harness crashed before it could open anything. Nothing in
	the emulated path fragments, so accepting the option without recording it
	is accurate rather than merely convenient.
*/
status_t
domain_setsockopt(net_protocol *protocol, int level, int option,
	const void *value, int length)
{
	return B_OK;
}


status_t
domain_getsockopt(net_protocol *protocol, int level, int option, void *value,
	int *_length)
{
	return B_ERROR;
}


/*!	Records the handshake segments the #830 check compares.

	Done here rather than from a packet monitor so the check does not depend on
	which dump mode the run happens to be in, and so that it sees the SYN-ACK
	that is about to be dropped -- which is the one every comparison is made
	against.
*/
static void
record_handshake(net_buffer* buffer)
{
	NetBufferHeaderReader<tcp_header> bufferHeader(buffer);
	if (bufferHeader.Status() < B_OK)
		return;

	tcp_header& header = bufferHeader.Data();
	const uint8 flags = header.flags;
	const uint8 handshake = TCP_FLAG_SYNCHRONIZE | TCP_FLAG_ACKNOWLEDGE;

	if (!is_server((sockaddr*)buffer->source)) {
		if ((flags & handshake) == TCP_FLAG_SYNCHRONIZE)
			sClientSynchronizeCount++;

		// The first bare acknowledgement from the client is the one that
		// completes the handshake, and what it acknowledges is the server's
		// initial sequence number as the client understood it -- the same
		// off-by-one seen from the other end, and the one that costs the
		// stream its first byte.
		if ((flags & handshake) == TCP_FLAG_ACKNOWLEDGE
			&& !sClientAcknowledgeSeen) {
			sClientAcknowledge = header.Acknowledge();
			sClientAcknowledgeSeen = true;
		}
		return;
	}

	if ((flags & handshake) != handshake)
		return;

	sSynAcknowledgeTotal++;

	if (sSynAcknowledgeCount >= kMaxRecordedSynAcknowledges)
		return;

	synchronize_acknowledge& record = sSynAcknowledges[sSynAcknowledgeCount++];
	record.seen = true;
	record.sequence = header.Sequence();
	record.acknowledge = header.Acknowledge();
	record.maxSegmentSize = 0;
	record.windowShift = 0;
	record.sackPermitted = false;

	int32 size = header.HeaderLength() - sizeof(tcp_header);
	if (size <= 0)
		return;

	uint8 optionsBuffer[64];
	if (size > (int32)sizeof(optionsBuffer))
		size = sizeof(optionsBuffer);
	if (gBufferModule->read(buffer, sizeof(tcp_header), optionsBuffer, size)
			!= B_OK)
		return;

	tcp_option* option = (tcp_option*)optionsBuffer;
	while (size > 0 && option->kind != TCP_OPTION_END) {
		uint32 length = 1;
		switch (option->kind) {
			case TCP_OPTION_NOP:
				break;
			case TCP_OPTION_MAX_SEGMENT_SIZE:
				record.maxSegmentSize = ntohs(option->max_segment_size);
				length = 4;
				break;
			case TCP_OPTION_WINDOW_SHIFT:
				record.windowShift = option->window_shift;
				length = 3;
				break;
			case TCP_OPTION_SACK_PERMITTED:
				record.sackPermitted = true;
				length = 2;
				break;

			default:
				length = option->length;
				// make sure we don't end up in an endless loop
				if (length == 0)
					size = 0;
				break;
		}

		size -= length;
		option = (tcp_option*)((uint8*)option + length);
	}
}


status_t
domain_receive_data(net_buffer *buffer)
{
	uint32 packetNumber = atomic_add(&sPacketNumber, 1);

	bool drop = false;
	if (sDropList.find(packetNumber) != sDropList.end()
		|| (sRandomDrop > 0.0 && (1.0 * rand() / RAND_MAX) > sRandomDrop))
		drop = true;

	// Losing the SYN-ACK by what it is rather than by its packet number: the
	// number depends on how many segments precede the handshake, which is not
	// something a test of the handshake should have to know.
	if (sLoseSynAcknowledges != 0 && is_server((sockaddr*)buffer->source)
		&& (tcp_segment_flags(buffer)
				& (TCP_FLAG_SYNCHRONIZE | TCP_FLAG_ACKNOWLEDGE))
			== (TCP_FLAG_SYNCHRONIZE | TCP_FLAG_ACKNOWLEDGE)) {
		if (sLoseSynAcknowledges > 0)
			sLoseSynAcknowledges--;
		drop = true;
	}

	// Losing the client's SYN retransmissions as well leaves the server's own
	// retransmission of its SYN-ACK as the only thing that can complete the
	// handshake, which is what makes that a test rather than a race: both ends
	// start from the same one-second initial timeout, so watching which RTO
	// fires first would decide nothing.
	if (sLoseClientSynchronize && sClientSynchronizeCount > 0
		&& !is_server((sockaddr*)buffer->source)
		&& (tcp_segment_flags(buffer)
				& (TCP_FLAG_SYNCHRONIZE | TCP_FLAG_ACKNOWLEDGE))
			== TCP_FLAG_SYNCHRONIZE) {
		drop = true;
	}

	if (sRecordHandshake)
		record_handshake(buffer);

	if (!drop && (sRoundTripTime > 0 || sRandomRoundTrip || sIncreasingRoundTrip)) {
		bigtime_t add = 0;
		if (sRandomRoundTrip)
			add = (bigtime_t)(1.0 * rand() / RAND_MAX * 500000) - 250000;
		if (sIncreasingRoundTrip)
			sRoundTripTime += (bigtime_t)(1.0 * rand() / RAND_MAX * 150000);

		snooze(sRoundTripTime / 2 + add);
	}

	if (sPacketMonitor != NULL) {
		sPacketMonitor(buffer, packetNumber, drop);
	} else if (drop)
		printf("<**** DROPPED %ld ****>\n", packetNumber);

	if (drop) {
		gNetBufferModule.free(buffer);
		return B_OK;
	}

	return gTCPModule->receive_data(buffer);
}


status_t
domain_error(net_error error, net_error_data* errorData, net_buffer* data)
{
	return B_ERROR;
}


status_t
domain_error_reply(net_protocol* self, net_buffer* cause,
	net_error error, net_error_data* errorData)
{
	return B_ERROR;
}


net_protocol_module_info gDomainModule = {
	{
		NULL,
		0,
		std_ops
	},
	NET_PROTOCOL_ATOMIC_MESSAGES,

	NULL, // init
	NULL, // uninit
	domain_open,
	domain_close,
	domain_free,
	domain_connect,
	domain_accept,
	domain_control,
	domain_getsockopt,
	domain_setsockopt,
	domain_bind,
	domain_unbind,
	domain_listen,
	domain_shutdown,
	domain_send_data,
	domain_send_routed_data,
	domain_send_avail,
	domain_read_data,
	domain_read_avail,
	domain_get_domain,
	domain_get_mtu,
	domain_receive_data,
	NULL, // deliver_data
	domain_error,
	domain_error_reply,
	NULL, // attach_ancillary_data
	NULL, // process_ancillary_data
};


//	#pragma mark - packet capture


static void
dump_printf(net_buffer* buffer, int32 packetNumber, bool willBeDropped)
{
	static bigtime_t lastTime = 0;

	NetBufferHeaderReader<tcp_header> bufferHeader(buffer);
	if (bufferHeader.Status() < B_OK)
		return;

	tcp_header &header = bufferHeader.Data();

	bigtime_t now = system_time();
	if (lastTime == 0)
		lastTime = now;

	printf("\33[0m% 3ld %8.6f (%8.6f) ", packetNumber, (now - sStartTime) / 1000000.0,
		(now - lastTime) / 1000000.0);
	lastTime = now;

	if (is_server((sockaddr *)buffer->source))
		printf("\33[31mserver > client: ");
	else
		printf("client > server: ");

	int32 length = buffer->size - header.HeaderLength();

	if ((header.flags & TCP_FLAG_PUSH) != 0)
		putchar('P');
	if ((header.flags & TCP_FLAG_SYNCHRONIZE) != 0)
		putchar('S');
	if ((header.flags & TCP_FLAG_FINISH) != 0)
		putchar('F');
	if ((header.flags & TCP_FLAG_RESET) != 0)
		putchar('R');
	if ((header.flags
		& (TCP_FLAG_SYNCHRONIZE | TCP_FLAG_FINISH | TCP_FLAG_PUSH | TCP_FLAG_RESET)) == 0)
		putchar('.');

	printf(" %lu:%lu (%lu)", header.Sequence(), header.Sequence() + length, length);
	if ((header.flags & TCP_FLAG_ACKNOWLEDGE) != 0)
		printf(" ack %lu", header.Acknowledge());

	printf(" win %u", header.AdvertisedWindow());

	if (header.HeaderLength() > sizeof(tcp_header)) {
		int32 size = header.HeaderLength() - sizeof(tcp_header);

		tcp_option *option;
		uint8 optionsBuffer[1024];
		if (gBufferModule->direct_access(buffer, sizeof(tcp_header),
				size, (void **)&option) != B_OK) {
			if (size > 1024) {
				printf("options too large to take into account (%ld bytes)\n", size);
				size = 1024;
			}

			gBufferModule->read(buffer, sizeof(tcp_header), optionsBuffer, size);
			option = (tcp_option *)optionsBuffer;
		}

		while (size > 0) {
			uint32 length = 1;
			switch (option->kind) {
				case TCP_OPTION_END:
				case TCP_OPTION_NOP:
					break;
				case TCP_OPTION_MAX_SEGMENT_SIZE:
					printf(" <mss %u>", ntohs(option->max_segment_size));
					length = 4;
					break;
				case TCP_OPTION_WINDOW_SHIFT:
					printf(" <ws %u>", option->window_shift);
					length = 3;
					break;
				case TCP_OPTION_TIMESTAMP:
					printf(" <ts %lu:%lu>", option->timestamp.value, option->timestamp.reply);
					length = 10;
					break;

				default:
					length = option->length;
					// make sure we don't end up in an endless loop
					if (length == 0)
						size = 0;
					break;
			}

			size -= length;
			option = (tcp_option *)((uint8 *)option + length);
		}
	}

	if (willBeDropped)
		printf(" <DROPPED>");
	printf("\33[0m\n");
}


static void
dump_pcap(net_buffer* buffer, int32 packetNumber, bool willBeDropped)
{
	if (willBeDropped) {
		printf(" <DROPPED>\n");
		return;
	}

	const bigtime_t time = real_time_clock_usecs();

	struct pcap_packet_header pcap_header;
	pcap_header.ts_sec = time / 1000000;
	pcap_header.ts_usec = time % 1000000;
	pcap_header.included_len = sizeof(struct ip) + buffer->size;
	pcap_header.original_len = pcap_header.included_len;

	struct ip ip_header;
	ip_header.ip_v = IPVERSION;
	ip_header.ip_hl = sizeof(struct ip) >> 2;
	ip_header.ip_tos = 0;
	ip_header.ip_len = htons(sizeof(struct ip) + buffer->size);
	ip_header.ip_id = htons(packetNumber);
	ip_header.ip_off = 0;
	ip_header.ip_ttl = 254;
	ip_header.ip_p = IPPROTO_TCP;
	ip_header.ip_sum = 0;
	ip_header.ip_src.s_addr = ((sockaddr_in*)buffer->source)->sin_addr.s_addr;
	ip_header.ip_dst.s_addr = ((sockaddr_in*)buffer->destination)->sin_addr.s_addr;

	size_t count = 16, used = 0;
	iovec vecs[count];

	vecs[used].iov_base = &pcap_header;
	vecs[used].iov_len = sizeof(pcap_header);
	used++;

	vecs[used].iov_base = &ip_header;
	vecs[used].iov_len = sizeof(ip_header);
	used++;

	used += gNetBufferModule.get_iovecs(buffer, vecs + used, count - used);

	static mutex writesLock = MUTEX_INITIALIZER("pcap writes");
	MutexLocker _(writesLock);
	ssize_t written = writev(sPcapFD, vecs, used);
	if (written != (pcap_header.included_len + sizeof(pcap_packet_header))) {
		fprintf(stderr, "writing to pcap file failed\n");
		exit(1);
	}
}


static bool
setup_dump_pcap(const char* file)
{
	sPcapFD = open(file, O_CREAT | O_WRONLY | O_TRUNC);
	if (sPcapFD < 0) {
		fprintf(stderr, "tcp_shell: Failed to open output pcap file: %d\n",
			errno);
		return false;
	}

	struct pcap_header header;
	header.magic = PCAP_MAGIC;
	header.version_major = 2;
	header.version_minor = 4;
	header.timezone = 0;
	header.timestamp_accuracy = 0;
	header.max_packet_length = 65535;
	header.linktype = PCAP_LINKTYPE_IPV4;
	if (write(sPcapFD, &header, sizeof(header)) != sizeof(header)) {
		fprintf(stderr, "tcp_shell: Failed to write pcap file header: %d\n",
			errno);
		return false;
	}

	sPacketMonitor = dump_pcap;
	return true;
}


//	#pragma mark - test


int32
receiving_thread(void* _data)
{
	struct context* context = (struct context*)_data;
	struct net_buffer* reorderBuffer = NULL;

	while (true) {
		status_t status;
		do {
			status = acquire_sem(context->wait_sem);
		} while (status == B_INTERRUPTED);

		if (status < B_OK)
			break;

		while (true) {
			mutex_lock(&context->lock);
			net_buffer* buffer = (net_buffer*)list_remove_head_item(
				&context->list);
			mutex_unlock(&context->lock);

			if (buffer == NULL)
				break;

			net_buffer* lateSpoof = spoof_segment(buffer, context->server);

			if (sSimultaneousConnect && context->server && is_syn(buffer)) {
				// delay getting the SYN request, and connect as well
				sockaddr_in address;
				memset(&address, 0, sizeof(address));
				address.sin_family = AF_INET;
				address.sin_port = htons(1023);
				address.sin_addr.s_addr = htonl(0xc0a80001);

				status_t status = socket_connect(gServerSocket,
					(struct sockaddr *)&address, sizeof(struct sockaddr));
				if (status < B_OK)
					fprintf(stderr, "tcp_tester: simultaneous connect failed: %s\n", strerror(status));

				sSimultaneousConnect = false;
			}
			if (sSimultaneousClose && !context->server && is_fin(buffer)) {
				close_owned_socket(gClientSocket, "client", &sClientClosed);
				sSimultaneousClose = false;
			}
			if ((sRandomReorder > 0.0
					|| sReorderList.find(sPacketNumber) != sReorderList.end())
				&& reorderBuffer == NULL
				&& (1.0 * rand() / RAND_MAX) > sRandomReorder) {
				reorderBuffer = buffer;
			} else {
				if (sDomain.module->receive_data(buffer) < B_OK)
					gNetBufferModule.free(buffer);

				if (reorderBuffer != NULL) {
					if (sDomain.module->receive_data(reorderBuffer) < B_OK)
						gNetBufferModule.free(reorderBuffer);
					reorderBuffer = NULL;
				}
			}

			if (lateSpoof != NULL) {
				// Behind the honest segment on purpose, which is what
				// "spoof ... late" asked for: receive_data() above runs the
				// whole input path inline, so RCV.NXT has already moved
				// past it. Do not combine this with reorder or drop on the
				// same segment -- the honest one would then not have gone
				// in first, which is the entire point.
				if (sDomain.module->receive_data(lateSpoof) < B_OK)
					gNetBufferModule.free(lateSpoof);
			}
		}
	}

	return 0;
}


int32
server_thread(void*)
{
	// A reference for as long as this thread is inside the listening socket,
	// which is the job the accept()ing file descriptor does in the real
	// kernel. Without it, the shell's own "quit" (close_protocol() on the
	// listener) destroys the endpoint while Accept() is parked on
	// fAcceptSemaphore, and the re-lock of fLock on the way out touches a
	// destroyed mutex.
	if (!socket_acquire(gServerSocket))
		return 0;

	while (true) {
		// main accept() loop
		net_socket* connectionSocket;
		sockaddr_in address;
		socklen_t size = sizeof(struct sockaddr_in);
		status_t status = socket_accept(gServerSocket,
			(struct sockaddr *)&address, &size, &connectionSocket);
		if (status < B_OK) {
			fprintf(stderr, "SERVER: accepting failed: %s\n", strerror(status));
			break;
		}

		printf("server: got connection from %08x\n", address.sin_addr.s_addr);

		char buffer[1024];
		ssize_t bytesRead;
		bool activeClose = false;
		while ((bytesRead = socket_recv(connectionSocket, buffer,
				sizeof(buffer), 0)) > 0) {
			printf("server: received %ld bytes\n", bytesRead);

			if (sServerActiveClose) {
				printf("server: active close\n");
				activeClose = true;
				break;
			}
		}

		if (activeClose) {
			close_protocol(connectionSocket->first_protocol);
			break;
		}

		if (bytesRead < 0)
			printf("server: receiving failed: %s\n", strerror(bytesRead));
		else
			printf("server: peer closed connection.\n");

		snooze(1000000);
		close_protocol(connectionSocket->first_protocol);
	}

	socket_release(gServerSocket);
	return 0;
}


void
setup_server()
{
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_len = sizeof(sockaddr_in);
	address.sin_family = AF_INET;
	address.sin_port = htons(1024);
	address.sin_addr.s_addr = INADDR_ANY;

	status_t status = socket_bind(gServerSocket, (struct sockaddr*)&address,
		sizeof(struct sockaddr));
	if (status < B_OK) {
		fprintf(stderr, "tcp_tester: cannot bind server: %s\n", strerror(status));
		exit(1);
	}
	status = socket_listen(gServerSocket, 40);
	if (status < B_OK) {
		fprintf(stderr, "tcp_tester: server cannot listen: %s\n",
			strerror(status));
		exit(1);
	}

	thread_id serverThread = spawn_thread(server_thread, "server",
		B_NORMAL_PRIORITY, NULL);
	if (serverThread < B_OK) {
		fprintf(stderr, "tcp_tester: cannot start server: %s\n",
			strerror(serverThread));
		exit(1);
	}

	resume_thread(serverThread);
}


void
setup_context(struct context& context, bool server)
{
	mutex_init(&context.lock, server ? "server context" : "client context");
	list_init(&context.list);
	context.route.interface_address = &gInterfaceAddress;
	context.route.gateway = (sockaddr *)&context;
		// backpointer to the context
	context.route.mtu = 1500;
	context.server = server;
	context.wait_sem = create_sem(0, "receive wait");

	context.thread = spawn_thread(receiving_thread,
		server ? "server receiver" : "client receiver", B_NORMAL_PRIORITY,
		&context);
	resume_thread(context.thread);
}


void
cleanup_context(struct context& context)
{
	delete_sem(context.wait_sem);

	status_t status;
	wait_for_thread(context.thread, &status);

	mutex_destroy(&context.lock);
}


//  #pragma mark -


static void do_help(int argc, char** argv);


static void
do_connect(int argc, char** argv)
{
	sSimultaneousConnect = false;

	int port = 1024;
	if (argc > 1) {
		if (!strcmp(argv[1], "-s"))
			sSimultaneousConnect = true;
		else if (isdigit(argv[1][0]))
			port = atoi(argv[1]);
		else {
			fprintf(stderr, "usage: connect [-s|<port-number>]\n");
			return;
		}
	}

	if (sSimultaneousConnect) {
		// bind to a port first, so the other end can find us
		sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons(1023);
		address.sin_addr.s_addr = htonl(0xc0a80001);

		status_t status = socket_bind(gClientSocket, (struct sockaddr *)&address,
			sizeof(struct sockaddr));
		if (status < B_OK) {
			fprintf(stderr, "Could not bind to port 1023: %s\n", strerror(status));
			sSimultaneousConnect = false;
			return;
		}
	}

	sStartTime = system_time();

	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(0xc0a80001);

	status_t status = socket_connect(gClientSocket, (struct sockaddr *)&address,
		sizeof(struct sockaddr));
	if (status < B_OK)
		fprintf(stderr, "tcp_tester: could not connect: %s\n", strerror(status));
}


static ssize_t
parse_size(const char* arg)
{
	char *unit;
	ssize_t size = strtoul(arg, &unit, 0);
	if (unit != NULL && unit[0]) {
		if (unit[0] == 'k' || unit[0] == 'K')
			size *= 1024;
		else if (unit[0] == 'm' || unit[0] == 'M')
			size *= 1024 * 1024;
		else {
			fprintf(stderr, "unknown unit specified!\n");
			return -1;
		}
	}
	return size;
}


static void
do_send(int argc, char** argv)
{
	ssize_t size = 1024;
	if (argc > 1 && isdigit(argv[1][0])) {
		size = parse_size(argv[1]);
		if (size < 0)
			return;
	} else if (argc > 1) {
		fprintf(stderr, "invalid args!\n");
		return;
	}

	if (size > 4 * 1024 * 1024) {
		printf("amount to send will be limited to 4 MB\n");
		size = 4 * 1024 * 1024;
	}

	char *buffer = (char *)malloc(size);
	if (buffer == NULL) {
		fprintf(stderr, "not enough memory!\n");
		return;
	}
	MemoryDeleter bufferDeleter(buffer);

	// initialize buffer with some not so random data
	for (uint32 i = 0; i < size; i++) {
		buffer[i] = (char)(i & 0xff);
	}

	ssize_t bytesWritten = socket_send(gClientSocket, buffer, size, 0);
	if (bytesWritten < B_OK) {
		fprintf(stderr, "failed sending buffer: %s\n", strerror(bytesWritten));
		return;
	}
}


static void
do_send_loop(int argc, char** argv)
{
	if (argc != 2 || !isdigit(argv[1][0])) {
		fprintf(stderr, "invalid args!\n");
		return;
	}
	ssize_t size = parse_size(argv[1]);
	if (size < 0)
		return;

	const size_t bufferSize = 4096;
	char *buffer = (char *)malloc(bufferSize);
	if (buffer == NULL) {
		fprintf(stderr, "not enough memory!\n");
		return;
	}
	MemoryDeleter bufferDeleter(buffer);

	// initialize buffer with some not so random data
	for (uint32 i = 0; i < bufferSize; i++) {
		buffer[i] = (char)(i & 0xff);
	}

	for (ssize_t total = 0; total < size; ) {
		ssize_t bytesWritten = socket_send(gClientSocket, buffer, bufferSize, 0);
		if (bytesWritten < B_OK) {
			fprintf(stderr, "failed sending buffer (after %" B_PRIdSSIZE "): %s\n",
				total, strerror(bytesWritten));
			return;
		}

		total += bufferSize;
	}
}


static void
do_close(int argc, char** argv)
{
	sSimultaneousClose = false;
	sServerActiveClose = true;

	if (argc > 1) {
		if (!strcmp(argv[1], "-s"))
			sSimultaneousClose = true;
		else {
			fprintf(stderr, "usage: close [-s]\n");
			return;
		}
	}

	gClientSocket->send.timeout = 0;

	char buffer[32767] = {'q'};
	ssize_t bytesWritten = socket_send(gClientSocket, buffer, sizeof(buffer), 0);
	if (bytesWritten < B_OK) {
		fprintf(stderr, "failed sending buffer: %s\n", strerror(bytesWritten));
		return;
	}
}


static void
do_drop(int argc, char** argv)
{
	if (argc == 1) {
		// show list of dropped packets
		if (sRandomDrop > 0.0)
			printf("Drop probability is %f\n", sRandomDrop);

		printf("Drop pakets:\n");

		std::set<uint32>::iterator iterator = sDropList.begin();
		uint32 count = 0;
		for (; iterator != sDropList.end(); iterator++) {
			printf("%4lu\n", *iterator);
			count++;
		}

		if (count == 0)
			printf("<empty>\n");
	} else if (!strcmp(argv[1], "-f")) {
		// flush drop list
		sDropList.clear();
		puts("drop list cleared.");
	} else if (!strcmp(argv[1], "-r")) {
		if (argc < 3) {
			fprintf(stderr, "No drop probability specified.\n");
			return;
		}

		sRandomDrop = atof(argv[2]);
		if (sRandomDrop < 0.0)
			sRandomDrop = 0;
		else if (sRandomDrop > 1.0)
			sRandomDrop = 1.0;
	} else if (isdigit(argv[1][0])) {
		// add to drop list
		for (int i = 1; i < argc; i++) {
			uint32 packet = strtoul(argv[i], NULL, 0);
			if (packet == 0) {
				fprintf(stderr, "invalid packet number: %s\n", argv[i]);
				break;
			}

			sDropList.insert(packet);
		}
	} else {
		// print usage
		puts("usage: drop <packet-number> [...]\n"
			"   or: drop -r <probability>\n\n"
			"   or: drop [-f]\n\n"
			"Specifiying -f flushes the drop list, -r sets the probability a packet\n"
			"is dropped; if you called drop without any arguments, the current\n"
			"drop list is dumped.");
	}
}


static void
do_reorder(int argc, char** argv)
{
	if (argc == 1) {
		// show list of dropped packets
		if (sRandomReorder > 0.0)
			printf("Reorder probability is %f\n", sRandomReorder);

		printf("Reorder packets:\n");

		std::set<uint32>::iterator iterator = sReorderList.begin();
		uint32 count = 0;
		for (; iterator != sReorderList.end(); iterator++) {
			printf("%4lu\n", *iterator);
			count++;
		}

		if (count == 0)
			printf("<empty>\n");
	} else if (!strcmp(argv[1], "-f")) {
		// flush reorder list
		sReorderList.clear();
		puts("reorder list cleared.");
	} else if (!strcmp(argv[1], "-r")) {
		if (argc < 3) {
			fprintf(stderr, "No reorder probability specified.\n");
			return;
		}

		sRandomReorder = atof(argv[2]);
		if (sRandomReorder < 0.0)
			sRandomReorder = 0;
		else if (sRandomReorder > 1.0)
			sRandomReorder = 1.0;
	} else if (isdigit(argv[1][0])) {
		// add to reorder list
		for (int i = 1; i < argc; i++) {
			uint32 packet = strtoul(argv[i], NULL, 0);
			if (packet == 0) {
				fprintf(stderr, "invalid packet number: %s\n", argv[i]);
				break;
			}

			sReorderList.insert(packet);
		}
	} else {
		// print usage
		puts("usage: reorder <packet-number> [...]\n"
			"   or: reorder -r <probability>\n\n"
			"   or: reorder [-f]\n\n"
			"Specifiying -f flushes the reorder list, -r sets the probability a packet\n"
			"is reordered; if you called reorder without any arguments, the current\n"
			"reorder list is dumped.");
	}
}


static void
do_round_trip_time(int argc, char** argv)
{
	if (argc == 1) {
		// show current time
		printf("Current round trip time: %g ms\n", sRoundTripTime / 1000.0);
	} else if (!strcmp(argv[1], "-r")) {
		// toggle random time
		sRandomRoundTrip = !sRandomRoundTrip;
		printf("Round trip time is now %s.\n", sRandomRoundTrip ? "random" : "fixed");
	} else if (!strcmp(argv[1], "-i")) {
		// toggle increasing time
		sIncreasingRoundTrip = !sIncreasingRoundTrip;
		printf("Round trip time is now %s.\n", sIncreasingRoundTrip ? "increasing" : "fixed");
	} else if (isdigit(argv[1][0])) {
		// set time
		sRoundTripTime = 1000LL * strtoul(argv[1], NULL, 0);
	} else {
		// print usage
		puts("usage: rtt <time in ms>\n"
			"   or: rtt [-r|-i]\n\n"
			"Specifiying -r sets random time, -i causes the times to increase over time;\n"
			"witout any arguments, the current time is printed.");
	}
}


static void
do_bad_synack(int argc, char** argv)
{
	if (argc > 1 && !strcmp(argv[1], "-f")) {
		sSpoofBadSynAck = SPOOF_SYN_ACKNOWLEDGE_NONE;
		puts("bad SYN-ACK disarmed.");
		return;
	}

	if (argc > 2 || (argc == 2 && strcmp(argv[1], "ack") != 0
			&& strcmp(argv[1], "rst") != 0)) {
		puts("usage: bad_synack [ack|rst]\n"
			"   or: bad_synack -f\n\n"
			"Arms a one-shot on the next SYN-ACK delivered to the client. It\n"
			"is rewritten into one of the two segments RFC 9293 3.10.7.3\n"
			"refuses to take instructions from:\n\n"
			"  ack  (the default) its acknowledgement is moved outside\n"
			"       [ISS, SND.MAX], so it acknowledges a byte the client\n"
			"       never sent (issue #613);\n"
			"  rst  it becomes a bare reset, carrying no acknowledgement at\n"
			"       all -- the whole segment an off-path attacker who knows\n"
			"       only the port pair can send (issue #614).\n\n"
			"Call this before connect. The handshake is retried, so a stack\n"
			"that stays in SYN-SENT still connects; one that gives up fails\n"
			"connect() or leaves it asleep until it times out. -f disarms.");
		return;
	}

	if (argc == 2 && !strcmp(argv[1], "rst")) {
		sSpoofBadSynAck = SPOOF_SYN_ACKNOWLEDGE_BARE_RESET;
		puts("the next SYN-ACK to the client will become a bare reset.");
		return;
	}

	sSpoofBadSynAck = SPOOF_SYN_ACKNOWLEDGE_BAD_ACK;
	puts("the next SYN-ACK to the client will be spoofed.");
}


static void
do_spoof(int argc, char** argv)
{
	if (argc > 1 && !strcmp(argv[1], "-f")) {
		sSpoofRemaining = 0;
		sSpoofLate = false;
		puts("spoofing disarmed.");
		return;
	}

	if (argc < 2 || (strcmp(argv[1], "syn") != 0
			&& strcmp(argv[1], "rst") != 0)) {
		puts("usage: spoof <syn|rst> [<offset> [<count> [side [late]]]]\n"
			"   or: spoof -f\n\n"
			"Rewrites the next inbound segment into a bare SYN or reset carrying\n"
			"RCV.NXT + <offset> (default 1): in the receive window, but not\n"
			"the sequence number the receiver is waiting for. That is the\n"
			"blind-reset segment RFC 5961 sections 3 and 4 answer with a\n"
			"challenge acknowledgement instead of tearing the connection down\n"
			"(issues #609, #614). <count> defaults to 1 and [side] to \"server\";\n"
			"pass \"client\" to aim it the other way. An offset of 0 makes the\n"
			"segment acceptable, which is the control -- the connection should\n"
			"then die.\n\n"
			"A fifth argument of \"late\" delivers the forgery after the\n"
			"segment it was built from instead of before it. That is what\n"
			"reaches the one reset outside the window that is still\n"
			"believed, the abortive close at RCV.NXT - 1: arm\n"
			"\"rst 0 1 <side> late\" and let it fire on a FIN, which the\n"
			"receiver consumes first, so the reset lands one behind the\n"
			"RCV.NXT it leaves. -f disarms.");
		return;
	}

	sSpoofFlags = !strcmp(argv[1], "syn")
		? TCP_FLAG_SYNCHRONIZE : TCP_FLAG_RESET;
	sSpoofOffset = argc > 2 ? strtol(argv[2], NULL, 0) : 1;
	sSpoofRemaining = argc > 3 ? strtol(argv[3], NULL, 0) : 1;
	sSpoofAtServer = argc > 4 ? strcmp(argv[4], "client") != 0 : true;
	sSpoofLate = argc > 5 && strcmp(argv[5], "late") == 0;

	printf("will spoof %" B_PRId32 " bare %s at RCV.NXT + %" B_PRId32
		" to the %s, %s the segment it is built from.\n", sSpoofRemaining,
		argv[1], sSpoofOffset, sSpoofAtServer ? "server" : "client",
		sSpoofLate ? "after" : "before");
}


/*!	Loses the server's first SYN-ACK and checks the one that replaces it.

	The regression test for issue #830. A retransmitted SYN arriving in
	SYN-RECEIVED is out of window by construction -- a zero-length segment at
	RCV.NXT - 1 -- so the generic out-of-window path answered it with a plain
	acknowledgement, which is stamped with SND.NXT (the ISN plus one, once the
	first SYN-ACK is out) and carries no options, because they are only added
	at the initial send sequence. The client reads that sequence number as the
	server's ISN and spends the rest of the connection one byte ahead of the
	stream, which is fatal to anything that cares about its first byte.

	All three symptoms are on the wire, so all three are checked: the sequence
	number, the options, and -- from the opposite endpoint, independently of
	what the server believes it sent -- what the client's handshake-completing
	acknowledgement says the server's ISN was.
*/
static void
do_lost_synack(int argc, char** argv)
{
	const bool serverOnly = argc == 2 && !strcmp(argv[1], "-s");
	const bool giveUp = argc == 2 && !strcmp(argv[1], "-g");

	if (argc > 1 && !serverOnly && !giveUp) {
		puts("usage: lost_synack [-s|-g]\n\n"
			"Drops the server's first SYN-ACK, lets the client's SYN\n"
			"retransmission provoke the next one, and checks that it is a\n"
			"retransmission of the first rather than a new segment: same\n"
			"sequence number, same MSS, window-scale and SACK-permitted, and\n"
			"a client acknowledging the ISN + 1 it was originally offered\n"
			"(issue #830). Prints PASS or FAIL and does its own connect, so\n"
			"it replaces the connect command rather than following it.\n\n"
			"-s also drops every SYN the client retransmits, which leaves\n"
			"the server's own retransmission of its SYN-ACK as the only way\n"
			"the handshake can complete at all (issue #832). Both ends start\n"
			"from the same one-second timeout, so this is the test that does\n"
			"not come down to which RTO happens to fire first.\n\n"
			"-g drops every SYN-ACK as well, so the server runs out of\n"
			"retransmissions and has to give up on the half-open connection\n"
			"instead of retrying it for as long as the listener lives.\n"
			"Takes as long as connect() does to time out.");
		return;
	}

	for (int32 i = 0; i < kMaxRecordedSynAcknowledges; i++)
		sSynAcknowledges[i].seen = false;
	sSynAcknowledgeCount = 0;
	sSynAcknowledgeTotal = 0;
	sClientSynchronizeCount = 0;
	sClientAcknowledgeSeen = false;
	sRecordHandshake = true;
	sLoseSynAcknowledges = giveUp ? -1 : 1;
	sLoseClientSynchronize = serverOnly || giveUp;

	char* connect = (char*)"connect";
	do_connect(1, &connect);

	// connect() returns once the client is established, which is after it has
	// sent the acknowledgement this looks at -- but only just, so give the
	// send path room rather than racing it.
	snooze(250000);

	sLoseSynAcknowledges = 0;
	sLoseClientSynchronize = false;
	sRecordHandshake = false;

	int32 failures = 0;

	if (giveUp) {
		// One SYN-ACK plus the retransmissions it is allowed, and then the
		// half-open connection has to be given up rather than retried for as
		// long as the listener lives. connect() timing out is what bounds the
		// wait here, and it outlasts the last retransmission by design.
		const int32 expected = TCP_MAX_SYN_ACKNOWLEDGE_RETRANSMITS + 1;
		if (sSynAcknowledgeTotal != expected) {
			printf("FAIL: the server put %" B_PRId32 " SYN-ACK(s) on the wire, "
				"expected %" B_PRId32 "\n", sSynAcknowledgeTotal, expected);
			failures++;
		} else {
			printf("the server sent %" B_PRId32 " SYN-ACKs and stopped\n",
				sSynAcknowledgeTotal);
		}
	} else if (sSynAcknowledgeCount < 2) {
		if (serverOnly) {
			printf("FAIL: the server never retransmitted its SYN-ACK; with "
				"the client's %" B_PRId32 " SYN retransmission(s) lost too, "
				"nothing was left to complete the handshake\n",
				sClientSynchronizeCount - 1);
		} else {
			printf("FAIL: the server sent %" B_PRId32 " SYN-ACK(s); the lost "
				"one was never answered again\n", sSynAcknowledgeCount);
		}
		failures++;
	} else {
		const synchronize_acknowledge& first = sSynAcknowledges[0];
		const synchronize_acknowledge& second = sSynAcknowledges[1];

		if (second.sequence != first.sequence) {
			printf("FAIL: the second SYN-ACK is at %" B_PRIu32 ", not the ISN "
				"%" B_PRIu32 " it must repeat (off by %" B_PRId32 ")\n",
				second.sequence, first.sequence,
				(int32)(second.sequence - first.sequence));
			failures++;
		}

		if (second.maxSegmentSize != first.maxSegmentSize
			|| second.windowShift != first.windowShift
			|| second.sackPermitted != first.sackPermitted) {
			printf("FAIL: the second SYN-ACK renegotiates the handshake "
				"options: mss %u -> %u, window shift %u -> %u, "
				"sack-permitted %s -> %s\n", first.maxSegmentSize,
				second.maxSegmentSize, first.windowShift, second.windowShift,
				first.sackPermitted ? "yes" : "no",
				second.sackPermitted ? "yes" : "no");
			failures++;
		}

		if (!sClientAcknowledgeSeen) {
			puts("FAIL: the client never acknowledged the handshake");
			failures++;
		} else if (sClientAcknowledge != first.sequence + 1) {
			printf("FAIL: the client acknowledged %" B_PRIu32 ", so it took "
				"%" B_PRIu32 " for the server's ISN instead of %" B_PRIu32
				" -- the first byte of the server's stream is lost\n",
				sClientAcknowledge, sClientAcknowledge - 1, first.sequence);
			failures++;
		}
	}

	printf("lost_synack: %s\n", failures == 0 ? "PASS" : "FAIL");
}


/*!	Waits before reading the next command.

	A scripted run otherwise has no way to say "let this finish": commands are
	read as fast as the pipe delivers them, so "quit" lands while the handshake
	and the transfer it follows are still in flight. That overlap is sometimes
	exactly what a test wants, but when it is not, the only control available
	was how fast the shell happened to be -- which made reproductions rate
	measurements rather than tests.
*/
static void
do_sleep(int argc, char** argv)
{
	if (argc != 2) {
		puts("usage: sleep <time in ms>");
		return;
	}

	snooze((bigtime_t)strtoul(argv[1], NULL, 0) * 1000);
}


static void
do_dprintf(int argc, char** argv)
{
	if (argc > 1)
		gDebugOutputEnabled = !strcmp(argv[1], "on");
	else
		gDebugOutputEnabled = !gDebugOutputEnabled;

	printf("debug output turned %s.\n", gDebugOutputEnabled ? "on" : "off");
}


static cmd_entry sBuiltinCommands[] = {
	{"connect", do_connect, "Connects the client"},
	{"send", do_send, "Sends data from the client to the server"},
	{"send_loop", do_send_loop, "Sends data in a loop"},
	{"close", do_close, "Performs an active or simultaneous close"},
	{"dprintf", do_dprintf, "Toggles debug output"},
	{"drop", do_drop, "Lets you drop packets during transfer"},
	{"reorder", do_reorder, "Lets you reorder packets during transfer"},
	{"bad_synack", do_bad_synack, "Spoofs a SYN-ACK the client never asked for"},
	{"spoof", do_spoof, "Spoofs an in-window bare SYN or reset"},
	{"lost_synack", do_lost_synack, "Checks the SYN-ACK sent after one is lost"},
	{"help", do_help, "prints this help text"},
	{"rtt", do_round_trip_time, "Specifies the round trip time"},
	{"sleep", do_sleep, "Waits before reading the next command"},
	{"quit", NULL, "exits the application"},
	{NULL, NULL, NULL},
};


static void
do_help(int argc, char** argv)
{
	printf("Available commands:\n");

	for (cmd_entry* command = sBuiltinCommands; command->name != NULL; command++) {
		printf("%8s - %s\n", command->name, command->help);
	}
}


//	#pragma mark -


int
main(int argc, char* argv[])
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-w") == 0 && (i + 1) < argc) {
			if (!setup_dump_pcap(argv[++i]))
				return 1;
		}
	}

	if (sPacketMonitor == NULL)
		sPacketMonitor = dump_printf;

	status_t status = init_timers();
	if (status < B_OK) {
		fprintf(stderr, "tcp_tester: Could not initialize timers: %s\n",
			strerror(status));
		return 1;
	}

	_add_builtin_module((module_info*)&gNetStackModule);
	_add_builtin_module((module_info*)&gNetBufferModule);
	_add_builtin_module((module_info*)&gNetSocketModule);
	_add_builtin_module((module_info*)&gNetDatalinkModule);
	_add_builtin_module(modules[0]);
	if (_get_builtin_dependencies() < B_OK) {
		fprintf(stderr, "tcp_tester: Could not initialize modules: %s\n",
			strerror(status));
		return 1;
	}

	sockaddr_in interfaceAddress;
	interfaceAddress.sin_len = sizeof(sockaddr_in);
	interfaceAddress.sin_family = AF_INET;
	interfaceAddress.sin_addr.s_addr = htonl(0xc0a80001);
	gInterfaceAddress.local = (sockaddr*)&interfaceAddress;
	gInterfaceAddress.domain = &sDomain;

	status = get_module("network/protocols/tcp/v1", (module_info **)&gTCPModule);
	if (status < B_OK) {
		fprintf(stderr, "tcp_tester: Could not open TCP module: %s\n",
			strerror(status));
		return 1;
	}

	net_protocol* client = init_protocol(&gClientSocket);
	if (client == NULL)
		return 1;
	net_protocol* server = init_protocol(&gServerSocket);
	if (server == NULL)
		return 1;

	setup_context(sClientContext, false);
	setup_context(sServerContext, true);

	printf("*** Server: %p (%ld), Client: %p (%ld)\n", server,
		sServerContext.thread, client, sClientContext.thread);

	setup_server();

	while (true) {
		printf("> ");
		fflush(stdout);

		char line[1024];
		if (fgets(line, sizeof(line), stdin) == NULL)
			break;

        argc = 0;
        argv = build_argv(line, &argc);
        if (argv == NULL || argc == 0)
            continue;

        int length = strlen(argv[0]);

#if 0
		char *newLine = strchr(line, '\n');
		if (newLine != NULL)
			newLine[0] = '\0';
#endif

		if (!strcmp(argv[0], "quit")
			|| !strcmp(argv[0], "exit")
			|| !strcmp(argv[0], "q"))
			break;

		bool found = false;

		for (cmd_entry* command = sBuiltinCommands; command->name != NULL; command++) {
			if (!strncmp(command->name, argv[0], length)) {
				command->func(argc, argv);
				found = true;
				break;
			}
		}

		if (!found)
			fprintf(stderr, "Unknown command \"%s\". Type \"help\" for a list of commands.\n", argv[0]);

		free(argv);
	}

	close_owned_socket(gClientSocket, "client", &sClientClosed);
	close_owned_socket(gServerSocket, "server", &sServerClosed);

	snooze(2000000);

	cleanup_context(sClientContext);
	cleanup_context(sServerContext);

	put_module("network/protocols/tcp/v1");
	uninit_timers();
	return 0;
}
