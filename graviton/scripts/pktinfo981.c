/*
 * pktinfo981 -- sendmsg() with a source-address control message on UDP
 * (IP_PKTINFO, IP_SENDSRCADDR, IPV6_PKTINFO), and IP_PKTINFO on receive
 * (issue 981). One row per case, the same program on DeBeOS and Linux.
 *
 *   pktinfo981 [-l <local IPv4> ] [-L <local IPv6>]
 *       The loopback matrix. -l/-L name a second, non-loopback address of
 *       this host (the ENA one) that a datagram to the loopback may carry as
 *       its source; without them those rows are skipped.
 *   pktinfo981 send <peer> <port> <source> [ifindex or name]
 *       One datagram "pktinfo981 from <source>" to <peer>:<port> with
 *       <source> (and ifindex) in the control message, or with none
 *       (a plain sendto()) for a <source> of "-". The socket has
 *       SO_BROADCAST. Prints the call's result.
 *       For a tcpdump on the peer.
 *   pktinfo981 recv <port> [count]
 *       Prints the source of each datagram that reaches <port> (IPv6 socket,
 *       dual-stack, so IPv4 senders show up v4-mapped).
 *
 * Build: cc -o pktinfo981 pktinfo981.c   (add -lnetwork on Haiku)
 */

#define _GNU_SOURCE
	/* struct in6_pktinfo and struct in_pktinfo on glibc */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


/* A source address that is not this host's: DeBeOS answers EADDRNOTAVAIL, as
   the BSDs and macOS do and as its bind() does. Linux answers ENETUNREACH
   (IPv4) and EINVAL (IPv6). */
#if defined(__linux__)
#	define FOREIGN4 ENETUNREACH
#	define FOREIGN6 EINVAL
#else
#	define FOREIGN4 EADDRNOTAVAIL
#	define FOREIGN6 EADDRNOTAVAIL
#endif


static int sFailures = 0;
static int sRows = 0;


static void
row(const char* name, int ok, const char* detail)
{
	/* an "(info only)" row records a difference without failing the run */
	int info = strstr(name, "(info only)") != NULL;
	printf("%-52s %s  %s\n", name, info ? "INFO" : ok ? "PASS" : "FAIL",
		detail);
	if (info)
		return;
	sRows++;
	if (!ok)
		sFailures++;
}


static const char*
errno_name(int error)
{
	switch (error) {
		case 0: return "0";
		case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
		case EINVAL: return "EINVAL";
		case ENXIO: return "ENXIO";
		case ENODEV: return "ENODEV";
		case ENETUNREACH: return "ENETUNREACH";
		case EOPNOTSUPP: return "EOPNOTSUPP";
		case ENOPROTOOPT: return "ENOPROTOOPT";
		case EAGAIN: return "EAGAIN";
		default: return strerror(error);
	}
}


static socklen_t
make_address(int af, const char* text, int port, struct sockaddr_storage* out)
{
	memset(out, 0, sizeof(*out));
	if (af == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)out;
		in->sin_family = AF_INET;
		in->sin_port = htons(port);
		if (inet_pton(AF_INET, text, &in->sin_addr) != 1)
			return 0;
		return sizeof(*in);
	}
	struct sockaddr_in6* in6 = (struct sockaddr_in6*)out;
	in6->sin6_family = AF_INET6;
	in6->sin6_port = htons(port);
	if (inet_pton(AF_INET6, text, &in6->sin6_addr) != 1)
		return 0;
	return sizeof(*in6);
}


static void
address_text(const struct sockaddr_storage* address, char* text, size_t size)
{
	char host[INET6_ADDRSTRLEN] = "?";
	int port = 0;
	if (address->ss_family == AF_INET) {
		const struct sockaddr_in* in = (const struct sockaddr_in*)address;
		inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
		port = ntohs(in->sin_port);
	} else if (address->ss_family == AF_INET6) {
		const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)address;
		inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
		port = ntohs(in6->sin6_port);
	}
	snprintf(text, size, "%s.%d", host, port);
}


/* A UDP socket of family af bound to bindText:0, with a 2 s receive timeout. */
static int
make_socket(int af, const char* bindText, struct sockaddr_storage* bound)
{
	int fd = socket(af, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0)
		return -1;

	struct sockaddr_storage address;
	socklen_t length = make_address(af, bindText, 0, &address);
	if (length == 0 || bind(fd, (struct sockaddr*)&address, length) != 0) {
		close(fd);
		return -1;
	}

	socklen_t boundLength = sizeof(*bound);
	getsockname(fd, (struct sockaddr*)bound, &boundLength);

	struct timeval timeout = { 2, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	return fd;
}


enum {
	kIpPktinfo,
	kIpSendSrcAddr,
	kIpv6Pktinfo,
	kBadLength,
	kIpTtl
};


/* sendmsg() of "data" to "to", with one control message of the given kind
   carrying source (an address string of its family, or NULL) and ifindex.
   Returns sendmsg()'s result; errno is preserved. */
static ssize_t
send_with(int fd, int kind, const struct sockaddr_storage* to,
	const char* source, unsigned ifindex, const char* data)
{
	union {
		struct cmsghdr align;
		char buffer[128];
	} control;
	memset(&control, 0, sizeof(control));

	struct iovec vector = { (void*)data, strlen(data) };
	struct msghdr header;
	memset(&header, 0, sizeof(header));
	header.msg_name = (void*)to;
	header.msg_namelen = to->ss_family == AF_INET
		? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.buffer;

	struct cmsghdr* message = (struct cmsghdr*)control.buffer;
	switch (kind) {
		case kIpPktinfo:
		case kBadLength:
		{
#if defined(IP_PKTINFO)
			struct in_pktinfo info;
			memset(&info, 0, sizeof(info));
			if (source != NULL)
				inet_pton(AF_INET, source, &info.ipi_spec_dst);
			info.ipi_ifindex = ifindex;
			message->cmsg_len = CMSG_LEN(sizeof(info));
			if (kind == kBadLength)
				message->cmsg_len = CMSG_LEN(sizeof(info) - 4);
			message->cmsg_level = IPPROTO_IP;
			message->cmsg_type = IP_PKTINFO;
			memcpy(CMSG_DATA(message), &info, sizeof(info));
			header.msg_controllen = CMSG_SPACE(sizeof(info));
			break;
#else
			errno = ENOPROTOOPT;
			return -2;
#endif
		}
		case kIpSendSrcAddr:
		{
#if defined(IP_SENDSRCADDR)
			struct in_addr address;
			inet_pton(AF_INET, source, &address);
			message->cmsg_len = CMSG_LEN(sizeof(address));
			message->cmsg_level = IPPROTO_IP;
			message->cmsg_type = IP_SENDSRCADDR;
			memcpy(CMSG_DATA(message), &address, sizeof(address));
			header.msg_controllen = CMSG_SPACE(sizeof(address));
			break;
#else
			errno = ENOPROTOOPT;
			return -2;
#endif
		}
		case kIpv6Pktinfo:
		{
			struct in6_pktinfo info;
			memset(&info, 0, sizeof(info));
			if (source != NULL)
				inet_pton(AF_INET6, source, &info.ipi6_addr);
			info.ipi6_ifindex = ifindex;
			message->cmsg_len = CMSG_LEN(sizeof(info));
			message->cmsg_level = IPPROTO_IPV6;
			message->cmsg_type = IPV6_PKTINFO;
			memcpy(CMSG_DATA(message), &info, sizeof(info));
			header.msg_controllen = CMSG_SPACE(sizeof(info));
			break;
		}
		case kIpTtl:
		{
			int ttl = 7;
			message->cmsg_len = CMSG_LEN(sizeof(ttl));
			message->cmsg_level = IPPROTO_IP;
			message->cmsg_type = IP_TTL;
			memcpy(CMSG_DATA(message), &ttl, sizeof(ttl));
			header.msg_controllen = CMSG_SPACE(sizeof(ttl));
			break;
		}
	}

	return sendmsg(fd, &header, 0);
}


/* Receives one datagram; returns its length and the sender, and describes
   any IP_PKTINFO into info (if non-NULL). */
static ssize_t
receive_from(int fd, struct sockaddr_storage* from, char* info,
	size_t infoSize)
{
	char data[256];
	union {
		struct cmsghdr align;
		char buffer[128];
	} control;
	struct iovec vector = { data, sizeof(data) };
	struct msghdr header;
	memset(&header, 0, sizeof(header));
	memset(from, 0, sizeof(*from));
	header.msg_name = from;
	header.msg_namelen = sizeof(*from);
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.buffer;
	header.msg_controllen = sizeof(control.buffer);

	ssize_t received = recvmsg(fd, &header, 0);
	if (info != NULL)
		info[0] = '\0';
	if (received < 0 || info == NULL)
		return received;

	size_t used = 0;
	struct cmsghdr* message;
	for (message = CMSG_FIRSTHDR(&header); message != NULL;
			message = CMSG_NXTHDR(&header, message)) {
#if defined(IP_PKTINFO)
		if (message->cmsg_level == IPPROTO_IP
			&& message->cmsg_type == IP_PKTINFO) {
			struct in_pktinfo pi;
			char a[INET_ADDRSTRLEN], s[INET_ADDRSTRLEN];
			memcpy(&pi, CMSG_DATA(message), sizeof(pi));
			inet_ntop(AF_INET, &pi.ipi_addr, a, sizeof(a));
			inet_ntop(AF_INET, &pi.ipi_spec_dst, s, sizeof(s));
			used += snprintf(info + used, infoSize - used,
				"[IP_PKTINFO addr=%s spec_dst=%s ifindex=%u]", a, s,
				(unsigned)pi.ipi_ifindex);
			continue;
		}
#endif
		used += snprintf(info + used, infoSize - used, "[cmsg %d/%d]",
			message->cmsg_level, message->cmsg_type);
	}
	return received;
}


/* Sends with a control message from fd to the receiver rfd (bound to to),
   and checks the outcome: expectErrno 0 means "delivered, from expectFrom". */
static void
check_send(const char* name, int fd, int rfd, int kind,
	const struct sockaddr_storage* to, const char* source, unsigned ifindex,
	int expectErrno, const char* expectFrom)
{
	char detail[512];
	errno = 0;
	ssize_t sent = send_with(fd, kind, to, source, ifindex, "pktinfo981");
	int error = sent < 0 ? errno : 0;
	if (sent == -2) {
		printf("%-52s SKIP  option not defined by the headers\n", name);
		return;
	}

	if (expectErrno != 0 || sent < 0) {
		snprintf(detail, sizeof(detail), "sendmsg=%zd %s (want %s)", sent,
			errno_name(error), errno_name(expectErrno));
		row(name, sent < 0 && error == expectErrno, detail);
		if (sent >= 0) {
			/* drain it */
			struct sockaddr_storage from;
			receive_from(rfd, &from, NULL, 0);
		}
		return;
	}

	struct sockaddr_storage from;
	ssize_t received = receive_from(rfd, &from, NULL, 0);
	char fromText[80] = "-";
	if (received >= 0)
		address_text(&from, fromText, sizeof(fromText));
	int ok = received == 10;
	if (ok && expectFrom != NULL) {
		size_t length = strlen(expectFrom);
		ok = strncmp(fromText, expectFrom, length) == 0
			&& fromText[length] == '.';
	}
	snprintf(detail, sizeof(detail), "sendmsg=%zd recv=%zd from %s (want %s)",
		sent, received, fromText, expectFrom != NULL ? expectFrom : "any");
	row(name, ok, detail);
}


static unsigned
loopback_index(void)
{
	unsigned index = if_nametoindex("loop");
	if (index == 0)
		index = if_nametoindex("lo");
	if (index == 0)
		index = if_nametoindex("lo0");
	return index;
}


static void
run_ipv4(const char* enaAddress)
{
	struct sockaddr_storage a1, a2;
	char name[96];
	int fd1 = make_socket(AF_INET, "0.0.0.0", &a1);
	int fd2 = make_socket(AF_INET, "127.0.0.1", &a2);
	if (fd1 < 0 || fd2 < 0) {
		row("ipv4 setup", 0, strerror(errno));
		return;
	}
	unsigned lo = loopback_index();

	check_send("ipv4 IP_PKTINFO src=127.0.0.1", fd1, fd2, kIpPktinfo, &a2,
		"127.0.0.1", 0, 0, "127.0.0.1");
	{
		/* 127.0.0.2 is local exactly where bind() takes it (all of 127/8 on
		   Linux, only 127.0.0.1 on DeBeOS) */
		struct sockaddr_storage probe;
		int fd = make_socket(AF_INET, "127.0.0.2", &probe);
		int local = fd >= 0;
		if (fd >= 0)
			close(fd);
		check_send(local ? "ipv4 IP_PKTINFO src=127.0.0.2 (bind() takes it)"
				: "ipv4 IP_PKTINFO src=127.0.0.2 (bind() refuses it)",
			fd1, fd2, kIpPktinfo, &a2, "127.0.0.2", 0,
			local ? 0 : EADDRNOTAVAIL, local ? "127.0.0.2" : NULL);
	}
	check_send("ipv4 IP_PKTINFO src=192.0.2.1 (foreign)", fd1, fd2,
		kIpPktinfo, &a2, "192.0.2.1", 0, FOREIGN4, NULL);
	check_send("ipv4 IP_PKTINFO src=255.255.255.255 (broadcast)", fd1, fd2,
		kIpPktinfo, &a2, "255.255.255.255", 0, EINVAL, NULL);
	check_send("ipv4 IP_PKTINFO src=224.0.0.1 (multicast)", fd1, fd2,
		kIpPktinfo, &a2, "224.0.0.1", 0, EINVAL, NULL);
	check_send("ipv4 IP_PKTINFO src=0.0.0.0 ifindex=lo", fd1, fd2,
		kIpPktinfo, &a2, "0.0.0.0", lo, 0, "127.0.0.1");
	check_send("ipv4 IP_PKTINFO src=127.0.0.1 ifindex=lo", fd1, fd2,
		kIpPktinfo, &a2, "127.0.0.1", lo, 0, "127.0.0.1");
	check_send("ipv4 IP_PKTINFO ifindex=9999 (none)", fd1, fd2, kIpPktinfo,
		&a2, "0.0.0.0", 9999, ENODEV, NULL);
	check_send("ipv4 IP_PKTINFO short cmsg_len", fd1, fd2, kBadLength, &a2,
		"127.0.0.1", 0, EINVAL, NULL);
	check_send("ipv4 IP_SENDSRCADDR src=127.0.0.1 (BSD)", fd1, fd2,
		kIpSendSrcAddr, &a2, "127.0.0.1", 0, 0, "127.0.0.1");
	check_send("ipv4 IP_SENDSRCADDR src=192.0.2.1 (BSD)", fd1, fd2,
		kIpSendSrcAddr, &a2, "192.0.2.1", 0, FOREIGN4, NULL);
	check_send("ipv4 IP_TTL cmsg (info only)", fd1, fd2, kIpTtl, &a2,
		NULL, 0, 0, "127.0.0.1");
	if (enaAddress != NULL) {
		snprintf(name, sizeof(name), "ipv4 IP_PKTINFO src=%s to loopback",
			enaAddress);
		check_send(name, fd1, fd2, kIpPktinfo, &a2, enaAddress, 0, 0,
			enaAddress);
	}

	/* the source applies to a connected socket too */
	int fd3 = make_socket(AF_INET, "0.0.0.0", &a1);
	connect(fd3, (struct sockaddr*)&a2, sizeof(struct sockaddr_in));
	{
		union {
			struct cmsghdr align;
			char buffer[64];
		} control;
		memset(&control, 0, sizeof(control));
		struct iovec vector = { (void*)"pktinfo981", 10 };
		struct msghdr header;
		memset(&header, 0, sizeof(header));
		header.msg_iov = &vector;
		header.msg_iovlen = 1;
		header.msg_control = control.buffer;
		ssize_t sent = -2;
#if defined(IP_PKTINFO)
		struct cmsghdr* message = (struct cmsghdr*)control.buffer;
		struct in_pktinfo info;
		memset(&info, 0, sizeof(info));
		inet_pton(AF_INET, enaAddress != NULL ? enaAddress : "127.0.0.1",
			&info.ipi_spec_dst);
		message->cmsg_len = CMSG_LEN(sizeof(info));
		message->cmsg_level = IPPROTO_IP;
		message->cmsg_type = IP_PKTINFO;
		memcpy(CMSG_DATA(message), &info, sizeof(info));
		header.msg_controllen = CMSG_SPACE(sizeof(info));
		sent = sendmsg(fd3, &header, 0);
#endif
		struct sockaddr_storage from;
		char fromText[80] = "-", detail[256];
		ssize_t received = sent == 10 ? receive_from(fd2, &from, NULL, 0) : -1;
		if (received >= 0)
			address_text(&from, fromText, sizeof(fromText));
		snprintf(detail, sizeof(detail), "sendmsg=%zd recv=%zd from %s", sent,
			received, fromText);
		row("ipv4 IP_PKTINFO on a connected socket", received == 10
			&& strncmp(fromText, enaAddress != NULL ? enaAddress
				: "127.0.0.1", strlen(enaAddress != NULL ? enaAddress
				: "127.0.0.1")) == 0, detail);
	}
	close(fd3);

	/* receive side: IP_PKTINFO */
#if defined(IP_PKTINFO)
	int one = 1;
	int result = setsockopt(fd2, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
	int value = 0;
	socklen_t valueLength = sizeof(value);
	getsockopt(fd2, IPPROTO_IP, IP_PKTINFO, &value, &valueLength);
	char detail[512], info[256], fromText[80];
	snprintf(detail, sizeof(detail), "setsockopt=%d getsockopt=%d", result,
		value);
	row("ipv4 IP_PKTINFO setsockopt/getsockopt", result == 0 && value != 0,
		detail);
	sendto(fd1, "pktinfo981", 10, 0, (struct sockaddr*)&a2,
		sizeof(struct sockaddr_in));
	struct sockaddr_storage from;
	ssize_t received = receive_from(fd2, &from, info, sizeof(info));
	address_text(&from, fromText, sizeof(fromText));
	char want[128];
	snprintf(want, sizeof(want),
		"[IP_PKTINFO addr=127.0.0.1 spec_dst=127.0.0.1 ifindex=%u]", lo);
	snprintf(detail, sizeof(detail), "recv=%zd %s (want %s)", received, info,
		want);
	row("ipv4 recv IP_PKTINFO cmsg", received == 10
		&& strcmp(info, want) == 0, detail);
#if defined(IP_RECVDSTADDR)
	/* both destination options: both control messages, in one buffer */
	setsockopt(fd2, IPPROTO_IP, IP_RECVDSTADDR, &one, sizeof(one));
	sendto(fd1, "pktinfo981", 10, 0, (struct sockaddr*)&a2,
		sizeof(struct sockaddr_in));
	received = receive_from(fd2, &from, info, sizeof(info));
	snprintf(want, sizeof(want), "[cmsg 0/%d][IP_PKTINFO addr=127.0.0.1 "
		"spec_dst=127.0.0.1 ifindex=%u]", IP_RECVDSTADDR, lo);
	snprintf(detail, sizeof(detail), "recv=%zd %s (want %s)", received, info,
		want);
	row("ipv4 recv IP_RECVDSTADDR + IP_PKTINFO cmsgs", received == 10
		&& strcmp(info, want) == 0, detail);

	/* a control buffer too small for in_pktinfo: the datagram is still
	   returned, with MSG_CTRUNC; with room for IP_RECVDSTADDR only, that one
	   is returned */
	size_t sizes[2] = { CMSG_SPACE(sizeof(struct in_addr)) - 1,
		CMSG_SPACE(sizeof(struct in_addr)) };
	for (int k = 0; k < 2; k++) {
		union {
			struct cmsghdr align;
			char buffer[128];
		} small;
		char data[32];
		struct iovec vector = { data, sizeof(data) };
		struct msghdr header;
		memset(&header, 0, sizeof(header));
		header.msg_iov = &vector;
		header.msg_iovlen = 1;
		header.msg_control = small.buffer;
		header.msg_controllen = sizes[k];
		sendto(fd1, "pktinfo981", 10, 0, (struct sockaddr*)&a2,
			sizeof(struct sockaddr_in));
		received = recvmsg(fd2, &header, 0);
		int firstType = header.msg_controllen >= sizeof(struct cmsghdr)
			? CMSG_FIRSTHDR(&header)->cmsg_type : -1;
		snprintf(detail, sizeof(detail),
			"recvmsg=%zd controllen=%u flags=0x%x first cmsg type %d",
			received, (unsigned)header.msg_controllen, header.msg_flags,
			firstType);
		row(k == 0 ? "ipv4 recv both options, no room: MSG_CTRUNC"
				: "ipv4 recv both options, room for one: MSG_CTRUNC",
			received == 10 && (header.msg_flags & MSG_CTRUNC) != 0
				&& (k == 0 ? header.msg_controllen == 0
					: firstType == IP_RECVDSTADDR),
			detail);
	}
#endif
#else
	row("ipv4 IP_PKTINFO setsockopt/getsockopt", 0,
		"IP_PKTINFO not defined by the headers");
#endif

	close(fd1);
	close(fd2);
}


static void
run_ipv6(const char* enaAddress)
{
	struct sockaddr_storage a1, a2;
	char name[96];
	int fd1 = make_socket(AF_INET6, "::", &a1);
	int fd2 = make_socket(AF_INET6, "::1", &a2);
	if (fd1 < 0 || fd2 < 0) {
		row("ipv6 setup", 0, strerror(errno));
		return;
	}
	unsigned lo = loopback_index();

	check_send("ipv6 IPV6_PKTINFO src=::1", fd1, fd2, kIpv6Pktinfo, &a2,
		"::1", 0, 0, "::1");
	check_send("ipv6 IPV6_PKTINFO src=2001:db8::1 (foreign)", fd1, fd2,
		kIpv6Pktinfo, &a2, "2001:db8::1", 0, FOREIGN6, NULL);
	check_send("ipv6 IPV6_PKTINFO src=ff02::1 (multicast)", fd1, fd2,
		kIpv6Pktinfo, &a2, "ff02::1", 0, EINVAL, NULL);
	check_send("ipv6 IPV6_PKTINFO src=:: ifindex=lo", fd1, fd2,
		kIpv6Pktinfo, &a2, "::", lo, 0, "::1");
	check_send("ipv6 IPV6_PKTINFO src=::1 ifindex=lo", fd1, fd2,
		kIpv6Pktinfo, &a2, "::1", lo, 0, "::1");
	check_send("ipv6 IPV6_PKTINFO ifindex=9999 (none)", fd1, fd2,
		kIpv6Pktinfo, &a2, "::", 9999, ENODEV, NULL);
	if (enaAddress != NULL) {
		snprintf(name, sizeof(name), "ipv6 IPV6_PKTINFO src=%s to loopback",
			enaAddress);
		check_send(name, fd1, fd2, kIpv6Pktinfo, &a2, enaAddress, 0, 0,
			enaAddress);
	}

	/* dual-stack: an IPv4 datagram of an IPv6 socket takes a mapped source */
	struct sockaddr_storage v4a;
	int v4 = make_socket(AF_INET, "127.0.0.1", &v4a);
	struct sockaddr_storage mapped;
	char port[16];
	snprintf(port, sizeof(port), "%d",
		ntohs(((struct sockaddr_in*)&v4a)->sin_port));
	make_address(AF_INET6, "::ffff:127.0.0.1",
		ntohs(((struct sockaddr_in*)&v4a)->sin_port), &mapped);
	check_send("ipv6 IPV6_PKTINFO mapped src to mapped dst", fd1, v4,
		kIpv6Pktinfo, &mapped, "::ffff:127.0.0.1", 0, 0, "127.0.0.1");
	check_send("ipv6 IPV6_PKTINFO src=::1 to mapped dst", fd1, v4,
		kIpv6Pktinfo, &mapped, "::1", 0, EINVAL, NULL);
	close(v4);

	close(fd1);
	close(fd2);
}


static int
do_send(const char* peer, int port, const char* source, unsigned ifindex)
{
	int af = strchr(peer, ':') != NULL ? AF_INET6 : AF_INET;
	struct sockaddr_storage to;
	if (make_address(af, peer, port, &to) == 0) {
		fprintf(stderr, "bad peer %s\n", peer);
		return 2;
	}
	int fd = socket(af, SOCK_DGRAM, IPPROTO_UDP);
	/* so that a broadcast destination is refused for its source, not for
	   want of SO_BROADCAST */
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	char data[128];
	snprintf(data, sizeof(data), "pktinfo981 from %s", source);
	errno = 0;
	ssize_t sent;
	if (strcmp(source, "-") == 0) {
		/* a plain sendto(), no control message */
		sent = sendto(fd, data, strlen(data), 0, (struct sockaddr*)&to,
			af == AF_INET ? sizeof(struct sockaddr_in)
				: sizeof(struct sockaddr_in6));
	} else {
		sent = send_with(fd, af == AF_INET ? kIpPktinfo : kIpv6Pktinfo, &to,
			source, ifindex, data);
	}
	int error = sent < 0 ? errno : 0;
	printf("send %s -> %s.%d ifindex=%u: %s=%zd %s\n", source, peer, port,
		ifindex, strcmp(source, "-") == 0 ? "sendto" : "sendmsg", sent,
		errno_name(error));
	close(fd);
	return sent < 0 ? 1 : 0;
}


static int
do_recv(int port, int count)
{
	struct sockaddr_storage bound;
	int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	int zero = 0;
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
	socklen_t length = make_address(AF_INET6, "::", port, &bound);
	if (bind(fd, (struct sockaddr*)&bound, length) != 0) {
		perror("bind");
		return 2;
	}
	struct timeval timeout = { 20, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	for (int i = 0; i < count; i++) {
		char data[256];
		struct sockaddr_storage from;
		socklen_t fromLength = sizeof(from);
		ssize_t received = recvfrom(fd, data, sizeof(data) - 1, 0,
			(struct sockaddr*)&from, &fromLength);
		if (received < 0) {
			printf("recv: %s\n", strerror(errno));
			break;
		}
		data[received] = '\0';
		char fromText[80];
		address_text(&from, fromText, sizeof(fromText));
		printf("recv from %s: \"%s\"\n", fromText, data);
		fflush(stdout);
	}
	close(fd);
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc >= 5 && strcmp(argv[1], "send") == 0) {
		/* the interface, by index or by name */
		unsigned ifindex = 0;
		if (argc > 5) {
			ifindex = (unsigned)atoi(argv[5]);
			if (ifindex == 0)
				ifindex = if_nametoindex(argv[5]);
		}
		return do_send(argv[2], atoi(argv[3]), argv[4], ifindex);
	}
	if (argc >= 3 && strcmp(argv[1], "recv") == 0)
		return do_recv(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 1);

	const char* ena4 = NULL;
	const char* ena6 = NULL;
	int option;
	while ((option = getopt(argc, argv, "l:L:")) != -1) {
		if (option == 'l')
			ena4 = optarg;
		else if (option == 'L')
			ena6 = optarg;
		else
			return 2;
	}

	run_ipv4(ena4);
	run_ipv6(ena6);
	printf("%s: %d of %d row(s) failed\n", sFailures == 0 ? "PASS" : "FAIL",
		sFailures, sRows);
	return sFailures == 0 ? 0 : 1;
}
