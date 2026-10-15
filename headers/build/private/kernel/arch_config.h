/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_CONFIG_H
#define _BUILD_KERNEL_ARCH_CONFIG_H


/*!	Host stand-in for the per-architecture <arch_config.h>.

	The target versions state the calling convention's parameter alignment and
	which way the stack grows. Every host DeBeOS builds on matches the arm64
	target in both respects, and nothing compiled for the host walks a stack
	frame anyway.
*/

#define FUNCTION_CALL_PARAMETER_ALIGNMENT_TYPE	unsigned long

#define STACK_GROWS_DOWNWARDS


#endif	// _BUILD_KERNEL_ARCH_CONFIG_H
