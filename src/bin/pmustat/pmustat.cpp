/*
 * Copyright 2026 DeBeOS. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	pmustat -- read the arm64 PMUv3 counters and report the aggregate set the
	AWS Graviton performance runbook investigates, in the runbook's order: IPC,
	branch MPKI, L1D/L2/LLC MPKI, dTLB/iTLB page-table walks, and the
	front-end/back-end stall split.

	How it reaches the PMU. The kernel PMU facility keeps EL0 away from the
	counters entirely (PMUSERENR_EL0 is zero; there is no dedicated syscall and
	no /dev node) because the counters are per core, not saved across a context
	switch, and PMCCNTR_EL0 at EL0 is a timing side channel. So this tool does
	not read a counter itself. It goes through the one narrow kernel conduit --
	the "arm64_pmu" generic syscall (headers/private/system/arm64_pmu_syscalls.h,
	handled in src/system/kernel/arch/arm64/arch_pmu.cpp) -- which programs the
	event counters (PMEVTYPER/PMEVCNTR) and reads them back on the CPU the
	calling thread is on.

	Because the counters are strictly per core, the tool pins itself to one CPU
	(sched_setaffinity) for the whole measurement. It runs the workload in
	chunks and reads the counters between chunks: the 32-bit event counters wrap
	in well under a second at Graviton clocks, and the kernel only extends them
	across the interval since the previous read, so polling is what keeps a
	multi-second measurement honest.

	Only five general event counters exist on the Neoverse parts (six, less one
	reserved for the sampling profiler), and the runbook set needs more than
	five events, so the tool measures in three passes -- rerunning the workload
	with a different <=5-event set each time -- and stitches the results
	together. Each ratio is computed from the pass that carries both its
	numerator and its instructions/cycles, so the arithmetic never mixes passes.

	Honesty caveats it prints: the counters count everything that runs on the
	pinned core, not just this thread (there is no per-thread PMU here), and on
	a virtualized instance a counter advances only while the vCPU is scheduled.
	The direct counter reads this tool uses are exact for what executed on the
	core; it is the sampling-overflow interrupt rate (which this tool does not
	use) that is coarse under virtualization. Any counter that reads zero for an
	event the part does not implement is flagged rather than folded into a ratio.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <arm64_pmu_syscalls.h>
#include <syscalls.h>


// ARM PMUv3 architected event numbers used here. Kept local so the tool does
// not pull in a kernel-private register header; they match the PMU_EVENT_*
// values in headers/private/kernel/arch/arm64/arm_registers.h.
enum {
	EV_L1D_CACHE_REFILL		= 0x0003,
	EV_L1D_CACHE			= 0x0004,
	EV_L1D_TLB_REFILL		= 0x0005,
	EV_L1I_TLB_REFILL		= 0x0002,
	EV_INST_RETIRED			= 0x0008,
	EV_CPU_CYCLES			= 0x0011,
	EV_L2D_CACHE_REFILL		= 0x0017,
	EV_BR_RETIRED			= 0x0021,
	EV_BR_MIS_PRED_RETIRED	= 0x0022,
	EV_STALL_FRONTEND		= 0x0023,
	EV_STALL_BACKEND		= 0x0024,
	EV_DTLB_WALK			= 0x0034,
	EV_ITLB_WALK			= 0x0035,
	EV_LL_CACHE_RD			= 0x0036,
	EV_LL_CACHE_MISS_RD		= 0x0037,
};


// How a metric's ratio is expressed, following the runbook's conventions.
enum ratio_kind {
	RATIO_MPKI,		// misses per 1000 instructions
	RATIO_WALKS,	// raw page-table-walk count, plus per-1000-instructions
	RATIO_STALL		// percent of cycles the pipe stalled
};


// The runbook aggregate set, in the runbook's investigation order. IPC is
// derived from INST_RETIRED and the always-available cycle counter and so is
// not listed here. Every other metric needs its own event counter alongside
// INST_RETIRED (the MPKI/stall denominators). The tool packs these into as few
// passes as the instance's general-counter count allows.
struct metric_def {
	uint16		event;
	const char*	label;
	ratio_kind	ratio;
};

static const metric_def kMetrics[] = {
	{ EV_BR_MIS_PRED_RETIRED,	"branch-mispredict",	RATIO_MPKI },
	{ EV_L1D_CACHE_REFILL,		"l1d-cache",			RATIO_MPKI },
	{ EV_L2D_CACHE_REFILL,		"l2-cache",				RATIO_MPKI },
	{ EV_LL_CACHE_MISS_RD,		"llc (last-level) read",RATIO_MPKI },
	{ EV_DTLB_WALK,				"dtlb page-table walk",	RATIO_WALKS },
	{ EV_ITLB_WALK,				"itlb page-table walk",	RATIO_WALKS },
	{ EV_STALL_FRONTEND,		"frontend stall",		RATIO_STALL },
	{ EV_STALL_BACKEND,			"backend stall",		RATIO_STALL },
};

static const int kMetricCount = (int)(sizeof(kMetrics) / sizeof(kMetrics[0]));


// What a metric measured, once its pass has run. value/instructions/cycles all
// come from the SAME pass, so a ratio never mixes two runs of the workload.
struct metric_result {
	bool	captured;
	uint64	value;
	uint64	instructions;
	uint64	cycles;
};

static metric_result sMetrics[kMetricCount];

// IPC comes from any pass; recorded from the first that runs.
static uint64 sIpcInstructions;
static uint64 sIpcCycles;
static bool sIpcValid;


// #pragma mark - workloads


static size_t sBufferBytes = 64 * 1024 * 1024;
static unsigned char* sBufferA;
static unsigned char* sBufferB;

static int sMatrixN = 384;
static float* sMatA;
static float* sMatB;
static float* sMatC;


static void
workload_memset(void)
{
	// One streaming pass over a large buffer: dominated by store bandwidth and
	// L1D/L2 refills, almost no branches -- a clean high-IPC streaming case.
	memset(sBufferA, 0xa5, sBufferBytes);
}


static void
workload_memcpy(void)
{
	// Streaming copy: loads and stores, exercising the #326 routine on real
	// data, so L1D/L2/LLC refills climb with the working set.
	memcpy(sBufferA, sBufferB, sBufferBytes);
}


static void
workload_matmul(void)
{
	// Naive triple-loop single-precision matmul, the shape of a ggml/llama.cpp
	// (#331) inner kernel: compute-bound with a predictable stride, so IPC is
	// high and branch mispredicts are low. One call is one full C = A*B.
	const int n = sMatrixN;
	for (int i = 0; i < n; i++) {
		for (int j = 0; j < n; j++) {
			float sum = 0.0f;
			const float* a = &sMatA[(size_t)i * n];
			const float* b = &sMatB[j];
			for (int k = 0; k < n; k++)
				sum += a[k] * b[(size_t)k * n];
			sMatC[(size_t)i * n + j] = sum;
		}
	}
}


typedef void (*workload_fn)(void);


struct workload_def {
	const char*	name;
	workload_fn	fn;
	const char*	description;
};

static const workload_def kWorkloads[] = {
	{ "matmul", workload_matmul, "naive float matmul (ggml/llama.cpp shape, #331)" },
	{ "memcpy", workload_memcpy, "streaming memcpy over a large buffer (#326)" },
	{ "memset", workload_memset, "streaming memset over a large buffer (#326)" },
};

static const int kWorkloadCount
	= (int)(sizeof(kWorkloads) / sizeof(kWorkloads[0]));


static bool
alloc_workload_buffers(void)
{
	sBufferA = (unsigned char*)malloc(sBufferBytes);
	sBufferB = (unsigned char*)malloc(sBufferBytes);
	if (sBufferA == NULL || sBufferB == NULL)
		return false;
	memset(sBufferA, 0x5a, sBufferBytes);
	memset(sBufferB, 0x3c, sBufferBytes);

	size_t matBytes = (size_t)sMatrixN * sMatrixN * sizeof(float);
	sMatA = (float*)malloc(matBytes);
	sMatB = (float*)malloc(matBytes);
	sMatC = (float*)malloc(matBytes);
	if (sMatA == NULL || sMatB == NULL || sMatC == NULL)
		return false;
	for (int i = 0; i < sMatrixN * sMatrixN; i++) {
		sMatA[i] = (float)(i % 7) * 0.5f + 1.0f;
		sMatB[i] = (float)(i % 5) * 0.25f + 0.5f;
	}
	return true;
}


// #pragma mark - PMU conduit


static status_t
pmu_info(arm64_pmu_user_info* info)
{
	return _kern_generic_syscall(ARM64_PMU_SYSCALLS, ARM64_PMU_SYSCALL_INFO,
		info, sizeof(*info));
}


static status_t
pmu_set_events(const uint16* events, uint32 count)
{
	arm64_pmu_user_events request;
	memset(&request, 0, sizeof(request));
	request.count = count;
	for (uint32 i = 0; i < count && i < ARM64_PMU_USER_MAX_EVENTS; i++)
		request.events[i] = events[i];
	return _kern_generic_syscall(ARM64_PMU_SYSCALLS,
		ARM64_PMU_SYSCALL_SET_EVENTS, &request, sizeof(request));
}


static status_t
pmu_read(arm64_pmu_user_sample* sample)
{
	return _kern_generic_syscall(ARM64_PMU_SYSCALLS, ARM64_PMU_SYSCALL_READ,
		sample, sizeof(*sample));
}


// Finds the value of a given event in a sample, or returns false if the event
// was not among the programmed counters (dropped for lack of room, or never
// requested). This is what keeps the reporting data-driven rather than
// assuming a counter layout.
static bool
sample_value(const arm64_pmu_user_sample& sample, uint16 event, uint64* value)
{
	for (uint32 i = 0; i < sample.eventCount; i++) {
		if (sample.events[i] == event) {
			*value = sample.values[i];
			return true;
		}
	}
	return false;
}


// #pragma mark - measurement driver


static bool
pin_to_cpu(int cpu)
{
	// The kernel's affinity mask is a bitmap of uint32 words (its CPUSet); bit
	// N of word N/32 selects CPU N. It copies only the first sizeof(CPUSet)
	// bytes, so a generously oversized zeroed buffer with the one bit set is
	// layout-compatible without needing the kernel-private CPUSet header. A
	// thread id of 0 means the calling thread.
	uint32 mask[32];
	memset(mask, 0, sizeof(mask));
	mask[cpu / 32] = 1u << (cpu % 32);
	return _kern_set_thread_affinity(0, mask, sizeof(mask)) == B_OK;
}


/*!	Runs one pass: program the given \a events on the pinned CPU, run the
	workload for \a durationUs microseconds, reading the counters often enough
	that no 32-bit counter wraps twice between reads, and return the final
	accumulated sample. events[0] must be INST_RETIRED (the ratio denominator).
*/
static status_t
run_pass(const uint16* events, uint32 count, workload_fn workload,
	bigtime_t durationUs, arm64_pmu_user_sample* out)
{
	status_t status = pmu_set_events(events, count);
	if (status != B_OK)
		return status;

	bigtime_t deadline = system_time() + durationUs;
	do {
		workload();
		// Read after every workload unit: this both accumulates the counters in
		// the kernel across the run and bounds the wrap window to one unit's
		// worth of cycles, which for these workloads is a handful of ms.
		status = pmu_read(out);
		if (status != B_OK)
			return status;
	} while (system_time() < deadline);

	return B_OK;
}


/*!	Packs the runbook metrics into passes of at most \a generalCounters events
	(INST_RETIRED plus up to generalCounters-1 metrics per pass), runs each pass,
	and records every metric's value alongside the instructions and cycles of the
	same pass. Adapts to the instance: five general counters take two passes,
	one general counter yields IPC only and every metric stays uncaptured.
*/
static status_t
measure(workload_fn workload, bigtime_t durationUs, uint32 generalCounters)
{
	uint32 metricsPerPass = generalCounters > 1 ? generalCounters - 1 : 0;

	int next = 0;
	do {
		uint16 events[ARM64_PMU_USER_MAX_EVENTS];
		uint32 count = 0;
		events[count++] = EV_INST_RETIRED;

		int firstMetric = next;
		while (next < kMetricCount && (count - 1) < metricsPerPass)
			events[count++] = kMetrics[next++].event;

		arm64_pmu_user_sample sample;
		status_t status = run_pass(events, count, workload, durationUs, &sample);
		if (status != B_OK)
			return status;

		uint64 inst = 0;
		sample_value(sample, EV_INST_RETIRED, &inst);
		if (!sIpcValid) {
			sIpcInstructions = inst;
			sIpcCycles = sample.cycles;
			sIpcValid = true;
		}

		for (int m = firstMetric; m < next; m++) {
			uint64 value;
			if (sample_value(sample, kMetrics[m].event, &value)) {
				sMetrics[m].captured = true;
				sMetrics[m].value = value;
				sMetrics[m].instructions = inst;
				sMetrics[m].cycles = sample.cycles;
			}
		}

		// When no metric fits (one general counter), still run exactly one pass
		// so IPC is measured, then stop.
		if (metricsPerPass == 0)
			break;
	} while (next < kMetricCount);

	return B_OK;
}


// #pragma mark - reporting


static void
report(const char* workloadName, uint32 generalCounters)
{
	printf("\n=== pmustat: AWS Graviton perf-runbook aggregate set ===\n");
	printf("workload: %s\n\n", workloadName);
	printf("derived (runbook investigation order):\n");

	// 1. IPC -- instructions retired over the free-running cycle counter.
	if (sIpcValid && sIpcCycles != 0) {
		printf("  %-26s %12.3f  inst/cycle\n", "ipc",
			(double)sIpcInstructions / (double)sIpcCycles);
	} else {
		printf("  %-26s %12s\n", "ipc", "n/a");
	}

	// 2..5. Each metric, from the pass that captured it. A metric that no pass
	// could program (too few counters, or the part does not implement the event)
	// is reported as n/a with the reason, never invented.
	for (int m = 0; m < kMetricCount; m++) {
		const metric_def& def = kMetrics[m];
		const metric_result& r = sMetrics[m];

		if (!r.captured) {
			printf("  %-26s %12s  (no counter free: instance exposes %u general "
				"counter%s)\n", def.label, "n/a", generalCounters,
				generalCounters == 1 ? "" : "s");
			continue;
		}

		switch (def.ratio) {
			case RATIO_MPKI:
			{
				if (r.instructions == 0) {
					printf("  %-26s %12s  (no instructions)\n", def.label, "n/a");
					break;
				}
				double mpki = (double)r.value * 1000.0 / (double)r.instructions;
				printf("  %-26s %12.3f  mpki%s\n", def.label, mpki,
					r.value == 0 ? "   (reads zero: event not implemented?)" : "");
				break;
			}
			case RATIO_WALKS:
			{
				double perki = r.instructions != 0
					? (double)r.value * 1000.0 / (double)r.instructions : 0.0;
				printf("  %-26s %12llu  walks (%.3f /kinst)\n", def.label,
					(unsigned long long)r.value, perki);
				break;
			}
			case RATIO_STALL:
			{
				if (r.cycles == 0) {
					printf("  %-26s %12s  (no cycles)\n", def.label, "n/a");
					break;
				}
				double pct = (double)r.value * 100.0 / (double)r.cycles;
				printf("  %-26s %12.3f  %% of cycles\n", def.label, pct);
				break;
			}
		}
	}

	printf("\nnote: counters are per CORE, not per thread -- they include\n"
		"      everything the pinned core ran, and on a virtualized instance a\n"
		"      counter advances only while the vCPU is scheduled. These are\n"
		"      DIRECT reads (exact for what executed on the core); it is the\n"
		"      sampling-overflow rate -- which pmustat does not use -- that is\n"
		"      coarse under virtualization. AWS exposes the full PMU only on the\n"
		"      16xlarge and metal sizes; smaller guests expose fewer counters, so\n"
		"      fewer metrics fit and some read n/a above.\n");
}


// #pragma mark - main


static void
usage(const char* self)
{
	fprintf(stderr,
		"usage: %s [-w workload] [-t seconds] [-p cpu]\n"
		"  -w  workload: matmul (default), memcpy, memset\n"
		"  -t  seconds per pass (default 2); the metric set is measured in as\n"
		"      many passes as the instance's general-counter count requires\n"
		"  -p  CPU to pin to (default 1, or 0 on a single-CPU instance)\n"
		"\n"
		"Reads the arm64 PMUv3 counters through the kernel \"arm64_pmu\"\n"
		"conduit and reports IPC, branch MPKI, L1D/L2/LLC MPKI, TLB walks and\n"
		"the frontend/backend stall split for the chosen workload.\n"
		"Requires the \"arm64_pmu\" kernel boot setting (baked into DeBeOS\n"
		"arm64 images).\n", self);
}


int
main(int argc, char** argv)
{
	const workload_def* workload = &kWorkloads[0];
	bigtime_t durationUs = 2000000;
	int cpu = -1;

	int opt;
	while ((opt = getopt(argc, argv, "w:t:p:h")) != -1) {
		switch (opt) {
			case 'w':
			{
				workload = NULL;
				for (int i = 0; i < kWorkloadCount; i++) {
					if (strcmp(optarg, kWorkloads[i].name) == 0) {
						workload = &kWorkloads[i];
						break;
					}
				}
				if (workload == NULL) {
					fprintf(stderr, "unknown workload: %s\n", optarg);
					return 1;
				}
				break;
			}
			case 't':
				durationUs = (bigtime_t)(atof(optarg) * 1000000.0);
				if (durationUs < 100000)
					durationUs = 100000;
				break;
			case 'p':
				cpu = atoi(optarg);
				break;
			case 'h':
			default:
				usage(argv[0]);
				return opt == 'h' ? 0 : 1;
		}
	}

	// Check the facility before touching a counter.
	arm64_pmu_user_info info;
	memset(&info, 0, sizeof(info));
	status_t status = pmu_info(&info);
	if (status != B_OK) {
		fprintf(stderr, "pmustat: the arm64_pmu kernel conduit is not "
			"available (%s). This tool needs an arm64 DeBeOS kernel with the "
			"PMU facility.\n", strerror(status));
		return 1;
	}
	if (!info.available) {
		fprintf(stderr, "pmustat: PMUv3 is not implemented on this CPU.\n");
		return 1;
	}
	if (!info.enabled) {
		fprintf(stderr, "pmustat: the PMU facility is present but disabled. "
			"Boot with the \"arm64_pmu\" kernel setting (add 'arm64_pmu true' "
			"to the kernel driver settings) and retry.\n");
		return 1;
	}

	system_info sysInfo;
	get_system_info(&sysInfo);
	int cpuCount = (int)sysInfo.cpu_count;
	if (cpu < 0)
		cpu = cpuCount > 1 ? 1 : 0;
	if (cpu >= cpuCount)
		cpu = cpuCount - 1;
	if (!pin_to_cpu(cpu)) {
		fprintf(stderr, "pmustat: could not pin to CPU %d\n", cpu);
		return 1;
	}

	printf("pmustat: PMUv3 present (PMUVer %llu), %u general counters, %s "
		"event counters; pinned to CPU %d\n",
		(unsigned long long)info.pmuVersion, info.generalCounters,
		info.longCounters ? "64-bit" : "32-bit", cpu);

	if (!alloc_workload_buffers()) {
		fprintf(stderr, "pmustat: out of memory allocating workload buffers\n");
		return 1;
	}

	status = measure(workload->fn, durationUs, info.generalCounters);
	if (status != B_OK) {
		fprintf(stderr, "pmustat: measurement failed: %s\n", strerror(status));
		return 1;
	}

	report(workload->name, info.generalCounters);
	return 0;
}
