/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * ena_stats -- print the ENA driver's ENI/device statistics.
 *
 * The driver already narrates its counters into the syslog, but a syslog line is
 * a point in time and cannot be diffed. This calls ENA_IOCTL_GET_ENI_STATS, which
 * returns the device's own drop counters, the driver's per-direction packet and
 * byte totals, the receive/transmit checksum-offload observations, and the reset
 * accounting -- in one snapshot a tool can poll. It exists for the same reason as
 * ena_fault: a stock image has no compiler and no generic ioctl tool, so there is
 * otherwise no way to read these from userland.
 *
 *   ena_stats            print the counters once
 *   ena_stats <seconds>  sample twice <seconds> apart and print the totals plus
 *                        the per-second rates between the two samples
 *
 * Totals are monotonic per device (they are not reset by a device reset), so a
 * single-shot read is meaningful on its own; the interval form is for watching a
 * load that is already in flight without counting the idle time before it.
 */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <SupportDefs.h>


/* Must match ena.h. Deliberately duplicated rather than pulling a driver header
   into a userland tool, exactly as ena_fault does. */
#define ENA_IOCTL_GET_ENI_STATS		9806

#define ENA_DEVICE_PATH			"/dev/net/ena/0"

/* Must stay byte-for-byte in lockstep with struct ena_eni_stats in the driver's
   ena.h: ENA_IOCTL_GET_ENI_STATS rejects the call when the caller's sizeof does
   not match the driver's, so a stale struct here fails with B_BAD_VALUE
   ("Invalid Argument") rather than returning wrong numbers. */
struct ena_eni_stats {
	uint64	hwRxDrops;
	uint64	hwTxDrops;
	uint64	rxPackets;
	uint64	rxBytes;
	uint64	txPackets;
	uint64	txBytes;
	uint64	rxDrainCycles;
	uint64	rxL4CsumChecked;
	uint64	rxL4CsumErrors;
	uint64	rxL3Ipv4Frames;
	uint64	rxL3CsumErrors;
	uint64	txChecksumOffloaded;
	uint64	txChecksumRejected;
	uint64	txDoorbells;
	uint64	txBurstExhausted;
	uint64	resetCount;
	uint64	adminWedgeResets;
	uint64	fatalErrorResets;
	uint64	deviceRequestResets;
	uint64	missingTxResets;
	uint64	rxStallDetections;
	uint64	linkUp;
	uint64	mtu;
};


static void
usage(const char* program)
{
	fprintf(stderr, "usage: %s [seconds]\n"
		"  (no argument)  print the ENI/device counters once\n"
		"  seconds        sample twice, <seconds> apart, and also report the\n"
		"                 per-second rates between the two samples (1-3600)\n",
		program);
}


static int
read_stats(const char* program, int fd, struct ena_eni_stats* stats)
{
	if (ioctl(fd, ENA_IOCTL_GET_ENI_STATS, stats, sizeof(*stats)) < 0) {
		fprintf(stderr, "%s: ioctl failed: %s\n"
			"  (does this driver have ENA_IOCTL_GET_ENI_STATS? check the build "
			"stamp in the syslog)\n", program, strerror(errno));
		return 1;
	}
	return 0;
}


static void
print_totals(const struct ena_eni_stats* s)
{
	printf("link                %s\n", s->linkUp ? "up" : "down");
	printf("mtu                 %llu\n", (unsigned long long)s->mtu);
	printf("\n");
	printf("rx packets          %llu\n", (unsigned long long)s->rxPackets);
	printf("rx bytes            %llu\n", (unsigned long long)s->rxBytes);
	printf("tx packets          %llu\n", (unsigned long long)s->txPackets);
	printf("tx bytes            %llu\n", (unsigned long long)s->txBytes);
	printf("\n");
	printf("hw rx drops         %llu\n", (unsigned long long)s->hwRxDrops);
	printf("hw tx drops         %llu\n", (unsigned long long)s->hwTxDrops);
	printf("\n");
	printf("rx drain cycles     %llu\n", (unsigned long long)s->rxDrainCycles);
	printf("rx L4 csum checked  %llu\n",
		(unsigned long long)s->rxL4CsumChecked);
	printf("rx L4 csum errors   %llu\n", (unsigned long long)s->rxL4CsumErrors);
	printf("rx L3 ipv4 frames   %llu\n", (unsigned long long)s->rxL3Ipv4Frames);
	printf("rx L3 csum errors   %llu\n", (unsigned long long)s->rxL3CsumErrors);
	printf("tx csum offloaded   %llu\n",
		(unsigned long long)s->txChecksumOffloaded);
	printf("tx csum rejected    %llu\n",
		(unsigned long long)s->txChecksumRejected);
	printf("tx doorbells        %llu\n", (unsigned long long)s->txDoorbells);
	printf("tx burst exhausted  %llu\n",
		(unsigned long long)s->txBurstExhausted);
	printf("\n");
	printf("resets total        %llu\n", (unsigned long long)s->resetCount);
	printf("  admin wedge       %llu\n",
		(unsigned long long)s->adminWedgeResets);
	printf("  fatal error       %llu\n",
		(unsigned long long)s->fatalErrorResets);
	printf("  device request    %llu\n",
		(unsigned long long)s->deviceRequestResets);
	printf("  missing tx compl  %llu\n", (unsigned long long)s->missingTxResets);
	printf("rx stall detections %llu\n",
		(unsigned long long)s->rxStallDetections);
}


static void
print_rates(const struct ena_eni_stats* before,
	const struct ena_eni_stats* after, int seconds)
{
	const uint64 rxPackets = after->rxPackets - before->rxPackets;
	const uint64 rxBytes = after->rxBytes - before->rxBytes;
	const uint64 txPackets = after->txPackets - before->txPackets;
	const uint64 txBytes = after->txBytes - before->txBytes;
	const uint64 hwRxDrops = after->hwRxDrops - before->hwRxDrops;
	const uint64 hwTxDrops = after->hwTxDrops - before->hwTxDrops;

	printf("\n--- rates over %d s ---\n", seconds);
	printf("rx                  %.0f pkt/s, %.2f Mbit/s\n",
		(double)rxPackets / seconds,
		(double)rxBytes * 8.0 / seconds / 1e6);
	printf("tx                  %.0f pkt/s, %.2f Mbit/s\n",
		(double)txPackets / seconds,
		(double)txBytes * 8.0 / seconds / 1e6);
	printf("hw rx drops         %llu (%.2f/s)\n",
		(unsigned long long)hwRxDrops, (double)hwRxDrops / seconds);
	printf("hw tx drops         %llu (%.2f/s)\n",
		(unsigned long long)hwTxDrops, (double)hwTxDrops / seconds);
}


int
main(int argc, char** argv)
{
	int seconds = 0;
	if (argc == 2) {
		seconds = (int)strtol(argv[1], NULL, 10);
		if (seconds < 1 || seconds > 3600) {
			usage(argv[0]);
			return 1;
		}
	} else if (argc > 2) {
		usage(argv[0]);
		return 1;
	}

	int fd = open(ENA_DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n", argv[0], ENA_DEVICE_PATH,
			strerror(errno));
		return 1;
	}

	struct ena_eni_stats before;
	if (read_stats(argv[0], fd, &before) != 0) {
		close(fd);
		return 1;
	}

	if (seconds == 0) {
		close(fd);
		print_totals(&before);
		return 0;
	}

	sleep(seconds);

	struct ena_eni_stats after;
	if (read_stats(argv[0], fd, &after) != 0) {
		close(fd);
		return 1;
	}
	close(fd);

	/* A reset zeroes some driver counters and leaves others alone, so a delta
	   spanning one is meaningless. Report the totals but refuse to print rates. */
	print_totals(&after);
	if (after.resetCount != before.resetCount) {
		fprintf(stderr, "\n%s: the device reset during the interval "
			"(resetCount %llu -> %llu); rates suppressed\n", argv[0],
			(unsigned long long)before.resetCount,
			(unsigned long long)after.resetCount);
		return 1;
	}

	print_rates(&before, &after, seconds);
	return 0;
}
