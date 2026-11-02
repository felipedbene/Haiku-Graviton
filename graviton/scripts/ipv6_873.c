/*
 * ipv6_873.c -- issue 873: dual-stack IPv6 sockets. With IPV6_V6ONLY off,
 * which is the default (as on Linux, net.ipv6.bindv6only=0), an IPv6 socket
 * also carries IPv4: a socket bound to :: accepts IPv4 connections and
 * datagrams, an IPv6 socket can connect and send to an IPv4-mapped address
 * (::ffff:a.b.c.d), and every address it reports for an IPv4 peer is
 * IPv4-mapped. With IPV6_V6ONLY on, none of that happens.
 *
 * Covered:
 *   - IPV6_V6ONLY: default, set 0 and 1 and read back, EINVAL once bound
 *   - TCP: an IPv4 client to a :: listener, IPV6_V6ONLY 0 and 1; the mapped
 *     addresses accept(), getsockname() and getpeername() report; an IPv6
 *     client to the same listener; a listener on ::1 takes no IPv4
 *   - TCP: an IPv6 socket connecting to ::ffff:127.0.0.1, bound to a mapped
 *     address, and IPV6_V6ONLY 1 refusing both
 *   - UDP: an IPv4 datagram to a :: socket and the address recvfrom()
 *     reports; the reply sent back to that mapped address; connect() to a
 *     mapped address; IPV6_V6ONLY 1 refusing all of it
 *   - the bind-conflict matrix: :: with IPV6_V6ONLY 0 and 0.0.0.0 on one
 *     port conflict in either order, with IPV6_V6ONLY 1 they do not; a
 *     specific IPv4 address against :: follows the rule this system applies
 *     within one family
 *
 * Exit status: the number of FAILs. Plain POSIX: builds and runs on Linux,
 * which is the reference.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


static int sFailed = 0;


static void
report(const char* what, int ok, const char* detail)
{
	printf("%-60s %s  %s\n", what, ok ? "PASS" : "FAIL", detail);
	if (!ok)
		sFailed++;
}


static const char*
error_text(int r, int e)
{
	static char buffer[96];
	if (r >= 0)
		snprintf(buffer, sizeof(buffer), "r=%d", r);
	else
		snprintf(buffer, sizeof(buffer), "r=%d errno=%d (%s)", r, e,
			strerror(e));
	return buffer;
}


static const char*
address_text(const struct sockaddr_storage* address, socklen_t length)
{
	static char buffers[4][80];
	static int next = 0;
	char* buffer = buffers[next++ % 4];
	char host[INET6_ADDRSTRLEN] = "?";

	if (address->ss_family == AF_INET) {
		const struct sockaddr_in* in = (const struct sockaddr_in*)address;
		inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
		snprintf(buffer, 80, "AF_INET %s:%u len=%u", host,
			ntohs(in->sin_port), (unsigned)length);
	} else if (address->ss_family == AF_INET6) {
		const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)address;
		inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
		snprintf(buffer, 80, "AF_INET6 [%s]:%u len=%u", host,
			ntohs(in6->sin6_port), (unsigned)length);
	} else
		snprintf(buffer, 80, "family %d len=%u", address->ss_family,
			(unsigned)length);
	return buffer;
}


static socklen_t
make_address(int family, const char* text, int port,
	struct sockaddr_storage* address)
{
	memset(address, 0, sizeof(*address));
	if (family == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)address;
		in->sin_family = AF_INET;
		in->sin_port = htons(port);
		inet_pton(AF_INET, text, &in->sin_addr);
		return sizeof(*in);
	}

	struct sockaddr_in6* in6 = (struct sockaddr_in6*)address;
	in6->sin6_family = AF_INET6;
	in6->sin6_port = htons(port);
	inet_pton(AF_INET6, text, &in6->sin6_addr);
	return sizeof(*in6);
}


static int
port_of(const struct sockaddr_storage* address)
{
	if (address->ss_family == AF_INET)
		return ntohs(((const struct sockaddr_in*)address)->sin_port);
	return ntohs(((const struct sockaddr_in6*)address)->sin6_port);
}


/*! True if \a address is AF_INET6 ::ffff:<ipv4>, port \a port (if >= 0). */
static int
is_mapped(const struct sockaddr_storage* address, socklen_t length,
	const char* ipv4, int port)
{
	if (address->ss_family != AF_INET6
		|| length != sizeof(struct sockaddr_in6))
		return 0;

	struct sockaddr_storage expected;
	char text[64];
	snprintf(text, sizeof(text), "::ffff:%s", ipv4);
	make_address(AF_INET6, text, 0, &expected);
	const struct sockaddr_in6* in6 = (const struct sockaddr_in6*)address;
	if (memcmp(&in6->sin6_addr,
			&((struct sockaddr_in6*)&expected)->sin6_addr,
			sizeof(struct in6_addr)) != 0)
		return 0;
	return port < 0 || ntohs(in6->sin6_port) == port;
}


static int
is_ipv4(const struct sockaddr_storage* address, const char* ipv4, int port)
{
	if (address->ss_family != AF_INET)
		return 0;
	const struct sockaddr_in* in = (const struct sockaddr_in*)address;
	struct in_addr expected;
	inet_pton(AF_INET, ipv4, &expected);
	return in->sin_addr.s_addr == expected.s_addr
		&& (port < 0 || ntohs(in->sin_port) == port);
}


static void
set_timeouts(int fd)
{
	struct timeval timeout = { 2, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}


/*! A socket of \a family; for AF_INET6, IPV6_V6ONLY is set to \a v6only
	unless it is -1 (the default is left alone). */
static int
open_socket(int family, int type, int v6only)
{
	int fd = socket(family, type, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}
	set_timeouts(fd);
	if (family == AF_INET6 && v6only >= 0) {
		if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
				sizeof(v6only)) != 0) {
			printf("  (setsockopt IPV6_V6ONLY=%d: %s)\n", v6only,
				strerror(errno));
		}
	}
	return fd;
}


static int
bound_port(int fd)
{
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	if (getsockname(fd, (struct sockaddr*)&address, &length) != 0)
		return -1;
	return port_of(&address);
}


/*!	A listener on \a text:0 (of \a family), returns its fd; *_port is set. */
static int
listener(int family, const char* text, int v6only, int* _port)
{
	int fd = open_socket(family, SOCK_STREAM, v6only);
	struct sockaddr_storage address;
	socklen_t length = make_address(family, text, 0, &address);
	if (bind(fd, (struct sockaddr*)&address, length) != 0
		|| listen(fd, 8) != 0) {
		printf("  (listener %s: %s)\n", text, strerror(errno));
		close(fd);
		return -1;
	}
	*_port = bound_port(fd);
	return fd;
}


/*!	Connects a \a family client to \a text:\a port; returns the fd or -1 with
	errno set. */
static int
connect_to(int family, const char* text, int port, int v6only)
{
	int fd = open_socket(family, SOCK_STREAM, v6only);
	struct sockaddr_storage address;
	socklen_t length = make_address(family, text, port, &address);
	if (connect(fd, (struct sockaddr*)&address, length) != 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	return fd;
}


/*!	Sends "ping" one way and "pong" back over a connected pair. */
static int
exchange(int a, int b)
{
	char buffer[8];
	if (send(a, "ping", 4, 0) != 4 || recv(b, buffer, sizeof(buffer), 0) != 4
		|| memcmp(buffer, "ping", 4) != 0)
		return 0;
	if (send(b, "pong", 4, 0) != 4 || recv(a, buffer, sizeof(buffer), 0) != 4
		|| memcmp(buffer, "pong", 4) != 0)
		return 0;
	return 1;
}


static int
accept_ready(int fd, int milliseconds)
{
	struct pollfd p = { fd, POLLIN, 0 };
	return poll(&p, 1, milliseconds) == 1;
}


//	#pragma mark - IPV6_V6ONLY itself


static void
test_option(int type)
{
	const char* name = type == SOCK_STREAM ? "tcp" : "udp";
	char what[96];
	int value = -1;
	socklen_t length = sizeof(value);

	int fd = socket(AF_INET6, type, 0);
	int r = getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &value, &length);
	snprintf(what, sizeof(what), "%s6 IPV6_V6ONLY default is 0", name);
	char detail[160];
	snprintf(detail, sizeof(detail), "%s value=%d", error_text(r, errno),
		value);
	report(what, r == 0 && value == 0, detail);

	int setting[] = { 1, 0 };
	for (int i = 0; i < 2; i++) {
		int on = setting[i];
		r = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
		int e = errno;
		value = -1;
		length = sizeof(value);
		getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &value, &length);
		snprintf(what, sizeof(what), "%s6 set IPV6_V6ONLY=%d, read back",
			name, on);
		snprintf(detail, sizeof(detail), "set %s, get value=%d",
			error_text(r, e), value);
		report(what, r == 0 && value == on, detail);
	}

	struct sockaddr_storage address;
	socklen_t addressLength = make_address(AF_INET6, "::", 0, &address);
	bind(fd, (struct sockaddr*)&address, addressLength);
	int on = 1;
	r = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
	int e = errno;
	snprintf(what, sizeof(what), "%s6 set IPV6_V6ONLY after bind -> EINVAL",
		name);
	report(what, r < 0 && e == EINVAL, error_text(r, e));
	close(fd);
}


//	#pragma mark - TCP


static void
test_tcp_ipv4_client_to_wildcard(int v6only)
{
	char what[96];
	char detail[200];
	int port;
	int server = listener(AF_INET6, "::", v6only, &port);
	if (server < 0) {
		snprintf(what, sizeof(what), "tcp listener [::] V6ONLY=%d", v6only);
		report(what, 0, "could not listen");
		return;
	}

	int client = connect_to(AF_INET, "127.0.0.1", port, -1);
	int e = errno;
	snprintf(what, sizeof(what), "tcp4 127.0.0.1 -> [::] V6ONLY=%d %s",
		v6only, v6only ? "refused" : "connects");
	if (v6only) {
		report(what, client < 0 && e == ECONNREFUSED,
			error_text(client < 0 ? -1 : 0, e));
		if (client >= 0)
			close(client);
		close(server);
		return;
	}
	report(what, client >= 0, error_text(client < 0 ? -1 : 0, e));
	if (client < 0) {
		close(server);
		return;
	}

	struct sockaddr_storage clientLocal;
	socklen_t clientLocalLength = sizeof(clientLocal);
	getsockname(client, (struct sockaddr*)&clientLocal, &clientLocalLength);
	int clientPort = port_of(&clientLocal);

	struct sockaddr_storage peer;
	socklen_t peerLength = sizeof(peer);
	int accepted = accept_ready(server, 2000)
		? accept(server, (struct sockaddr*)&peer, &peerLength) : -1;
	e = errno;
	snprintf(detail, sizeof(detail), "%s %s",
		error_text(accepted < 0 ? -1 : 0, e),
		accepted >= 0 ? address_text(&peer, peerLength) : "");
	report("  accept() address is ::ffff:127.0.0.1:<client port>",
		accepted >= 0 && is_mapped(&peer, peerLength, "127.0.0.1", clientPort),
		detail);
	if (accepted < 0) {
		close(client);
		close(server);
		return;
	}

	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	getsockname(accepted, (struct sockaddr*)&address, &length);
	report("  accepted getsockname() is ::ffff:127.0.0.1:<listen port>",
		is_mapped(&address, length, "127.0.0.1", port),
		address_text(&address, length));

	length = sizeof(address);
	getpeername(accepted, (struct sockaddr*)&address, &length);
	report("  accepted getpeername() is ::ffff:127.0.0.1:<client port>",
		is_mapped(&address, length, "127.0.0.1", clientPort),
		address_text(&address, length));

	length = sizeof(address);
	getpeername(client, (struct sockaddr*)&address, &length);
	report("  client getpeername() is plain 127.0.0.1:<listen port>",
		is_ipv4(&address, "127.0.0.1", port), address_text(&address, length));

	report("  data both ways", exchange(client, accepted), "ping/pong");

	length = sizeof(address);
	getsockname(server, (struct sockaddr*)&address, &length);
	struct sockaddr_storage wildcard;
	make_address(AF_INET6, "::", port, &wildcard);
	report("  listener getsockname() stays [::]:<port>",
		address.ss_family == AF_INET6
			&& memcmp(&((struct sockaddr_in6*)&address)->sin6_addr,
				&((struct sockaddr_in6*)&wildcard)->sin6_addr, 16) == 0
			&& port_of(&address) == port,
		address_text(&address, length));

	close(accepted);
	close(client);

	// the IPv6 path through the same listener is unaffected
	client = connect_to(AF_INET6, "::1", port, -1);
	e = errno;
	peerLength = sizeof(peer);
	accepted = client >= 0 && accept_ready(server, 2000)
		? accept(server, (struct sockaddr*)&peer, &peerLength) : -1;
	struct sockaddr_storage loopback;
	make_address(AF_INET6, "::1", 0, &loopback);
	snprintf(detail, sizeof(detail), "%s %s",
		error_text(client < 0 ? -1 : 0, e),
		accepted >= 0 ? address_text(&peer, peerLength) : "");
	report("  tcp6 ::1 -> same listener, accept() is [::1] (control)",
		accepted >= 0 && peer.ss_family == AF_INET6
			&& memcmp(&((struct sockaddr_in6*)&peer)->sin6_addr,
				&((struct sockaddr_in6*)&loopback)->sin6_addr, 16) == 0
			&& exchange(client, accepted),
		detail);
	if (accepted >= 0)
		close(accepted);
	if (client >= 0)
		close(client);
	close(server);
}


static void
test_tcp_default_listener()
{
	// Node's listen(port) without a host: bind :: with nothing set
	int port;
	int server = listener(AF_INET6, "::", -1, &port);
	int client = server >= 0 ? connect_to(AF_INET, "127.0.0.1", port, -1) : -1;
	int e = errno;
	int accepted = client >= 0 && accept_ready(server, 2000)
		? accept(server, NULL, NULL) : -1;
	report("tcp4 127.0.0.1 -> [::] with IPV6_V6ONLY untouched connects",
		accepted >= 0 && exchange(client, accepted),
		error_text(client < 0 ? -1 : 0, e));
	if (accepted >= 0)
		close(accepted);
	if (client >= 0)
		close(client);
	if (server >= 0)
		close(server);
}


static void
test_tcp_ipv4_client_to_loopback6()
{
	// ::1 is not an IPv4 address; a listener there takes no IPv4
	int port;
	int server = listener(AF_INET6, "::1", 0, &port);
	int client = server >= 0 ? connect_to(AF_INET, "127.0.0.1", port, -1) : -1;
	int e = errno;
	report("tcp4 127.0.0.1 -> [::1] V6ONLY=0 refused",
		server >= 0 && client < 0 && e == ECONNREFUSED,
		error_text(client < 0 ? -1 : 0, e));
	if (client >= 0)
		close(client);
	if (server >= 0)
		close(server);
}


static void
test_tcp_ipv6_client_to_mapped(int v6only)
{
	char what[96];
	char detail[200];
	int port;
	int server = listener(AF_INET, "127.0.0.1", -1, &port);

	int client = connect_to(AF_INET6, "::ffff:127.0.0.1", port, v6only);
	int e = errno;
	snprintf(what, sizeof(what),
		"tcp6 V6ONLY=%d connect ::ffff:127.0.0.1 -> tcp4 listener %s", v6only,
		v6only ? "ENETUNREACH" : "connects");
	if (v6only) {
		report(what, client < 0 && e == ENETUNREACH,
			error_text(client < 0 ? -1 : 0, e));
		if (client >= 0)
			close(client);
		close(server);
		return;
	}
	report(what, client >= 0, error_text(client < 0 ? -1 : 0, e));
	if (client < 0) {
		close(server);
		return;
	}

	struct sockaddr_storage local;
	socklen_t localLength = sizeof(local);
	getsockname(client, (struct sockaddr*)&local, &localLength);
	report("  client getsockname() is ::ffff:127.0.0.1:<port>",
		is_mapped(&local, localLength, "127.0.0.1", -1)
			&& port_of(&local) != 0,
		address_text(&local, localLength));

	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	getpeername(client, (struct sockaddr*)&address, &length);
	report("  client getpeername() is ::ffff:127.0.0.1:<listen port>",
		is_mapped(&address, length, "127.0.0.1", port),
		address_text(&address, length));

	struct sockaddr_storage peer;
	socklen_t peerLength = sizeof(peer);
	int accepted = accept_ready(server, 2000)
		? accept(server, (struct sockaddr*)&peer, &peerLength) : -1;
	snprintf(detail, sizeof(detail), "%s",
		accepted >= 0 ? address_text(&peer, peerLength) : "no connection");
	report("  tcp4 server sees plain 127.0.0.1:<client port>",
		accepted >= 0 && is_ipv4(&peer, "127.0.0.1", port_of(&local)),
		detail);
	if (accepted >= 0) {
		report("  data both ways", exchange(client, accepted), "ping/pong");
		close(accepted);
	}
	close(client);
	close(server);

	// nobody listening: refused, like any IPv4 connect
	client = connect_to(AF_INET6, "::ffff:127.0.0.1", port, 0);
	e = errno;
	report("tcp6 connect ::ffff:127.0.0.1:<closed port> -> ECONNREFUSED",
		client < 0 && e == ECONNREFUSED, error_text(client < 0 ? -1 : 0, e));
	if (client >= 0)
		close(client);
}


static void
test_tcp_bind_mapped()
{
	int fd = open_socket(AF_INET6, SOCK_STREAM, 0);
	struct sockaddr_storage address;
	socklen_t length = make_address(AF_INET6, "::ffff:127.0.0.1", 0, &address);
	int r = bind(fd, (struct sockaddr*)&address, length);
	int e = errno;
	if (r == 0)
		r = listen(fd, 8);
	struct sockaddr_storage local;
	socklen_t localLength = sizeof(local);
	getsockname(fd, (struct sockaddr*)&local, &localLength);
	char detail[200];
	snprintf(detail, sizeof(detail), "%s %s", error_text(r, e),
		address_text(&local, localLength));
	report("tcp6 bind ::ffff:127.0.0.1 + listen, getsockname() mapped",
		r == 0 && is_mapped(&local, localLength, "127.0.0.1", -1), detail);

	int port = port_of(&local);
	int client = r == 0 ? connect_to(AF_INET, "127.0.0.1", port, -1) : -1;
	e = errno;
	struct sockaddr_storage peer;
	socklen_t peerLength = sizeof(peer);
	int accepted = client >= 0 && accept_ready(fd, 2000)
		? accept(fd, (struct sockaddr*)&peer, &peerLength) : -1;
	snprintf(detail, sizeof(detail), "%s %s",
		error_text(client < 0 ? -1 : 0, e),
		accepted >= 0 ? address_text(&peer, peerLength) : "");
	report("  tcp4 client connects, accept() is mapped",
		accepted >= 0 && is_mapped(&peer, peerLength, "127.0.0.1",
			bound_port(client)) && exchange(client, accepted),
		detail);
	if (accepted >= 0)
		close(accepted);
	if (client >= 0)
		close(client);
	close(fd);

	fd = open_socket(AF_INET6, SOCK_STREAM, 1);
	r = bind(fd, (struct sockaddr*)&address, length);
	e = errno;
	report("tcp6 V6ONLY=1 bind ::ffff:127.0.0.1 -> EINVAL",
		r < 0 && e == EINVAL, error_text(r, e));
	close(fd);
}


//	#pragma mark - UDP


static void
test_udp_ipv4_to_wildcard(int v6only)
{
	char what[96];
	char detail[200];
	int server = open_socket(AF_INET6, SOCK_DGRAM, v6only);
	struct sockaddr_storage address;
	socklen_t length = make_address(AF_INET6, "::", 0, &address);
	bind(server, (struct sockaddr*)&address, length);
	int port = bound_port(server);

	int client = open_socket(AF_INET, SOCK_DGRAM, -1);
	struct sockaddr_storage target;
	socklen_t targetLength = make_address(AF_INET, "127.0.0.1", port, &target);
	int r = sendto(client, "udp4", 4, 0, (struct sockaddr*)&target,
		targetLength);
	int clientPort = bound_port(client);

	struct timeval timeout = { 0, v6only ? 500000 : 2000000 };
	setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	char buffer[16];
	struct sockaddr_storage from;
	socklen_t fromLength = sizeof(from);
	memset(&from, 0, sizeof(from));
	ssize_t received = recvfrom(server, buffer, sizeof(buffer), 0,
		(struct sockaddr*)&from, &fromLength);
	int e = errno;

	snprintf(what, sizeof(what), "udp4 127.0.0.1 -> [::] V6ONLY=%d %s",
		v6only, v6only ? "not delivered" : "delivered");
	snprintf(detail, sizeof(detail), "sendto r=%d, recvfrom %s %s", r,
		error_text((int)received, e),
		received >= 0 ? address_text(&from, fromLength) : "");
	if (v6only) {
		report(what, received < 0, detail);
		close(client);
		close(server);
		return;
	}
	report(what, received == 4 && memcmp(buffer, "udp4", 4) == 0, detail);
	report("  recvfrom() address is ::ffff:127.0.0.1:<sender port>",
		received == 4 && is_mapped(&from, fromLength, "127.0.0.1", clientPort),
		address_text(&from, fromLength));

	// answer to the address recvfrom() gave: an IPv6 socket sending to a
	// mapped address
	r = sendto(server, "back", 4, 0, (struct sockaddr*)&from, fromLength);
	e = errno;
	struct sockaddr_storage replyFrom;
	socklen_t replyFromLength = sizeof(replyFrom);
	received = recvfrom(client, buffer, sizeof(buffer), 0,
		(struct sockaddr*)&replyFrom, &replyFromLength);
	snprintf(detail, sizeof(detail), "sendto %s, udp4 got %zd from %s",
		error_text(r, e), received,
		received >= 0 ? address_text(&replyFrom, replyFromLength) : "-");
	report("  udp6 sendto ::ffff:127.0.0.1 reply reaches the udp4 socket",
		r == 4 && received == 4 && memcmp(buffer, "back", 4) == 0
			&& is_ipv4(&replyFrom, "127.0.0.1", port),
		detail);

	// IPv6 datagrams to the same socket are unaffected
	int client6 = open_socket(AF_INET6, SOCK_DGRAM, -1);
	targetLength = make_address(AF_INET6, "::1", port, &target);
	r = sendto(client6, "udp6", 4, 0, (struct sockaddr*)&target,
		targetLength);
	fromLength = sizeof(from);
	received = recvfrom(server, buffer, sizeof(buffer), 0,
		(struct sockaddr*)&from, &fromLength);
	e = errno;
	snprintf(detail, sizeof(detail), "sendto r=%d, recvfrom %s %s", r,
		error_text((int)received, e),
		received >= 0 ? address_text(&from, fromLength) : "");
	report("  udp6 ::1 -> same socket (control)",
		received == 4 && memcmp(buffer, "udp6", 4) == 0
			&& from.ss_family == AF_INET6
			&& !is_mapped(&from, fromLength, "127.0.0.1", -1),
		detail);
	close(client6);
	close(client);
	close(server);
}


static void
test_udp_ipv6_to_mapped(int v6only)
{
	char what[96];
	char detail[200];
	int server = open_socket(AF_INET, SOCK_DGRAM, -1);
	struct sockaddr_storage address;
	socklen_t length = make_address(AF_INET, "127.0.0.1", 0, &address);
	bind(server, (struct sockaddr*)&address, length);
	int port = bound_port(server);

	int client = open_socket(AF_INET6, SOCK_DGRAM, v6only);
	struct sockaddr_storage target;
	socklen_t targetLength = make_address(AF_INET6, "::ffff:127.0.0.1", port,
		&target);
	int r = sendto(client, "map6", 4, 0, (struct sockaddr*)&target,
		targetLength);
	int e = errno;

	snprintf(what, sizeof(what), "udp6 V6ONLY=%d sendto ::ffff:127.0.0.1 %s",
		v6only, v6only ? "-> ENETUNREACH" : "reaches udp4");
	if (v6only) {
		report(what, r < 0 && e == ENETUNREACH, error_text(r, e));

		r = connect(client, (struct sockaddr*)&target, targetLength);
		e = errno;
		report("udp6 V6ONLY=1 connect ::ffff:127.0.0.1 -> ENETUNREACH",
			r < 0 && e == ENETUNREACH, error_text(r, e));
		close(client);
		close(server);
		return;
	}

	char buffer[16];
	struct sockaddr_storage from;
	socklen_t fromLength = sizeof(from);
	ssize_t received = recvfrom(server, buffer, sizeof(buffer), 0,
		(struct sockaddr*)&from, &fromLength);
	int clientPort = bound_port(client);
	snprintf(detail, sizeof(detail), "sendto %s, udp4 got %zd from %s",
		error_text(r, e), received,
		received >= 0 ? address_text(&from, fromLength) : "-");
	report(what, r == 4 && received == 4 && memcmp(buffer, "map6", 4) == 0
		&& is_ipv4(&from, "127.0.0.1", clientPort), detail);
	close(client);

	// connect() to the mapped address, then send() and recv()
	client = open_socket(AF_INET6, SOCK_DGRAM, 0);
	r = connect(client, (struct sockaddr*)&target, targetLength);
	e = errno;
	report("udp6 connect ::ffff:127.0.0.1", r == 0, error_text(r, e));

	struct sockaddr_storage local;
	socklen_t localLength = sizeof(local);
	getsockname(client, (struct sockaddr*)&local, &localLength);
	report("  getsockname() is ::ffff:127.0.0.1:<port>",
		is_mapped(&local, localLength, "127.0.0.1", -1) && port_of(&local) != 0,
		address_text(&local, localLength));

	length = sizeof(address);
	r = getpeername(client, (struct sockaddr*)&address, &length);
	report("  getpeername() is ::ffff:127.0.0.1:<server port>",
		r == 0 && is_mapped(&address, length, "127.0.0.1", port),
		r == 0 ? address_text(&address, length) : error_text(r, errno));

	r = send(client, "conn", 4, 0);
	fromLength = sizeof(from);
	received = recvfrom(server, buffer, sizeof(buffer), 0,
		(struct sockaddr*)&from, &fromLength);
	ssize_t back = -1;
	if (received == 4) {
		sendto(server, "ack!", 4, 0, (struct sockaddr*)&from, fromLength);
		back = recv(client, buffer, sizeof(buffer), 0);
	}
	snprintf(detail, sizeof(detail), "send r=%d, udp4 got %zd from %s, "
		"reply %zd", r, received,
		received >= 0 ? address_text(&from, fromLength) : "-", back);
	report("  send() and recv() through the connection",
		r == 4 && received == 4 && is_ipv4(&from, "127.0.0.1", port_of(&local))
			&& back == 4 && memcmp(buffer, "ack!", 4) == 0,
		detail);
	close(client);
	close(server);
}


//	#pragma mark - bind conflicts


/*!	Binds \a first, then \a second, to one port (the one \a first got);
	returns the result of the second bind: 0 or errno. */
static int
second_bind(int type, int firstFamily, const char* first, int firstV6only,
	int secondFamily, const char* second, int secondV6only, int reusePort)
{
	int a = open_socket(firstFamily, type, firstV6only);
	int b = open_socket(secondFamily, type, secondV6only);
	if (reusePort) {
		int on = 1;
		setsockopt(a, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
		setsockopt(b, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
	}

	struct sockaddr_storage address;
	socklen_t length = make_address(firstFamily, first, 0, &address);
	if (bind(a, (struct sockaddr*)&address, length) != 0) {
		int e = errno;
		printf("  (first bind %s failed: %s)\n", first, strerror(e));
		close(a);
		close(b);
		return -1;
	}
	int port = bound_port(a);
	if (type == SOCK_STREAM)
		listen(a, 4);

	length = make_address(secondFamily, second, port, &address);
	int result = bind(b, (struct sockaddr*)&address, length) == 0 ? 0 : errno;
	close(a);
	close(b);
	return result;
}


static void
conflict(int type, int firstFamily, const char* first, int firstV6only,
	int secondFamily, const char* second, int secondV6only, int reusePort,
	int expected)
{
	int result = second_bind(type, firstFamily, first, firstV6only,
		secondFamily, second, secondV6only, reusePort);
	char what[120];
	char v6a[16] = "", v6b[16] = "";
	if (firstFamily == AF_INET6)
		snprintf(v6a, sizeof(v6a), " V6ONLY=%d", firstV6only);
	if (secondFamily == AF_INET6)
		snprintf(v6b, sizeof(v6b), " V6ONLY=%d", secondV6only);
	snprintf(what, sizeof(what), "%s bind %s%s then %s%s%s -> %s",
		type == SOCK_STREAM ? "tcp" : "udp", first, v6a, second, v6b,
		reusePort ? " (SO_REUSEPORT)" : "",
		expected == 0 ? "ok" : "EADDRINUSE");
	char detail[64];
	snprintf(detail, sizeof(detail), "second bind: %s",
		result == 0 ? "ok" : strerror(result));
	report(what, result == expected, detail);
}


static void
test_bind_conflicts(int type)
{
	conflict(type, AF_INET6, "::", 0, AF_INET, "0.0.0.0", -1, 0, EADDRINUSE);
	conflict(type, AF_INET, "0.0.0.0", -1, AF_INET6, "::", 0, 0, EADDRINUSE);
	conflict(type, AF_INET6, "::", 1, AF_INET, "0.0.0.0", -1, 0, 0);
	conflict(type, AF_INET, "0.0.0.0", -1, AF_INET6, "::", 1, 0, 0);
	conflict(type, AF_INET6, "::", 0, AF_INET, "127.0.0.1", -1, 0,
		EADDRINUSE);
	conflict(type, AF_INET6, "::", 0, AF_INET6, "::", 0, 0, EADDRINUSE);
		// control
	if (type == SOCK_DGRAM) {
		conflict(type, AF_INET6, "::", 0, AF_INET, "0.0.0.0", -1, 1, 0);
		conflict(type, AF_INET, "0.0.0.0", -1, AF_INET6, "::", 0, 1, 0);
	}

	// A specific IPv4 address bound first, then the wildcard: this differs
	// between systems within one family already, so the dual-stack case must
	// do what 127.0.0.1-then-0.0.0.0 does here.
	int sameFamily = second_bind(type, AF_INET, "127.0.0.1", -1, AF_INET,
		"0.0.0.0", -1, 0);
	int dualStack = second_bind(type, AF_INET, "127.0.0.1", -1, AF_INET6,
		"::", 0, 0);
	char what[120];
	snprintf(what, sizeof(what), "%s bind 127.0.0.1 then :: V6ONLY=0 "
		"= 127.0.0.1 then 0.0.0.0", type == SOCK_STREAM ? "tcp" : "udp");
	char detail[96];
	snprintf(detail, sizeof(detail), "dual-stack: %s, same family: %s",
		dualStack == 0 ? "ok" : strerror(dualStack),
		sameFamily == 0 ? "ok" : strerror(sameFamily));
	report(what, dualStack == sameFamily, detail);
}


int
main()
{
	test_option(SOCK_STREAM);
	test_option(SOCK_DGRAM);

	test_tcp_ipv4_client_to_wildcard(0);
	test_tcp_ipv4_client_to_wildcard(1);
	test_tcp_default_listener();
	test_tcp_ipv4_client_to_loopback6();
	test_tcp_ipv6_client_to_mapped(0);
	test_tcp_ipv6_client_to_mapped(1);
	test_tcp_bind_mapped();

	test_udp_ipv4_to_wildcard(0);
	test_udp_ipv4_to_wildcard(1);
	test_udp_ipv6_to_mapped(0);
	test_udp_ipv6_to_mapped(1);

	test_bind_conflicts(SOCK_STREAM);
	test_bind_conflicts(SOCK_DGRAM);

	printf("ipv6_873: %d failed\n", sFailed);
	return sFailed;
}
