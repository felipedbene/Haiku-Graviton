/*
 * udprefused993 -- does a UDP datagram sent to a loopback port that nobody is
 * bound to come back as ECONNREFUSED?
 *
 * That is how the stub resolver gives up on a name server that is not there.
 * With no resolv.conf, netresolv's res_init() falls back to two servers,
 * 0.0.0.0:53 and [::]:53, and res_send() connect()s its UDP socket to each.
 * On Linux the ICMP port unreachable from the loopback makes the next recv()
 * fail with ECONNREFUSED at once, so the lookup fails in microseconds. Where
 * the error never arrives, every query waits out its RES_TIMEOUT share on each
 * retry (issue 993: a lookup in haikuporter's chroot, which has no
 * settings/network/, takes 7 s, and "localhost" 28 s).
 *
 * Rows: a connected UDP socket to 127.0.0.1:<unused>, ::1:<unused> and the
 * resolver's two built-in defaults; send() one byte, then recv() with a 3 s
 * SO_RCVTIMEO, or poll() for 3 s.
 *
 * Build: cc -o udprefused993 udprefused993.c   (add -lnetwork on Haiku)
 */

#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


static double
now(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}


/* Returns a port on the given loopback address that nothing is bound to. */
static unsigned short
unused_port(int af)
{
	int fd = socket(af, SOCK_DGRAM, 0);
	struct sockaddr_storage address;
	memset(&address, 0, sizeof(address));
	socklen_t length;
	if (af == AF_INET) {
		((struct sockaddr_in*)&address)->sin_family = AF_INET;
		((struct sockaddr_in*)&address)->sin_addr.s_addr
			= htonl(INADDR_LOOPBACK);
		length = sizeof(struct sockaddr_in);
	} else {
		((struct sockaddr_in6*)&address)->sin6_family = AF_INET6;
		((struct sockaddr_in6*)&address)->sin6_addr = in6addr_loopback;
		length = sizeof(struct sockaddr_in6);
	}
	bind(fd, (struct sockaddr*)&address, length);
	getsockname(fd, (struct sockaddr*)&address, &length);
	close(fd);
	return af == AF_INET ? ((struct sockaddr_in*)&address)->sin_port
		: ((struct sockaddr_in6*)&address)->sin6_port;
}


static int
probe(const char* label, int af, const char* host, unsigned short port,
	int usePoll)
{
	struct sockaddr_storage address;
	memset(&address, 0, sizeof(address));
	socklen_t length;
	if (af == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)&address;
		in->sin_family = AF_INET;
		in->sin_port = port;
		inet_pton(AF_INET, host, &in->sin_addr);
		length = sizeof(*in);
	} else {
		struct sockaddr_in6* in6 = (struct sockaddr_in6*)&address;
		in6->sin6_family = AF_INET6;
		in6->sin6_port = port;
		inet_pton(AF_INET6, host, &in6->sin6_addr);
		length = sizeof(*in6);
	}

	int fd = socket(af, SOCK_DGRAM, 0);
	struct timeval timeout = { 3, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	char detail[160];
	int refused = 0;
	double start = now();
	if (connect(fd, (struct sockaddr*)&address, length) != 0) {
		snprintf(detail, sizeof(detail), "connect: %s", strerror(errno));
	} else if (send(fd, "x", 1, 0) != 1) {
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
	close(fd);

	printf("%-34s %s  %.3fs  %s\n", label, refused ? "PASS" : "FAIL",
		elapsed, detail);
	return refused;
}


int
main(void)
{
	unsigned short v4 = unused_port(AF_INET), v6 = unused_port(AF_INET6);
	int passed = 0;
	passed += probe("udp 127.0.0.1:<unused> recv", AF_INET, "127.0.0.1", v4, 0);
	passed += probe("udp 127.0.0.1:<unused> poll", AF_INET, "127.0.0.1", v4, 1);
	passed += probe("udp ::1:<unused> recv", AF_INET6, "::1", v6, 0);
	passed += probe("udp ::1:<unused> poll", AF_INET6, "::1", v6, 1);
	passed += probe("udp 0.0.0.0:53 recv (resolver default)", AF_INET,
		"0.0.0.0", htons(53), 0);
	passed += probe("udp [::]:53 recv (resolver default)", AF_INET6, "::",
		htons(53), 0);
	printf("%s: %d/6 refused\n", passed == 6 ? "PASS" : "FAIL", passed);
	return passed == 6 ? 0 : 1;
}
