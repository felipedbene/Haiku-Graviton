/*
 * tcp975.c -- issue 975: an established TCP connection whose peer stops
 * answering has to give up retransmitting (R2, RFC 9293 3.8.3) and report
 * ETIMEDOUT to whoever is blocked on it, rather than retransmit forever.
 *
 * Talks to tcp975-peer.py, which runs as root on a Linux host and black-holes
 * one connection on request with iptables (both directions, matched on the
 * client's port, so the control connection and every other test keep
 * working). Plain POSIX; builds on Haiku (-lnetwork) and Linux alike.
 *
 * Usage: tcp975 <peer-ip> <data-port> <control-port> <mode> [cap-seconds]
 *
 * Modes (one connection each; run several at once to share a black-hole
 * window):
 *   write     keep a blocking send() busy, black-hole the connection, and time
 *             how long until send() fails.
 *   read      black-hole, send 1000 bytes, then time a blocking recv().
 *   poll      black-hole, send 1000 bytes, then time poll(POLLIN) and report
 *             revents and SO_ERROR.
 *   close     black-hole, send 1000 bytes and close() at once: the endpoint
 *             outlives the descriptor. Nothing to time; exercises the teardown
 *             of a connection the application has already let go of.
 *   recover   two black-holes of 500 s each with traffic in between: 1000 s
 *             without an answer in total, but never 924.6 s in a row, so the
 *             connection must survive both and carry data afterwards.
 *   zerowin   the peer stops reading for 1000 s: the window closes, the peer
 *             keeps answering the probes, and the connection must survive and
 *             deliver everything once it reads again.
 *   reopen    (issue 1044) the peer stops reading for 30 s, then reads again
 *             while dropping its own segments to us for 10 s, so the window
 *             update that reopens the window is lost. Only a window probe
 *             the peer answers can find the window open again: time how long
 *             the 64 MiB take after that, capped.
 *   zerodead  (issue 1044) the peer stops reading, and once the window is
 *             closed it is black-holed: a peer gone while its window is
 *             closed. Time how long the blocked send() takes to fail.
 *
 * Every wait is bounded (cap, default 1200 s): a stack that never gives up is
 * reported as "no error after N s" instead of hanging the run.
 * Output is one RESULT line per mode; exit status 0 means the mode behaved as
 * an R2-implementing stack must.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char* sPeer;
static int sDataPort;
static int sControlPort;
static double sCap = 1200;
static const char* sMode;

static double
now()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void
note(const char* format, const char* detail)
{
	time_t t = time(NULL);
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%H:%M:%SZ", gmtime(&t));
	printf("  [%s %s] %s%s\n", stamp, sMode, format, detail != NULL ? detail : "");
	fflush(stdout);
}

static int
connect_to(int port)
{
	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	inet_pton(AF_INET, sPeer, &address.sin_addr);

	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0 || connect(s, (struct sockaddr*)&address, sizeof(address)) != 0) {
		printf("RESULT %s FAIL connect to %d: %s\n", sMode, port,
			strerror(errno));
		exit(2);
	}
	return s;
}

static int
local_port(int s)
{
	struct sockaddr_in address;
	socklen_t length = sizeof(address);
	getsockname(s, (struct sockaddr*)&address, &length);
	return ntohs(address.sin_port);
}

static int
socket_error(int s)
{
	int error = 0;
	socklen_t length = sizeof(error);
	getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &length);
	return error;
}

/* One line to the peer's control port, one line back. */
static void
control(int c, const char* command, char* reply, size_t replySize)
{
	char line[256];
	snprintf(line, sizeof(line), "%s\n", command);
	if (send(c, line, strlen(line), 0) < 0) {
		printf("RESULT %s FAIL control send: %s\n", sMode, strerror(errno));
		exit(2);
	}
	size_t got = 0;
	while (got + 1 < replySize) {
		ssize_t n = recv(c, reply + got, 1, 0);
		if (n <= 0) {
			printf("RESULT %s FAIL control reply: %s\n", sMode,
				n == 0 ? "EOF" : strerror(errno));
			exit(2);
		}
		if (reply[got] == '\n')
			break;
		got++;
	}
	reply[got] = '\0';
	note("control: ", command);
}

static long
peer_count(int c, int port)
{
	char command[64], reply[128];
	snprintf(command, sizeof(command), "COUNT %d", port);
	control(c, command, reply, sizeof(reply));
	return atol(reply + 3);
		/* "OK <bytes>" */
}

/* The cap. Deliberately not SO_SNDTIMEO/SO_RCVTIMEO: on Haiku their expiry
   and the error under test could both read as ETIMEDOUT (B_TIMED_OUT), and
   the calls under test are meant to block with no timeout of their own.
   SIGALRM without SA_RESTART turns the cap into EINTR instead -- or, for a
   send() that had already copied some of its buffer, into a short count, so
   the write loop checks sCapReached as well. */
static volatile sig_atomic_t sCapReached;

static void
on_alarm(int signal)
{
	(void)signal;
	sCapReached = 1;
}

static void
arm_cap(double seconds)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = on_alarm;
	sigemptyset(&action.sa_mask);
	sigaction(SIGALRM, &action, NULL);
	alarm((unsigned)seconds);
}

/* Sends `total` bytes with blocking send()s, returns 0 or the errno. */
static int
send_all(int s, size_t total)
{
	static char buffer[65536];
	size_t sent = 0;
	while (sent < total) {
		size_t chunk = total - sent;
		if (chunk > sizeof(buffer))
			chunk = sizeof(buffer);
		ssize_t n = send(s, buffer, chunk, 0);
		if (n > 0) {
			sent += n;
			/* A cap that cut a send() short must still end the loop. */
			if (sCapReached)
				return EINTR;
			continue;
		}
		return n < 0 ? errno : EIO;
	}
	return 0;
}

static int
report_error(double start, int error, const char* call)
{
	double elapsed = now() - start;
	if (error == ETIMEDOUT) {
		printf("RESULT %s PASS %s failed with ETIMEDOUT after %.1f s\n", sMode,
			call, elapsed);
		return 0;
	}
	if (error == EINTR) {
		printf("RESULT %s FAIL no error after %.0f s: %s still blocked at the "
			"cap\n", sMode, elapsed, call);
		return 1;
	}
	printf("RESULT %s FAIL %s: %s after %.1f s\n", sMode, call,
		error == 0 ? "no error" : strerror(error), elapsed);
	return 1;
}

static int
mode_write(int s, int c)
{
	char command[64], reply[128];
	/* Get the connection going first, so the black-hole lands on a flow
	   with data in flight. */
	send_all(s, 4 << 20);
	snprintf(command, sizeof(command), "DROP %d", local_port(s));
	control(c, command, reply, sizeof(reply));
	double start = now();
	arm_cap(sCap);

	static char buffer[65536];
	for (;;) {
		ssize_t n = send(s, buffer, sizeof(buffer), 0);
		if (n > 0 && !sCapReached)
			continue;
		return report_error(start, n < 0 ? errno : sCapReached ? EINTR : EIO,
			"send()");
	}
}

static int
drop_and_send_request(int s, int c)
{
	char command[64], reply[128];
	snprintf(command, sizeof(command), "DROP %d", local_port(s));
	control(c, command, reply, sizeof(reply));
	char request[1000];
	memset(request, 'r', sizeof(request));
	if (send(s, request, sizeof(request), 0) != (ssize_t)sizeof(request)) {
		printf("RESULT %s FAIL request send: %s\n", sMode, strerror(errno));
		exit(2);
	}
	return 0;
}

static int
mode_read(int s, int c)
{
	drop_and_send_request(s, c);
	double start = now();
	arm_cap(sCap);
	char buffer[4096];
	for (;;) {
		ssize_t n = recv(s, buffer, sizeof(buffer), 0);
		if (n > 0)
			continue;
		if (n == 0) {
			printf("RESULT %s FAIL recv() returned EOF after %.1f s\n", sMode,
				now() - start);
			return 1;
		}
		return report_error(start, errno, "recv()");
	}
}

static int
mode_poll(int s, int c)
{
	drop_and_send_request(s, c);
	double start = now();
	arm_cap(sCap);
	for (;;) {
		struct pollfd p = { s, POLLIN, 0 };
		int n = poll(&p, 1, -1);
		if (n < 0) {
			printf("RESULT %s FAIL no event after %.0f s: poll() %s (SO_ERROR "
				"%d)\n", sMode, now() - start, strerror(errno),
				socket_error(s));
			return 1;
		}
		double elapsed = now() - start;
		int error = socket_error(s);
		char buffer[16];
		ssize_t r = recv(s, buffer, sizeof(buffer), MSG_DONTWAIT);
		int recvError = r < 0 ? errno : 0;
		printf("  poll revents 0x%x%s%s%s, SO_ERROR %d (%s), then recv() %zd "
			"(%s)\n", p.revents, (p.revents & POLLIN) ? " IN" : "",
			(p.revents & POLLERR) ? " ERR" : "",
			(p.revents & POLLHUP) ? " HUP" : "", error, strerror(error), r,
			strerror(recvError));
		/* The error is reported once: by SO_ERROR here, which consumes it. */
		if (error == ETIMEDOUT) {
			printf("RESULT %s PASS poll() woke after %.1f s with SO_ERROR "
				"ETIMEDOUT\n", sMode, elapsed);
			return 0;
		}
		printf("RESULT %s FAIL poll() woke after %.1f s, SO_ERROR %s\n", sMode,
			elapsed, strerror(error));
		return 1;
	}
}

static int
mode_close(int s, int c)
{
	drop_and_send_request(s, c);
	int result = close(s);
	printf("RESULT %s PASS close() of a black-holed connection returned %d\n",
		sMode, result);
	return 0;
}

static int
traffic_check(int s, int c, const char* when)
{
	long before = peer_count(c, local_port(s));
	int error = send_all(s, 1 << 20);
	if (error != 0) {
		printf("RESULT %s FAIL send %s: %s\n", sMode, when, strerror(error));
		exit(1);
	}
	for (int i = 0; i < 120; i++) {
		if (peer_count(c, local_port(s)) - before >= (1 << 20))
			return 0;
		sleep(1);
	}
	printf("RESULT %s FAIL 1 MiB %s not delivered in 120 s\n", sMode, when);
	exit(1);
}

static int
mode_recover(int s, int c)
{
	char command[64], reply[128];
	int port = local_port(s);
	traffic_check(s, c, "before");
	double start = now();
	for (int episode = 1; episode <= 2; episode++) {
		snprintf(command, sizeof(command), "DROP %d", port);
		control(c, command, reply, sizeof(reply));
		/* Data in flight while the peer is unreachable: 64 KiB. */
		int error = send_all(s, 64 << 10);
		if (error != 0) {
			printf("RESULT %s FAIL send during black-hole %d: %s\n", sMode,
				episode, strerror(error));
			return 1;
		}
		sleep(500);
		snprintf(command, sizeof(command), "UNDROP %d", port);
		control(c, command, reply, sizeof(reply));
		traffic_check(s, c, "after a black-hole");
		if (socket_error(s) != 0) {
			printf("RESULT %s FAIL SO_ERROR after black-hole %d\n", sMode,
				episode);
			return 1;
		}
	}
	printf("RESULT %s PASS survived 2 x 500 s black-holes (%.0f s in all), "
		"data delivered after each\n", sMode, now() - start);
	return 0;
}

static int
mode_zerowin(int s, int c)
{
	char command[64], reply[128];
	int port = local_port(s);
	snprintf(command, sizeof(command), "STOPREAD %d 1000", port);
	control(c, command, reply, sizeof(reply));
	double start = now();
	/* Far more than both buffers hold, so the writer blocks on a closed
	   window for the whole pause. */
	const size_t total = 64 << 20;
	arm_cap(sCap + 300);
	int error = send_all(s, total);
	alarm(0);
	if (error != 0) {
		printf("RESULT %s FAIL send() on a closed window: %s after %.1f s\n",
			sMode, strerror(error), now() - start);
		return 1;
	}
	for (int i = 0; i < 120; i++) {
		if (peer_count(c, port) >= (long)total) {
			printf("RESULT %s PASS 64 MiB delivered across a %.0f s closed "
				"window, SO_ERROR %d\n", sMode, now() - start, socket_error(s));
			return 0;
		}
		sleep(1);
	}
	printf("RESULT %s FAIL 64 MiB not all delivered\n", sMode);
	return 1;
}

static int
mode_reopen(int s, int c)
{
	char command[64], reply[128];
	int port = local_port(s);
	snprintf(command, sizeof(command), "SILENTREOPEN %d 30 10", port);
	control(c, command, reply, sizeof(reply));
	double start = now();
	const size_t total = 64 << 20;
	/* The peer reads again 30 s in, and can be heard again at 40 s. */
	arm_cap(40 + sCap);
	int error = send_all(s, total);
	alarm(0);
	if (error != 0) {
		printf("RESULT %s FAIL send(): %s after %.1f s (%s)\n", sMode,
			strerror(error), now() - start,
			error == EINTR ? "the window never reopened" : "error");
		return 1;
	}
	for (int i = 0; i < 120; i++) {
		long count = peer_count(c, port);
		if (count >= (long)total) {
			printf("RESULT %s PASS 64 MiB delivered %.1f s after the peer "
				"could be heard again (%.1f s in all)\n", sMode,
				now() - start - 40, now() - start);
			return 0;
		}
		sleep(1);
	}
	printf("RESULT %s FAIL 64 MiB not all delivered\n", sMode);
	return 1;
}

static int
mode_zerodead(int s, int c)
{
	char command[64], reply[128];
	int port = local_port(s);
	snprintf(command, sizeof(command), "STOPREAD %d 100000", port);
	control(c, command, reply, sizeof(reply));
	/* Fill both buffers, so the window is closed, then lose the peer. */
	struct pollfd p = { s, POLLOUT, 0 };
	static char buffer[65536];
	double filling = now();
	while (now() - filling < 20) {
		if (poll(&p, 1, 1000) == 1 && (p.revents & POLLOUT) != 0)
			send(s, buffer, sizeof(buffer), MSG_DONTWAIT);
	}
	snprintf(command, sizeof(command), "DROP %d", port);
	control(c, command, reply, sizeof(reply));
	double start = now();
	arm_cap(sCap);
	for (;;) {
		ssize_t n = send(s, buffer, sizeof(buffer), 0);
		if (n > 0 && !sCapReached)
			continue;
		return report_error(start, n < 0 ? errno : sCapReached ? EINTR : EIO,
			"send() on a closed window");
	}
}

int
main(int argc, char** argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s <peer-ip> <data-port> <control-port> "
			"<write|read|poll|close|recover|zerowin|reopen|zerodead> "
			"[cap-seconds]\n", argv[0]);
		return 100;
	}
	sPeer = argv[1];
	sDataPort = atoi(argv[2]);
	sControlPort = atoi(argv[3]);
	sMode = argv[4];
	if (argc > 5)
		sCap = atof(argv[5]);

	int c = connect_to(sControlPort);
	int s = connect_to(sDataPort);
	note("connected from port ", NULL);
	printf("  local port %d\n", local_port(s));

	if (strcmp(sMode, "write") == 0)
		return mode_write(s, c);
	if (strcmp(sMode, "read") == 0)
		return mode_read(s, c);
	if (strcmp(sMode, "poll") == 0)
		return mode_poll(s, c);
	if (strcmp(sMode, "close") == 0)
		return mode_close(s, c);
	if (strcmp(sMode, "recover") == 0)
		return mode_recover(s, c);
	if (strcmp(sMode, "zerowin") == 0)
		return mode_zerowin(s, c);
	if (strcmp(sMode, "reopen") == 0)
		return mode_reopen(s, c);
	if (strcmp(sMode, "zerodead") == 0)
		return mode_zerodead(s, c);
	fprintf(stderr, "unknown mode %s\n", sMode);
	return 100;
}
