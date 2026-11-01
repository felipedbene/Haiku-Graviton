/*
 * atime879 -- st_atime must be stable and must honour an explicit set
 * (issue 879).
 *
 * Whether a read updates it is reported for information only: noatime and
 * relatime are both legitimate. So is the UTIME_NOW case: libroot's
 * utimensat() is a separate code path from the file system's handling.
 *
 * Usage: atime879 [dir]   (default /tmp)
 * Build: cc -O2 -Wall -o atime879 atime879.c      Exit status: failures.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int sFailures;

static double
ts(const struct timespec* t)
{
	return t->tv_sec + t->tv_nsec / 1e9;
}

static void
result(int counted, int ok, const char* label, const char* detail)
{
	printf("%-4s %-46s %s\n", !counted ? "INFO" : ok ? "PASS" : "FAIL", label,
		detail);
	if (counted && !ok)
		sFailures++;
}

int
main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : "/tmp";
	char path[256], detail[256];
	snprintf(path, sizeof(path), "%s/atime879.%d", dir, (int)getpid());
	int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	write(fd, "x", 1);
	struct stat a, b;

	// 1. two stats of an untouched file, 200 ms apart
	stat(path, &a);
	usleep(200000);
	stat(path, &b);
	snprintf(detail, sizeof(detail), "atime %.6f then %.6f", ts(&a.st_atim),
		ts(&b.st_atim));
	result(1, a.st_atim.tv_sec == b.st_atim.tv_sec
		&& a.st_atim.tv_nsec == b.st_atim.tv_nsec, "stable across two stat()s",
		detail);

	// 2. never set: not in the future of the last modification
	snprintf(detail, sizeof(detail), "atime %.6f mtime %.6f now %ld",
		ts(&b.st_atim), ts(&b.st_mtim), (long)time(NULL));
	result(1, ts(&b.st_atim) <= time(NULL) + 1.0, "unset atime is not 'now'",
		detail);

	// 3. utimes() sets both
	struct timeval tv[2] = { { 1000000000, 0 }, { 1000000005, 0 } };
	utimes(path, tv);
	stat(path, &a);
	snprintf(detail, sizeof(detail), "atime %.6f mtime %.6f", ts(&a.st_atim),
		ts(&a.st_mtim));
	result(1, a.st_atim.tv_sec == 1000000000 && a.st_mtim.tv_sec == 1000000005,
		"utimes(atime=1e9, mtime=1e9+5)", detail);

	// 4. utimensat() access time only, with nanoseconds
	struct timespec t2[2] = { { 1234567890, 123456789 }, { 0, UTIME_OMIT } };
	int r = utimensat(AT_FDCWD, path, t2, 0);
	stat(path, &a);
	snprintf(detail, sizeof(detail), "r=%d atime %ld.%09ld mtime %.6f", r,
		(long)a.st_atim.tv_sec, (long)a.st_atim.tv_nsec, ts(&a.st_mtim));
	result(1, r == 0 && a.st_atim.tv_sec == 1234567890
		&& a.st_atim.tv_nsec == 123456789 && a.st_mtim.tv_sec == 1000000005,
		"utimensat(atime only, mtime UTIME_OMIT)", detail);

	// 5. futimens() on the descriptor
	struct timespec t3[2] = { { 1500000000, 0 }, { 1500000001, 0 } };
	r = futimens(fd, t3);
	fstat(fd, &a);
	snprintf(detail, sizeof(detail), "r=%d atime %.6f mtime %.6f", r,
		ts(&a.st_atim), ts(&a.st_mtim));
	result(1, r == 0 && a.st_atim.tv_sec == 1500000000
		&& a.st_mtim.tv_sec == 1500000001, "futimens(atime=1.5e9)", detail);

	// 6. a later write moves mtime but leaves the explicit atime alone
	sleep(1);
	write(fd, "y", 1);
	fsync(fd);
	fstat(fd, &a);
	snprintf(detail, sizeof(detail), "atime %.6f mtime %.6f", ts(&a.st_atim),
		ts(&a.st_mtim));
	result(1, a.st_atim.tv_sec == 1500000000 && a.st_mtim.tv_sec > 1500000001,
		"write keeps the explicit atime", detail);

	// 7. a read may update it (Linux relatime does, atime being older than
	// mtime here) or not (noatime): information only
	char c;
	lseek(fd, 0, SEEK_SET);
	read(fd, &c, 1);
	fstat(fd, &b);
	snprintf(detail, sizeof(detail), "atime %.6f", ts(&b.st_atim));
	result(0, b.st_atim.tv_sec == a.st_atim.tv_sec, "read leaves it unchanged",
		detail);

	// 8. UTIME_NOW for the access time (information only)
	struct timespec t4[2] = { { 0, UTIME_NOW }, { 0, UTIME_OMIT } };
	r = utimensat(AT_FDCWD, path, t4, 0);
	stat(path, &a);
	snprintf(detail, sizeof(detail), "r=%d atime %.6f now %ld", r,
		ts(&a.st_atim), (long)time(NULL));
	result(0, r == 0 && labs((long)a.st_atim.tv_sec - (long)time(NULL)) <= 2,
		"utimensat(atime UTIME_NOW)", detail);

	close(fd);
	unlink(path);
	printf("atime879: %d failed\n", sFailures);
	return sFailures;
}
