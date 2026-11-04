/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include <arch/vm_translation_map.h>
#include <boot/kernel_args.h>
#include <string.h>
#include <vm/VMAddressSpace.h>
#include <vm/vm.h>
#include <vm/vm_page.h>

#include "PMAPPhysicalPageMapper.h"
#include "VMSAv8TranslationMap.h"
#include "arm_registers.h"

static char sPhysicalPageMapperData[sizeof(PMAPPhysicalPageMapper)];

// Physical pointer to an empty page table, which is used for break-before-make
// when updating TTBR0_EL1.
static phys_addr_t sEmptyTable;



void
arch_vm_install_empty_table_ttbr0(void)
{
	WRITE_SPECIALREG(TTBR0_EL1, sEmptyTable);
	asm("isb");
}


status_t
arch_vm_translation_map_create_map(bool kernel, VMTranslationMap** _map)
{
	phys_addr_t pt = 0;
	if (kernel) {
		pt = READ_SPECIALREG(TTBR1_EL1) & kTtbrBasePhysAddrMask;
		arch_vm_install_empty_table_ttbr0();
	}

	*_map = new (std::nothrow) VMSAv8TranslationMap(kernel, pt, 12, 48, 1);

	if (*_map == NULL)
		return B_NO_MEMORY;

	return B_OK;
}


// Replace the EFI loader's records of the physical memory ranges it mapped at
// KERNEL_PMAP_BASE with one record covering the whole window. Those records are
// kernel addresses, so reserve_boot_loader_ranges() reserves them; reserved in
// one piece, the window leaves no hole between RAM ranges for an area created
// before arch_vm_translation_map_init_post_area() -- a debug heap's, say -- to
// land in and keep the "physical map area" from being created.
static void
reserve_physical_map_window(kernel_args* args)
{
	const addr_t windowEnd = KERNEL_PMAP_BASE + (KERNEL_PMAP_SIZE - 1);
	addr_range* ranges = args->virtual_allocated_range;

	uint32 count = 0;
	uint32 insertIndex = UINT32_MAX;
	for (uint32 i = 0; i < args->num_virtual_allocated_ranges; i++) {
		const addr_range range = ranges[i];
		if (range.size == 0)
			continue;
		const addr_t rangeEnd = range.start + (range.size - 1);
		if (range.start >= KERNEL_PMAP_BASE && rangeEnd <= windowEnd)
			continue;
		if (range.start <= windowEnd && rangeEnd >= KERNEL_PMAP_BASE) {
			panic("boot loader range %#" B_PRIx64 " - %#" B_PRIx64 " straddles the "
				"physical map window", range.start, (uint64)rangeEnd);
		}

		if (insertIndex == UINT32_MAX && range.start > windowEnd)
			insertIndex = count;
		ranges[count++] = range;
	}

	if (count >= MAX_VIRTUAL_ALLOCATED_RANGE)
		panic("no room to record the physical map window in the kernel args");
	if (insertIndex == UINT32_MAX)
		insertIndex = count;

	memmove(&ranges[insertIndex + 1], &ranges[insertIndex],
		(count - insertIndex) * sizeof(addr_range));
	ranges[insertIndex].start = KERNEL_PMAP_BASE;
	ranges[insertIndex].size = KERNEL_PMAP_SIZE;
	args->num_virtual_allocated_ranges = count + 1;
}


status_t
arch_vm_translation_map_init(kernel_args* args, VMPhysicalPageMapper** _physicalPageMapper)
{
	reserve_physical_map_window(args);

	// nuke TTBR0 mapping, we use identity mapping in kernel space at KERNEL_PMAP_BASE
	memset((void*) (KERNEL_PMAP_BASE + READ_SPECIALREG(TTBR0_EL1)), 0, B_PAGE_SIZE);

	uint64_t mair = READ_SPECIALREG(MAIR_EL1);
	uint64_t mmfr1 = READ_SPECIALREG(ID_AA64MMFR1_EL1);
	uint64_t mmfr2 = READ_SPECIALREG(ID_AA64MMFR2_EL1);

	ASSERT(VMSAv8TranslationMap::fHwFeature == 0);
	uint64_t hafdbs = ID_AA64MMFR1_HAFDBS(mmfr1);
	if (hafdbs == ID_AA64MMFR1_HAFDBS_AF) {
		VMSAv8TranslationMap::fHwFeature = VMSAv8TranslationMap::HW_ACCESS;
	}
	if (hafdbs == ID_AA64MMFR1_HAFDBS_AF_DBS) {
		VMSAv8TranslationMap::fHwFeature
			= VMSAv8TranslationMap::HW_ACCESS | VMSAv8TranslationMap::HW_DIRTY;
	}
	if (ID_AA64MMFR2_CNP(mmfr2) == ID_AA64MMFR2_CNP_IMPL) {
		VMSAv8TranslationMap::fHwFeature |= VMSAv8TranslationMap::HW_COMMON_NOT_PRIVATE;
	}

	VMSAv8TranslationMap::fMair = mair;

	*_physicalPageMapper = new (&sPhysicalPageMapperData) PMAPPhysicalPageMapper();

	// Create an empty page table for use when we don't want a userspace page table.
	sEmptyTable = vm_allocate_early_physical_page(args) << PAGE_SHIFT;

	return B_OK;
}


status_t
arch_vm_translation_map_init_post_sem(kernel_args* args)
{
	dprintf("arch_vm_translation_map_init_post_sem\n");

	return B_OK;
}


status_t
arch_vm_translation_map_init_post_area(kernel_args* args)
{
	dprintf("arch_vm_translation_map_init_post_area\n");

	// reserve_boot_loader_ranges() has reserved the window (see
	// reserve_physical_map_window()). Drop the reservation and create the area
	// in its place; the area takes over keeping the window allocated, and
	// unreserve_boot_loader_ranges() later finds nothing left to unreserve.
	vm_unreserve_address_range(VMAddressSpace::KernelID(),
		(void*)KERNEL_PMAP_BASE, KERNEL_PMAP_SIZE);

	// Create an area covering the physical map area.
	void* address = (void*) KERNEL_PMAP_BASE;
	area_id area = vm_create_null_area(VMAddressSpace::KernelID(), "physical map area", &address,
		B_EXACT_ADDRESS, KERNEL_PMAP_SIZE, 0);
	if (area < 0) {
		panic("arch_vm_translation_map_init_post_area(): failed to create the physical map "
			"area: %s", strerror(area));
	}

	if (args->arch_args.uart.kind[0] != 0) {
		// debug uart is already mapped by the efi loader
		address = (void*)args->arch_args.uart.regs.start;
		area = vm_create_null_area(VMAddressSpace::KernelID(),
			"debug uart map area", &address, B_EXACT_ADDRESS,
			ROUNDUP(args->arch_args.uart.regs.size, B_PAGE_SIZE), 0);
		if (area < 0) {
			dprintf("arch_vm_translation_map_init_post_area(): failed to create the debug uart "
				"map area: %s\n", strerror(area));
		}
	}

	return B_OK;
}

// TODO: reuse some bits from VMSAv8TranslationMap

static uint64_t page_bits = 12;
static uint64_t tsz = 16;


static uint64_t*
TableFromPa(phys_addr_t pa)
{
	return reinterpret_cast<uint64_t*>(KERNEL_PMAP_BASE + pa);
}


static void
map_page_early(phys_addr_t ptPa, int level, addr_t va, phys_addr_t pa,
	kernel_args* args)
{
	int tableBits = page_bits - 3;
	uint64_t tableMask = (1UL << tableBits) - 1;

	int shift = tableBits * (3 - level) + page_bits;

	int index = (va >> shift) & tableMask;
	uint64_t* pte = &TableFromPa(ptPa)[index];

	if (level == 3) {
		atomic_set64((int64*) pte, pa | 0x3);
		asm("dsb ish");
	} else {
		uint64_t pteVal = atomic_get64((int64*) pte);
		int type = pteVal & 0x3;

		phys_addr_t table;
		if (type == 0x3) {
			table = pteVal & kPteAddrMask;
		} else {
			table = vm_allocate_early_physical_page(args) << page_bits;
			dprintf("early: pulling page %lx\n", table);
			uint64_t* newTableVa = TableFromPa(table);

			if (type == 0x1) {
				int shift = tableBits * (3 - (level + 1)) + page_bits;
				int entrySize = 1UL << shift;

				for (int i = 0; i < (1 << tableBits); i++)
					newTableVa[i] = pteVal + i * entrySize;
			} else {
				memset(newTableVa, 0, 1 << page_bits);
			}

			asm("dsb ish");

			atomic_set64((int64*) pte, table | 0x3);
		}

		map_page_early(table, level + 1, va, pa, args);
	}
}


status_t
arch_vm_translation_map_early_map(kernel_args* args, addr_t va, phys_addr_t pa, uint8 attributes)
{
	int va_bits = 64 - tsz;
	uint64_t va_mask = (1UL << va_bits) - 1;
	ASSERT((va & ~va_mask) == ~va_mask);

	phys_addr_t ptPa = READ_SPECIALREG(TTBR1_EL1) & kTtbrBasePhysAddrMask;
	int level = VMSAv8TranslationMap::CalcStartLevel(va_bits, page_bits);
	va &= va_mask;
	pa |= VMSAv8TranslationMap::GetMemoryAttr(attributes, 0, true);

	map_page_early(ptPa, level, va, pa, args);

	return B_OK;
}


bool
arch_vm_translation_map_is_kernel_page_accessible(addr_t va, uint32 protection)
{
	if (protection & B_KERNEL_WRITE_AREA) {
		asm("at s1e1w, %0" : : "r"((uint64_t) va));
		asm volatile("isb");
		return (READ_SPECIALREG(PAR_EL1) & PAR_F) == 0;
	} else {
		asm("at s1e1r, %0" : : "r"((uint64_t) va));
		asm volatile("isb");
		return (READ_SPECIALREG(PAR_EL1) & PAR_F) == 0;
	}
}
