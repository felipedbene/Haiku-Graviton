/*
 * utime896.c -- issue 896: do utimensat() and futimens() set a UTIME_NOW
 * field to the current time?
 *
 * Each check starts from a file whose times were set explicitly to
 * 1000000000 (2001-09-09); "now" means within 10 s of time().
 *   mtime-now        {UTIME_OMIT, {0, UTIME_NOW}}: mtime now, atime kept
 *   mtime-now-sec    the same with tv_sec 12345 (must be ignored)
 *   atime-now        {{0, UTIME_NOW}, UTIME_OMIT}: atime now, mtime kept
 *                    (needs a file system that stores the atime it is given)
 *   both-now         futimens() with both UTIME_NOW
 *   null             utimensat(NULL): both now (was never affected)
 *   explicit         {{123456789, 500000}, {123456790, 600000}} stored, within
 *                    1 ms (BFS keeps the mtime coarser than a nanosecond)
 *   einval           tv_nsec -7 fails EINVAL (not 1000000001: that is
 *                    UTIME_OMIT on Haiku)
 *   touch-m          touch -m: mtime now
 * Exit status: number of failed checks.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


static const char* kFile = "utime896.dat";
static int sFailed = 0;


static void
reset()
{
	struct timespec old[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
	int fd = open(kFile, O_CREAT | O_WRONLY, 0644);
	close(fd);
	utimensat(AT_FDCWD, kFile, old, 0);
}


static int
is_now(const struct timespec* t)
{
	time_t now = time(NULL);
	return t->tv_sec >= now - 10 && t->tv_sec <= now + 10;
}


static void
report(const char* name, int ok, int rc)
{
	struct stat st;
	stat(kFile, &st);
	printf("%-14s %s  rc=%d atime=%ld.%09ld mtime=%ld.%09ld now=%ld\n", name,
		ok ? "PASS" : "FAIL", rc, (long)st.st_atim.tv_sec,
		(long)st.st_atim.tv_nsec, (long)st.st_mtim.tv_sec,
		(long)st.st_mtim.tv_nsec, (long)time(NULL));
	sFailed += !ok;
}


int
main()
{
	struct stat st;
	int rc;

	reset();
	struct timespec t1[2] = { { 0, UTIME_OMIT }, { 0, UTIME_NOW } };
	rc = utimensat(AT_FDCWD, kFile, t1, 0);
	stat(kFile, &st);
	report("mtime-now", rc == 0 && is_now(&st.st_mtim)
		&& st.st_atim.tv_sec == 1000000000, rc);

	reset();
	struct timespec t2[2] = { { 0, UTIME_OMIT }, { 12345, UTIME_NOW } };
	rc = utimensat(AT_FDCWD, kFile, t2, 0);
	stat(kFile, &st);
	report("mtime-now-sec", rc == 0 && is_now(&st.st_mtim), rc);

	reset();
	struct timespec t3[2] = { { 0, UTIME_NOW }, { 0, UTIME_OMIT } };
	rc = utimensat(AT_FDCWD, kFile, t3, 0);
	stat(kFile, &st);
	report("atime-now", rc == 0 && is_now(&st.st_atim)
		&& st.st_mtim.tv_sec == 1000000000, rc);

	reset();
	struct timespec t4[2] = { { 0, UTIME_NOW }, { 0, UTIME_NOW } };
	int fd = open(kFile, O_WRONLY);
	rc = futimens(fd, t4);
	close(fd);
	stat(kFile, &st);
	report("both-now", rc == 0 && is_now(&st.st_atim) && is_now(&st.st_mtim),
		rc);

	reset();
	rc = utimensat(AT_FDCWD, kFile, NULL, 0);
	stat(kFile, &st);
	report("null", rc == 0 && is_now(&st.st_mtim), rc);

	reset();
	struct timespec t5[2] = { { 123456789, 500000 }, { 123456790, 600000 } };
	rc = utimensat(AT_FDCWD, kFile, t5, 0);
	stat(kFile, &st);
	report("explicit", rc == 0 && st.st_atim.tv_sec == 123456789
		&& labs(st.st_atim.tv_nsec - 500000) < 1000000
		&& st.st_mtim.tv_sec == 123456790
		&& labs(st.st_mtim.tv_nsec - 600000) < 1000000, rc);

	reset();
	struct timespec t6[2] = { { 0, UTIME_OMIT }, { 5, -7 } };
	errno = 0;
	rc = utimensat(AT_FDCWD, kFile, t6, 0);
	int error = errno;
	stat(kFile, &st);
	report("einval", rc == -1 && error == EINVAL
		&& st.st_mtim.tv_sec == 1000000000, rc);

	reset();
	rc = system("touch -m utime896.dat");
	stat(kFile, &st);
	report("touch-m", rc == 0 && is_now(&st.st_mtim), rc);

	unlink(kFile);
	printf("utime896: %d failed\n", sFailed);
	return sFailed;
}
