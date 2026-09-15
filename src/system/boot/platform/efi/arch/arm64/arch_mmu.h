/*
 * Copyright 2021-2022 Haiku, Inc. All rights reserved.
 * Released under the terms of the MIT License.
 */

#ifndef _ARM64_ARCH_MMU_H
#define _ARM64_ARCH_MMU_H


/*
 * Quotes taken from:
 * Arm(C) Architecture Reference Manual
 * Armv8, for Armv8-A architecture profile
 * Chapter: D5.3 VMSAv8-64 translation table format descriptors
 */
class ARMv8TranslationTableDescriptor {

	/* Descriptor bit[0] identifies whether the descriptor is valid,
	 * and is 1 for a valid descriptor. If a lookup returns an invalid
	 * descriptor, the associated input address is unmapped, and any
	 * attempt to access it generates a Translation fault.
	 *
	 * Descriptor bit[1] identifies the descriptor type, and is encoded as:
	 * 0, Block The descriptor gives the base address of a block of memory,
	 * and the attributes for that memory region.
	 * 1, Table The descriptor gives the address of the next level of
	 * translation table, and for a stage 1 translation, some attributes for
	 * that translation.
	 */

	static constexpr uint64_t kTypeMask = 0x3u;

	static constexpr uint64_t kTypeInvalid = 0x0u;
	static constexpr uint64_t kTypeBlock = 0x1u;
	static constexpr uint64_t kTypeTable = 0x3u;
	static constexpr uint64_t kTypePage = 0x3u;

public:
	/* Descriptor bit[52] is the Contiguous bit. When a naturally aligned run
	 * of block or page descriptors all set it, the TLB may cache the run as a
	 * single entry, cutting TLB pressure on large mappings. At the 4KB granule
	 * this port uses the run is 16 consecutive entries at every level that maps
	 * memory (64KB at L3, 32MB at L2, 16GB at L1). The architecture leaves the
	 * behaviour UNPREDICTABLE unless every entry in the run is present, has a
	 * contiguous output address, carries identical attributes, and the run is
	 * aligned to its total size. Critically, that uniformity has to hold for
	 * the whole life of the mapping: if any single member is later given
	 * different attributes (a sub-range re-protect) or replaced by a finer
	 * table (a split), the group is silently corrupt. The loader therefore only
	 * sets it on the linear physical map -- a permanent, uniformly attributed,
	 * kernel-global mapping the runtime never re-protects or sub-maps -- and
	 * never on the kernel image, whose sections are re-protected per-range.
	 */
	static constexpr uint64_t kContiguousBit = (1UL << 52);

private:

	// TODO: Place TABLE PAGE BLOCK prefixes accordingly
	struct UpperAttributes {
		static constexpr uint64_t TABLE_PXN	= (1UL << 59);
		static constexpr uint64_t TABLE_XN	= (1UL << 60);
		static constexpr uint64_t TABLE_AP	= (1UL << 61);
		static constexpr uint64_t TABLE_NS	= (1UL << 63);
		static constexpr uint64_t BLOCK_PXN	= (1UL << 53);
		static constexpr uint64_t BLOCK_UXN	= (1UL << 54);
	};

	struct LowerAttributes {
		static constexpr uint64_t BLOCK_NS			= (1 << 5);
		static constexpr uint64_t BLOCK_NON_SHARE	= (0 << 8);
		static constexpr uint64_t BLOCK_OUTER_SHARE	= (2 << 8);
		static constexpr uint64_t BLOCK_INNER_SHARE	= (3 << 8);
		static constexpr uint64_t BLOCK_AF			= (1UL << 10);
		static constexpr uint64_t BLOCK_NG			= (1UL << 11);
	};

public:

	static constexpr uint64 DefaultPeripheralAttribute = LowerAttributes::BLOCK_AF
		| LowerAttributes::BLOCK_NON_SHARE
		| UpperAttributes::BLOCK_PXN
		| UpperAttributes::BLOCK_UXN;

	static constexpr uint64 DefaultCodeAttribute = LowerAttributes::BLOCK_AF
		| LowerAttributes::BLOCK_INNER_SHARE;

	ARMv8TranslationTableDescriptor(uint64_t* descriptor)
		: fDescriptor(descriptor)
	{}

	ARMv8TranslationTableDescriptor(uint64_t descriptor)
		: fDescriptor(reinterpret_cast<uint64_t*>(descriptor))
	{}

	bool IsInvalid() {
		return (*fDescriptor & kTypeMask) == kTypeInvalid;
	}

	bool IsBlock() {
		return (*fDescriptor & kTypeMask) == kTypeBlock;
	}

	bool IsPage() {
		return (*fDescriptor & kTypeMask) == kTypePage;
	}

	bool IsTable() {
		return (*fDescriptor & kTypeMask) == kTypeTable;
	}


	uint64_t* Dereference() {
		if (IsTable())
			// TODO: Use ATTR_MASK
			return reinterpret_cast<uint64_t*>((*fDescriptor) & 0x0000fffffffff000ULL);
		else
			return NULL;
	}

	void SetToTable(uint64* descriptor, uint64_t attributes) {
		*fDescriptor = reinterpret_cast<uint64_t>(descriptor) | kTypeTable;
	}

	void SetAsPage(uint64_t* physical, uint64_t attributes) {
		*fDescriptor = CleanAttributes(reinterpret_cast<uint64_t>(physical)) | attributes | kTypePage;
	}

	void SetAsBlock(uint64_t* physical, uint64_t attributes) {
		*fDescriptor = CleanAttributes(reinterpret_cast<uint64_t>(physical)) | attributes | kTypeBlock;
	}

	void Next() {
		fDescriptor++;
	}

	void JumpTo(uint16 slot) {
		fDescriptor += slot;
	}

	uint64 Value() {
		return *fDescriptor;
	}

	uint64 Location() {
		return reinterpret_cast<uint64_t>(fDescriptor);
	}

private:

	static uint64 CleanAttributes(uint64 address) {
		return address & ~ATTR_MASK;
	}

	uint64_t* fDescriptor;
};


class MemoryAttributeIndirection {
public:
	uint8 IndexOf(uint8 requirement) {
		uint64 processedMair = MAIR_VALUE;
		uint8 index = 0;

		while (((processedMair & 0xFF) != requirement) && (index < 8)) {
			index++;
			processedMair = (processedMair >> 8);
		}

		return (index < 8)?index:0xff;
	}


	uint64 MaskOf(uint8 requirement) {
		return IndexOf(requirement) << 2;
	}
};


class ARMv8TranslationRegime {

	static const uint8 skTranslationLevels = 4;
public:

	struct TranslationLevel {
		uint8 shift;
		uint64 mask;
		bool blocks;
		bool tables;
		bool pages;
	};

	typedef struct TranslationLevel TranslationDescriptor[skTranslationLevels];

	ARMv8TranslationRegime(TranslationDescriptor& regime)
		: fRegime(regime)
	{}

	uint16 DescriptorIndex(addr_t virt_addr, uint8 level) {
		return (virt_addr >> fRegime[level].shift) & fRegime[level].mask;
	}

	bool BlocksAllowed(uint8 level) {
		return fRegime[level].blocks;
	}

	bool TablesAllowed(uint8 level) {
		return fRegime[level].tables;
	}

	bool PagesAllowed(uint8 level) {
		return fRegime[level].pages;
	}

	// The Contiguous bit is meaningful at any level that maps memory directly
	// (a block level or the page level), never at a pure table level.
	bool ContiguousAllowed(uint8 level) {
		return fRegime[level].blocks || fRegime[level].pages;
	}

	// Number of consecutive descriptors that form a Contiguous run. The
	// architecture fixes this per granule; only the 4KB granule this port uses
	// has a single uniform value across levels (16). For other granules the
	// length differs by level, so return 1 (a "run" that sets no Contiguous
	// bit) rather than risk emitting an UNPREDICTABLE mismatched group.
	uint32 ContiguousCount() {
		return (Granularity() == 0x1000) ? 16 : 1;
	}

	uint64 Mask(uint8 level) {
		return EntrySize(level) - 1;
	}

	bool Aligned(addr_t address, uint8 level) {
		return (address & Mask(level)) == 0;
	}

	uint64 EntrySize(uint8 level) {
		return 1ul << fRegime[level].shift;
	}

	uint64 TableSize(uint8 level) {
		return EntrySize(level) * arch_mmu_entries_per_granularity(Granularity());
	}

	uint64* AllocatePage(void) {
		uint64 size = Granularity();
		uint64* page = NULL;
#if 0
		// BUG: allocation here overlaps assigned memory ...
		if (platform_allocate_region((void **)&page, size, 0) == B_OK) {
#else
		// TODO: luckly size == B_PAGE_SIZE == 4KB ...
		page = reinterpret_cast<uint64*>(mmu_allocate_page());
		if (page != NULL) {
#endif
			memset(page, 0, size);
			if ((reinterpret_cast<uint64>(page) & (size - 1)) != 0) {
				panic("Memory requested not %lx aligned\n", size - 1);
			}
			return page;
		} else {
			panic("Unavalable memory for descriptors\n");
			return NULL;
		}
	}

	uint8 MaxLevels() {
		return skTranslationLevels;
	}

	uint64 Granularity() {
		// Size of the last level ...
		return EntrySize(skTranslationLevels - 1);
	}

private:
	TranslationDescriptor& fRegime;
};

#endif /* _ARM64_ARCH_MMU_H */
