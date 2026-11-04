/*
 * hoplimit958.c -- issue 958: IPV6_RECVHOPLIMIT reports the hop limit the
 * datagram arrived with, on every receive path that queues a clone of it,
 * and an IPv4 datagram on a dual-stack socket gets no IPV6_HOPLIMIT at all.
 * udp947.c covers the option matrix; this covers the paths:
 *   - UDP over ::1, the sender's IPV6_UNICAST_HOPS set to 1, 7 and 200
 *   - the same datagram read first with MSG_PEEK and then for real (the peek
 *     reads a clone of the queued clone)
 *   - a raw IPPROTO_UDP socket, which gets its own clone of the datagram
 *   - a zero-length datagram, read directly and with MSG_PEEK first: removing
 *     its UDP header empties the buffer's first node
 *   - an IPv4 datagram to a dual-stack :: socket: no IPV6_HOPLIMIT
 *
 * Exit status: the number of FAILs. Plain POSIX: builds and runs on Linux,
 * which is the reference.
 */

#define _GNU_SOURCE
#define __APPLE_USE_RFC_3542

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


static int sFailures;


static void
result(const char* name, int pass, const char* detail)
{
	printf("%-52s %s  %s\n", name, pass ? "PASS" : "FAIL", detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static void
set_timeout(int fd)
{
	struct timeval timeout = { 1, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}


/*!	recvmsg() with room for any control message; returns the data length and
	sets \a _hopLimit to the IPV6_HOPLIMIT value, or -1 if there was none.
*/
static ssize_t
receive(int fd, int flags, char* data, size_t size, int* _hopLimit,
	int* _messageFlags)
{
	union {
		struct cmsghdr align;
		char buffer[256];
	} control;
	struct iovec vector = { data, size };
	struct msghdr message;
	memset(&message, 0, sizeof(message));
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.buffer;
	message.msg_controllen = sizeof(control.buffer);

	ssize_t length = recvmsg(fd, &message, flags);
	*_hopLimit = -1;
	*_messageFlags = message.msg_flags;
	if (length < 0)
		return length;

	for (struct cmsghdr* header = CMSG_FIRSTHDR(&message); header != NULL;
			header = CMSG_NXTHDR(&message, header)) {
		if (header->cmsg_level == IPPROTO_IPV6
			&& header->cmsg_type == IPV6_HOPLIMIT)
			memcpy(_hopLimit, CMSG_DATA(header), sizeof(int));
	}
	return length;
}


static int
udp6_receiver(unsigned short port, int dualStack)
{
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	int on = 1, off = 0;
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, dualStack ? &off : &on,
		sizeof(int));
	setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &on, sizeof(on));
	set_timeout(fd);

	struct sockaddr_in6 address;
	memset(&address, 0, sizeof(address));
	address.sin6_family = AF_INET6;
	address.sin6_port = htons(port);
	address.sin6_addr = dualStack ? in6addr_any : in6addr_loopback;
	if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}


static int
send6(unsigned short port, int hops, const char* text)
{
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops));
	struct sockaddr_in6 to;
	memset(&to, 0, sizeof(to));
	to.sin6_family = AF_INET6;
	to.sin6_port = htons(port);
	to.sin6_addr = in6addr_loopback;
	ssize_t sent = sendto(fd, text, strlen(text), 0, (struct sockaddr*)&to,
		sizeof(to));
	close(fd);
	return sent == (ssize_t)strlen(text) ? 0 : -1;
		// an empty text sends a zero-length datagram
}


int
main(void)
{
	unsigned short port = 39580 + (unsigned short)(getpid() % 1000);
	char detail[160], name[80], data[64];
	int hopLimit, messageFlags;

	static const int kHops[] = { 1, 7, 200 };
	for (int i = 0; i < 3; i++, port++) {
		int fd = udp6_receiver(port, 0);
		send6(port, kHops[i], "958");
		ssize_t length = receive(fd, 0, data, sizeof(data), &hopLimit,
			&messageFlags);
		snprintf(name, sizeof(name), "udp ::1, sender hops %d", kHops[i]);
		snprintf(detail, sizeof(detail), "recvmsg=%zd [IPV6_HOPLIMIT %d]",
			length, hopLimit);
		result(name, length == 3 && hopLimit == kHops[i], detail);
		close(fd);
	}

	{
		int fd = udp6_receiver(port, 0);
		send6(port, 9, "958");
		int peekHops;
		ssize_t peeked = receive(fd, MSG_PEEK, data, sizeof(data), &peekHops,
			&messageFlags);
		ssize_t length = receive(fd, 0, data, sizeof(data), &hopLimit,
			&messageFlags);
		snprintf(detail, sizeof(detail),
			"peek=%zd [IPV6_HOPLIMIT %d], then recvmsg=%zd [IPV6_HOPLIMIT %d]",
			peeked, peekHops, length, hopLimit);
		result("udp ::1, MSG_PEEK then read, sender hops 9",
			peeked == 3 && peekHops == 9 && length == 3 && hopLimit == 9,
			detail);
		close(fd);
		port++;
	}

	{
		int fd = udp6_receiver(port, 0);
		send6(port, 5, "");
		ssize_t length = receive(fd, 0, data, sizeof(data), &hopLimit,
			&messageFlags);
		snprintf(detail, sizeof(detail), "recvmsg=%zd [IPV6_HOPLIMIT %d]",
			length, hopLimit);
		result("udp ::1, zero-length datagram, sender hops 5",
			length == 0 && hopLimit == 5, detail);
		close(fd);
		port++;
	}

	{
		int fd = udp6_receiver(port, 0);
		send6(port, 6, "");
		int peekHops;
		ssize_t peeked = receive(fd, MSG_PEEK, data, sizeof(data), &peekHops,
			&messageFlags);
		ssize_t length = receive(fd, 0, data, sizeof(data), &hopLimit,
			&messageFlags);
		snprintf(detail, sizeof(detail),
			"peek=%zd [IPV6_HOPLIMIT %d], then recvmsg=%zd [IPV6_HOPLIMIT %d]",
			peeked, peekHops, length, hopLimit);
		result("udp ::1, zero-length, MSG_PEEK then read, hops 6",
			peeked == 0 && peekHops == 6 && length == 0 && hopLimit == 6,
			detail);
		close(fd);
		port++;
	}

	{
		int raw = socket(AF_INET6, SOCK_RAW, IPPROTO_UDP);
		int on = 1;
		if (raw >= 0) {
			setsockopt(raw, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &on, sizeof(on));
			set_timeout(raw);
		}
		int fd = udp6_receiver(port, 0);
		send6(port, 11, "958");
		ssize_t length = raw < 0 ? -1
			: receive(raw, 0, data, sizeof(data), &hopLimit, &messageFlags);
		snprintf(detail, sizeof(detail), "raw recvmsg=%zd%s [IPV6_HOPLIMIT %d]",
			length, raw < 0 ? " (no raw socket)" : "", hopLimit);
		result("raw IPPROTO_UDP socket, sender hops 11",
			length == 8 + 3 && hopLimit == 11, detail);
		if (raw >= 0)
			close(raw);
		close(fd);
		port++;
	}

	{
		int fd = udp6_receiver(port, 1);
		int sender = socket(AF_INET, SOCK_DGRAM, 0);
		int ttl = 7;
		setsockopt(sender, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
		struct sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_port = htons(port);
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sendto(sender, "958", 3, 0, (struct sockaddr*)&to, sizeof(to));
		close(sender);
		ssize_t length = receive(fd, 0, data, sizeof(data), &hopLimit,
			&messageFlags);
		snprintf(detail, sizeof(detail), "recvmsg=%zd [IPV6_HOPLIMIT %d]",
			length, hopLimit);
		result("v4-to-dual 127.0.0.1 -> ::, no IPV6_HOPLIMIT",
			length == 3 && hopLimit == -1, detail);
		close(fd);
	}

	printf("%d FAIL(s)\n", sFailures);
	return sFailures;
}
