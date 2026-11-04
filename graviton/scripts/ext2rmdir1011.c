/*
 * ext2rmdir1011.c -- issue 1011: rmdir() and rename() must refuse to remove
 * or replace a directory that still holds entries.
 *
 * Usage: ext2rmdir1011 <base-directory> [<populated-directory>]
 *
 * Under <base>/t1011.<pid> it checks, each row printing got/expected:
 *   - rmdir of a directory holding one regular file      -> ENOTEMPTY
 *     (and the file must still be reachable afterwards)
 *   - rmdir of a directory whose only file was unlinked  -> OK
 *   - rmdir of an empty directory                        -> OK
 *   - rmdir of a directory holding only a subdirectory   -> ENOTEMPTY
 *   - rename(dirA, dirB) with dirB holding a file        -> ENOTEMPTY
 *     (dirA and dirB/x must both survive)
 *   - rename(dirA, dirB) with dirB holding a subdir      -> ENOTEMPTY
 *   - rename(dirA, dirC) with dirC empty                 -> OK, and the
 *     parent's link count drops by the replaced directory's ".."
 *   - the same across directories, rename(X, P/Q) with P/Q empty
 *   - rmdir of a regular file                            -> ENOTDIR
 *   - a large, multi-block directory of BIG_COUNT files: rmdir -> ENOTEMPTY;
 *     unlink all but the last-created file -> ENOTEMPTY (the remaining
 *     entry sits past the first block); unlink it too -> OK
 * With <populated-directory> (a directory filled elsewhere, e.g. an htree-
 * indexed one created by Linux), it also does rmdir -> ENOTEMPTY, unlinks
 * every entry, then rmdir -> OK.
 *
 * The tree is left in place on purpose, so the volume can be checked with
 * e2fsck afterwards. The last line is the failure count; the exit status is
 * non-zero if any row failed.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BIG_COUNT	2000

static char sBase[1024];
static int sFailures;
static int sRows;


static const char*
errno_name(int error)
{
	switch (error) {
		case 0:			return "OK";
		case ENOTEMPTY:	return "ENOTEMPTY";
		case ENOTDIR:	return "ENOTDIR";
		case EISDIR:	return "EISDIR";
		case ENOENT:	return "ENOENT";
		case EEXIST:	return "EEXIST";
		case EPERM:		return "EPERM";
		case EACCES:	return "EACCES";
		case EINVAL:	return "EINVAL";
		case EBUSY:		return "EBUSY";
		case EIO:		return "EIO";
		default:		return strerror(error);
	}
}


static void
check(const char* row, int got, int expected)
{
	sRows++;
	bool ok = got == expected;
	if (!ok)
		sFailures++;
	printf("%s %-44s got=%-10s expected=%s\n", ok ? "PASS" : "FAIL", row,
		errno_name(got), errno_name(expected));
	fflush(stdout);
}


static const char*
path(const char* relative)
{
	static char buffer[4][2048];
	static int next;
	char* result = buffer[next++ % 4];
	snprintf(result, sizeof(buffer[0]), "%s/%s", sBase, relative);
	return result;
}


static int
result_of(int returnValue)
{
	return returnValue == 0 ? 0 : errno;
}


static void
must(int returnValue, const char* what)
{
	if (returnValue != 0) {
		fprintf(stderr, "setup: %s: %s\n", what, strerror(errno));
		exit(100);
	}
}


static void
make_file(const char* name)
{
	int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (fd < 0) {
		fprintf(stderr, "setup: open(%s): %s\n", name, strerror(errno));
		exit(100);
	}
	if (write(fd, "x\n", 2) != 2) {
		fprintf(stderr, "setup: write(%s): %s\n", name, strerror(errno));
		exit(100);
	}
	close(fd);
}


static int
exists(const char* name)
{
	struct stat st;
	return lstat(name, &st) == 0 ? 0 : errno;
}


/* number of entries besides "." and ".." readdir() returns */
static int
count_entries(const char* name)
{
	DIR* dir = opendir(name);
	if (dir == NULL)
		return -1;
	int count = 0;
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			count++;
	}
	closedir(dir);
	return count;
}


/* checks that a directory's link count is 2 plus its number of subdirectories */
static void
check_links(const char* row, const char* name)
{
	struct stat st;
	int subdirs = 0;
	DIR* dir = opendir(name);
	if (dir == NULL || stat(name, &st) != 0) {
		check(row, errno, 0);
		if (dir != NULL)
			closedir(dir);
		return;
	}
	struct dirent* entry;
	char child[2048];
	while ((entry = readdir(dir)) != NULL) {
		struct stat childStat;
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		snprintf(child, sizeof(child), "%s/%s", name, entry->d_name);
		if (lstat(child, &childStat) == 0 && S_ISDIR(childStat.st_mode))
			subdirs++;
	}
	closedir(dir);
	printf("links of %s: %u (2 + %d subdirectories expected)\n", name,
		(unsigned)st.st_nlink, subdirs);
	check(row, (int)st.st_nlink == 2 + subdirs ? 0 : EINVAL, 0);
}


static void
test_populated(const char* dirPath)
{
	struct stat st;
	if (stat(dirPath, &st) != 0) {
		fprintf(stderr, "populated dir %s: %s\n", dirPath, strerror(errno));
		sFailures++;
		return;
	}
	int entries = count_entries(dirPath);
	printf("populated dir %s: size %lld, %d entries\n", dirPath,
		(long long)st.st_size, entries);

	check("rmdir(populated dir)", result_of(rmdir(dirPath)), ENOTEMPTY);
	check("populated dir still listable after rmdir",
		count_entries(dirPath) == entries ? 0 : EIO, 0);

	/* unlink everything; collect names first, removing while iterating is
	   not something to depend on */
	DIR* dir = opendir(dirPath);
	if (dir == NULL) {
		sFailures++;
		return;
	}
	char** names = calloc(entries > 0 ? entries : 1, sizeof(char*));
	int count = 0;
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL && count < entries) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			names[count++] = strdup(entry->d_name);
	}
	closedir(dir);

	int unlinkFailures = 0;
	char file[2048];
	for (int i = 0; i < count; i++) {
		snprintf(file, sizeof(file), "%s/%s", dirPath, names[i]);
		if (unlink(file) != 0)
			unlinkFailures++;
		free(names[i]);
	}
	free(names);
	check("unlink every entry of populated dir",
		unlinkFailures == 0 ? 0 : EIO, 0);
	check("rmdir(populated dir, now empty)", result_of(rmdir(dirPath)), 0);
	check("populated dir gone", exists(dirPath), ENOENT);
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <base-directory> [<populated-directory>]\n",
			argv[0]);
		return 2;
	}
	snprintf(sBase, sizeof(sBase), "%s/t1011.%d", argv[1], (int)getpid());
	must(mkdir(sBase, 0755), sBase);
	printf("base %s\n", sBase);

	/* a directory holding one regular file */
	must(mkdir(path("onefile"), 0755), "mkdir onefile");
	make_file(path("onefile/inner"));
	check("rmdir(dir holding one file)", result_of(rmdir(path("onefile"))),
		ENOTEMPTY);
	check("its file still reachable", exists(path("onefile/inner")), 0);
	check("its file still listed", count_entries(path("onefile")) == 1
		? 0 : EIO, 0);

	/* the same after the file is gone: a deleted entry is not an entry */
	check("unlink(onefile/inner)", result_of(unlink(path("onefile/inner"))), 0);
	check("rmdir(dir whose file was unlinked)",
		result_of(rmdir(path("onefile"))), 0);

	/* an empty directory */
	must(mkdir(path("empty"), 0755), "mkdir empty");
	check("rmdir(empty dir)", result_of(rmdir(path("empty"))), 0);
	check("empty dir gone", exists(path("empty")), ENOENT);

	/* a directory holding only a subdirectory */
	must(mkdir(path("onesub"), 0755), "mkdir onesub");
	must(mkdir(path("onesub/sub"), 0755), "mkdir onesub/sub");
	check("rmdir(dir holding a subdir)", result_of(rmdir(path("onesub"))),
		ENOTEMPTY);
	check("its subdir still reachable", exists(path("onesub/sub")), 0);

	/* rename over a directory */
	must(mkdir(path("A"), 0755), "mkdir A");
	make_file(path("A/a"));
	must(mkdir(path("B"), 0755), "mkdir B");
	make_file(path("B/x"));
	must(mkdir(path("Bsub"), 0755), "mkdir Bsub");
	must(mkdir(path("Bsub/s"), 0755), "mkdir Bsub/s");
	must(mkdir(path("C"), 0755), "mkdir C");
	check("rename(A, B holding a file)", result_of(rename(path("A"), path("B"))),
		ENOTEMPTY);
	check("A still there", exists(path("A/a")), 0);
	check("B/x still there", exists(path("B/x")), 0);
	check("rename(A, Bsub holding a subdir)",
		result_of(rename(path("A"), path("Bsub"))), ENOTEMPTY);
	check("Bsub/s still there", exists(path("Bsub/s")), 0);
	check("rename(A, C empty)", result_of(rename(path("A"), path("C"))), 0);
	check("A gone after rename", exists(path("A")), ENOENT);
	check("C/a present after rename", exists(path("C/a")), 0);
	check_links("base links after rename over a dir", sBase);

	/* rename across directories over an empty directory */
	must(mkdir(path("P"), 0755), "mkdir P");
	must(mkdir(path("P/Q"), 0755), "mkdir P/Q");
	must(mkdir(path("X"), 0755), "mkdir X");
	make_file(path("X/x"));
	check("rename(X, P/Q empty) across dirs",
		result_of(rename(path("X"), path("P/Q"))), 0);
	check("P/Q/x present after rename", exists(path("P/Q/x")), 0);
	check_links("P links after cross-dir rename over a dir", path("P"));
	check_links("base links after cross-dir rename over a dir", sBase);

	/* rmdir of a regular file */
	make_file(path("regular"));
	check("rmdir(regular file)", result_of(rmdir(path("regular"))), ENOTDIR);
	check("regular file still there", exists(path("regular")), 0);

	/* a large, multi-block directory */
	must(mkdir(path("big"), 0755), "mkdir big");
	char name[2048];
	for (int i = 0; i < BIG_COUNT; i++) {
		snprintf(name, sizeof(name), "%s/big/file-with-a-longish-name-%05d",
			sBase, i);
		make_file(name);
	}
	struct stat st;
	must(stat(path("big"), &st), "stat big");
	printf("big: %d files, directory size %lld bytes\n", BIG_COUNT,
		(long long)st.st_size);
	check("big dir spans more than one 4 KiB block",
		st.st_size > 4096 ? 0 : EINVAL, 0);
	check("rmdir(big dir)", result_of(rmdir(path("big"))), ENOTEMPTY);
	check("big dir still lists every file",
		count_entries(path("big")) == BIG_COUNT ? 0 : EIO, 0);
	int unlinkFailures = 0;
	for (int i = 0; i < BIG_COUNT - 1; i++) {
		snprintf(name, sizeof(name), "%s/big/file-with-a-longish-name-%05d",
			sBase, i);
		if (unlink(name) != 0)
			unlinkFailures++;
	}
	check("unlink all but the last file", unlinkFailures == 0 ? 0 : EIO, 0);
	check("rmdir(big dir, one file left)", result_of(rmdir(path("big"))),
		ENOTEMPTY);
	snprintf(name, sizeof(name), "%s/big/file-with-a-longish-name-%05d",
		sBase, BIG_COUNT - 1);
	check("last file still reachable", exists(name), 0);
	check("unlink the last file", result_of(unlink(name)), 0);
	check("rmdir(big dir, now empty)", result_of(rmdir(path("big"))), 0);
	check("big dir gone", exists(path("big")), ENOENT);

	check_links("base links at the end", sBase);

	if (argc > 2)
		test_populated(argv[2]);

	sync();
	printf("%d rows, %d failures\n", sRows, sFailures);
	return sFailures != 0 ? 1 : 0;
}
