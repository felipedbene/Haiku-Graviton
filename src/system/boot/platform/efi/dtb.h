/*
 * Copyright 2019-2020, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef DTB_H
#define DTB_H

#ifndef _ASSEMBLER

#include "efi_platform.h"

#include <util/FixedWidthPointer.h>


extern void dtb_init();
extern void dtb_set_kernel_args();

bool dtb_get_reg(const void* fdt, int node, size_t idx, addr_range& range);
uint32 dtb_get_interrupt(const void* fdt, int node);
bool dtb_get_interrupt_at(const void* fdt, int node, uint32 index,
	uint32& interrupt);
bool dtb_has_fdt_string(const char* prop, int size, const char* pattern);
// Walk a "compatible" property's entries in order; see dtb.cpp for why order
// matters. NULL `cur` yields the first entry, NULL return ends the walk.
const char* dtb_next_fdt_string(const char* prop, int size, const char* cur);


#endif /* !_ASSEMBLER */

#endif /* DTB_H */
