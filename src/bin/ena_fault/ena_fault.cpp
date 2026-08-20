/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * ena_fault -- provoke the ENA driver's watchdog without breaking hardware.
 *
 * The driver can be built with ENA_DEBUG_FAULT_INJECTION, which adds one private
 * ioctl that makes its keep-alive handler stop advancing the timestamp. The
 * watchdog then observes a device that has stopped talking, while the device is in
 * fact perfectly healthy -- so anything that goes wrong during the test is
 * unambiguously the driver's fault rather than the hardware's.
 *
 * This is the caller for that ioctl, and it exists because there is no other way
 * to reach it: a stock image has no compiler and no generic ioctl tool.
 *
 *   ena_fault 0    stop suppressing
 *   ena_fault 1    suppress one keep-alive (a single timeout)
 *   ena_fault 2    suppress until cleared (repeated resets)
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

#define ENA_DEVICE_PATH			"/dev/net/ena/0"


int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <0|1|2>\n"
			"  0  stop suppressing keep-alive\n"
			"  1  suppress one keep-alive (single timeout)\n"
			"  2  suppress until cleared (repeated resets)\n", argv[0]);
		return 1;
	}

	int32 mode = (int32)strtol(argv[1], NULL, 10);
	if (mode < 0 || mode > 2) {
		fprintf(stderr, "%s: mode must be 0, 1 or 2\n", argv[0]);
		return 1;
	}

	int fd = open(ENA_DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n", argv[0], ENA_DEVICE_PATH,
			strerror(errno));
		return 1;
	}

	int result = ioctl(fd, ENA_IOCTL_SUPPRESS_KEEP_ALIVE, &mode, sizeof(mode));
	if (result < 0) {
		fprintf(stderr, "%s: ioctl failed: %s\n"
			"  (is the driver built with ENA_DEBUG_FAULT_INJECTION?)\n",
			argv[0], strerror(errno));
		close(fd);
		return 1;
	}

	printf("keep-alive suppression set to %d\n", (int)mode);
	close(fd);
	return 0;
}
