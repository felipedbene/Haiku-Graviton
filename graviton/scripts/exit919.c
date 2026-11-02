/*
 * exit919.c -- issue 919: do _exit() and _Exit() leave stdio alone?
 *
 * POSIX (XSH _exit): "Open streams shall not be flushed", and functions
 * registered with atexit() are not called. Each case runs in a fresh "runner"
 * process whose stdout is a pipe, so stdout is fully buffered. The runner
 * printf()s a marker without a newline, then does what the case says, then
 * returns from its case function and exit()s normally -- which flushes the
 * marker once. Everything the runner and its children write is collected and
 * the markers are counted.
 *   fork-_exit      child _exit(0)s right away: marker once
 *   fork-_Exit      the same with _Exit(0): marker once
 *   failed-exec     child execv()s a missing file, then _exit(127)s (the
 *                   usual fork/exec idiom): marker once
 *   spawn-missing   posix_spawn() with file actions (libroot forks for those)
 *                   of a missing file; its child _exit(127)s: marker once
 *   popen-exit      popen() a command; its child execs /bin/sh, so only the
 *                   runner's copy: marker once
 *   child-buffer    child printf()s its own marker, then _exit(0)s: the
 *                   child's marker must not appear at all (not flushed)
 *   atexit          child registers an atexit() handler, then _exit(0)s: the
 *                   handler must not run
 *   fork-exit       control: child exit(0)s, which flushes its copy of the
 *                   buffer, so the marker appears twice, as on every libc
 *
 * Plain POSIX, so the same source builds and runs on Linux for reference.
 * Exit status: number of failed cases.
 */

#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


extern char** environ;


static void
reap(pid_t child)
{
	int status;
	while (waitpid(child, &status, 0) < 0)
		;
}


static void
atexit_handler(void)
{
	write(STDOUT_FILENO, "HANDLER", 7);
}


static void
case_fork_exit_(void)
{
	printf("MARK");
	pid_t child = fork();
	if (child == 0)
		_exit(0);
	reap(child);
}


static void
case_fork_Exit(void)
{
	printf("MARK");
	pid_t child = fork();
	if (child == 0)
		_Exit(0);
	reap(child);
}


static void
case_failed_exec(void)
{
	printf("MARK");
	pid_t child = fork();
	if (child == 0) {
		char* argv[] = { (char*)"/no/such/binary919", NULL };
		execv(argv[0], argv);
		_exit(127);
	}
	reap(child);
}


static void
case_spawn_missing(void)
{
	printf("MARK");
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 9, "/dev/null", 0, 0);
	char* argv[] = { (char*)"/no/such/binary919", NULL };
	pid_t child;
	int error = posix_spawn(&child, argv[0], &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	// glibc reports the exec failure as the return value; libroot's fork
	// path also does. Either way any child it left is reaped here.
	if (error == 0)
		reap(child);
}


static void
case_popen_exit(void)
{
	printf("MARK");
	FILE* f = popen("true", "r");
	if (f != NULL)
		pclose(f);
}


static void
case_child_buffer(void)
{
	printf("MARK");
	fflush(stdout);
	pid_t child = fork();
	if (child == 0) {
		printf("CHILD");
		_exit(0);
	}
	reap(child);
}


static void
case_atexit(void)
{
	printf("MARK");
	fflush(stdout);
	pid_t child = fork();
	if (child == 0) {
		atexit(atexit_handler);
		_exit(0);
	}
	reap(child);
}


static void
case_fork_exit(void)
{
	printf("MARK");
	pid_t child = fork();
	if (child == 0)
		exit(0);
	reap(child);
}


struct test_case {
	const char*	name;
	void		(*function)(void);
	int			marks;
	int			children;
	int			handlers;
};


static const struct test_case kCases[] = {
	{ "fork-_exit",		case_fork_exit_,	1, 0, 0 },
	{ "fork-_Exit",		case_fork_Exit,		1, 0, 0 },
	{ "failed-exec",	case_failed_exec,	1, 0, 0 },
	{ "spawn-missing",	case_spawn_missing,	1, 0, 0 },
	{ "popen-exit",		case_popen_exit,	1, 0, 0 },
	{ "child-buffer",	case_child_buffer,	1, 0, 0 },
	{ "atexit",			case_atexit,		1, 0, 0 },
	{ "fork-exit",		case_fork_exit,		2, 0, 0 },
};


static int
count(const char* haystack, const char* needle)
{
	int n = 0;
	size_t length = strlen(needle);
	for (const char* p = haystack; (p = strstr(p, needle)) != NULL; p += length)
		n++;
	return n;
}


int
main(void)
{
	int failed = 0;
	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		const struct test_case* test = &kCases[i];
		int p[2];
		if (pipe(p) != 0) {
			perror("pipe");
			return 100;
		}
		fflush(stdout);
		pid_t runner = fork();
		if (runner < 0) {
			perror("fork");
			return 100;
		}
		if (runner == 0) {
			close(p[0]);
			dup2(p[1], STDOUT_FILENO);
			close(p[1]);
			// stdout was a pipe before main() on some runs and a terminal on
			// others; make it fully buffered either way.
			setvbuf(stdout, NULL, _IOFBF, BUFSIZ);
			test->function();
			exit(0);
		}
		close(p[1]);
		char output[4096];
		size_t used = 0;
		ssize_t bytes;
		while (used < sizeof(output) - 1
			&& (bytes = read(p[0], output + used, sizeof(output) - 1 - used))
				> 0) {
			used += bytes;
		}
		output[used] = '\0';
		close(p[0]);
		reap(runner);

		int marks = count(output, "MARK");
		int children = count(output, "CHILD");
		int handlers = count(output, "HANDLER");
		int ok = marks == test->marks && children == test->children
			&& handlers == test->handlers;
		if (!ok)
			failed++;
		printf("%-14s marks %d (want %d) child %d (want %d) handler %d "
			"(want %d)  %s   raw '%s'\n", test->name, marks, test->marks,
			children, test->children, handlers, test->handlers,
			ok ? "PASS" : "FAIL", output);
	}
	printf("exit919: %d failed\n", failed);
	return failed;
}
