/*
 * rlim877.c -- issue 877: may a process lower its RLIMIT_NOFILE (and
 * RLIMIT_NOVMON) hard limit, and is the hard limit kept and enforced?
 *
 * Checks, for RLIMIT_NOFILE (and RLIMIT_NOVMON where it exists), in a forked
 * child each so they do not disturb one another:
 *   lower      setrlimit({64, 64}) succeeds, getrlimit() then reads {64, 64}
 *   inherit    a fork()ed and an exec'd child see the lowered hard limit
 *   cur>max    setrlimit({128, 64}) fails with EINVAL
 *   raise-root after lowering, root may raise the hard limit back (skipped if
 *              not root)
 *   raise-user after lowering and setuid() to an unprivileged user, raising
 *              the hard limit fails with EPERM, lowering it further succeeds
 *   enforced   with {64, 64}, open() of the 65th descriptor fails EMFILE
 *
 * Usage: rlim877 [exec-check]   Exit status: number of failed checks.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>


static int sFailed = 0;


static void
check(const char* what, int ok, const char* detail)
{
	printf("%-28s %s  %s\n", what, ok ? "PASS" : "FAIL", detail);
	if (!ok)
		sFailed++;
}


static int
in_child(int (*test)(int), int resource)
{
	fflush(stdout);
	pid_t child = fork();
	if (child == 0) {
		sFailed = 0;
		test(resource);
		fflush(stdout);
		_exit(sFailed);
	}
	int status;
	waitpid(child, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 100;
}


static const char*
name_of(int resource)
{
#ifdef RLIMIT_NOVMON
	if (resource == RLIMIT_NOVMON)
		return "NOVMON";
#endif
	return "NOFILE";
}


static int
test_lower(int resource)
{
	char what[64], detail[128];
	struct rlimit old, now, want = { 64, 64 };
	getrlimit(resource, &old);
	int rc = setrlimit(resource, &want);
	int error = errno;
	getrlimit(resource, &now);
	snprintf(what, sizeof(what), "%s lower {64,64}", name_of(resource));
	snprintf(detail, sizeof(detail), "was {%lu,%lu} rc=%d%s%s now {%lu,%lu}",
		(unsigned long)old.rlim_cur, (unsigned long)old.rlim_max, rc,
		rc ? " " : "", rc ? strerror(error) : "", (unsigned long)now.rlim_cur,
		(unsigned long)now.rlim_max);
	check(what, rc == 0 && now.rlim_cur == 64 && now.rlim_max == 64, detail);
	if (rc != 0)
		return 0;

	// a forked child inherits it
	fflush(stdout);
	int pipeFDs[2];
	pipe(pipeFDs);
	pid_t child = fork();
	if (child == 0) {
		struct rlimit inherited;
		getrlimit(resource, &inherited);
		write(pipeFDs[1], &inherited, sizeof(inherited));
		_exit(0);
	}
	struct rlimit inherited = { 0, 0 };
	read(pipeFDs[0], &inherited, sizeof(inherited));
	waitpid(child, NULL, 0);
	snprintf(what, sizeof(what), "%s inherit fork", name_of(resource));
	snprintf(detail, sizeof(detail), "child sees {%lu,%lu}",
		(unsigned long)inherited.rlim_cur, (unsigned long)inherited.rlim_max);
	check(what, inherited.rlim_cur == 64 && inherited.rlim_max == 64, detail);

	// cur > max is invalid
	struct rlimit bad = { 128, 64 };
	rc = setrlimit(resource, &bad);
	error = errno;
	snprintf(what, sizeof(what), "%s cur>max {128,64}", name_of(resource));
	snprintf(detail, sizeof(detail), "rc=%d %s", rc, rc ? strerror(error) : "");
	check(what, rc == -1 && error == EINVAL, detail);

	if (resource == RLIMIT_NOFILE) {
		// enforced: the 65th descriptor cannot be opened
		int opened = 0, lastErrno = 0;
		for (;;) {
			int fd = open("/dev/null", O_RDONLY);
			if (fd < 0) {
				lastErrno = errno;
				break;
			}
			opened++;
			if (opened > 1000)
				break;
		}
		snprintf(detail, sizeof(detail), "opened %d more, then %s", opened,
			strerror(lastErrno));
		check("NOFILE enforced at 64", lastErrno == EMFILE && opened < 64,
			detail);
	}
	return 0;
}


static int
test_raise(int resource)
{
	char what[64], detail[128];
	struct rlimit old, low = { 64, 64 };
	getrlimit(resource, &old);
	setrlimit(resource, &low);

	if (geteuid() == 0) {
		int rc = setrlimit(resource, &old);
		int error = errno;
		struct rlimit now;
		getrlimit(resource, &now);
		snprintf(what, sizeof(what), "%s raise back as root", name_of(resource));
		snprintf(detail, sizeof(detail), "rc=%d %s now {%lu,%lu}", rc,
			rc ? strerror(error) : "", (unsigned long)now.rlim_cur,
			(unsigned long)now.rlim_max);
		check(what, rc == 0 && now.rlim_max == old.rlim_max, detail);
		setrlimit(resource, &low);

		if (setuid(1001) != 0) {
			printf("%-28s SKIP  setuid: %s\n", "raise as user", strerror(errno));
			return 0;
		}
	}

	struct rlimit higher = { 64, 128 };
	int rc = setrlimit(resource, &higher);
	int error = errno;
	snprintf(what, sizeof(what), "%s raise as uid %d", name_of(resource),
		(int)geteuid());
	snprintf(detail, sizeof(detail), "{64,128}: rc=%d %s", rc,
		rc ? strerror(error) : "");
	check(what, rc == -1 && error == EPERM, detail);

	struct rlimit lower = { 32, 32 };
	rc = setrlimit(resource, &lower);
	error = errno;
	snprintf(what, sizeof(what), "%s lower as uid %d", name_of(resource),
		(int)geteuid());
	snprintf(detail, sizeof(detail), "{32,32}: rc=%d %s", rc,
		rc ? strerror(error) : "");
	check(what, rc == 0, detail);
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "exec-check") == 0) {
		struct rlimit now;
		getrlimit(RLIMIT_NOFILE, &now);
		printf("%lu %lu\n", (unsigned long)now.rlim_cur,
			(unsigned long)now.rlim_max);
		return 0;
	}

	int resources[] = {
		RLIMIT_NOFILE,
#ifdef RLIMIT_NOVMON
		RLIMIT_NOVMON,
#endif
	};
	int failed = 0;
	for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); i++) {
		failed += in_child(test_lower, resources[i]);
		failed += in_child(test_raise, resources[i]);
	}

	// exec'd child
	fflush(stdout);
	int pipeFDs[2];
	pipe(pipeFDs);
	pid_t child = fork();
	if (child == 0) {
		struct rlimit low = { 64, 64 };
		setrlimit(RLIMIT_NOFILE, &low);
		dup2(pipeFDs[1], 1);
		execl("/proc/self/exe", argv[0], "exec-check", (char*)NULL);
		execl(argv[0], argv[0], "exec-check", (char*)NULL);
		_exit(127);
	}
	close(pipeFDs[1]);
	char buffer[64] = "";
	ssize_t bytes = read(pipeFDs[0], buffer, sizeof(buffer) - 1);
	if (bytes > 0)
		buffer[bytes] = '\0';
	buffer[strcspn(buffer, "\n")] = '\0';
	waitpid(child, NULL, 0);
	char detail[128];
	snprintf(detail, sizeof(detail), "exec'd child sees \"%s\"", buffer);
	int ok = strcmp(buffer, "64 64") == 0;
	printf("%-28s %s  %s\n", "NOFILE inherit exec", ok ? "PASS" : "FAIL",
		detail);
	failed += !ok;

	printf("rlim877: %d failed\n", failed);
	return failed;
}
