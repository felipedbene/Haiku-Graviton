/*
 * chown970 -- chown()/chgrp() on a volume that stores no owner (issue 970).
 *
 * FAT stores no owner: Haiku's driver makes the volume's uid and gid the owner
 * of every node. Linux vfat (without its "quiet" option) lets a chown()/chgrp()
 * to those ids through as a no-op and refuses any other id with EPERM, so a
 * caller learns that its change was not made. Before the fix, Haiku reported
 * success for root's chown() to another id and changed nothing.
 *
 * Every case runs on a fresh file (or directory) under each dir given, as root
 * and as uid 1002 (not the owner), through chown(), lchown() and fchown():
 *   - root, to the current uid and/or gid (or -1): ok, nothing changes;
 *   - root, to another uid or gid (alone or with the current other id): EPERM,
 *     nothing changes; with -o (a file system that stores owners, the control)
 *     ok, and stat() must then show the new ids;
 *   - uid 1002, to any id, including the current owner: EPERM (only the owner
 *     or root may chown; POSIX, and Linux's chown_ok());
 *   - anyone, chown(-1, -1), which asks for nothing: ok.
 * Every outcome is checked twice: the call's result, and what stat() reports
 * afterwards (uid, gid, mode, size and times untouched unless the call
 * succeeded with a change).
 *
 * Usage: chown970 [-o] dir...      (must run as root)
 * Build: cc -O2 -Wall -o chown970 chown970.c      Exit status: failures.
 */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define OTHER_ID	1002
#define CUR			((long)-2)	/* the node's current id */
#define KEEP		((long)-1)	/* -1: leave the id alone */

enum call { C_CHOWN, C_LCHOWN, C_FCHOWN };
enum who { ROOT, OTHER };

struct op {
	const char*	name;
	enum call	call;
	long		uid;	/* CUR, KEEP or an id */
	long		gid;
	int			dir;	/* on a directory instead of a file */
};

static const struct op kOps[] = {
	{ "chown(cur,-1)",       C_CHOWN,  CUR,      KEEP,     0 },
	{ "chown(-1,cur)",       C_CHOWN,  KEEP,     CUR,      0 },
	{ "chown(cur,cur)",      C_CHOWN,  CUR,      CUR,      0 },
	{ "chown(-1,-1)",        C_CHOWN,  KEEP,     KEEP,     0 },
	{ "chown(1002,-1)",      C_CHOWN,  OTHER_ID, KEEP,     0 },
	{ "chown(-1,1002)",      C_CHOWN,  KEEP,     OTHER_ID, 0 },
	{ "chown(1002,1002)",    C_CHOWN,  OTHER_ID, OTHER_ID, 0 },
	{ "chown(cur,1002)",     C_CHOWN,  CUR,      OTHER_ID, 0 },
	{ "chown(1002,cur)",     C_CHOWN,  OTHER_ID, CUR,      0 },
	{ "lchown(cur,-1)",      C_LCHOWN, CUR,      KEEP,     0 },
	{ "lchown(1002,-1)",     C_LCHOWN, OTHER_ID, KEEP,     0 },
	{ "fchown(cur,cur)",     C_FCHOWN, CUR,      CUR,      0 },
	{ "fchown(1002,-1)",     C_FCHOWN, OTHER_ID, KEEP,     0 },
	{ "fchown(-1,1002)",     C_FCHOWN, KEEP,     OTHER_ID, 0 },
	{ "dir chown(cur,cur)",  C_CHOWN,  CUR,      CUR,      1 },
	{ "dir chown(1002,-1)",  C_CHOWN,  OTHER_ID, KEEP,     1 },
	{ "dir chown(-1,1002)",  C_CHOWN,  KEEP,     OTHER_ID, 1 },
};

static int sStoresOwners;
static int sCases, sFailures;


static const char*
err_name(int err)
{
	static char buffer[32];
	switch (err) {
		case 0:			return "ok";
		case EPERM:		return "EPERM";
		case EACCES:	return "EACCES";
		case EINVAL:	return "EINVAL";
		case ENOTSUP:	return "ENOTSUP";
		case EROFS:		return "EROFS";
	}
	snprintf(buffer, sizeof(buffer), "errno%d", err);
	return buffer;
}


static long
resolve(long id, long current)
{
	return id == CUR ? current : id;
}


/* Runs the call as `who` in a child; returns 0 or an errno. */
static int
run_as(enum who who, const struct op* op, const char* path, uid_t uid, gid_t gid)
{
	/* the errno travels through a pipe: an exit status keeps only 8 bits,
	   and Haiku's errno values are large negative numbers */
	int fds[2];
	if (pipe(fds) != 0)
		return errno;
	pid_t pid = fork();
	if (pid < 0) {
		int err = errno;
		close(fds[0]);
		close(fds[1]);
		return err;
	}
	if (pid == 0) {
		close(fds[0]);
		if (who == OTHER && (setgroups(0, NULL) != 0 || setgid(OTHER_ID) != 0
				|| setuid(OTHER_ID) != 0))
			_exit(125);
		int r, fd;
		switch (op->call) {
			case C_CHOWN:
				r = chown(path, uid, gid);
				break;
			case C_LCHOWN:
				r = lchown(path, uid, gid);
				break;
			default:
				fd = open(path, O_RDONLY);
				if (fd < 0) {
					r = -1;
					break;
				}
				r = fchown(fd, uid, gid);
				break;
		}
		int err = r == 0 ? 0 : errno;
		_exit(write(fds[1], &err, sizeof(err)) == sizeof(err) ? 0 : 126);
	}
	close(fds[1]);
	int err = -1, status;
	if (read(fds[0], &err, sizeof(err)) != sizeof(err))
		err = -1;
	close(fds[0]);
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)
			|| WEXITSTATUS(status) != 0)
		return -1;
	return err;
}


static void
run_case(const char* base, enum who who, const struct op* op)
{
	const char* whoName = who == ROOT ? "root" : "uid1002";
	char path[1024];
	snprintf(path, sizeof(path), "%s/c970-%s-%d", base, whoName, sCases);
	sCases++;

	int made = op->dir ? mkdir(path, 0755) : -1;
	if (!op->dir) {
		int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		made = fd < 0 ? -1 : (write(fd, "x", 1) == 1 ? 0 : -1);
		if (fd >= 0)
			close(fd);
	}
	struct stat before, st;
	if (made != 0 || stat(path, &before) != 0) {
		printf("FAIL %-7s %-20s setup: %s\n", whoName, op->name, strerror(errno));
		sFailures++;
		return;
	}

	long uid = resolve(op->uid, before.st_uid);
	long gid = resolve(op->gid, before.st_gid);
	int change = (uid != KEEP && uid != (long)before.st_uid)
		|| (gid != KEEP && gid != (long)before.st_gid);

	int err = run_as(who, op, path, (uid_t)uid, (gid_t)gid);
	if (stat(path, &st) != 0) {
		printf("FAIL %-7s %-20s stat afterwards: %s\n", whoName, op->name,
			strerror(errno));
		sFailures++;
		return;
	}

	const char* expect;
	int ok;
	if (uid == KEEP && gid == KEEP) {
		/* asks for nothing: anyone may (Linux, and every Haiku file system) */
		expect = "ok";
		ok = err == 0;
	} else if (who == OTHER) {
		expect = "EPERM";
		ok = err == EPERM;
	} else if (!change) {
		expect = "ok";
		ok = err == 0;
	} else if (sStoresOwners) {
		expect = "ok";
		ok = err == 0;
	} else {
		expect = "EPERM";
		ok = err == EPERM;
	}

	/* what stat() must show: the new ids only after a successful change */
	uid_t wantUid = before.st_uid;
	gid_t wantGid = before.st_gid;
	if (err == 0) {
		if (uid != KEEP)
			wantUid = (uid_t)uid;
		if (gid != KEEP)
			wantGid = (gid_t)gid;
	}
	int idsOk = st.st_uid == wantUid && st.st_gid == wantGid;
	/* chown() does not touch the data, the mode's permission bits (beyond
	   clearing set-id bits, none here) or the access/modification times */
	int restOk = st.st_size == before.st_size
		&& (st.st_mode & 07777) == (before.st_mode & 07777)
		&& st.st_mtime == before.st_mtime && st.st_atime == before.st_atime;
	ok = ok && idsOk && restOk;

	printf("%s %-7s %-20s -> %-7s (expect %-5s) uid %d->%d gid %d->%d%s"
		" mode %04o size %ld mtime %ld atime %ld%s\n",
		ok ? "PASS" : "FAIL", whoName, op->name, err_name(err), expect,
		(int)before.st_uid, (int)st.st_uid, (int)before.st_gid,
		(int)st.st_gid, idsOk ? "" : "(BAD)",
		(unsigned)(st.st_mode & 07777), (long)st.st_size, (long)st.st_mtime,
		(long)st.st_atime, restOk ? "" : " (BAD: changed)");
	if (!ok)
		sFailures++;
}


int
main(int argc, char** argv)
{
	int first = 1;
	if (argc > 1 && strcmp(argv[1], "-o") == 0) {
		sStoresOwners = 1;
		first = 2;
	}
	if (argc <= first || geteuid() != 0) {
		fprintf(stderr, "usage: chown970 [-o] dir... (as root)\n");
		return 2;
	}

	for (int d = first; d < argc; d++) {
		printf("== %s (%s)\n", argv[d], sStoresOwners
			? "stores owners: root's chown to another id must change it"
			: "stores no owner: another id must be refused with EPERM");
		for (int w = ROOT; w <= OTHER; w++) {
			for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++)
				run_case(argv[d], (enum who)w, &kOps[i]);
		}
	}

	printf("SUMMARY cases=%d failures=%d\n", sCases, sFailures);
	return sFailures > 255 ? 255 : sFailures;
}
