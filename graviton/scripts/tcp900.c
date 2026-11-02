/*
 * tcp900.c -- issue 900: TCP over ::1 delivered nothing once a segment filled
 * the loopback MTU (a 1 MiB write, a 64 MiB stream), while 127.0.0.1 worked.
 *
 * For every target address this runs, with a hard per-transfer deadline:
 *   - the connection's TCP_MAXSEG
 *   - single writes of MSS-13, MSS-12 and MSS bytes, with TCP_NODELAY. A data
 *     segment carries 12 bytes of timestamp option, so it holds MSS-12 bytes:
 *     the first write fills a packet of the path MTU less one byte, the other
 *     two fill it exactly (65535 and 65536 bytes over ::1, whose MSS is
 *     65476)
 *   - 1 KiB, 64 KiB, 1 MiB and 64 MiB transfers, in writes of up to 1 MiB,
 *     server-to-client and client-to-server, and 64 MiB in 64 KiB writes;
 *     each one checks every byte, then a FIN, and reports MiB/s
 * and, below TCP, UDP datagrams over ::1 and 127.0.0.1 whose IP packets are
 * 65535 and 65536 bytes long.
 *
 * Both ends of a connection are driven from one non-blocking poll() loop, so a
 * stall cannot wedge a thread: it ends at the deadline and is reported.
 *
 * Targets: ::1, ::ffff:127.0.0.1 (an IPv6 client of a :: listener with
 * IPV6_V6ONLY off), 127.0.0.1, then any extra addresses given as arguments
 * (the host's own addresses). Exit status: the number of FAILs. Plain POSIX:
 * builds and runs on Linux, which is the reference.
 *
 *   Haiku: gcc -O2 -Wall -o tcp900 tcp900.c -lnetwork
 *   Linux: gcc -O2 -Wall -o tcp900 tcp900.c
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>


static const double kDeadline = 10.0;
	// seconds per transfer; 64 MiB takes well under one on either system
static int sFailed = 0;


static double
now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}


static uint8_t
pattern(uint64_t offset)
{
	// position-dependent, so a lost, repeated or reordered chunk is caught
	return (uint8_t)(offset * 131 + (offset >> 13));
}


struct target {
	const char*	label;
	int			family;
	const char*	listen_on;
	const char*	connect_to;
	int			v6only_off;
};


static socklen_t
make_address(int family, const char* text, int port,
	struct sockaddr_storage* address)
{
	memset(address, 0, sizeof(*address));
	if (family == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)address;
		in->sin_family = AF_INET;
		in->sin_port = htons(port);
		if (inet_pton(AF_INET, text, &in->sin_addr) != 1)
			return 0;
		return sizeof(*in);
	}

	struct sockaddr_in6* in6 = (struct sockaddr_in6*)address;
	in6->sin6_family = AF_INET6;
	in6->sin6_port = htons(port);
	if (inet_pton(AF_INET6, text, &in6->sin6_addr) != 1)
		return 0;
	return sizeof(*in6);
}


static int
set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


/*
 * Make a connected pair: *_server is the accepted socket, *_client the
 * connecting one, both non-blocking. Returns 0, or -1 with a reason in why.
 */
static int
make_pair(const struct target* t, int noDelay, int* _server, int* _client,
	char* why, size_t whySize)
{
	struct sockaddr_storage address;
	socklen_t length = make_address(t->family, t->listen_on, 0, &address);
	int listener = -1, client = -1, server = -1;

	if (length == 0) {
		snprintf(why, whySize, "bad listen address %s", t->listen_on);
		return -1;
	}

	listener = socket(t->family, SOCK_STREAM, 0);
	if (listener < 0) {
		snprintf(why, whySize, "socket: %s", strerror(errno));
		return -1;
	}
	int one = 1, zero = 0;
	if (t->v6only_off && setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY,
			&zero, sizeof(zero)) != 0) {
		snprintf(why, whySize, "setsockopt(IPV6_V6ONLY, 0): %s",
			strerror(errno));
		goto fail;
	}
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(listener, (struct sockaddr*)&address, length) != 0) {
		snprintf(why, whySize, "bind %s: %s", t->listen_on, strerror(errno));
		goto fail;
	}
	if (listen(listener, 1) != 0) {
		snprintf(why, whySize, "listen: %s", strerror(errno));
		goto fail;
	}
	length = sizeof(address);
	if (getsockname(listener, (struct sockaddr*)&address, &length) != 0) {
		snprintf(why, whySize, "getsockname: %s", strerror(errno));
		goto fail;
	}
	int port = t->family == AF_INET
		? ntohs(((struct sockaddr_in*)&address)->sin_port)
		: ntohs(((struct sockaddr_in6*)&address)->sin6_port);

	length = make_address(t->family, t->connect_to, port, &address);
	if (length == 0) {
		snprintf(why, whySize, "bad connect address %s", t->connect_to);
		goto fail;
	}
	client = socket(t->family, SOCK_STREAM, 0);
	if (client < 0) {
		snprintf(why, whySize, "socket: %s", strerror(errno));
		goto fail;
	}
	if (set_nonblocking(client) != 0) {
		snprintf(why, whySize, "fcntl: %s", strerror(errno));
		goto fail;
	}
	if (connect(client, (struct sockaddr*)&address, length) != 0
		&& errno != EINPROGRESS) {
		snprintf(why, whySize, "connect %s: %s", t->connect_to,
			strerror(errno));
		goto fail;
	}

	struct pollfd p = { listener, POLLIN, 0 };
	int r = poll(&p, 1, 5000);
	if (r != 1) {
		snprintf(why, whySize, "no connection to accept within 5 s (%s)",
			r < 0 ? strerror(errno) : "timeout");
		goto fail;
	}
	server = accept(listener, NULL, NULL);
	if (server < 0) {
		snprintf(why, whySize, "accept: %s", strerror(errno));
		goto fail;
	}
	if (set_nonblocking(server) != 0) {
		snprintf(why, whySize, "fcntl: %s", strerror(errno));
		goto fail;
	}

	p.fd = client;
	p.events = POLLOUT;
	if (poll(&p, 1, 5000) != 1) {
		snprintf(why, whySize, "connect did not complete within 5 s");
		goto fail;
	}
	int error = 0;
	length = sizeof(error);
	if (getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &length) != 0
		|| error != 0) {
		snprintf(why, whySize, "connect: %s", strerror(error ? error : errno));
		goto fail;
	}

	if (noDelay) {
		setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	}

	close(listener);
	*_server = server;
	*_client = client;
	return 0;

fail:
	if (server >= 0)
		close(server);
	if (client >= 0)
		close(client);
	close(listener);
	return -1;
}


struct result {
	uint64_t	delivered;
	int			corrupt;
	int			eof;
	int			timed_out;
	char		error[96];
	double		elapsed;
};


/*
 * Send size bytes from sender to receiver in writes of up to chunk bytes, then
 * shut the sender down and wait for the receiver to see EOF, all within
 * kDeadline seconds.
 */
static void
transfer(int sender, int receiver, uint64_t size, size_t chunk,
	struct result* res)
{
	uint8_t* out = malloc(chunk);
	uint8_t* in = malloc(1 << 20);
	uint64_t sent = 0;
	int shut = 0;

	memset(res, 0, sizeof(*res));
	if (out == NULL || in == NULL) {
		snprintf(res->error, sizeof(res->error), "out of memory");
		free(out);
		free(in);
		return;
	}

	double start = now();
	double deadline = start + kDeadline;
	size_t pending = 0, pendingOffset = 0;
		// bytes of out[] still to write, and where they start

	while (!res->eof) {
		double left = deadline - now();
		if (left <= 0) {
			res->timed_out = 1;
			break;
		}

		if (sent == size && !shut) {
			if (shutdown(sender, SHUT_WR) != 0) {
				snprintf(res->error, sizeof(res->error), "shutdown: %s",
					strerror(errno));
				break;
			}
			shut = 1;
		}

		struct pollfd p[2] = {
			{ receiver, POLLIN, 0 },
			{ sender, (short)(shut ? 0 : POLLOUT), 0 }
		};
		int r = poll(p, shut ? 1 : 2, (int)(left * 1000) + 1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			snprintf(res->error, sizeof(res->error), "poll: %s",
				strerror(errno));
			break;
		}

		if (!shut && (p[1].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
			if (pending == 0) {
				size_t n = size - sent < chunk ? (size_t)(size - sent) : chunk;
				for (size_t i = 0; i < n; i++)
					out[i] = pattern(sent + i);
				pending = n;
				pendingOffset = 0;
			}
			ssize_t w = send(sender, out + pendingOffset, pending, 0);
			if (w > 0) {
				pending -= w;
				pendingOffset += w;
				sent += w;
			} else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK
				&& errno != EINTR) {
				snprintf(res->error, sizeof(res->error), "send: %s",
					strerror(errno));
				break;
			}
		}

		if ((p[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
			ssize_t n = recv(receiver, in, 1 << 20, 0);
			if (n > 0) {
				for (ssize_t i = 0; i < n && !res->corrupt; i++) {
					if (in[i] != pattern(res->delivered + i))
						res->corrupt = 1;
				}
				res->delivered += n;
				if (res->delivered > size)
					res->corrupt = 1;
			} else if (n == 0) {
				res->eof = 1;
			} else if (errno != EAGAIN && errno != EWOULDBLOCK
				&& errno != EINTR) {
				snprintf(res->error, sizeof(res->error), "recv: %s",
					strerror(errno));
				break;
			}
		}
	}

	res->elapsed = now() - start;
	free(out);
	free(in);
}


static const char*
size_text(uint64_t size)
{
	static char buffers[4][24];
	static int next = 0;
	char* buffer = buffers[next++ % 4];
	if (size >= (1 << 20) && size % (1 << 20) == 0)
		snprintf(buffer, 24, "%lluMiB", (unsigned long long)(size >> 20));
	else if (size >= 1024 && size % 1024 == 0)
		snprintf(buffer, 24, "%lluKiB", (unsigned long long)(size >> 10));
	else
		snprintf(buffer, 24, "%llu", (unsigned long long)size);
	return buffer;
}


static void
run_transfer(const struct target* t, const char* direction, uint64_t size,
	size_t chunk, int noDelay)
{
	char what[128];
	snprintf(what, sizeof(what), "tcp %-17s %s %8s in %7s writes",
		t->label, direction, size_text(size), size_text(chunk));

	int server, client;
	char why[160];
	if (make_pair(t, noDelay, &server, &client, why, sizeof(why)) != 0) {
		printf("%-58s FAIL  setup: %s\n", what, why);
		sFailed++;
		return;
	}

	struct result res;
	if (strcmp(direction, "s2c") == 0)
		transfer(server, client, size, chunk, &res);
	else
		transfer(client, server, size, chunk, &res);
	close(server);
	close(client);

	int ok = res.delivered == size && !res.corrupt && res.eof
		&& !res.timed_out && res.error[0] == '\0';
	printf("%-58s %s  delivered %llu/%llu in %.3fs, %.1f MiB/s%s%s%s%s%s\n",
		what, ok ? "PASS" : "FAIL",
		(unsigned long long)res.delivered, (unsigned long long)size,
		res.elapsed, res.delivered / 1048576.0 / (res.elapsed > 0
			? res.elapsed : 1e-9),
		res.corrupt ? ", CORRUPT" : "", res.eof ? ", eof" : ", no eof",
		res.timed_out ? ", DEADLINE" : "", res.error[0] ? ", " : "",
		res.error);
	fflush(stdout);
	if (!ok)
		sFailed++;
}


static void
run_target(const struct target* t)
{
	static const uint64_t kSizes[] = { 1024, 64 << 10, 1 << 20, 64 << 20 };

	// The MSS this path negotiates, and single writes on either side of it.
	int server, client;
	char why[160];
	if (make_pair(t, 1, &server, &client, why, sizeof(why)) != 0) {
		printf("tcp %-17s FAIL  setup: %s\n", t->label, why);
		sFailed++;
		return;
	}
	int mss = 0;
	socklen_t length = sizeof(mss);
	if (getsockopt(server, IPPROTO_TCP, TCP_MAXSEG, &mss, &length) != 0) {
		printf("tcp %-17s TCP_MAXSEG: %s\n", t->label, strerror(errno));
		mss = 0;
	} else
		printf("tcp %-17s TCP_MAXSEG %d\n", t->label, mss);
	close(server);
	close(client);

	if (mss > 13) {
		static const int kDeltas[] = { -13, -12, 0 };
		for (int i = 0; i < 3; i++)
			run_transfer(t, "s2c", mss + kDeltas[i], mss + kDeltas[i], 1);
	}

	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
		size_t chunk = kSizes[i] < (1 << 20) ? kSizes[i] : (1 << 20);
		run_transfer(t, "s2c", kSizes[i], chunk, 0);
		run_transfer(t, "c2s", kSizes[i], chunk, 0);
	}
	run_transfer(t, "s2c", 64 << 20, 64 << 10, 0);
	run_transfer(t, "c2s", 64 << 20, 64 << 10, 0);
}


/*
 * One UDP datagram over a loopback address whose IP packet is ipLength bytes.
 * Reported, not counted: it shows, below TCP, whether IPv6 sends and then
 * delivers a 65536-byte packet (legal: the length field counts only the
 * payload), which is what every full-sized TCP segment over ::1 is.
 */
static void
run_udp(int family, const char* address, size_t ipLength)
{
	size_t headers = (family == AF_INET6 ? 40 : 20) + 8;
	size_t size = ipLength - headers;
	char what[96];
	snprintf(what, sizeof(what), "udp %-17s %zu-byte IP packet (%zu payload)",
		address, ipLength, size);

	struct sockaddr_storage to;
	socklen_t length = make_address(family, address, 0, &to);
	int receiver = socket(family, SOCK_DGRAM, 0);
	int sender = socket(family, SOCK_DGRAM, 0);
	int buffer = 1 << 20;
	char* data = calloc(1, size + 1);
	if (receiver < 0 || sender < 0 || data == NULL) {
		printf("%-58s ERROR socket: %s\n", what, strerror(errno));
		goto out;
	}
	setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
	setsockopt(sender, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
	if (bind(receiver, (struct sockaddr*)&to, length) != 0
		|| getsockname(receiver, (struct sockaddr*)&to, &length) != 0) {
		printf("%-58s ERROR bind: %s\n", what, strerror(errno));
		goto out;
	}
	ssize_t sent = sendto(sender, data, size, 0, (struct sockaddr*)&to, length);
	if (sent < 0) {
		printf("%-58s refused by sendto: %s\n", what, strerror(errno));
		goto out;
	}
	struct pollfd p = { receiver, POLLIN, 0 };
	if (poll(&p, 1, 2000) != 1) {
		printf("%-58s sent %zd, NOT DELIVERED within 2 s\n", what, sent);
		goto out;
	}
	ssize_t got = recv(receiver, data, size + 1, 0);
	printf("%-58s sent %zd, delivered %zd\n", what, sent, got);

out:
	free(data);
	if (receiver >= 0)
		close(receiver);
	if (sender >= 0)
		close(sender);
}


int
main(int argc, char** argv)
{
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);

	struct target targets[16] = {
		{ "::1", AF_INET6, "::1", "::1", 0 },
		{ "::ffff:127.0.0.1", AF_INET6, "::", "::ffff:127.0.0.1", 1 },
		{ "127.0.0.1", AF_INET, "127.0.0.1", "127.0.0.1", 0 },
	};
	int count = 3;
	for (int i = 1; i < argc && count < 16; i++) {
		int family = strchr(argv[i], ':') != NULL ? AF_INET6 : AF_INET;
		struct target t = { argv[i], family, argv[i], argv[i], 0 };
		targets[count++] = t;
	}

	double start = now();
	for (int i = 0; i < count; i++)
		run_target(&targets[i]);

	run_udp(AF_INET6, "::1", 65535);
	run_udp(AF_INET6, "::1", 65536);
	run_udp(AF_INET, "127.0.0.1", 65535);

	printf("total %.1fs, %d FAIL\n", now() - start, sFailed);
	return sFailed;
}
