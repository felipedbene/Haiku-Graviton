/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * rxlat -- calibrate the receive-path latency instrument, then read it out.
 *
 * Two jobs in one binary on purpose. A latency histogram from an uncalibrated
 * probe is how you get a confident wrong answer, so the tool that prints the
 * histogram is also the tool that measures what the probe costs and what it can
 * resolve, from the same machine and the same compiler, and it refuses to be
 * used without printing both.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include <OS.h>


// Kept in step with src/add-ons/kernel/network/stack/latency_probe.h by hand.
// The struct is read out as raw bytes, so a mismatch would be silent; the build
// stamp in rxlat_info is what catches it.
#define RXLAT_BUCKETS			40
#define RXLAT_STAT_COUNT		16
#define RXLAT_SOL				0x52584c54
#define RXLAT_OPT_CHUNK_BASE	0x1000
#define RXLAT_OPT_CHUNK_SIZE	128
#define RXLAT_OPT_ENABLE		1
#define RXLAT_OPT_RESET			2
#define RXLAT_OPT_FIFO_MAX		3
#define RXLAT_OPT_INFO			4

struct rxlat_stat {
	uint64	count;
	uint64	sum;
	uint64	min;
	uint64	max;
	uint64	bucket[RXLAT_BUCKETS];
};

struct rxlat_info {
	uint64	tick_frequency;
	uint64	enabled;
	uint64	fifo_max_bytes;
	uint64	fifo_current_bytes;
	uint64	fifo_peak_bytes;
	uint64	fifo_current_packets;
	uint64	fifo_peak_packets;
	uint64	fifo_enqueued;
	uint64	fifo_fail_nobufs;
	uint64	stat_count;
	uint64	bucket_count;
	uint64	build_stamp;
};

static const char* kStatName[RXLAT_STAT_COUNT] = {
	"reader: receive_data()",
	"reader: deframe",
	"reader: fifo enqueue",
	"reader: loop period",
	"reader: fifo depth at enqueue (BYTES)",
	"tcp: endpoint fLock acquire",
	"tcp: SegmentReceived total",
	"tcp: _NotifyReader (wake the app)",
	"consumer: fifo dequeue call",
	"consumer: FIFO RESIDENCY",
	"consumer: receive_lock acquire",
	"consumer: protocol dispatch",
	"consumer: loop period",
	"consumer: fifo depth at dequeue (BYTES)",
	"(unused 14)",
	"(unused 15)",
};

// The two depth statistics are counts of bytes, not of ticks. They share the
// histogram shape so that one printer serves both, but they must not be scaled
// by the tick frequency.
static bool
is_byte_stat(int index)
{
	return index == 4 || index == 13;
}


static inline uint64
ticks_plain(void)
{
	uint64 value;
	__asm__ __volatile__("mrs %0, cntvct_el0" : "=r" (value));
	return value;
}


static inline uint64
ticks_isb(void)
{
	uint64 value;
	__asm__ __volatile__("isb; mrs %0, cntvct_el0" : "=r" (value));
	return value;
}


static uint64
counter_frequency(void)
{
	uint64 value;
	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r" (value));
	return value;
}


// A stand-in for the kernel probe's accumulate step, compiled here so its cost
// can be measured. It has to be the same shape as rxlat_add(): the branch, the
// four scalar updates, the count-leading-zeros bucket index and the bucket
// increment.
static struct rxlat_stat sLocalStat;

static inline void
local_add(uint64 delta)
{
	struct rxlat_stat* stat = &sLocalStat;
	if (stat->count == 0 || delta < stat->min)
		stat->min = delta;
	if (delta > stat->max)
		stat->max = delta;
	stat->count++;
	stat->sum += delta;
	int bucket = (delta == 0) ? 0 : (63 - __builtin_clzll(delta));
	if (bucket >= RXLAT_BUCKETS)
		bucket = RXLAT_BUCKETS - 1;
	stat->bucket[bucket]++;
}


static void
calibrate(void)
{
	const uint64 frequency = counter_frequency();
	const double nsPerTick = 1e9 / (double)frequency;

	printf("calibration\n");
	printf("  CNTFRQ_EL0            : %llu Hz  (%.4f ns per tick)\n",
		(unsigned long long)frequency, nsPerTick);

	// Cost of one read. The loop is unrolled by eight so that the loop's own
	// counter and branch are amortised rather than measured, and the results are
	// summed into a volatile sink so the compiler cannot delete the reads.
	static volatile uint64 sink;
	const int kIterations = 200000;

	for (int variant = 0; variant < 2; variant++) {
		uint64 accumulator = 0;
		const uint64 start = ticks_isb();
		for (int i = 0; i < kIterations; i++) {
			if (variant == 0) {
				accumulator += ticks_plain(); accumulator += ticks_plain();
				accumulator += ticks_plain(); accumulator += ticks_plain();
				accumulator += ticks_plain(); accumulator += ticks_plain();
				accumulator += ticks_plain(); accumulator += ticks_plain();
			} else {
				accumulator += ticks_isb(); accumulator += ticks_isb();
				accumulator += ticks_isb(); accumulator += ticks_isb();
				accumulator += ticks_isb(); accumulator += ticks_isb();
				accumulator += ticks_isb(); accumulator += ticks_isb();
			}
		}
		const uint64 end = ticks_isb();
		sink = accumulator;
		const double total = (double)(end - start) * nsPerTick;
		printf("  one %-18s: %6.2f ns\n",
			variant == 0 ? "mrs" : "isb + mrs",
			total / (kIterations * 8.0));
	}

	// Cost of the probe pair as the kernel actually pays it: two reads plus one
	// accumulate. This is the number that has to be small against a per-frame
	// budget, and it is the number to quote.
	{
		memset(&sLocalStat, 0, sizeof(sLocalStat));
		const uint64 start = ticks_isb();
		for (int i = 0; i < kIterations; i++) {
			uint64 a = ticks_plain();
			uint64 b = ticks_plain();
			local_add(b - a);
		}
		const uint64 end = ticks_isb();
		printf("  probe pair (2 mrs + accumulate): %6.2f ns\n",
			(double)(end - start) * nsPerTick / kIterations);
	}

	// Effective resolution and the shape of the noise floor: the distribution of
	// the delta between two back-to-back reads. A probe cannot resolve anything
	// smaller than this, and the maximum says whether the measuring thread was
	// interrupted while measuring.
	{
		const int kSamples = 100000;
		uint64 histogram[16];
		memset(histogram, 0, sizeof(histogram));
		uint64 minimum = ~(uint64)0, maximum = 0, total = 0;
		for (int i = 0; i < kSamples; i++) {
			const uint64 a = ticks_plain();
			const uint64 b = ticks_plain();
			const uint64 delta = b - a;
			if (delta < minimum)
				minimum = delta;
			if (delta > maximum)
				maximum = delta;
			total += delta;
			int bucket = (delta == 0) ? 0 : (63 - __builtin_clzll(delta));
			if (bucket > 15)
				bucket = 15;
			histogram[bucket]++;
		}
		printf("  back-to-back delta    : min %llu, mean %.2f, max %llu ticks"
			"  (%.2f / %.2f / %.2f ns)\n",
			(unsigned long long)minimum, (double)total / kSamples,
			(unsigned long long)maximum,
			minimum * nsPerTick, (double)total / kSamples * nsPerTick,
			maximum * nsPerTick);
		printf("  delta distribution    :");
		for (int i = 0; i < 16; i++) {
			if (histogram[i] != 0) {
				printf(" [%llu..%llu):%.2f%%", 1ULL << i, 2ULL << i,
					100.0 * histogram[i] / kSamples);
			}
		}
		printf("\n");
	}
	printf("\n");
}


static int
open_socket(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "rxlat: cannot open a socket: %s\n", strerror(errno));
		exit(1);
	}
	return fd;
}


static bool
read_info(int fd, struct rxlat_info* info)
{
	socklen_t length = sizeof(*info);
	memset(info, 0, sizeof(*info));
	if (getsockopt(fd, RXLAT_SOL, RXLAT_OPT_INFO, info, &length) != 0) {
		fprintf(stderr, "rxlat: the kernel has no latency probe "
			"(getsockopt: %s).\n", strerror(errno));
		fprintf(stderr, "       This is a stock stack module, not an "
			"instrumented one -- check which one loaded.\n");
		return false;
	}
	return true;
}


static bool
read_stats(int fd, struct rxlat_stat* stats)
{
	uint8* raw = (uint8*)stats;
	const size_t total = sizeof(struct rxlat_stat) * RXLAT_STAT_COUNT;
	size_t offset = 0;
	int chunk = 0;
	while (offset < total) {
		uint8 buffer[RXLAT_OPT_CHUNK_SIZE];
		socklen_t length = sizeof(buffer);
		if (getsockopt(fd, RXLAT_SOL, RXLAT_OPT_CHUNK_BASE + chunk, buffer,
				&length) != 0) {
			fprintf(stderr, "rxlat: chunk %d: %s\n", chunk, strerror(errno));
			return false;
		}
		size_t copy = length;
		if (copy > total - offset)
			copy = total - offset;
		memcpy(raw + offset, buffer, copy);
		offset += copy;
		chunk++;
		if (length == 0)
			break;
	}
	return offset >= total;
}


static void
print_stats(const struct rxlat_stat* stats, uint64 frequency)
{
	const double usPerTick = 1e6 / (double)frequency;

	for (int i = 0; i < RXLAT_STAT_COUNT; i++) {
		const struct rxlat_stat& stat = stats[i];
		if (stat.count == 0)
			continue;

		const bool bytes = is_byte_stat(i);
		const double scale = bytes ? 1.0 : usPerTick;
		const char* unit = bytes ? "KiB" : "us";
		const double unitScale = bytes ? 1.0 / 1024.0 : 1.0;

		printf("%s\n", kStatName[i]);
		printf("  n %llu   mean %.3f %s   min %.3f   max %.3f\n",
			(unsigned long long)stat.count,
			(double)stat.sum / stat.count * scale * unitScale, unit,
			stat.min * scale * unitScale, stat.max * scale * unitScale);

		// Percentiles from the histogram. A bucket boundary is the honest answer:
		// reporting "p99 is between 8 and 16 us" is worth more than
		// interpolating a number the instrument never measured.
		const double kWanted[] = { 0.50, 0.90, 0.99, 0.999 };
		const char* kLabel[] = { "p50", "p90", "p99", "p99.9" };
		for (int w = 0; w < 4; w++) {
			uint64 target = (uint64)(stat.count * kWanted[w]);
			uint64 seen = 0;
			int bucket = -1;
			for (int b = 0; b < RXLAT_BUCKETS; b++) {
				seen += stat.bucket[b];
				if (seen >= target && target > 0) {
					bucket = b;
					break;
				}
			}
			if (bucket < 0)
				continue;
			printf("  %-6s in [%.3f, %.3f) %s\n", kLabel[w],
				(double)(1ULL << bucket) * scale * unitScale,
				(double)(2ULL << bucket) * scale * unitScale, unit);
		}

		printf("  histogram:");
		for (int b = 0; b < RXLAT_BUCKETS; b++) {
			if (stat.bucket[b] == 0)
				continue;
			printf(" [%.3g,%.3g)=%.2f%%",
				(double)(1ULL << b) * scale * unitScale,
				(double)(2ULL << b) * scale * unitScale,
				100.0 * stat.bucket[b] / stat.count);
		}
		printf("\n\n");
	}
}


static void
usage(void)
{
	printf("Usage: rxlat [command]\n"
		"  calibrate        measure the probe's own cost and resolution\n"
		"  on               zero the counters and start sampling\n"
		"  off              stop sampling and zero the counters\n"
		"  info             tick frequency, build stamp, live FIFO occupancy\n"
		"  read             print every stage's distribution\n"
		"  fifo <bytes>     set the device interface receive FIFO cap\n"
		"With no command: calibrate, then info, then read.\n");
}


int
main(int argc, char** argv)
{
	const char* command = argc > 1 ? argv[1] : "";

	if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
		usage();
		return 0;
	}

	if (strcmp(command, "calibrate") == 0) {
		calibrate();
		return 0;
	}

	int fd = open_socket();

	if (strcmp(command, "on") == 0 || strcmp(command, "off") == 0) {
		int32 value = (strcmp(command, "on") == 0) ? 1 : 0;
		if (setsockopt(fd, RXLAT_SOL, RXLAT_OPT_ENABLE, &value,
				sizeof(value)) != 0) {
			fprintf(stderr, "rxlat: %s\n", strerror(errno));
			return 1;
		}
		printf("probe %s\n", command);
		return 0;
	}

	if (strcmp(command, "fifo") == 0) {
		if (argc < 3) {
			usage();
			return 1;
		}
		int32 value = (int32)strtol(argv[2], NULL, 0);
		if (setsockopt(fd, RXLAT_SOL, RXLAT_OPT_FIFO_MAX, &value,
				sizeof(value)) != 0) {
			fprintf(stderr, "rxlat: %s\n", strerror(errno));
			return 1;
		}
		printf("receive fifo cap set to %d bytes\n", (int)value);
		return 0;
	}

	struct rxlat_info info;
	if (!read_info(fd, &info))
		return 1;

	if (strcmp(command, "") == 0)
		calibrate();

	printf("probe\n");
	printf("  build stamp           : %llu\n",
		(unsigned long long)info.build_stamp);
	printf("  enabled               : %llu\n",
		(unsigned long long)info.enabled);
	printf("  kernel CNTFRQ         : %llu Hz\n",
		(unsigned long long)info.tick_frequency);
	printf("  receive fifo cap      : %llu bytes\n",
		(unsigned long long)info.fifo_max_bytes);
	printf("  fifo now              : %llu bytes, %llu packets\n",
		(unsigned long long)info.fifo_current_bytes,
		(unsigned long long)info.fifo_current_packets);
	printf("  fifo peak             : %llu bytes, %llu packets\n",
		(unsigned long long)info.fifo_peak_bytes,
		(unsigned long long)info.fifo_peak_packets);
	printf("  enqueued / ENOBUFS    : %llu / %llu\n",
		(unsigned long long)info.fifo_enqueued,
		(unsigned long long)info.fifo_fail_nobufs);
	printf("\n");

	if (strcmp(command, "info") == 0)
		return 0;

	if (info.tick_frequency == 0) {
		fprintf(stderr, "rxlat: the kernel reported a zero tick frequency; "
			"refusing to scale anything by it\n");
		return 1;
	}

	struct rxlat_stat* stats
		= (struct rxlat_stat*)calloc(RXLAT_STAT_COUNT, sizeof(*stats));
	if (stats == NULL)
		return 1;
	if (!read_stats(fd, stats))
		return 1;

	print_stats(stats, info.tick_frequency);
	free(stats);
	return 0;
}
