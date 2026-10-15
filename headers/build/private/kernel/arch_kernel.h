/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_KERNEL_H
#define _BUILD_KERNEL_ARCH_KERNEL_H


/*!	Host stand-in for the per-architecture <arch_kernel.h>.

	Only the address-space layout constants are needed, and in a host build
	there is one flat user address space. KERNEL_BASE is therefore set past
	everything the process can address, so that IS_KERNEL_ADDRESS() is false
	and IS_USER_ADDRESS() true for every pointer the code under test handles --
	which is what is actually the case here.
*/

#define KERNEL_BASE		0xffffffffffffe000ULL
#define KERNEL_SIZE		0x1000ULL
#define KERNEL_TOP		(KERNEL_BASE + (KERNEL_SIZE - 1))

#define USER_BASE		0x1000
#define USER_BASE_ANY	USER_BASE
#define USER_SIZE		(KERNEL_BASE - USER_BASE)
#define USER_TOP		(KERNEL_BASE - 1)


#endif	// _BUILD_KERNEL_ARCH_KERNEL_H
