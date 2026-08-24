/*
 * Copyright 2007 Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * arch-specific config manager
 *
 * Authors (in chronological order):
 *              François Revol (revol@free.fr)
 */


#include <KernelExport.h>
#include "ISA.h"
#include "arch_cpu.h"
#include "isa_arch.h"


/*!	arm64 has no ISA bus and no port IO space: there is no instruction that
	addresses one, and no window that stands in for one. This file was a
	verbatim copy of the x86 implementation, calling the in8()/out8() family
	that only the x86 arch headers define, so it had never compiled -- and had
	it been made to compile by supplying those, every accessor would have
	dereferenced a small integer as an address and faulted.

	So the accessors do nothing and arch_isa_init() declines. isa.cpp returns
	arch_isa_init() from B_MODULE_INIT, so declining is what keeps the bus
	manager from publishing an ISA bus that is not there and from handing these
	accessors to a driver. Should an arm64 machine ever reach an ISA bridge
	through a memory window, this is the file that maps it and stops declining.
*/


uint8
arch_isa_read_io_8(int mapped_io_addr)
{
	return 0;
}


void
arch_isa_write_io_8(int mapped_io_addr, uint8 value)
{
}


uint16
arch_isa_read_io_16(int mapped_io_addr)
{
	return 0;
}


void
arch_isa_write_io_16(int mapped_io_addr, uint16 value)
{
}


uint32
arch_isa_read_io_32(int mapped_io_addr)
{
	return 0;
}


void
arch_isa_write_io_32(int mapped_io_addr, uint32 value)
{
}


phys_addr_t
arch_isa_ram_address(phys_addr_t physical_address_in_system_memory)
{
	// this is what the BeOS kernel does
	return physical_address_in_system_memory;
}


status_t
arch_isa_init(void)
{
	return B_NOT_SUPPORTED;
}
