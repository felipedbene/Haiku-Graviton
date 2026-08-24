/*
 * Copyright 2018, Jaroslaw Pelczar <jarek@jpelczar.com>
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_ARM64_ARCH_DEBUG_H_
#define _KERNEL_ARCH_ARM64_ARCH_DEBUG_H_


#include <SupportDefs.h>


struct arch_debug_registers {
	// Frame pointer (x29) of whatever was running on this CPU when it entered
	// the kernel debugger. Needed to trace a thread that is running on another
	// CPU: its arch_info.regs[] are stale in that case, because they are only
	// written by _arch_context_swap() when it is switched out.
	addr_t	fp;
};


#endif /* _KERNEL_ARCH_ARM64_ARCH_DEBUG_H_ */
