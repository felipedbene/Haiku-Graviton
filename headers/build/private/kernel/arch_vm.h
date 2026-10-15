/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_VM_H
#define _BUILD_KERNEL_ARCH_VM_H


/*!	Host stand-in for the per-architecture <arch_vm.h>.

	<arch/vm.h> includes this for PAGE_SHIFT and declares the arch_vm_* entry
	points; <vm/vm_page.h> pulls the chain in, which is how the kernel-land
	emulation's vm.cpp reaches it. Host-built code has no address spaces to
	manage, so none of the arch_vm_* functions is ever called or defined.

	PAGE_SHIFT only feeds page-count arithmetic in the emulation (which reports
	a fixed amount of memory); 12 matches every architecture DeBeOS targets with
	a 4K granule and is never shared with a target binary.
*/

#define PAGE_SHIFT		12


#endif	// _BUILD_KERNEL_ARCH_VM_H
