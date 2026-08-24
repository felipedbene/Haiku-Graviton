/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_REGS_H
#define ARCH_ARM64_GICV3_REGS_H

#include <SupportDefs.h>


// Distributor register byte offsets (GICD)
#define GICD_CTLR				0x0000
#define GICD_TYPER				0x0004
#define GICD_IGROUPR			0x0080
#define GICD_ISENABLER			0x0100
#define GICD_ICENABLER			0x0180
#define GICD_IPRIORITYR			0x0400
#define GICD_ICFGR				0x0c00
#define GICD_IROUTER			0x6000

// Which bit enables non-secure group 1 depends on something software cannot
// see: with GICD_CTLR.DS set (a single security state, which is what a
// hypervisor's emulated distributor presents) it is bit 1, but on a
// distributor with two security states the non-secure view puts it at bit 0
// and leaves bit 1 reserved once affinity routing is on. DS itself is only
// readable from the secure view, so both bits get written -- the one that does
// not apply is reserved, not harmful.
#define GICD_CTLR_ENABLE_G1		(1u << 0)
#define GICD_CTLR_ENABLE_G1NS	(1u << 1)
#define GICD_CTLR_ARE_NS		(1u << 4)
#define GICD_CTLR_RWP			(1u << 31)

// GICD_PIDR2.ArchRev: 3 for a GICv3 implementation, 4 for GICv4. This is what
// decides how many 64 KB frames each redistributor occupies, and it is the
// only in-band way to find that out before touching a redistributor.
#define GICD_PIDR2				0xffe8
#define GICD_PIDR2_ARCH(v)		(((v) >> 4) & 0xf)

// GICD_TYPER.ITLinesNumber: max SPI is 32*(N+1) - 1
#define GICD_TYPER_ITLINES(t)	((((t) & 0x1f) + 1) * 32)

// Interrupt Routing Mode: 1 = deliver to any participating PE
#define GICD_IROUTER_IRM		(1ull << 31)

// Redistributor, RD_base frame
#define GICR_CTLR				0x0000
#define GICR_TYPER				0x0008
#define GICR_WAKER				0x0014

#define GICR_WAKER_PROCESSOR_SLEEP	(1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP	(1u << 2)

// GICR_TYPER: bit 1 reports support for virtual LPIs (and therefore the
// presence of the two extra GICv4 frames), bit 4 marks the last redistributor
// in the region, bits [63:32] hold this redistributor's packed affinity value.
#define GICR_TYPER_VLPIS		(1ull << 1)
#define GICR_TYPER_LAST			(1ull << 4)

// Redistributor SGI/PPI frame lives one 64K page after RD_base
#define GICR_SGI_FRAME			0x10000
#define GICR_IGROUPR0			0x0080
#define GICR_ISENABLER0			0x0100
#define GICR_ICENABLER0			0x0180
#define GICR_IPRIORITYR			0x0400

// GICv3 redistributors occupy two 64K frames per PE; GICv4 adds two more
// for the virtual LPI frames.
#define GICR_STRIDE_V3			0x20000
#define GICR_STRIDE_V4			0x40000


// One mapped run of redistributor frames. There is more than one whenever
// firmware describes the redistributors per-CPU instead of with a single
// range, which it does exactly when they are not all adjacent.
struct gicr_region {
	addr_t		base;
	phys_addr_t	physicalBase;
	size_t		size;
};

// Interrupt ID layout
#define GIC_SGI_BASE			0
#define GIC_PPI_BASE			16
#define GIC_SPI_BASE			32
#define GIC_SPECIAL_BASE		1020

// Priority the driver assigns to every interrupt, and the running priority
// mask. Both are chosen so they behave identically whether or not the
// distributor implements two security states (GICD_CTLR.DS).
#define GIC_PRIORITY_DEFAULT	0xa0
#define GIC_PRIORITY_MASK		0xf0



// ---------------------------------------------------------------------------
// LPIs and the Interrupt Translation Service
//
// An MSI write lands in GITS_TRANSLATER carrying an EventID. The ITS looks the
// writing device up in its device table, walks that device's Interrupt
// Translation Table to turn (DeviceID, EventID) into an LPI, and forwards the
// LPI to the redistributor named by the associated collection.

// LPI INTIDs start here; anything below is an SGI, PPI or SPI.
#define GIC_LPI_BASE			8192

// GICD_TYPER.IDbits gives the number of INTID bits implemented.
#define GICD_TYPER_IDBITS(t)	((((t) >> 19) & 0x1f) + 1)

// Redistributor LPI registers, RD_base frame
#define GICR_PROPBASER			0x0070
#define GICR_PENDBASER			0x0078

#define GICR_CTLR_ENABLE_LPIS	(1u << 0)

// GICR_TYPER[23:8] is this redistributor's processor number, used as the ITS
// target when GITS_TYPER.PTA is clear.
#define GICR_TYPER_PROC_NUM(t)	(((t) >> 8) & 0xffff)

// LPI configuration table entry: one byte per LPI.
#define GIC_LPI_CONFIG_ENABLE	(1u << 0)

// ITS registers
#define GITS_CTLR				0x0000
#define GITS_TYPER				0x0008
#define GITS_CBASER				0x0080
#define GITS_CWRITER			0x0088
#define GITS_CREADR				0x0090
#define GITS_BASER				0x0100
#define GITS_BASER_COUNT		8
#define GITS_TRANSLATER			0x10040

#define GITS_CTLR_ENABLED		(1u << 0)
#define GITS_CTLR_QUIESCENT		(1u << 31)

#define GITS_TYPER_PHYSICAL		(1ull << 0)
#define GITS_TYPER_ITT_SIZE(t)	((((t) >> 4) & 0xf) + 1)
#define GITS_TYPER_ID_BITS(t)	((((t) >> 8) & 0x1f) + 1)
#define GITS_TYPER_DEV_BITS(t)	((((t) >> 13) & 0x1f) + 1)
#define GITS_TYPER_PTA			(1ull << 19)

#define GITS_BASER_VALID		(1ull << 63)
#define GITS_BASER_INDIRECT		(1ull << 62)
#define GITS_BASER_TYPE(b)		(((b) >> 56) & 0x7)
#define GITS_BASER_ENTRY_SIZE(b)	((((b) >> 48) & 0x1f) + 1)
// GITS_BASER[9:8] selects the granule the Size field counts in. An
// implementation may ignore what we ask for and report something else, and the
// granule changes how the address field itself is laid out, so the value read
// back has to be honoured rather than assumed -- see _InitTables().
#define GITS_BASER_PAGE_SIZE_MASK	(3ull << 8)
#define GITS_BASER_PAGE_SIZE_4K		(0ull << 8)
#define GITS_BASER_PAGE_SIZE_16K	(1ull << 8)
#define GITS_BASER_PAGE_SIZE_64K	(2ull << 8)

// InnerCache is a three-bit field, not a flag: 0 is Device-nGnRnE, 1 is Normal
// Inner Non-cacheable, and 7 is Normal Inner Cacheable read-allocate,
// write-allocate, write-back. It has to be a *cacheable* encoding, because we
// build these tables through the kernel's ordinary cacheable mapping. Asking
// for non-cacheable makes the ITS read around the caches those writes are
// sitting in, so it sees an empty or stale table and silently has no
// translation for any interrupt -- which looks exactly like a device that
// never raised one. Emulated ITS models read guest memory directly and do not
// care, which is why this only shows up on real hardware.
#define GITS_BASER_CACHE_SHIFT	59
#define GITS_BASER_CACHE_MASK	(7ull << GITS_BASER_CACHE_SHIFT)
#define GITS_BASER_INNER_CACHE	(7ull << GITS_BASER_CACHE_SHIFT)
#define GITS_BASER_SHARE_MASK	(3ull << 10)
#define GITS_BASER_SHAREABILITY	(1ull << 10)	/* inner shareable */

#define GITS_BASER_TYPE_NONE		0
#define GITS_BASER_TYPE_DEVICE		1
#define GITS_BASER_TYPE_COLLECTION	4

#define GITS_CBASER_VALID		(1ull << 63)
#define GITS_CBASER_CACHE_MASK	(7ull << 59)
#define GITS_CBASER_INNER_CACHE	(7ull << 59)
#define GITS_CBASER_SHARE_MASK	(3ull << 10)
#define GITS_CBASER_SHAREABILITY	(1ull << 10)

// Command queue: 32 bytes per command.
#define GITS_CMD_SIZE			32
#define GITS_CMD_QUEUE_SIZE		0x10000

#define GITS_CMD_MAPD			0x08
#define GITS_CMD_MAPC			0x09
#define GITS_CMD_MAPTI			0x0a
#define GITS_CMD_INV			0x0c
#define GITS_CMD_INVALL			0x0d
#define GITS_CMD_DISCARD		0x0f
#define GITS_CMD_SYNC			0x05

// PROPBASER/PENDBASER cacheability and shareability, matching the tables we
// allocate as normal write-back memory. InnerCache is bits [9:7] here and uses
// the same encoding as GITS_BASER above, so it must be 7 (RaWaWb) rather than
// 1 (non-cacheable); see the comment there.
#define GICR_BASER_CACHE_MASK	(7ull << 7)
#define GICR_BASER_INNER_CACHE	(7ull << 7)
#define GICR_BASER_SHARE_MASK	(3ull << 10)
#define GICR_BASER_SHAREABILITY	(1ull << 10)

// The redistributor may access a whole 64 KB granule of the LPI pending table
// regardless of how many LPIs we actually use, so the allocation is rounded up
// to that even though the bitmap itself is far smaller.
#define GICR_PENDBASER_ALIGNMENT	0x10000


// The GIC is emulated by the hypervisor on virtualised systems, so every
// register access is trapped and then reconstructed from the ESR syndrome.
// Instructions using writeback addressing (post-/pre-index) report ISV=0,
// leaving the hypervisor unable to decode them: KVM answers with an external
// abort rather than emulating the access. A plain `volatile` pointer is not
// enough, because the compiler is free to turn a loop of stores into the
// post-indexed form. Go through inline assembly so the addressing mode is
// guaranteed to stay simple.

static inline uint32
gic_read32(addr_t address)
{
	uint32 value;
	__asm__ __volatile__("ldr %w0, [%1]" : "=r" (value) : "r" (address)
		: "memory");
	return value;
}


static inline void
gic_write32(addr_t address, uint32 value)
{
	__asm__ __volatile__("str %w0, [%1]" :: "rZ" (value), "r" (address)
		: "memory");
}


static inline uint64
gic_read64(addr_t address)
{
	uint64 value;
	__asm__ __volatile__("ldr %x0, [%1]" : "=r" (value) : "r" (address)
		: "memory");
	return value;
}


static inline void
gic_write64(addr_t address, uint64 value)
{
	__asm__ __volatile__("str %x0, [%1]" :: "rZ" (value), "r" (address)
		: "memory");
}


// Packed affinity, matching the layout of GICR_TYPER[63:32] and the
// Aff3.Aff2.Aff1.Aff0 encoding used by GICD_IROUTER.
static inline uint32
gic_packed_affinity(uint64 mpidr)
{
	return (uint32)(CPU_AFF0(mpidr) | (CPU_AFF1(mpidr) << 8)
		| (CPU_AFF2(mpidr) << 16) | (CPU_AFF3(mpidr) << 24));
}


static inline uint64
gic_routing_affinity(uint64 mpidr)
{
	return (uint64)CPU_AFF0(mpidr) | ((uint64)CPU_AFF1(mpidr) << 8)
		| ((uint64)CPU_AFF2(mpidr) << 16) | ((uint64)CPU_AFF3(mpidr) << 32);
}


#endif /* ARCH_ARM64_GICV3_REGS_H */
