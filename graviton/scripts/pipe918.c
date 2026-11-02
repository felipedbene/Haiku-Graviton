/*
 * pipe918 -- what pipe() does when the fd table is (nearly) full (issue 918).
 *
 *   pipe918 full      no free fd: pipe() must fail with EMFILE. Before the fix
 *                     Haiku blocked forever opening the write end of a FIFO
 *                     whose read end had already failed.
 *   pipe918 full2     the same with pipe2(O_NONBLOCK). Its write end never
 *                     blocked, so it already failed with EMFILE; it stays as
 *                     a check that it still does.
 *   pipe918 one       one free fd: the read end fits, the write end does not;
 *                     pipe() must fail with EMFILE and leave the table as it
 *                     was.
 *   pipe918 leak <n>  n pipe() calls in the "one" state, then the change in
 *                     used memory pages; a vnode reference leaked per failed
 *                     call shows up as growth proportional to n.
 *
 * pipe() runs on a second thread and is given 5 s, so a hang is reported
 * rather than inherited. Linux reference: same source, -DLINUX_REF.
 */

#ifdef LINUX_REF
#	define _GNU_SOURCE
		/* pipe2() */
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifndef LINUX_REF
#	include <OS.h>
#endif

static int sResult = -2;
static int sFlags = 0;
static int sErrno = 0;
static int sFds[2] = { -1, -1 };


static void*
do_pipe(void* unused)
{
	int r = sFlags != 0 ? pipe2(sFds, sFlags) : pipe(sFds);
	sErrno = errno;
	__atomic_store_n(&sResult, r, __ATOMIC_SEQ_CST);
	return NULL;
}


/* fill the table, leaving `free` descriptors unused; returns the first fd
 * that was opened, so the caller can tell which slots are free */
static int
fill(int free)
{
	int last = -1;
	for (;;) {
		int fd = dup(0);
		if (fd < 0)
			break;
		last = fd;
	}
	for (int i = 0; i < free; i++)
		close(last - i);
	return last;
}


static int
timed_pipe(const char* label)
{
	pthread_t thread;
	sResult = -2;
	pthread_create(&thread, NULL, do_pipe, NULL);
	for (int i = 0; i < 500; i++) {
		if (__atomic_load_n(&sResult, __ATOMIC_SEQ_CST) != -2)
			break;
		usleep(10000);
	}
	int r = __atomic_load_n(&sResult, __ATOMIC_SEQ_CST);
	if (r == -2) {
		printf("%s: pipe() still blocked after 5 s  -> HANG\n", label);
		return 2;
	}
	pthread_join(thread, NULL);
	if (r == 0) {
		printf("%s: pipe() succeeded fds %d %d\n", label, sFds[0], sFds[1]);
		return 1;
	}
	printf("%s: pipe() = -1 errno %d (%s)%s\n", label, sErrno, strerror(sErrno),
		sErrno == EMFILE ? "  -> ok" : "  -> WRONG ERRNO");
	return sErrno == EMFILE ? 0 : 1;
}


int
main(int argc, char** argv)
{
	const char* mode = argc > 1 ? argv[1] : "full";
	struct rlimit rl = { 64, 64 };
	if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
		perror("setrlimit");
		return 1;
	}

	if (strcmp(mode, "full") == 0) {
		/* the helper thread itself needs no fd; fill completely */
		fill(0);
		return timed_pipe("full table");
	}

	if (strcmp(mode, "full2") == 0) {
		sFlags = O_NONBLOCK;
		fill(0);
		return timed_pipe("full table, pipe2(O_NONBLOCK)");
	}

	if (strcmp(mode, "one") == 0) {
		int last = fill(1);
		int r = timed_pipe("one free fd");
		/* the free slot must still be free: dup() must land on it */
		int fd = dup(0);
		printf("after: dup() = %d (expected %d, the slot pipe() must not "
			"keep)\n", fd, last);
		return r != 0 || fd != last;
	}

	if (strcmp(mode, "leak") == 0) {
		long n = argc > 2 ? atol(argv[2]) : 100000;
		fill(1);
#ifndef LINUX_REF
		system_info before, after;
		get_system_info(&before);
#endif
		long emfile = 0, other = 0;
		for (long i = 0; i < n; i++) {
			int fds[2];
			if (pipe(fds) == 0) {
				close(fds[0]);
				close(fds[1]);
			} else if (errno == EMFILE)
				emfile++;
			else
				other++;
		}
#ifndef LINUX_REF
		get_system_info(&after);
		printf("%ld pipe() calls: %ld EMFILE, %ld other; used pages %lld -> "
			"%lld (delta %lld, %lld bytes per call)\n", n, emfile, other,
			(long long)before.used_pages, (long long)after.used_pages,
			(long long)(after.used_pages - before.used_pages),
			(long long)((after.used_pages - before.used_pages) * B_PAGE_SIZE
				/ n));
#else
		printf("%ld pipe() calls: %ld EMFILE, %ld other\n", n, emfile, other);
#endif
		return other != 0;
	}

	fprintf(stderr, "usage: %s full|full2|one|leak [n]\n", argv[0]);
	return 2;
}
