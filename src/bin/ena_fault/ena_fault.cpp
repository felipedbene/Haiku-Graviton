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
#define ENA_MAX_RESET_HOLD_MS		30000

#define ENA_DEVICE_PATH			"/dev/net/ena/0"


static void
usage(const char* program)
{
	fprintf(stderr, "usage: %s <0|1|2>\n"
		"       %s hold <milliseconds>\n"
		"  0             stop suppressing keep-alive\n"
		"  1             suppress until the watchdog fires once\n"
		"  2             suppress until cleared (repeated resets)\n"
		"  hold <ms>     stall the next reset for <ms> at its widest point, so a\n"
		"                concurrent \"ifconfig down\" can be aimed at it; 0 disables\n",
		program, program);
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


int
main(int argc, char** argv)
{
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
