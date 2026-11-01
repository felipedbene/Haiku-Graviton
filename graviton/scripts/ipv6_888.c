/*
 * ipv6_888.c -- issue 888: setsockopt(IPPROTO_IPV6, IPV6_MULTICAST_IF) takes
 * an unsigned interface index, 0 meaning the default interface (RFC 3493
 * section 5.2, POSIX <netinet/in.h>), and getsockopt() returns it. libuv
 * passes sin6_scope_id that way.
 *
 * Checks, for 0 and for every interface if_nameindex() lists: set, get back
 * the same index. An index no interface has must be refused. Informational
 * (not counted): the old 16-byte in6_addr form, a UDP datagram sent to
 * ff02::1 with the option set to each interface, and the same through a raw
 * ICMPv6 socket -- the only kind whose sends consult the option on Haiku
 * (ipv6_send_data()); UDP sends are routed by the datalink layer.
 *
 * Exit status: the number of FAILs. Plain POSIX: builds and runs on Linux.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static int sFailed = 0;


static void
print_result(int r, int e)
{
	printf("r=%d", r);
	if (r < 0)
		printf(" errno=%d (%s)", e, strerror(e));
}


static void
set_and_get(int fd, unsigned int index, const char* name)
{
	errno = 0;
	int r = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &index,
		sizeof(index));
	int e = errno;

	unsigned int got = 12345;
	socklen_t length = sizeof(got);
	errno = 0;
	int g = getsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &got, &length);
	int ge = errno;

	int ok = r == 0 && g == 0 && got == index;
	char what[96];
	snprintf(what, sizeof(what), "set/get index %u (%s)", index, name);
	printf("%-44s %s  set ", what, ok ? "PASS" : "FAIL");
	print_result(r, e);
	printf("  get ");
	print_result(g, ge);
	if (g == 0)
		printf(" value=%u", got);
	printf("\n");
	if (!ok)
		sFailed++;

	// informational: does a multicast send go out through it?
	struct sockaddr_in6 group;
	memset(&group, 0, sizeof(group));
	group.sin6_family = AF_INET6;
	group.sin6_port = htons(9);
	inet_pton(AF_INET6, "ff02::1", &group.sin6_addr);
	group.sin6_scope_id = index;
	errno = 0;
	ssize_t s = sendto(fd, "hi", 2, 0, (struct sockaddr*)&group,
		sizeof(group));
	e = errno;
	printf("    info: sendto(ff02::1%%%u) ", index);
	print_result((int)s, e);
	printf("\n");
}


int
main()
{
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	set_and_get(fd, 0, "default");

	struct if_nameindex* interfaces = if_nameindex();
	unsigned int highest = 0;
	for (struct if_nameindex* i = interfaces; i != NULL && i->if_index != 0;
			i++) {
		set_and_get(fd, i->if_index, i->if_name);
		if (i->if_index > highest)
			highest = i->if_index;
	}
	if (interfaces != NULL)
		if_freenameindex(interfaces);

	// back to the default
	set_and_get(fd, 0, "default again");

	// an index no interface has
	unsigned int bogus = highest + 1000;
	errno = 0;
	int r = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &bogus,
		sizeof(bogus));
	int e = errno;
	char what[64];
	snprintf(what, sizeof(what), "set index %u (no such interface)", bogus);
	printf("%-44s %s  ", what, r < 0 ? "PASS" : "FAIL");
	print_result(r, e);
	printf("\n");
	if (r == 0)
		sFailed++;

	// informational: the old, non-standard 16-byte in6_addr form
	struct in6_addr any = IN6ADDR_ANY_INIT;
	errno = 0;
	r = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &any, sizeof(any));
	e = errno;
	printf("info: set in6_addr :: (16-byte form)         ");
	print_result(r, e);
	printf("\n");

	close(fd);

	// informational: a raw ICMPv6 echo request to ff02::1 through each index
	int raw = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (raw < 0) {
		printf("info: raw ICMPv6 socket: %s\n", strerror(errno));
	} else {
		struct icmp6_hdr echo;
		memset(&echo, 0, sizeof(echo));
		echo.icmp6_type = ICMP6_ECHO_REQUEST;
		struct sockaddr_in6 group;
		memset(&group, 0, sizeof(group));
		group.sin6_family = AF_INET6;
		inet_pton(AF_INET6, "ff02::1", &group.sin6_addr);
		for (unsigned int index = 0; index <= highest; index++) {
			errno = 0;
			int r = setsockopt(raw, IPPROTO_IPV6, IPV6_MULTICAST_IF, &index,
				sizeof(index));
			int e = errno;
			printf("info: raw: set index %u ", index);
			print_result(r, e);
			errno = 0;
			ssize_t s = sendto(raw, &echo, sizeof(echo), 0,
				(struct sockaddr*)&group, sizeof(group));
			e = errno;
			printf("  sendto(ff02::1) ");
			print_result((int)s, e);
			printf("\n");
		}
		close(raw);
	}

	printf("ipv6_888: %d failed\n", sFailed);
	return sFailed;
}
