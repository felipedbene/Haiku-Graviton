/*
 * Copyright 2004-2009, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include <sys/stat.h>
#include <errno.h>

#include <errno_private.h>
#include <syscalls.h>
#include <umask.h>


mode_t __gUmask = 022;
	// this must be made available to open() and friends


mode_t
umask(mode_t newMask)
{
	mode_t oldMask = __gUmask;

	// Only the file permission bits of the mask are used (POSIX); keeping the
	// others would also hand them back from the next umask() call, and on to
	// every program exec'd from here.
	__gUmask = newMask & ACCESSPERMS;

	return oldMask;
}

