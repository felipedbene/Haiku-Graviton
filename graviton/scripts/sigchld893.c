/*
 * sigchld893.c -- issue 893: are a dying child's file descriptors closed
 * before its SIGCHLD is sent and it becomes reapable?
 *
 * Each round forks a child holding one end of a socketpair (and of a pipe),
 * kills it (SIGKILL, or the child exits by itself), and poll()s the parent's
 * ends together with a SIGCHLD self-pipe. At the first wakeup that shows the
 * SIGCHLD, the peer ends must already be at EOF/HUP; and as soon as
 * waitpid(WNOHANG) can reap the child, the ends must be at EOF/HUP too.
 *
 * Usage: sigchld893 [rounds]   (default 50 per mode). Exit status: 0 if every
 * round of every mode was in order, else 1.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>


static int sSelfPipe[2];


static void
sigchld_handler(int signal)
{
	(void)signal;
	int saved = errno;
	write(sSelfPipe[1], "c", 1);
	errno = saved;
}


static int
at_eof(int fd)
{
	struct pollfd pfd = { fd, POLLIN, 0 };
	return poll(&pfd, 1, 0) == 1 && (pfd.revents & (POLLIN | POLLHUP)) != 0;
}


static int
run_mode(const char* mode, int rounds)
{
	int sigFirst = 0, reapableOpen = 0;
	for (int i = 0; i < rounds; i++) {
		int sv[2], p[2];
		socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
		pipe(p);
		fflush(stdout);
		pid_t child = fork();
		if (child == 0) {
			close(sv[0]);
			close(p[0]);
			if (mode[0] == 'e') {
				usleep(20000);
				_exit(0);
			}
			for (;;)
				pause();
		}
		close(sv[1]);
		close(p[1]);
		if (mode[0] == 'k') {
			usleep(20000);
			kill(child, SIGKILL);
		}

		struct pollfd pfds[3] = {
			{ sSelfPipe[0], POLLIN, 0 }, { sv[0], POLLIN, 0 }, { p[0], POLLIN, 0 }
		};
		while (poll(pfds, 3, -1) < 0)
			;
		int sigReady = (pfds[0].revents & POLLIN) != 0;
		int peersReady = (pfds[1].revents & (POLLIN | POLLHUP)) != 0
			&& (pfds[2].revents & (POLLIN | POLLHUP)) != 0;
		if (sigReady && !peersReady)
			sigFirst++;

		// once the child can be reaped, its descriptors must be closed
		int status;
		while (waitpid(child, &status, WNOHANG) == 0)
			usleep(100);
		if (!at_eof(sv[0]) || !at_eof(p[0]))
			reapableOpen++;

		char buffer[16];
		while (read(sSelfPipe[0], buffer, sizeof(buffer)) > 0)
			;
		close(sv[0]);
		close(p[0]);
	}
	printf("%-5s SIGCHLD seen before the child's fds closed: %d of %d; "
		"reaped with fds still open: %d of %d  %s\n", mode, sigFirst, rounds,
		reapableOpen, rounds, sigFirst || reapableOpen ? "FAIL" : "PASS");
	return sigFirst || reapableOpen;
}


int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 50;
	pipe(sSelfPipe);
	fcntl(sSelfPipe[0], F_SETFL, O_NONBLOCK);
	struct sigaction action = {};
	action.sa_handler = sigchld_handler;
	action.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &action, NULL);

	int failed = run_mode("kill", rounds);
	failed += run_mode("exit", rounds);
	printf("sigchld893: %d failed\n", failed);
	return failed ? 1 : 0;
}
