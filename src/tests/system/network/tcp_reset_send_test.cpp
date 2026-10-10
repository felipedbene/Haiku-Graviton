/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	send() after the peer reset the connection must fail the same way no
	matter whether the sender was blocked in send() when the RST arrived or
	called send() after it had been processed (#634): the first send()
	reports ECONNRESET without a signal, every later one EPIPE plus SIGPIPE,
	as on Linux. Before the fix the blocked sender got EPIPE+SIGPIPE and the
	unblocked one ENOTCONN, which made #617 intermittent.
*/


#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static volatile sig_atomic_t sPipeSignals = 0;
static int sFailures = 0;


static void
pipe_handler(int)
{
	sPipeSignals++;
}


static void
check(const char* arm, const char* what, int got, int expected)
{
	if (got == expected) {
		printf("ok   %s: %s = %s\n", arm, what, strerror(expected));
		return;
	}
	printf("FAIL %s: %s = %d (%s), expected %d (%s)\n", arm, what, got,
		strerror(got), expected, strerror(expected));
	sFailures++;
}


static void
check_count(const char* arm, const char* what, int got, int expected)
{
	if (got == expected) {
		printf("ok   %s: %s = %d\n", arm, what, expected);
		return;
	}
	printf("FAIL %s: %s = %d, expected %d\n", arm, what, got, expected);
	sFailures++;
}


static void
connected_pair(int& client, int& server)
{
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t addrLen = sizeof(addr);
	if (listener < 0 || bind(listener, (sockaddr*)&addr, addrLen) != 0
		|| listen(listener, 1) != 0
		|| getsockname(listener, (sockaddr*)&addr, &addrLen) != 0) {
		fprintf(stderr, "listener setup failed: %s\n", strerror(errno));
		exit(2);
	}

	client = socket(AF_INET, SOCK_STREAM, 0);
	int sendBuffer = 4096;
	setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
	if (client < 0 || connect(client, (sockaddr*)&addr, addrLen) != 0) {
		fprintf(stderr, "connect failed: %s\n", strerror(errno));
		exit(2);
	}
	server = accept(listener, NULL, NULL);
	if (server < 0) {
		fprintf(stderr, "accept failed: %s\n", strerror(errno));
		exit(2);
	}
	close(listener);
}


static void
reset_by_peer(int server)
{
	// SO_LINGER with a zero timeout turns close() into an abortive RST.
	linger abortive = { 1, 0 };
	setsockopt(server, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive));
	close(server);
}


static int
send_errno(int fd, const char* buffer, size_t size)
{
	errno = 0;
	if (send(fd, buffer, size, 0) >= 0)
		return 0;
	return errno;
}


/*!	The first and second send() after the reset, plus the SIGPIPE count each
	one raised; both arms MUST produce the same four values.
*/
static void
check_after_reset(const char* arm, int firstErrno, int firstSignals,
	int client)
{
	char byte = 'x';
	check(arm, "first send() after reset", firstErrno, ECONNRESET);
	check_count(arm, "SIGPIPE from first send()", firstSignals, 0);

	int before = sPipeSignals;
	int secondErrno = send_errno(client, &byte, 1);
	check(arm, "second send() after reset", secondErrno, EPIPE);
	check_count(arm, "SIGPIPE from second send()", sPipeSignals - before, 1);
}


struct BlockedSender {
	int		fd;
	int		error;
	int		signals;
	volatile bool blocked;
};


static void*
blocked_sender(void* data)
{
	BlockedSender* sender = (BlockedSender*)data;
	static char chunk[65536];

	// The peer never reads, so this fills its receive window and our send
	// queue, and then blocks until the reset wakes it.
	sender->blocked = true;
	int before = sPipeSignals;
	while (true) {
		errno = 0;
		if (send(sender->fd, chunk, sizeof(chunk), 0) < 0)
			break;
	}
	sender->error = errno;
	sender->signals = sPipeSignals - before;
	return NULL;
}


static void
arm_blocked()
{
	int client, server;
	connected_pair(client, server);

	BlockedSender sender = { client, 0, 0, false };
	pthread_t thread;
	pthread_create(&thread, NULL, blocked_sender, &sender);
	while (!sender.blocked)
		usleep(1000);
	usleep(500000);
		// long enough for the window to close and the sender to sleep

	reset_by_peer(server);
	pthread_join(thread, NULL);

	check_after_reset("blocked", sender.error, sender.signals, client);
	close(client);
}


static void
arm_unblocked()
{
	int client, server;
	connected_pair(client, server);

	reset_by_peer(server);
	usleep(200000);
		// let the RST be processed before the first send()

	char byte = 'x';
	int before = sPipeSignals;
	int error = send_errno(client, &byte, 1);
	check_after_reset("unblocked", error, sPipeSignals - before, client);
	close(client);
}


int
main()
{
	signal(SIGPIPE, pipe_handler);

	arm_blocked();
	arm_unblocked();

	if (sFailures != 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
