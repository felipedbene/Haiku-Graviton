/*
 * smpscale -- does this machine actually execute N threads on N CPUs?
 *
 * Written to refute, or confirm with better evidence, a claim that 16 CPUs run
 * in parallel on Graviton. The claim it replaces was a POSIX-shell arithmetic
 * loop timed with `date +%s` -- one-second resolution against a ~2 s
 * measurement, two data points, and a shell whose fork and variable-scoping
 * behaviour was never established. This tool removes every one of those
 * degrees of freedom:
 *
 *  - Timing is system_time(), microseconds.
 *  - The work is a serial dependency chain of integer multiply-add in
 *    registers. It cannot be vectorised, strength-reduced, cached, prefetched
 *    or memory-bound, so its rate is a direct measure of one core's issue
 *    latency and nothing else. The result is consumed, so it cannot be
 *    optimised away.
 *  - Each thread does an IDENTICAL, FIXED amount of work. Perfect scaling
 *    therefore holds wall time constant as N rises; perfect serialisation
 *    makes wall time proportional to N. Those two predictions differ by 16x at
 *    N=16, which no timing error can bridge.
 *  - A scaling CURVE at 1,2,4,8,16,... threads, not two points. A curve is
 *    falsifiable: partial parallelism (k usable cores) shows up as a knee at
 *    N=k, which two points cannot distinguish from full parallelism.
 *  - Two INDEPENDENT measures of the same fact: aggregate wall-clock
 *    throughput, and per-CPU cpu_info::active_time deltas. If they disagree,
 *    one of them is broken and we learn that too, instead of trusting a single
 *    number.
 *
 * Usage: smpscale [-w warmup_ms] [-t target_ms] [-m] [-g] [-x] [-s us] [-p]
 *                 [threads ...]
 *   -w   per-run warmup in ms (default 200)
 *   -t   calibrate the work unit so ONE thread takes about this long
 *        (default 3000 ms). Bigger swamps timing error; 3 s against a 1 us
 *        clock is 6 orders of magnitude of margin.
 *   -m   also run a memory-streaming variant, to separate CPU parallelism from
 *        memory-bandwidth saturation. The network receive path is 88 %
 *        per-byte cost, so whether memory bandwidth scales matters as much as
 *        whether cores do.
 *   -g   COUNT MIGRATIONS. Break the work into ~1 ms chunks and have each
 *        thread sample sched_getcpu() between them, so a fix that reaches full
 *        efficiency by thrashing is distinguishable from one that reaches it by
 *        migrating a bounded number of times. This costs a syscall per chunk,
 *        so its wall times are NOT comparable with the plain ladder -- run it
 *        as a separate experiment, never as the primary evidence.
 *   -x   MIXED WORKLOAD negative control (implies -g). Odd-numbered threads
 *        sleep 4 ms per 1 ms of work and do a sixteenth of the work; even ones
 *        stay purely CPU-bound. Sleepers pass through the scheduler's
 *        rebalance path on every wake, so this is where an over-eager
 *        balancer would show up. The CPU-bound half is reported separately,
 *        since averaging the two classes would hide a straggler.
 *   -s   stagger thread spawns by this many MICROseconds. The imbalance
 *        vanishes once spawns are separated by about 1 ms, which is exactly
 *        kLoadMeasureInterval, so this turns that observation into a dial that
 *        can be swept: -s 0 against -s 1000 is the experiment.
 *   -p   after the ladder, ask an instrumented kernel to dump its choose_core()
 *        placement trace. No effect on a stock kernel.
 *   threads   the ladder; default "1 2 4 8 16"
 *
 * READ THE PER-CPU TABLE, NOT `eff`. `eff` is baseWall/wallMs against the FIRST
 * ladder row, so in a single-point run it is trivially 1.000 and means nothing.
 * It also SATURATES: one doubled core pins it to 0.500 whether one core is
 * doubled or five, and whether one CPU is idle or nine. The busy/idle CPU counts
 * and max/min below it are the honest measures.
 */

#include <OS.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// From libgnu.so. Declared here rather than pulled in via <sched.h>, which only
// exposes it under _DEFAULT_SOURCE and from the gnu compatibility include dir.
// On every non-x86_64 target this is a straight _kern_get_cpu() syscall
// (src/libs/gnu/sched_getcpu.cpp), so a thread asking which CPU it is on gets
// an authoritative answer from the kernel.
extern "C" int sched_getcpu(void);

// Private libroot syscall. With an instrumented kernel, the magic value 0x5350
// makes the kernel dprintf its choose_core() placement trace to the syslog and
// serial console; on a stock kernel it simply fails, which is harmless. Dumping
// has to be asked for from ordinary thread context because it does blocking
// per-character serial I/O.
extern "C" status_t _kern_set_scheduler_mode(int32 mode);
#define SMPSCALE_DUMP_PLACEMENT	0x5350


#define MAX_CPUS		256
#define MAX_THREADS		256
#define MAX_LADDER		32

// 16 MiB per streaming thread: comfortably past any plausible last-level cache
// slice, so the streaming variant really does go to DRAM.
#define STREAM_BYTES	(16 * 1024 * 1024)


struct cpu_snapshot {
	uint32		count;
	bigtime_t	active[MAX_CPUS];
	bigtime_t	wall;
};


static void
take_cpu_snapshot(cpu_snapshot& snapshot)
{
	system_info systemInfo;
	cpu_info cpuInfo[MAX_CPUS];

	snapshot.count = 0;
	snapshot.wall = system_time();

	if (get_system_info(&systemInfo) != B_OK)
		return;

	uint32 count = systemInfo.cpu_count;
	if (count > MAX_CPUS)
		count = MAX_CPUS;

	if (get_cpu_info(0, count, cpuInfo) != B_OK)
		return;

	snapshot.count = count;
	for (uint32 i = 0; i < count; i++)
		snapshot.active[i] = cpuInfo[i].active_time;
}


// The work. A serial dependency chain: each iteration needs the previous
// result, so there is no instruction-level parallelism to exploit and no way
// for the compiler to hoist, unroll into independent chains, or vectorise it.
// One core's rate is a hardware constant; N cores' aggregate rate is the thing
// under test.
static uint64
spin_chain(uint64 iterations, uint64 seed)
{
	uint64 x = seed;
	for (uint64 i = 0; i < iterations; i++) {
		// Multiply is 3-4 cycles of latency on Neoverse and the xor-shift
		// keeps the value from collapsing to zero or a short cycle.
		x = x * 6364136223846793005ULL + 1442695040888963407ULL;
		x ^= x >> 29;
	}
	return x;
}


// Streaming variant: read and write a buffer larger than cache, so the limit is
// memory bandwidth rather than core issue rate.
static uint64
spin_stream(uint64 rounds, volatile uint64* buffer, size_t words)
{
	uint64 sum = 0;
	for (uint64 r = 0; r < rounds; r++) {
		for (size_t i = 0; i < words; i++)
			sum += buffer[i];
		for (size_t i = 0; i < words; i++)
			buffer[i] = sum + i;
	}
	return sum;
}


struct worker_arg {
	uint64			iterations;
	uint64			seed;
	bool			stream;
	volatile uint64* buffer;
	size_t			words;

	// Chunked mode. When chunkIterations is non-zero the work is broken into
	// pieces and sched_getcpu() is sampled between them, which counts
	// migrations from userland with no kernel change and no image bake. It is
	// deliberately a SEPARATE mode: the syscall between chunks costs time, so
	// it must never contaminate the timing ladder that is the primary evidence.
	uint64			chunkIterations;
	bigtime_t		snoozeMicros;	// > 0 makes this a sleeper, not CPU-bound
	bigtime_t		releaseTime;	// absolute instant to wake at, 0 = now

	// Results, written by the worker only.
	bigtime_t		start;
	bigtime_t		end;
	uint64			sink;
	uint32			migrations;
	uint32			samples;
	uint64			cpuVisited;		// bitmask over CPUs 0..63
	int32			lastCpu;
};


static volatile uint64 sGlobalSink = 0;
static volatile int32 sGo = 0;


static int32
worker(void* data)
{
	worker_arg* arg = (worker_arg*)data;

	// Wait for the release without burning a CPU. The original gate was
	// `while (atomic_get(&sGo) == 0) ;`, a busy-wait, which manufactures N
	// extra runnable threads during the very spawn burst whose placement is
	// under investigation -- it perturbs the thing being measured. An absolute
	// deadline released by snooze_until() lets every worker sit blocked and wake
	// on a common instant instead, so thread-creation cost still lands outside
	// the timed region without inventing load.
	if (arg->releaseTime > 0)
		snooze_until(arg->releaseTime, B_SYSTEM_TIMEBASE);
	while (atomic_get((int32*)&sGo) == 0)
		snooze(200);

	arg->start = system_time();
	if (arg->chunkIterations > 0) {
		// Chunked: same total work, but observable. Counting transitions of
		// sched_getcpu() as seen BY THE THREAD ITSELF is authoritative for
		// where it is now; it can only miss migrations faster than the sample
		// interval, so it is a sensitive detector of thrashing (the thing we
		// need to rule out) even though it is a lower bound on the true count.
		uint64 remaining = arg->iterations;
		uint64 x = arg->seed;
		while (remaining > 0) {
			uint64 chunk = remaining < arg->chunkIterations
				? remaining : arg->chunkIterations;
			x = spin_chain(chunk, x);
			remaining -= chunk;

			int32 cpu = sched_getcpu();
			if (cpu >= 0) {
				arg->samples++;
				if (cpu < 64)
					arg->cpuVisited |= 1ULL << cpu;
				if (arg->lastCpu >= 0 && cpu != arg->lastCpu)
					arg->migrations++;
				arg->lastCpu = cpu;
			}

			// A sleeper blocks, so it goes through GoesAway()/Enqueue() and
			// therefore through the scheduler's rebalance path, which is
			// exactly what a mixed workload needs to exercise.
			if (arg->snoozeMicros > 0)
				snooze(arg->snoozeMicros);
		}
		arg->sink = x;
	} else if (arg->stream)
		arg->sink = spin_stream(arg->iterations, arg->buffer, arg->words);
	else
		arg->sink = spin_chain(arg->iterations, arg->seed);
	arg->end = system_time();

	sGlobalSink += arg->sink;
	return 0;
}


struct run_result {
	uint32		threads;
	bigtime_t	wall;			// slowest thread's own span
	bigtime_t	spanWall;		// parent-observed span, includes join
	bigtime_t	minThread;
	bigtime_t	maxThread;
	bigtime_t	cpuBusy;		// summed active_time delta
	uint32		cpusUsed;		// CPUs whose active_time moved >10% of wall
	uint32		cpuCount;
	bigtime_t	perCpu[MAX_CPUS];

	// Chunked mode only.
	uint32		migrations;		// summed over threads
	uint32		maxMigrations;	// worst single thread
	uint32		samples;
	uint32		distinctCpus;	// CPUs any thread was ever observed on
	// Mixed mode only: the CPU-bound half, measured apart from the sleepers,
	// because averaging the two classes together would hide a straggler.
	bigtime_t	busyMax;
	bigtime_t	busyMin;
};


static bool
run_ladder_point(uint32 threads, uint64 iterations, bool stream,
	uint64 chunkIterations, bigtime_t sleeperSnooze, bigtime_t staggerMicros,
	run_result& out)
{
	static worker_arg args[MAX_THREADS];
	static thread_id ids[MAX_THREADS];
	static volatile uint64* buffers[MAX_THREADS];

	memset(&out, 0, sizeof(out));
	out.threads = threads;

	size_t words = STREAM_BYTES / sizeof(uint64);

	for (uint32 i = 0; i < threads; i++) {
		args[i].iterations = iterations;
		args[i].seed = 0x9e3779b97f4a7c15ULL + i;
		args[i].stream = stream;
		args[i].words = words;
		args[i].buffer = NULL;
		args[i].chunkIterations = chunkIterations;
		args[i].migrations = 0;
		args[i].samples = 0;
		args[i].cpuVisited = 0;
		args[i].lastCpu = -1;
		args[i].snoozeMicros = 0;
		// Mixed workload: odd threads are sleepers on a ~20 % duty cycle and do
		// a sixteenth of the work, so they finish on a comparable timescale
		// while generating a lot of wake/sleep traffic through the rebalance
		// path. Even threads stay purely CPU-bound.
		if (sleeperSnooze > 0 && (i % 2) == 1) {
			args[i].snoozeMicros = sleeperSnooze;
			args[i].iterations = iterations / 16;
		}
		if (stream) {
			if (buffers[i] == NULL) {
				buffers[i] = (volatile uint64*)malloc(STREAM_BYTES);
				if (buffers[i] == NULL) {
					fprintf(stderr, "smpscale: out of memory for stream "
						"buffer %" B_PRIu32 "\n", i);
					return false;
				}
				// Touch every page so the timed region has no page faults.
				for (size_t w = 0; w < words; w++)
					buffers[i][w] = w;
			}
			args[i].buffer = buffers[i];
		}
	}

	sGo = 0;

	// Every worker wakes at the same absolute instant, far enough ahead that
	// even a staggered spawn has finished by then.
	bigtime_t releaseTime = system_time() + 200000
		+ (bigtime_t)threads * staggerMicros;
	for (uint32 i = 0; i < threads; i++)
		args[i].releaseTime = releaseTime;

	for (uint32 i = 0; i < threads; i++) {
		char name[32];
		snprintf(name, sizeof(name), "smpscale%" B_PRIu32, i);
		// B_NORMAL_PRIORITY: we want the ordinary scheduler behaviour, not a
		// real-time priority that might mask a scheduler that refuses to
		// migrate work off CPU 0.
		ids[i] = spawn_thread(worker, name, B_NORMAL_PRIORITY, &args[i]);
		if (ids[i] < B_OK) {
			fprintf(stderr, "smpscale: spawn_thread %" B_PRIu32 " failed: %s\n",
				i, strerror(ids[i]));
			for (uint32 j = 0; j < i; j++)
				kill_thread(ids[j]);
			return false;
		}
		resume_thread(ids[i]);
		// Spawn stagger. The whole imbalance vanishes once spawns are separated
		// by about a millisecond, which is kLoadMeasureInterval -- so making the
		// stagger a controllable variable turns that observation into a dial
		// this tool can sweep.
		if (staggerMicros > 0)
			snooze(staggerMicros);
	}

	// Flip the gate BEFORE the release instant, so that when the workers wake
	// they see it already set and never enter the polling loop. Polling would
	// have them sleeping and waking repeatedly right before the measurement,
	// which lowers their fNeededLoad -- the exact quantity placement keys on.
	snooze_until(releaseTime - 20000, B_SYSTEM_TIMEBASE);
	atomic_set((int32*)&sGo, 1);

	// Now start the clock at the instant the workers actually wake.
	snooze_until(releaseTime, B_SYSTEM_TIMEBASE);

	cpu_snapshot before;
	take_cpu_snapshot(before);
	bigtime_t spanStart = system_time();

	for (uint32 i = 0; i < threads; i++) {
		status_t exit;
		wait_for_thread(ids[i], &exit);
	}

	bigtime_t spanEnd = system_time();
	cpu_snapshot after;
	take_cpu_snapshot(after);

	out.spanWall = spanEnd - spanStart;
	out.minThread = args[0].end - args[0].start;
	out.maxThread = out.minThread;
	for (uint32 i = 1; i < threads; i++) {
		bigtime_t span = args[i].end - args[i].start;
		if (span < out.minThread)
			out.minThread = span;
		if (span > out.maxThread)
			out.maxThread = span;
	}
	out.wall = out.maxThread;

	uint64 visited = 0;
	out.busyMin = -1;
	for (uint32 i = 0; i < threads; i++) {
		out.migrations += args[i].migrations;
		out.samples += args[i].samples;
		if (args[i].migrations > out.maxMigrations)
			out.maxMigrations = args[i].migrations;
		visited |= args[i].cpuVisited;

		if (args[i].snoozeMicros == 0) {
			bigtime_t span = args[i].end - args[i].start;
			if (span > out.busyMax)
				out.busyMax = span;
			if (out.busyMin < 0 || span < out.busyMin)
				out.busyMin = span;
		}
	}
	for (uint32 c = 0; c < 64; c++) {
		if ((visited & (1ULL << c)) != 0)
			out.distinctCpus++;
	}

	out.cpuCount = before.count;
	if (before.count > 0 && before.count == after.count) {
		for (uint32 i = 0; i < before.count; i++) {
			bigtime_t delta = after.active[i] - before.active[i];
			if (delta < 0)
				delta = 0;
			out.perCpu[i] = delta;
			out.cpuBusy += delta;
			// "Used" means this CPU was busy for at least a tenth of the run.
			// A CPU only servicing interrupts will not clear that bar.
			if (delta > out.spanWall / 10)
				out.cpusUsed++;
		}
	} else {
		out.cpuBusy = -1;
	}

	return true;
}


int
main(int argc, char** argv)
{
	bigtime_t targetMicros = 3000000;
	bigtime_t warmupMicros = 200000;
	bool stream = false;
	bool countMigrations = false;
	bool mixed = false;
	bigtime_t staggerMicros = 0;
	bool dumpPlacement = false;
	uint32 ladder[MAX_LADDER];
	uint32 ladderSize = 0;

	int i = 1;
	for (; i < argc; i++) {
		if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
			targetMicros = (bigtime_t)atoll(argv[++i]) * 1000;
		else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
			warmupMicros = (bigtime_t)atoll(argv[++i]) * 1000;
		else if (strcmp(argv[i], "-m") == 0)
			stream = true;
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			staggerMicros = (bigtime_t)atoll(argv[++i]);
		else if (strcmp(argv[i], "-p") == 0)
			dumpPlacement = true;
		else if (strcmp(argv[i], "-g") == 0)
			countMigrations = true;
		else if (strcmp(argv[i], "-x") == 0) {
			mixed = true;
			countMigrations = true;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "smpscale: unknown option %s\n", argv[i]);
			return 1;
		} else
			break;
	}
	for (; i < argc && ladderSize < MAX_LADDER; i++) {
		int n = atoi(argv[i]);
		if (n > 0 && n <= MAX_THREADS)
			ladder[ladderSize++] = (uint32)n;
	}
	if (ladderSize == 0) {
		static const uint32 kDefault[] = { 1, 2, 4, 8, 16 };
		for (uint32 j = 0; j < 5; j++)
			ladder[ladderSize++] = kDefault[j];
	}

	system_info systemInfo;
	if (get_system_info(&systemInfo) != B_OK) {
		fprintf(stderr, "smpscale: get_system_info failed\n");
		return 1;
	}

	printf("smpscale: get_system_info reports cpu_count = %" B_PRIu32 "\n",
		systemInfo.cpu_count);
	printf("smpscale: work = %s\n", stream
		? "memory streaming (16 MiB/thread, DRAM-bound)"
		: "serial integer dependency chain (register-bound)");

	// Calibrate so one thread's run is long enough that timing error is
	// irrelevant. This is the whole point: a 3 s measurement with a 1 us clock
	// has 0.00003 % quantisation error, against the ~50 % a `date +%s`
	// difference over 2 s carries.
	uint64 iterations;
	if (stream) {
		iterations = 1;
		bigtime_t elapsed = 0;
		while (elapsed < targetMicros / 4 && iterations < (1 << 20)) {
			run_result probe;
			if (!run_ladder_point(1, iterations, true, 0, 0, 0, probe))
				return 1;
			elapsed = probe.wall;
			if (elapsed >= targetMicros / 4)
				break;
			iterations *= 2;
		}
		if (elapsed > 0) {
			uint64 scaled = (uint64)((double)iterations
				* (double)targetMicros / (double)elapsed);
			if (scaled > 0)
				iterations = scaled;
		}
	} else {
		iterations = 1000000;
		while (true) {
			bigtime_t start = system_time();
			sGlobalSink += spin_chain(iterations, 12345);
			bigtime_t elapsed = system_time() - start;
			if (elapsed > 200000) {
				iterations = (uint64)((double)iterations
					* (double)targetMicros / (double)elapsed);
				break;
			}
			iterations *= 4;
			if (iterations > (1ULL << 40)) {
				fprintf(stderr, "smpscale: calibration ran away\n");
				return 1;
			}
		}
	}

	printf("smpscale: calibrated to %llu iterations per thread"
		" (~%lld ms for one thread)\n",
		(unsigned long long)iterations, (long long)(targetMicros / 1000));

	if (warmupMicros > 0) {
		bigtime_t start = system_time();
		while (system_time() - start < warmupMicros)
			sGlobalSink += spin_chain(100000, 999);
	}

	// One chunk per millisecond of single-thread work: a 1 kHz migration
	// sampler. Only used in -g/-x mode.
	uint64 chunkIterations = 0;
	if (countMigrations) {
		chunkIterations = iterations * 1000 / (uint64)targetMicros;
		if (chunkIterations == 0)
			chunkIterations = 1;
		printf("smpscale: migration sampling on, %llu iterations per chunk"
			" (~1 ms, so ~1 kHz per thread)\n",
			(unsigned long long)chunkIterations);
		printf("smpscale: NOTE this mode adds a syscall between chunks, so its"
			" times are NOT comparable to the plain ladder\n");
	}
	if (mixed) {
		printf("smpscale: mixed workload, odd threads sleep 4 ms per 1 ms of"
			" work and do 1/16 the work\n");
	}

	printf("\n");
	printf("%8s %10s %10s %10s %8s %10s %8s %9s\n",
		"threads", "wall_ms", "min_ms", "max_ms", "speedup", "cpu_busy_ms",
		"cpus>10%", "eff");
	printf("-------- ---------- ---------- ---------- -------- ----------"
		" -------- ---------\n");

	run_result results[MAX_LADDER];
	double baseWall = 0.0;

	for (uint32 k = 0; k < ladderSize; k++) {
		if (!run_ladder_point(ladder[k], iterations, stream, chunkIterations,
				mixed ? 4000 : 0, staggerMicros, results[k])) {
			return 1;
		}

		run_result& r = results[k];
		double wallMs = (double)r.wall / 1000.0;
		if (k == 0)
			baseWall = wallMs;

		// Fixed work per thread, so aggregate throughput speedup is
		// N * (base wall / this wall).
		double speedup = baseWall > 0.0
			? (double)r.threads * baseWall / wallMs : 0.0;
		double eff = r.threads > 0 ? speedup / (double)r.threads : 0.0;

		printf("%8" B_PRIu32 " %10.1f %10.1f %10.1f %8.2f %10.1f %8" B_PRIu32
			" %9.3f\n",
			r.threads, wallMs, (double)r.minThread / 1000.0,
			(double)r.maxThread / 1000.0, speedup,
			(double)r.cpuBusy / 1000.0, r.cpusUsed, eff);
		fflush(stdout);
	}

	// The per-CPU distribution is the independent second witness. If wall-clock
	// scaling says 16-way parallel but only CPU 0's active_time moved, then
	// active_time accounting is broken -- which is itself worth knowing, and is
	// exactly the kind of thing a single aggregate number hides.
	printf("\nPer-CPU active_time delta, milliseconds, one row per ladder"
		" point:\n");
	printf("%8s", "threads");
	uint32 cpuCount = results[0].cpuCount;
	if (cpuCount > 32)
		cpuCount = 32;
	for (uint32 c = 0; c < cpuCount; c++)
		printf(" %6" B_PRIu32, c);
	printf("\n");
	for (uint32 k = 0; k < ladderSize; k++) {
		printf("%8" B_PRIu32, results[k].threads);
		for (uint32 c = 0; c < cpuCount; c++)
			printf(" %6.0f", (double)results[k].perCpu[c] / 1000.0);
		printf("\n");
	}

	// Migrations are COUNTED, not inferred from throughput. A fix that reaches
	// efficiency 1.000 by migrating threads thousands of times per second has
	// traded one defect for another, and only a count can tell the difference.
	if (countMigrations) {
		printf("\nMigrations, counted by each thread sampling sched_getcpu()"
			" between work chunks:\n");
		printf("%8s %10s %10s %10s %10s %8s", "threads", "migr_tot",
			"migr_max", "samples", "migr/1ks", "cpus");
		if (mixed)
			printf(" %10s %10s", "busy_min", "busy_max");
		printf("\n");
		for (uint32 k = 0; k < ladderSize; k++) {
			run_result& r = results[k];
			// Per thousand samples, i.e. roughly per second per thread, so the
			// number does not silently scale with run length or thread count.
			double rate = r.samples > 0
				? 1000.0 * (double)r.migrations / (double)r.samples : 0.0;
			printf("%8" B_PRIu32 " %10" B_PRIu32 " %10" B_PRIu32 " %10" B_PRIu32
				" %10.2f %8" B_PRIu32,
				r.threads, r.migrations, r.maxMigrations, r.samples, rate,
				r.distinctCpus);
			if (mixed) {
				printf(" %10.1f %10.1f", (double)r.busyMin / 1000.0,
					(double)r.busyMax / 1000.0);
			}
			printf("\n");
		}
	}

	// The honest summary, and the one the acceptance criteria are stated in:
	// how many CPUs actually did work, how many sat idle, and how lopsided the
	// busiest was against the least busy of the working ones. Unlike `eff` this
	// does not saturate -- it separates "one core doubled" from "five cores
	// doubled and nine CPUs idle", which is the distinction the ladder hid.
	printf("\nPer-CPU busy set. unit_ms is the single-thread work unit; a CPU is"
		"\n'busy' at >=90%% of one unit and 'idle' at <10%%. max/min is over the"
		"\nbusy CPUs only, so 1.00 is perfect and 2.00 means a doubled core.\n");
	printf("%8s %8s %6s %6s %6s %9s %9s\n", "threads", "unit_ms", "busy",
		"idle", "part", "max/min", "idle_cpus");
	for (uint32 k = 0; k < ladderSize; k++) {
		run_result& r = results[k];
		double unit = baseWall;
		uint32 busy = 0;
		uint32 idle = 0;
		uint32 partial = 0;
		double busyMaxMs = 0.0;
		double busyMinMs = 0.0;
		for (uint32 c = 0; c < r.cpuCount; c++) {
			double ms = (double)r.perCpu[c] / 1000.0;
			if (ms < unit * 0.10) {
				idle++;
				continue;
			}
			if (ms < unit * 0.90) {
				partial++;
				continue;
			}
			busy++;
			if (busyMinMs == 0.0 || ms < busyMinMs)
				busyMinMs = ms;
			if (ms > busyMaxMs)
				busyMaxMs = ms;
		}
		printf("%8" B_PRIu32 " %8.1f %6" B_PRIu32 " %6" B_PRIu32 " %6" B_PRIu32
			" %9.3f  ", r.threads, unit, busy, idle, partial,
			busyMinMs > 0.0 ? busyMaxMs / busyMinMs : 0.0);
		// Name the idle CPUs: a fix must not leave any named while work queues.
		for (uint32 c = 0; c < r.cpuCount; c++) {
			if ((double)r.perCpu[c] / 1000.0 < unit * 0.10)
				printf("%" B_PRIu32 " ", c);
		}
		printf("\n");
	}

	printf("\nsink %llu (printed so the work cannot be optimised away)\n",
		(unsigned long long)sGlobalSink);

	if (dumpPlacement) {
		status_t status = _kern_set_scheduler_mode(SMPSCALE_DUMP_PLACEMENT);
		printf("placement trace dump requested: %s (a stock kernel refuses;"
			" look in the syslog / serial console for 'sched_placement:')\n",
			strerror(status));
	}
	return 0;
}
