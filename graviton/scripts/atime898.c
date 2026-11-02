/*
 * atime898 -- who may set a file's times (issue 898).
 *
 * POSIX utimensat()/futimens(), and utimes()/utime() with a NULL times:
 *   - setting both times to the current time (times == NULL, or UTIME_NOW
 *     for both) is allowed to the owner, to a process with write access to
 *     the file, and to root;
 *   - anything else that sets a time needs the owner or root (EPERM), write
 *     access is not enough: an explicit time, for either or both times, and
 *     also UTIME_NOW for one time with UTIME_OMIT for the other (POSIX:
 *     "does not have both tv_nsec fields set to UTIME_NOW"; Linux agrees);
 *   - UTIME_OMIT for both times succeeds without any permission check.
 *
 * Each case runs in a child that has switched to the identity under test, on a
 * fresh file whose times were set to a known old value first. Every outcome is
 * checked twice: the call's result, and the times stat() reports afterwards
 * (changed as requested on success, untouched on failure).
 *
 * A denied "now" request is EACCES in POSIX and on Linux; it is counted as a
 * pass with EPERM too, but reported as an errno note (see the summary).
 *
 * Usage: atime898 dir...                     (must run as root)
 *        atime898 --as uid gid cmd args...   run cmd as uid/gid, report status
 * Build: cc -O2 -Wall -o atime898 atime898.c      Exit status: failures.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define OWNER_UID	1001
#define OTHER_UID	1002
#define OLD_TIME	1000000000		/* 2001-09-09 */
#define A_TIME		1234567890		/* explicit access time */
#define M_TIME		1111111111		/* explicit modification time */
#define NSEC		500000000

static int sFailures, sCases, sErrnoNotes;

enum who { ROOT, OWNER, WRITER, NONWRITER };
static const char* kWhoName[] = { "root", "owner", "writer", "nonwriter" };

/* What a call asks for, per time: keep it, set it to now, set it explicitly. */
enum want { KEEP, NOW, SET };

struct op {
	const char*	name;
	enum want	atime, mtime;
	int			kind;	/* 0 utimensat, 1 futimens O_RDONLY, 2 utimes, 3 utime */
	int			null;	/* times == NULL */
	struct timespec	ts[2];
};

#define TS(s, n) { (s), (n) }
static const struct op kOps[] = {
	{ "utimensat(NULL)",          NOW,  NOW,  0, 1, { TS(0, 0), TS(0, 0) } },
	{ "utimensat{NOW,NOW}",       NOW,  NOW,  0, 0, { TS(0, UTIME_NOW), TS(0, UTIME_NOW) } },
	{ "utimensat{NOW,OMIT}",      NOW,  KEEP, 0, 0, { TS(0, UTIME_NOW), TS(0, UTIME_OMIT) } },
	{ "utimensat{OMIT,NOW}",      KEEP, NOW,  0, 0, { TS(0, UTIME_OMIT), TS(0, UTIME_NOW) } },
	{ "utimensat{a,m}",           SET,  SET,  0, 0, { TS(A_TIME, NSEC), TS(M_TIME, NSEC) } },
	{ "utimensat{a,OMIT}",        SET,  KEEP, 0, 0, { TS(A_TIME, NSEC), TS(0, UTIME_OMIT) } },
	{ "utimensat{OMIT,m}",        KEEP, SET,  0, 0, { TS(0, UTIME_OMIT), TS(M_TIME, NSEC) } },
	{ "utimensat{NOW,m}",         NOW,  SET,  0, 0, { TS(0, UTIME_NOW), TS(M_TIME, NSEC) } },
	{ "utimensat{a,NOW}",         SET,  NOW,  0, 0, { TS(A_TIME, NSEC), TS(0, UTIME_NOW) } },
	{ "utimensat{OMIT,OMIT}",     KEEP, KEEP, 0, 0, { TS(0, UTIME_OMIT), TS(0, UTIME_OMIT) } },
	{ "futimens(NULL)",           NOW,  NOW,  1, 1, { TS(0, 0), TS(0, 0) } },
	{ "futimens{a,OMIT}",         SET,  KEEP, 1, 0, { TS(A_TIME, NSEC), TS(0, UTIME_OMIT) } },
	{ "futimens{OMIT,OMIT}",      KEEP, KEEP, 1, 0, { TS(0, UTIME_OMIT), TS(0, UTIME_OMIT) } },
	{ "utimes(NULL)",             NOW,  NOW,  2, 1, { TS(0, 0), TS(0, 0) } },
	{ "utimes{a,m}",              SET,  SET,  2, 0, { TS(A_TIME, NSEC), TS(M_TIME, NSEC) } },
	{ "utime(NULL)",              NOW,  NOW,  3, 1, { TS(0, 0), TS(0, 0) } },
	{ "utime{a,m}",               SET,  SET,  3, 0, { TS(A_TIME, 0), TS(M_TIME, 0) } },
};


static void
become(uid_t uid, gid_t gid)
{
	if (setgroups(0, NULL) != 0 || setgid(gid) != 0 || setuid(uid) != 0) {
		fprintf(stderr, "cannot become %d:%d: %s\n", (int)uid, (int)gid,
			strerror(errno));
		_exit(125);
	}
}


/* Runs the call in the current process; returns 0 or an errno. */
static int
do_op(const struct op* op, const char* path)
{
	const struct timespec* t = op->null ? NULL : op->ts;
	int r, fd;

	switch (op->kind) {
		case 0:
			r = utimensat(AT_FDCWD, path, t, 0);
			break;
		case 1:
			fd = open(path, O_RDONLY);
			if (fd < 0)
				return errno;
			r = futimens(fd, t) == 0 ? 0 : errno;
			close(fd);
			return r;
		case 2: {
			struct timeval tv[2];
			tv[0].tv_sec = op->ts[0].tv_sec;
			tv[0].tv_usec = op->ts[0].tv_nsec / 1000;
			tv[1].tv_sec = op->ts[1].tv_sec;
			tv[1].tv_usec = op->ts[1].tv_nsec / 1000;
			r = utimes(path, op->null ? NULL : tv);
			break;
		}
		default: {
			struct utimbuf ub;
			ub.actime = op->ts[0].tv_sec;
			ub.modtime = op->ts[1].tv_sec;
			r = utime(path, op->null ? NULL : &ub);
			break;
		}
	}
	return r == 0 ? 0 : errno;
}


/* Is one time what the call asked for (or untouched, if it failed)? */
static int
time_ok(enum want want, int succeeded, const struct timespec* got,
	const struct timespec* asked, time_t t0, time_t t1)
{
	if (!succeeded || want == KEEP)
		return got->tv_sec == OLD_TIME && got->tv_nsec == 0;
	if (want == NOW)
		return got->tv_sec >= t0 - 1 && got->tv_sec <= t1 + 1;
	/* explicit: the file system may round to its resolution, allow 1 ms */
	long diff = got->tv_nsec - asked->tv_nsec;
	return got->tv_sec == asked->tv_sec && diff > -1000000 && diff < 1000000;
}


static void
run_case(const char* dir, enum who who, const struct op* op)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/atime898.%d.%s.%d", dir, (int)getpid(),
		kWhoName[who], sCases);
	unlink(path);
	int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	if (fd < 0 || write(fd, "x", 1) != 1) {
		printf("FAIL %-9s %-22s cannot create %s: %s\n", kWhoName[who],
			op->name, path, strerror(errno));
		sFailures++;
		return;
	}
	close(fd);

	/* The owner gets a read-only file: ownership alone must be enough. */
	mode_t mode = who == WRITER ? 0666 : who == OWNER ? 0444 : 0644;
	struct timespec old[2] = { TS(OLD_TIME, 0), TS(OLD_TIME, 0) };
	if (chown(path, OWNER_UID, OWNER_UID) != 0 || chmod(path, mode) != 0
		|| utimensat(AT_FDCWD, path, old, 0) != 0) {
		printf("FAIL %-9s %-22s setup: %s\n", kWhoName[who], op->name,
			strerror(errno));
		sFailures++;
		unlink(path);
		return;
	}

	/* Haiku's _exit() flushes stdio (issue 919): empty the buffer first, or
	   the child would print it again. */
	fflush(stdout);
	/* The errno comes back through a pipe: Haiku's errno values are negative
	   and do not survive an 8-bit exit status. */
	int fds[2];
	if (pipe(fds) != 0) {
		printf("FAIL %-9s %-22s pipe: %s\n", kWhoName[who], op->name,
			strerror(errno));
		sFailures++;
		unlink(path);
		return;
	}
	time_t t0 = time(NULL);
	pid_t child = fork();
	if (child == 0) {
		close(fds[0]);
		if (who == OWNER)
			become(OWNER_UID, OWNER_UID);
		else if (who != ROOT)
			become(OTHER_UID, OTHER_UID);
		int childErr = do_op(op, path);
		_exit(write(fds[1], &childErr, sizeof(childErr)) == sizeof(childErr)
			? 0 : 1);
	}
	close(fds[1]);
	int err = -1;
	ssize_t got = read(fds[0], &err, sizeof(err));
	close(fds[0]);
	int status;
	waitpid(child, &status, 0);
	time_t t1 = time(NULL);
	if (got != sizeof(err) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("FAIL %-9s %-22s child failed (status %#x)\n", kWhoName[who],
			op->name, status);
		sFailures++;
		unlink(path);
		return;
	}

	struct stat st;
	stat(path, &st);
	unlink(path);
	sCases++;

	/* expected outcome: only "both now" is open to a writer */
	int anyTime = op->atime != KEEP || op->mtime != KEEP;
	int explicitTime = anyTime && !(op->atime == NOW && op->mtime == NOW);
	int allowed = who == ROOT || who == OWNER || !anyTime
		|| (!explicitTime && who == WRITER);

	int ok, errnoNote = 0;
	char detail[200];
	if (allowed)
		ok = err == 0;
	else if (explicitTime)
		ok = err == EPERM;
	else {
		ok = err == EACCES || err == EPERM;
		errnoNote = err == EPERM;
	}
	int succeeded = err == 0;
	int atimeOk = time_ok(op->atime, succeeded, &st.st_atim, &op->ts[0], t0, t1);
	int mtimeOk = time_ok(op->mtime, succeeded, &st.st_mtim, &op->ts[1], t0, t1);
	ok = ok && atimeOk && mtimeOk;

	snprintf(detail, sizeof(detail), "-> %-7s (expect %-7s) atime %ld.%09ld%s"
		" mtime %ld.%09ld%s%s", err == 0 ? "ok" : err == EPERM ? "EPERM"
			: err == EACCES ? "EACCES" : strerror(err),
		allowed ? "ok" : explicitTime ? "EPERM" : "EACCES",
		(long)st.st_atim.tv_sec, st.st_atim.tv_nsec, atimeOk ? "" : "(BAD)",
		(long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec, mtimeOk ? "" : "(BAD)",
		errnoNote ? "  [errno note: POSIX says EACCES]" : "");
	printf("%s %-9s %-22s %s\n", ok ? "PASS" : "FAIL", kWhoName[who],
		op->name, detail);
	if (!ok)
		sFailures++;
	if (errnoNote)
		sErrnoNotes++;
}


int
main(int argc, char** argv)
{
	if (argc > 4 && strcmp(argv[1], "--as") == 0) {
		fflush(stdout);
		pid_t child = fork();
		if (child == 0) {
			become(atoi(argv[2]), atoi(argv[3]));
			execvp(argv[4], argv + 4);
			fprintf(stderr, "exec %s: %s\n", argv[4], strerror(errno));
			_exit(127);
		}
		int status;
		waitpid(child, &status, 0);
		int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
		printf("as %s:%s: %s -> exit %d\n", argv[2], argv[3], argv[4], rc);
		return rc;
	}

	if (argc < 2 || geteuid() != 0) {
		fprintf(stderr, "usage: atime898 dir... (as root)\n"
			"       atime898 --as uid gid cmd args...\n");
		return 2;
	}

	for (int d = 1; d < argc; d++) {
		printf("== %s\n", argv[d]);
		for (int w = ROOT; w <= NONWRITER; w++) {
			for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++)
				run_case(argv[d], w, &kOps[i]);
		}
	}

	printf("SUMMARY cases=%d failures=%d errno-notes=%d\n", sCases, sFailures,
		sErrnoNotes);
	return sFailures > 255 ? 255 : sFailures;
}
