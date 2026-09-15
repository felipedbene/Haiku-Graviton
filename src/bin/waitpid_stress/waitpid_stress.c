/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * waitpid_stress -- reproducer for the high-parallelism waitpid(ECHILD) race
 * (DeBeOS issue #262). Under a large `ninja -jN` build a per-pid
 * waitpid(pid, &status, 0) fatals with ECHILD ("No child process") even
 * though ninja never reaped that child. This harness mimics ninja's POSIX
 * subprocess reaper closely enough to pin the failure to the kernel/libroot
 * child-death accounting rather than to ninja.
 *
 * Two modes:
 *
 *   --batch  (default): fork <count> short-lived children that all exit
 *            (closing their stdout), wait until every one has become a
 *            zombie, THEN reap each with a per-pid blocking waitpid(). This
 *            deterministically exposes the kernel's MAX_DEAD_CHILDREN soft
 *            limit: when more than that many unreaped children accumulate,
 *            the kernel silently discards the oldest death entries and the
 *            later waitpid() for those pids returns ECHILD.
 *
 *   --ninja: keep up to <jobs> children running concurrently, detect each
 *            child's completion via EOF on its stdout pipe (exactly how
 *            ninja's SubprocessSet::DoWork learns a job finished), then call
 *            waitpid(pid, &status, 0). This is the faithful ninja model.
 *
 * Exit status: 0 if no spurious ECHILD was seen, 1 if at least one was.
 */

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


static int
reap_one(pid_t pid, int* echild)
{
	int status;
	pid_t r;

	// This is the exact call ninja makes in Subprocess::Finish().
	while ((r = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
		;

	if (r < 0) {
		if (errno == ECHILD) {
			(*echild)++;
			fprintf(stderr, "waitpid(%d): No child process (spurious ECHILD)\n",
				(int)pid);
			return -1;
		}
		fprintf(stderr, "waitpid(%d): unexpected error: %s\n", (int)pid,
			strerror(errno));
		return -1;
	}
	return 0;
}


/*!	Fork \a count children that immediately exit, let them all pile up as
	unreaped zombies, then reap them in creation order. Exposes the
	MAX_DEAD_CHILDREN soft-limit drop directly.
*/
static int
run_batch(int count, int settle_ms)
{
	pid_t* pids = calloc(count, sizeof(pid_t));
	if (pids == NULL) {
		perror("calloc");
		return 2;
	}

	for (int i = 0; i < count; i++) {
		pid_t pid = fork();
		if (pid < 0) {
			perror("fork");
			free(pids);
			return 2;
		}
		if (pid == 0)
			_exit(0);
		pids[i] = pid;
	}

	// Let every child become a zombie before we reap any. This is the
	// worst case for the kernel: all <count> death entries coexist, so if
	// count > MAX_DEAD_CHILDREN the oldest are dropped.
	if (settle_ms > 0)
		usleep((useconds_t)settle_ms * 1000);

	int echild = 0;
	for (int i = 0; i < count; i++)
		reap_one(pids[i], &echild);

	free(pids);

	printf("batch: forked=%d spurious_ECHILD=%d\n", count, echild);
	return echild != 0 ? 1 : 0;
}


/*!	Faithful ninja model: keep up to \a jobs children alive, each writing
	nothing and exiting; learn of completion via EOF on the child's stdout
	pipe, then per-pid waitpid().
*/
static int
run_ninja(int total, int jobs)
{
	pid_t* pid_of = calloc(jobs, sizeof(pid_t));
	struct pollfd* fds = calloc(jobs, sizeof(struct pollfd));
	if (pid_of == NULL || fds == NULL) {
		perror("calloc");
		return 2;
	}

	int started = 0;
	int finished = 0;
	int echild = 0;
	int live = 0;

	while (finished < total) {
		// Fill the slots up to the job limit.
		while (live < jobs && started < total) {
			int pipefd[2];
			if (pipe(pipefd) < 0) {
				perror("pipe");
				return 2;
			}
			pid_t pid = fork();
			if (pid < 0) {
				perror("fork");
				return 2;
			}
			if (pid == 0) {
				// Child: mimic a compiler that produces no stdout and exits
				// promptly. Closing on _exit() gives the parent an EOF.
				close(pipefd[0]);
				close(pipefd[1]);
				_exit(0);
			}
			close(pipefd[1]);
			// Find a free slot.
			int slot = -1;
			for (int s = 0; s < jobs; s++) {
				if (fds[s].fd == 0 || fds[s].fd == -1) {
					slot = s;
					break;
				}
			}
			fds[slot].fd = pipefd[0];
			fds[slot].events = POLLIN;
			pid_of[slot] = pid;
			started++;
			live++;
		}

		int n = poll(fds, jobs, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return 2;
		}

		for (int s = 0; s < jobs && n > 0; s++) {
			if (fds[s].fd <= 0)
				continue;
			if ((fds[s].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
				continue;
			n--;
			// Drain then detect EOF, like ninja does.
			char buf[256];
			ssize_t r;
			while ((r = read(fds[s].fd, buf, sizeof(buf))) > 0)
				;
			if (r == 0 || (r < 0 && errno != EINTR)) {
				// EOF: the job is done. Reap it, exactly as ninja does.
				close(fds[s].fd);
				reap_one(pid_of[s], &echild);
				fds[s].fd = -1;
				pid_of[s] = 0;
				finished++;
				live--;
			}
		}
	}

	free(pid_of);
	free(fds);
	printf("ninja: total=%d jobs=%d spurious_ECHILD=%d\n", total, jobs, echild);
	return echild != 0 ? 1 : 0;
}


static void
usage(const char* prog)
{
	fprintf(stderr,
		"usage: %s [--batch] [--ninja] [--count N] [--jobs N] [--settle MS]\n"
		"  --batch          fork N children, let them all zombie, then reap "
			"(default)\n"
		"  --ninja          concurrent pipe-EOF driven per-pid reaper\n"
		"  --count N        number of children to spawn (default 64)\n"
		"  --jobs N         concurrency for --ninja (default 64)\n"
		"  --settle MS      batch: ms to wait before reaping (default 100)\n",
		prog);
}


int
main(int argc, char** argv)
{
	int mode_ninja = 0;
	int count = 64;
	int jobs = 64;
	int settle = 100;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--ninja") == 0)
			mode_ninja = 1;
		else if (strcmp(argv[i], "--batch") == 0)
			mode_ninja = 0;
		else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
			count = atoi(argv[++i]);
		else if (strcmp(argv[i], "--jobs") == 0 && i + 1 < argc)
			jobs = atoi(argv[++i]);
		else if (strcmp(argv[i], "--settle") == 0 && i + 1 < argc)
			settle = atoi(argv[++i]);
		else {
			usage(argv[0]);
			return 2;
		}
	}

	if (mode_ninja)
		return run_ninja(count, jobs);
	return run_batch(count, settle);
}
