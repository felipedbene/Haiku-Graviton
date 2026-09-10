/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * ena_fault -- provoke the ENA driver's watchdog without breaking hardware.
 *
 * The driver can be built with ENA_DEBUG_FAULT_INJECTION, which adds two private
 * ioctls. The first makes its keep-alive handler stop advancing the timestamp: the
 * watchdog then observes a device that has stopped talking, while the device is in
 * fact perfectly healthy -- so anything that goes wrong during the test is
 * unambiguously the driver's fault rather than the hardware's. The second stalls a
 * reset at its widest point, so a concurrent teardown can be aimed at a window
 * that is otherwise 27-84 ms wide.
 *
 * This is the caller for both, and it exists because there is no other way to
 * reach them: a stock image has no compiler and no generic ioctl tool.
 *
 *   ena_fault 0            stop suppressing
 *   ena_fault 1            suppress until the watchdog fires once
 *   ena_fault 2            suppress until cleared (repeated resets)
 *   ena_fault hold <ms>    stall the next reset for <ms>, 0 to disable
 *   ena_fault doorbells <n>  ring the transmit doorbell n extra times per frame
 *   ena_fault stats [s]    sample the counters for s seconds and report frames
 *                          per io interrupt
 *
 * The doorbell knob is a measurement tool rather than a fault, and unlike the
 * other two it is always compiled into the driver. It prices one doorbell write
 * without needing the batched transmit entry point that real coalescing would
 * require, and because it takes effect immediately the A/B can be interleaved
 * inside one boot.
 *
 * Opening the device while the network stack holds it open is safe and is
 * deliberately part of the test: it exercises the open-count guard that makes a
 * second open a no-op instead of resetting the receive ring under the stack.
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
   into a userland tool. */
#define ENA_IOCTL_SUPPRESS_KEEP_ALIVE	9800
#define ENA_IOCTL_HOLD_RESET		9801
#define ENA_IOCTL_TX_EXTRA_DOORBELLS	9802
#define ENA_IOCTL_GET_IRQ_STATS		9803
#define ENA_IOCTL_REARM_MODE		9804
#define ENA_REARM_IN_HANDLER		0
#define ENA_REARM_AFTER_DRAIN		1
#define ENA_MAX_RESET_HOLD_MS		30000
#define ENA_MAX_EXTRA_DOORBELLS		64

#define ENA_DEVICE_PATH			"/dev/net/ena/0"

struct ena_irq_stats {
	uint64	ioInterrupts;
	uint64	irqArms;
	uint64	rxFrames;
	uint64	rxDrainCycles;
	uint64	txFrames;
	uint64	resetCount;
	uint64	rearmMode;
	/* These two trailing fields must stay in lockstep with struct ena_irq_stats
	   in the driver's ena.h: ENA_IOCTL_GET_IRQ_STATS rejects the call when the
	   caller's sizeof does not match the driver's, so a short struct here makes
	   `ena_fault stats` fail with B_BAD_VALUE ("Invalid Argument"). */
	uint64	rxIrqInterval;
	uint64	intrDelayResolution;
};


static void
usage(const char* program)
{
	fprintf(stderr, "usage: %s <0|1|2>\n"
		"       %s hold <milliseconds>\n"
		"       %s doorbells <n>\n"
		"       %s stats [seconds]\n"
		"       %s rearm <0|1>\n"
		"  stats [s]     sample the driver's counters over [s] seconds "
		"(default 10)\n"
		"                and report frames per io interrupt\n"
		"  rearm <0|1>   where the io vector is re-armed: 0 in the interrupt\n"
		"                handler (the old cadence), 1 after the drain (default)\n"
		"  0             stop suppressing keep-alive\n"
		"  1             suppress until the watchdog fires once\n"
		"  2             suppress until cleared (repeated resets)\n"
		"  hold <ms>     stall the next reset for <ms> at its widest point, so a\n"
		"                concurrent \"ifconfig down\" can be aimed at it; 0 disables\n"
		"  doorbells <n> ring the transmit doorbell <n> extra times per frame, to\n"
		"                price one doorbell write; 0 restores normal behaviour\n",
		program, program, program, program, program);
}


static int
send_value(const char* program, uint32 op, int32 value, const char* description)
{
	int fd = open(ENA_DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n", program, ENA_DEVICE_PATH,
			strerror(errno));
		return 1;
	}

	int result = ioctl(fd, op, &value, sizeof(value));
	if (result < 0) {
		fprintf(stderr, "%s: ioctl failed: %s\n"
			"  (is the driver built with ENA_DEBUG_FAULT_INJECTION?)\n",
			program, strerror(errno));
		close(fd);
		return 1;
	}

	printf("%s %d\n", description, (int)value);
	close(fd);
	return 0;
}


/*!	Samples the driver's counters twice and reports the rates between them.

	Frames per interrupt is the point of this. The driver is re-armed at the end
	of a drain, so a burst that arrives while the vector is masked is consumed by
	one wakeup: the ratio is how many frames that turned out to be, and it is the
	only figure that distinguishes an interrupt cadence change from a throughput
	change that happened for some other reason.

	Deltas rather than totals, so it can be run against a load that is already in
	flight without counting the idle period before it.
*/
static int
show_stats(const char* program, int seconds)
{
	int fd = open(ENA_DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n", program, ENA_DEVICE_PATH,
			strerror(errno));
		return 1;
	}

	struct ena_irq_stats before;
	struct ena_irq_stats after;

	if (ioctl(fd, ENA_IOCTL_GET_IRQ_STATS, &before, sizeof(before)) < 0) {
		fprintf(stderr, "%s: ioctl failed: %s\n"
			"  (does this driver have ENA_IOCTL_GET_IRQ_STATS? check the build "
			"stamp in the syslog)\n", program, strerror(errno));
		close(fd);
		return 1;
	}

	sleep(seconds);

	if (ioctl(fd, ENA_IOCTL_GET_IRQ_STATS, &after, sizeof(after)) < 0) {
		fprintf(stderr, "%s: second ioctl failed: %s\n", program,
			strerror(errno));
		close(fd);
		return 1;
	}

	close(fd);

	/* The reset path zeroes ioInterrupts and leaves rxFrames alone, so a reset
	   inside the interval produces a ratio that is wrong without looking wrong.
	   Refuse to print one. */
	if (after.resetCount != before.resetCount) {
		fprintf(stderr, "%s: the device reset during the interval "
			"(resetCount %llu -> %llu); sample discarded\n", program,
			(unsigned long long)before.resetCount,
			(unsigned long long)after.resetCount);
		return 1;
	}

	/* Likewise for the arm itself: a sample that spans a mode change belongs to
	   neither arm. */
	if (after.rearmMode != before.rearmMode) {
		fprintf(stderr, "%s: the rearm mode changed during the interval "
			"(%llu -> %llu); sample discarded\n", program,
			(unsigned long long)before.rearmMode,
			(unsigned long long)after.rearmMode);
		return 1;
	}

	const uint64 interrupts = after.ioInterrupts - before.ioInterrupts;
	const uint64 arms = after.irqArms - before.irqArms;
	const uint64 frames = after.rxFrames - before.rxFrames;
	const uint64 drains = after.rxDrainCycles - before.rxDrainCycles;
	const uint64 txFrames = after.txFrames - before.txFrames;

	printf("rearm mode          %llu (%s)\n",
		(unsigned long long)after.rearmMode,
		after.rearmMode == ENA_REARM_IN_HANDLER
			? "in handler -- the old cadence" : "after drain");
	printf("interval            %d s\n", seconds);
	printf("io interrupts       %llu (%.0f/s)\n", (unsigned long long)interrupts,
		(double)interrupts / seconds);
	printf("vector re-arms      %llu\n", (unsigned long long)arms);
	printf("rx frames           %llu (%.0f/s)\n", (unsigned long long)frames,
		(double)frames / seconds);
	printf("rx drain cycles     %llu\n", (unsigned long long)drains);
	printf("tx frames           %llu (%.0f/s)\n",
		(unsigned long long)txFrames, (double)txFrames / seconds);

	if (interrupts > 0) {
		printf("FRAMES/INTERRUPT    %.2f\n", (double)frames / interrupts);
	} else {
		printf("FRAMES/INTERRUPT    n/a (no interrupts in the interval -- if "
			"frames moved, this driver is not the one being measured)\n");
	}
	if (drains > 0)
		printf("frames/drain        %.2f\n", (double)frames / drains);

	return 0;
}


int
main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "rearm") == 0) {
		int32 mode = (int32)strtol(argv[2], NULL, 10);
		if (mode != ENA_REARM_IN_HANDLER && mode != ENA_REARM_AFTER_DRAIN) {
			fprintf(stderr, "%s: rearm mode must be %d (in handler) or %d "
				"(after drain)\n", argv[0], ENA_REARM_IN_HANDLER,
				ENA_REARM_AFTER_DRAIN);
			return 1;
		}

		return send_value(argv[0], ENA_IOCTL_REARM_MODE, mode,
			"io vector rearm mode set to");
	}

	if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
		int seconds = 10;
		if (argc == 3)
			seconds = (int)strtol(argv[2], NULL, 10);
		if (argc > 3 || seconds < 1 || seconds > 3600) {
			fprintf(stderr, "%s: stats takes an interval of 1-3600 seconds\n",
				argv[0]);
			return 1;
		}
		return show_stats(argv[0], seconds);
	}

	if (argc == 3 && strcmp(argv[1], "doorbells") == 0) {
		int32 extra = (int32)strtol(argv[2], NULL, 10);
		if (extra < 0 || extra > ENA_MAX_EXTRA_DOORBELLS) {
			fprintf(stderr, "%s: doorbells must be between 0 and %d\n", argv[0],
				ENA_MAX_EXTRA_DOORBELLS);
			return 1;
		}

		return send_value(argv[0], ENA_IOCTL_TX_EXTRA_DOORBELLS, extra,
			"extra transmit doorbells per frame set to");
	}

	if (argc == 3 && strcmp(argv[1], "hold") == 0) {
		int32 milliseconds = (int32)strtol(argv[2], NULL, 10);
		if (milliseconds < 0 || milliseconds > ENA_MAX_RESET_HOLD_MS) {
			fprintf(stderr, "%s: hold must be between 0 and %d ms\n", argv[0],
				ENA_MAX_RESET_HOLD_MS);
			return 1;
		}

		return send_value(argv[0], ENA_IOCTL_HOLD_RESET, milliseconds,
			"reset hold set to");
	}

	if (argc != 2) {
		usage(argv[0]);
		return 1;
	}

	int32 mode = (int32)strtol(argv[1], NULL, 10);
	if (mode < 0 || mode > 2) {
		usage(argv[0]);
		return 1;
	}

	return send_value(argv[0], ENA_IOCTL_SUPPRESS_KEEP_ALIVE, mode,
		"keep-alive suppression set to");
}
