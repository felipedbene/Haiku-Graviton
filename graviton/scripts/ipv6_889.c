/*
 * ipv6_889.c -- issue 889: bind() of an IPv6 socket to an address that is not
 * configured on this host must fail with EADDRNOTAVAIL, as the IPv4 bind
 * does, not with B_ERROR (errno -1, "General system error").
 *
 * Checked for UDP and TCP over IPv6 (both reach ipv6_bind()). Controls: the
 * same for IPv4, and IPv6 binds that must work: ::1 and ::.
 *
 * Exit status: the number of FAILs. Plain POSIX: builds and runs on Linux.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static int sFailed = 0;


static void
try_bind(int family, int type, const char* text, int expectOk)
{
	struct sockaddr_storage address;
	memset(&address, 0, sizeof(address));
	socklen_t length;
	if (family == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)&address;
		in->sin_family = AF_INET;
		inet_pton(AF_INET, text, &in->sin_addr);
		length = sizeof(*in);
	} else {
		struct sockaddr_in6* in6 = (struct sockaddr_in6*)&address;
		in6->sin6_family = AF_INET6;
		inet_pton(AF_INET6, text, &in6->sin6_addr);
		length = sizeof(*in6);
	}

	int fd = socket(family, type, 0);
	if (family == AF_INET6) {
		int on = 1;
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
	}
	errno = 0;
	int r = bind(fd, (struct sockaddr*)&address, length);
	int e = errno;
	int ok = expectOk ? r == 0 : (r < 0 && e == EADDRNOTAVAIL);

	char what[64];
	snprintf(what, sizeof(what), "%s%s bind %s%s",
		type == SOCK_DGRAM ? "udp" : "tcp", family == AF_INET ? "4" : "6",
		text, expectOk ? " (control)" : "");
	printf("%-36s %s  r=%d", what, ok ? "PASS" : "FAIL", r);
	if (r < 0)
		printf(" errno=%d (%s)", e, strerror(e));
	printf("\n");
	if (!ok)
		sFailed++;
	close(fd);
}


int
main()
{
	static const int kTypes[] = { SOCK_DGRAM, SOCK_STREAM };
	for (int i = 0; i < 2; i++) {
		try_bind(AF_INET6, kTypes[i], "111::1", 0);
		try_bind(AF_INET6, kTypes[i], "2001:db8::1", 0);
		try_bind(AF_INET, kTypes[i], "1.1.1.1", 0);
		try_bind(AF_INET6, kTypes[i], "::1", 1);
		try_bind(AF_INET6, kTypes[i], "::", 1);
	}
	printf("ipv6_889: %d failed (EADDRNOTAVAIL=%d)\n", sFailed, EADDRNOTAVAIL);
	return sFailed;
}
