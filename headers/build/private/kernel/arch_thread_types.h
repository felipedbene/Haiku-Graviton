/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_THREAD_TYPES_H
#define _BUILD_KERNEL_ARCH_THREAD_TYPES_H


/*!	Host stand-in for the per-architecture <arch_thread_types.h>.

	<thread_types.h> embeds these by value in Thread and Team, so they have to
	exist; a host build never saves or restores a register set, so they are
	empty. They are not the real thread state either way: on the host the
	kernel-land emulation maps a Thread* onto a host thread id.
*/

struct iframe_stack {
	int		index;
};

struct arch_thread {
};

struct arch_team {
};

struct arch_fork_arg {
};


#endif	// _BUILD_KERNEL_ARCH_THREAD_TYPES_H
