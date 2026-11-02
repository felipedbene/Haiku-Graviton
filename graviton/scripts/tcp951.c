/*
 * tcp951.c -- issue 951: a TCP segment the IP layer refuses must not stall
 * the connection forever. Two ways to make the IP layer refuse segments of
 * an established loopback connection:
 *
 *   mtu    the interface MTU is lowered under the connection, so segments
 *          sized for the old one are refused (EMSGSIZE, as IP_DONTFRAG is
 *          set). Linux re-sizes its segments and carries on.
 *   down   the interface is taken down for 2 s, so every segment is refused,
 *          then brought back up. The connection must recover by itself.
 *
 * Usage: tcp951 <address> <mtu|down> <lower-cmd> <restore-cmd>
 * The commands run through system(). Plain POSIX; both ends live in one
 * poll() loop, every byte is checked, every wait has a deadline. Exit status
 * is the number of FAILs. Linux 7.0 is the reference.
 *
 *   Haiku: gcc -O2 -Wall -o tcp951 tcp951.c -lnetwork
 *          tcp951 127.0.0.1 mtu "ifconfig loop mtu 1500" "ifconfig loop mtu 65536"
 *   Linux: gcc -O2 -Wall -o tcp951 tcp951.c
 *          tcp951 127.0.0.1 mtu "ip link set lo mtu 1500" "ip link set lo mtu 65536"
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static double
now()
{
	struct timeval t;
	gettimeofday(&t, NULL);
	return t.tv_sec + t.tv_usec / 1e6;
}

static unsigned char
pattern(size_t offset)
{
	return (unsigned char)((offset * 2654435761u) >> 13);
}

/* Moves `total` bytes from `out` to `in` (non-blocking), deadline `limit`
 * seconds. Returns the bytes delivered and verified. `midway` is run once,
 * after `trigger` bytes have been written. */
static size_t
transfer(int out, int in, size_t total, double limit, size_t trigger,
	const char* midway, size_t* corrupt)
{
	static unsigned char buffer[65536];
	size_t sent = 0, received = 0;
	int triggered = midway == NULL;
	double end = now() + limit;
	*corrupt = 0;

	while (received < total && now() < end) {
		struct pollfd p[2] = {
			{ in, POLLIN, 0 },
			{ out, sent < total ? POLLOUT : 0, 0 },
		};
		poll(p, 2, 100);
		if (sent < total && (p[1].revents & POLLOUT) != 0) {
			size_t chunk = total - sent;
			if (chunk > sizeof(buffer))
				chunk = sizeof(buffer);
			for (size_t i = 0; i < chunk; i++)
				buffer[i] = pattern(sent + i);
			ssize_t n = send(out, buffer, chunk, 0);
			if (n > 0)
				sent += n;
			else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				printf("  send: %s\n", strerror(errno));
				break;
			}
		}
		if (!triggered && sent >= trigger) {
			triggered = 1;
			int status = system(midway);
			printf("  [%.3f s] ran '%s' (status %d) at %zu sent, %zu received\n",
				limit - (end - now()), midway, status, sent, received);
		}
		if ((p[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
			ssize_t n = recv(in, buffer, sizeof(buffer), 0);
			if (n > 0) {
				for (ssize_t i = 0; i < n; i++) {
					if (buffer[i] != pattern(received + i))
						(*corrupt)++;
				}
				received += n;
			} else if (n == 0) {
				printf("  recv: unexpected EOF\n");
				break;
			} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
				printf("  recv: %s\n", strerror(errno));
				break;
			}
		}
	}
	return received;
}

static int
socket_error(int s)
{
	int error = 0;
	socklen_t length = sizeof(error);
	getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &length);
	return error;
}

int
main(int argc, char** argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s <address> <mtu|down> <lower> <restore>\n",
			argv[0]);
		return 100;
	}
	const char* host = argv[1];
	const char* mode = argv[2];
	int failures = 0;

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	inet_pton(AF_INET, host, &address.sin_addr);
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	socklen_t length = sizeof(address);
	if (bind(listener, (struct sockaddr*)&address, length) != 0
		|| listen(listener, 1) != 0
		|| getsockname(listener, (struct sockaddr*)&address, &length) != 0) {
		printf("setup: %s\n", strerror(errno));
		return 100;
	}
	int client = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(client, (struct sockaddr*)&address, length) != 0) {
		printf("connect: %s\n", strerror(errno));
		return 100;
	}
	int server = accept(listener, NULL, NULL);
	fcntl(client, F_SETFL, O_NONBLOCK);
	fcntl(server, F_SETFL, O_NONBLOCK);

	int maxSegment = 0;
	socklen_t optionLength = sizeof(maxSegment);
	getsockopt(server, IPPROTO_TCP, TCP_MAXSEG, &maxSegment, &optionLength);
	printf("%s %s: TCP_MAXSEG %d\n", host, mode, maxSegment);

	size_t corrupt;
	const size_t warm = 1 << 20;
	double start = now();
	size_t got = transfer(server, client, warm, 10, 0, NULL, &corrupt);
	printf("%-36s %s  %zu/%zu in %.3f s, corrupt %zu\n", "warm-up 1 MiB",
		got == warm && corrupt == 0 ? "PASS" : "FAIL", got, warm,
		now() - start, corrupt);
	failures += got != warm || corrupt != 0;

	const size_t total = 8 << 20;
	start = now();
	got = transfer(server, client, total, 30, 256 * 1024, argv[3], &corrupt);
	double elapsed = now() - start;
	char name[64];
	snprintf(name, sizeof(name), "8 MiB across '%s'", mode);
	printf("%-36s %s  %zu/%zu in %.3f s, corrupt %zu, SO_ERROR %d/%d\n", name,
		got == total && corrupt == 0 ? "PASS" : "FAIL", got, total, elapsed,
		corrupt, socket_error(server), socket_error(client));
	failures += got != total || corrupt != 0;

	optionLength = sizeof(maxSegment);
	getsockopt(server, IPPROTO_TCP, TCP_MAXSEG, &maxSegment, &optionLength);
	printf("  after: sender TCP_MAXSEG %d\n", maxSegment);

	int status = system(argv[4]);
	printf("  ran '%s' (status %d)\n", argv[4], status);

	/* A fresh connection after the restore must work as well. */
	start = now();
	int client2 = socket(AF_INET, SOCK_STREAM, 0);
	int ok = connect(client2, (struct sockaddr*)&address, length) == 0;
	int server2 = ok ? accept(listener, NULL, NULL) : -1;
	got = 0;
	if (server2 >= 0) {
		fcntl(client2, F_SETFL, O_NONBLOCK);
		fcntl(server2, F_SETFL, O_NONBLOCK);
		got = transfer(server2, client2, warm, 10, 0, NULL, &corrupt);
	}
	printf("%-36s %s  %zu/%zu in %.3f s\n", "fresh connection 1 MiB",
		got == warm && corrupt == 0 ? "PASS" : "FAIL", got, warm,
		now() - start);
	failures += got != warm || corrupt != 0;

	printf("FAILS %d\n", failures);
	return failures;
}
