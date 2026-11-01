/*
 * udp887.c -- issue 887: a UDP sendto() whose destination is INADDR_ANY
 * (0.0.0.0), or in6addr_any (::) on an IPv6 socket, must reach this host, as
 * connect() to that address already does here and as both do on Linux. The
 * receiver is bound to the wildcard address, as a Node cluster
 * worker's shared socket is.
 *
 * Controls: connect() to the wildcard address and send() (worked before), a
 * send() with no address on an unconnected socket (must stay EDESTADDRREQ),
 * and a sendto() to the loopback address.
 *
 * Exit status: the number of FAILs. Plain POSIX: builds and runs on Linux.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static int sFailed = 0;


static int
receiver(int family, struct sockaddr_storage* address, socklen_t* length)
{
	int fd = socket(family, SOCK_DGRAM, 0);
	memset(address, 0, sizeof(*address));
	address->ss_family = family;
	*length = family == AF_INET ? sizeof(struct sockaddr_in)
		: sizeof(struct sockaddr_in6);
	if (family == AF_INET6) {
		int on = 1;
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
	}
	if (bind(fd, (struct sockaddr*)address, *length) != 0
		|| getsockname(fd, (struct sockaddr*)address, length) != 0) {
		perror("bind");
		return -1;
	}
	return fd;
}


/*! \a destination is the receiver's port with the address \a which:
	0 the wildcard address, 1 loopback. */
static void
make_destination(int family, const struct sockaddr_storage* bound, int which,
	struct sockaddr_storage* destination)
{
	memcpy(destination, bound, sizeof(*destination));
	if (family == AF_INET) {
		((struct sockaddr_in*)destination)->sin_addr.s_addr
			= which ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
	} else {
		((struct sockaddr_in6*)destination)->sin6_addr
			= which ? in6addr_loopback : in6addr_any;
	}
}


static void
run(int family, const char* what, int mode)
{
	struct sockaddr_storage bound, destination;
	socklen_t length;
	int fd = receiver(family, &bound, &length);
	int sender = socket(family, SOCK_DGRAM, 0);
	ssize_t r;
	int e;

	errno = 0;
	if (mode == 0 || mode == 1) {
		// 0: sendto(wildcard), 1: sendto(loopback)
		make_destination(family, &bound, mode, &destination);
		r = sendto(sender, "hi", 2, 0, (struct sockaddr*)&destination,
			length);
	} else if (mode == 2) {
		make_destination(family, &bound, 0, &destination);
		r = connect(sender, (struct sockaddr*)&destination, length);
		if (r == 0)
			r = send(sender, "hi", 2, 0);
	} else {
		r = send(sender, "hi", 2, 0);
	}
	e = errno;

	struct pollfd p = { fd, POLLIN, 0 };
	int ready = poll(&p, 1, 500);
	char from[64] = "-";
	if (ready > 0) {
		struct sockaddr_storage source;
		socklen_t sourceLength = sizeof(source);
		char buffer[16];
		recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&source,
			&sourceLength);
		inet_ntop(family, family == AF_INET
				? (void*)&((struct sockaddr_in*)&source)->sin_addr
				: (void*)&((struct sockaddr_in6*)&source)->sin6_addr,
			from, sizeof(from));
	}

	int ok = mode == 3 ? (r < 0 && e == EDESTADDRREQ && ready == 0)
		: (r == 2 && ready == 1);
	printf("%s %-42s %s  r=%zd", family == AF_INET ? "udp4" : "udp6", what,
		ok ? "PASS" : "FAIL", r);
	if (r < 0)
		printf(" errno=%d (%s)", e, strerror(e));
	printf("  received=%d from=%s\n", ready, from);
	if (!ok)
		sFailed++;
	close(sender);
	close(fd);
}


int
main()
{
	static const int kFamilies[] = { AF_INET, AF_INET6 };
	for (int i = 0; i < 2; i++) {
		run(kFamilies[i], "sendto(wildcard address)", 0);
		run(kFamilies[i], "sendto(loopback) (control)", 1);
		run(kFamilies[i], "connect(wildcard) + send (control)", 2);
		run(kFamilies[i], "unconnected send, no address (control)", 3);
	}
	printf("udp887: %d failed\n", sFailed);
	return sFailed;
}
