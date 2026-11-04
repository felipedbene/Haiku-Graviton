/*
 * rename1118 -- rename() and unlink() over NTFS targets, with no stat()
 * first (issue 1118).
 *
 * ntfs_fuse_rm() asked for the target's vnode while it had the parent and
 * the target open; when the vnode was not loaded, loading it opened those
 * inodes a second time and the inode cache KDLed ("ntfs_inode_close: N
 * already in cache!"). A rename over an existing target always got there.
 * coreutils mv stat()s first, which loads the vnode and hides the unlink
 * case, so this calls the syscalls directly.
 *
 * Usage:
 *   rename1118 rename <from> <to>
 *   rename1118 unlink <path>
 *   rename1118 rmdir <path>
 *   rename1118 open-rename <keep-open> <from> <to>
 *       open <keep-open> read-only, rename <from> over <to>, then read the
 *       open descriptor back and print what it holds
 * Prints "<op> rc=0" or "<op> rc=-1 <strerror>"; exit status 0 on success.
 *
 * Build: cc -O2 -Wall -o rename1118 rename1118.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


static int
report(const char* op, int r)
{
	if (r == 0)
		printf("%s rc=0\n", op);
	else
		printf("%s rc=-1 %s\n", op, strerror(errno));
	return r == 0 ? 0 : 1;
}


int
main(int argc, char** argv)
{
	if (argc == 4 && strcmp(argv[1], "rename") == 0)
		return report("rename", rename(argv[2], argv[3]));
	if (argc == 3 && strcmp(argv[1], "unlink") == 0)
		return report("unlink", unlink(argv[2]));
	if (argc == 3 && strcmp(argv[1], "rmdir") == 0)
		return report("rmdir", rmdir(argv[2]));
	if (argc == 5 && strcmp(argv[1], "open-rename") == 0) {
		int fd = open(argv[2], O_RDONLY);
		if (fd < 0)
			return report("open", -1);
		int status = report("rename", rename(argv[3], argv[4]));
		char buffer[64];
		ssize_t n = pread(fd, buffer, sizeof(buffer) - 1, 0);
		if (n < 0)
			status |= report("read open target", -1);
		else {
			buffer[n] = '\0';
			if (n > 0 && buffer[n - 1] == '\n')
				buffer[n - 1] = '\0';
			printf("open target still reads \"%s\"\n", buffer);
		}
		status |= report("close", close(fd));
		return status;
	}
	fprintf(stderr, "usage: rename1118 rename|unlink|rmdir|open-rename ...\n");
	return 2;
}
