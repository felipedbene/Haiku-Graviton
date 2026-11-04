/*
 * ipfragconf.c -- IPv4 and IPv6 fragment reassembly conformance over the
 * loopback (issues 1034, 1035, 1036).
 *
 * Small, well-formed test vectors that check delivery semantics only; nothing
 * here loads the stack. Each row sends one UDP datagram to 127.0.0.1 or ::1,
 * either through the kernel's UDP (so the kernel fragments it, with the
 * loopback MTU lowered), or as fragments built here and sent through a raw
 * socket: IP_HDRINCL for IPv4, an IPPROTO_FRAGMENT socket for IPv6 (the
 * kernel puts the IPv6 header in front).
 *
 * Rows:
 *   send     kernel-fragmented UDP datagrams; delivered intact.
 *   deliver  crafted sets that must be delivered intact: in order, reversed,
 *            middle first, a duplicate, the last fragment first.
 *   drop     crafted sets that must NOT be delivered (RFC 791, RFC 8200 4.5,
 *            RFC 5722):
 *              gap:     the middle fragment never arrives;
 *              past:    [0,8) M=1, [16,24) M=0, [24,32) M=1 (issue 1035):
 *                       the data past the end makes up for the hole in a
 *                       byte count;
 *              short:   [0,8) M=1, [24,32) M=1, then a last fragment
 *                       [16,24) that ends before data already received;
 *              twoends: [0,8) M=1, last [16,24), then a second last
 *                       fragment [8,16) with a different end;
 *              covered: [0,1008) M=1, then a last fragment [1000,1008)
 *                       wholly inside it;
 *              overlap: [0,1008) M=1, then a last fragment [1000,1016)
 *                       partly over it: IPv6 only (RFC 5722), reported as
 *                       INFO for IPv4, where either outcome is allowed;
 *              short total length (IPv4): a header length of 60 with a
 *                       total length of 20.
 *            "last fragment twice, then the middle" must be delivered.
 *            The bytes of "past" and "short" are chosen so that joining the
 *            pieces back to back gives a valid UDP datagram, so a receiver
 *            that does that delivers it and the row fails.
 *   timeout  (-t only, takes ~72 s) per RFC 8200 4.5, reassembly must be
 *            abandoned 60 s after the FIRST fragment:
 *              within:    first at 0 s, last at 50 s: delivered;
 *              refreshed: first at 0 s, middle at 40 s, last at 70 s: NOT
 *                         delivered; a timer restarted by every fragment
 *                         would still be running and deliver it.
 *            On Linux, set net.ipv4.ipfrag_time=60 first (its default is
 *            30 s); net.ipv6.ip6frag_time is already 60.
 *
 * Usage: ipfragconf [-s] [-c] [-t]   (no option: -s and -c)
 *   -s  send rows (lower the loopback MTU first, e.g. to 1280)
 *   -c  crafted deliver and drop rows
 *   -t  timeout rows
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
#ifndef IP_HDRINCL
#	define IP_HDRINCL	2
#endif

static int sFailures;
static unsigned short sPort = 41000;
static uint32_t sIdent;


static void
result(const char* name, int pass, const char* detail)
{
	printf("%-50s %s  %s\n", name, pass ? "PASS" : "FAIL", detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static const char*
family_name(int family)
{
	return family == AF_INET ? "v4" : "v6";
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
	UDP. Returns whether they arrived intact.
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


static int
loopback_alive(void)
{
	char detail[128];
	return udp_round_trip(AF_INET, 100, detail, sizeof(detail))
		&& udp_round_trip(AF_INET6, 100, detail, sizeof(detail));
}


static int
check_alive(const char* after)
{
	if (loopback_alive()) {
		printf("  loopback alive after: %s\n", after);
		return 1;
	}
	printf("  LOOPBACK DEAD after: %s\n", after);
	fflush(stdout);
	return 0;
}


/*!	The UDP checksum of \a udp from and to the loopback address. */
static uint16_t
udp_checksum(int family, const unsigned char* udp, size_t length)
{
	uint32_t sum = 0;
	if (family == AF_INET6) {
		sum += 2;
			// the two 0x0001 words of ::1 -> ::1
		sum += (uint32_t)(length >> 16);
	} else
		sum += 2 * (0x7f00 + 0x0001);
			// 127.0.0.1 -> 127.0.0.1
	sum += (uint32_t)(length & 0xffff);
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


/*!	Builds a UDP datagram of \a length bytes, header included, for \a port. */
static unsigned char*
make_udp(int family, unsigned short port, size_t length)
{
	unsigned char* udp = calloc(1, length);
	udp[0] = 0xa0; udp[1] = 0x28;
		// source port 41000; not looked at
	udp[2] = port >> 8; udp[3] = port & 0xff;
	udp[4] = length >> 8; udp[5] = length & 0xff;
	fill(udp + 8, length - 8, 1035);
	uint16_t checksum = udp_checksum(family, udp, length);
	udp[6] = checksum >> 8; udp[7] = checksum & 0xff;
	return udp;
}


static uint16_t
ip_checksum(const unsigned char* header, size_t length)
{
	uint32_t sum = 0;
	for (size_t i = 0; i + 1 < length; i += 2)
		sum += (uint32_t)(header[i] << 8 | header[i + 1]);
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}


/*!	Sends \a length bytes of \a data as the fragment at byte \a offset of the
	datagram \a ident, with the more fragments flag \a more.
*/
static int
send_fragment(int family, const unsigned char* data, size_t length,
	size_t offset, int more, uint32_t ident)
{
	unsigned char packet[40 + 2048];
	if (length > 2048) {
		errno = EMSGSIZE;
		return -1;
	}

	int fd;
	size_t size;
	ssize_t sent;
	if (family == AF_INET6) {
		fd = socket(AF_INET6, SOCK_RAW, IPPROTO_FRAGMENT);
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
		size = 8 + length;

		struct sockaddr_in6 to;
		memset(&to, 0, sizeof(to));
		to.sin6_family = AF_INET6;
		to.sin6_addr = in6addr_loopback;
		sent = sendto(fd, packet, size, 0, (struct sockaddr*)&to, sizeof(to));
	} else {
		fd = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
		if (fd < 0)
			return -1;
		int on = 1;
		if (setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on)) != 0) {
			int error = errno;
			close(fd);
			errno = error;
			return -1;
		}
		size = 20 + length;
		memset(packet, 0, 20);
		packet[0] = 0x45;
		packet[2] = size >> 8; packet[3] = size & 0xff;
		packet[4] = (ident >> 8) & 0xff; packet[5] = ident & 0xff;
		uint16_t offlg = (uint16_t)((offset >> 3) & 0x1fff)
			| (more ? 0x2000 : 0);
		packet[6] = offlg >> 8; packet[7] = offlg & 0xff;
		packet[8] = 64;
		packet[9] = IPPROTO_UDP;
		packet[12] = 127; packet[15] = 1;
		packet[16] = 127; packet[19] = 1;
		uint16_t checksum = ip_checksum(packet, 20);
		packet[10] = checksum >> 8; packet[11] = checksum & 0xff;
		memcpy(packet + 20, data, length);

		struct sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		sent = sendto(fd, packet, size, 0, (struct sockaddr*)&to, sizeof(to));
	}
	int error = errno;
	close(fd);
	errno = error;
	return sent == (ssize_t)size ? 0 : -1;
}


/*!	A fragment: \a length bytes of the datagram from \a from, sent as if
	they were at fragment offset \a offset, with the more fragments flag
	\a more.
*/
struct piece {
	size_t	from;
	size_t	length;
	size_t	offset;
	int		more;
};


enum {
	kDeliver = 0,
	kDrop = 1,
	kEither = 2
		// either is conformant; reported, not counted
};


/*!	Sends a UDP datagram of \a length bytes as \a pieces, and checks that it is
	delivered intact (kDeliver), that it is not delivered at all (kDrop), or
	only reports which happened (kEither).
*/
static void
crafted(int family, const char* label, size_t length,
	const struct piece* pieces, int count, int expect)
{
	char name[96], detail[160];
	snprintf(name, sizeof(name), "%s %s %s", family_name(family),
		expect == kDrop ? "drop" : (expect == kDeliver ? "deliver" : "either"),
		label);

	unsigned short port = sPort++;
	int receiver = bind_receiver(family, port);
	if (receiver < 0) {
		snprintf(detail, sizeof(detail), "bind: %s", strerror(errno));
		result(name, 0, detail);
		return;
	}

	unsigned char* udp = make_udp(family, port, length);
	uint32_t ident = ++sIdent;
	for (int i = 0; i < count; i++) {
		if (send_fragment(family, udp + pieces[i].from, pieces[i].length,
				pieces[i].offset, pieces[i].more, ident) != 0) {
			snprintf(detail, sizeof(detail), "raw send of fragment %d: %s",
				i, strerror(errno));
			result(name, 0, detail);
			free(udp);
			close(receiver);
			return;
		}
	}

	unsigned char in[4096];
	ssize_t received = recv(receiver, in, sizeof(in), 0);
	int intact = received == (ssize_t)(length - 8)
		&& memcmp(in, udp + 8, length - 8) == 0;
	if (expect == kDrop) {
		if (received < 0)
			snprintf(detail, sizeof(detail), "not delivered, as required");
		else {
			snprintf(detail, sizeof(detail), "DELIVERED %zd bytes%s, but "
				"must be dropped", received, intact ? " (the joined pieces)"
					: "");
		}
		result(name, received < 0, detail);
	} else if (expect == kDeliver) {
		snprintf(detail, sizeof(detail), "received %zd of %zu%s", received,
			length - 8, intact ? ", intact"
				: (received < 0 ? " (NOT DELIVERED within 1 s)" : ", WRONG"));
		result(name, intact, detail);
	} else {
		printf("%-50s INFO  %s\n", name, received < 0 ? "not delivered"
			: (intact ? "delivered intact" : "delivered, WRONG CONTENT"));
		fflush(stdout);
		if (received >= 0 && !intact)
			sFailures++;
	}

	free(udp);
	close(receiver);
}


/*!	One fragment whose header says it is 60 bytes long but whose total length
	is only 20, followed by the last fragment that would complete it if the
	fragment's length, total length minus header length, wrapped around to
	65496. Must not be delivered, and must not upset the receiver.
	(Linux rewrites the total length of an IP_HDRINCL packet, so on Linux
	this row only shows the second fragment being dropped.)
*/
static void
short_total_length_row(void)
{
	const char* name = "v4 drop total length shorter than the header";
	char detail[160];
	unsigned short port = sPort++;
	int receiver = bind_receiver(AF_INET, port);
	if (receiver < 0) {
		snprintf(detail, sizeof(detail), "bind: %s", strerror(errno));
		result(name, 0, detail);
		return;
	}
	uint32_t ident = ++sIdent;

	unsigned char packet[68];
	memset(packet, 0, sizeof(packet));
	packet[0] = 0x4f;
		// header length 60: 40 bytes of zero options (end of list)
	packet[2] = 0; packet[3] = 20;
	packet[4] = (ident >> 8) & 0xff; packet[5] = ident & 0xff;
	packet[6] = 0x20;
		// more fragments, offset 0
	packet[8] = 64;
	packet[9] = IPPROTO_UDP;
	packet[12] = 127; packet[15] = 1;
	packet[16] = 127; packet[19] = 1;
	uint16_t checksum = ip_checksum(packet, 60);
	packet[10] = checksum >> 8; packet[11] = checksum & 0xff;
	unsigned char* udp = make_udp(AF_INET, port, 8);
	memcpy(packet + 60, udp, 8);

	int fd = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
	int on = 1;
	int status = -1;
	if (fd >= 0 && setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &on, sizeof(on))
			== 0) {
		struct sockaddr_in to;
		memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		status = sendto(fd, packet, sizeof(packet), 0, (struct sockaddr*)&to,
			sizeof(to)) == (ssize_t)sizeof(packet) ? 0 : -1;
	}
	int error = errno;
	if (fd >= 0)
		close(fd);
	if (status == 0) {
		unsigned char tail[8];
		fill(tail, sizeof(tail), 7);
		status = send_fragment(AF_INET, tail, sizeof(tail), 65496, 0, ident);
		error = errno;
	}
	if (status != 0) {
		snprintf(detail, sizeof(detail), "raw send: %s", strerror(error));
		result(name, 0, detail);
	} else {
		unsigned char in[4096];
		ssize_t received = recv(receiver, in, sizeof(in), 0);
		snprintf(detail, sizeof(detail), "%s", received < 0
			? "not delivered, as required" : "DELIVERED, but must be dropped");
		result(name, received < 0, detail);
	}
	free(udp);
	close(receiver);
}


static void
crafted_rows(int family)
{
	// 3000 bytes of UDP, header included, in fragments of 1000 bytes
	const size_t kLength = 3000;
	const struct piece inOrder[] = { { 0, 1000, 0, 1 }, { 1000, 1000, 1000, 1 },
		{ 2000, 1000, 2000, 0 } };
	const struct piece reverse[] = { { 2000, 1000, 2000, 0 },
		{ 1000, 1000, 1000, 1 }, { 0, 1000, 0, 1 } };
	const struct piece middleFirst[] = { { 1000, 1000, 1000, 1 },
		{ 0, 1000, 0, 1 }, { 2000, 1000, 2000, 0 } };
	const struct piece duplicate[] = { { 0, 1000, 0, 1 },
		{ 1000, 1000, 1000, 1 }, { 1000, 1000, 1000, 1 },
		{ 2000, 1000, 2000, 0 } };
	const struct piece lastFirst[] = { { 2000, 1000, 2000, 0 },
		{ 0, 1000, 0, 1 }, { 1000, 1000, 1000, 1 } };
	const struct piece gap[] = { { 0, 1000, 0, 1 }, { 2000, 1000, 2000, 0 } };

	crafted(family, "in order", kLength, inOrder, 3, 0);
	crafted(family, "reversed", kLength, reverse, 3, 0);
	crafted(family, "middle first", kLength, middleFirst, 3, 0);
	crafted(family, "duplicate middle", kLength, duplicate, 4, 0);
	crafted(family, "last fragment first", kLength, lastFirst, 3, 0);
	crafted(family, "gap (middle missing)", kLength, gap, 2, 1);

	// 24 bytes of UDP; the three 8-byte pieces joined back to back are the
	// valid datagram
	const size_t kShort = 24;
	const struct piece past[] = { { 0, 8, 0, 1 }, { 8, 8, 16, 0 },
		{ 16, 8, 24, 1 } };
	const struct piece shortLast[] = { { 0, 8, 0, 1 }, { 16, 8, 24, 1 },
		{ 8, 8, 16, 0 } };
	const struct piece twoEnds[] = { { 0, 8, 0, 1 }, { 16, 8, 16, 0 },
		{ 8, 8, 8, 0 } };
	crafted(family, "past: data past the last fragment", kShort, past, 3, 1);
	crafted(family, "short: last ends before received data", kShort,
		shortLast, 3, 1);
	crafted(family, "twoends: second last fragment, other end", kShort,
		twoEnds, 3, 1);

	// A last fragment that is a duplicate: exactly, before the datagram is
	// complete; and wholly inside data already held, which must not make the
	// datagram look complete (it is dropped, or the datagram abandoned).
	const struct piece lastTwice[] = { { 0, 1000, 0, 1 },
		{ 2000, 1000, 2000, 0 }, { 2000, 1000, 2000, 0 },
		{ 1000, 1000, 1000, 1 } };
	const struct piece lastCovered[] = { { 0, 1008, 0, 1 },
		{ 1000, 8, 1000, 0 } };
	crafted(family, "last fragment twice, then the middle", kLength,
		lastTwice, 4, kDeliver);
	crafted(family, "last fragment inside held data", 1008, lastCovered, 2,
		kDrop);

	// A last fragment that partly overlaps held data, with the same bytes.
	// RFC 5722 requires IPv6 to abandon the datagram; RFC 791 lets IPv4 keep
	// either copy.
	const struct piece lastOverlaps[] = { { 0, 1008, 0, 1 },
		{ 1000, 16, 1000, 0 } };
	crafted(family, "last fragment overlapping held data", 1016, lastOverlaps,
		2, family == AF_INET6 ? kDrop : kEither);

	if (family == AF_INET)
		short_total_length_row();
}


struct timed {
	int				family;
	const char*		label;
	int				expectDrop;
	unsigned short	port;
	int				receiver;
	unsigned char*	udp;
	uint32_t		ident;
	int				sendError;
};


static void
timed_send(struct timed* row, size_t from, size_t length, int more)
{
	if (row->sendError == 0 && send_fragment(row->family, row->udp + from,
			length, from, more, row->ident) != 0)
		row->sendError = errno;
}


static void
timed_check(struct timed* row)
{
	char name[96], detail[160];
	snprintf(name, sizeof(name), "%s timeout %s", family_name(row->family),
		row->label);
	if (row->sendError != 0) {
		snprintf(detail, sizeof(detail), "raw send: %s",
			strerror(row->sendError));
		result(name, 0, detail);
		return;
	}

	unsigned char in[4096];
	ssize_t received = recv(row->receiver, in, sizeof(in), 0);
	int intact = received == 3000 - 8
		&& memcmp(in, row->udp + 8, 3000 - 8) == 0;
	if (row->expectDrop) {
		snprintf(detail, sizeof(detail), "%s", received < 0
			? "not delivered, as required"
			: "DELIVERED, but must have timed out 60 s after the first "
				"fragment");
		result(name, received < 0, detail);
	} else {
		snprintf(detail, sizeof(detail), "received %zd of 2992%s", received,
			intact ? ", intact" : (received < 0 ? " (NOT DELIVERED)"
				: ", WRONG"));
		result(name, intact, detail);
	}
}


static void
sleep_until(time_t start, int seconds)
{
	while (time(NULL) < start + seconds)
		sleep(1);
	printf("  t=%ds\n", (int)(time(NULL) - start));
	fflush(stdout);
}


/*!	Both families at once on one timeline:
	within:    [0,1000) at 0 s, [1000,3000) at 50 s
	refreshed: [0,1000) at 0 s, [1000,2000) at 40 s, [2000,3000) at 70 s
*/
static void
timeout_rows(void)
{
	struct timed rows[4];
	int count = 0;
	for (int f = 0; f < 2; f++) {
		for (int r = 0; r < 2; r++) {
			struct timed* row = &rows[count++];
			row->family = f == 0 ? AF_INET : AF_INET6;
			row->label = r == 0 ? "within: last fragment at 50 s"
				: "refreshed: at 0, 40 and 70 s";
			row->expectDrop = r;
			row->port = sPort++;
			row->receiver = bind_receiver(row->family, row->port);
			row->udp = make_udp(row->family, row->port, 3000);
			row->ident = ++sIdent;
			row->sendError = row->receiver < 0 ? errno : 0;
		}
	}

	time_t start = time(NULL);
	for (int i = 0; i < count; i++)
		timed_send(&rows[i], 0, 1000, 1);
	printf("  t=0s first fragments sent\n");
	sleep_until(start, 40);
	for (int i = 0; i < count; i++) {
		if (rows[i].expectDrop)
			timed_send(&rows[i], 1000, 1000, 1);
	}
	sleep_until(start, 50);
	for (int i = 0; i < count; i++) {
		if (!rows[i].expectDrop) {
			timed_send(&rows[i], 1000, 1000, 1);
			timed_send(&rows[i], 2000, 1000, 0);
		}
	}
	for (int i = 0; i < count; i++) {
		if (!rows[i].expectDrop)
			timed_check(&rows[i]);
	}
	sleep_until(start, 70);
	for (int i = 0; i < count; i++) {
		if (rows[i].expectDrop)
			timed_send(&rows[i], 2000, 1000, 0);
	}
	for (int i = 0; i < count; i++) {
		if (rows[i].expectDrop)
			timed_check(&rows[i]);
	}

	for (int i = 0; i < count; i++) {
		free(rows[i].udp);
		if (rows[i].receiver >= 0)
			close(rows[i].receiver);
	}
}


int
main(int argc, char** argv)
{
	int sendRows = 0, craftedRows = 0, timeoutRows = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0)
			sendRows = 1;
		else if (strcmp(argv[i], "-c") == 0)
			craftedRows = 1;
		else if (strcmp(argv[i], "-t") == 0)
			timeoutRows = 1;
		else {
			fprintf(stderr, "usage: %s [-s] [-c] [-t]\n", argv[0]);
			return 2;
		}
	}
	if (!sendRows && !craftedRows && !timeoutRows)
		sendRows = craftedRows = 1;

	srandom((unsigned)time(NULL) ^ (unsigned)getpid());
	sPort = 41000 + (unsigned short)(random() % 4000);
	sIdent = (uint32_t)random();

	if (!check_alive("start"))
		return 100 + sFailures;

	if (sendRows) {
		static const size_t kSizes[] = { 100, 1200, 3000, 9000, 20000, 65507,
			65527 };
		for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
			for (int f = 0; f < 2; f++) {
				int family = f == 0 ? AF_INET : AF_INET6;
				if (family == AF_INET && kSizes[i] > 65507)
					continue;
					// larger than an IPv4 datagram can carry
				char name[80], detail[128];
				snprintf(name, sizeof(name), "%s send udp %6zu bytes",
					family_name(family), kSizes[i]);
				result(name, udp_round_trip(family, kSizes[i], detail,
					sizeof(detail)), detail);
			}
		}
		if (!check_alive("send rows"))
			return 100 + sFailures;
	}

	if (craftedRows) {
		crafted_rows(AF_INET);
		crafted_rows(AF_INET6);
		if (!check_alive("crafted rows"))
			return 100 + sFailures;
	}

	if (timeoutRows) {
		timeout_rows();
		if (!check_alive("timeout rows"))
			return 100 + sFailures;
	}

	printf("FAILURES %d\n", sFailures);
	return sFailures;
}
