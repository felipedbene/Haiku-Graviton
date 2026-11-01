/*
 * tcp891.c -- issue 891: once the application has close()d a TCP socket,
 * new data from the peer must be answered with a RST (RFC 1122 section
 * 4.2.2.13), so the peer's writes start failing within about one round trip.
 *
 *   closed    the server accept()s and close()s. The client reads the FIN
 *             (read() == 0), then writes 10 bytes every 10 ms for up to
 *             <seconds>. PASS if a write fails (EPIPE or ECONNRESET) within
 *             1 s. Linux: the second write fails, after 10 ms.
 *   halfclose the control, issue 615's case: the server only shutdown(SHUT_WR)s
 *             and goes on reading. All 100 of the client's writes must
 *             succeed and the server must read all 1000 bytes.
 *
 * Both run over loopback for IPv4 and IPv6. Usage: tcp891 [seconds]
 * (default 10). Exit status: the number of FAILs. Plain POSIX: builds and
 * runs on Linux too.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>


static int sFailed = 0;
static int sSeconds = 10;


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
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
	if (connect(client, (struct sockaddr*)&address, length) != 0) {
		perror("connect");
		return -1;
	}
	int server = accept(listener, NULL, NULL);
	close(listener);
	if (server < 0) {
		perror("accept");
		return -1;
	}
	*_server = server;
	*_client = client;
	return 0;
}


static void
run_closed(int family, const char* familyName)
{
	int server, client;
	if (make_pair(family, &server, &client) != 0) {
		sFailed++;
		return;
	}

	close(server);
	char buffer[16];
	ssize_t eof = read(client, buffer, sizeof(buffer));

	double start = now_ms();
	int writes = 0, error = 0;
	while (now_ms() - start < sSeconds * 1000.0) {
		ssize_t r = write(client, "0123456789", 10);
		if (r < 0) {
			error = errno;
			break;
		}
		writes++;
		usleep(10 * 1000);
	}
	double elapsed = now_ms() - start;

	int ok = eof == 0 && error != 0 && elapsed < 1000.0
		&& (error == EPIPE || error == ECONNRESET);
	if (error != 0) {
		printf("%s closed     %s  read=%zd  write #%d failed after %.1f ms: "
			"errno=%d (%s)\n", familyName, ok ? "PASS" : "FAIL", eof,
			writes + 1, elapsed, error, strerror(error));
	} else {
		printf("%s closed     FAIL  read=%zd  still writing OK after %.1f ms "
			"(%d writes)\n", familyName, eof, elapsed, writes);
	}
	if (!ok)
		sFailed++;
	close(client);
}


static void
run_halfclose(int family, const char* familyName)
{
	int server, client;
	if (make_pair(family, &server, &client) != 0) {
		sFailed++;
		return;
	}

	shutdown(server, SHUT_WR);
	char buffer[2048];
	ssize_t eof = read(client, buffer, sizeof(buffer));

	int writes = 0, error = 0;
	for (int i = 0; i < 100; i++) {
		if (write(client, "0123456789", 10) != 10) {
			error = errno;
			break;
		}
		writes++;
		usleep(1000);
	}
	close(client);

	// the server is still a reader: it must get every byte, then the EOF
	size_t total = 0;
	ssize_t r;
	while ((r = read(server, buffer, sizeof(buffer))) > 0)
		total += r;
	int readError = r < 0 ? errno : 0;

	int ok = eof == 0 && writes == 100 && total == 1000 && r == 0;
	printf("%s halfclose  %s  read=%zd  writes=%d%s%s  server read %zu bytes, "
		"then %zd%s%s\n", familyName, ok ? "PASS" : "FAIL", eof, writes,
		error ? " error=" : "", error ? strerror(error) : "", total, r,
		readError ? " " : "", readError ? strerror(readError) : "");
	if (!ok)
		sFailed++;
	close(server);
}


int
main(int argc, char** argv)
{
	if (argc > 1)
		sSeconds = atoi(argv[1]);
	signal(SIGPIPE, SIG_IGN);

	run_closed(AF_INET, "ipv4");
	run_halfclose(AF_INET, "ipv4");
	run_closed(AF_INET6, "ipv6");
	run_halfclose(AF_INET6, "ipv6");

	printf("tcp891: %d failed\n", sFailed);
	return sFailed;
}
