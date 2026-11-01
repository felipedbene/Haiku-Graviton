/*
 * unix890.c -- issue 890: write() on an AF_UNIX stream end whose peer has
 * closed must fail with EPIPE, on every end: both ends of a socketpair(), the
 * connect()ing end and the accept()ed end. Linux and the BSDs say EPIPE.
 *
 * write() runs with a counting SIGPIPE handler, and a write that fails with
 * EPIPE must also have raised SIGPIPE (as on Linux); a second pass checks the
 * same errno through send(MSG_NOSIGNAL), which must raise none. Usage: unix890 [dir] (default /tmp). Exit status: the number of
 * FAILs. Plain POSIX: builds and runs on Linux too.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#	define MSG_NOSIGNAL 0
#endif


static int sFailed = 0;
static volatile sig_atomic_t sPipeSignals = 0;
static const char* sDir = "/tmp";


/*! Makes a connected pair; [0] is the accept()ed end, [1] the connecting
	one. With \a socketPair, uses socketpair() instead (net_socket accept()s
	there too, which end is which is the question). */
static int
make_pair(int socketPair, int fds[2])
{
	if (socketPair)
		return socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s/unix890.%d",
		sDir, (int)getpid());
	unlink(address.sun_path);

	int listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0
		|| listen(listener, 1) != 0) {
		perror("listener");
		return -1;
	}
	fds[1] = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connect(fds[1], (struct sockaddr*)&address, sizeof(address)) != 0) {
		perror("connect");
		return -1;
	}
	fds[0] = accept(listener, NULL, NULL);
	close(listener);
	unlink(address.sun_path);
	return fds[0] < 0 ? -1 : 0;
}


static void
run(const char* label, int socketPair, int writer, int useSend)
{
	int fds[2];
	if (make_pair(socketPair, fds) != 0) {
		printf("%s: setup failed\n", label);
		sFailed++;
		return;
	}

	close(fds[1 - writer]);
	usleep(20 * 1000);

	int signalsBefore = sPipeSignals;
	errno = 0;
	ssize_t r = useSend ? send(fds[writer], "x", 1, MSG_NOSIGNAL)
		: write(fds[writer], "x", 1);
	int e = errno;
	int signals = sPipeSignals - signalsBefore;
	int ok = r < 0 && e == EPIPE && signals == (useSend ? 0 : 1);
	printf("%-52s %s  r=%zd errno=%d (%s) sigpipe=%d\n", label,
		ok ? "PASS" : "FAIL", r, e, r < 0 ? strerror(e) : "-", signals);
	if (!ok)
		sFailed++;
	close(fds[writer]);
}


static void
pipe_handler(int signal)
{
	sPipeSignals++;
}


int
main(int argc, char** argv)
{
	if (argc > 1)
		sDir = argv[1];

	signal(SIGPIPE, pipe_handler);
	for (int useSend = 0; useSend < 2; useSend++) {
		const char* how = useSend ? "send(MSG_NOSIGNAL)" : "write";
		char label[96];
		snprintf(label, sizeof(label), "socketpair sv[0], sv[1] closed, %s",
			how);
		run(label, 1, 0, useSend);
		snprintf(label, sizeof(label), "socketpair sv[1], sv[0] closed, %s",
			how);
		run(label, 1, 1, useSend);
		snprintf(label, sizeof(label), "accepted end, peer closed, %s", how);
		run(label, 0, 0, useSend);
		snprintf(label, sizeof(label), "connecting end, peer closed, %s",
			how);
		run(label, 0, 1, useSend);
	}
	printf("unix890: %d failed\n", sFailed);
	return sFailed;
}
