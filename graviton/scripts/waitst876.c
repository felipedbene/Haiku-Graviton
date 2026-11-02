/*
 * waitst876.c -- issue 876: does waitpid() report a crashed or aborted child
 * as killed by the signal that ended it?
 *
 * Each case forks a child that ends in a particular way; the parent checks
 * WIFSIGNALED/WTERMSIG (or WIFEXITED/WEXITSTATUS) against what POSIX asks for.
 * On Haiku the crashing cases go through debug_server, which cannot start a
 * debugger on a headless system and kills the team; that kill must not change
 * what the parent sees.
 *
 *   abort            abort() with SIGABRT at its default action   -> SIGABRT
 *   abort-blocked    abort() with SIGABRT blocked                 -> SIGABRT
 *   abort-ignored    abort() with SIGABRT ignored                 -> SIGABRT
 *   abort-handler    abort() with a handler that returns          -> SIGABRT,
 *                    and the handler ran once
 *   abort-thread     abort() in a second thread                   -> SIGABRT
 *   raise-abrt       raise(SIGABRT)                               -> SIGABRT
 *   segv-null        store through NULL                           -> SIGSEGV
 *   segv-high        load from (int*)-16 (the issue's comment)    -> SIGSEGV
 *   segv-thread      store through NULL in a second thread        -> SIGSEGV
 *   fpe-trap         __builtin_trap()                             -> SIGILL or
 *                    SIGTRAP (architecture dependent; either is accepted)
 *   kill-segv        kill(getpid(), SIGSEGV), not a fault         -> SIGSEGV
 *   kill-9           kill(getpid(), SIGKILL)                      -> SIGKILL
 *   exit-3           exit(3)                                      -> exited 3
 *   handled-segv     SIGSEGV handler that _exit(7)s               -> exited 7
 *
 * Usage: waitst876 [case...]   (default: all). Exit status: failed cases.
 */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


static volatile sig_atomic_t sHandlerRuns = 0;


static void
returning_handler(int signal)
{
	(void)signal;
	sHandlerRuns++;
}


static void
exiting_handler(int signal)
{
	(void)signal;
	_exit(7);
}


static void*
abort_thread(void* arg)
{
	(void)arg;
	abort();
	return NULL;
}


static void*
segv_thread(void* arg)
{
	(void)arg;
	*(volatile int*)NULL = 1;
	return NULL;
}


static void
run_child(const char* name)
{
	if (strcmp(name, "abort") == 0) {
		abort();
	} else if (strcmp(name, "abort-blocked") == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SIGABRT);
		sigprocmask(SIG_BLOCK, &set, NULL);
		abort();
	} else if (strcmp(name, "abort-ignored") == 0) {
		signal(SIGABRT, SIG_IGN);
		abort();
	} else if (strcmp(name, "abort-handler") == 0) {
		signal(SIGABRT, returning_handler);
		abort();
	} else if (strcmp(name, "abort-thread") == 0) {
		pthread_t thread;
		pthread_create(&thread, NULL, abort_thread, NULL);
		for (;;)
			pause();
	} else if (strcmp(name, "raise-abrt") == 0) {
		raise(SIGABRT);
	} else if (strcmp(name, "segv-null") == 0) {
		*(volatile int*)NULL = 1;
	} else if (strcmp(name, "segv-high") == 0) {
		volatile int* p = (int*)-16;
		_exit(*p);
	} else if (strcmp(name, "segv-thread") == 0) {
		pthread_t thread;
		pthread_create(&thread, NULL, segv_thread, NULL);
		for (;;)
			pause();
	} else if (strcmp(name, "fpe-trap") == 0) {
		__builtin_trap();
	} else if (strcmp(name, "kill-segv") == 0) {
		kill(getpid(), SIGSEGV);
		for (;;)
			pause();
	} else if (strcmp(name, "kill-9") == 0) {
		kill(getpid(), SIGKILL);
		for (;;)
			pause();
	} else if (strcmp(name, "exit-3") == 0) {
		exit(3);
	} else if (strcmp(name, "handled-segv") == 0) {
		signal(SIGSEGV, exiting_handler);
		*(volatile int*)NULL = 1;
	}
	// the handler case returns here if abort() did
	if (strcmp(name, "abort-handler") == 0)
		_exit(100 + sHandlerRuns);
	_exit(99);
}


static int
expect(const char* name, int status)
{
	// returns 0 if status is what POSIX asks for
	if (strncmp(name, "abort", 5) == 0 || strcmp(name, "raise-abrt") == 0)
		return !(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
	if (strncmp(name, "segv", 4) == 0 || strcmp(name, "kill-segv") == 0)
		return !(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
	if (strcmp(name, "fpe-trap") == 0) {
		return !(WIFSIGNALED(status) && (WTERMSIG(status) == SIGILL
			|| WTERMSIG(status) == SIGTRAP));
	}
	if (strcmp(name, "kill-9") == 0)
		return !(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
	if (strcmp(name, "exit-3") == 0)
		return !(WIFEXITED(status) && WEXITSTATUS(status) == 3);
	if (strcmp(name, "handled-segv") == 0)
		return !(WIFEXITED(status) && WEXITSTATUS(status) == 7);
	return 1;
}


int
main(int argc, char** argv)
{
	static const char* kCases[] = {
		"abort", "abort-blocked", "abort-ignored", "abort-handler",
		"abort-thread", "raise-abrt", "segv-null", "segv-high", "segv-thread",
		"fpe-trap", "kill-segv", "kill-9", "exit-3", "handled-segv"
	};
	const char** cases = kCases;
	int count = sizeof(kCases) / sizeof(kCases[0]);
	if (argc > 1) {
		cases = (const char**)argv + 1;
		count = argc - 1;
	}

	printf("SIGABRT=%d SIGSEGV=%d SIGKILL=%d SIGILL=%d SIGTRAP=%d"
#ifdef SIGKILLTHR
		" SIGKILLTHR=%d"
#endif
		"\n", SIGABRT, SIGSEGV, SIGKILL, SIGILL, SIGTRAP
#ifdef SIGKILLTHR
		, SIGKILLTHR
#endif
		);

	int failed = 0;
	for (int i = 0; i < count; i++) {
		fflush(stdout);
		pid_t child = fork();
		if (child == 0) {
			run_child(cases[i]);
			_exit(98);
		}
		int status;
		waitpid(child, &status, 0);
		int bad = expect(cases[i], status);
		failed += bad;
		if (WIFSIGNALED(status)) {
			printf("%-14s %s  signaled  WTERMSIG=%d (%s)\n", cases[i],
				bad ? "FAIL" : "PASS", WTERMSIG(status),
				strsignal(WTERMSIG(status)));
		} else {
			printf("%-14s %s  exited    WEXITSTATUS=%d\n", cases[i],
				bad ? "FAIL" : "PASS", WEXITSTATUS(status));
		}
	}
	printf("waitst876: %d failed\n", failed);
	return failed;
}
