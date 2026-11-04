/*
 * timeexceeded1098 -- when an IPv6 datagram's reassembly times out, is the
 * sender told with a Time Exceeded (code 1) that quotes the invoking packet,
 * and only if its first fragment arrived? (RFC 8200 4.5, issue 1098)
 *
 * Two well-formed datagrams of 3000 bytes to an unbound UDP port on ::1, each
 * cut short, sent from a raw IPPROTO_FRAGMENT socket (the kernel puts the
 * IPv6 header in front), as graviton/scripts/ipfragconf.c does:
 *   first: only the fragment at offset 0, [0,1000) M=1;
 *   later: only the fragment at offset 1000, [1000,2000) M=1.
 * A raw ICMPv6 socket then waits up to 75 s. The first datagram must draw one
 * Time Exceeded whose quote is the IPv6 header (::1 -> ::1, next header 44,
 * payload length 1008), the fragment header (next header 17, offset 0, M=1,
 * the same identification), and the fragment's data, UDP header first. The
 * second must draw none: no first fragment arrived.
 *
 * Takes about 75 s. On Linux, net.ipv6.ip6frag_time is 60 by default.
 * Build: cc -o timeexceeded1098 timeexceeded1098.c   (add -lnetwork on Haiku)
 * Exit status: the number of FAILs.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef IPPROTO_FRAGMENT
#	define IPPROTO_FRAGMENT	44
#endif


static int sFailures;


static void
result(const char* name, int pass, const char* detail)
{
	printf("%-48s %s  %s\n", name, pass ? "PASS" : "FAIL", detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static int
send_fragment(const unsigned char* data, size_t length, size_t offset,
	int more, uint32_t ident)
{
	unsigned char packet[8 + 2048];
	int fd = socket(AF_INET6, SOCK_RAW, IPPROTO_FRAGMENT);
	if (fd < 0)
		return -1;
	packet[0] = IPPROTO_UDP;
	packet[1] = 0;
	uint16_t offlg = (uint16_t)(offset & 0xfff8) | (more ? 1 : 0);
	packet[2] = offlg >> 8;
	packet[3] = offlg & 0xff;
	packet[4] = ident >> 24; packet[5] = ident >> 16;
	packet[6] = ident >> 8; packet[7] = ident;
	memcpy(packet + 8, data, length);

	struct sockaddr_in6 to;
	memset(&to, 0, sizeof(to));
	to.sin6_family = AF_INET6;
	to.sin6_addr = in6addr_loopback;
	ssize_t sent = sendto(fd, packet, 8 + length, 0, (struct sockaddr*)&to,
		sizeof(to));
	int error = errno;
	close(fd);
	errno = error;
	return sent == (ssize_t)(8 + length) ? 0 : -1;
}


struct seen {
	int		count;
	int		code;
	ssize_t	length;
	char	detail[256];
};


/*!	Checks a Time Exceeded quote against the first fragment of \a ident. */
static void
check_quote(const unsigned char* icmp, ssize_t length, uint32_t ident,
	const unsigned char* udp, struct seen* seen)
{
	const unsigned char* ip = icmp + 8;
	const unsigned char* fragment = ip + 40;
	const unsigned char* data = fragment + 8;
	ssize_t dataLength = length - 8 - 40 - 8;

	char problems[160] = "";
	int plen = ip[4] << 8 | ip[5];
	int offlg = fragment[2] << 8 | fragment[3];
	if ((ip[0] >> 4) != 6)
		strcat(problems, " version");
	if (memcmp(ip + 8, &in6addr_loopback, 16) != 0
		|| memcmp(ip + 24, &in6addr_loopback, 16) != 0)
		strcat(problems, " addresses");
	if (ip[6] != IPPROTO_FRAGMENT)
		strcat(problems, " ip6_nxt");
	if (plen != 1008)
		strcat(problems, " plen");
	if (fragment[0] != IPPROTO_UDP)
		strcat(problems, " frag-nxt");
	if (offlg != 1)
		strcat(problems, " offset/M");
	if (dataLength != 1000 || memcmp(data, udp, 1000) != 0)
		strcat(problems, " data");

	snprintf(seen->detail, sizeof(seen->detail),
		"code %d, %zd bytes; quote: nxt %u, plen %d, frag nxt %u "
		"offlg 0x%04x, %zd data bytes%s%s", icmp[1], length, ip[6], plen,
		fragment[0], offlg, dataLength, problems[0] ? "; WRONG:" : ", intact",
		problems);
	seen->code = icmp[1];
	seen->length = length;
	seen->count++;
	(void)ident;
}


int
main(void)
{
	int capture = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (capture < 0) {
		printf("raw ICMPv6 socket: %s (run as root)\n", strerror(errno));
		return 2;
	}

	srandom((unsigned)time(NULL) ^ (unsigned)getpid());
	uint32_t firstIdent = (uint32_t)random();
	uint32_t laterIdent = firstIdent + 1;
	unsigned short port = 45000 + (unsigned short)(random() % 4000);

	// A UDP datagram of 3000 bytes to an unbound port; the checksum does not
	// matter, as it is never reassembled.
	unsigned char udp[3000];
	for (size_t i = 0; i < sizeof(udp); i++)
		udp[i] = (unsigned char)(i * 7 + 1098);
	udp[0] = 0xa0; udp[1] = 0x28;
	udp[2] = port >> 8; udp[3] = port & 0xff;
	udp[4] = sizeof(udp) >> 8; udp[5] = sizeof(udp) & 0xff;
	udp[6] = udp[7] = 0;

	if (send_fragment(udp, 1000, 0, 1, firstIdent) != 0
		|| send_fragment(udp + 1000, 1000, 1000, 1, laterIdent) != 0) {
		printf("raw IPPROTO_FRAGMENT send: %s\n", strerror(errno));
		return 2;
	}
	time_t start = time(NULL);
	printf("t=0s sent [0,1000) of ident 0x%08x and [1000,2000) of ident "
		"0x%08x; waiting 75 s\n", firstIdent, laterIdent);
	fflush(stdout);

	struct seen first = { 0 }, later = { 0 };
	int firstAt = -1;
	while (time(NULL) < start + 75) {
		struct pollfd entry = { capture, POLLIN, 0 };
		if (poll(&entry, 1, 1000) <= 0)
			continue;
		unsigned char buffer[2048];
		ssize_t length = recv(capture, buffer, sizeof(buffer), 0);
		if (length < 8 + 40 + 8 || buffer[0] != ICMP6_TIME_EXCEEDED)
			continue;
		const unsigned char* fragment = buffer + 8 + 40;
		uint32_t ident = (uint32_t)fragment[4] << 24 | fragment[5] << 16
			| fragment[6] << 8 | fragment[7];
		if (buffer[8 + 6] != IPPROTO_FRAGMENT)
			continue;
		if (ident == firstIdent) {
			if (first.count == 0)
				firstAt = (int)(time(NULL) - start);
			check_quote(buffer, length, ident, udp, &first);
		} else if (ident == laterIdent)
			check_quote(buffer, length, ident, udp, &later);
	}

	char detail[320];
	if (first.count == 0)
		snprintf(detail, sizeof(detail), "none in 75 s");
	else {
		snprintf(detail, sizeof(detail), "%d after %d s: %s", first.count,
			firstAt, first.detail);
	}
	result("first fragment only: one Time Exceeded, code 1",
		first.count == 1 && first.code == ICMP6_TIME_EXCEED_REASSEMBLY
			&& strstr(first.detail, "intact") != NULL
			&& firstAt >= 55 && firstAt <= 70, detail);

	snprintf(detail, sizeof(detail), "%d%s%s", later.count,
		later.count ? ": " : " in 75 s", later.detail);
	result("later fragment only: no Time Exceeded", later.count == 0, detail);

	printf("FAILURES %d\n", sFailures);
	return sFailures;
}
