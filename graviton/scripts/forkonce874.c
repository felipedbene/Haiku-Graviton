/*
 * forkonce874.c -- issue 874: can the child of a fork() made right after a
 * process created its first thread start threads of its own?
 *
 * Each round runs in a fresh process (a forked "runner"), so that the thread
 * it creates is the first one of its team. The runner creates one idle
 * pthread and immediately fork()s. The fork child creates a thread that sets
 * a flag, and must see the flag within 2 s; it also must be able to malloc()
 * from both threads. The runner then also checks that its own new threads
 * still start, and that malloc() works in it.
 *
 * Usage: forkonce874 [rounds]   (default 40). Exit status: failed rounds.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


static volatile int sStarted = 0;


static void*
idle_thread(void* arg)
{
	(void)arg;
	usleep(200000);
	return NULL;
}


static void*
flag_thread(void* arg)
{
	(void)arg;
	free(malloc(100));
	sStarted = 1;
	return NULL;
}


static int
thread_starts()
{
	sStarted = 0;
	pthread_t thread;
	if (pthread_create(&thread, NULL, flag_thread, NULL) != 0)
		return 0;
	for (int i = 0; i < 200 && !sStarted; i++)
		usleep(10000);
	if (!sStarted)
		return 0;
	pthread_join(thread, NULL);
	return 1;
}


static int
run_round()
{
	pthread_t idle;
	pthread_create(&idle, NULL, idle_thread, NULL);
	pid_t child = fork();
	if (child == 0) {
		int ok = thread_starts();
		free(malloc(1000));
		_exit(ok ? 0 : 1);
	}
	int status;
	waitpid(child, &status, 0);
	int childOk = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	int parentOk = thread_starts();
	pthread_join(idle, NULL);
	return (childOk ? 0 : 1) | (parentOk ? 0 : 2);
}


int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 40;
	int failedChild = 0, failedParent = 0, failed = 0;
	for (int i = 0; i < rounds; i++) {
		fflush(stdout);
		pid_t runner = fork();
		if (runner == 0)
			_exit(run_round());
		int status;
		waitpid(runner, &status, 0);
		int result = WIFEXITED(status) ? WEXITSTATUS(status) : 4;
		if (result != 0) {
			failed++;
			if (result & 1)
				failedChild++;
			if (result & 2)
				failedParent++;
			if (failed <= 3) {
				printf("round %d: %s%s%s\n", i,
					result & 1 ? "fork child could not start a thread " : "",
					result & 2 ? "runner could not start a thread " : "",
					result & 4 ? "runner died" : "");
			}
		}
	}
	printf("forkonce874: %d of %d rounds failed (child %d, runner %d)\n", failed,
		rounds, failedChild, failedParent);
	return failed;
}
