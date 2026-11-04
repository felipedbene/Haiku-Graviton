/*
 * silent967 -- chmod()/chown() on a file system that cannot store them
 * (issue 967, owner decision: ntfs-3g's default "silent" behaviour).
 *
 * Haiku's NTFS driver has no user mapping, so it cannot store a mode or an
 * owner: every node is root's, files 0666, directories 0777. Like ntfs-3g
 * without a user mapping (its default, "silent"), a chmod() or chown() that
 * the caller is allowed to make is accepted and dropped. coreutils install -m
 * creates the file 0600 and fchmod()s it before it sets the final mode, so a
 * refused real change made every install -m fail.
 *
 * The expectations checked here are that policy:
 *   - root, chmod/fchmod/fchmodat to any permission bits, the current ones
 *     or others (also the set-id bits): ok, and stat() still shows the old
 *     mode;
 *   - root, chmod with type bits that differ from the node's (a file passed
 *     S_IFDIR, a directory S_IFREG): refused, mode unchanged;
 *   - root, chown/fchown/lchown to any uid/gid: ok, owner unchanged;
 *   - uid 1002 (not the owner): EPERM for every chmod and every chown that
 *     names an id; chown(-1, -1) changes nothing and is ok.
 * Every case also checks that the size and modification time are untouched;
 * a ctime change is reported, not failed. A file system that stores modes
 * (BFS, ext4) fails the "unchanged" rows by design: run it as a control only
 * with -s ("stores"), which expects the new mode/owner instead, and accepts
 * either outcome for a wrong type as long as the type stays.
 *
 * Usage: silent967 [-s] dir...      (must run as root)
 * Build: cc -O2 -Wall -o silent967 silent967.c      Exit status: failures.
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

enum call { C_CHMOD, C_FCHMOD, C_FCHMODAT, C_CHOWN, C_FCHOWN, C_LCHOWN };
enum who { ROOT, OTHER };
enum target {
	T_SAME, T_SAME_WITH_TYPE, T_OTHER_BITS, T_0600, T_SETUID, T_WRONG_TYPE,
	T_OWN_CUR, T_OWN_UID, T_OWN_GID, T_OWN_BOTH, T_OWN_NONE
};

struct op {
	const char*	name;
	enum call	call;
	enum target	target;
	int			dir;	/* on a directory instead of a file */
};

static const struct op kOps[] = {
	{ "chmod(same)",          C_CHMOD,    T_SAME,           0 },
	{ "chmod(same|S_IFREG)",  C_CHMOD,    T_SAME_WITH_TYPE, 0 },
	{ "chmod(other)",         C_CHMOD,    T_OTHER_BITS,     0 },
	{ "chmod(0600)",          C_CHMOD,    T_0600,           0 },
	{ "fchmod(0600)",         C_FCHMOD,   T_0600,           0 },
	{ "fchmodat(other)",      C_FCHMODAT, T_OTHER_BITS,     0 },
	{ "chmod(same|S_ISUID)",  C_CHMOD,    T_SETUID,         0 },
	{ "chmod(S_IFDIR|same)",  C_CHMOD,    T_WRONG_TYPE,     0 },
	{ "dir chmod(same)",      C_CHMOD,    T_SAME,           1 },
	{ "dir chmod(other)",     C_CHMOD,    T_OTHER_BITS,     1 },
	{ "dir chmod(S_IFREG|..)",C_CHMOD,    T_WRONG_TYPE,     1 },
	{ "chown(cur,cur)",       C_CHOWN,    T_OWN_CUR,        0 },
	{ "chown(-1,-1)",         C_CHOWN,    T_OWN_NONE,       0 },
	{ "chown(1002,-1)",       C_CHOWN,    T_OWN_UID,        0 },
	{ "chown(-1,1002)",       C_CHOWN,    T_OWN_GID,        0 },
	{ "chown(1002,1002)",     C_CHOWN,    T_OWN_BOTH,       0 },
	{ "fchown(1002,1002)",    C_FCHOWN,   T_OWN_BOTH,       0 },
	{ "lchown(1002,-1)",      C_LCHOWN,   T_OWN_UID,        0 },
	{ "dir chown(1002,1002)", C_CHOWN,    T_OWN_BOTH,       1 },
};

static int sCases, sFailures, sStores;


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


static int
is_chown(const struct op* op)
{
	return op->call == C_CHOWN || op->call == C_FCHOWN || op->call == C_LCHOWN;
}


static mode_t
target_mode(enum target target, const struct stat* st)
{
	mode_t current = st->st_mode;
	switch (target) {
		case T_SAME:			return current & 07777;
		case T_SAME_WITH_TYPE:	return current & (S_IFMT | 07777);
		case T_OTHER_BITS:		return (current & 07777) ^ S_IWOTH;
		case T_0600:			return 0600;
		case T_SETUID:			return (current & 07777) | S_ISUID;
		default:
			return (S_ISDIR(current) ? S_IFREG : S_IFDIR) | (current & 07777);
	}
}


static void
target_owner(enum target target, const struct stat* st, uid_t* uid, gid_t* gid)
{
	*uid = (uid_t)-1;
	*gid = (gid_t)-1;
	switch (target) {
		case T_OWN_CUR:		*uid = st->st_uid; *gid = st->st_gid; break;
		case T_OWN_UID:		*uid = OTHER_ID; break;
		case T_OWN_GID:		*gid = OTHER_ID; break;
		case T_OWN_BOTH:	*uid = OTHER_ID; *gid = OTHER_ID; break;
		default:			break;
	}
}


/* Runs the call as `who` in a child; returns 0 or an errno. */
static int
run_as(enum who who, const struct op* op, const char* path, mode_t mode,
	uid_t uid, gid_t gid)
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
		int r = -1, fd;
		switch (op->call) {
			case C_CHMOD:
				r = chmod(path, mode);
				break;
			case C_FCHMODAT:
				r = fchmodat(AT_FDCWD, path, mode, 0);
				break;
			case C_FCHMOD:
				fd = open(path, O_RDONLY);
				if (fd >= 0)
					r = fchmod(fd, mode);
				break;
			case C_CHOWN:
				r = chown(path, uid, gid);
				break;
			case C_LCHOWN:
				r = lchown(path, uid, gid);
				break;
			case C_FCHOWN:
				fd = open(path, O_RDONLY);
				if (fd >= 0)
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
	snprintf(path, sizeof(path), "%s/s967-%s-%d", base, whoName, sCases);
	sCases++;

	int made;
	if (op->dir)
		made = mkdir(path, 0777);
	else {
		int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
		made = fd < 0 ? -1 : (write(fd, "x", 1) == 1 ? 0 : -1);
		if (fd >= 0)
			close(fd);
	}
	struct stat before, st;
	if (made != 0 || stat(path, &before) != 0) {
		printf("FAIL %-7s %-21s setup: %s\n", whoName, op->name,
			strerror(errno));
		sFailures++;
		return;
	}

	mode_t mode = 0;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;
	if (is_chown(op))
		target_owner(op->target, &before, &uid, &gid);
	else
		mode = target_mode(op->target, &before);

	int err = run_as(who, op, path, mode, uid, gid);
	if (stat(path, &st) != 0) {
		printf("FAIL %-7s %-21s stat afterwards: %s\n", whoName, op->name,
			strerror(errno));
		sFailures++;
		return;
	}

	/* what the call should return */
	const char* expect;
	int callOk;
	int names_id = uid != (uid_t)-1 || gid != (gid_t)-1;
	if (who == OTHER && (!is_chown(op) || names_id)) {
		expect = "EPERM";
		callOk = err == EPERM;
	} else if (op->target == T_WRONG_TYPE) {
		/* a file system that stores modes may ignore the type bits instead
		   (BFS and Linux do); the type must not change either way */
		expect = sStores ? "any" : "refuse";
		callOk = sStores || err != 0;
	} else {
		expect = "ok";
		callOk = err == 0;
	}

	/* what stat() should show afterwards: unchanged, unless -s and ok */
	mode_t wantMode = before.st_mode;
	uid_t wantUid = before.st_uid;
	gid_t wantGid = before.st_gid;
	if (sStores && err == 0) {
		if (is_chown(op)) {
			if (uid != (uid_t)-1)
				wantUid = uid;
			if (gid != (gid_t)-1)
				wantGid = gid;
		} else
			wantMode = (before.st_mode & S_IFMT) | (mode & 07777);
	}
	/* only the type and the user-settable bits: BFS sets flags above S_IFMT
	   on directories */
	int stateOk = (st.st_mode & (S_IFMT | 07777))
			== (wantMode & (S_IFMT | 07777))
		&& st.st_uid == wantUid && st.st_gid == wantGid;
	int restOk = st.st_size == before.st_size && st.st_mtime == before.st_mtime;
	int ok = callOk && stateOk && restOk;

	char asked[48];
	if (is_chown(op))
		snprintf(asked, sizeof(asked), "%d:%d", (int)uid, (int)gid);
	else
		snprintf(asked, sizeof(asked), "%06o", (unsigned)mode);
	printf("%s %-7s %-21s -> %-8s (expect %-6s) asked %-9s mode %06o->%06o"
		" uid %d->%d gid %d->%d%s size %ld mtime %ld%s ctime %s\n",
		ok ? "PASS" : "FAIL", whoName, op->name, err_name(err), expect, asked,
		(unsigned)before.st_mode, (unsigned)st.st_mode, (int)before.st_uid,
		(int)st.st_uid, (int)before.st_gid, (int)st.st_gid,
		stateOk ? "" : " (BAD state)", (long)st.st_size, (long)st.st_mtime,
		restOk ? "" : " (BAD: changed)",
		st.st_ctime == before.st_ctime ? "same" : "changed");
	if (!ok)
		sFailures++;
}


int
main(int argc, char** argv)
{
	int first = 1;
	if (argc > 1 && strcmp(argv[1], "-s") == 0) {
		sStores = 1;
		first = 2;
	}
	if (argc <= first || geteuid() != 0) {
		fprintf(stderr, "usage: silent967 [-s] dir... (as root)\n");
		return 2;
	}

	for (int d = first; d < argc; d++) {
		printf("== %s%s\n", argv[d], sStores ? " (-s: expects stored changes)"
			: "");
		for (int w = ROOT; w <= OTHER; w++) {
			for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++)
				run_case(argv[d], (enum who)w, &kOps[i]);
		}
	}

	printf("SUMMARY cases=%d failures=%d\n", sCases, sFailures);
	return sFailures > 255 ? 255 : sFailures;
}
