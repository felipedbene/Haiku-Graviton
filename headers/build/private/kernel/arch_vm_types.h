/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_VM_TYPES_H
#define _BUILD_KERNEL_ARCH_VM_TYPES_H


/*!	Host stand-in for the per-architecture <arch_vm_types.h>.

	Nothing is needed here -- arm64's is empty too -- but <arch/vm_types.h>
	includes it unconditionally, so the file has to exist for the host include
	path to resolve the chain <vm/VMArea.h> pulls in.
*/


#endif	// _BUILD_KERNEL_ARCH_VM_TYPES_H
