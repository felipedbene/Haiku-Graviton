/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_BOOT_PLATFORM_KERNEL_ARGS_H
#define _BUILD_KERNEL_BOOT_PLATFORM_KERNEL_ARGS_H

#ifndef KERNEL_BOOT_KERNEL_ARGS_H
#	error This file is included from <boot/kernel_args.h> only
#endif


/*!	Host stand-in for the boot platform's <platform_kernel_args.h>.

	Nothing on the host is booted, so there are no kernel args to describe; the
	header is reached only because <smp.h> includes <boot/kernel_args.h> and
	code under test includes <smp.h> for the spinlock API. The sizes just have
	to exist -- the struct is never filled in.
*/

#define SMP_MAX_CPUS					1

#define MAX_PHYSICAL_MEMORY_RANGE		1
#define MAX_PHYSICAL_ALLOCATED_RANGE	1
#define MAX_VIRTUAL_ALLOCATED_RANGE		1

typedef struct {
	int		dummy;
} platform_kernel_args;


#endif	// _BUILD_KERNEL_BOOT_PLATFORM_KERNEL_ARGS_H
