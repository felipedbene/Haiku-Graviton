/*
 * sticky1008.c -- issue 1008: POSIX restricted deletion in sticky directories,
 * and the write permission rmdir() needs.
 *
 * Run as root: sticky1008 [-x <op>]... [base-directory]   (default /tmp)
 *   -x <op>  skip every row of operation <op> (printed as SKIP, not counted): for a
 *            file system with an unrelated defect in that operation that would
 *            corrupt the tree the later rows need (ext2's rmdir of a non-empty
 *            directory). Always also run the full matrix without it.
 *
 * Under <base>/sticky1008.<pid> (root, 0755) it makes three directories:
 *   S  1777, owned by the "directory owner" user C
 *   N  0777, owned by C (no sticky bit: today's behaviour must stay)
 *   W  0755, owned by root (only root may remove entries)
 * and for every (directory, actor, operation) recreates, as root, a file "f"
 * and the directories "de" (empty) and "dn" (not empty) owned by user A, plus
 * a file "mine" owned by the actor, then performs the operation in a child
 * that has dropped to the actor's uid/gid. Actors: A (owner of the entries),
 * B (an unrelated user), C (owner of the directory) and root.
 *
 * The expected result of each row is what POSIX specifies and Linux does
 * (may_delete(): write+search on the directory, then check_sticky(), EPERM).
 * Every row prints got/expected; the last line is the failure count, and the
 * exit status is non-zero if any row failed.
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define UID_A	2001
#define UID_B	2002
#define UID_C	2003

static char sBase[1024];
static int sFailures;
static int sRows;
static int sSkipped;
static bool sSkipOp[16];

struct actor {
	const char*	name;
	uid_t		uid;
};

static const struct actor kActors[] = {
	{ "A(entry-owner)", UID_A },
	{ "B(other-user)", UID_B },
	{ "C(dir-owner)", UID_C },
	{ "root", 0 },
};

enum {
	OP_CREATE,			/* open(dir/new, O_CREAT|O_EXCL) */
	OP_UNLINK,			/* unlink(dir/f) */
	OP_RENAME_AWAY,		/* rename(dir/f, dir/g) */
	OP_RENAME_OUT,		/* rename(dir/f, <base>/out/f.<actor>) */
	OP_RENAME_OVER,		/* rename(dir/mine, dir/f) */
	OP_RMDIR_EMPTY,		/* rmdir(dir/de) */
	OP_RMDIR_FULL,		/* rmdir(dir/dn) */
	OP_COUNT
};

static const char* kOpNames[OP_COUNT] = {
	"create-own", "unlink-f", "rename-f-away", "rename-f-out",
	"rename-own-over-f", "rmdir-empty-de", "rmdir-nonempty-dn"
};


static const char*
errno_name(int error)
{
	switch (error) {
		case 0:			return "OK";
		case EPERM:		return "EPERM";
		case EACCES:	return "EACCES";
		case ENOTEMPTY:	return "ENOTEMPTY";
		case EEXIST:	return "EEXIST";
		case ENOENT:	return "ENOENT";
		case EISDIR:	return "EISDIR";
		case ENOTDIR:	return "ENOTDIR";
		default:		return strerror(error);
	}
}


static void
die(const char* what, const char* path)
{
	fprintf(stderr, "setup: %s(%s): %s\n", what, path, strerror(errno));
	exit(2);
}


static void
make_file(const char* path, uid_t owner)
{
	int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (fd < 0)
		die("open", path);
	if (fchown(fd, owner, owner) != 0)
		die("fchown", path);
	close(fd);
}


static void
make_dir(const char* path, mode_t mode, uid_t owner)
{
	if (mkdir(path, 0700) != 0)
		die("mkdir", path);
	if (chown(path, owner, owner) != 0)
		die("chown", path);
	/* chmod() after mkdir(), so the umask cannot drop S_ISVTX */
	if (chmod(path, mode) != 0)
		die("chmod", path);
}


/* Remove (as root) whatever an earlier row left behind in a directory. */
static void
remove_quiet(const char* path)
{
	if (unlink(path) != 0 && errno != ENOENT)
		die("unlink", path);
}


static void
reset_entries(const char* dir, uid_t actor)
{
	char path[1200];
	static const char* kFiles[] = { "f", "g", "mine", "new" };
	for (size_t i = 0; i < sizeof(kFiles) / sizeof(kFiles[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", dir, kFiles[i]);
		remove_quiet(path);
	}
	snprintf(path, sizeof(path), "%s/dn/inner", dir);
	remove_quiet(path);
	snprintf(path, sizeof(path), "%s/dn", dir);
	if (rmdir(path) != 0 && errno != ENOENT)
		die("rmdir", path);
	snprintf(path, sizeof(path), "%s/de", dir);
	if (rmdir(path) != 0 && errno != ENOENT)
		die("rmdir", path);

	snprintf(path, sizeof(path), "%s/f", dir);
	make_file(path, UID_A);
	snprintf(path, sizeof(path), "%s/mine", dir);
	make_file(path, actor);
	snprintf(path, sizeof(path), "%s/de", dir);
	make_dir(path, 0755, UID_A);
	snprintf(path, sizeof(path), "%s/dn", dir);
	make_dir(path, 0755, UID_A);
	snprintf(path, sizeof(path), "%s/dn/inner", dir);
	make_file(path, UID_A);
}


static int
do_op(int op, char dirKind, const char* dir, const struct actor* actor)
{
	char from[1200], to[1200];
	int fd;

	switch (op) {
		case OP_CREATE:
			snprintf(from, sizeof(from), "%s/new", dir);
			fd = open(from, O_CREAT | O_EXCL | O_WRONLY, 0644);
			if (fd < 0)
				return errno;
			close(fd);
			return 0;
		case OP_UNLINK:
			snprintf(from, sizeof(from), "%s/f", dir);
			return unlink(from) == 0 ? 0 : errno;
		case OP_RENAME_AWAY:
			snprintf(from, sizeof(from), "%s/f", dir);
			snprintf(to, sizeof(to), "%s/g", dir);
			return rename(from, to) == 0 ? 0 : errno;
		case OP_RENAME_OUT:
			snprintf(from, sizeof(from), "%s/f", dir);
			snprintf(to, sizeof(to), "%s/out/f.%c.%u", sBase, dirKind,
				(unsigned)actor->uid);
			return rename(from, to) == 0 ? 0 : errno;
		case OP_RENAME_OVER:
			snprintf(from, sizeof(from), "%s/mine", dir);
			snprintf(to, sizeof(to), "%s/f", dir);
			return rename(from, to) == 0 ? 0 : errno;
		case OP_RMDIR_EMPTY:
			snprintf(from, sizeof(from), "%s/de", dir);
			return rmdir(from) == 0 ? 0 : errno;
		case OP_RMDIR_FULL:
			snprintf(from, sizeof(from), "%s/dn", dir);
			return rmdir(from) == 0 ? 0 : errno;
	}
	return EINVAL;
}


static int
expected(char dirKind, int op, const struct actor* actor)
{
	if (op == OP_CREATE) {
		/* W is 0755 root: only root may create there */
		return dirKind == 'W' && actor->uid != 0 ? EACCES : 0;
	}

	if (dirKind == 'W' && actor->uid != 0) {
		/* no write permission on the directory: may_delete()'s first check */
		return EACCES;
	}

	if (dirKind == 'S' && actor->uid != 0 && actor->uid != UID_C
		&& actor->uid != UID_A) {
		/* the sticky rule comes before the emptiness check */
		return EPERM;
	}

	if (op == OP_RMDIR_FULL)
		return ENOTEMPTY;
	return 0;
}


static void
run_row(char dirKind, const char* dir, int op, const struct actor* actor)
{
	if (sSkipOp[op]) {
		sSkipped++;
		printf("SKIP dir=%c actor=%-15s op=%-18s (-x)\n", dirKind, actor->name,
			kOpNames[op]);
		return;
	}

	reset_entries(dir, actor->uid);

	int pipeFDs[2];
	if (pipe(pipeFDs) != 0)
		die("pipe", "");

	pid_t child = fork();
	if (child < 0)
		die("fork", "");
	if (child == 0) {
		close(pipeFDs[0]);
		int result;
		gid_t gid = actor->uid;
		if (actor->uid != 0
			&& (setgroups(1, &gid) != 0 || setgid(gid) != 0
				|| setuid(actor->uid) != 0)) {
			fprintf(stderr, "child: cannot become uid %u: %s\n",
				(unsigned)actor->uid, strerror(errno));
			_exit(3);
		}
		if (actor->uid != 0 && (geteuid() != actor->uid || getuid() == 0)) {
			fprintf(stderr, "child: still privileged\n");
			_exit(3);
		}
		result = do_op(op, dirKind, dir, actor);
		if (write(pipeFDs[1], &result, sizeof(result)) != sizeof(result))
			_exit(4);
		_exit(0);
	}
	close(pipeFDs[1]);

	int result = -1;
	ssize_t bytes = read(pipeFDs[0], &result, sizeof(result));
	close(pipeFDs[0]);
	int status;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status)
		|| WEXITSTATUS(status) != 0 || bytes != sizeof(result)) {
		fprintf(stderr, "child for %c %s %s failed (status %d)\n", dirKind,
			actor->name, kOpNames[op], status);
		exit(2);
	}

	int want = expected(dirKind, op, actor);
	sRows++;
	bool pass = result == want;
	if (!pass)
		sFailures++;
	printf("%s dir=%c actor=%-15s op=%-18s got=%-9s expected=%-9s\n",
		pass ? "PASS" : "FAIL", dirKind, actor->name, kOpNames[op],
		errno_name(result), errno_name(want));
}


int
main(int argc, char** argv)
{
	const char* base = "/tmp";
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
			int op = 0;
			while (op < OP_COUNT && strcmp(kOpNames[op], argv[i + 1]) != 0)
				op++;
			if (op == OP_COUNT) {
				fprintf(stderr, "unknown op %s\n", argv[i + 1]);
				return 2;
			}
			sSkipOp[op] = true;
			i++;
		} else
			base = argv[i];
	}
	if (geteuid() != 0) {
		fprintf(stderr, "run as root\n");
		return 2;
	}

	struct stat st;
	if (stat(base, &st) != 0)
		die("stat", base);
	printf("base %s: mode %04o owner %u:%u\n", base,
		(unsigned)(st.st_mode & 07777), (unsigned)st.st_uid,
		(unsigned)st.st_gid);

	snprintf(sBase, sizeof(sBase), "%s/sticky1008.%d", base, (int)getpid());
	make_dir(sBase, 0755, 0);

	char path[1200];
	snprintf(path, sizeof(path), "%s/out", sBase);
	make_dir(path, 0777, 0);

	struct { char kind; mode_t mode; uid_t owner; } dirs[] = {
		{ 'S', 01777, UID_C },
		{ 'N', 0777, UID_C },
		{ 'W', 0755, 0 },
	};

	for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
		char dir[1100];
		snprintf(dir, sizeof(dir), "%s/%c", sBase, dirs[d].kind);
		make_dir(dir, dirs[d].mode, dirs[d].owner);
		if (stat(dir, &st) != 0)
			die("stat", dir);
		printf("dir %c: mode %04o owner %u\n", dirs[d].kind,
			(unsigned)(st.st_mode & 07777), (unsigned)st.st_uid);
		if ((st.st_mode & 07777) != dirs[d].mode) {
			fprintf(stderr, "setup: %s did not keep mode %04o\n", dir,
				(unsigned)dirs[d].mode);
			return 2;
		}

		for (size_t a = 0; a < sizeof(kActors) / sizeof(kActors[0]); a++) {
			for (int op = 0; op < OP_COUNT; op++)
				run_row(dirs[d].kind, dir, op, &kActors[a]);
		}
	}

	printf("SUMMARY rows=%d failures=%d skipped=%d\n", sRows, sFailures,
		sSkipped);
	return sFailures == 0 ? 0 : 1;
}
