/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_ELF_H
#define _BUILD_KERNEL_ARCH_ELF_H


/*!	Host stand-in for the per-architecture <arch_elf.h>.

	The target versions hold nothing but that architecture's relocation
	constants. Nothing a host build links against resolves relocations; the
	header is reached only because <util/Random.h> pulls in <smp.h> and from
	there the kernel args and the ELF loader's private header.
*/

#define TLS_DTV_OFFSET	0


#endif	// _BUILD_KERNEL_ARCH_ELF_H
