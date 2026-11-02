/*
 * Copyright 2002-2009, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include <utime.h>

#include <errno.h>
#include <time.h>

#include <fs_interface.h>
#include <NodeMonitor.h>

#include <errno_private.h>
#include <syscalls.h>
#include <syscall_utils.h>


int
utime(const char *path, const struct utimbuf *times)
{
	struct stat stat;
	status_t status;
	uint32 mask = B_STAT_MODIFICATION_TIME | B_STAT_ACCESS_TIME;

	if (times != NULL) {
		stat.st_atim.tv_sec = times->actime;
		stat.st_mtim.tv_sec = times->modtime;
		stat.st_atim.tv_nsec = stat.st_mtim.tv_nsec = 0;
	} else {
		bigtime_t now = real_time_clock_usecs();
		stat.st_atim.tv_sec = stat.st_mtim.tv_sec = now / 1000000;
		stat.st_atim.tv_nsec = stat.st_mtim.tv_nsec = (now % 1000000) * 1000;
		mask |= B_STAT_TIMES_NOW;
			// write access suffices for "now", see check_write_stat_permissions()
	}

	status = _kern_write_stat(AT_FDCWD, path, true, &stat, sizeof(struct stat),
		mask);

	RETURN_AND_SET_ERRNO(status);
}

