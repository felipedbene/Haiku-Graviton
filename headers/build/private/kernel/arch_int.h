/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_INT_H
#define _BUILD_KERNEL_ARCH_INT_H


/*!	Host stand-in for the per-architecture <arch_int.h>.

	A host build has no interrupts to mask, and the target's version is written
	in the target's assembly ("msr daifset" on arm64), so including it here is
	not an option. Kernel code that wraps a critical section in
	InterruptsSpinLocker still compiles and still takes the lock; only the
	masking, which protects against an interrupt handler that does not exist in
	a user process, becomes a no-op.
*/

#define NUM_IO_VECTORS	256

#define arch_int_enable_interrupts()		do {} while (0)
#define arch_int_disable_interrupts()		0
#define arch_int_restore_interrupts(status)	do { (void)(status); } while (0)
#define arch_int_are_interrupts_enabled()	true


#endif	// _BUILD_KERNEL_ARCH_INT_H
