/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Attributes CPU time to individual threads over a measurement window.

	`nettput` reports the *total* CPU cost of a transfer, summed over every CPU
	from cpu_info::active_time. That total is the number worth optimising but it
	says nothing about where the time goes. This tool partitions it.

	The partition is exact rather than statistical, and that is the point of
	using thread times instead of a sampling profiler. CPUEntry::TrackActivity()
	(scheduler_cpu.cpp) derives active_time by *adding up* the kernel_time and
	user_time deltas of every non-idle thread as it leaves the CPU, so the sum of
	the per-thread deltas below is the same quantity nettput divides by the byte
	count -- not an estimate of it. Any residual is therefore meaningful and is
	reported rather than hidden: see the reconciliation at the end of the output.

	Interrupt handler time is *not* broken out, because the kernel charges it to
	whichever thread happened to be running (interrupts.cpp only accumulates it
	separately in cpu_ent::irq_time, which userland cannot read). On a machine
	whose interrupts are woken by its own network threads this mostly lands on
	the right thread anyway, but time spent in an interrupt that arrives while a
	CPU is idle is charged to the idle thread and so escapes active_time
	entirely. That shows up here as a negative residual.
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


#define MAX_CPUS			64
#define MAX_THREADS			2048
#define DEFAULT_TOP_COUNT	24


struct thread_sample {
	thread_id	thread;
	team_id		team;
	bigtime_t	userTime;
	bigtime_t	kernelTime;
	char		name[B_OS_NAME_LENGTH];
	char		teamName[B_OS_NAME_LENGTH];
};


struct snapshot {
	bigtime_t		wallTime;
	uint32			cpuCount;
	bigtime_t		active[MAX_CPUS];
	int32			threadCount;
	thread_sample	threads[MAX_THREADS];
};


struct delta {
	thread_id	thread;
	bigtime_t	userTime;
	bigtime_t	kernelTime;
	bigtime_t	total;
	const char*	name;
	const char*	teamName;
	bool		appeared;
};


static struct snapshot sBefore;
static struct snapshot sAfter;
static struct delta sDeltas[MAX_THREADS];


/*!	Records every thread on the system plus the per-CPU active time.

	The CPU totals are read last so that the window they bound always contains
	the window the thread totals bound; a thread appearing between the two reads
	then shows up as a small positive residual rather than as a phantom cost.
*/
static status_t
take_snapshot(struct snapshot* snapshot)
{
	snapshot->threadCount = 0;

	int32 teamCookie = 0;
	team_info teamInfo;
	while (get_next_team_info(&teamCookie, &teamInfo) == B_OK) {
		int32 threadCookie = 0;
		thread_info threadInfo;
		while (get_next_thread_info(teamInfo.team, &threadCookie, &threadInfo)
				== B_OK) {
			if (snapshot->threadCount >= MAX_THREADS)
				break;

			struct thread_sample* sample
				= &snapshot->threads[snapshot->threadCount++];
			sample->thread = threadInfo.thread;
			sample->team = threadInfo.team;
			sample->userTime = threadInfo.user_time;
			sample->kernelTime = threadInfo.kernel_time;
			strlcpy(sample->name, threadInfo.name, sizeof(sample->name));
			strlcpy(sample->teamName, teamInfo.name, sizeof(sample->teamName));
		}
	}

	system_info systemInfo;
	if (get_system_info(&systemInfo) != B_OK)
		return errno != 0 ? errno : B_ERROR;

	snapshot->cpuCount = systemInfo.cpu_count;
	if (snapshot->cpuCount > MAX_CPUS)
		snapshot->cpuCount = MAX_CPUS;

	cpu_info cpuInfo[MAX_CPUS];
	if (get_cpu_info(0, snapshot->cpuCount, cpuInfo) != B_OK)
		return errno != 0 ? errno : B_ERROR;

	for (uint32 i = 0; i < snapshot->cpuCount; i++)
		snapshot->active[i] = cpuInfo[i].active_time;

	snapshot->wallTime = system_time();

	return B_OK;
}


static const struct thread_sample*
find_thread(const struct snapshot* snapshot, thread_id thread)
{
	for (int32 i = 0; i < snapshot->threadCount; i++) {
		if (snapshot->threads[i].thread == thread)
			return &snapshot->threads[i];
	}

	return NULL;
}


static int
compare_deltas(const void* _a, const void* _b)
{
	const struct delta* a = (const struct delta*)_a;
	const struct delta* b = (const struct delta*)_b;

	if (a->total > b->total)
		return -1;
	if (a->total < b->total)
		return 1;

	return 0;
}


static void
print_usage(const char* name)
{
	fprintf(stderr,
		"Usage: %s [-n <count>] [-b <bytes>] <seconds>\n"
		"Partitions the CPU time spent during a window across every thread on\n"
		"the system, and reconciles the partition against the per-CPU\n"
		"active_time that nettput measures.\n"
		"\n"
		"  -n <count>   Show the <count> busiest threads. Default %d; 0 shows\n"
		"               every thread that used any CPU at all.\n"
		"  -b <bytes>   Also express each thread's cost as microseconds per\n"
		"               mebibyte, given the bytes moved during the window.\n"
		"               Accepts a K/M/G suffix.\n",
		name, DEFAULT_TOP_COUNT);
}


static bool
parse_size(const char* text, double* _bytes)
{
	char* end = NULL;
	double value = strtod(text, &end);
	if (end == text)
		return false;

	switch (*end) {
		case 'k': case 'K':
			value *= 1024.0;
			end++;
			break;
		case 'm': case 'M':
			value *= 1024.0 * 1024.0;
			end++;
			break;
		case 'g': case 'G':
			value *= 1024.0 * 1024.0 * 1024.0;
			end++;
			break;
		default:
			break;
	}

	if (*end != '\0')
		return false;

	*_bytes = value;
	return true;
}


int
main(int argc, char** argv)
{
	int32 topCount = DEFAULT_TOP_COUNT;
	double bytes = 0.0;

	int option;
	while ((option = getopt(argc, argv, "n:b:h")) != -1) {
		switch (option) {
			case 'n':
				topCount = atol(optarg);
				break;
			case 'b':
				if (!parse_size(optarg, &bytes)) {
					fprintf(stderr, "%s: bad byte count \"%s\"\n", argv[0],
						optarg);
					return 1;
				}
				break;
			case 'h':
			default:
				print_usage(argv[0]);
				return option == 'h' ? 0 : 1;
		}
	}

	if (optind >= argc) {
		print_usage(argv[0]);
		return 1;
	}

	double seconds = atof(argv[optind]);
	if (seconds <= 0.0) {
		fprintf(stderr, "%s: duration must be positive\n", argv[0]);
		return 1;
	}

	if (take_snapshot(&sBefore) != B_OK) {
		fprintf(stderr, "%s: cannot read the initial state: %s\n", argv[0],
			strerror(errno));
		return 1;
	}

	snooze((bigtime_t)(seconds * 1000000.0));

	if (take_snapshot(&sAfter) != B_OK) {
		fprintf(stderr, "%s: cannot read the final state: %s\n", argv[0],
			strerror(errno));
		return 1;
	}

	// Build the deltas from the *later* snapshot, so that a thread created
	// during the window is reported with everything it used rather than
	// silently dropped.
	int32 deltaCount = 0;
	bigtime_t threadSum = 0;
	for (int32 i = 0; i < sAfter.threadCount; i++) {
		const struct thread_sample* after = &sAfter.threads[i];
		const struct thread_sample* before
			= find_thread(&sBefore, after->thread);

		struct delta* delta = &sDeltas[deltaCount];
		delta->thread = after->thread;
		delta->name = after->name;
		delta->teamName = after->teamName;
		delta->appeared = before == NULL;
		delta->userTime
			= after->userTime - (before != NULL ? before->userTime : 0);
		delta->kernelTime
			= after->kernelTime - (before != NULL ? before->kernelTime : 0);
		delta->total = delta->userTime + delta->kernelTime;

		if (delta->total <= 0)
			continue;

		threadSum += delta->total;
		deltaCount++;
	}

	qsort(sDeltas, deltaCount, sizeof(struct delta), compare_deltas);

	bigtime_t wall = sAfter.wallTime - sBefore.wallTime;
	bigtime_t activeSum = 0;
	for (uint32 i = 0; i < sAfter.cpuCount && i < sBefore.cpuCount; i++)
		activeSum += sAfter.active[i] - sBefore.active[i];

	double mib = bytes / (1024.0 * 1024.0);

	printf("netprof: %" B_PRId32 " cpus, window %.3f s wall\n",
		(int32)sAfter.cpuCount, wall / 1000000.0);
	printf("\n");
	printf("per-cpu active time over the window\n");
	for (uint32 i = 0; i < sAfter.cpuCount && i < sBefore.cpuCount; i++) {
		bigtime_t active = sAfter.active[i] - sBefore.active[i];
		printf("  cpu %-2" B_PRIu32 " %10" B_PRId64 " us  %6.2f%% busy\n", i,
			active, wall > 0 ? 100.0 * active / wall : 0.0);
	}
	printf("  %-6s %10" B_PRId64 " us  %6.2f%% of %" B_PRId32 " cpus\n",
		"total", activeSum,
		wall > 0 ? 100.0 * activeSum / (wall * (bigtime_t)sAfter.cpuCount) : 0.0,
		(int32)sAfter.cpuCount);

	printf("\n");
	printf("cpu time by thread (%s)\n",
		topCount > 0 ? "busiest first" : "every thread that ran");
	printf("      cpu us    user    kernel   %%window   %%total");
	if (mib > 0.0)
		printf("   us/MiB");
	printf("  thread\n");

	int32 shown = topCount > 0 && topCount < deltaCount ? topCount : deltaCount;
	for (int32 i = 0; i < shown; i++) {
		const struct delta* delta = &sDeltas[i];
		printf("  %10" B_PRId64 " %7" B_PRId64 " %9" B_PRId64 "  %7.2f%% %7.2f%%",
			delta->total, delta->userTime, delta->kernelTime,
			wall > 0 ? 100.0 * delta->total / wall : 0.0,
			activeSum > 0 ? 100.0 * delta->total / activeSum : 0.0);
		if (mib > 0.0)
			printf(" %8.1f", delta->total / mib);
		printf("  %s:%s (%" B_PRId32 ")%s\n", delta->teamName, delta->name,
			delta->thread, delta->appeared ? " [new]" : "");
	}

	if (shown < deltaCount) {
		bigtime_t rest = 0;
		for (int32 i = shown; i < deltaCount; i++)
			rest += sDeltas[i].total;
		printf("  %10" B_PRId64 " %7s %9s  %7.2f%% %7.2f%%", rest, "-", "-",
			wall > 0 ? 100.0 * rest / wall : 0.0,
			activeSum > 0 ? 100.0 * rest / activeSum : 0.0);
		if (mib > 0.0)
			printf(" %8.1f", rest / mib);
		printf("  ... %" B_PRId32 " further threads\n", deltaCount - shown);
	}

	printf("\n");
	printf("reconciliation\n");
	printf("  sum over threads      : %10" B_PRId64 " us\n", threadSum);
	printf("  sum over cpus (active): %10" B_PRId64 " us\n", activeSum);
	printf("  residual              : %10" B_PRId64 " us (%+.2f%%)\n",
		activeSum - threadSum,
		threadSum > 0 ? 100.0 * (activeSum - threadSum) / threadSum : 0.0);
	if (mib > 0.0) {
		printf("  cost of %.1f MiB       : %10.1f us/MiB (threads),"
			" %.1f us/MiB (cpus)\n", mib, threadSum / mib, activeSum / mib);
	}

	return 0;
}
