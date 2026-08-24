/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * disktput -- measure disk throughput, IOPS, latency and the CPU cost of each
 * block, on the raw device and through the file system.
 *
 * Nothing in this tree has ever measured storage. That is not a small omission:
 * the worst bug found in this project so far was on this path -- the page writer
 * never flushed file data at all, because BinarySemaphore::Wait() returns false
 * on timeout and the timeout branch skipped the periodic flush. File writes
 * stayed in memory indefinitely. It was found by accident, when a stop/start
 * lost an ssh host key that had the right size, mode and mtime and 411 bytes of
 * zeros in it. A benchmark would not have caught that one, but the absence of
 * any benchmark is why nobody was looking here.
 *
 * The design follows src/bin/nettput, for the same reason it was written that
 * way: a stock image has no fio, no bonnie, no dd worth trusting and no
 * compiler, so without this there is no way to ask whether a change helped.
 *
 *   disktput -f <path> [-m mode] [-b bytes] [-n bytes] [-t threads] ...
 *
 * Four things it reports that a naive rate-only benchmark would not, each
 * because a storage number is easy to get wrong in a specific way:
 *
 *  - CPU microseconds per mebibyte, from cpu_info::active_time summed over every
 *    CPU, exactly as nettput does it. Block I/O completes in interrupt context
 *    and is reaped by kernel threads; a per-thread measurement sees almost none
 *    of the real cost. This is also the number that tells a copy problem from a
 *    device problem: if MiB/s is flat but CPU per MiB falls, the win was in the
 *    software path.
 *
 *  - Concurrency as an explicit variable, because the NVMe driver picks its
 *    submission queue with smp_get_current_cpu() % qpair_count. One thread
 *    issuing blocking preads therefore exercises exactly one queue and one core,
 *    and can never show whether the other queues work. Threads here *are* the
 *    queue depth: one blocking pread per thread is one request outstanding, so
 *    -t is the honest way to ask for depth without aio.
 *
 *  - Latency percentiles, not just a mean. A device that is fast on average and
 *    occasionally stalls for a second looks identical to a uniformly fast one in
 *    a MiB/s figure, and completely different to anything running on top of it.
 *
 *  - Whether the cache was in the way, and what it cost. -D opens with
 *    O_NOCACHE, which BFS honours by calling file_cache_disable() on the inode
 *    for as long as the descriptor is open. Without it, a "read" of a file just
 *    written measures memcpy from the page cache and nothing else -- which is
 *    the single easiest way to publish a storage number that is off by an order
 *    of magnitude in the flattering direction. Runs print the cache state they
 *    ran under so a number can never be quoted without it.
 *
 * Writes to a raw device destroy whatever is on it, so a write mode against a
 * character/block device refuses to run without -y. The root disk of the machine
 * doing the measuring is a very easy thing to point this at by accident.
 *
 * The tool deliberately does one run and prints one result. Repetitions,
 * medians and A/B sweeps belong in the harness (graviton/scripts/disktput-run),
 * the same split nettput and nettput-run use: a single run on shared EBS tells
 * you very little, and the tool should not be the thing deciding how many
 * samples is enough.
 */


#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>

#include <Drivers.h>
#include <OS.h>
#include <SupportDefs.h>


#define DISKTPUT_BUILD		__DATE__ " " __TIME__

#define DEFAULT_BLOCK		(64 * 1024)
#define DEFAULT_BYTES		(512 * 1024 * 1024LL)
#define DEFAULT_THREADS		1

#define MAX_CPUS			64
#define MAX_THREADS			256

// Latency samples are kept for percentiles, in a ring so that a run of any
// length is bounded and the percentiles describe its steady state rather than
// its ramp-up. A burst of slow operations once write-back stops keeping up is
// precisely the signature worth seeing, and it appears at the end of a run.
#define MAX_SAMPLES			(64 * 1024)


enum io_mode {
	MODE_SEQ_READ,
	MODE_SEQ_WRITE,
	MODE_RAND_READ,
	MODE_RAND_WRITE
};


static const char*
mode_name(io_mode mode)
{
	switch (mode) {
		case MODE_SEQ_READ:		return "seqread";
		case MODE_SEQ_WRITE:	return "seqwrite";
		case MODE_RAND_READ:	return "randread";
		case MODE_RAND_WRITE:	return "randwrite";
	}
	return "?";
}


static bool
mode_is_write(io_mode mode)
{
	return mode == MODE_SEQ_WRITE || mode == MODE_RAND_WRITE;
}


static bool
mode_is_random(io_mode mode)
{
	return mode == MODE_RAND_READ || mode == MODE_RAND_WRITE;
}


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


// Machine-wide busy time, not this process's. Block I/O is completed in
// interrupt handlers and drained by kernel threads, so a per-thread figure would
// attribute almost none of the real cost to the work that caused it.
static bigtime_t
cpu_busy_between(const cpu_snapshot& before, const cpu_snapshot& after)
{
	if (before.count == 0 || before.count != after.count)
		return -1;

	bigtime_t busy = 0;
	for (uint32 i = 0; i < before.count; i++) {
		bigtime_t delta = after.active[i] - before.active[i];
		if (delta > 0)
			busy += delta;
	}

	return busy;
}


static off_t
parse_size(const char* text)
{
	char* end = NULL;
	double value = strtod(text, &end);

	if (end == text || value <= 0)
		return -1;

	switch (*end) {
		case 'g': case 'G':
			value *= 1024.0 * 1024.0 * 1024.0;
			end++;
			break;
		case 'm': case 'M':
			value *= 1024.0 * 1024.0;
			end++;
			break;
		case 'k': case 'K':
			value *= 1024.0;
			end++;
			break;
		case '\0':
			break;
		default:
			return -1;
	}

	if (*end != '\0')
		return -1;

	return (off_t)value;
}


// Deterministic per-thread stream, so a random run is repeatable and two
// interleaved A/B runs touch the same blocks in the same order. An A/B where the
// two arms read different offsets of a lazily-loaded EBS volume is not an A/B.
static inline uint64
next_random(uint64& state)
{
	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	return state;
}


// ---------------------------------------------------------------- the barrier

// Hand-rolled rather than pthread_barrier_t so that the start of the measured
// window is unambiguous: every thread has its file descriptor open and its
// buffer touched before any of them issues an operation. Otherwise the first
// thread's I/O overlaps the last thread's setup and short runs measure
// thread creation.
struct barrier {
	pthread_mutex_t	lock;
	pthread_cond_t	condition;
	int				waiting;
	int				total;
	bool			released;
};


static void
barrier_init(barrier& gate, int total)
{
	pthread_mutex_init(&gate.lock, NULL);
	pthread_cond_init(&gate.condition, NULL);
	gate.waiting = 0;
	gate.total = total;
	gate.released = false;
}


static void
barrier_wait(barrier& gate)
{
	pthread_mutex_lock(&gate.lock);
	gate.waiting++;
	if (gate.waiting >= gate.total) {
		gate.released = true;
		pthread_cond_broadcast(&gate.condition);
	} else {
		while (!gate.released)
			pthread_cond_wait(&gate.condition, &gate.lock);
	}
	pthread_mutex_unlock(&gate.lock);
}


// ----------------------------------------------------------------- the worker

struct run_config {
	const char*	path;
	io_mode		mode;
	off_t		blockSize;
	off_t		spanStart;
	off_t		span;
	int			threads;
	bool		noCache;
	bool		syncEveryOp;
	uint64		seed;
	off_t		alignment;
	off_t		misalign;
	bigtime_t	duration;		// 0 = move -n bytes instead of running for a time
};


// The buffer's physical alignment decides which of two completely different
// code paths the measurement lands in, so it is a parameter here rather than
// whatever malloc happened to return.
//
// nvme_disk checks every vec of a request: middle vecs must be page-aligned in
// address and length, and if the check fails the request goes down the bounce
// path instead. That path is capped by kMaxBounceBufferSize in
// dma_resources.cpp, which is 4 * B_PAGE_SIZE == 16384 -- so a misaligned 1 MiB
// request is not one command, it is 64 sequential 16 KiB commands, each with its
// own completion wait, and bounced writes additionally take rounded_write_lock
// exclusively and serialise against every other write on the device.
//
// malloc() gives no page-alignment guarantee, so a benchmark that simply
// malloc()s its buffer measures whichever path it happened to land in that run.
// Default here is page-aligned; -U deliberately breaks it, which turns the
// cliff into something measurable instead of something to be careful about.
static uint8*
allocate_io_buffer(off_t size, off_t alignment, off_t misalign, uint8** _base)
{
	void* base = NULL;
	size_t total = (size_t)(size + alignment + misalign);

	if (posix_memalign(&base, (size_t)alignment, total) != 0 || base == NULL) {
		// Not fatal on its own, but the run can no longer claim to know which
		// path it is measuring, so say so rather than quietly continuing.
		fprintf(stderr, "disktput: posix_memalign(%" B_PRIdOFF ", %zu) failed;"
			" falling back to malloc and the alignment below is not"
			" guaranteed\n", alignment, total);
		base = malloc(total);
		if (base == NULL)
			return NULL;
	}

	*_base = (uint8*)base;
	return (uint8*)base + misalign;
}


struct worker {
	pthread_t		thread;
	const run_config* config;
	barrier*		gate;

	int				index;
	int				fd;
	uint8*			buffer;
	uint8*			bufferBase;		// what to free; buffer may be offset from it

	off_t			regionStart;	// this thread's slice of the span
	off_t			regionSize;
	off_t			operations;		// operations this thread should perform

	// Results.
	off_t			bytesMoved;
	off_t			operationsDone;
	bigtime_t		elapsed;
	bigtime_t		latencyTotal;
	bigtime_t		latencyMax;
	bigtime_t*		samples;
	uint32			sampleCount;
	off_t			samplesRecorded;
	status_t		error;
	off_t			errorOffset;
};


static void*
worker_main(void* data)
{
	worker* self = (worker*)data;
	const run_config& config = *self->config;

	// Touch the whole buffer before the barrier: a first-touch page fault inside
	// the measured window is charged to the device.
	memset(self->buffer, 0xa5 ^ self->index, config.blockSize);
	if (mode_is_write(config.mode)) {
		// Non-constant payload so that nothing in the path -- or in EBS -- can
		// take a shortcut on a run of identical bytes.
		for (off_t i = 0; i < config.blockSize; i++)
			self->buffer[i] = (uint8)((i + self->index * 31) & 0xff);
	}

	uint64 random = config.seed + (uint64)self->index * 0x9e3779b97f4a7c15ULL;
	if (random == 0)
		random = 1;

	off_t blocksInRegion = self->regionSize / config.blockSize;
	if (blocksInRegion <= 0)
		blocksInRegion = 1;

	barrier_wait(*self->gate);

	bigtime_t start = system_time();

	// A fixed byte count makes every cell of a concurrency sweep a different
	// duration: at 16 threads the same -n finishes in a third of a second, and
	// EBS rate limiting is a token bucket that a third of a second does not
	// come close to draining. A sweep measured that way shows throughput rising
	// linearly past the instance's own documented ceiling, which is not a
	// result, it is a burst. -T holds every cell to the same wall time so the
	// limiter binds equally in all of them.
	bigtime_t deadline = self->config->duration > 0
		? start + self->config->duration : 0;

	for (off_t op = 0; op < self->operations; op++) {
		off_t offset;
		if (mode_is_random(config.mode)) {
			offset = self->regionStart
				+ (off_t)(next_random(random) % (uint64)blocksInRegion)
					* config.blockSize;
		} else {
			offset = self->regionStart
				+ (op % blocksInRegion) * config.blockSize;
		}

		bigtime_t opStart = system_time();
		if (deadline != 0 && opStart >= deadline)
			break;

		ssize_t moved;
		if (mode_is_write(config.mode)) {
			moved = pwrite(self->fd, self->buffer, config.blockSize, offset);
			if (moved > 0 && config.syncEveryOp) {
				if (fsync(self->fd) != 0) {
					self->error = errno;
					self->errorOffset = offset;
					break;
				}
			}
		} else {
			moved = pread(self->fd, self->buffer, config.blockSize, offset);
		}

		bigtime_t latency = system_time() - opStart;

		if (moved < 0) {
			self->error = errno;
			self->errorOffset = offset;
			break;
		}
		if (moved == 0) {
			// A short read at the end of the region is the region being smaller
			// than advertised, which is a configuration error worth surfacing
			// rather than a quietly shorter run.
			self->error = EIO;
			self->errorOffset = offset;
			break;
		}

		self->bytesMoved += moved;
		self->operationsDone++;
		self->latencyTotal += latency;
		if (latency > self->latencyMax)
			self->latencyMax = latency;

		// A ring, so the percentiles describe the most recent MAX_SAMPLES
		// operations however long the run turned out to be. Keeping only the
		// first MAX_SAMPLES would describe the ramp-up instead of the steady
		// state, which for a write run is the flattering half.
		if (self->samples != NULL) {
			self->samples[self->samplesRecorded % MAX_SAMPLES] = latency;
			self->samplesRecorded++;
			self->sampleCount = self->samplesRecorded < MAX_SAMPLES
				? (uint32)self->samplesRecorded : MAX_SAMPLES;
		}
	}

	self->elapsed = system_time() - start;
	return NULL;
}


// ------------------------------------------------------------------ reporting

static int
compare_latency(const void* a, const void* b)
{
	bigtime_t left = *(const bigtime_t*)a;
	bigtime_t right = *(const bigtime_t*)b;
	if (left < right)
		return -1;
	return left > right ? 1 : 0;
}


static bigtime_t
percentile(bigtime_t* sorted, uint32 count, double fraction)
{
	if (count == 0)
		return -1;
	uint32 index = (uint32)(fraction * (double)(count - 1) + 0.5);
	if (index >= count)
		index = count - 1;
	return sorted[index];
}


// Printed with every result rather than left to the reader, because a raw-device
// number and a through-BFS number are different measurements and have been
// confused before. bytes_per_physical_sector is worth stating too: the driver
// reports a 512-byte logical block, and whether the device underneath prefers
// 4096 changes what an unaligned block size costs.
static void
report_target(const char* path, int fd, bool noCache, off_t size)
{
	printf("  target          : %s\n", path);

	struct stat info;
	const char* kind = "unknown";
	bool raw = false;
	if (fstat(fd, &info) == 0) {
		raw = S_ISCHR(info.st_mode) || S_ISBLK(info.st_mode);
		if (S_ISCHR(info.st_mode))
			kind = "character device (raw)";
		else if (S_ISBLK(info.st_mode))
			kind = "block device (raw)";
		else if (S_ISREG(info.st_mode))
			kind = "regular file (through the file system)";
	}
	printf("  target kind     : %s\n", kind);
	printf("  usable size     : %" B_PRIdOFF " bytes (%.2f GiB)\n", size,
		(double)size / (1024.0 * 1024.0 * 1024.0));

	device_geometry geometry;
	if (ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) == 0) {
		printf("  logical sector  : %" B_PRIu32 " bytes\n",
			geometry.bytes_per_sector);
		printf("  physical sector : %" B_PRIu32 " bytes\n",
			geometry.bytes_per_physical_sector);
		printf("  read only       : %s\n", geometry.read_only ? "yes" : "no");
	}

	// Saying "cache in use" for a raw device would be wrong and would make a
	// perfectly good device number look untrustworthy: devfs installs no file
	// cache for a raw disk device, so these reads and writes reach the driver
	// whether O_NOCACHE was asked for or not.
	if (raw) {
		printf("  file cache      : n/a -- devfs does not cache a raw device,"
			" so this is the driver path\n");
	} else {
		printf("  file cache      : %s\n", noCache
			? "bypassed (O_NOCACHE accepted, file_cache_disable)"
			: "IN USE -- results include the page cache");
	}
}


static void
report(const run_config& config, const char* label, off_t bytes, off_t operations,
	bigtime_t elapsed, bigtime_t cpuBusy, uint32 cpuCount,
	bigtime_t* samples, uint32 sampleCount, bigtime_t latencyTotal,
	bigtime_t latencyMax, bigtime_t syncTime, bool machineReadable)
{
	if (elapsed <= 0) {
		fprintf(stderr, "disktput: implausible elapsed time %" B_PRIdBIGTIME
			" us\n", elapsed);
		return;
	}

	double seconds = (double)elapsed / 1000000.0;
	double mebibytes = (double)bytes / (1024.0 * 1024.0);
	double rate = mebibytes / seconds;
	double iops = (double)operations / seconds;
	double costPerMiB = mebibytes > 0 && cpuBusy >= 0
		? (double)cpuBusy / mebibytes : -1.0;

	qsort(samples, sampleCount, sizeof(bigtime_t), compare_latency);
	bigtime_t p50 = percentile(samples, sampleCount, 0.50);
	bigtime_t p99 = percentile(samples, sampleCount, 0.99);
	double meanLatency = operations > 0
		? (double)latencyTotal / (double)operations : 0.0;

	printf("\n=== disktput result%s%s ===\n", label != NULL ? ": " : "",
		label != NULL ? label : "");
	printf("  build           : %s\n", DISKTPUT_BUILD);
	printf("  mode            : %s\n", mode_name(config.mode));
	printf("  block size      : %" B_PRIdOFF " bytes\n", config.blockSize);
	printf("  concurrency     : %d thread(s) = %d request(s) outstanding\n",
		config.threads, config.threads);
	printf("  buffer align    : %" B_PRIdOFF " bytes%s\n", config.alignment,
		config.misalign != 0 ? " (deliberately offset -- BOUNCE PATH)" : "");
	printf("  span            : %" B_PRIdOFF " bytes (%.2f GiB) from offset %"
		B_PRIdOFF "\n", config.span,
		(double)config.span / (1024.0 * 1024.0 * 1024.0), config.spanStart);
	printf("  transferred     : %" B_PRIdOFF " bytes (%.1f MiB) in %" B_PRIdOFF
		" operations\n", bytes, mebibytes, operations);
	printf("  elapsed         : %.3f s%s\n", seconds,
		config.duration > 0 ? " (time-based, so the EBS limiter binds)" : "");
	printf("  throughput      : %.1f MiB/s (%.1f Mbit/s)\n", rate,
		((double)bytes * 8.0 / 1000000.0) / seconds);
	printf("  iops            : %.0f\n", iops);
	printf("  latency         : mean %.0f us, p50 %" B_PRIdBIGTIME
		" us, p99 %" B_PRIdBIGTIME " us, max %" B_PRIdBIGTIME " us"
		" (%" B_PRIu32 " samples)\n", meanLatency, p50, p99, latencyMax,
		sampleCount);

	if (syncTime >= 0) {
		printf("  final fsync     : %.3f s (%.1f%% of the run)\n",
			(double)syncTime / 1000000.0,
			100.0 * (double)syncTime / (double)elapsed);
	}

	if (cpuBusy < 0) {
		printf("  cpu cost        : unavailable (cpu_info unreadable)\n");
	} else {
		double cpuSeconds = (double)cpuBusy / 1000000.0;
		printf("  cpu busy        : %.3f s across %" B_PRIu32 " cpu(s)"
			" (%.1f%% of the machine)\n", cpuSeconds, cpuCount,
			100.0 * cpuSeconds / (seconds * (double)cpuCount));
		printf("  cost per MiB    : %.0f us of cpu\n", costPerMiB);
		printf("  cost per op     : %.1f us of cpu\n", operations > 0
			? (double)cpuBusy / (double)operations : 0.0);
	}

	// One line the harness can parse without caring about the prose above.
	if (machineReadable) {
		printf("DISKTPUT\t%s\t%" B_PRIdOFF "\t%d\t%s\t%.2f\t%.0f\t%.0f\t%"
			B_PRIdBIGTIME "\t%" B_PRIdBIGTIME "\t%.0f\t%" B_PRIdOFF "\n",
			mode_name(config.mode), config.blockSize, config.threads,
			config.noCache ? "nocache" : "cached", rate, iops, meanLatency,
			p50, p99, costPerMiB, config.misalign);
	}
}


// ------------------------------------------------------------------- the setup

static off_t
target_size(int fd)
{
	struct stat info;
	if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode))
		return info.st_size;

	// Devices do not report a size through stat, and B_GET_DEVICE_SIZE is a
	// size_t that cannot describe a modern disk. Geometry multiplies out.
	device_geometry geometry;
	if (ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) == 0) {
		return (off_t)geometry.bytes_per_sector
			* (off_t)geometry.sectors_per_track
			* (off_t)geometry.cylinder_count
			* (off_t)geometry.head_count;
	}

	size_t deviceSize = 0;
	if (ioctl(fd, B_GET_DEVICE_SIZE, &deviceSize, sizeof(deviceSize)) == 0)
		return (off_t)deviceSize;

	return -1;
}


static bool
is_device(int fd)
{
	struct stat info;
	if (fstat(fd, &info) != 0)
		return false;
	return S_ISCHR(info.st_mode) || S_ISBLK(info.st_mode);
}


// A read benchmark needs something to read. Growing the file with ftruncate
// would produce a sparse file whose "read" never reaches the device at all, so
// the region is written out for real -- and how long that took is printed,
// because it is itself a sequential write measurement and a useful cross-check
// on the one the run is about to make.
static status_t
populate(const char* path, off_t needed, off_t blockSize)
{
	int fd = open(path, O_RDWR | O_CREAT | O_NOCACHE, 0644);
	if (fd < 0) {
		fd = open(path, O_RDWR | O_CREAT, 0644);
		if (fd < 0)
			return errno;
	}

	off_t have = 0;
	struct stat info;
	if (fstat(fd, &info) == 0)
		have = info.st_size;

	if (have >= needed) {
		close(fd);
		return B_OK;
	}

	printf("disktput: %s holds %" B_PRIdOFF " of %" B_PRIdOFF " bytes,"
		" writing the rest so reads reach the device\n", path, have, needed);
	fflush(stdout);

	// Page-aligned like the measured path: a populate pass down the 16 KiB
	// bounce path would take long enough to look like a hung tool.
	uint8* base = NULL;
	uint8* buffer = allocate_io_buffer(blockSize, B_PAGE_SIZE, 0, &base);
	if (buffer == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}
	for (off_t i = 0; i < blockSize; i++)
		buffer[i] = (uint8)(i & 0xff);

	bigtime_t start = system_time();
	off_t at = have - (have % blockSize);
	status_t result = B_OK;
	while (at < needed) {
		ssize_t written = pwrite(fd, buffer, blockSize, at);
		if (written <= 0) {
			result = written < 0 ? errno : EIO;
			break;
		}
		at += written;
	}

	if (result == B_OK && fsync(fd) != 0)
		result = errno;

	bigtime_t elapsed = system_time() - start;
	free(base);
	close(fd);

	if (result == B_OK) {
		double mebibytes = (double)(needed - have) / (1024.0 * 1024.0);
		printf("disktput: populated %.1f MiB in %.3f s (%.1f MiB/s,"
			" including the fsync)\n", mebibytes,
			(double)elapsed / 1000000.0,
			mebibytes / ((double)elapsed / 1000000.0));
		fflush(stdout);
	}

	return result;
}


static int
usage(int status)
{
	FILE* out = status == 0 ? stdout : stderr;
	fprintf(out,
		"Usage: disktput -f <path> [options]\n"
		"\n"
		"Measure disk throughput, IOPS, latency and CPU cost per byte, either\n"
		"on a raw device (/dev/disk/nvme/0/raw) or through the file system.\n"
		"\n"
		"  -f <path>     device or file to measure (required)\n"
		"  -m <mode>     seqread | seqwrite | randread | randwrite\n"
		"                (default seqread)\n"
		"  -b <bytes>    block size per operation, K/M suffixes ok"
			" (default 64K)\n"
		"  -n <bytes>    total bytes to move across all threads (default 512M)\n"
		"  -t <n>        concurrent threads, which is the request depth the\n"
		"                device sees (default 1, max %d)\n"
		"  -s <bytes>    span to operate within (default: -n, or the whole\n"
		"                device). Make this much larger than RAM for random\n"
		"                reads, or the page cache answers instead of the disk\n"
		"  -o <bytes>    offset the span starts at (default 0)\n"
		"  -D            open with O_NOCACHE, bypassing the file cache\n"
		"  -S            fsync once at the end and report its cost separately\n"
		"  -F            fsync after every write (durability, not throughput)\n"
		"  -e <n>        random seed, so a random run is repeatable"
			" (default 1)\n"
		"  -T <secs>     run for this long instead of for -n bytes. Use this for\n"
		"                any concurrency sweep: a fixed -n makes high thread\n"
		"                counts finish too fast for the EBS token bucket to\n"
		"                bind, and throughput then appears to exceed the\n"
		"                instance's own documented ceiling\n"
		"  -A <bytes>    I/O buffer alignment, power of two (default one page).\n"
		"                Below a page, nvme_disk bounces the request through a\n"
		"                16 KiB buffer, so this is not a cosmetic knob\n"
		"  -U <bytes>    deliberately offset the buffer this far past its\n"
		"                alignment, to measure the bounce path on purpose\n"
		"  -y            confirm writing to a raw device, destroying its"
			" contents\n"
		"  -J            also print one tab-separated line for a harness\n"
		"  -L <label>    label echoed into the result\n"
		"  -h            this help\n"
		"\n"
		"Build: %s\n",
		MAX_THREADS, DISKTPUT_BUILD);
	return status;
}


int
main(int argc, char** argv)
{
	run_config config;
	memset(&config, 0, sizeof(config));
	config.mode = MODE_SEQ_READ;
	config.blockSize = DEFAULT_BLOCK;
	config.threads = DEFAULT_THREADS;
	config.seed = 1;
	config.alignment = B_PAGE_SIZE;
	config.misalign = 0;

	off_t totalBytes = DEFAULT_BYTES;
	off_t wantedSpan = 0;
	const char* label = NULL;
	bool finalSync = false;
	bool confirmed = false;
	bool machineReadable = false;

	int option;
	while ((option = getopt(argc, argv, "f:m:b:n:t:s:o:e:A:U:L:T:DSFyJh")) != -1) {
		switch (option) {
			case 'f':
				config.path = optarg;
				break;
			case 'm':
				if (strcmp(optarg, "seqread") == 0)
					config.mode = MODE_SEQ_READ;
				else if (strcmp(optarg, "seqwrite") == 0)
					config.mode = MODE_SEQ_WRITE;
				else if (strcmp(optarg, "randread") == 0)
					config.mode = MODE_RAND_READ;
				else if (strcmp(optarg, "randwrite") == 0)
					config.mode = MODE_RAND_WRITE;
				else {
					fprintf(stderr, "disktput: unknown mode \"%s\"\n", optarg);
					return 1;
				}
				break;
			case 'b':
				config.blockSize = parse_size(optarg);
				if (config.blockSize <= 0
					|| config.blockSize > 64 * 1024 * 1024) {
					fprintf(stderr, "disktput: bad block size \"%s\"\n", optarg);
					return 1;
				}
				break;
			case 'n':
				totalBytes = parse_size(optarg);
				if (totalBytes <= 0) {
					fprintf(stderr, "disktput: bad size \"%s\"\n", optarg);
					return 1;
				}
				break;
			case 't':
				config.threads = atoi(optarg);
				if (config.threads < 1 || config.threads > MAX_THREADS) {
					fprintf(stderr, "disktput: bad thread count \"%s\"\n",
						optarg);
					return 1;
				}
				break;
			case 's':
				wantedSpan = parse_size(optarg);
				if (wantedSpan <= 0) {
					fprintf(stderr, "disktput: bad span \"%s\"\n", optarg);
					return 1;
				}
				break;
			case 'o':
				config.spanStart = parse_size(optarg);
				if (config.spanStart < 0) {
					fprintf(stderr, "disktput: bad offset \"%s\"\n", optarg);
					return 1;
				}
				break;
			case 'e':
				config.seed = (uint64)strtoull(optarg, NULL, 0);
				break;
			case 'T': {
				double seconds = strtod(optarg, NULL);
				if (seconds <= 0 || seconds > 3600) {
					fprintf(stderr, "disktput: bad duration \"%s\"\n", optarg);
					return 1;
				}
				config.duration = (bigtime_t)(seconds * 1000000.0);
				break;
			}
			case 'A':
				config.alignment = parse_size(optarg);
				// posix_memalign requires a power-of-two multiple of sizeof(void*).
				if (config.alignment < (off_t)sizeof(void*)
					|| (config.alignment & (config.alignment - 1)) != 0) {
					fprintf(stderr, "disktput: alignment \"%s\" must be a power"
						" of two and at least %zu\n", optarg, sizeof(void*));
					return 1;
				}
				break;
			case 'U':
				config.misalign = parse_size(optarg);
				if (config.misalign < 0 || config.misalign > B_PAGE_SIZE) {
					fprintf(stderr, "disktput: misalignment \"%s\" must be"
						" between 0 and one page\n", optarg);
					return 1;
				}
				break;
			case 'L':
				label = optarg;
				break;
			case 'D':
				config.noCache = true;
				break;
			case 'S':
				finalSync = true;
				break;
			case 'F':
				config.syncEveryOp = true;
				break;
			case 'y':
				confirmed = true;
				break;
			case 'J':
				machineReadable = true;
				break;
			case 'h':
				return usage(0);
			default:
				return usage(1);
		}
	}

	if (config.path == NULL)
		return usage(1);

	signal(SIGPIPE, SIG_IGN);

	// -------------------------------------------------------------- open once
	// A probe descriptor answers "what is this thing" before anything is
	// written to it, so the destructive-write refusal below can be based on
	// what the target actually is rather than on its path looking device-ish.
	int probeFlags = mode_is_write(config.mode) ? O_RDWR : O_RDONLY;
	int probe = open(config.path, probeFlags);
	if (probe < 0 && mode_is_write(config.mode)) {
		probe = open(config.path, O_RDWR | O_CREAT, 0644);
	}
	if (probe < 0) {
		fprintf(stderr, "disktput: cannot open %s: %s\n", config.path,
			strerror(errno));
		return 1;
	}

	bool device = is_device(probe);
	off_t size = target_size(probe);

	if (device && mode_is_write(config.mode) && !confirmed) {
		fprintf(stderr, "disktput: %s is a raw device and mode %s writes to"
			" it.\n", config.path, mode_name(config.mode));
		fprintf(stderr, "disktput: that destroys every partition and file"
			" system on it. Pass -y if that is genuinely what you want.\n");
		close(probe);
		return 1;
	}

	// Work out the span before deciding how much of it each thread owns.
	off_t span = wantedSpan > 0 ? wantedSpan : totalBytes;
	if (device && size > 0) {
		off_t available = size - config.spanStart;
		if (available <= 0) {
			fprintf(stderr, "disktput: offset %" B_PRIdOFF " is beyond the"
				" %" B_PRIdOFF " byte device\n", config.spanStart, size);
			close(probe);
			return 1;
		}
		if (wantedSpan == 0)
			span = available < totalBytes ? available : totalBytes;
		else if (span > available)
			span = available;
	}

	span -= span % config.blockSize;
	if (span < config.blockSize) {
		fprintf(stderr, "disktput: span %" B_PRIdOFF " is smaller than one"
			" %" B_PRIdOFF " byte block\n", span, config.blockSize);
		close(probe);
		return 1;
	}
	config.span = span;

	close(probe);

	// A read run against a regular file needs the file to exist at full size,
	// and to hold real data rather than holes.
	if (!device && !mode_is_write(config.mode)) {
		status_t status = populate(config.path, config.spanStart + span,
			config.blockSize);
		if (status != B_OK) {
			fprintf(stderr, "disktput: cannot prepare %s: %s\n", config.path,
				strerror(status));
			return 1;
		}
	}

	// ------------------------------------------------------------ the workers
	off_t totalOperations = totalBytes / config.blockSize;
	if (totalOperations < 1)
		totalOperations = 1;

	// A time-based run stops on the clock, not on a count, so the per-thread
	// budget must not be what ends it. Kept finite rather than infinite so a
	// pathological run still terminates.
	if (config.duration > 0)
		totalOperations = (off_t)1 << 40;

	worker* workers = (worker*)calloc(config.threads, sizeof(worker));
	if (workers == NULL) {
		fprintf(stderr, "disktput: out of memory\n");
		return 1;
	}

	barrier gate;
	barrier_init(gate, config.threads + 1);

	// Sequential threads each walk their own contiguous slice, so every thread
	// is doing sequential work; interleaving them across one shared cursor would
	// turn -t into a random-access test and confuse the two measurements.
	off_t blocksPerThread = (span / config.blockSize) / config.threads;
	if (blocksPerThread < 1)
		blocksPerThread = 1;

	int openFlags = mode_is_write(config.mode) ? O_RDWR : O_RDONLY;
	if (config.noCache)
		openFlags |= O_NOCACHE;

	bool noCacheAccepted = config.noCache;
	int started = 0;
	for (int i = 0; i < config.threads; i++) {
		worker& current = workers[i];
		current.config = &config;
		current.gate = &gate;
		current.index = i;
		current.error = B_OK;
		current.fd = -1;

		current.fd = open(config.path, openFlags);
		if (current.fd < 0 && config.noCache) {
			// Worth knowing, and worth saying out loud rather than silently
			// measuring the page cache: the whole point of -D is that the
			// numbers mean something different without it.
			current.fd = open(config.path, openFlags & ~O_NOCACHE);
			if (current.fd >= 0) {
				noCacheAccepted = false;
				if (i == 0) {
					fprintf(stderr, "disktput: O_NOCACHE rejected on %s --"
						" this run INCLUDES the cache\n", config.path);
				}
			}
		}
		if (current.fd < 0) {
			fprintf(stderr, "disktput: thread %d cannot open %s: %s\n", i,
				config.path, strerror(errno));
			break;
		}

		current.buffer = allocate_io_buffer(config.blockSize, config.alignment,
			config.misalign, &current.bufferBase);
		if (current.buffer == NULL) {
			fprintf(stderr, "disktput: thread %d cannot allocate %" B_PRIdOFF
				" bytes\n", i, config.blockSize);
			close(current.fd);
			current.fd = -1;
			break;
		}

		current.regionStart = config.spanStart
			+ (off_t)i * blocksPerThread * config.blockSize;
		current.regionSize = blocksPerThread * config.blockSize;

		current.operations = totalOperations / config.threads;
		if (i < totalOperations % config.threads)
			current.operations++;

		current.samples = (bigtime_t*)malloc(MAX_SAMPLES * sizeof(bigtime_t));

		if (pthread_create(&current.thread, NULL, worker_main, &current) != 0) {
			fprintf(stderr, "disktput: cannot start thread %d\n", i);
			free(current.bufferBase);
			close(current.fd);
			current.fd = -1;
			break;
		}
		started++;
	}

	if (started != config.threads) {
		fprintf(stderr, "disktput: only %d of %d threads started; the run"
			" would not measure the concurrency it claims\n", started,
			config.threads);
		// Release whoever did start so they can exit rather than hang on the
		// barrier, then give up.
		gate.total = started + 1;
		barrier_wait(gate);
		for (int i = 0; i < started; i++) {
			pthread_join(workers[i].thread, NULL);
			free(workers[i].bufferBase);
			free(workers[i].samples);
			close(workers[i].fd);
		}
		free(workers);
		return 1;
	}

	printf("disktput: %s %" B_PRIdOFF " bytes of %s in %" B_PRIdOFF
		" byte blocks across %d thread(s)\n", mode_is_write(config.mode)
			? "writing" : "reading", totalBytes, config.path, config.blockSize,
		config.threads);
	report_target(config.path, workers[0].fd, noCacheAccepted, size);
	fflush(stdout);

	cpu_snapshot before;
	cpu_snapshot after;

	take_cpu_snapshot(before);
	barrier_wait(gate);				// the run starts here

	for (int i = 0; i < config.threads; i++)
		pthread_join(workers[i].thread, NULL);

	// The fsync is timed inside the measured window on purpose. A write
	// benchmark that stops when the last pwrite returns is measuring how fast
	// the kernel can accept bytes, which on this tree used to be infinite
	// because the page writer never wrote them out at all.
	bigtime_t syncTime = -1;
	if (finalSync && mode_is_write(config.mode)) {
		bigtime_t syncStart = system_time();
		for (int i = 0; i < config.threads; i++) {
			if (fsync(workers[i].fd) != 0) {
				fprintf(stderr, "disktput: fsync failed on thread %d: %s\n", i,
					strerror(errno));
			}
		}
		syncTime = system_time() - syncStart;
	}

	take_cpu_snapshot(after);

	// ------------------------------------------------------------- accumulate
	off_t bytes = 0;
	off_t operations = 0;
	bigtime_t latencyTotal = 0;
	bigtime_t latencyMax = 0;
	uint32 sampleCount = 0;
	bool failed = false;

	for (int i = 0; i < config.threads; i++) {
		const worker& current = workers[i];
		bytes += current.bytesMoved;
		operations += current.operationsDone;
		latencyTotal += current.latencyTotal;
		if (current.latencyMax > latencyMax)
			latencyMax = current.latencyMax;
		sampleCount += current.sampleCount;
		if (current.error != B_OK) {
			fprintf(stderr, "disktput: thread %d failed at offset %" B_PRIdOFF
				" after %" B_PRIdOFF " operations: %s\n", i,
				current.errorOffset, current.operationsDone,
				strerror(current.error));
			failed = true;
		}
	}

	bigtime_t* samples = (bigtime_t*)malloc(
		(sampleCount > 0 ? sampleCount : 1) * sizeof(bigtime_t));
	uint32 at = 0;
	if (samples != NULL) {
		for (int i = 0; i < config.threads; i++) {
			for (uint32 j = 0; j < workers[i].sampleCount; j++)
				samples[at++] = workers[i].samples[j];
		}
	}

	for (int i = 0; i < config.threads; i++) {
		free(workers[i].bufferBase);
		free(workers[i].samples);
		close(workers[i].fd);
	}

	if (!failed) {
		config.noCache = noCacheAccepted;
		report(config, label, bytes, operations, after.wall - before.wall,
			cpu_busy_between(before, after), before.count, samples, at,
			latencyTotal, latencyMax, syncTime, machineReadable);
	}

	free(samples);
	free(workers);

	return failed ? 1 : 0;
}
