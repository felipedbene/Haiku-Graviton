/*
 * Copyright 2022 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include "VMSAv8TranslationMap.h"

#include <algorithm>
#include <slab/Slab.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMCache.h>
#include <vm/vm_page.h>
#include <vm/vm_priv.h>


//#define DO_TRACE
#ifdef DO_TRACE
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


uint32_t VMSAv8TranslationMap::fHwFeature;
uint64_t VMSAv8TranslationMap::fMair;

// ASID Management
static constexpr size_t kAsidBits = 8;
static constexpr size_t kNumAsids = (1 << kAsidBits);
static spinlock sAsidLock = B_SPINLOCK_INITIALIZER;
// A bitmap to track which ASIDs are in use.
static uint64 sAsidBitMap[kNumAsids / 64] = {};
// A mapping from ASID to translation map.
static VMSAv8TranslationMap* sAsidMapping[kNumAsids] = {};


static void
free_asid(size_t asid)
{
	for (size_t i = 0; i < B_COUNT_OF(sAsidBitMap); ++i) {
		if (asid < 64) {
			sAsidBitMap[i] &= ~(uint64_t{1} << asid);
			return;
		}
		asid -= 64;
	}

	panic("Could not free ASID!");
}


static void
flush_tlb_whole_asid(uint64_t asid)
{
	asm("dsb ishst");
	asm("tlbi aside1is, %0" ::"r"(asid << 48));
	asm("dsb ish");
	asm("isb");
}


static size_t
alloc_first_free_asid(void)
{
	int asid = 0;
	for (size_t i = 0; i < B_COUNT_OF(sAsidBitMap); ++i) {
		int avail = __builtin_ffsll(~sAsidBitMap[i]);
		if (avail != 0) {
			sAsidBitMap[i] |= (uint64_t{1} << (avail-1));
			asid += (avail - 1);
			return asid;
		}
		asid += 64;
	}

	return kNumAsids;
}


static bool
is_pte_dirty(uint64_t pte)
{
	// An invalid entry has no dirty state. Without this test a zero PTE reads
	// as dirty, because "not read-only" is how a writable-and-written entry is
	// encoded. is_pte_accessed() below guards the same way.
	if ((pte & kPteValidMask) == 0)
		return false;

	if ((pte & kAttrSWDIRTY) != 0)
		return true;

	return (pte & kAttrAPReadOnly) == 0;
}


static uint64_t
set_pte_dirty(uint64_t pte)
{
	if ((pte & kAttrSWDBM) != 0)
		return pte & ~kAttrAPReadOnly;

	return pte | kAttrSWDIRTY;
}


static uint64_t
set_pte_clean(uint64_t pte)
{
	pte &= ~kAttrSWDIRTY;
	return pte | kAttrAPReadOnly;
}


static bool
is_pte_accessed(uint64_t pte)
{
	return (pte & kPteValidMask) != 0 && (pte & kAttrAF) != 0;
}


VMSAv8TranslationMap::VMSAv8TranslationMap(
	bool kernel, phys_addr_t pageTable, int pageBits, int vaBits, int minBlockLevel)
	:
	fIsKernel(kernel),
	fPageTable(pageTable),
	fPageBits(pageBits),
	fVaBits(vaBits),
	fMinBlockLevel(minBlockLevel),
	fASID(kernel ? 0 : -1),
	fRefcount(0)
{
	TRACE("+VMSAv8TranslationMap(%p, %d, 0x%" B_PRIxADDR ", %d, %d, %d)\n", this,
		kernel, pageTable, pageBits, vaBits, minBlockLevel);

	fInitialLevel = CalcStartLevel(fVaBits, fPageBits);
}


VMSAv8TranslationMap::~VMSAv8TranslationMap()
{
	TRACE("-VMSAv8TranslationMap(%p)\n", this);
	TRACE("  fIsKernel: %d, fPageTable: 0x%" B_PRIxADDR ", fASID: %d, fRefcount: %d\n",
		fIsKernel, fPageTable, fASID, fRefcount);

	ASSERT(!fIsKernel);
	ASSERT(fRefcount == 0);

	// Retire the ASID before freeing the translation tables. We must not hold
	// sAsidLock across the latter: it disables interrupts, while FreeTable()
	// frees pages, and vm_page_free_etc() takes an rw_lock (and
	// vm_page_unreserve_pages() a mutex) that may block.
	//
	// Invalidating the whole ASID here makes FreeTable()'s per-entry
	// invalidation unnecessary, so it no longer needs fASID to be stable: all
	// entries of a user map are non-global, and flush_va_if_accessed() is a
	// no-op for those once fASID is -1. Whoever gets this ASID next flushes it
	// wholesale in SwitchUserMap(). Flushing before clearing the entries is
	// safe because no CPU can have fPageTable installed in TTBR0 here:
	// fRefcount is 0, and taking sAsidLock serialises us against any
	// SwitchUserMap() still in flight.
	{
		InterruptsSpinLocker locker(sAsidLock);

		if (fASID != -1) {
			flush_tlb_whole_asid(fASID);
			sAsidMapping[fASID] = NULL;
			free_asid(fASID);
			fASID = -1;
		}
	}

	// The root table is allocated lazily by Map(), so it may not exist.
	if (fPageTable == 0)
		return;

	ThreadCPUPinner pinner(thread_get_current_thread());

	vm_page_reservation reservation = {};
	FreeTable(fPageTable, 0, fInitialLevel, &reservation);
	vm_page_unreserve_pages(&reservation);
}


// Switch user map into TTBR0.
// Passing kernel map here configures empty page table.
void
VMSAv8TranslationMap::SwitchUserMap(VMSAv8TranslationMap *from, VMSAv8TranslationMap *to)
{
	InterruptsSpinLocker locker(sAsidLock);

	if (!from->fIsKernel) {
		from->fRefcount--;
	}

	if (!to->fIsKernel) {
		to->fRefcount++;
	} else {
		arch_vm_install_empty_table_ttbr0();
		return;
	}

	ASSERT(to->fPageTable != 0);
	uint64_t ttbr = to->fPageTable | ((fHwFeature & HW_COMMON_NOT_PRIVATE) != 0 ? 1 : 0);

	if (to->fASID != -1) {
		WRITE_SPECIALREG(TTBR0_EL1, ((uint64_t)to->fASID << 48) | ttbr);
		asm("isb");
		return;
	}

	size_t allocatedAsid = alloc_first_free_asid();
	if (allocatedAsid != kNumAsids) {
		to->fASID = allocatedAsid;
		sAsidMapping[allocatedAsid] = to;

		WRITE_SPECIALREG(TTBR0_EL1, (allocatedAsid << 48) | ttbr);
		flush_tlb_whole_asid(allocatedAsid);
		return;
	}

	for (size_t i = 0; i < kNumAsids; ++i) {
		if (sAsidMapping[i]->fRefcount == 0) {
			sAsidMapping[i]->fASID = -1;
			to->fASID = i;
			sAsidMapping[i] = to;

			WRITE_SPECIALREG(TTBR0_EL1, (i << 48) | ttbr);
			flush_tlb_whole_asid(i);
			return;
		}
	}

	panic("cannot assign ASID");
}


int
VMSAv8TranslationMap::CalcStartLevel(int vaBits, int pageBits)
{
	int level = 4;

	int bitsLeft = vaBits - pageBits;
	while (bitsLeft > 0) {
		int tableBits = pageBits - 3;
		bitsLeft -= tableBits;
		level--;
	}

	ASSERT(level >= 0);

	return level;
}


bool
VMSAv8TranslationMap::Lock()
{
	TRACE("VMSAv8TranslationMap::Lock()\n");
	recursive_lock_lock(&fLock);
	return true;
}


void
VMSAv8TranslationMap::Unlock()
{
	TRACE("VMSAv8TranslationMap::Unlock()\n");
	recursive_lock_unlock(&fLock);
}


addr_t
VMSAv8TranslationMap::MappedSize() const
{
	panic("VMSAv8TranslationMap::MappedSize not implemented");
	return 0;
}


size_t
VMSAv8TranslationMap::MaxPagesNeededToMap(addr_t start, addr_t end) const
{
	constexpr uint64_t level3Range = B_PAGE_SIZE * 512;
	constexpr uint64_t level2Range = level3Range * 512;
	constexpr uint64_t level1Range = level2Range * 512;
	constexpr uint64_t level0Range = level1Range * 512;

	if (start == 0) {
		start = level0Range - B_PAGE_SIZE;
		end += start;
	}

	size_t requiredPages[] = {
		end / level0Range + 1 - start / level0Range,
		end / level1Range + 1 - start / level1Range,
		end / level2Range + 1 - start / level2Range,
		end / level3Range + 1 - start / level3Range
	};

	size_t ret = 0;
	for (int i = fInitialLevel; i < 4; ++i) {
		ret += requiredPages[i];
	}

	return ret;
}


uint64_t*
VMSAv8TranslationMap::TableFromPa(phys_addr_t pa)
{
	return reinterpret_cast<uint64_t*>(KERNEL_PMAP_BASE + pa);
}


void
VMSAv8TranslationMap::FreeTable(phys_addr_t ptPa, uint64_t va, int level,
	vm_page_reservation* reservation)
{
	ASSERT(level < 4);

	int tableBits = fPageBits - 3;
	uint64_t tableSize = 1UL << tableBits;
	uint64_t vaMask = (1UL << fVaBits) - 1;

	int shift = tableBits * (3 - level) + fPageBits;
	uint64_t entrySize = 1UL << shift;

	uint64_t nextVa = va;
	uint64_t* pt = TableFromPa(ptPa);
	for (uint64_t i = 0; i < tableSize; i++) {
		uint64_t oldPte = (uint64_t) atomic_get_and_set64((int64*) &pt[i], 0);

		if (level < 3 && (oldPte & kPteTypeMask) == kPteTypeL012Table) {
			FreeTable(oldPte & kPteAddrMask, nextVa, level + 1, reservation);
		} else if ((oldPte & kPteTypeMask) != 0) {
			uint64_t fullVa = (fIsKernel ? ~vaMask : 0) | nextVa;

			// Use this rather than FlushVAIfAccessed so that we don't have to
			// acquire sAsidLock for every entry.
			flush_va_if_accessed(oldPte, nextVa, fASID);
		}

		nextVa += entrySize;
	}

	vm_page* page = vm_lookup_page(ptPa >> fPageBits);
	DEBUG_PAGE_ACCESS_START(page);
	vm_page_free_etc(NULL, page, reservation);
}


// Make a new page sub-table.
// The parent table is `ptPa`, and the new sub-table's PTE will be at `index`
// in it.
// Returns the physical address of the new table, or the address of the existing
// one if the PTE is already filled.
phys_addr_t
VMSAv8TranslationMap::GetOrMakeTable(phys_addr_t ptPa, int level, int index,
	vm_page_reservation* reservation)
{
	ASSERT(level < 3);

	uint64_t* ptePtr = TableFromPa(ptPa) + index;
	uint64_t oldPte = atomic_get64((int64*) ptePtr);

	int type = oldPte & kPteTypeMask;

	if (type == kPteTypeL012Table) {
		// This is table entry already, just return it
		return oldPte & kPteAddrMask;
	}

	if (type == kPteTypeL12Block) {
		// A block descriptor covers this whole slot. To install anything finer
		// underneath it (the caller wants to descend), we must first break the
		// block into a next-level table describing the same output range. That
		// needs a page for the new table, so it is only possible on the mapping
		// path, which supplies a reservation. The read/teardown walkers pass
		// null: they never create finer mappings, so leaving the block intact
		// and reporting "no sub-table" is correct for them, and -- crucially --
		// avoids the previous behaviour of overwriting a live block with an
		// empty table *without* break-before-make, which is architecturally
		// illegal and silently corrupts the range the block mapped.
		if (reservation == nullptr)
			return 0;

		return SplitBlock(ptePtr, oldPte, level, reservation);
	}

	if (reservation != nullptr) {
		// Create new table there
		vm_page* page = vm_page_allocate_page(reservation, PAGE_STATE_WIRED | VM_PAGE_ALLOC_CLEAR);
		phys_addr_t newTablePa = page->physical_page_number << fPageBits;
		DEBUG_PAGE_ACCESS_END(page);

		// Ensure that writes to page being attached have completed
		asm("dsb ishst");

		uint64_t oldPteRefetch = (uint64_t)atomic_test_and_set64((int64*) ptePtr,
			newTablePa | kPteTypeL012Table, oldPte);
		if (oldPteRefetch != oldPte) {
			// If the old PTE has mutated, it must be because another thread has allocated the
			// sub-table at the same time as us. If that has happened, deallocate the page we
			// setup and use the one they installed instead.
			ASSERT((oldPteRefetch & kPteTypeMask) == kPteTypeL012Table);
			DEBUG_PAGE_ACCESS_START(page);
			vm_page_free_etc(NULL, page, reservation);
			return oldPteRefetch & kPteAddrMask;
		}

		return newTablePa;
	}

	// There's no existing table and we have no reservation
	return 0;
}


// Break a block descriptor at `level` into a next-level table that maps the
// same output range with the same attributes, and install it in place of the
// block. Returns the physical address of the new table.
//
// This is the "split a block back to pages" path required whenever a mapping
// finer than the block has to be made inside its span. At present the runtime
// map never *creates* blocks (Map() only ever installs level-3 pages), so the
// only blocks reachable here are the ones the EFI loader put in the shared
// TTBR1 tables (the linear physical map and other early regions). Those live in
// null areas the VM does not fault on, so this path is not expected to run yet
// -- but a future bulk-map path that emits blocks will depend on it, and having
// it correct means GetOrMakeTable() can never again silently clobber a block.
phys_addr_t
VMSAv8TranslationMap::SplitBlock(uint64_t* ptePtr, uint64_t blockPte, int level,
	vm_page_reservation* reservation)
{
	ASSERT((blockPte & kPteTypeMask) == kPteTypeL12Block);
	ASSERT(level < 3);
	ASSERT(reservation != nullptr);

	int tableBits = fPageBits - 3;
	int childLevel = level + 1;
	int childShift = tableBits * (3 - childLevel) + fPageBits;
	uint64_t childSize = 1UL << childShift;
	uint64_t childCount = 1UL << tableBits;

	// Every fragment inherits the block's output base and attributes. The
	// Contiguous bit is dropped: the block was a single TLB unit, its fragments
	// form a different (finer) grouping, and once the caller edits one fragment
	// a stale Contiguous hint spanning the run would be architecturally
	// UNPREDICTABLE.
	phys_addr_t basePa = blockPte & kPteAddrMask;
	uint64_t attr = blockPte & kPteAttrMask & ~kAttrContiguous;

	// A leaf that maps memory is a block at L1/L2 but a page at L3; the two
	// encodings differ only in the type field.
	uint64_t childType = (childLevel == 3) ? kPteTypeL3Page : kPteTypeL12Block;

	vm_page* page = vm_page_allocate_page(reservation, PAGE_STATE_WIRED | VM_PAGE_ALLOC_CLEAR);
	phys_addr_t newTablePa = page->physical_page_number << fPageBits;
	DEBUG_PAGE_ACCESS_END(page);

	uint64_t* newTable = TableFromPa(newTablePa);
	for (uint64_t i = 0; i < childCount; i++)
		newTable[i] = (basePa + i * childSize) | attr | childType;

	// Make the child table's contents visible to the page-table walker before
	// any descriptor can point at it.
	asm("dsb ishst");

	// Break-before-make. The block and the replacement table describe the same
	// VA range with different structure; the architecture forbids both being
	// live at once, so the block must be invalidated and evicted from the TLB
	// before the table is published. A block spans many pages and may be cached
	// as several TLB entries, so a single by-VA invalidation is insufficient --
	// flush the whole regime it belongs to. This is a rare, slow path, so the
	// broad flush is acceptable.
	atomic_set64((int64_t*)ptePtr, 0);
	asm("dsb ishst");

	if ((blockPte & kAttrNG) == 0) {
		// Global (kernel) entry: it can be cached under any ASID.
		asm("tlbi vmalle1is");
		asm("dsb ish");
		asm("isb");
	} else {
		// Non-global (user) entry: flush just this map's ASID. If it has none,
		// the map is not installed on any CPU and nothing can be cached.
		InterruptsSpinLocker locker(sAsidLock);
		if (fASID != -1)
			flush_tlb_whole_asid(fASID);
	}

	atomic_set64((int64_t*)ptePtr, newTablePa | kPteTypeL012Table);
	asm("dsb ishst");
	asm("isb");

	return newTablePa;
}


bool
flush_va_if_accessed(uint64_t pte, addr_t va, int asid)
{
	if (!is_pte_accessed(pte))
		return false;

	if ((pte & kAttrNG) == 0) {
		// Flush from all address spaces
		asm("dsb ishst"); // Ensure PTE write completed
		asm("tlbi vaae1is, %0" ::"r"(((va >> 12) & kTLBIMask)));
		asm("dsb ish");
		asm("isb");
	} else if (asid != -1) {
		asm("dsb ishst"); // Ensure PTE write completed
        asm("tlbi vae1is, %0" ::"r"(((va >> 12) & kTLBIMask) | (uint64_t(asid) << 48)));
		asm("dsb ish"); // Wait for TLB flush to complete
		asm("isb");
		return true;
	}

	return false;
}

bool
VMSAv8TranslationMap::FlushVAIfAccessed(uint64_t pte, addr_t va)
{
	InterruptsSpinLocker locker(sAsidLock);
	return flush_va_if_accessed(pte, va, fASID);
}


bool
VMSAv8TranslationMap::AttemptPteBreakBeforeMake(uint64_t* ptePtr, uint64_t oldPte, addr_t va)
{
	uint64_t loadedPte = atomic_test_and_set64((int64_t*)ptePtr, 0, oldPte);
	if (loadedPte != oldPte)
		return false;

	FlushVAIfAccessed(oldPte, va);

	return true;
}


template<typename UpdatePte>
void
VMSAv8TranslationMap::ProcessRange(phys_addr_t ptPa, int level, addr_t va, size_t size,
    vm_page_reservation* reservation, UpdatePte&& updatePte)
{
	ASSERT(level < 4);

	// The root table is allocated lazily by Map(), so a map that has never mapped
	// anything has fPageTable == 0. Unmap() and Query() test for that themselves,
	// but UnmapPage(), UnmapPages(), Protect(), ClearFlags() and
	// ClearAccessedAndModified() all walk unconditionally, and this was only an
	// ASSERT -- compiled out unless KDEBUG is on, so a release kernel had no check
	// at all. TableFromPa(0) then treats physical page zero as the root table, and
	// wherever the garbage there happens to have bits[1:0] == 0b11 the walk
	// descends and the callbacks zero, or CAS attribute bits into, arbitrary
	// physical memory. That is silent corruption, which is strictly worse to
	// diagnose than the panic this file was just fixed to avoid.
	//
	// An absent table means there are no mappings, so "nothing to do" is the
	// correct answer for every walker. Testing it here rather than in each caller
	// covers all of them at once and cannot rot as callers are added -- riscv64
	// centralises the same check in RISCV64VMTranslationMap::LookupPte(). Recursive
	// calls cannot arrive with zero, because GetOrMakeTable() returning 0 is
	// filtered by the `continue` below, so this only fires for a top-level call on
	// an empty map. Map() is unaffected: it allocates the root table at first use
	// before walking.
	if (ptPa == 0)
		return;

	uint64_t pageMask = (1UL << fPageBits) - 1;
	uint64_t vaMask = (1UL << fVaBits) - 1;

	ASSERT((va & pageMask) == 0);

	int tableBits = fPageBits - 3;
	uint64_t tableMask = (1UL << tableBits) - 1;

	int shift = tableBits * (3 - level) + fPageBits;
	uint64_t entrySize = 1UL << shift;
	uint64_t entryMask = entrySize - 1;

	uint64_t alignedDownVa = va & ~entryMask;
	uint64_t end = va + size - 1;
	if (level == 3)
		ASSERT(alignedDownVa == va);

    for (uint64_t effectiveVa = alignedDownVa; effectiveVa < end; effectiveVa += entrySize) {
		int index = ((effectiveVa & vaMask) >> shift) & tableMask;
		uint64_t* ptePtr = TableFromPa(ptPa) + index;

		if (level == 3) {
			updatePte(ptePtr, effectiveVa);
		} else {
			phys_addr_t subTable = GetOrMakeTable(ptPa, level, index, reservation);

			// When reservation is null, we can't create a new subtable. This can be intentional,
			// for example when called from Unmap().
			if (subTable == 0)
				continue;

			if (effectiveVa < va) {
				// The range begins inside the slot.
				if (effectiveVa + entrySize - 1 > end) {
					// The range ends within the slot.
					ProcessRange(subTable, level + 1, va, size, reservation, updatePte);
				} else {
					// The range extends past the end of the slot.
					ProcessRange(subTable, level + 1, va, effectiveVa + entrySize - va, reservation, updatePte);
				}
			} else {
				// The range beginning is aligned to the slot.
				if (effectiveVa + entrySize - 1 > end) {
					// The range ends within the slot.
					ProcessRange(subTable, level + 1, effectiveVa, end - effectiveVa + 1,
						reservation, updatePte);
				} else {
					// The range extends past the end of the slot.
					ProcessRange(subTable, level + 1, effectiveVa, entrySize, reservation, updatePte);
				}
			}
		}
	}
}


uint8_t
VMSAv8TranslationMap::MairIndex(uint8_t type)
{
	for (int i = 0; i < 8; i++)
		if (((fMair >> (i * 8)) & 0xff) == type)
			return i;

	panic("MAIR entry not found");
	return 0;
}


uint64_t
VMSAv8TranslationMap::GetMemoryAttr(uint32 attributes, uint32 memoryType, bool isKernel)
{
	uint64_t attr = 0;

	if (!isKernel)
		attr |= kAttrNG;

	if ((attributes & B_EXECUTE_AREA) == 0)
		attr |= kAttrUXN;
	if ((attributes & B_KERNEL_EXECUTE_AREA) == 0)
		attr |= kAttrPXN;

	// SWDBM is software reserved bit that we use to mark that
	// writes are allowed, and fault handler should clear kAttrAPReadOnly.
	// In that case kAttrAPReadOnly doubles as not-dirty bit.
	// Additionally dirty state can be stored in SWDIRTY, in order not to lose
	// dirty state when changing protection from RW to RO.

	// All page permissions begin life in RO state.
	attr |= kAttrAPReadOnly;

	// User-Execute implies User-Read, because it would break PAN otherwise
	if ((attributes & B_READ_AREA) != 0 || (attributes & B_EXECUTE_AREA) != 0)
		attr |= kAttrAPUserAccess; // Allow user reads

	if ((attributes & B_WRITE_AREA) != 0 || (attributes & B_KERNEL_WRITE_AREA) != 0)
		attr |= kAttrSWDBM; // Mark as writeable

	// When supported by hardware copy our SWDBM bit into DBM,
	// so that kAttrAPReadOnly is cleared on write attempt automatically
	// without going through fault handler.
	if ((fHwFeature & HW_DIRTY) != 0 && (attr & kAttrSWDBM) != 0)
		attr |= kAttrDBM;

	attr |= kAttrSHInnerShareable; // Inner Shareable

	uint8_t type = MAIR_NORMAL_WB;

	switch (memoryType & B_MEMORY_TYPE_MASK) {
		case B_UNCACHED_MEMORY:
			// TODO: This probably should be nGnRE for PCI
			type = MAIR_DEVICE_nGnRnE;
			break;
		case B_WRITE_COMBINING_MEMORY:
			type = MAIR_NORMAL_NC;
			break;
		case B_WRITE_THROUGH_MEMORY:
			type = MAIR_NORMAL_WT;
			break;
		case B_WRITE_PROTECTED_MEMORY:
			type = MAIR_NORMAL_WT;
			break;
		default:
		case B_WRITE_BACK_MEMORY:
			type = MAIR_NORMAL_WB;
			break;
	}

	attr |= MairIndex(type) << 2;

	return attr;
}


status_t
VMSAv8TranslationMap::Map(addr_t va, phys_addr_t pa, uint32 attributes, uint32 memoryType,
	vm_page_reservation* reservation)
{
	TRACE("VMSAv8TranslationMap::Map(0x%" B_PRIxADDR ", 0x%" B_PRIxADDR
		", 0x%x, 0x%x)\n", va, pa, attributes, memoryType);

	ThreadCPUPinner pinner(thread_get_current_thread());

	ASSERT(ValidateVa(va));
	uint64_t attr = GetMemoryAttr(attributes, memoryType, fIsKernel);

	// During first mapping we need to allocate root table
	if (fPageTable == 0) {
		vm_page* page = vm_page_allocate_page(reservation, PAGE_STATE_WIRED | VM_PAGE_ALLOC_CLEAR);
		DEBUG_PAGE_ACCESS_END(page);
		fPageTable = page->physical_page_number << fPageBits;
	}

	ProcessRange(fPageTable, fInitialLevel, va, B_PAGE_SIZE, reservation,
		[=](uint64_t* ptePtr, uint64_t effectiveVa) {
			while (true) {
				phys_addr_t effectivePa = effectiveVa - va + pa;
				uint64_t oldPte = atomic_get64((int64*)ptePtr);
				uint64_t newPte = effectivePa | attr | kPteTypeL3Page;

				if (newPte == oldPte)
					return;

				if ((oldPte & kPteValidMask) != 0) {
					// ARM64 requires "break-before-make". We must set the PTE to an invalid
					// entry and flush the TLB as appropriate before we can write the new PTE.
					if (!AttemptPteBreakBeforeMake(ptePtr, oldPte, effectiveVa))
						continue;
				}

				// Install the new PTE
				atomic_set64((int64*)ptePtr, newPte);
				asm("dsb ishst"); // Ensure PTE write completed
				asm("isb");
				if ((attributes & (B_EXECUTE_AREA | B_KERNEL_EXECUTE_AREA)) != 0)
					arch_cpu_sync_icache((void*)(KERNEL_PMAP_BASE + effectivePa), B_PAGE_SIZE);
				break;
			}
		});

	return B_OK;
}


status_t
VMSAv8TranslationMap::Unmap(addr_t start, addr_t end)
{
	TRACE("VMSAv8TranslationMap::Unmap(0x%" B_PRIxADDR ", 0x%" B_PRIxADDR
		")\n", start, end);
	ThreadCPUPinner pinner(thread_get_current_thread());

	size_t size = end - start + 1;
	ASSERT(ValidateVa(start));

	if (fPageTable == 0)
		return B_OK;

	ProcessRange(fPageTable, fInitialLevel, start, size, nullptr,
		[=](uint64_t* ptePtr, uint64_t effectiveVa) {
			ASSERT(effectiveVa <= end);
			uint64_t oldPte = atomic_get_and_set64((int64_t*)ptePtr, 0);
			FlushVAIfAccessed(oldPte, effectiveVa);
		});

	return B_OK;
}


status_t
VMSAv8TranslationMap::UnmapPage(VMArea* area, addr_t address,
	bool updatePageQueue, bool deletingAddressSpace, uint32* _flags)
{
	ASSERT(address % B_PAGE_SIZE == 0);
	ASSERT(_flags == NULL || !updatePageQueue);

	TRACE("VMSAv8TranslationMap::UnmapPage(0x%" B_PRIxADDR "(%s), 0x%"
		B_PRIxADDR ", %d)\n", (addr_t)area, area->name, address,
		updatePageQueue);

	ASSERT(ValidateVa(address));
	ThreadCPUPinner pinner(thread_get_current_thread());
	RecursiveLocker locker(fLock);

	uint64_t oldPte = 0;
	ProcessRange(fPageTable, fInitialLevel, address, B_PAGE_SIZE, nullptr,
		[=, &oldPte](uint64_t* ptePtr, uint64_t effectiveVa) {
			oldPte = atomic_get_and_set64((int64_t*)ptePtr, 0);
			if (!deletingAddressSpace)
				FlushVAIfAccessed(oldPte, effectiveVa);
		});

	if ((oldPte & kPteValidMask) == 0)
		return B_ENTRY_NOT_FOUND;

	pinner.Unlock();

	if (_flags == NULL) {
		locker.Detach();
		PageUnmapped(area, (oldPte & kPteAddrMask) >> fPageBits, is_pte_accessed(oldPte),
			is_pte_dirty(oldPte), updatePageQueue);
	} else {
		uint32 flags = PAGE_PRESENT;
		if (is_pte_accessed(oldPte))
			flags |= PAGE_ACCESSED;
		if (is_pte_dirty(oldPte))
			flags |= PAGE_MODIFIED;
		*_flags = flags;
	}

	return B_OK;
}


void
VMSAv8TranslationMap::UnmapPages(VMArea* area, addr_t address, size_t size,
	bool updatePageQueue, bool deletingAddressSpace)
{
	TRACE("VMSAv8TranslationMap::UnmapPages(0x%" B_PRIxADDR "(%s), 0x%"
		B_PRIxADDR ", 0x%" B_PRIxSIZE ", %d)\n", (addr_t)area,
		area->name, address, size, updatePageQueue);

	ASSERT(ValidateVa(address));
	VMAreaMappings queue;
	ThreadCPUPinner pinner(thread_get_current_thread());
	RecursiveLocker locker(fLock);

	ProcessRange(fPageTable, fInitialLevel, address, size, nullptr,
		[=, &queue](uint64_t* ptePtr, uint64_t effectiveVa) {
			uint64_t oldPte = atomic_get_and_set64((int64_t*)ptePtr, 0);
			FlushVAIfAccessed(oldPte, effectiveVa);
			if ((oldPte & kPteValidMask) == 0)
				return;

			if (area->cache_type == CACHE_TYPE_DEVICE)
				return;

			page_num_t page = (oldPte & kPteAddrMask) >> fPageBits;
			PageUnmapped(area, page,
				is_pte_accessed(oldPte), is_pte_dirty(oldPte),
				updatePageQueue, &queue);
		});

	// TODO: As in UnmapPage() we can lose page dirty flags here. ATM it's not
	// really critical here, as in all cases this method is used, the unmapped
	// area range is unmapped for good (resized/cut) and the pages will likely
	// be freed.

	locker.Unlock();

	// free removed mappings
	bool isKernelSpace = area->address_space == VMAddressSpace::Kernel();
	uint32 freeFlags = CACHE_DONT_WAIT_FOR_MEMORY
		| (isKernelSpace ? CACHE_DONT_LOCK_KERNEL_SPACE : 0);

	while (vm_page_mapping* mapping = queue.RemoveHead())
		vm_free_page_mapping(mapping->page->physical_page_number, mapping, freeFlags);
}


bool
VMSAv8TranslationMap::ValidateVa(addr_t va)
{
	uint64_t vaMask = (1UL << fVaBits) - 1;
	bool kernelAddr = (va & (1UL << 63)) != 0;
	if (kernelAddr != fIsKernel)
		return false;
	if ((va & ~vaMask) != (fIsKernel ? ~vaMask : 0))
		return false;
	return true;
}


status_t
VMSAv8TranslationMap::Query(addr_t va, phys_addr_t* pa, uint32* flags)
{
	*flags = 0;
	*pa = 0;

	uint64_t pageMask = (1UL << fPageBits) - 1;
	va &= ~pageMask;

	ThreadCPUPinner pinner(thread_get_current_thread());
	ASSERT(ValidateVa(va));

	// The root table is allocated lazily by Map(), so there may be nothing to
	// walk yet. Report "not present" rather than dereferencing a null table.
	if (fPageTable == 0)
		return B_OK;

	ProcessRange(fPageTable, fInitialLevel, va, B_PAGE_SIZE, nullptr,
		[=](uint64_t* ptePtr, uint64_t effectiveVa) {
			uint64_t pte = atomic_get64((int64_t*)ptePtr);

			// ProcessRange() hands us every level-3 slot covered by the range,
			// whether or not it holds a live mapping: a level-3 table exists as
			// soon as any single page in its 2MB span is mapped, so slots for
			// never-faulted pages are reached here holding an invalid entry.
			// Such an entry carries neither a physical address nor attributes,
			// so it must be reported as absent. Callers use PAGE_PRESENT to
			// decide whether *pa is meaningful, and a zero *pa passed to
			// vm_lookup_page() is fatal.
			if ((pte & kPteValidMask) == 0)
				return;

			*pa = pte & kPteAddrMask;
			*flags |= PAGE_PRESENT | B_KERNEL_READ_AREA;
			if (is_pte_accessed(pte))
				*flags |= PAGE_ACCESSED;
			if (is_pte_dirty(pte))
				*flags |= PAGE_MODIFIED;

			if ((pte & kAttrPXN) == 0)
				*flags |= B_KERNEL_EXECUTE_AREA;

			if ((pte & kAttrAPUserAccess) != 0) {
				*flags |= B_READ_AREA;
				if ((pte & kAttrUXN) == 0)
					*flags |= B_EXECUTE_AREA;
			}

			if ((pte & kAttrSWDBM) != 0) {
				*flags |= B_KERNEL_WRITE_AREA;
				if ((pte & kAttrAPUserAccess) != 0)
					*flags |= B_WRITE_AREA;
			}
		});

	return B_OK;
}


status_t
VMSAv8TranslationMap::QueryInterrupt(
	addr_t virtualAddress, phys_addr_t* _physicalAddress, uint32* _flags)
{
	return Query(virtualAddress, _physicalAddress, _flags);
}


status_t
VMSAv8TranslationMap::Protect(addr_t start, addr_t end, uint32 attributes, uint32 memoryType)
{
	TRACE("VMSAv8TranslationMap::Protect(0x%" B_PRIxADDR ", 0x%"
		B_PRIxADDR ", 0x%x, 0x%x)\n", start, end, attributes, memoryType);

	uint64_t attr = GetMemoryAttr(attributes, memoryType, fIsKernel);
	size_t size = end - start + 1;
	ASSERT(ValidateVa(start));

	ThreadCPUPinner pinner(thread_get_current_thread());

	ProcessRange(fPageTable, fInitialLevel, start, size, nullptr,
		[=](uint64_t* ptePtr, uint64_t effectiveVa) {
			ASSERT(effectiveVa <= end);

			uint64_t pte = atomic_get64((int64_t*)ptePtr);

			// ProcessRange() also visits level-3 slots for pages that were
			// never faulted in. Protection bits on an absent page mean
			// nothing: Map() derives them afresh from the area's current
			// protection when the fault arrives. Writing them here would only
			// leave attributes behind in an invalid entry, and would pay for a
			// break-before-make and a TLB invalidation to do it.
			if ((pte & kPteValidMask) == 0)
				return;

			phys_addr_t pa = pte & kPteAddrMask;

			// We need to use an atomic compare-swap loop because we must
			// need to clear somes bits while setting others.
			while (true) {
				uint64_t oldPte = atomic_get64((int64_t*)ptePtr);
				uint64_t newPte = oldPte & ~kPteAttrMask;
				newPte |= attr;

				// Preserve access bit.
				newPte |= oldPte & kAttrAF;

				// Preserve the Contiguous grouping. attr is derived afresh and
				// never sets it, so without this a Protect() would strip the
				// bit from one member of a group and leave the rest set, which
				// is UNPREDICTABLE. This is only safe because a whole
				// Contiguous group always shares one VMArea and is re-protected
				// as a unit -- every member gets the same attr. Should partial
				// re-protection of a group ever become possible, the group must
				// instead be dissolved (bit cleared across all members with
				// break-before-make) before any member changes.
				newPte |= oldPte & kAttrContiguous;

				// Preserve the dirty bit.
				if (is_pte_dirty(oldPte))
					newPte = set_pte_dirty(newPte);

				uint64_t oldMemoryType = oldPte & (kAttrShareability | kAttrMemoryAttrIdx);
				uint64_t newMemoryType = newPte & (kAttrShareability | kAttrMemoryAttrIdx);
				if (oldMemoryType != newMemoryType) {
					// ARM64 requires "break-before-make". We must set the PTE to an invalid
					// entry and flush the TLB as appropriate before we can write the new PTE.
					// In this case specifically, it applies any time we change cacheability or
					// shareability.
					if (!AttemptPteBreakBeforeMake(ptePtr, oldPte, effectiveVa))
						continue;

					atomic_set64((int64_t*)ptePtr, newPte);
					asm("dsb ishst"); // Ensure PTE write completed
					asm("isb");

					// No compare-exchange loop required in this case.
					break;
				} else {
					if ((uint64_t)atomic_test_and_set64((int64_t*)ptePtr, newPte, oldPte) == oldPte) {
						FlushVAIfAccessed(oldPte, effectiveVa);
						break;
					}
				}
			}

			if ((attributes & (B_EXECUTE_AREA | B_KERNEL_EXECUTE_AREA)) != 0 && pa != 0)
				arch_cpu_sync_icache((void*)(KERNEL_PMAP_BASE + pa), B_PAGE_SIZE);
		});

	return B_OK;
}


status_t
VMSAv8TranslationMap::ClearFlags(addr_t va, uint32 flags)
{
	ASSERT(ValidateVa(va));

	bool clearAF = flags & PAGE_ACCESSED;
	bool setRO = flags & PAGE_MODIFIED;

	if (!clearAF && !setRO)
		return B_OK;

	ThreadCPUPinner pinner(thread_get_current_thread());

	ProcessRange(fPageTable, fInitialLevel, va, B_PAGE_SIZE, nullptr,
		[=](uint64_t* ptePtr, uint64_t effectiveVa) {
			// An absent page has no accessed or dirty state to clear, and
			// set_pte_clean() would otherwise leave kAttrAPReadOnly behind in
			// an entry that should stay zero.
			if ((atomic_get64((int64_t*)ptePtr) & kPteValidMask) == 0)
				return;

			if (clearAF && setRO) {
				// We need to use an atomic compare-swap loop because we must
				// need to clear one bit while setting the other.
				while (true) {
					uint64_t oldPte = atomic_get64((int64_t*)ptePtr);
					uint64_t newPte = oldPte & ~kAttrAF;
					newPte = set_pte_clean(newPte);

                    if ((uint64_t)atomic_test_and_set64((int64_t*)ptePtr, newPte, oldPte) == oldPte) {
						FlushVAIfAccessed(oldPte, va);
						break;
					}
				}
			} else if (clearAF) {
				atomic_and64((int64_t*)ptePtr, ~kAttrAF);
			} else {
				while (true) {
					uint64_t oldPte = atomic_get64((int64_t*)ptePtr);
					if (!is_pte_dirty(oldPte))
						return;
					uint64_t newPte = set_pte_clean(oldPte);
                    if ((uint64_t)atomic_test_and_set64((int64_t*)ptePtr, newPte, oldPte) == oldPte) {
						FlushVAIfAccessed(oldPte, va);
						break;
					}
				}
			}
		});

	return B_OK;
}


bool
VMSAv8TranslationMap::ClearAccessedAndModified(
	VMArea* area, addr_t address, bool unmapIfUnaccessed, bool& _modified)
{
	TRACE("VMSAv8TranslationMap::ClearAccessedAndModified(0x%"
		B_PRIxADDR "(%s), 0x%" B_PRIxADDR ", %d)\n", (addr_t)area,
		area->name, address, unmapIfUnaccessed);
	ASSERT(ValidateVa(address));

	RecursiveLocker locker(fLock);
	ThreadCPUPinner pinner(thread_get_current_thread());

	uint64_t oldPte = 0;
	ProcessRange(fPageTable, fInitialLevel, address, B_PAGE_SIZE, nullptr,
		[=, &oldPte](uint64_t* ptePtr, uint64_t effectiveVa) {
			// We need to use an atomic compare-swap loop because we must
			// first read the old PTE and make decisions based on the AF
			// bit to proceed.
			while (true) {
				oldPte = atomic_get64((int64_t*)ptePtr);

				// Nothing is mapped here, so there is no state to clear and
				// nothing to unmap. Leave the entry alone.
				if ((oldPte & kPteValidMask) == 0)
					return;

				uint64_t newPte = oldPte & ~kAttrAF;
				newPte = set_pte_clean(newPte);

				// If the page has been not be accessed, then unmap it.
				if (unmapIfUnaccessed && (oldPte & kAttrAF) == 0)
					newPte = 0;

				if ((uint64_t)atomic_test_and_set64((int64_t*)ptePtr, newPte, oldPte) == oldPte)
					break;
			}
			asm("dsb ishst"); // Ensure PTE write completed
		});

	pinner.Unlock();

	// oldPte is still zero if the range had no level-3 table at all, or if the
	// entry we found was invalid. Either way nothing was mapped: report no
	// modification and do not hand a zero physical page number to
	// UnaccessedPageUnmapped(), which would look it up and dereference NULL.
	if ((oldPte & kPteValidMask) == 0) {
		_modified = false;
		return false;
	}

	_modified = is_pte_dirty(oldPte);

	if (FlushVAIfAccessed(oldPte, address))
		return true;

	if (!unmapIfUnaccessed)
		return false;

	locker.Detach(); // UnaccessedPageUnmapped takes ownership
	phys_addr_t oldPa = oldPte & kPteAddrMask;
	UnaccessedPageUnmapped(area, oldPa >> fPageBits);
	return false;
}


void
VMSAv8TranslationMap::Flush()
{
	// Necessary invalidation is performed during mapping,
	// no need to do anything more here.
}
