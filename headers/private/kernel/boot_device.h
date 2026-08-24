/*
 * Copyright 2005-2009, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_BOOT_DEVICE_H
#define _KERNEL_BOOT_DEVICE_H


#include <sys/types.h>


extern dev_t gBootDevice;
extern bool gReadOnlyBootDevice;
	// defined in fs/vfs_boot.cpp


/*!	Whether the boot volume has been mounted yet.

	gBootDevice is initialised to -1 and assigned the boot volume's mount ID
	once it is mounted, so the question is whether it is negative. That test
	used to be open coded at each of its callers, and they did not all agree:
	some read "> 0" and some read ">= 0". Mount IDs presently start at 1, so
	the two behave identically today -- but they ask one question and must not
	be able to answer it differently, particularly since the callers that
	disagreed are the module search path walk and the devfs driver scan that
	depends on it.
*/
static inline bool
has_boot_device()
{
	return gBootDevice >= 0;
}

#endif	/* _KERNEL_BOOT_DEVICE_H */
