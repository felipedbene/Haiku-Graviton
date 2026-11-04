/*
 * icmp6unreach1082 -- does a UDP datagram sent over IPv6 to a port nobody is
 * bound to come back as an ICMPv6 port unreachable, and does a connected
 * socket then see ECONNREFUSED? (issue 1082)
 *
 * A raw ICMPv6 socket stands in for the peer's packet capture: it receives a
 * copy of every ICMPv6 message this host takes in, starting at the ICMPv6
 * header, on Linux and on Haiku alike. Each probe sends from a socket of its
 * own and then checks the capture for an error that quotes that probe's ports.
 *
 * Rows:
 *  - a connected socket to [::1]:<unused>: recv() and poll()/SO_ERROR give
 *    ECONNREFUSED, and the error was a type 1 code 4 that quotes the probe;
 *  - the next send() on a connected socket that was refused fails with
 *    ECONNREFUSED;
 *  - [::]:53, the resolver's built-in IPv6 default;
 *  - a 4000-byte datagram: the error must fit the 1280-byte minimum MTU, so
 *    it quotes 1232 bytes of the invoking packet (RFC 4443, 2.4 (c));
 *  - optionally an address given on the command line (an ENA or link-local
 *    address of this host, "fe80::...%if" is accepted);
 *  - an ICMPv6 echo request to ::1 still gets its reply;
 *  - rate limit: 200 datagrams to an unused port back to back, and how many
 *    errors come back (informational, no pass/fail). Errors to this host
 *    itself are not rate-limited, on Linux or with the issue 1082 fix, so
 *    all 200 are expected; icmp6ratelimit1082.cpp checks the limiter itself.
 *
 * Build: cc -o icmp6unreach1082 icmp6unreach1082.c   (add -lnetwork on Haiku)
 * Usage: icmp6unreach1082 [extra-ipv6-address ...]
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


static int sCapture = -1;
static int sFailures = 0;


static double
now(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}


static int
parse_address(const char* text, unsigned short port, struct sockaddr_in6* out)
{
	struct addrinfo hints, *result;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET6;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICHOST;
	if (getaddrinfo(text, NULL, &hints, &result) != 0)
		return -1;
	memcpy(out, result->ai_addr, sizeof(*out));
	freeaddrinfo(result);
	out->sin6_port = port;
	return 0;
}


/* Returns a port on the address that nothing is bound to. */
static unsigned short
unused_port(const struct sockaddr_in6* on)
{
	struct sockaddr_in6 address = *on;
	address.sin6_port = 0;
	socklen_t length = sizeof(address);
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0
		|| getsockname(fd, (struct sockaddr*)&address, &length) != 0) {
		close(fd);
		return htons(33434);
	}
	close(fd);
	return address.sin6_port;
}


static void
drain_capture(void)
{
	char buffer[2048];
	while (recv(sCapture, buffer, sizeof(buffer), MSG_DONTWAIT) > 0)
		;
}


/*
 * Waits up to timeout seconds for an ICMPv6 error that quotes a UDP datagram
 * from localPort to remotePort. Returns the ICMPv6 message length, or -1.
 */
static int
capture_error(unsigned short localPort, unsigned short remotePort,
	double timeout, int* _type, int* _code, int* _innerNext)
{
	double end = now() + timeout;
	unsigned char buffer[2048];
	while (now() < end) {
		struct pollfd entry = { sCapture, POLLIN, 0 };
		int left = (int)((end - now()) * 1000);
		if (poll(&entry, 1, left > 0 ? left : 0) <= 0)
			break;
		ssize_t length = recv(sCapture, buffer, sizeof(buffer), 0);
		if (length < 8 + 40 + 8)
			continue;
		if ((buffer[0] & ICMP6_INFOMSG_MASK) != 0)
			continue;
		/* the quoted IPv6 header starts after the 8-byte ICMPv6 header */
		const unsigned char* inner = buffer + 8;
		const unsigned char* udp = inner + 40;
		unsigned short source, destination;
		memcpy(&source, udp, 2);
		memcpy(&destination, udp + 2, 2);
		if (inner[6] != IPPROTO_UDP || source != localPort
			|| destination != remotePort)
			continue;
		*_type = buffer[0];
		*_code = buffer[1];
		*_innerNext = inner[6];
		return (int)length;
	}
	return -1;
}


static void
row(const char* label, int pass, double elapsed, const char* detail)
{
	printf("%-44s %s  %.3fs  %s\n", label, pass ? "PASS" : "FAIL", elapsed,
		detail);
	if (!pass)
		sFailures++;
}


/* A connected socket to address: send one byte, then recv() or poll(). */
static void
probe_connected(const char* label, const struct sockaddr_in6* address,
	int usePoll, size_t size)
{
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	struct timeval timeout = { 3, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	char detail[256] = "";
	int refused = 0;
	drain_capture();
	double start = now();
	struct sockaddr_in6 local;
	socklen_t localLength = sizeof(local);
	char* payload = calloc(1, size);
	if (connect(fd, (const struct sockaddr*)address, sizeof(*address)) != 0) {
		snprintf(detail, sizeof(detail), "connect: %s", strerror(errno));
	} else if (getsockname(fd, (struct sockaddr*)&local, &localLength) != 0) {
		snprintf(detail, sizeof(detail), "getsockname: %s", strerror(errno));
	} else if (send(fd, payload, size, 0) != (ssize_t)size) {
		refused = errno == ECONNREFUSED;
		snprintf(detail, sizeof(detail), "send: %s", strerror(errno));
	} else if (usePoll) {
		struct pollfd entry = { fd, POLLIN, 0 };
		int result = poll(&entry, 1, 3000);
		int error = 0;
		socklen_t errorLength = sizeof(error);
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLength);
		refused = error == ECONNREFUSED;
		snprintf(detail, sizeof(detail), "poll=%d revents=0x%x SO_ERROR=%s",
			result, entry.revents, error == 0 ? "0" : strerror(error));
	} else {
		char buffer[16];
		ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
		refused = received < 0 && errno == ECONNREFUSED;
		snprintf(detail, sizeof(detail), "recv=%zd %s", received,
			received < 0 ? strerror(errno) : "");
	}
	double elapsed = now() - start;

	int type = -1, code = -1, next = -1;
	int length = capture_error(local.sin6_port, address->sin6_port, 0.5,
		&type, &code, &next);
	size_t used = strlen(detail);
	if (length < 0) {
		snprintf(detail + used, sizeof(detail) - used, " | capture: none");
	} else {
		snprintf(detail + used, sizeof(detail) - used,
			" | capture: icmp6 type %d code %d len %d", type, code, length);
	}
	int captured = length >= 0 && type == ICMP6_DST_UNREACH
		&& code == ICMP6_DST_UNREACH_NOPORT;
	if (size > 1232) {
		/* 8 bytes of ICMPv6 header + 1232 quoted: 1280 with the IPv6 one */
		captured = captured && length == 1240;
	}
	row(label, refused && captured, elapsed, detail);
	free(payload);
	close(fd);
}


/* Two sends on a connected socket: the second one reports the refusal. */
static void
probe_second_send(const char* label, const struct sockaddr_in6* address)
{
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	char detail[160] = "";
	int refused = 0;
	double start = now();
	if (connect(fd, (const struct sockaddr*)address, sizeof(*address)) != 0)
		snprintf(detail, sizeof(detail), "connect: %s", strerror(errno));
	else if (send(fd, "x", 1, 0) != 1)
		snprintf(detail, sizeof(detail), "send 1: %s", strerror(errno));
	else {
		usleep(200000);
		ssize_t sent = send(fd, "x", 1, 0);
		refused = sent < 0 && errno == ECONNREFUSED;
		snprintf(detail, sizeof(detail), "send 2 = %zd %s", sent,
			sent < 0 ? strerror(errno) : "");
	}
	row(label, refused, now() - start, detail);
	close(fd);
}


/* An echo request to ::1 over a raw socket gets its echo reply. */
static void
probe_echo(void)
{
	int fd = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	struct sockaddr_in6 address;
	parse_address("::1", 0, &address);

	struct icmp6_hdr request;
	memset(&request, 0, sizeof(request));
	request.icmp6_type = ICMP6_ECHO_REQUEST;
	request.icmp6_id = htons(1082);
	request.icmp6_seq = htons(1);

	char detail[96] = "no reply";
	int replied = 0;
	double start = now();
	if (sendto(fd, &request, sizeof(request), 0, (struct sockaddr*)&address,
			sizeof(address)) != sizeof(request)) {
		snprintf(detail, sizeof(detail), "sendto: %s", strerror(errno));
	} else {
		while (!replied && now() - start < 2.0) {
			struct pollfd entry = { fd, POLLIN, 0 };
			if (poll(&entry, 1, 2000) <= 0)
				break;
			struct icmp6_hdr reply;
			if (recv(fd, &reply, sizeof(reply), 0) < (ssize_t)sizeof(reply))
				continue;
			if (reply.icmp6_type == ICMP6_ECHO_REPLY
				&& reply.icmp6_id == htons(1082)) {
				replied = 1;
				snprintf(detail, sizeof(detail), "echo reply seq %u",
					ntohs(reply.icmp6_seq));
			}
		}
	}
	row("icmp6 echo request to ::1", replied, now() - start, detail);
	close(fd);
}


/* Back-to-back datagrams from one unconnected socket; count the errors. */
static void
probe_rate(const struct sockaddr_in6* address, int count)
{
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	struct sockaddr_in6 local;
	memset(&local, 0, sizeof(local));
	local.sin6_family = AF_INET6;
	socklen_t localLength = sizeof(local);
	bind(fd, (struct sockaddr*)&local, sizeof(local));
	getsockname(fd, (struct sockaddr*)&local, &localLength);

	drain_capture();
	double start = now();
	int sent = 0;
	for (int i = 0; i < count; i++) {
		if (sendto(fd, "x", 1, 0, (const struct sockaddr*)address,
				sizeof(*address)) == 1)
			sent++;
	}
	double sendTime = now() - start;

	int errors = 0, type, code, next;
	while (capture_error(local.sin6_port, address->sin6_port, 1.0, &type,
			&code, &next) >= 0)
		errors++;
	printf("%-44s INFO  sent %d in %.3fs, %d errors back\n",
		"rate limit: 200 back-to-back to <unused>", sent, sendTime, errors);
	close(fd);
}


int
main(int argc, char** argv)
{
	sCapture = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (sCapture < 0) {
		printf("raw ICMPv6 capture socket: %s (run as root)\n",
			strerror(errno));
		return 2;
	}
	int bufferSize = 1 << 20;
	setsockopt(sCapture, SOL_SOCKET, SO_RCVBUF, &bufferSize,
		sizeof(bufferSize));

	struct sockaddr_in6 loopback, any;
	parse_address("::1", 0, &loopback);
	loopback.sin6_port = unused_port(&loopback);
	parse_address("::", htons(53), &any);

	probe_connected("udp [::1]:<unused> recv", &loopback, 0, 1);
	probe_connected("udp [::1]:<unused> poll", &loopback, 1, 1);
	probe_second_send("udp [::1]:<unused> second send()", &loopback);
	probe_connected("udp [::]:53 recv (resolver default)", &any, 0, 1);
	probe_connected("udp [::1]:<unused> 4000 B, quote <= 1280", &loopback,
		0, 4000);

	for (int i = 1; i < argc; i++) {
		struct sockaddr_in6 extra;
		if (parse_address(argv[i], 0, &extra) != 0) {
			printf("bad address %s\n", argv[i]);
			sFailures++;
			continue;
		}
		extra.sin6_port = unused_port(&extra);
		char label[96];
		snprintf(label, sizeof(label), "udp [%s]:<unused> recv", argv[i]);
		probe_connected(label, &extra, 0, 1);
	}

	probe_echo();
	probe_rate(&loopback, 200);

	printf("%s: %d failure(s)\n", sFailures == 0 ? "PASS" : "FAIL", sFailures);
	return sFailures == 0 ? 0 : 1;
}
