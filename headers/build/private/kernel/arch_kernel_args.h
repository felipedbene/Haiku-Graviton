/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_KERNEL_ARGS_H
#define _BUILD_KERNEL_ARCH_KERNEL_ARGS_H

#ifndef KERNEL_BOOT_KERNEL_ARGS_H
#	error This file is included from <boot/kernel_args.h> only
#endif


/*!	Host stand-in for the per-architecture <arch_kernel_args.h>.

	Same reason as <platform_kernel_args.h>: <boot/kernel_args.h> is pulled in
	by <smp.h>, and the struct only has to have a size. The target version
	describes a page directory and the firmware handoff, which a host build
	has no business knowing about.
*/

#define MAX_VIRTUAL_RANGES_TO_KEEP	1

typedef struct {
	int		dummy;
} arch_kernel_args;


#endif	// _BUILD_KERNEL_ARCH_KERNEL_ARGS_H
