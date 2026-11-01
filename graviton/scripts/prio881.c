/*
 * prio881.c -- issue 881: does getpriority() return the nice value
 * setpriority() set?
 *
 * For every nice value from -20 to 19: setpriority(PRIO_PROCESS, 0, n), then
 * getpriority(PRIO_PROCESS, 0) and getpriority(PRIO_PROCESS, getpid()) must
 * both return n. A forked child must report the parent's nice value, too.
 * Exit status: number of mismatches.
 */

#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>


int
main()
{
	int failed = 0;
	for (int n = -20; n <= 19; n++) {
		if (setpriority(PRIO_PROCESS, 0, n) != 0) {
			printf("setpriority(%d): errno %d -- skipped\n", n, errno);
			continue;
		}
		errno = 0;
		int self = getpriority(PRIO_PROCESS, 0);
		int byPid = getpriority(PRIO_PROCESS, getpid());
		int ok = self == n && byPid == n;
		if (!ok || n % 5 == 0) {
			printf("set %3d  get(0)=%3d  get(pid)=%3d  %s\n", n, self, byPid,
				ok ? "PASS" : "FAIL");
		}
		failed += !ok;
	}

	setpriority(PRIO_PROCESS, 0, 10);
	int want = getpriority(PRIO_PROCESS, 0);
	fflush(stdout);
	pid_t child = fork();
	if (child == 0)
		_exit(getpriority(PRIO_PROCESS, 0) + 20);
	int status;
	waitpid(child, &status, 0);
	int inherited = WEXITSTATUS(status) - 20;
	printf("forked child reads %d, parent %d  %s\n", inherited, want,
		inherited == want ? "PASS" : "FAIL");
	failed += inherited != want;

	printf("prio881: %d failed\n", failed);
	return failed;
}
