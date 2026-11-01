/*
 * tcp871.c -- issue #871: does an ACCEPTED TCP socket become readable through
 * poll()/select() once its connection has ended?
 *
 * Every case runs over loopback, for IPv4 and IPv6, with the client in the same
 * process. In each one the server-side (accepted) socket is waited on by a
 * thread that is ALREADY blocked in poll() -- or select() -- before the client
 * acts, so it is the kernel's wakeup that is tested (socket_notify()), and then
 * once more with a zero-timeout poll() after the fact, which tests the
 * availability check made when the wait is registered
 * (socket_request_notification()). Both answers come from the same
 * _AvailableData() in the tcp add-on.
 *
 *   srvfin     the #871 case. The server writes "hi" and shuts down its
 *              sending side FIRST; the client reads the data and the EOF and
 *              closes. The server ends in TIME_WAIT (FINISH_ACKNOWLEDGED, then
 *              the peer's FIN). Expect POLLIN and read() == 0.
 *   srvfin-sel the same, waiting in select() instead of poll().
 *   peerfin    the client closes first (server in CLOSE_WAIT). Expect POLLIN
 *              and read() == 0. This one worked before the fix: it is the
 *              control.
 *   simul      both sides shut down their sending side back to back, before
 *              either reads (CLOSING if the FINs cross). Expect POLLIN and
 *              read() == 0.
 *   reset      the client aborts with SO_LINGER {1, 0} (a RST) while the
 *              server is ESTABLISHED. Expect POLLIN and read() ==
 *              -1/ECONNRESET.
 *   srvfin-rst the server shuts down its sending side first, then the client
 *              aborts with a RST (server in FINISH_ACKNOWLEDGED). Expect
 *              POLLIN and read() == -1/ECONNRESET.
 *
 * After the read, the srvfin case also reports what a zero-timeout
 * poll(POLLOUT) answers on the finished connection (informational only).
 *
 * Usage: tcp871 [timeout-ms]   (default 2000; a FAIL is a wait that timed out
 *                              or a read that returned the wrong result)
 * Exit status: the number of failed cases.
 *
 * Build: gcc -O2 -Wall -o tcp871 tcp871.c -lnetwork   (Haiku; drop -lnetwork
 *        elsewhere). Plain POSIX: Linux (7.0, Ubuntu 24.04) and macOS pass
 *        every case.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


static int sTimeout = 2000;


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}


struct waiter {
	int			fd;
	int			useSelect;
	pthread_t	thread;
	int			result;
	short		revents;
	double		woke;
};


static void*
wait_thread(void* _w)
{
	struct waiter* w = (struct waiter*)_w;
	if (w->useSelect) {
		fd_set set;
		struct timeval tv;
		FD_ZERO(&set);
		FD_SET(w->fd, &set);
		tv.tv_sec = sTimeout / 1000;
		tv.tv_usec = (sTimeout % 1000) * 1000;
		w->result = select(w->fd + 1, &set, NULL, NULL, &tv);
		w->revents = (w->result > 0 && FD_ISSET(w->fd, &set)) ? POLLIN : 0;
	} else {
		struct pollfd p = { w->fd, POLLIN, 0 };
		w->result = poll(&p, 1, sTimeout);
		w->revents = p.revents;
	}
	w->woke = now_ms();
	return NULL;
}


static void
start_wait(struct waiter* w, int fd, int useSelect)
{
	memset(w, 0, sizeof(*w));
	w->fd = fd;
	w->useSelect = useSelect;
	pthread_create(&w->thread, NULL, wait_thread, w);
	// let it get into poll()/select() before the client acts
	usleep(100 * 1000);
}


static int
make_pair(int family, int* _server, int* _client)
{
	struct sockaddr_storage address;
	socklen_t length;
	memset(&address, 0, sizeof(address));
	if (family == AF_INET) {
		struct sockaddr_in* in = (struct sockaddr_in*)&address;
		in->sin_family = AF_INET;
		in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		length = sizeof(*in);
	} else {
		struct sockaddr_in6* in6 = (struct sockaddr_in6*)&address;
		in6->sin6_family = AF_INET6;
		in6->sin6_addr = in6addr_loopback;
		length = sizeof(*in6);
	}

	int listener = socket(family, SOCK_STREAM, 0);
	if (listener < 0 || bind(listener, (struct sockaddr*)&address, length) != 0
		|| listen(listener, 4) != 0
		|| getsockname(listener, (struct sockaddr*)&address, &length) != 0) {
		perror("listener");
		return -1;
	}
	int client = socket(family, SOCK_STREAM, 0);
	if (client < 0
		|| connect(client, (struct sockaddr*)&address, length) != 0) {
		perror("connect");
		return -1;
	}
	int server = accept(listener, NULL, NULL);
	if (server < 0) {
		perror("accept");
		return -1;
	}
	close(listener);
	*_server = server;
	*_client = client;
	return 0;
}


static void
abort_socket(int fd)
{
	struct linger linger = { 1, 0 };
	setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
	close(fd);
}


static void
drain_to_eof(int fd)
{
	char buffer[64];
	while (read(fd, buffer, sizeof(buffer)) > 0)
		;
}


/*! Runs one case; returns 0 on PASS, 1 on FAIL. */
static int
run_case(int family, const char* name)
{
	const char* familyName = family == AF_INET ? "ipv4" : "ipv6";
	int server, client;
	if (make_pair(family, &server, &client) != 0) {
		printf("%s %-10s FAIL setup\n", familyName, name);
		return 1;
	}

	int expectReset = 0;
	int useSelect = strcmp(name, "srvfin-sel") == 0;
	struct waiter w;
	double acted;

	if (strncmp(name, "srvfin", 6) == 0 && strcmp(name, "srvfin-rst") != 0) {
		if (write(server, "hi", 2) != 2)
			perror("write");
		shutdown(server, SHUT_WR);
		// the server has nothing to read yet: wait for the client's FIN
		start_wait(&w, server, useSelect);
		acted = now_ms();
		drain_to_eof(client);
		close(client);
	} else if (strcmp(name, "peerfin") == 0) {
		start_wait(&w, server, 0);
		acted = now_ms();
		close(client);
	} else if (strcmp(name, "simul") == 0) {
		start_wait(&w, server, 0);
		acted = now_ms();
		shutdown(server, SHUT_WR);
		shutdown(client, SHUT_WR);
		usleep(50 * 1000);
		close(client);
	} else if (strcmp(name, "reset") == 0) {
		expectReset = 1;
		start_wait(&w, server, 0);
		acted = now_ms();
		abort_socket(client);
	} else {
		// srvfin-rst
		expectReset = 1;
		shutdown(server, SHUT_WR);
		usleep(50 * 1000);
			// let the FIN be acknowledged: FINISH_ACKNOWLEDGED
		start_wait(&w, server, 0);
		acted = now_ms();
		abort_socket(client);
	}

	pthread_join(w.thread, NULL);
	double wakeMs = w.woke - acted;

	// after the fact: what does a fresh, zero-timeout poll() see?
	struct pollfd p = { server, POLLIN, 0 };
	int late = poll(&p, 1, 0);
	short lateRevents = p.revents;

	char buffer[16];
	errno = 0;
	ssize_t bytes = read(server, buffer, sizeof(buffer));
	int readErrno = errno;

	// srvfin: the server's own "hi" never comes back, so read() is the EOF
	int readOk = expectReset ? (bytes < 0 && readErrno == ECONNRESET)
		: bytes == 0;
	int waitOk = w.result > 0 && (w.revents & (POLLIN | POLLHUP | POLLERR));
	int lateOk = late > 0;

	printf("%s %-10s %s  wait=%d revents=0x%x wake=%.1fms  "
		"late-poll=%d revents=0x%x  read=%zd errno=%s",
		familyName, name, waitOk && lateOk && readOk ? "PASS" : "FAIL",
		w.result, w.revents, wakeMs, late, lateRevents, bytes,
		bytes < 0 ? strerror(readErrno) : "-");

	if (strcmp(name, "srvfin") == 0) {
		struct pollfd q = { server, POLLOUT, 0 };
		int out = poll(&q, 1, 0);
		printf("  pollout=%d revents=0x%x", out, q.revents);
	}
	printf("\n");

	close(server);
	return waitOk && lateOk && readOk ? 0 : 1;
}


int
main(int argc, char** argv)
{
	if (argc > 1)
		sTimeout = atoi(argv[1]);

	static const char* kCases[] = {
		"srvfin", "srvfin-sel", "peerfin", "simul", "reset", "srvfin-rst"
	};
	static const int kFamilies[] = { AF_INET, AF_INET6 };

	int failed = 0;
	for (size_t f = 0; f < sizeof(kFamilies) / sizeof(kFamilies[0]); f++) {
		for (size_t c = 0; c < sizeof(kCases) / sizeof(kCases[0]); c++)
			failed += run_case(kFamilies[f], kCases[c]);
	}
	printf("tcp871: %d failed\n", failed);
	return failed;
}
