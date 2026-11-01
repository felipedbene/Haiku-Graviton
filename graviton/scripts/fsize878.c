/*
 * fsize878.c -- issue 878: is RLIMIT_FSIZE implemented?
 *
 * Each check runs in a forked child with a fresh file in the current
 * directory. Limit 1024 bytes unless said otherwise.
 *   get-default  getrlimit(RLIMIT_FSIZE) succeeds, {RLIM_INFINITY, RLIM_INFINITY}
 *   set          setrlimit({1024, 1024}) succeeds and reads back
 *   partial      write(4096) at offset 0 writes 1024 bytes
 *   efbig        the next write fails EFBIG, SIGXFSZ ignored
 *   sigxfsz      ... and with a handler, the handler runs once
 *   default-kill ... and with the default action, the child dies of SIGXFSZ
 *   append       O_APPEND write to a 1000-byte file writes 24 bytes, then EFBIG
 *   pwrite       pwrite() at offset 2048 fails EFBIG
 *   writev       writev() of 3 x 512 bytes writes 1024
 *   ftruncate    ftruncate(4096) fails EFBIG; ftruncate(512) succeeds
 *   no-limit     without a limit, a 64 KiB write is whole
 *   inherit      a forked child sees the limit
 *   raise-user   after setuid(), raising the hard limit fails EPERM
 *   pipe         the limit does not apply to a pipe
 *
 * Exit status: number of failed checks.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>


static char sBuffer[65536];
static volatile sig_atomic_t sSignals = 0;


static void
handler(int signal)
{
	(void)signal;
	sSignals++;
}


static int
set_limit(rlim_t limit)
{
	struct rlimit rl = { limit, limit };
	return setrlimit(RLIMIT_FSIZE, &rl);
}


static int
fresh_file(const char* name, int flags)
{
	unlink(name);
	return open(name, O_RDWR | O_CREAT | O_TRUNC | flags, 0644);
}


static int
run_check(const char* name, char* detail, size_t size)
{
	// returns 1 if the check passed
	if (strcmp(name, "get-default") == 0) {
		struct rlimit rl;
		int rc = getrlimit(RLIMIT_FSIZE, &rl);
		snprintf(detail, size, "rc=%d %s cur=%#lx max=%#lx", rc,
			rc ? strerror(errno) : "", (unsigned long)rl.rlim_cur,
			(unsigned long)rl.rlim_max);
		return rc == 0 && rl.rlim_cur == RLIM_INFINITY
			&& rl.rlim_max == RLIM_INFINITY;
	}
	if (strcmp(name, "set") == 0) {
		int rc = set_limit(1024);
		struct rlimit rl = { 0, 0 };
		getrlimit(RLIMIT_FSIZE, &rl);
		snprintf(detail, size, "rc=%d %s now {%lu,%lu}", rc,
			rc ? strerror(errno) : "", (unsigned long)rl.rlim_cur,
			(unsigned long)rl.rlim_max);
		return rc == 0 && rl.rlim_cur == 1024 && rl.rlim_max == 1024;
	}
	if (strcmp(name, "no-limit") == 0) {
		int fd = fresh_file("fsize878.dat", 0);
		ssize_t n = write(fd, sBuffer, sizeof(sBuffer));
		snprintf(detail, size, "write(65536)=%zd", n);
		return n == (ssize_t)sizeof(sBuffer);
	}
	if (strcmp(name, "pipe") == 0) {
		int fds[2];
		pipe(fds);
		set_limit(1024);
		ssize_t n = write(fds[1], sBuffer, 4096);
		snprintf(detail, size, "write(pipe, 4096)=%zd", n);
		return n == 4096;
	}
	if (strcmp(name, "inherit") == 0) {
		set_limit(1024);
		fflush(stdout);
		pid_t child = fork();
		if (child == 0) {
			struct rlimit rl;
			getrlimit(RLIMIT_FSIZE, &rl);
			_exit(rl.rlim_cur == 1024 && rl.rlim_max == 1024 ? 0 : 1);
		}
		int status;
		waitpid(child, &status, 0);
		snprintf(detail, size, "child exit %d", WEXITSTATUS(status));
		return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}
	if (strcmp(name, "raise-user") == 0) {
		set_limit(1024);
		if (geteuid() == 0 && setuid(1001) != 0) {
			snprintf(detail, size, "setuid: %s", strerror(errno));
			return 0;
		}
		struct rlimit rl = { 1024, 4096 };
		int rc = setrlimit(RLIMIT_FSIZE, &rl);
		snprintf(detail, size, "uid %d {1024,4096}: rc=%d %s", (int)geteuid(),
			rc, rc ? strerror(errno) : "");
		return rc == -1 && errno == EPERM;
	}

	signal(SIGXFSZ, SIG_IGN);
	if (strcmp(name, "partial") == 0 || strcmp(name, "efbig") == 0
		|| strcmp(name, "sigxfsz") == 0 || strcmp(name, "default-kill") == 0) {
		int fd = fresh_file("fsize878.dat", 0);
		if (set_limit(1024) != 0) {
			snprintf(detail, size, "setrlimit: %s", strerror(errno));
			return 0;
		}
		ssize_t first = write(fd, sBuffer, 4096);
		if (strcmp(name, "partial") == 0) {
			snprintf(detail, size, "write(4096)=%zd", first);
			return first == 1024;
		}
		if (strcmp(name, "sigxfsz") == 0)
			signal(SIGXFSZ, handler);
		if (strcmp(name, "default-kill") == 0)
			signal(SIGXFSZ, SIG_DFL);
		errno = 0;
		ssize_t second = write(fd, sBuffer, 4096);
		int error = errno;
		snprintf(detail, size, "write(4096)=%zd then %zd %s, SIGXFSZ caught %d",
			first, second, second < 0 ? strerror(error) : "", (int)sSignals);
		if (strcmp(name, "sigxfsz") == 0)
			return second == -1 && error == EFBIG && sSignals == 1;
		return second == -1 && error == EFBIG;
	}
	if (strcmp(name, "append") == 0) {
		int fd = fresh_file("fsize878.dat", 0);
		write(fd, sBuffer, 1000);
		close(fd);
		fd = open("fsize878.dat", O_WRONLY | O_APPEND);
		set_limit(1024);
		ssize_t first = write(fd, sBuffer, 100);
		errno = 0;
		ssize_t second = write(fd, sBuffer, 100);
		int error = errno;
		snprintf(detail, size, "append write(100)=%zd then %zd %s", first,
			second, second < 0 ? strerror(error) : "");
		return first == 24 && second == -1 && error == EFBIG;
	}
	if (strcmp(name, "pwrite") == 0) {
		int fd = fresh_file("fsize878.dat", 0);
		set_limit(1024);
		errno = 0;
		ssize_t n = pwrite(fd, sBuffer, 10, 2048);
		int error = errno;
		snprintf(detail, size, "pwrite(10 @ 2048)=%zd %s", n,
			n < 0 ? strerror(error) : "");
		return n == -1 && error == EFBIG;
	}
	if (strcmp(name, "writev") == 0) {
		int fd = fresh_file("fsize878.dat", 0);
		set_limit(1024);
		struct iovec vecs[3] = {
			{ sBuffer, 512 }, { sBuffer, 512 }, { sBuffer, 512 }
		};
		ssize_t n = writev(fd, vecs, 3);
		snprintf(detail, size, "writev(3 x 512)=%zd", n);
		return n == 1024;
	}
	if (strcmp(name, "ftruncate") == 0) {
		int fd = fresh_file("fsize878.dat", 0);
		set_limit(1024);
		errno = 0;
		int big = ftruncate(fd, 4096);
		int error = errno;
		int small = ftruncate(fd, 512);
		snprintf(detail, size, "ftruncate(4096)=%d %s, ftruncate(512)=%d", big,
			big ? strerror(error) : "", small);
		return big == -1 && error == EFBIG && small == 0;
	}
	snprintf(detail, size, "unknown check");
	return 0;
}


int
main(int argc, char** argv)
{
	static const char* kChecks[] = {
		"get-default", "set", "partial", "efbig", "sigxfsz", "default-kill",
		"append", "pwrite", "writev", "ftruncate", "no-limit", "inherit",
		"raise-user", "pipe"
	};
	const char** checks = kChecks;
	int count = sizeof(kChecks) / sizeof(kChecks[0]);
	if (argc > 1) {
		checks = (const char**)argv + 1;
		count = argc - 1;
	}

	int failed = 0;
	for (int i = 0; i < count; i++) {
		int pipeFDs[2];
		pipe(pipeFDs);
		fflush(stdout);
		pid_t child = fork();
		if (child == 0) {
			char detail[256] = "";
			int ok = run_check(checks[i], detail, sizeof(detail));
			dprintf(pipeFDs[1], "%s", detail);
			_exit(ok ? 0 : 1);
		}
		close(pipeFDs[1]);
		char detail[256] = "";
		ssize_t bytes = read(pipeFDs[0], detail, sizeof(detail) - 1);
		detail[bytes > 0 ? bytes : 0] = '\0';
		close(pipeFDs[0]);
		int status;
		waitpid(child, &status, 0);

		int ok;
		if (strcmp(checks[i], "default-kill") == 0) {
			ok = WIFSIGNALED(status) && WTERMSIG(status) == SIGXFSZ;
			snprintf(detail + strlen(detail), sizeof(detail) - strlen(detail),
				"child %s %d", WIFSIGNALED(status) ? "signaled" : "exited",
				WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
		} else
			ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		printf("%-12s %s  %s\n", checks[i], ok ? "PASS" : "FAIL", detail);
		failed += !ok;
	}
	unlink("fsize878.dat");
	printf("fsize878: %d failed (SIGXFSZ=%d EFBIG=%d)\n", failed, SIGXFSZ,
		EFBIG);
	return failed;
}
