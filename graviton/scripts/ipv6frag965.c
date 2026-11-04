/*
 * ipv6frag965.c -- issue 965: IPv6 fragmentation and reassembly over the
 * loopback, and a receive path that cannot be made to loop.
 *
 * Run it with the loopback MTU lowered (ifconfig loop mtu 9001 / 1280 on
 * Haiku, ip link set lo mtu ... on Linux), so that large datagrams have to be
 * fragmented. After every row the loopback is probed with a small IPv4 and a
 * small IPv6 datagram; if either is lost the loopback is reported DEAD and
 * the run stops, since nothing after that would mean anything.
 *
 * Rows:
 *   - send: one UDP datagram of N bytes to 127.0.0.1 or ::1, sent by the
 *     kernel, so it fragments and reassembles it. Delivered, byte for byte.
 *   - crafted: fragments built here and sent through a raw IPPROTO_FRAGMENT
 *     socket (the kernel puts the IPv6 header in front), so the receiver is
 *     tested on its own: in order, in reverse, with an exact duplicate, an
 *     atomic fragment (offset 0, no more fragments; RFC 6946), a non-last
 *     fragment whose length is not a multiple of 8 (must be dropped), a
 *     fragment header that names itself as the next header, and a chain of
 *     empty destination options headers that each name the next as another
 *     one and run past the end of the packet.
 *
 * Options pick one group of rows, so that a receiver which dies on one row
 * can still be tested on the others on a fresh instance:
 *   -s  only the send rows      -c  only the well-formed crafted rows
 *   -f  only the self-naming fragment header rows
 *   -d  only the destination options chain row
 *
 * Exit status: the number of FAILs, or 100 + that if the loopback died.
 * Plain POSIX: builds and runs on Linux, which is the reference.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


#ifndef IPPROTO_FRAGMENT
#	define IPPROTO_FRAGMENT	44
#endif
#ifndef IPPROTO_DSTOPTS
#	define IPPROTO_DSTOPTS	60
#endif

static int sFailures;
static unsigned short sPort = 39650;


static void
result(const char* name, int pass, const char* detail)
{
	printf("%-58s %s  %s\n", name, pass ? "PASS" : "FAIL", detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static void
fill(unsigned char* data, size_t length, unsigned seed)
{
	for (size_t i = 0; i < length; i++)
		data[i] = (unsigned char)(i * 131 + seed + (i >> 8));
}


static int
bind_receiver(int family, unsigned short port)
{
	int fd = socket(family, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	int size = 1 << 20;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
	struct timeval timeout = { 1, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	int status;
	if (family == AF_INET6) {
		struct sockaddr_in6 address;
		memset(&address, 0, sizeof(address));
		address.sin6_family = AF_INET6;
		address.sin6_port = htons(port);
		address.sin6_addr = in6addr_loopback;
		status = bind(fd, (struct sockaddr*)&address, sizeof(address));
	} else {
		struct sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		status = bind(fd, (struct sockaddr*)&address, sizeof(address));
	}
	if (status != 0) {
		close(fd);
		return -1;
	}
	return fd;
}


/*!	Sends \a length bytes to the loopback of \a family through the kernel's
	UDP and reports what arrived in \a detail. Returns whether it arrived
	intact.
*/
static int
udp_round_trip(int family, size_t length, char* detail, size_t detailSize)
{
	unsigned short port = sPort++;
	int receiver = bind_receiver(family, port);
	int sender = socket(family, SOCK_DGRAM, 0);
	if (receiver < 0 || sender < 0) {
		snprintf(detail, detailSize, "socket/bind: %s", strerror(errno));
		if (receiver >= 0)
			close(receiver);
		if (sender >= 0)
			close(sender);
		return 0;
	}
	int size = 1 << 20;
	setsockopt(sender, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));

	unsigned char* out = malloc(length + 1);
	unsigned char* in = malloc(length + 16);
	fill(out, length, (unsigned)length);

	ssize_t sent;
	if (family == AF_INET6) {
		struct sockaddr_in6 to;
		memset(&to, 0, sizeof(to));
		to.sin6_family = AF_INET6;
		to.sin6_port = htons(port);
		to.sin6_addr = in6addr_loopback;
		sent = sendto(sender, out, length, 0, (struct sockaddr*)&to,
			sizeof(to));
	} else {
		struct sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_port = htons(port);
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sent = sendto(sender, out, length, 0, (struct sockaddr*)&to,
			sizeof(to));
	}
	int sendErrno = errno;

	ssize_t received = -1;
	int pass = 0;
	if (sent == (ssize_t)length) {
		received = recv(receiver, in, length + 16, 0);
		pass = received == (ssize_t)length && memcmp(in, out, length) == 0;
	}

	if (sent != (ssize_t)length) {
		snprintf(detail, detailSize, "sendto=%zd (%s)", sent,
			strerror(sendErrno));
	} else if (received < 0) {
		snprintf(detail, detailSize, "sent %zu, NOT DELIVERED within 1 s",
			length);
	} else {
		snprintf(detail, detailSize, "sent %zu, received %zd%s", length,
			received, pass ? ", intact"
				: (received == (ssize_t)length ? ", CONTENT DIFFERS" : ""));
	}

	free(out);
	free(in);
	close(sender);
	close(receiver);
	return pass;
}


/*!	A small datagram over 127.0.0.1 and one over ::1. */
static int
loopback_alive(void)
{
	char detail[128];
	return udp_round_trip(AF_INET, 100, detail, sizeof(detail))
		&& udp_round_trip(AF_INET6, 100, detail, sizeof(detail));
}


static uint16_t
udp6_checksum(const unsigned char* udp, size_t length)
{
	uint32_t sum = 0;
	// pseudo header: ::1 -> ::1, length, next header 17
	sum += 2;
		// the two 0x0001 words of the addresses
	sum += (uint32_t)(length >> 16) + (uint32_t)(length & 0xffff);
	sum += 17;
	for (size_t i = 0; i + 1 < length; i += 2)
		sum += (uint32_t)(udp[i] << 8 | udp[i + 1]);
	if (length & 1)
		sum += (uint32_t)(udp[length - 1] << 8);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	uint16_t result = (uint16_t)~sum;
	return result == 0 ? 0xffff : result;
}


/*!	Builds a UDP datagram (header and \a payloadLength bytes) for \a port. */
static unsigned char*
make_udp(unsigned short port, size_t payloadLength, size_t* _length)
{
	size_t length = 8 + payloadLength;
	unsigned char* udp = calloc(1, length);
	udp[0] = 0x9a; udp[1] = 0xbc;
		// source port 39612; not looked at
	udp[2] = port >> 8; udp[3] = port & 0xff;
	udp[4] = length >> 8; udp[5] = length & 0xff;
	fill(udp + 8, payloadLength, 965);
	uint16_t checksum = udp6_checksum(udp, length);
	udp[6] = checksum >> 8; udp[7] = checksum & 0xff;
	*_length = length;
	return udp;
}


/*!	Sends a raw packet whose IPv6 next header is \a protocol and whose payload
	is \a data, to ::1.
*/
static int
send_raw(int protocol, const void* data, size_t length)
{
	int fd = socket(AF_INET6, SOCK_RAW, protocol);
	if (fd < 0)
		return -1;
	struct sockaddr_in6 to;
	memset(&to, 0, sizeof(to));
	to.sin6_family = AF_INET6;
	to.sin6_addr = in6addr_loopback;
	ssize_t sent = sendto(fd, data, length, 0, (struct sockaddr*)&to,
		sizeof(to));
	int error = errno;
	close(fd);
	errno = error;
	return sent == (ssize_t)length ? 0 : -1;
}


/*!	Sends bytes [start, end) of \a udp as one fragment. */
static int
send_fragment(const unsigned char* udp, size_t start, size_t end,
	int more, uint32_t ident, int nextHeader)
{
	unsigned char packet[8 + 1024];
	if (end - start > sizeof(packet) - 8) {
		errno = EMSGSIZE;
		return -1;
	}
	packet[0] = nextHeader;
	packet[1] = 0;
	uint16_t offlg = (uint16_t)(start & 0xfff8) | (more ? 1 : 0);
	packet[2] = offlg >> 8;
	packet[3] = offlg & 0xff;
	packet[4] = ident >> 24; packet[5] = ident >> 16;
	packet[6] = ident >> 8; packet[7] = ident;
	memcpy(packet + 8, udp + start, end - start);
	return send_raw(IPPROTO_FRAGMENT, packet, 8 + end - start);
}


struct piece {
	size_t start;
	size_t end;
};


/*!	Sends the datagram as the given fragments, in the given order, and reports
	whether it arrived intact (or, with \a expectDrop, whether it did not).
*/
static void
crafted(const char* name, size_t payloadLength, const struct piece* pieces,
	int count, int expectDrop)
{
	char detail[160];
	unsigned short port = sPort++;
	int receiver = bind_receiver(AF_INET6, port);
	if (receiver < 0) {
		snprintf(detail, sizeof(detail), "bind: %s", strerror(errno));
		result(name, 0, detail);
		return;
	}

	size_t length;
	unsigned char* udp = make_udp(port, payloadLength, &length);
	uint32_t ident = (uint32_t)random();

	for (int i = 0; i < count; i++) {
		size_t end = pieces[i].end > length ? length : pieces[i].end;
		if (send_fragment(udp, pieces[i].start, end, end < length, ident,
				17) != 0) {
			snprintf(detail, sizeof(detail), "raw send of fragment %d: %s", i,
				strerror(errno));
			result(name, 0, detail);
			free(udp);
			close(receiver);
			return;
		}
	}

	unsigned char in[4096];
	ssize_t received = recv(receiver, in, sizeof(in), 0);
	int intact = received == (ssize_t)payloadLength
		&& memcmp(in, udp + 8, payloadLength) == 0;
	if (expectDrop) {
		snprintf(detail, sizeof(detail), "%s",
			received < 0 ? "dropped, as it must be"
				: "DELIVERED, but must be dropped");
		result(name, received < 0, detail);
	} else {
		snprintf(detail, sizeof(detail), "received %zd of %zu%s", received,
			payloadLength, intact ? ", intact"
				: (received < 0 ? " (NOT DELIVERED within 1 s)" : ", WRONG"));
		result(name, intact, detail);
	}

	free(udp);
	close(receiver);
}


static int
check_alive(const char* after)
{
	if (loopback_alive()) {
		printf("  loopback alive\n");
		return 1;
	}
	printf("  LOOPBACK DEAD after: %s\n", after);
	fflush(stdout);
	return 0;
}


int
main(int argc, char** argv)
{
	int sendRows = 1, craftedRows = 1, selfNamingRows = 1, optionsRows = 1;
	if (argc > 1) {
		sendRows = strcmp(argv[1], "-s") == 0;
		craftedRows = strcmp(argv[1], "-c") == 0;
		selfNamingRows = strcmp(argv[1], "-f") == 0;
		optionsRows = strcmp(argv[1], "-d") == 0;
	}

	srandom((unsigned)time(NULL) ^ (unsigned)getpid());
	sPort = 39650 + (unsigned short)(random() % 2000);

	if (!check_alive("start"))
		return 100 + sFailures;

	static const size_t kSizes[] = { 100, 1200, 8900, 9000, 20000, 60000,
		65000, 65507, 65527 };
	for (size_t i = 0; sendRows && i < sizeof(kSizes) / sizeof(kSizes[0]);
			i++) {
		for (int family = 0; family < 2; family++) {
			size_t length = kSizes[i];
			int af = family == 0 ? AF_INET : AF_INET6;
			if (af == AF_INET && length > 65507)
				continue;
				// larger than an IPv4 datagram can carry
			char name[80], detail[128];
			snprintf(name, sizeof(name), "send udp %-9s %6zu bytes",
				af == AF_INET ? "127.0.0.1" : "::1", length);
			result(name, udp_round_trip(af, length, detail, sizeof(detail)),
				detail);
			if (!check_alive(name))
				return 100 + sFailures;
		}
	}

	// 3000 bytes of UDP (header included): fragments of 1000 bytes
	const size_t kPayload = 2992;
	struct piece inOrder[] = { { 0, 1000 }, { 1000, 2000 }, { 2000, 3000 } };
	struct piece reverse[] = { { 2000, 3000 }, { 1000, 2000 }, { 0, 1000 } };
	struct piece middleFirst[] = { { 1000, 2000 }, { 2000, 3000 },
		{ 0, 1000 } };
	struct piece duplicate[] = { { 0, 1000 }, { 1000, 2000 }, { 1000, 2000 },
		{ 2000, 3000 } };
	struct piece atomic[] = { { 0, 1000 } };
		// small enough for a 1280-byte MTU, so it is not fragmented again
	struct piece notMultipleOf8[] = { { 0, 1001 }, { 1000, 2000 },
		{ 2000, 3000 } };

	if (craftedRows) {
		crafted("crafted 3 fragments, in order", kPayload, inOrder, 3, 0);
		if (!check_alive("in order"))
			return 100 + sFailures;
		crafted("crafted 3 fragments, in reverse", kPayload, reverse, 3, 0);
		if (!check_alive("reverse"))
			return 100 + sFailures;
		crafted("crafted 3 fragments, middle first", kPayload, middleFirst, 3, 0);
		if (!check_alive("middle first"))
			return 100 + sFailures;
		crafted("crafted 3 fragments, one sent twice", kPayload, duplicate, 4, 0);
		if (!check_alive("duplicate"))
			return 100 + sFailures;
		crafted("crafted atomic fragment (RFC 6946)", 992, atomic, 1, 0);
		if (!check_alive("atomic"))
			return 100 + sFailures;
		crafted("crafted non-last fragment of 1001 bytes", kPayload,
			notMultipleOf8, 3, 1);
		if (!check_alive("not a multiple of 8"))
			return 100 + sFailures;
	}

	if (selfNamingRows) {
		// A fragment header whose next header is a fragment header again.
		// An atomic one, so a receiver that reassembles it is left with a
		// packet whose first header after the fixed one is a fragment header.
		unsigned char packet[8 + 64];
		memset(packet, 0, sizeof(packet));
		packet[0] = IPPROTO_FRAGMENT;
		packet[4] = 0x96; packet[5] = 0x50;
		int sent = send_raw(IPPROTO_FRAGMENT, packet, sizeof(packet));
		char detail[96];
		snprintf(detail, sizeof(detail), "raw send %s",
			sent == 0 ? "ok" : strerror(errno));
		result("crafted fragment header naming itself as next", sent == 0,
			detail);
		if (!check_alive("self-naming fragment header"))
			return 100 + sFailures;

		// the same with more fragments to come
		packet[3] = 1;
		packet[7] = 1;
		sent = send_raw(IPPROTO_FRAGMENT, packet, sizeof(packet));
		snprintf(detail, sizeof(detail), "raw send %s",
			sent == 0 ? "ok" : strerror(errno));
		result("crafted ... and with more fragments to come", sent == 0,
			detail);
		if (!check_alive("self-naming fragment header, M=1"))
			return 100 + sFailures;
	}

	if (optionsRows) {
		// Destination options headers of 8 bytes (length field 0), each
		// naming another one as the next header, running off the end.
		unsigned char packet[24];
		for (int i = 0; i < 24; i += 8) {
			memset(packet + i, 0, 8);
			packet[i] = IPPROTO_DSTOPTS;
			packet[i + 2] = 1;
			packet[i + 3] = 4;
				// a PadN option filling the header's other 6 bytes
		}
		int sent = send_raw(IPPROTO_DSTOPTS, packet, sizeof(packet));
		char detail[96];
		snprintf(detail, sizeof(detail), "raw send %s",
			sent == 0 ? "ok" : strerror(errno));
		result("crafted destination options chain off the end", sent == 0,
			detail);
		if (!check_alive("destination options chain"))
			return 100 + sFailures;
	}

	printf("%d FAIL(s)\n", sFailures);
	return sFailures;
}
