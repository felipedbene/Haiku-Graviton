/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_CPU_H
#define _BUILD_KERNEL_ARCH_CPU_H


/*!	Host stand-in for the per-architecture <arch_cpu.h>.

	<arch/cpu.h> pulls this in for CACHE_LINE_SIZE and the per-CPU arch state,
	and the kernel-land emulation's smp.cpp includes <cpu.h> in turn. The host
	emulation reports a single CPU and never fills the struct in, so the only
	thing that has to be right is that cpu_ent stays a complete type.

	64 bytes is the cache line on both of the architectures DeBeOS builds on
	(arm64 and x86_64); it only affects alignment padding here, never layout
	shared with a target binary.
*/

#define CACHE_LINE_SIZE		64


typedef struct arch_cpu_info {
	int		dummy;
} arch_cpu_info;


#endif	// _BUILD_KERNEL_ARCH_CPU_H
