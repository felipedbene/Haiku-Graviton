/*
 * Copyright 2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


//! Empty main temporary directory


#include "InitTemporaryDirectoryJob.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <FindDirectory.h>
#include <Path.h>


InitTemporaryDirectoryJob::InitTemporaryDirectoryJob()
	:
	AbstractEmptyDirectoryJob("init /tmp")
{
}


status_t
InitTemporaryDirectoryJob::Execute()
{
	// TODO: the /tmp entries could be scanned synchronously, and deleted
	// later
	BPath path;
	status_t status = find_directory(B_SYSTEM_TEMP_DIRECTORY, &path, true);
	if (status != B_OK)
		return status;

	status = CreateAndEmpty(path.Path());

	// Everybody may create files in /tmp, but with the sticky bit set only
	// their owner (or root) may remove or replace them again, as on any
	// other POSIX system. Programs check for exactly this before they trust
	// the directory, so it is set even when emptying it failed, and the
	// mode is reapplied on every boot to repair an older image's 0777.
	if (chmod(path.Path(), S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO) != 0) {
		status_t chmodStatus = errno;
		fprintf(stderr, "Cannot make \"%s\" sticky: %s\n", path.Path(),
			strerror(chmodStatus));
		if (status == B_OK)
			status = chmodStatus;
	}

	return status;
}
