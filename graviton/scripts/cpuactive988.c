/*
 * cpuactive988 -- does per-CPU busy time ever run ahead of the wall clock, or
 * step backwards, between two close samples? (DeBeOS issue 988)
 *
 * Haiku: reads get_cpu_info() active_time. Linux control: reads /proc/stat
 * (busy = user+nice+system+irq+softirq+steal, idle = idle), which only has
 * USER_HZ resolution, so its rows are about direction, not microseconds.
 *
 * Load: <spin> processes that spin forever, plus <mix> processes that spin for
 * a few milliseconds and then sleep for one, so the CPUs reschedule all the
 * time and switch between busy and idle mid-quantum.
 *
 * Phases, each <secs> long under the same load:
 *   tight  sample as fast as possible, report every backwards step of busy,
 *          and every pair where busy grew by more than the wall time that
 *          could possibly have passed (t_after(k) - t_before(k-1)), which is
 *          exactly "idle = uptime - busy went backwards".
 *   100ms  sample every 100 ms, the issue's libuv-style measurement: busy
 *          share against the wall time between the midpoints of two samples
 *          (which can overshoot 100% by up to one get_cpu_info() call even on
 *          a correct kernel), and, as the pass/fail row, the same bracketed
 *          check as the tight phase.
 *   1000ms the same every second.
 * Finally the total busy time of the 100ms phase is printed against the
 * expected spin share, so a fix that merely stopped the clock would show.
 *
 * usage: cpuactive988 [-s spin] [-m mix] [-t secs]
 * Exit 0 if every row is clean, 1 otherwise.
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __HAIKU__
#	include <OS.h>
#endif

#define MAX_CPUS 256

static int sCPUs;
static int64_t sSlack;
	// how far one sample's busy may legitimately exceed the bracketing wall
	// time: 0 for Haiku's microsecond counter, one tick for /proc/stat


static int64_t
now_us(void)
{
#ifdef __HAIKU__
	return system_time();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}


/* busy[i] in microseconds */
static int
read_busy(int64_t* busy)
{
#ifdef __HAIKU__
	cpu_info info[MAX_CPUS];
	if (get_cpu_info(0, sCPUs, info) != B_OK)
		return -1;
	for (int i = 0; i < sCPUs; i++)
		busy[i] = info[i].active_time;
	return 0;
#else
	static long hz;
	if (hz == 0)
		hz = sysconf(_SC_CLK_TCK);
	FILE* f = fopen("/proc/stat", "r");
	if (f == NULL)
		return -1;
	char line[512];
	int n = 0;
	while (fgets(line, sizeof(line), f) != NULL && n < sCPUs) {
		int cpu;
		unsigned long long u, ni, sy, id, io, irq, sirq, st;
		if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9')
			continue;
		if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu,
				&u, &ni, &sy, &id, &io, &irq, &sirq, &st) != 9)
			continue;
		busy[cpu] = (int64_t)(u + ni + sy + irq + sirq + st) * 1000000 / hz;
		n++;
	}
	fclose(f);
	return n == sCPUs ? 0 : -1;
#endif
}


static void
spin_forever(void)
{
	volatile unsigned long x = 0;
	for (;;)
		x++;
}


static void
spin_and_nap(unsigned seed)
{
	volatile unsigned long x = 0;
	for (;;) {
		int64_t until = now_us() + 1000 + (seed = seed * 1103515245 + 12345)
			% 4000;
		while (now_us() < until)
			x++;
		usleep(500 + seed % 1000);
	}
}


static int
tight_phase(int secs)
{
	static int64_t prev[MAX_CPUS], cur[MAX_CPUS];
	int64_t prevBefore, before, after;
	long samples = 0, backwards = 0, ahead = 0;
	int64_t maxBack = 0, maxAhead = 0;

	prevBefore = now_us();
	if (read_busy(prev) != 0)
		return -1;
	int64_t end = prevBefore + (int64_t)secs * 1000000;
	do {
		before = now_us();
		if (read_busy(cur) != 0)
			return -1;
		after = now_us();
		for (int i = 0; i < sCPUs; i++) {
			int64_t delta = cur[i] - prev[i];
			int64_t wall = after - prevBefore;
			samples++;
			if (delta < 0) {
				backwards++;
				if (-delta > maxBack)
					maxBack = -delta;
			}
			if (delta > wall + sSlack) {
				ahead++;
				if (delta - wall > maxAhead)
					maxAhead = delta - wall;
			}
			prev[i] = cur[i];
		}
		prevBefore = before;
	} while (after < end);

	printf("tight: %ld per-CPU samples in %d s, busy went backwards in %ld"
		" (max %lld us), busy ran ahead of wall (idle went back) in %ld"
		" (max %lld us; slack %lld us)\n", samples, secs, backwards,
		(long long)maxBack, ahead, (long long)maxAhead, (long long)sSlack);
	return backwards + ahead > 0;
}


static int
interval_phase(int secs, int intervalMs, int64_t* _totalBusy, int64_t* _wall)
{
	static int64_t prev[MAX_CPUS], cur[MAX_CPUS];
	long samples = 0, outside = 0, idleBack = 0, ahead = 0, back = 0;
	int64_t maxIdleBack = 0, maxAhead = 0;
	double maxShare = 0;
	int64_t totalBusy = 0;

	int64_t prevBefore = now_us();
	if (read_busy(prev) != 0)
		return -1;
	int64_t prevT = (prevBefore + now_us()) / 2;
	int64_t start = prevT;
	int rounds = secs * 1000 / intervalMs;
	for (int r = 0; r < rounds; r++) {
		usleep(intervalMs * 1000);
		int64_t t0 = now_us();
		if (read_busy(cur) != 0)
			return -1;
		int64_t t1 = now_us();
		int64_t t = (t0 + t1) / 2;
		for (int i = 0; i < sCPUs; i++) {
			int64_t delta = cur[i] - prev[i];
			int64_t wall = t - prevT;
			double share = (double)delta / (double)wall;
			// idle = uptime - busy, in the same time base
			int64_t idleDelta = wall - delta;
			samples++;
			if (share < 0 || share > 1)
				outside++;
			if (idleDelta < 0) {
				idleBack++;
				if (-idleDelta > maxIdleBack)
					maxIdleBack = -idleDelta;
			}
			if (delta < 0)
				back++;
			if (delta > t1 - prevBefore + sSlack) {
				ahead++;
				if (delta - (t1 - prevBefore) > maxAhead)
					maxAhead = delta - (t1 - prevBefore);
			}
			if (share > maxShare)
				maxShare = share;
			totalBusy += delta;
			prev[i] = cur[i];
		}
		prevT = t;
		prevBefore = t0;
	}

	printf("every %d ms: %ld per-CPU samples; by midpoints: idle went back in"
		" %ld (max %lld us), busy share outside [0,1] in %ld, max busy share"
		" %.3f%%; bracketed: busy went backwards in %ld, ran ahead of wall in"
		" %ld (max %lld us)\n", intervalMs, samples, idleBack,
		(long long)maxIdleBack, outside, maxShare * 100, back, ahead,
		(long long)maxAhead);
	if (_totalBusy != NULL)
		*_totalBusy = totalBusy;
	if (_wall != NULL)
		*_wall = prevT - start;
	return back + ahead > 0;
}


int
main(int argc, char** argv)
{
	int spin = 4, mix = 2, secs = 10;
	int opt;
	while ((opt = getopt(argc, argv, "s:m:t:")) != -1) {
		switch (opt) {
			case 's': spin = atoi(optarg); break;
			case 'm': mix = atoi(optarg); break;
			case 't': secs = atoi(optarg); break;
			default:
				fprintf(stderr, "usage: %s [-s spin] [-m mix] [-t secs]\n",
					argv[0]);
				return 2;
		}
	}

	sCPUs = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (sCPUs < 1 || sCPUs > MAX_CPUS)
		return 2;
#ifndef __HAIKU__
	sSlack = 1000000 / sysconf(_SC_CLK_TCK);
#endif

	pid_t children[512];
	int childCount = 0;
	for (int i = 0; i < spin + mix && childCount < 512; i++) {
		pid_t pid = fork();
		if (pid == 0) {
			if (i < spin)
				spin_forever();
			spin_and_nap(0x9e3779b9u * (i + 1));
			_exit(0);
		}
		if (pid > 0)
			children[childCount++] = pid;
	}
	usleep(200000);

	printf("cpuactive988: %d CPUs, load %d spinners + %d spin/nap, %d s per"
		" phase\n", sCPUs, spin, mix, secs);
	int bad = 0;
	int r = tight_phase(secs);
	bad |= r != 0;
	int64_t totalBusy = 0, wall = 0;
	r = interval_phase(secs, 100, &totalBusy, &wall);
	bad |= r != 0;
	r = interval_phase(secs, 1000, NULL, NULL);
	bad |= r != 0;
	printf("100ms phase total busy %lld ms over %lld ms wall: %.2f CPUs busy"
		" (%d spinners alone would be %d.00; spin/nap adds less than %d)\n",
		(long long)(totalBusy / 1000), (long long)(wall / 1000),
		(double)totalBusy / (double)wall, spin, spin, mix);

	for (int i = 0; i < childCount; i++)
		kill(children[i], SIGKILL);
	for (int i = 0; i < childCount; i++)
		waitpid(children[i], NULL, 0);

	printf("RESULT: %s\n", bad ? "FAIL" : "PASS");
	return bad;
}
