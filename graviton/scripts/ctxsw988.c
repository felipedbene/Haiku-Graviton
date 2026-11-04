/*
 * ctxsw988 -- context-switch cost check for DeBeOS issue 988.
 *
 * The 988 fix adds a clock read and a store barrier to the scheduler's
 * TrackActivity() whenever a non-idle thread is switched in or out. This
 * measures what that costs on the paths that switch most:
 *   pingpong  two threads hand a semaphore back and forth, so every round trip
 *             is two blocking switches (busy <-> idle on each CPU, or a direct
 *             busy -> busy switch when both land on one CPU);
 *   yield     <n> threads per CPU calling sched_yield(), a busy -> busy switch
 *             on every call.
 * Each is repeated <reps> times and printed per run, then as the median.
 *
 * usage: ctxsw988 [-r reps] [-n round_trips]
 */

#include <OS.h>

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static sem_id sPing, sPong;
static int32 sRounds;
static volatile int32 sStop;
static int32 sYields[64];


static status_t
pong_thread(void*)
{
	for (int32 i = 0; i < sRounds; i++) {
		acquire_sem(sPing);
		release_sem(sPong);
	}
	return B_OK;
}


static double
pingpong(void)
{
	sPing = create_sem(0, "ping");
	sPong = create_sem(0, "pong");
	thread_id t = spawn_thread(pong_thread, "pong", B_NORMAL_PRIORITY, NULL);
	resume_thread(t);
	bigtime_t start = system_time();
	for (int32 i = 0; i < sRounds; i++) {
		release_sem(sPing);
		acquire_sem(sPong);
	}
	bigtime_t elapsed = system_time() - start;
	status_t r;
	wait_for_thread(t, &r);
	delete_sem(sPing);
	delete_sem(sPong);
	return (double)elapsed * 1000.0 / sRounds;
		// ns per round trip
}


static status_t
yield_thread(void* arg)
{
	int32* counter = (int32*)arg;
	while (!sStop) {
		sched_yield();
		(*counter)++;
	}
	return B_OK;
}


static double
yield_rate(int threads)
{
	thread_id t[64];
	sStop = 0;
	for (int i = 0; i < threads; i++) {
		sYields[i] = 0;
		t[i] = spawn_thread(yield_thread, "yield", B_NORMAL_PRIORITY,
			&sYields[i]);
		resume_thread(t[i]);
	}
	snooze(2000000);
	sStop = 1;
	long total = 0;
	for (int i = 0; i < threads; i++) {
		status_t r;
		wait_for_thread(t[i], &r);
		total += sYields[i];
	}
	return total / 2.0;
		// yields per second, all threads together
}


static int
compare(const void* a, const void* b)
{
	double x = *(const double*)a, y = *(const double*)b;
	return x < y ? -1 : x > y;
}


int
main(int argc, char** argv)
{
	int reps = 7;
	sRounds = 200000;
	int opt;
	while ((opt = getopt(argc, argv, "r:n:")) != -1) {
		if (opt == 'r')
			reps = atoi(optarg);
		else if (opt == 'n')
			sRounds = atoi(optarg);
	}
	if (reps < 1 || reps > 64)
		return 2;
	int cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
	int threads = cpus * 2 > 64 ? 64 : cpus * 2;

	double pp[64], yr[64];
	for (int i = 0; i < reps; i++) {
		pp[i] = pingpong();
		yr[i] = yield_rate(threads);
		printf("run %d: pingpong %.0f ns/round trip, yield %.0f /s (%d"
			" threads)\n", i + 1, pp[i], yr[i], threads);
	}
	qsort(pp, reps, sizeof(double), compare);
	qsort(yr, reps, sizeof(double), compare);
	printf("median: pingpong %.0f ns/round trip, yield %.0f /s\n",
		pp[reps / 2], yr[reps / 2]);
	return 0;
}
