/*
 * udp947.c -- issue 947: the control messages recvmsg() returns for a UDP
 * datagram are exactly the ones the application asked for with
 * IP_RECVDSTADDR (IP_PKTINFO on Linux), IPV6_RECVPKTINFO and
 * IPV6_RECVHOPLIMIT -- no more, no fewer -- and they carry the datagram's
 * real destination address and hop limit.
 *
 * Covered, for each address setup (IPv4 on 127.0.0.1, IPv6 on ::1, and an
 * IPv4 sender to a dual-stack :: socket) and each receive option state (none
 * requested; each option on its own; both IPv6 options; an option set and
 * then cleared again):
 *   - recvmsg() with a 256-byte control buffer returns the expected set of
 *     control messages and nothing else, and no MSG_CTRUNC
 *   - the destination address and the hop limit in them are the real ones
 *     (the sender sets IP_TTL / IPV6_UNICAST_HOPS to 7, so a default 64 is
 *     told apart from the real value)
 *   - an IPv4 datagram on a dual-stack socket gets IPV6_PKTINFO, with the
 *     IPv4-mapped destination, but no IPV6_HOPLIMIT: an IPv4 header has no
 *     hop limit, and Linux, FreeBSD and macOS all send none
 *   - getsockopt() reads back what setsockopt() set
 * And once per setup, with no option requested:
 *   - recvmsg() with a control buffer too small for any control message
 *     still returns the datagram, with an empty control buffer
 * With -t, instead: a control message that was requested but does not fit
 * in the control buffer truncates it (MSG_CTRUNC), and neither the datagram
 * nor the next one is lost.
 *
 * Exit status: the number of FAILs. Plain POSIX plus the per-system name of
 * the IPv4 destination address option: builds and runs on Linux, which is the
 * reference.
 */

#define _GNU_SOURCE
	// struct in6_pktinfo on Linux
#define __APPLE_USE_RFC_3542
	// and on macOS, where only a smoke-compile runs

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


#ifdef IP_RECVDSTADDR
	// Haiku, FreeBSD: the option and the control message share one name
#	define DSTADDR_OPTION	IP_RECVDSTADDR
#	define DSTADDR_CMSG		IP_RECVDSTADDR
#	define DSTADDR_NAME		"IP_RECVDSTADDR"
#else
	// Linux: IP_PKTINFO carries the destination address in ipi_addr
#	define DSTADDR_OPTION	IP_PKTINFO
#	define DSTADDR_CMSG		IP_PKTINFO
#	define DSTADDR_NAME		"IP_PKTINFO"
#endif

static const int kHopLimit = 7;

static int sFailed = 0;
static int sPassed = 0;


static void
report(const char* what, int ok, const char* detail)
{
	printf("%-58s %s  %s\n", what, ok ? "PASS" : "FAIL", detail);
	if (ok)
		sPassed++;
	else
		sFailed++;
}


enum {
	SETUP_IPV4,
	SETUP_IPV6,
	SETUP_V4_TO_DUAL
};

static const char* const kSetupNames[] = { "ipv4", "ipv6", "v4-to-dual" };


enum {
	WANT_DSTADDR	= 0x01,
	WANT_PKTINFO	= 0x02,
	WANT_HOPLIMIT	= 0x04
};

struct option_case {
	const char*	name;
	int			set;		// options switched on
	int			cleared;	// options switched on and then off again
};

static const struct option_case kIPv4Cases[] = {
	{ "none", 0, 0 },
	{ DSTADDR_NAME, WANT_DSTADDR, 0 },
	{ DSTADDR_NAME " set+cleared", 0, WANT_DSTADDR },
};

static const struct option_case kIPv6Cases[] = {
	{ "none", 0, 0 },
	{ "IPV6_RECVPKTINFO", WANT_PKTINFO, 0 },
	{ "IPV6_RECVHOPLIMIT", WANT_HOPLIMIT, 0 },
	{ "IPV6_RECVPKTINFO+IPV6_RECVHOPLIMIT", WANT_PKTINFO | WANT_HOPLIMIT, 0 },
	{ "IPV6_RECVPKTINFO set+cleared", 0, WANT_PKTINFO },
	{ "both set+cleared", 0, WANT_PKTINFO | WANT_HOPLIMIT },
};


static int
option_of(int want, int* level)
{
	switch (want) {
		case WANT_DSTADDR:
			*level = IPPROTO_IP;
			return DSTADDR_OPTION;
		case WANT_PKTINFO:
			*level = IPPROTO_IPV6;
			return IPV6_RECVPKTINFO;
		default:
			*level = IPPROTO_IPV6;
			return IPV6_RECVHOPLIMIT;
	}
}


/*!	Switches \a want on (\a on 1) or off on \a fd and reads it back. Returns
	0 when both the set and the read-back did what they should.
*/
static int
set_option(int fd, int want, int on, char* detail, size_t detailSize)
{
	int level;
	int option = option_of(want, &level);
	if (setsockopt(fd, level, option, &on, sizeof(on)) != 0) {
		snprintf(detail, detailSize, "setsockopt(%d, %d)=%s", level, option,
			strerror(errno));
		return -1;
	}
	int value = -1;
	socklen_t length = sizeof(value);
	if (getsockopt(fd, level, option, &value, &length) != 0) {
		snprintf(detail, detailSize, "getsockopt(%d, %d)=%s", level, option,
			strerror(errno));
		return -1;
	}
	if ((value != 0) != (on != 0)) {
		snprintf(detail, detailSize, "getsockopt(%d, %d) read %d after %d",
			level, option, value, on);
		return -1;
	}
	return 0;
}


/*!	Opens the receiving and the sending socket of \a setup, the sender
	connected to the receiver, and sets the sender's hop limit to kHopLimit.
*/
static int
open_pair(int setup, int* _receiver, int* _sender)
{
	struct sockaddr_storage address;
	memset(&address, 0, sizeof(address));
	socklen_t length;

	int receiverFamily = setup == SETUP_IPV4 ? AF_INET : AF_INET6;
	int receiver = socket(receiverFamily, SOCK_DGRAM, 0);
	if (receiver < 0)
		return -1;

	if (setup == SETUP_IPV4) {
		struct sockaddr_in* in = (struct sockaddr_in*)&address;
		in->sin_family = AF_INET;
		in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		length = sizeof(*in);
	} else {
		struct sockaddr_in6* in6 = (struct sockaddr_in6*)&address;
		in6->sin6_family = AF_INET6;
		if (setup == SETUP_IPV6)
			in6->sin6_addr = in6addr_loopback;
		else {
			int off = 0;
			if (setsockopt(receiver, IPPROTO_IPV6, IPV6_V6ONLY, &off,
					sizeof(off)) != 0) {
				close(receiver);
				return -1;
			}
			in6->sin6_addr = in6addr_any;
		}
		length = sizeof(*in6);
	}
	if (bind(receiver, (struct sockaddr*)&address, length) != 0
		|| getsockname(receiver, (struct sockaddr*)&address, &length) != 0) {
		close(receiver);
		return -1;
	}

	int sender;
	if (setup == SETUP_V4_TO_DUAL) {
		uint16_t port = ((struct sockaddr_in6*)&address)->sin6_port;
		struct sockaddr_in* in = (struct sockaddr_in*)&address;
		memset(&address, 0, sizeof(address));
		in->sin_family = AF_INET;
		in->sin_port = port;
		in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		length = sizeof(*in);
	}
	sender = socket(address.ss_family, SOCK_DGRAM, 0);
	int hops = kHopLimit;
	int status;
	if (address.ss_family == AF_INET)
		status = setsockopt(sender, IPPROTO_IP, IP_TTL, &hops, sizeof(hops));
	else {
		status = setsockopt(sender, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops,
			sizeof(hops));
	}
	if (sender < 0 || status != 0
		|| connect(sender, (struct sockaddr*)&address, length) != 0) {
		close(receiver);
		if (sender >= 0)
			close(sender);
		return -1;
	}

	struct timeval timeout = { 5, 0 };
	setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	*_receiver = receiver;
	*_sender = sender;
	return 0;
}


/*!	Appends a description of every control message in \a message to
	\a detail, and returns the set of WANT_* flags that were found with the
	right content. A control message with the wrong content, or one that is
	not one of the three, is returned in \a _bad.
*/
static int
parse_control(int setup, struct msghdr* message, char* detail,
	size_t detailSize, int* _bad)
{
	int found = 0;
	int bad = 0;
	size_t used = strlen(detail);

	for (struct cmsghdr* header = CMSG_FIRSTHDR(message); header != NULL;
			header = CMSG_NXTHDR(message, header)) {
		char text[96];
		char address[INET6_ADDRSTRLEN];
		int kind = 0;
		int ok = 0;

		if (header->cmsg_level == IPPROTO_IP
			&& header->cmsg_type == DSTADDR_CMSG) {
			struct in_addr destination;
#ifdef IP_RECVDSTADDR
			memcpy(&destination, CMSG_DATA(header), sizeof(destination));
#else
			struct in_pktinfo info;
			memcpy(&info, CMSG_DATA(header), sizeof(info));
			destination = info.ipi_addr;
#endif
			inet_ntop(AF_INET, &destination, address, sizeof(address));
			kind = WANT_DSTADDR;
			ok = destination.s_addr == htonl(INADDR_LOOPBACK);
			snprintf(text, sizeof(text), " [" DSTADDR_NAME " %s]", address);
		} else if (header->cmsg_level == IPPROTO_IPV6
			&& header->cmsg_type == IPV6_PKTINFO) {
			struct in6_pktinfo info;
			memcpy(&info, CMSG_DATA(header), sizeof(info));
			inet_ntop(AF_INET6, &info.ipi6_addr, address, sizeof(address));
			kind = WANT_PKTINFO;
			ok = strcmp(address,
				setup == SETUP_IPV6 ? "::1" : "::ffff:127.0.0.1") == 0
				&& info.ipi6_ifindex != 0;
			snprintf(text, sizeof(text), " [IPV6_PKTINFO %s if %u]", address,
				info.ipi6_ifindex);
		} else if (header->cmsg_level == IPPROTO_IPV6
			&& header->cmsg_type == IPV6_HOPLIMIT) {
			int hopLimit;
			memcpy(&hopLimit, CMSG_DATA(header), sizeof(hopLimit));
			kind = WANT_HOPLIMIT;
			ok = hopLimit == kHopLimit;
			snprintf(text, sizeof(text), " [IPV6_HOPLIMIT %d]", hopLimit);
		} else {
			snprintf(text, sizeof(text), " [level %d type %d len %u]",
				header->cmsg_level, header->cmsg_type,
				(unsigned)header->cmsg_len);
		}

		if (kind != 0 && ok && (found & kind) == 0)
			found |= kind;
		else
			bad = 1;

		if (used < detailSize) {
			snprintf(detail + used, detailSize - used, "%s", text);
			used = strlen(detail);
		}
	}

	*_bad = bad;
	return found;
}


static void
test_options(int setup, const struct option_case* option)
{
	char what[96];
	char detail[256];
	detail[0] = '\0';
	snprintf(what, sizeof(what), "%s %s", kSetupNames[setup], option->name);

	int receiver, sender;
	if (open_pair(setup, &receiver, &sender) != 0) {
		report(what, 0, "could not open the sockets");
		return;
	}

	for (int want = WANT_DSTADDR; want <= WANT_HOPLIMIT; want <<= 1) {
		int status = 0;
		if ((option->set & want) != 0)
			status = set_option(receiver, want, 1, detail, sizeof(detail));
		else if ((option->cleared & want) != 0) {
			status = set_option(receiver, want, 1, detail, sizeof(detail));
			if (status == 0)
				status = set_option(receiver, want, 0, detail, sizeof(detail));
		}
		if (status != 0) {
			report(what, 0, detail);
			close(receiver);
			close(sender);
			return;
		}
	}

	if (send(sender, "947", 3, 0) != 3) {
		snprintf(detail, sizeof(detail), "send: %s", strerror(errno));
		report(what, 0, detail);
		close(receiver);
		close(sender);
		return;
	}

	char data[16];
	struct iovec vector = { data, sizeof(data) };
	union {
		char buffer[256];
		struct cmsghdr align;
	} control;
	memset(&control, 0xa5, sizeof(control));
	struct msghdr message;
	memset(&message, 0, sizeof(message));
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.buffer;
	message.msg_controllen = sizeof(control.buffer);

	ssize_t received = recvmsg(receiver, &message, 0);
	if (received != 3) {
		snprintf(detail, sizeof(detail), "recvmsg=%zd (%s)", received,
			received < 0 ? strerror(errno) : "short");
		report(what, 0, detail);
		close(receiver);
		close(sender);
		return;
	}

	snprintf(detail, sizeof(detail), "controllen=%u flags=0x%x",
		(unsigned)message.msg_controllen, message.msg_flags);
	int bad;
	int found = parse_control(setup, &message, detail, sizeof(detail), &bad);
	int expected = option->set;
	if (setup == SETUP_V4_TO_DUAL)
		expected &= ~WANT_HOPLIMIT;
	report(what, !bad && found == expected
		&& (message.msg_flags & MSG_CTRUNC) == 0, detail);

	close(receiver);
	close(sender);
}


/*!	A control buffer too small for any control message. With no option
	requested (\a want 0) the datagram must be received with nothing in the
	control buffer. With \a want requested, the control message is cut off:
	the datagram is still received, MSG_CTRUNC is set, and the next datagram
	is still queued (POSIX; Linux, FreeBSD).
*/
static void
test_small_control(int setup, int want, const char* wantName)
{
	char what[96];
	char detail[256];
	detail[0] = '\0';
	snprintf(what, sizeof(what), "%s %s, 4-byte control buffer",
		kSetupNames[setup], wantName);

	int receiver, sender;
	if (open_pair(setup, &receiver, &sender) != 0) {
		report(what, 0, "could not open the sockets");
		return;
	}
	if (want != 0 && set_option(receiver, want, 1, detail, sizeof(detail))
			!= 0) {
		report(what, 0, detail);
		close(receiver);
		close(sender);
		return;
	}
	if (send(sender, "947", 3, 0) != 3 || send(sender, "2nd", 3, 0) != 3) {
		snprintf(detail, sizeof(detail), "send: %s", strerror(errno));
		report(what, 0, detail);
		close(receiver);
		close(sender);
		return;
	}

	char data[16];
	memset(data, 0, sizeof(data));
	struct iovec vector = { data, sizeof(data) };
	union {
		char buffer[4];
		int align;
	} control;
	struct msghdr message;
	memset(&message, 0, sizeof(message));
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = control.buffer;
	message.msg_controllen = sizeof(control.buffer);

	ssize_t received = recvmsg(receiver, &message, 0);
	int receiveError = errno;

	char next[16];
	memset(next, 0, sizeof(next));
	ssize_t nextReceived = recv(receiver, next, sizeof(next) - 1,
		MSG_DONTWAIT);

	snprintf(detail, sizeof(detail),
		"recvmsg=%zd%s%s data=\"%.3s\" controllen=%u flags=0x%x;"
		" next recv=%zd \"%s\"",
		received, received < 0 ? " " : "",
		received < 0 ? strerror(receiveError) : "",
		received == 3 ? data : "", (unsigned)message.msg_controllen,
		message.msg_flags, nextReceived, nextReceived > 0 ? next : "");
	int truncated = (message.msg_flags & MSG_CTRUNC) != 0;
	report(what, received == 3 && memcmp(data, "947", 3) == 0
		&& (want != 0 || message.msg_controllen == 0)
		&& truncated == (want != 0)
		&& nextReceived == 3 && memcmp(next, "2nd", 3) == 0, detail);

	close(receiver);
	close(sender);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc > 1 && strcmp(argv[1], "-t") == 0) {
		// a requested control message that does not fit
		test_small_control(SETUP_IPV4, WANT_DSTADDR, DSTADDR_NAME);
		test_small_control(SETUP_IPV6, WANT_PKTINFO, "IPV6_RECVPKTINFO");
		test_small_control(SETUP_V4_TO_DUAL, WANT_PKTINFO,
			"IPV6_RECVPKTINFO");
		printf("udp947 -t: %d passed, %d failed\n", sPassed, sFailed);
		return sFailed;
	}

	for (size_t i = 0; i < sizeof(kIPv4Cases) / sizeof(kIPv4Cases[0]); i++)
		test_options(SETUP_IPV4, &kIPv4Cases[i]);
	test_small_control(SETUP_IPV4, 0, "none");

	for (int setup = SETUP_IPV6; setup <= SETUP_V4_TO_DUAL; setup++) {
		for (size_t i = 0; i < sizeof(kIPv6Cases) / sizeof(kIPv6Cases[0]);
				i++) {
			test_options(setup, &kIPv6Cases[i]);
		}
		test_small_control(setup, 0, "none");
	}

	printf("udp947: %d passed, %d failed\n", sPassed, sFailed);
	return sFailed;
}
