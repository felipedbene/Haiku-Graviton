/*
 * listen946.c -- issue 946: closing a listening socket must reset the
 * connections it never accept()ed, so their peers learn the connection is
 * gone (Linux: RST, the client's recv() fails with ECONNRESET). An accepted
 * connection must not be affected: it keeps working, and its normal close()
 * still ends in an orderly EOF, not a reset.
 *
 * Plain POSIX; every wait has a deadline. Exit status is the number of FAILs.
 * Linux 7.0 is the reference: it passes every row.
 *
 *   Haiku: gcc -O2 -Wall -o listen946 listen946.c -lnetwork
 *   Linux: gcc -O2 -Wall -o listen946 listen946.c
 */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int sFailures = 0;

static void
result(int pass, const char* name, const char* detail)
{
	printf("%-44s %s  %s\n", name, pass ? "PASS" : "FAIL", detail);
	if (!pass)
		sFailures++;
	fflush(stdout);
}

static int
make_listener(int family, const char* host, struct sockaddr_storage* address,
	socklen_t* length, int backlog)
{
	int s = socket(family, SOCK_STREAM, 0);
	memset(address, 0, sizeof(*address));
	if (family == AF_INET) {
		struct sockaddr_in* a = (struct sockaddr_in*)address;
		a->sin_family = AF_INET;
		inet_pton(AF_INET, host, &a->sin_addr);
		*length = sizeof(*a);
	} else {
		struct sockaddr_in6* a = (struct sockaddr_in6*)address;
		a->sin6_family = AF_INET6;
		inet_pton(AF_INET6, host, &a->sin6_addr);
		*length = sizeof(*a);
	}
	if (s < 0 || bind(s, (struct sockaddr*)address, *length) != 0
		|| listen(s, backlog) != 0) {
		printf("setup %s: %s\n", host, strerror(errno));
		exit(100);
	}
	getsockname(s, (struct sockaddr*)address, length);
	return s;
}

static int
connect_to(int family, struct sockaddr_storage* address, socklen_t length)
{
	int c = socket(family, SOCK_STREAM, 0);
	if (connect(c, (struct sockaddr*)address, length) != 0) {
		printf("connect: %s\n", strerror(errno));
		exit(101);
	}
	return c;
}

/* Waits up to 5 s for the client to learn the connection's fate. Returns
 * the errno its recv() failed with, 0 for EOF, -1 for "not told", -2 for
 * data. */
static int
client_outcome(int c, char* detail, size_t size)
{
	struct pollfd p = { c, POLLIN, 0 };
	int ready = poll(&p, 1, 5000);
	if (ready <= 0) {
		snprintf(detail, size, "not told within 5 s (poll=%d)", ready);
		return -1;
	}
	char byte;
	ssize_t n = recv(c, &byte, 1, 0);
	int e = errno;
	snprintf(detail, size, "poll revents=%#x recv=%zd %s", p.revents, n,
		n < 0 ? strerror(e) : n == 0 ? "(EOF)" : "(data)");
	return n < 0 ? e : n == 0 ? 0 : -2;
}

static void
unaccepted(int family, const char* host, const char* label, int count,
	int sendFirst)
{
	struct sockaddr_storage address;
	socklen_t length;
	int listener = make_listener(family, host, &address, &length, 8);
	int clients[8];
	for (int i = 0; i < count; i++) {
		clients[i] = connect_to(family, &address, length);
		if (sendFirst)
			send(clients[i], "hello", 5, 0);
	}
	usleep(100000);
	close(listener);

	for (int i = 0; i < count; i++) {
		char name[96], detail[160];
		snprintf(name, sizeof(name), "%s unaccepted %d/%d%s", label, i + 1,
			count, sendFirst ? " +data" : "");
		int outcome = client_outcome(clients[i], detail, sizeof(detail));
		result(outcome == ECONNRESET, name, detail);
		close(clients[i]);
	}
}

/* An accepted connection survives its listener's close, and its own close
 * is still an orderly one; an unaccepted sibling is reset. */
static void
accepted_survives(int family, const char* host, const char* label)
{
	struct sockaddr_storage address;
	socklen_t length;
	int listener = make_listener(family, host, &address, &length, 8);
	int first = connect_to(family, &address, length);
	int server = accept(listener, NULL, NULL);
	int second = connect_to(family, &address, length);
	usleep(100000);
	close(listener);

	char name[96], detail[160];
	snprintf(name, sizeof(name), "%s unaccepted sibling", label);
	int outcome = client_outcome(second, detail, sizeof(detail));
	result(outcome == ECONNRESET, name, detail);

	char buffer[8] = { 0 };
	int ok = server >= 0 && send(first, "ping", 4, 0) == 4;
	struct pollfd p = { server, POLLIN, 0 };
	ok = ok && poll(&p, 1, 5000) == 1 && recv(server, buffer, 8, 0) == 4
		&& memcmp(buffer, "ping", 4) == 0;
	ok = ok && send(server, "pong", 4, 0) == 4;
	p.fd = first;
	ok = ok && poll(&p, 1, 5000) == 1 && recv(first, buffer, 8, 0) == 4
		&& memcmp(buffer, "pong", 4) == 0;
	snprintf(name, sizeof(name), "%s accepted still works", label);
	result(ok, name, ok ? "ping/pong after the listener closed"
		: "exchange failed");

	close(server);
	outcome = client_outcome(first, detail, sizeof(detail));
	snprintf(name, sizeof(name), "%s accepted close is EOF", label);
	result(outcome == 0, name, detail);
	close(first);
	close(second);
}

int
main()
{
	char own[INET_ADDRSTRLEN] = "";
	struct ifaddrs* list;
	if (getifaddrs(&list) == 0) {
		for (struct ifaddrs* i = list; i != NULL; i = i->ifa_next) {
			if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET
				|| (i->ifa_flags & IFF_LOOPBACK) != 0)
				continue;
			inet_ntop(AF_INET, &((struct sockaddr_in*)i->ifa_addr)->sin_addr,
				own, sizeof(own));
			break;
		}
		freeifaddrs(list);
	}

	unaccepted(AF_INET, "127.0.0.1", "127.0.0.1", 1, 0);
	unaccepted(AF_INET, "127.0.0.1", "127.0.0.1", 1, 1);
	unaccepted(AF_INET, "127.0.0.1", "127.0.0.1", 4, 0);
	unaccepted(AF_INET6, "::1", "::1", 1, 0);
	unaccepted(AF_INET6, "::1", "::1", 2, 1);
	if (own[0] != '\0') {
		char label[64];
		snprintf(label, sizeof(label), "own %s", own);
		unaccepted(AF_INET, own, label, 2, 0);
	} else
		printf("no non-loopback IPv4 address: own-address rows skipped\n");
	accepted_survives(AF_INET, "127.0.0.1", "127.0.0.1");
	accepted_survives(AF_INET6, "::1", "::1");

	printf("FAILS %d\n", sFailures);
	return sFailures;
}
