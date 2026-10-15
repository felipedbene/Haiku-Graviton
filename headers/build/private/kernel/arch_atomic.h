/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_ATOMIC_H
#define _BUILD_KERNEL_ARCH_ATOMIC_H


/*!	Host stand-in for <arch_atomic.h>, which the real kernel supplies per
	architecture.

	Kernel code compiled for the build host must not pull in the target's
	version: that one speaks the target ISA (arm64 emits "dmb ishld" and
	friends), so it either fails to assemble or silently pins the host build to
	one architecture. The compiler's own fences are the portable equivalent and
	compile to the same instructions where they are needed at all.
*/

static inline void
memory_read_barrier_inline(void)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
}


static inline void
memory_write_barrier_inline(void)
{
	__atomic_thread_fence(__ATOMIC_RELEASE);
}


static inline void
memory_full_barrier_inline(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}


#define memory_read_barrier		memory_read_barrier_inline
#define memory_write_barrier	memory_write_barrier_inline
#define memory_full_barrier		memory_full_barrier_inline


#endif	// _BUILD_KERNEL_ARCH_ATOMIC_H
