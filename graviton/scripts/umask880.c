/*
 * umask880.c -- issue 880: does umask() keep only the permission bits?
 *
 * For several masks with bits beyond 0777, umask(mask) followed by umask()
 * must read back mask & 0777, a file created with mode 0777 must get
 * 0777 & ~(mask & 0777), and an exec'd shell must report the masked value.
 * Exit status: number of failed checks.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


int
main()
{
	static const mode_t kMasks[] = { 010664, 07022, 0777, 022, 017777, 0 };
	int failed = 0;
	for (size_t i = 0; i < sizeof(kMasks) / sizeof(kMasks[0]); i++) {
		umask(kMasks[i]);
		mode_t got = umask(022);
		mode_t want = kMasks[i] & 0777;

		umask(kMasks[i]);
		unlink("umask880.dat");
		int fd = open("umask880.dat", O_CREAT | O_WRONLY, 0777);
		struct stat st;
		fstat(fd, &st);
		close(fd);
		unlink("umask880.dat");
		mode_t fileMode = st.st_mode & 07777;

		int ok = got == want && fileMode == (0777 & ~want);
		printf("umask(%#o): reads back %#o (want %#o), file 0777 -> %#o  %s\n",
			(unsigned)kMasks[i], (unsigned)got, (unsigned)want,
			(unsigned)fileMode, ok ? "PASS" : "FAIL");
		failed += !ok;
	}

	umask(010664);
	fflush(stdout);
	FILE* shell = popen("umask", "r");
	char line[64] = "";
	if (shell != NULL) {
		if (fgets(line, sizeof(line), shell) == NULL)
			line[0] = '\0';
		pclose(shell);
	}
	line[strcspn(line, "\n")] = '\0';
	int ok = strtol(line, NULL, 8) == 0664;
	printf("exec'd sh after umask(010664) reports \"%s\" (want 0664)  %s\n", line,
		ok ? "PASS" : "FAIL");
	failed += !ok;

	printf("umask880: %d failed\n", failed);
	return failed;
}
