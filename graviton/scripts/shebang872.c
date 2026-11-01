/*
 * shebang872.c -- issue 872: does exec of a "#!" script whose interpreter is
 * missing fail with ENOENT?
 *
 * Creates scripts in the current directory:
 *   missing   #!/no/such/interp            exec fails ENOENT
 *   noexec    #!./shebang872.txt (a file without x permission)  EACCES
 *   valid     #!/bin/sh, prints "ok"       exec succeeds, child exits 0
 *   args      #!/bin/sh -e                 exec succeeds (interpreter args)
 * each through execv() in a forked child (the errno is passed back) and
 * through posix_spawn(), whose return value is the error.
 * Exit status: number of failed checks.
 */

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;


static void
make_script(const char* path, const char* line)
{
	FILE* file = fopen(path, "w");
	fprintf(file, "%s\necho ok > /dev/null\nexit 0\n", line);
	fclose(file);
	chmod(path, 0755);
}


static int
exec_errno(const char* path, int* _status)
{
	int pipeFDs[2];
	pipe(pipeFDs);
	fcntl(pipeFDs[1], F_SETFD, FD_CLOEXEC);
	pid_t child = fork();
	if (child == 0) {
		char* argv[] = { (char*)path, NULL };
		execv(path, argv);
		int error = errno;
		write(pipeFDs[1], &error, sizeof(error));
		_exit(127);
	}
	close(pipeFDs[1]);
	int error = 0;
	if (read(pipeFDs[0], &error, sizeof(error)) != sizeof(error))
		error = 0;
	close(pipeFDs[0]);
	waitpid(child, _status, 0);
	return error;
}


static int
spawn_error(const char* path, int* _status)
{
	pid_t child;
	char* argv[] = { (char*)path, NULL };
	int error = posix_spawn(&child, path, NULL, NULL, argv, environ);
	*_status = -1;
	if (error == 0)
		waitpid(child, _status, 0);
	return error;
}


int
main()
{
	FILE* file = fopen("shebang872.txt", "w");
	fprintf(file, "not a program\n");
	fclose(file);
	chmod("shebang872.txt", 0644);

	struct {
		const char* path;
		const char* line;
		int error;
	} cases[] = {
		{ "./shebang872-missing.sh", "#!/no/such/interp", ENOENT },
		{ "./shebang872-noexec.sh", "#!./shebang872.txt", EACCES },
		{ "./shebang872-valid.sh", "#!/bin/sh", 0 },
		{ "./shebang872-args.sh", "#!/bin/sh -e", 0 },
	};

	int failed = 0;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		make_script(cases[i].path, cases[i].line);
		int status;
		int error = exec_errno(cases[i].path, &status);
		int ok = error == cases[i].error
			&& (error != 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 0));
		printf("execv       %-26s errno=%d (%s), child %s %d  %s\n",
			cases[i].line, error, error ? strerror(error) : "-",
			WIFEXITED(status) ? "exit" : "signal",
			WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status),
			ok ? "PASS" : "FAIL");
		failed += !ok;

		error = spawn_error(cases[i].path, &status);
		ok = error == cases[i].error
			&& (error != 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 0));
		printf("posix_spawn %-26s error=%d (%s), child status %#x  %s\n",
			cases[i].line, error, error ? strerror(error) : "-", status,
			ok ? "PASS" : "FAIL");
		failed += !ok;
		unlink(cases[i].path);
	}
	unlink("shebang872.txt");
	printf("shebang872: %d failed\n", failed);
	return failed;
}
