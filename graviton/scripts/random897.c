/*
 * random897 -- fcntl(F_SETFL) and O_NONBLOCK on /dev/random and /dev/urandom
 * (issue 897).
 *
 * For each device and open mode: F_GETFL, F_SETFL clearing and setting
 * O_NONBLOCK (each checked through F_GETFL and followed by a read that must
 * return the full count), F_SETFL with O_APPEND, and a restore. Then the
 * same through ioctl(FIONBIO), and an fd opened with O_NONBLOCK. Linux and
 * POSIX accept all of these, and reads from these devices never block.
 *
 * The driver must still reject what it does not implement, so the last
 * group checks that isatty(), ioctl(FIONREAD) and an unknown ioctl keep
 * failing. Their errno is printed for comparison but not counted: Linux and
 * Haiku spell "unsupported" differently.
 *
 * Build: cc -O2 -Wall -o random897 random897.c     Exit status: failures.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int sFailures;

static void
report(int ok, const char* label, const char* detail)
{
	printf("%-4s %-30s %s\n", ok ? "PASS" : "FAIL", label, detail);
	if (!ok)
		sFailures++;
}

/* Reads 64 bytes; returns NULL on success or a description of the failure. */
static const char*
read_full(int fd, char* why, size_t whySize)
{
	unsigned char buf[64];
	ssize_t r = read(fd, buf, sizeof(buf));
	if (r == (ssize_t)sizeof(buf))
		return NULL;
	if (r < 0)
		snprintf(why, whySize, "read: %s", strerror(errno));
	else
		snprintf(why, whySize, "read: short %zd", r);
	return why;
}

static void
check_setfl(const char* path, int mode)
{
	char label[64], detail[512], why[128];
	snprintf(label, sizeof(label), "F_SETFL %s (%s)", path,
		mode == O_RDONLY ? "rdonly" : "rdwr");
	int fd = open(path, mode);
	if (fd < 0) {
		snprintf(detail, sizeof(detail), "open: %s", strerror(errno));
		report(0, label, detail);
		return;
	}

	int fl = fcntl(fd, F_GETFL);
	int n = snprintf(detail, sizeof(detail), "getfl=0x%x", fl);
	int bad = fl < 0;

	struct { const char* name; int flags; int expectNB; int doRead; } steps[] = {
		{ "clear-NB", fl & ~O_NONBLOCK, 0, 1 },
		{ "set-NB", fl | O_NONBLOCK, 1, 1 },
		{ "set-APPEND", (fl | O_APPEND) & ~O_NONBLOCK, 0, 0 },
		{ "restore", fl, (fl & O_NONBLOCK) != 0, 1 },
	};
	for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		int r = fcntl(fd, F_SETFL, steps[i].flags);
		int e = errno;
		int now = fcntl(fd, F_GETFL);
		const char* result = "ok";
		if (r != 0)
			result = strerror(e);
		else if (((now & O_NONBLOCK) != 0) != steps[i].expectNB
			|| ((steps[i].flags & O_APPEND) && !(now & O_APPEND)))
			result = "not-applied";
		else if (steps[i].doRead)
			result = read_full(fd, why, sizeof(why));
		if (result == NULL)
			result = "ok";
		if (strcmp(result, "ok") != 0)
			bad = 1;
		n += snprintf(detail + n, sizeof(detail) - n, " %s=%s", steps[i].name,
			result);
	}
	report(!bad, label, detail);
	close(fd);
}

static void
check_fionbio(const char* path)
{
	char label[64], detail[256], why[128];
	snprintf(label, sizeof(label), "FIONBIO %s", path);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		snprintf(detail, sizeof(detail), "open: %s", strerror(errno));
		report(0, label, detail);
		return;
	}
	int on = 1, off = 0;
	int r1 = ioctl(fd, FIONBIO, &on);
	int e1 = errno;
	int nb1 = (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
	const char* rd1 = r1 == 0 ? read_full(fd, why, sizeof(why)) : NULL;
	int r0 = ioctl(fd, FIONBIO, &off);
	int e0 = errno;
	int nb0 = (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
	snprintf(detail, sizeof(detail), "on=%s nb=%d %s off=%s nb=%d",
		r1 == 0 ? "ok" : strerror(e1), nb1, rd1 != NULL ? rd1 : "read=ok",
		r0 == 0 ? "ok" : strerror(e0), nb0);
	report(r1 == 0 && nb1 && rd1 == NULL && r0 == 0 && !nb0, label, detail);
	close(fd);
}

static void
check_open_nonblock(const char* path)
{
	char label[64], detail[256], why[128];
	snprintf(label, sizeof(label), "open(O_NONBLOCK) %s", path);
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		snprintf(detail, sizeof(detail), "open: %s", strerror(errno));
		report(0, label, detail);
		return;
	}
	int nb = (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
	const char* rd = read_full(fd, why, sizeof(why));
	struct pollfd p = { fd, POLLIN, 0 };
	int pr = poll(&p, 1, 0);
	int r = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	int e = errno;
	int nbAfter = (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
	snprintf(detail, sizeof(detail), "nb=%d %s poll=%d revents=0x%x "
		"clear-NB=%s nb=%d", nb, rd != NULL ? rd : "read=ok", pr, p.revents,
		r == 0 ? "ok" : strerror(e), nbAfter);
	report(nb && rd == NULL && pr == 1 && (p.revents & POLLIN) && r == 0
		&& !nbAfter, label, detail);
	close(fd);
}

/* Things the driver does not implement must keep failing. */
static void
check_still_rejected(const char* path)
{
	char label[64], detail[256];
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		snprintf(label, sizeof(label), "reject %s", path);
		snprintf(detail, sizeof(detail), "open: %s", strerror(errno));
		report(0, label, detail);
		return;
	}

	errno = 0;
	int tty = isatty(fd);
	int e = errno;
	snprintf(label, sizeof(label), "isatty %s", path);
	snprintf(detail, sizeof(detail), "isatty=%d errno=%d (%s)", tty, e,
		strerror(e));
	report(tty == 0, label, detail);

	int avail = -12345;
	int r = ioctl(fd, FIONREAD, &avail);
	e = errno;
	snprintf(label, sizeof(label), "FIONREAD %s", path);
	snprintf(detail, sizeof(detail), "r=%d errno=%d (%s)", r, e,
		r == 0 ? "succeeded" : strerror(e));
	report(r != 0, label, detail);

	/* Not a request either system defines. */
	unsigned long op = 0x7e5a1897UL;
	char arg[64];
	r = ioctl(fd, op, arg);
	e = errno;
	snprintf(label, sizeof(label), "unknown ioctl %s", path);
	snprintf(detail, sizeof(detail), "op=0x%lx r=%d errno=%d (%s)", op, r, e,
		r == 0 ? "succeeded" : strerror(e));
	report(r != 0, label, detail);

	close(fd);
}

int
main(void)
{
	const char* paths[] = { "/dev/random", "/dev/urandom" };
	for (int i = 0; i < 2; i++) {
		check_setfl(paths[i], O_RDONLY);
		check_setfl(paths[i], O_RDWR);
		check_fionbio(paths[i]);
		check_open_nonblock(paths[i]);
		check_still_rejected(paths[i]);
	}

	printf("random897: %d failed\n", sFailures);
	return sFailures;
}
