/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Haiku platform layer for Amazon's ENA host abstraction layer (ena-com).
 *
 * Everything else in this directory is Amazon's ena-com, copied verbatim from
 * amzn-drivers/kernel/fbsd/ena/ena-com (BSD-3-Clause) so that it can be
 * updated by re-copying. This header is the only file we write: ena-com
 * reaches the operating system exclusively through the macros and types
 * below, so porting the HAL means implementing this contract and nothing
 * more. Do not edit the ena-com sources; if something does not build, the
 * missing piece belongs here.
 *
 * Notes for anyone changing this file:
 *
 * - ENA_COM_* are small positive errno-style values and must stay that way,
 *   because ena-com smuggles them through pointers with ERR_PTR()/IS_ERR().
 *   They are NOT Haiku status_t values; the driver translates at its boundary.
 *   See the ENA_COM_* block below for the full reasoning.
 *
 * - Device-visible ordering uses memory_full_barrier() ("dsb sy"), not
 *   memory_write_barrier() ("dsb ishst"). Haiku's arm64 barriers are scoped to
 *   the inner-shareable domain, which does not necessarily order a descriptor
 *   write in DRAM against the doorbell MMIO write that publishes it to a PCIe
 *   master. Doorbells are batched, so the stronger barrier costs nothing
 *   measurable and removes a whole class of "device saw a stale descriptor"
 *   bug that is near-impossible to diagnose on hardware we cannot emulate.
 *
 * - MMIO goes through inline asm with a plain [base] register operand. On
 *   virtualised arm64 a compiler is free to fold the address arithmetic into
 *   writeback (pre/post-index) addressing, and a writeback load or store
 *   traps with ISV=0, meaning the hypervisor cannot decode the instruction and
 *   injects an external abort instead of emulating it. "volatile" does not
 *   prevent the addressing mode; only keeping the address in its own register
 *   does. This bit us on the GICv3 work already.
 */
#ifndef ENA_PLAT_H_
#define ENA_PLAT_H_


#include <KernelExport.h>
#include <OS.h>
#include <SupportDefs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch_atomic.h>


#ifdef __cplusplus
extern "C" {
#endif


/* #pragma mark - logging */

enum ena_log_t {
	ENA_ERR = 0,
	ENA_WARN,
	ENA_INFO,
	ENA_DBG,
};

extern int ena_log_level;

/* ena-com passes the log level as a bare token (ERR, WARN, ...) which the
   macro pastes onto ENA_. Keep that shape. */
#define ena_log(dev, level, fmt, args...)				\
	do {								\
		(void)(dev);						\
		if (ENA_##level <= ena_log_level)			\
			dprintf("ena: " fmt, ##args);			\
	} while (0)

#define ena_log_raw(level, fmt, args...)				\
	do {								\
		if (ENA_##level <= ena_log_level)			\
			dprintf(fmt, ##args);				\
	} while (0)

#define ena_log_unused(dev, level, fmt, args...)			\
	do {								\
		(void)(dev);						\
	} while (0)

/* The datapath log is compiled out unless explicitly asked for: it fires per
   packet and would make the serial console the bottleneck. */
#ifdef ENA_LOG_IO_ENABLE
#	define ena_log_io(dev, level, fmt, args...)			\
		ena_log((dev), level, fmt, ##args)
#else
#	define ena_log_io(dev, level, fmt, args...)			\
		ena_log_unused((dev), level, fmt, ##args)
#endif

#define ena_log_nm(dev, level, fmt, args...)				\
	ena_log((dev), level, "[nm] " fmt, ##args)

#define ena_trace(ctx, level, fmt, args...)				\
	ena_log((ctx)->dmadev, level, "%s(): " fmt, __func__, ##args)

#define ena_trc_dbg(ctx, format, arg...)	ena_trace(ctx, DBG, format, ##arg)
#define ena_trc_info(ctx, format, arg...)	ena_trace(ctx, INFO, format, ##arg)
#define ena_trc_warn(ctx, format, arg...)	ena_trace(ctx, WARN, format, ##arg)
#define ena_trc_err(ctx, format, arg...)	ena_trace(ctx, ERR, format, ##arg)

#define ENA_WARN(cond, ctx, format, arg...)				\
	do {								\
		if (unlikely((cond)))					\
			ena_trc_warn(ctx, format, ##arg);		\
	} while (0)


/* #pragma mark - compiler and bit helpers */

#define unlikely(x)	__builtin_expect(!!(x), 0)
#define likely(x)	__builtin_expect(!!(x), 1)

#define __iomem
#define ____cacheline_aligned	__attribute__((aligned(64)))

/* Deliberately not implemented with offsetof(). ena_com.h calls this as
   container_of(io_sq, struct ena_com_dev, io_sq_queues[io_sq->qid]), i.e. with
   a member whose array index is a runtime value. GCC accepts that in
   __builtin_offsetof as an extension, but in C++ -- and ena.cpp is C++ --
   offsetof must be a constant expression, so it is rejected. Computing the
   member address relative to a null pointer keeps the same arithmetic without
   demanding a constant, which is what Linux's container_of does; Haiku already
   builds kernel code with -fno-delete-null-pointer-checks, so the compiler
   will not treat the null base as an assertion it can exploit. */
#ifndef container_of
#	define container_of(ptr, type, member)				\
		((type*)((addr_t)(ptr) - (addr_t)&(((type*)0)->member)))
#endif

#ifndef MIN
#	define MIN(a, b)	((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#	define MAX(a, b)	((a) > (b) ? (a) : (b))
#endif

#define min_t(type, _x, _y)	((type)(_x) < (type)(_y) ? (type)(_x) : (type)(_y))
#define max_t(type, _x, _y)	((type)(_x) > (type)(_y) ? (type)(_x) : (type)(_y))

#define ENA_MIN32(x, y)	MIN(x, y)
#define ENA_MIN16(x, y)	MIN(x, y)
#define ENA_MIN8(x, y)	MIN(x, y)
#define ENA_MAX32(x, y)	MAX(x, y)
#define ENA_MAX16(x, y)	MAX(x, y)
#define ENA_MAX8(x, y)	MAX(x, y)

#define GENMASK(h, l)		(((~0U) - (1U << (l)) + 1) & (~0U >> (32 - 1 - (h))))
#define GENMASK_ULL(h, l)	(((~0ULL) << (l)) & (~0ULL >> (64 - 1 - (h))))
#define BIT(x)			(1UL << (x))
#define BIT64(x)		BIT(x)

#define SZ_256	(256)
#define SZ_4K	(4096)

#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define upper_32_bits(n)	((uint32_t)(((n) >> 16) >> 16))
#define lower_32_bits(n)	((uint32_t)(n))

#define ENA_FFS(x)		__builtin_ffs(x)
#define ENA_BITS_PER_U64(bitmap) __builtin_popcountll(bitmap)
#define ENA_FIELD_GET(value, mask, offset) (((value) & (mask)) >> (offset))

#define MAX_ERRNO	4095
#define IS_ERR_VALUE(x)	unlikely((x) <= (unsigned long)MAX_ERRNO)

static inline long IS_ERR(const void* ptr)
	{ return IS_ERR_VALUE((unsigned long)ptr); }
static inline void* ERR_PTR(long error)
	{ return (void*)error; }
static inline long PTR_ERR(const void* ptr)
	{ return (long)ptr; }

#define BUG()		panic("ENA BUG")
#define ENA_ABORT()	BUG()

#define prefetch(x)	(void)(x)
#define prefetchw(x)	(void)(x)

#define time_after(a, b) ((long)((unsigned long)(b) - (unsigned long)(a)) < 0)


/* #pragma mark - error codes */

/* These MUST stay small positive integers -- POSIX errno values, exactly as
   Amazon's own platform headers define them. They are not merely compared for
   equality.

   ena_com_submit_admin_cmd() reports failure by returning ERR_PTR(code), and
   its caller detects that with IS_ERR(), which is

       (unsigned long)pointer <= MAX_ERRNO   (4095)

   so an error code only survives the round trip through a pointer if its
   magnitude is small. Mapping these onto Haiku's status_t values, which live
   at 0x8000xxxx and sign-extend to 0xffffffff8000xxxx, makes IS_ERR() answer
   false for every error: the caller then treats the error code as a valid
   ena_comp_ctx and dereferences it, faulting on a sign-extended address.

   Translate to status_t at the driver boundary instead -- see
   ena_translate_error() in ena.cpp. */
#define ENA_COM_OK		0
#define ENA_COM_FAULT		14	/* EFAULT */
#define ENA_COM_INVAL		22	/* EINVAL */
#define ENA_COM_NO_MEM		12	/* ENOMEM */
#define ENA_COM_NO_SPACE	28	/* ENOSPC */
#define ENA_COM_TRY_AGAIN	-1
#define ENA_COM_UNSUPPORTED	45	/* EOPNOTSUPP */
#define ENA_COM_NO_DEVICE	19	/* ENODEV */
#define ENA_COM_PERMISSION	1	/* EPERM */
#define ENA_COM_TIMER_EXPIRED	60	/* ETIMEDOUT */
#define ENA_COM_EIO		5	/* EIO */
#define ENA_COM_DEVICE_BUSY	16	/* EBUSY */

#define ENA_NODE_ANY	(-1)

/* How often the admin wait re-checks its flag. */
#define ENA_ADMIN_POLL_INTERVAL_US	100


/* #pragma mark - time and delay */

#define ENA_MSLEEP(x)	snooze((bigtime_t)(x) * 1000)
#define ENA_USLEEP(x)	snooze((bigtime_t)(x))
#define ENA_UDELAY(x)	spin(x)

/* ena-com only ever compares a timeout against "now", so system_time()
   microseconds serve directly as the time base. */
#define ENA_GET_SYSTEM_TIMEOUT(timeout_us)	(system_time() + (timeout_us))
#define ENA_TIME_EXPIRE(timeout)		((timeout) < system_time())
#define ENA_GET_SYSTEM_TIME_HIGH_RES()		system_time()
#define ENA_GET_SYSTEM_TIMEOUT_HIGH_RES(current_time, timeout_us)	\
	((current_time) + (timeout_us))
#define ENA_TIME_EXPIRE_HIGH_RES(timeout)	ENA_TIME_EXPIRE(timeout)
#define ENA_TIME_INIT_HIGH_RES()		(0)
#define ENA_TIME_COMPARE_HIGH_RES(time1, time2)				\
	(((time1) < (time2)) ? -1 : (((time1) > (time2)) ? 1 : 0))

/* We never call into ena-com from a context that may not sleep except for the
   admin-completion interrupt path, which does not use this. */
#define ENA_MIGHT_SLEEP()


/* #pragma mark - locking */

/* ena-com takes this lock in the admin-completion interrupt handler as well as
   from threads, so it has to be a real spinlock with interrupts disabled --
   a mutex would deadlock. ena-com already threads an "unsigned long flags"
   through every lock/unlock pair (FreeBSD ignores it), which is exactly the
   place to stash the interrupt state. */
typedef spinlock ena_spinlock_t;

#define ENA_SPINLOCK_INIT(spinlock)					\
	B_INITIALIZE_SPINLOCK(&(spinlock))
#define ENA_SPINLOCK_DESTROY(spinlock)					\
	do { (void)(spinlock); } while (0)
#define ENA_SPINLOCK_LOCK(spinlock, flags)				\
	do {								\
		(flags) = (unsigned long)disable_interrupts();		\
		acquire_spinlock(&(spinlock));				\
	} while (0)
#define ENA_SPINLOCK_UNLOCK(spinlock, flags)				\
	do {								\
		release_spinlock(&(spinlock));				\
		restore_interrupts((cpu_status)(flags));		\
	} while (0)


/* #pragma mark - wait queues */

/* A flag, not a semaphore.

   ena-com clears the wait event from inside __ena_com_submit_admin_cmd(), which
   runs with admin_queue->q_lock held and therefore with interrupts disabled. A
   sem_id cannot be cleared there: draining it means acquire_sem_etc(), and
   Haiku panics if that is called with interrupts disabled. That is reachable
   for real -- when an admin completion arrives without an interrupt, ena-com
   signals a comp_ctx nobody is waiting on, leaving a stale count behind, and
   the panic fires once that command id is reused 32 commands later.

   Admin commands only happen during setup and reconfiguration, so polling the
   flag costs nothing and is legal in every context ena-com uses these from:
   clearing and signalling are single atomic stores, and the wait always runs
   outside the lock. */
typedef int32 ena_wait_event_t;

#define ENA_WAIT_EVENT_INIT(waitqueue)					\
	atomic_set(&(waitqueue), 0)

#define ENA_WAIT_EVENT_CLEAR(waitqueue)					\
	atomic_set(&(waitqueue), 0)

#define ENA_WAIT_EVENT_SIGNAL(waitqueue)				\
	atomic_set(&(waitqueue), 1)

#define ENA_WAIT_EVENT_WAIT(waitqueue, timeout_us)			\
	do {								\
		bigtime_t __end = system_time() + (bigtime_t)(timeout_us); \
		while (atomic_get(&(waitqueue)) == 0			\
				&& system_time() < __end) {		\
			snooze(ENA_ADMIN_POLL_INTERVAL_US);		\
		}							\
	} while (0)

#define ENA_WAIT_EVENTS_DESTROY(admin_queue)				\
	do { (void)(admin_queue); } while (0)


/* #pragma mark - types */

#define dma_addr_t	phys_addr_t
#define u8		uint8_t
#define u16		uint16_t
#define u32		uint32_t
#define u64		uint64_t

#define ENA_PRIu64	B_PRIu64

typedef bigtime_t ena_time_t;
typedef bigtime_t ena_time_high_res_t;
typedef int32 ena_atomic32_t;

/* ena-com stores a back pointer to the driver's device but never dereferences
   it, so an opaque forward declaration is enough. */
struct ena_haiku_device;
typedef struct ena_haiku_device ena_netdev;

#define DEFAULT_ALLOC_ALIGNMENT		8
#define ENA_CDESC_RING_SIZE_ALIGNMENT	(1 << 12) /* 4K */

/* Coherent DMA allocation. Haiku has no bus_dma equivalent, so we keep the
   area around to free it and the physical address we already had to look up. */
typedef struct {
	phys_addr_t	paddr;
	void*		vaddr;
	area_id		area;
	size_t		size;
} ena_mem_handle_t;

/* Mapped BARs. reg_bar is BAR0 (registers); mem_bar is BAR2 (the LLQ push
   window) and is 0 when the device does not offer it. */
struct ena_bus {
	addr_t	reg_bar;
	addr_t	mem_bar;
	size_t	reg_bar_size;
	size_t	mem_bar_size;
};

int	ena_dma_alloc(void* dmadev, size_t size, ena_mem_handle_t* dma,
		int mapflags, size_t alignment, int domain);
void	ena_dma_free(void* dmadev, ena_mem_handle_t* dma);
void	ena_rss_key_fill(void* key, size_t size);

#define ENA_RSS_FILL_KEY(key, size)	ena_rss_key_fill(key, size)


/* #pragma mark - barriers */

#define barrier()	__asm__ __volatile__("" : : : "memory")

/* ena-com uses the Linux/FreeBSD barrier names directly, expecting the platform
   to supply them. wmb() guards descriptor writes before a doorbell, so it has
   to order against a PCIe master and gets the full-system barrier for the
   reason given in the header comment -- an inner-shareable one is not
   guaranteed to be enough. */
#define mb()		memory_full_barrier()
#define wmb()		memory_full_barrier()
/* dma_rmb() is the only barrier guarding every read of device-written memory --
   descriptor phase bits in the RX/TX completion rings, the admin CQ and the
   AENQ. By the same argument as wmb() above, an inner-shareable read barrier is
   not guaranteed to order "read the phase bit" before "read the rest of the
   descriptor" against a PCIe master, which would yield a fresh phase bit beside
   a stale length or request id. FreeBSD gets away with a bare compiler barrier
   here only because it relies on bus_dmamap_sync(POSTREAD), which we have no
   equivalent of. */
#define rmb()		memory_full_barrier()
#define dma_rmb()	memory_full_barrier()
#define mmiowb()	memory_full_barrier()

#define ACCESS_ONCE(x)	(*(volatile __typeof(x)*)&(x))
#define READ_ONCE(x)							\
	({								\
		__typeof(x) __var;					\
		barrier();						\
		__var = ACCESS_ONCE(x);					\
		barrier();						\
		__var;							\
	})
#define READ_ONCE8(x)	READ_ONCE(x)
#define READ_ONCE16(x)	READ_ONCE(x)
#define READ_ONCE32(x)	READ_ONCE(x)

/* Descriptor rings live in coherent memory on every machine that has ENA
   (Nitro PCIe is I/O-coherent), so publishing a descriptor is an ordering
   problem, not a cache-maintenance one. See the header comment on why this is
   a full barrier. */
#define ENA_DB_SYNC_WRITE(mem_handle)					\
	do { (void)(mem_handle); memory_full_barrier(); } while (0)
#define ENA_DB_SYNC_PREREAD(mem_handle)					\
	do { (void)(mem_handle); memory_full_barrier(); } while (0)
#define ENA_DB_SYNC_POSTREAD(mem_handle)				\
	do { (void)(mem_handle); memory_read_barrier(); } while (0)
#define ENA_DB_SYNC(mem_handle)		ENA_DB_SYNC_WRITE(mem_handle)


/* #pragma mark - register access */

/* Keep the MMIO address in its own register operand; see the header comment
   about ISV=0 traps on writeback addressing. */
static inline uint32_t
ena_reg_read32(struct ena_bus* bus, unsigned long offset)
{
	uint32_t value;
	addr_t address = bus->reg_bar + offset;

#if defined(__aarch64__)
	__asm__ __volatile__("ldr %w0, [%1]"
		: "=r"(value) : "r"(address) : "memory");
#else
	value = *(volatile uint32_t*)address;
#endif

	memory_read_barrier();
	return value;
}


static inline void
ena_reg_write32_relaxed(struct ena_bus* bus, uint32_t value,
	unsigned long offset)
{
	addr_t address = bus->reg_bar + offset;

#if defined(__aarch64__)
	__asm__ __volatile__("str %w0, [%1]"
		: : "r"(value), "r"(address) : "memory");
#else
	*(volatile uint32_t*)address = value;
#endif
}


#define ENA_REG_READ32(bus, offset)					\
	ena_reg_read32((struct ena_bus*)(bus), (unsigned long)(offset))

#define ENA_REG_WRITE32_RELAXED(bus, value, offset)			\
	ena_reg_write32_relaxed((struct ena_bus*)(bus), (value),		\
		(unsigned long)(offset))

#define ENA_REG_WRITE32(bus, value, offset)				\
	do {								\
		memory_full_barrier();					\
		ENA_REG_WRITE32_RELAXED(bus, value, offset);		\
	} while (0)


static inline void
ena_write64_to_device(addr_t address, uint64_t value)
{
#if defined(__aarch64__)
	__asm__ __volatile__("str %0, [%1]"
		: : "r"(value), "r"(address) : "memory");
#else
	*(volatile uint64_t*)address = value;
#endif
}


/* Pushes an LLQ header into the device's BAR2 window. The device requires
   64-bit wide stores, so this cannot go through memcpy. */
#define ENA_MEMCPY_TO_DEVICE_64(bus, dst, src, size)			\
	do {								\
		int __count = (int)((size) / 8);				\
		int __i;						\
		const uint64_t* __from = (const uint64_t*)(src);		\
		addr_t __to = (addr_t)(dst);				\
		(void)(bus);						\
		for (__i = 0; __i < __count; __i++, __from++, __to += 8) { \
			ena_write64_to_device(__to, *__from);		\
		}							\
		memory_full_barrier();					\
	} while (0)


#define memcpy_toio	memcpy


/* #pragma mark - memory allocation */

#define ENA_MEM_ALLOC(dmadev, size)					\
	calloc(1, (size))

#define ENA_MEM_ALLOC_NODE(dmadev, size, virt, node, dev_node)		\
	do {								\
		(void)(node);						\
		(void)(dev_node);					\
		(virt) = calloc(1, (size));				\
	} while (0)

#define ENA_MEM_FREE(dmadev, ptr, size)					\
	do {								\
		(void)(size);						\
		free(ptr);						\
	} while (0)

#define ENA_MEM_ALLOC_COHERENT_ALIGNED(dmadev, size, virt, phys, dma,	\
    alignment)								\
	do {								\
		ena_dma_alloc((dmadev), (size), &(dma), 0, (alignment),	\
			ENA_NODE_ANY);					\
		(virt) = (dma).vaddr;					\
		(phys) = (dma).paddr;					\
	} while (0)

#define ENA_MEM_ALLOC_COHERENT(dmadev, size, virt, phys, dma)		\
	ENA_MEM_ALLOC_COHERENT_ALIGNED(dmadev, size, virt, phys, dma,	\
		DEFAULT_ALLOC_ALIGNMENT)

/* Haiku has no NUMA allocator, so the node hint is dropped. */
#define ENA_MEM_ALLOC_COHERENT_NODE_ALIGNED(dmadev, size, virt, phys,	\
    dma, node, dev_node, alignment)					\
	do {								\
		(void)(node);						\
		(void)(dev_node);					\
		ENA_MEM_ALLOC_COHERENT_ALIGNED(dmadev, size, virt, phys, \
			dma, alignment);				\
	} while (0)

#define ENA_MEM_ALLOC_COHERENT_NODE(dmadev, size, virt, phys, handle,	\
    node, dev_node)							\
	ENA_MEM_ALLOC_COHERENT_NODE_ALIGNED(dmadev, size, virt, phys,	\
		handle, node, dev_node, DEFAULT_ALLOC_ALIGNMENT)

#define ENA_MEM_FREE_COHERENT(dmadev, size, virt, phys, dma)		\
	do {								\
		(void)(size);						\
		(void)(phys);						\
		ena_dma_free((dmadev), &(dma));				\
		(virt) = NULL;						\
	} while (0)


/* #pragma mark - atomics */

#define ATOMIC32_INC(I32_PTR)		atomic_add((int32*)(I32_PTR), 1)
#define ATOMIC32_DEC(I32_PTR)		atomic_add((int32*)(I32_PTR), -1)
#define ATOMIC32_READ(I32_PTR)		atomic_get((int32*)(I32_PTR))
#define ATOMIC32_SET(I32_PTR, VAL)	atomic_set((int32*)(I32_PTR), (VAL))


/* #pragma mark - DMA buffer accessors */

#define dma_unmap_addr(p, name)			((p)->dma->name)
#define dma_unmap_addr_set(p, name, v)		(((p)->dma->name) = (v))
#define dma_unmap_len(p, name)			((p)->name)
#define dma_unmap_len_set(p, name, v)		(((p)->name) = (v))


#ifdef __cplusplus
}
#endif


#include "ena_defs/ena_includes.h"

/* AWS allocates the host-OS identifiers reported in the host attributes
   (1 Linux ... 7 macOS) and has none for Haiku. The field is telemetry, but
   an out-of-range value risks the device rejecting SET_HOST_ATTRIBUTES
   outright, so we report FreeBSD: it is the lineage of the HAL we build here
   and it keeps us inside the enum. Revisit if AWS assigns Haiku an id. */
#define ENA_ADMIN_OS_FREEBSD	4
#define ENA_ADMIN_OS_HAIKU	ENA_ADMIN_OS_FREEBSD


#endif /* ENA_PLAT_H_ */
