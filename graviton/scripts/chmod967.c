/*
 * chmod967 -- chmod() to the mode a node already has (issue 967).
 *
 * Haiku's NTFS driver cannot store a mode: every file is 0666, every directory
 * 0777. A chmod() to that mode is a no-op and must succeed, as it does on Linux
 * (ntfs3) and on every other Haiku file system; tools that restore modes
 * (cp -p, tar x, rsync -p, install -m) do it on every file. Before the fix the
 * driver compared the node's full mode (S_IFREG|0666) with the bare permission
 * bits chmod() passes, so every chmod() failed with ENOTSUP, even for root.
 *
 * Every case runs on a fresh file (or directory) under each dir given, as root
 * and as uid 1002 (not the owner), through chmod(), fchmod() and fchmodat():
 *   - root, to the current mode (also with the file type bits passed along):
 *     ok, mode unchanged;
 *   - root, to another mode: either ok with stat() showing the new mode (a file
 *     system that stores modes) or refused with the mode unchanged; the outcome
 *     is reported ("refused-by-fs"), only "ok but unchanged" fails;
 *   - uid 1002, to any mode, including the current one: EPERM (only the owner
 *     or root may chmod).
 * Every outcome is checked twice: the call's result, and what stat() reports
 * afterwards (mode as requested on success, untouched on failure; size and
 * modification time untouched either way).
 *
 * Usage: chmod967 dir...      (must run as root)
 * Build: cc -O2 -Wall -o chmod967 chmod967.c      Exit status: failures.
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

enum call { C_CHMOD, C_FCHMOD, C_FCHMODAT };
enum who { ROOT, OTHER };
enum target { T_SAME, T_SAME_WITH_TYPE, T_OTHER_BITS, T_SETUID };

struct op {
	const char*	name;
	enum call	call;
	enum target	target;
	int			dir;	/* on a directory instead of a file */
};

static const struct op kOps[] = {
	{ "chmod(same)",          C_CHMOD,    T_SAME,           0 },
	{ "chmod(same|S_IFMT)",   C_CHMOD,    T_SAME_WITH_TYPE, 0 },
	{ "fchmod(same)",         C_FCHMOD,   T_SAME,           0 },
	{ "fchmodat(same)",       C_FCHMODAT, T_SAME,           0 },
	{ "chmod(other)",         C_CHMOD,    T_OTHER_BITS,     0 },
	{ "chmod(same|S_ISUID)",  C_CHMOD,    T_SETUID,         0 },
	{ "dir chmod(same)",      C_CHMOD,    T_SAME,           1 },
	{ "dir chmod(other)",     C_CHMOD,    T_OTHER_BITS,     1 },
};

static int sCases, sFailures, sUnsupported;


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


static mode_t
target_mode(enum target target, mode_t current)
{
	switch (target) {
		case T_SAME:			return current & 07777;
		case T_SAME_WITH_TYPE:	return current & (S_IFMT | 07777);
		case T_OTHER_BITS:		return (current & 07777) ^ S_IWOTH;
		default:				return (current & 07777) | S_ISUID;
	}
}


/* Runs the call as `who` in a child; returns 0 or an errno. */
static int
run_as(enum who who, const struct op* op, const char* path, mode_t mode)
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
			case C_CHMOD:
				r = chmod(path, mode);
				break;
			case C_FCHMODAT:
				r = fchmodat(AT_FDCWD, path, mode, 0);
				break;
			default:
				fd = open(path, O_RDONLY);
				if (fd < 0) {
					r = -1;
					break;
				}
				r = fchmod(fd, mode);
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
	snprintf(path, sizeof(path), "%s/m967-%s-%d", base, whoName, sCases);
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
		printf("FAIL %-7s %-20s setup: %s\n", whoName, op->name, strerror(errno));
		sFailures++;
		return;
	}

	mode_t mode = target_mode(op->target, before.st_mode);
	int change = (mode & 07777) != (before.st_mode & 07777);

	int err = run_as(who, op, path, mode);
	if (stat(path, &st) != 0) {
		printf("FAIL %-7s %-20s stat afterwards: %s\n", whoName, op->name,
			strerror(errno));
		sFailures++;
		return;
	}

	const char* expect;
	int ok, unsupported = 0;
	if (who == OTHER) {
		expect = "EPERM";
		ok = err == EPERM;
	} else if (!change) {
		expect = "ok";
		ok = err == 0;
	} else {
		/* a real change: the file system may not be able to store it */
		expect = "ok/ref";
		unsupported = err != 0;
		ok = 1;
	}

	mode_t want = err == 0 ? mode & 07777 : before.st_mode & 07777;
	int modeOk = (st.st_mode & 07777) == want
		&& (st.st_mode & S_IFMT) == (before.st_mode & S_IFMT);
	int restOk = st.st_size == before.st_size && st.st_mtime == before.st_mtime;
	ok = ok && modeOk && restOk;

	printf("%s %-7s %-20s -> %-7s (expect %-6s) mode %06o->%06o (asked %06o)%s"
		" size %ld mtime %ld%s%s\n",
		ok ? "PASS" : "FAIL", whoName, op->name, err_name(err), expect,
		(unsigned)before.st_mode, (unsigned)st.st_mode, (unsigned)mode,
		modeOk ? "" : "(BAD)", (long)st.st_size, (long)st.st_mtime,
		restOk ? "" : " (BAD: changed)",
		unsupported ? "  [refused: the file system cannot store it]" : "");
	if (!ok)
		sFailures++;
	if (unsupported)
		sUnsupported++;
}


int
main(int argc, char** argv)
{
	if (argc < 2 || geteuid() != 0) {
		fprintf(stderr, "usage: chmod967 dir... (as root)\n");
		return 2;
	}

	for (int d = 1; d < argc; d++) {
		printf("== %s\n", argv[d]);
		for (int w = ROOT; w <= OTHER; w++) {
			for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); i++)
				run_case(argv[d], (enum who)w, &kOps[i]);
		}
	}

	printf("SUMMARY cases=%d failures=%d refused-by-fs=%d\n", sCases,
		sFailures, sUnsupported);
	return sFailures > 255 ? 255 : sFailures;
}
