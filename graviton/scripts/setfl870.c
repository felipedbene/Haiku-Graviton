/*
 * setfl870 -- fcntl(F_SETFL) on /dev/null, /dev/zero and /dev/full (issue 870).
 *
 * For each device: F_GETFL, then F_SETFL clearing O_NONBLOCK, F_SETFL setting
 * it, and F_SETFL toggling O_APPEND, each checked through F_GETFL. Linux and
 * POSIX accept all of these. A pipe and a regular file are controls, and the
 * random devices are reported for information only (not counted).
 *
 * Build: cc -O2 -Wall -o setfl870 setfl870.c     Exit status: number of failures.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int sFailures;

static int
check_fd(const char* label, int fd, int counted)
{
	int bad = 0;
	int fl = fcntl(fd, F_GETFL);
	char detail[256];
	int n = snprintf(detail, sizeof(detail), "getfl=0x%x", fl);

	struct { const char* name; int flags; int expectNB; } steps[] = {
		{ "clear-NB", fl & ~O_NONBLOCK, 0 },
		{ "set-NB", fl | O_NONBLOCK, 1 },
		{ "set-APPEND", (fl | O_APPEND) & ~O_NONBLOCK, 0 },
		{ "restore", fl, (fl & O_NONBLOCK) != 0 },
	};
	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		int r = fcntl(fd, F_SETFL, steps[i].flags);
		int e = errno;
		int now = fcntl(fd, F_GETFL);
		int ok = r == 0 && ((now & O_NONBLOCK) != 0) == steps[i].expectNB;
		if (r == 0 && steps[i].flags & O_APPEND)
			ok = ok && (now & O_APPEND) != 0;
		n += snprintf(detail + n, sizeof(detail) - n, " %s=%s", steps[i].name,
			r != 0 ? strerror(e) : ok ? "ok" : "not-applied");
		if (!ok)
			bad = 1;
	}
	printf("%-5s %-26s %s\n", !counted ? "INFO" : bad ? "FAIL" : "PASS", label,
		detail);
	if (counted && bad)
		sFailures++;
	return bad;
}

static void
check_path(const char* path, int mode, int counted)
{
	char label[64];
	snprintf(label, sizeof(label), "%s (%s)", path,
		mode == O_RDONLY ? "rdonly" : mode == O_WRONLY ? "wronly" : "rdwr");
	int fd = open(path, mode);
	if (fd < 0) {
		printf("%-5s %-26s open: %s\n", counted ? "FAIL" : "INFO", label,
			strerror(errno));
		if (counted)
			sFailures++;
		return;
	}
	check_fd(label, fd, counted);
	close(fd);
}

int
main(void)
{
	check_path("/dev/null", O_RDONLY, 1);
	check_path("/dev/null", O_RDWR, 1);
	check_path("/dev/null", O_WRONLY, 1);
	check_path("/dev/zero", O_RDONLY, 1);
	check_path("/dev/zero", O_RDWR, 1);
	check_path("/dev/full", O_RDWR, 1);

	int p[2];
	if (pipe(p) == 0) {
		check_fd("pipe read end (control)", p[0], 1);
		close(p[0]);
		close(p[1]);
	}
	char tmpl[] = "/tmp/setfl870.XXXXXX";
	int f = mkstemp(tmpl);
	if (f >= 0) {
		check_fd("regular file (control)", f, 1);
		close(f);
		unlink(tmpl);
	}

	check_path("/dev/random", O_RDONLY, 0);
	check_path("/dev/urandom", O_RDONLY, 0);

	printf("setfl870: %d failed\n", sFailures);
	return sFailures;
}
