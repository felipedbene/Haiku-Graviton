/*
 * atime934 -- who may truncate a file and set its times, on a FAT volume
 * (issue 934).
 *
 * This is atime898's matrix for a volume where chown() and chmod() cannot set
 * up the identities atime898 needs. FAT stores no owner: Haiku's driver makes
 * the uid that mounted the volume the owner of every node, and the only mode
 * bit it stores is the owner's write bit (the read-only attribute), so a file
 * on a volume root mounted is 0644 or 0444. Each file is used as the volume
 * presents it, and the identity under test is classified from what stat()
 * reports:
 *   - root;
 *   - uid 1002 (not the owner, not in the group): "writer" if the mode gives
 *     others write access, "nonwriter" if it does not.
 * On Linux, mount vfat with uid=0,gid=0,fmask=0022,dmask=0022,tz=UTC to match.
 *
 * FAT's resolution is kept as it is: the access time is a date only, the
 * modification time has 2 s granularity. So the explicit times used here are
 * midnights (UTC; both systems are expected to run in UTC, which the run
 * records), and a time set to "now" is accepted from the start of the current
 * day for the access time and from 2 s before the call for the others.
 *
 * The rules checked, per POSIX:
 *   - utimensat()/futimens()/utimes()/utime(): as atime898. Setting both times
 *     to the current time is open to anyone with write access; any other time
 *     change needs the owner or root (EPERM); UTIME_OMIT for both always works.
 *   - truncate()/ftruncate(): need write access (EACCES; EPERM is counted as a
 *     pass with an errno note). A successful size change marks the
 *     modification and change times for update: both must be "now" after it,
 *     the access time untouched.
 *   - chmod()/chown(): need the owner or root (EPERM). The mode change tried
 *     is the owner's write bit, the one bit FAT stores. FAT cannot store an
 *     owner, so root's chown() to another uid may fail (any error is reported,
 *     not failed), but it must not report success without changing the owner.
 * Every outcome is checked twice: the call's result, and what stat() reports
 * afterwards (changed as requested on success, untouched on failure).
 *
 * Usage: atime934 dir...      (must run as root)
 * Build: cc -O2 -Wall -o atime934 atime934.c      Exit status: failures.
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

#define OTHER_UID	1002
#define DAY			86400
#define OLD_TIME	999993600		/* 2001-09-09 00:00:00 UTC */
#define A_TIME		1234483200		/* explicit access time, 2009-02-13 */
#define M_TIME		1111104000		/* explicit modification time, 2005-03-18 */
#define NSEC		0
#define NEW_SIZE	3

static int sFailures, sCases, sErrnoNotes, sUnsupported;

enum who { ROOT, OTHER };

/* What a call asks for, per time: keep it, set it to now, set it explicitly. */
enum want { KEEP, NOW, SET };

enum kind {
	K_UTIMENSAT, K_FUTIMENS, K_UTIMES, K_UTIME,	/* time setters */
	K_TRUNCATE, K_FTRUNCATE,					/* need write access */
	K_CHMOD_SAME, K_CHMOD_OTHER, K_CHOWN_OTHER	/* need the owner */
};

struct op {
	const char*	name;
	enum want	atime, mtime;
	enum kind	kind;
	int			null;	/* times == NULL */
	struct timespec	ts[2];
};

#define TS(s, n) { (s), (n) }
static const struct op kOps[] = {
	{ "utimensat(NULL)",      NOW,  NOW,  K_UTIMENSAT, 1, { TS(0, 0), TS(0, 0) } },
	{ "utimensat{NOW,NOW}",   NOW,  NOW,  K_UTIMENSAT, 0, { TS(0, UTIME_NOW), TS(0, UTIME_NOW) } },
	{ "utimensat{NOW,OMIT}",  NOW,  KEEP, K_UTIMENSAT, 0, { TS(0, UTIME_NOW), TS(0, UTIME_OMIT) } },
	{ "utimensat{OMIT,NOW}",  KEEP, NOW,  K_UTIMENSAT, 0, { TS(0, UTIME_OMIT), TS(0, UTIME_NOW) } },
	{ "utimensat{a,m}",       SET,  SET,  K_UTIMENSAT, 0, { TS(A_TIME, NSEC), TS(M_TIME, NSEC) } },
	{ "utimensat{a,OMIT}",    SET,  KEEP, K_UTIMENSAT, 0, { TS(A_TIME, NSEC), TS(0, UTIME_OMIT) } },
	{ "utimensat{OMIT,m}",    KEEP, SET,  K_UTIMENSAT, 0, { TS(0, UTIME_OMIT), TS(M_TIME, NSEC) } },
	{ "utimensat{NOW,m}",     NOW,  SET,  K_UTIMENSAT, 0, { TS(0, UTIME_NOW), TS(M_TIME, NSEC) } },
	{ "utimensat{a,NOW}",     SET,  NOW,  K_UTIMENSAT, 0, { TS(A_TIME, NSEC), TS(0, UTIME_NOW) } },
	{ "utimensat{OMIT,OMIT}", KEEP, KEEP, K_UTIMENSAT, 0, { TS(0, UTIME_OMIT), TS(0, UTIME_OMIT) } },
	{ "futimens(NULL)",       NOW,  NOW,  K_FUTIMENS,  1, { TS(0, 0), TS(0, 0) } },
	{ "futimens{a,OMIT}",     SET,  KEEP, K_FUTIMENS,  0, { TS(A_TIME, NSEC), TS(0, UTIME_OMIT) } },
	{ "futimens{OMIT,OMIT}",  KEEP, KEEP, K_FUTIMENS,  0, { TS(0, UTIME_OMIT), TS(0, UTIME_OMIT) } },
	{ "utimes(NULL)",         NOW,  NOW,  K_UTIMES,    1, { TS(0, 0), TS(0, 0) } },
	{ "utimes{a,m}",          SET,  SET,  K_UTIMES,    0, { TS(A_TIME, NSEC), TS(M_TIME, NSEC) } },
	{ "utime(NULL)",          NOW,  NOW,  K_UTIME,     1, { TS(0, 0), TS(0, 0) } },
	{ "utime{a,m}",           SET,  SET,  K_UTIME,     0, { TS(A_TIME, 0), TS(M_TIME, 0) } },
	/* the times here are what a successful call must leave behind */
	{ "truncate(3)",          KEEP, NOW,  K_TRUNCATE,  0, { TS(0, 0), TS(0, 0) } },
	{ "ftruncate(3)",         KEEP, NOW,  K_FTRUNCATE, 0, { TS(0, 0), TS(0, 0) } },
	{ "chmod(same mode)",     KEEP, KEEP, K_CHMOD_SAME,  0, { TS(0, 0), TS(0, 0) } },
	{ "chmod(other mode)",    KEEP, KEEP, K_CHMOD_OTHER, 0, { TS(0, 0), TS(0, 0) } },
	{ "chown(1002)",          KEEP, KEEP, K_CHOWN_OTHER, 0, { TS(0, 0), TS(0, 0) } },
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
do_op(const struct op* op, const char* path, mode_t mode)
{
	const struct timespec* t = op->null ? NULL : op->ts;
	int r, fd;

	switch (op->kind) {
		case K_UTIMENSAT:
			r = utimensat(AT_FDCWD, path, t, 0);
			break;
		case K_FUTIMENS:
			fd = open(path, O_RDONLY);
			if (fd < 0)
				return errno;
			r = futimens(fd, t) == 0 ? 0 : errno;
			close(fd);
			return r;
		case K_UTIMES: {
			struct timeval tv[2];
			tv[0].tv_sec = op->ts[0].tv_sec;
			tv[0].tv_usec = op->ts[0].tv_nsec / 1000;
			tv[1].tv_sec = op->ts[1].tv_sec;
			tv[1].tv_usec = op->ts[1].tv_nsec / 1000;
			r = utimes(path, op->null ? NULL : tv);
			break;
		}
		case K_UTIME: {
			struct utimbuf ub;
			ub.actime = op->ts[0].tv_sec;
			ub.modtime = op->ts[1].tv_sec;
			r = utime(path, op->null ? NULL : &ub);
			break;
		}
		case K_TRUNCATE:
			r = truncate(path, NEW_SIZE);
			break;
		case K_FTRUNCATE:
			/* opening for writing needs write access itself */
			fd = open(path, O_WRONLY);
			if (fd < 0)
				return errno;
			r = ftruncate(fd, NEW_SIZE) == 0 ? 0 : errno;
			close(fd);
			return r;
		case K_CHMOD_SAME:
			r = chmod(path, mode & 07777);
			break;
		case K_CHMOD_OTHER:
			r = chmod(path, (mode & 07777) ^ S_IWUSR);
			break;
		case K_CHOWN_OTHER:
		default:
			r = chown(path, OTHER_UID, (gid_t)-1);
			break;
	}
	return r == 0 ? 0 : errno;
}


/* Is one time what the call asked for (or untouched, if it failed)? FAT keeps
   the access time as a date, the others in 2 s steps: "now" may read back as
   anything from the start of the day, or 2 s, before the call. The explicit
   times are representable exactly. */
static int
time_ok(enum want want, int succeeded, const struct timespec* got,
	const struct timespec* asked, time_t t0, time_t t1, int dateOnly)
{
	if (!succeeded || want == KEEP)
		return got->tv_sec == OLD_TIME && got->tv_nsec == 0;
	if (want == NOW) {
		time_t earliest = dateOnly ? t0 - t0 % DAY : t0 - 2;
		return got->tv_sec >= earliest && got->tv_sec <= t1 + 1;
	}
	return got->tv_sec == asked->tv_sec && got->tv_nsec == asked->tv_nsec;
}


static const char*
err_name(int err)
{
	switch (err) {
		case 0:			return "ok";
		case EPERM:		return "EPERM";
		case EACCES:	return "EACCES";
		default:		return strerror(err);
	}
}


static void
run_case(const char* dir, enum who who, const struct op* op)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/atime934.%d.%d", dir, (int)getpid(),
		sCases);
	unlink(path);
	int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
	if (fd < 0 || write(fd, "x", 1) != 1) {
		printf("FAIL %-9s %-20s cannot create %s: %s\n",
			who == ROOT ? "root" : "uid1002", op->name, path, strerror(errno));
		sFailures++;
		if (fd >= 0)
			close(fd);
		return;
	}
	close(fd);

	struct timespec old[2] = { TS(OLD_TIME, 0), TS(OLD_TIME, 0) };
	struct stat before;
	if (utimensat(AT_FDCWD, path, old, 0) != 0 || stat(path, &before) != 0) {
		printf("FAIL %-9s %-20s setup: %s\n", who == ROOT ? "root" : "uid1002",
			op->name, strerror(errno));
		sFailures++;
		unlink(path);
		return;
	}

	/* The volume decides the identities; uid 1002 must be neither the owner
	   nor in the group, or the classification below would be wrong. */
	if (before.st_uid == OTHER_UID || before.st_gid == OTHER_UID) {
		printf("FAIL %-9s %-20s setup: file is owned by %d:%d, need another"
			" owner\n", "uid1002", op->name, (int)before.st_uid,
			(int)before.st_gid);
		sFailures++;
		unlink(path);
		return;
	}
	int otherWrites = (before.st_mode & S_IWOTH) != 0;
	const char* whoName = who == ROOT ? "root"
		: otherWrites ? "writer" : "nonwriter";

	/* Haiku's _exit() flushes stdio (issue 919): empty the buffer first, or
	   the child would print it again. */
	fflush(stdout);
	/* The errno comes back through a pipe: Haiku's errno values are negative
	   and do not survive an 8-bit exit status. */
	int fds[2];
	if (pipe(fds) != 0) {
		printf("FAIL %-9s %-20s pipe: %s\n", whoName, op->name,
			strerror(errno));
		sFailures++;
		unlink(path);
		return;
	}
	time_t t0 = time(NULL);
	pid_t child = fork();
	if (child == 0) {
		close(fds[0]);
		if (who != ROOT)
			become(OTHER_UID, OTHER_UID);
		int childErr = do_op(op, path, before.st_mode);
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
		printf("FAIL %-9s %-20s child failed (status %#x)\n", whoName,
			op->name, status);
		sFailures++;
		unlink(path);
		return;
	}

	struct stat st;
	int statErr = stat(path, &st) == 0 ? 0 : errno;
	unlink(path);
	sCases++;
	if (statErr != 0) {
		printf("FAIL %-9s %-20s stat after: %s\n", whoName, op->name,
			strerror(statErr));
		sFailures++;
		return;
	}

	int isRoot = who == ROOT;
	int allowed, ok, errnoNote = 0, unsupported = 0;
	const char* expect;
	int succeeded = err == 0;
	int sizeOk = st.st_size == (succeeded && (op->kind == K_TRUNCATE
		|| op->kind == K_FTRUNCATE) ? NEW_SIZE : 1);
	int modeOk = 1, ownerOk = 1, ctimeOk = 1;

	switch (op->kind) {
		case K_TRUNCATE:
		case K_FTRUNCATE:
			allowed = isRoot || otherWrites;
			expect = allowed ? "ok" : "EACCES";
			ok = allowed ? err == 0 : err == EACCES || err == EPERM;
			errnoNote = !allowed && err == EPERM;
			/* a size change marks both mtime and ctime for update (FAT
			   has no ctime of its own and reports the mtime) */
			if (succeeded)
				ctimeOk = st.st_ctim.tv_sec >= t0 - 2 && st.st_ctim.tv_sec <= t1 + 1;
			break;
		case K_CHMOD_SAME:
		case K_CHMOD_OTHER:
		case K_CHOWN_OTHER:
			/* only the owner or root; root's chown() may meet a file
			   system that cannot store an owner */
			allowed = isRoot;
			unsupported = isRoot && op->kind == K_CHOWN_OTHER && err != 0;
			expect = allowed ? "ok" : "EPERM";
			ok = allowed ? err == 0 || unsupported : err == EPERM;
			if (op->kind == K_CHOWN_OTHER)
				ownerOk = st.st_uid == (succeeded ? OTHER_UID : before.st_uid);
			else {
				mode_t want = before.st_mode;
				if (succeeded && op->kind == K_CHMOD_OTHER)
					want ^= S_IWUSR;
				modeOk = (st.st_mode & 07777) == (want & 07777);
			}
			break;
		default: {
			/* only "both now" is open to a writer */
			int anyTime = op->atime != KEEP || op->mtime != KEEP;
			int explicitTime = anyTime
				&& !(op->atime == NOW && op->mtime == NOW);
			allowed = isRoot || !anyTime || (!explicitTime && otherWrites);
			expect = allowed ? "ok" : explicitTime ? "EPERM" : "EACCES";
			if (allowed)
				ok = err == 0;
			else if (explicitTime)
				ok = err == EPERM;
			else {
				ok = err == EACCES || err == EPERM;
				errnoNote = err == EPERM;
			}
			break;
		}
	}

	int atimeOk = time_ok(op->atime, succeeded, &st.st_atim, &op->ts[0], t0, t1, 1);
	int mtimeOk = time_ok(op->mtime, succeeded, &st.st_mtim, &op->ts[1], t0, t1, 0);
	ok = ok && atimeOk && mtimeOk && ctimeOk && sizeOk && modeOk && ownerOk;

	char detail[300];
	snprintf(detail, sizeof(detail), "-> %-7s (expect %-6s) atime %ld.%09ld%s"
		" mtime %ld.%09ld%s ctime %ld%s size %ld%s mode %04o%s uid %d%s%s%s",
		err_name(err), expect,
		(long)st.st_atim.tv_sec, st.st_atim.tv_nsec, atimeOk ? "" : "(BAD)",
		(long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec, mtimeOk ? "" : "(BAD)",
		(long)st.st_ctim.tv_sec, ctimeOk ? "" : "(BAD)",
		(long)st.st_size, sizeOk ? "" : "(BAD)",
		(unsigned)(st.st_mode & 07777), modeOk ? "" : "(BAD)",
		(int)st.st_uid, ownerOk ? "" : "(BAD)",
		errnoNote ? "  [errno note: POSIX says EACCES]" : "",
		unsupported ? "  [refused: the file system cannot store it]" : "");
	printf("%s %-9s %-20s %s\n", ok ? "PASS" : "FAIL", whoName, op->name,
		detail);
	if (!ok)
		sFailures++;
	if (errnoNote)
		sErrnoNotes++;
	if (unsupported)
		sUnsupported++;
}


int
main(int argc, char** argv)
{
	if (argc < 2 || geteuid() != 0) {
		fprintf(stderr, "usage: atime934 dir... (as root)\n");
		return 2;
	}

	for (int d = 1; d < argc; d++) {
		printf("== %s\n", argv[d]);
		for (int w = ROOT; w <= OTHER; w++) {
			for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++)
				run_case(argv[d], w, &kOps[i]);
		}
	}

	printf("SUMMARY cases=%d failures=%d errno-notes=%d refused-by-fs=%d\n",
		sCases, sFailures, sErrnoNotes, sUnsupported);
	return sFailures > 255 ? 255 : sFailures;
}
