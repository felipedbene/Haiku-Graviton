/*
 * dualstack982 -- can an IPv4 client reach a TCP listener bound to [::]?
 *
 * OpenSSL's s_server and ocsp responder (with the DeBeOS patchset, which drops
 * the IPV6_V6ONLY setsockopt on Haiku) listen on [::] and print
 * "ACCEPT [::]:<port>". test/recipes/82-test_ocsp_cert_chain.t then connects
 * to "localhost:<port>", which resolves to 127.0.0.1. That only works if the
 * [::] listener is dual-stack, as it is by default on Linux (issue 982).
 *
 * Rows: the listener's IPV6_V6ONLY as read back, then a connect() to the
 * listener's port over 127.0.0.1 and over ::1.
 *
 * Build: cc -o dualstack982 dualstack982.c   (add -lnetwork on Haiku)
 */

#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static int
try_connect(int af, unsigned short port)
{
	struct sockaddr_storage address;
	memset(&address, 0, sizeof(address));
	socklen_t length;
	if (af == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)&address;
		in->sin_family = AF_INET;
		in->sin_port = port;
		in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		length = sizeof(*in);
	} else {
		struct sockaddr_in6* in6 = (struct sockaddr_in6*)&address;
		in6->sin6_family = AF_INET6;
		in6->sin6_port = port;
		in6->sin6_addr = in6addr_loopback;
		length = sizeof(*in6);
	}

	int fd = socket(af, SOCK_STREAM, 0);
	int result = connect(fd, (struct sockaddr*)&address, length);
	int error = errno;
	printf("connect %-9s -> [::]:%u  %s\n", af == AF_INET ? "127.0.0.1" : "::1",
		ntohs(port), result == 0 ? "OK" : strerror(error));
	close(fd);
	return result == 0;
}


int
main(void)
{
	int listener = socket(AF_INET6, SOCK_STREAM, 0);
	struct sockaddr_in6 any;
	memset(&any, 0, sizeof(any));
	any.sin6_family = AF_INET6;
	any.sin6_addr = in6addr_any;
	if (bind(listener, (struct sockaddr*)&any, sizeof(any)) != 0
		|| listen(listener, 8) != 0) {
		perror("bind/listen [::]:0");
		return 2;
	}
	socklen_t length = sizeof(any);
	getsockname(listener, (struct sockaddr*)&any, &length);

	int v6only = -1;
	socklen_t optionLength = sizeof(v6only);
	if (getsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
			&optionLength) != 0)
		printf("listener [::]:%u IPV6_V6ONLY: getsockopt failed: %s\n",
			ntohs(any.sin6_port), strerror(errno));
	else
		printf("listener [::]:%u IPV6_V6ONLY=%d\n", ntohs(any.sin6_port),
			v6only);

	int v4 = try_connect(AF_INET, any.sin6_port);
	int v6 = try_connect(AF_INET6, any.sin6_port);
	printf("%s\n", v4 && v6 ? "PASS: [::] listener is dual-stack"
		: "FAIL: [::] listener does not take IPv4 connections");
	close(listener);
	return v4 && v6 ? 0 : 1;
}
