/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * BFS mount-time large auto-grow (design v2 Parts B-F). See
 * graviton/docs/develop/bfs-auto-grow-design.md.
 *
 * All grow I/O is raw read_pos/write_pos on the device fd with explicit device
 * flushes. It runs before the block cache and journal are live, so it is
 * single-threaded with no transactions in flight, and every write is
 * absolute-content (idempotent on replay) rather than read-modify-write --
 * except the old-tail-bit clear, which is itself idempotent.
 */


#include "GrowEngine.h"

#include "BlockAllocator.h"
#include "Debug.h"
#include "Utility.h"
#include "Volume.h"


#ifdef BFS_GROW_FAULT_INJECTION
// Host-only (bfs_shell) crash-injection instrumentation. Compiled out of the
// kernel add-on entirely. The libc-backed implementation lives in GrowFault.cpp
// (kept out of the fs_shell API-wrapper translation unit). Driven by two
// environment variables:
//   BFS_GROW_ABORT=<label>    -- _exit() right at the named boundary, leaving
//                                the on-disk state as a real power loss would.
//                                "commit-torn" writes only the first half of
//                                the block-0 superblock before exiting.
//   BFS_GROW_WRITELOG=<path>  -- append a binary record of every raw grow write
//                                and every flush barrier, for external
//                                write-subset replay.
#	include "GrowFault.h"
#	define GROW_LOG_WRITE_IO(off, buf, len) \
		bfs_grow_fault_log_write((long long)(off), buf, (unsigned int)(len))
#	define GROW_CHECKPOINT(label)		bfs_grow_fault_checkpoint(label)
#	define GROW_ABORT_IS(label)			bfs_grow_fault_abort_is(label)
#else
#	define GROW_LOG_WRITE_IO(off, buf, len)	do {} while (0)
#	define GROW_CHECKPOINT(label)			do {} while (0)
#	define GROW_ABORT_IS(label)				(0)
#endif	// BFS_GROW_FAULT_INJECTION


#ifdef _BOOT_MODE
namespace BFS {
#endif


//	#pragma mark - low-level helpers


static uint32
grow_crc32(const uint8* data, size_t length)
{
	// standard CRC-32 (reflected, poly 0xedb88320)
	uint32 crc = 0xffffffff;
	for (size_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320 & (-(int32)(crc & 1)));
	}
	return crc ^ 0xffffffff;
}


static status_t
grow_write(Volume* volume, off_t offset, const void* buffer, size_t length)
{
	if (write_pos(volume->Device(), offset, buffer, length) != (ssize_t)length)
		return B_IO_ERROR;
	GROW_LOG_WRITE_IO(offset, buffer, length);
	return B_OK;
}


static status_t
grow_read(Volume* volume, off_t offset, void* buffer, size_t length)
{
	if (read_pos(volume->Device(), offset, buffer, length) != (ssize_t)length)
		return B_IO_ERROR;
	return B_OK;
}


static void
grow_flush(Volume* volume)
{
	// Full device flush (Part F). Best-effort: on hosts without a real drive
	// this is a no-op, but the on-disk bytes are already durable through the
	// raw fd, which is what the crash-injection harness models.
	ioctl(volume->Device(), B_FLUSH_DRIVE_CACHE);
}


//	#pragma mark - geometry


struct GrowGeometry {
	off_t	partitionBlocks;
	off_t	oldNumBlocks;
	off_t	newNumBlocks;
	off_t	oldBitmapBlocks;
	off_t	newBitmapBlocks;
	off_t	maxBitmapBlocks;	// = grow_max_blocks bitmap span (gap upper bound)
	off_t	intentBlock;		// partitionBlocks - 1
	uint32	blockSize;
	uint32	bitsPerBlock;
	int32	newNumAGs;
};


/*!	Derives the target geometry from the on-disk (old) superblock and the
	partition capacity. Returns B_NOT_SUPPORTED for anything that is not a
	provably-safe headroom large grow (the same guard the ioctl path applies).
*/
static status_t
grow_classify(Volume* volume, off_t deviceSize, GrowGeometry& geometry)
{
	const disk_super_block& super = volume->SuperBlock();

	uint32 blockSize = volume->BlockSize();
	uint32 bitsPerBlock = blockSize << 3;
	off_t partitionBlocks = deviceSize / blockSize;
	off_t oldNumBlocks = volume->NumBlocks();

	// Only ever grow, never shrink, and require enough new space to matter.
	if (partitionBlocks <= oldNumBlocks)
		return B_NOT_SUPPORTED;

	off_t growMaxBlocks = super.GrowMaxBlocks();
	if (growMaxBlocks <= 0) {
		// stock / foreign volume: no baked headroom, relocating grow not shipped
		return B_NOT_SUPPORTED;
	}

	// Target the partition, capped at the baked headroom.
	off_t newNumBlocks = partitionBlocks;
	if (newNumBlocks > growMaxBlocks)
		newNumBlocks = growMaxBlocks;

	off_t oldBitmapBlocks = (oldNumBlocks + bitsPerBlock - 1) / bitsPerBlock;
	off_t newBitmapBlocks = (newNumBlocks + bitsPerBlock - 1) / bitsPerBlock;

	// A grow that needs no extra bitmap block is the item-1 in-capacity path
	// (handled elsewhere, while mounted); this engine only does the large grow.
	if (newBitmapBlocks <= oldBitmapBlocks)
		return B_NOT_SUPPORTED;

	off_t maxBitmapBlocks = (growMaxBlocks + bitsPerBlock - 1) / bitsPerBlock;

	// The new bitmap must fit entirely inside the pre-reserved gap that the log
	// sits above; otherwise the extension would run into the log/data (a
	// non-headroom or over-cap layout). Refuse -- FS untouched.
	off_t logStartBlock = volume->ToBlock(volume->Log());
	if (newBitmapBlocks + 1 > logStartBlock)
		return B_NOT_SUPPORTED;

	// num_ags must scale so IsValid() keeps holding after commit.
	int32 agSize = 1L << volume->AllocationGroupShift();
	int64 newNumAGs = divide_roundup(newNumBlocks, agSize);
	if (newNumAGs < 1 || newNumAGs > 0x7fffffffLL)
		return B_NOT_SUPPORTED;

	// The intent record must be a single block outside the old FS.
	off_t intentBlock = partitionBlocks - 1;
	if (intentBlock < oldNumBlocks)
		return B_NOT_SUPPORTED;
	if (sizeof(bfs_grow_intent) > blockSize)
		return B_NOT_SUPPORTED;

	geometry.partitionBlocks = partitionBlocks;
	geometry.oldNumBlocks = oldNumBlocks;
	geometry.newNumBlocks = newNumBlocks;
	geometry.oldBitmapBlocks = oldBitmapBlocks;
	geometry.newBitmapBlocks = newBitmapBlocks;
	geometry.maxBitmapBlocks = maxBitmapBlocks;
	geometry.intentBlock = intentBlock;
	geometry.blockSize = blockSize;
	geometry.bitsPerBlock = bitsPerBlock;
	geometry.newNumAGs = (int32)newNumAGs;

	return B_OK;
}


/*!	Fills in the target superblock: new num_blocks / num_ags / used_blocks. The
	log is never moved, so log_blocks / log_start / log_end and everything else
	are copied verbatim from the old superblock.
*/
static void
grow_build_target_super(Volume* volume, const GrowGeometry& geometry,
	disk_super_block& target)
{
	target = volume->SuperBlock();
	target.num_blocks = HOST_ENDIAN_TO_BFS_INT64(geometry.newNumBlocks);
	target.num_ags = HOST_ENDIAN_TO_BFS_INT32(geometry.newNumAGs);

	// The only block in the new territory that is allocated is the intent
	// block, and only if it falls inside the (possibly capped) new FS. All
	// other new blocks are free; the reserved gap and log were already counted
	// as used in the old superblock and their physical blocks do not move.
	off_t used = volume->UsedBlocks();
	if (geometry.intentBlock < geometry.newNumBlocks)
		used += 1;
	target.used_blocks = HOST_ENDIAN_TO_BFS_INT64(used);
}


/*!	Part D step 1 sanity: the blocks the grow is about to claim as new bitmap
	blocks (and the whole reserved gap up to the log) must already be marked
	allocated in the on-disk bitmap -- that is how a baked headroom volume
	fences the gap so nothing live can be sitting there. Any clear bit means
	this is not a clean baked gap (foreign layout, prior corruption, or a
	tampered image); refuse and leave the FS untouched.
*/
static status_t
grow_check_gap_reserved(Volume* volume, const GrowGeometry& geometry)
{
	uint32 blockSize = geometry.blockSize;
	uint32 bitsPerBlock = geometry.bitsPerBlock;

	uint8* block = (uint8*)malloc(blockSize);
	if (block == NULL)
		return B_NO_MEMORY;
	MemoryDeleter deleter(block);

	// The gap the log sits above: device blocks [oldBitmapBlocks+1,
	// maxBitmapBlocks+1). Their representing bits live in the low bitmap
	// blocks; read each such bitmap block once and verify every bit is set.
	off_t gapStart = geometry.oldBitmapBlocks + 1;
	off_t gapEnd = geometry.maxBitmapBlocks + 1;
	if (gapEnd <= gapStart)
		return B_OK;

	off_t firstBitmapBlock = 1 + gapStart / bitsPerBlock;
	off_t lastBitmapBlock = 1 + (gapEnd - 1) / bitsPerBlock;

	for (off_t bm = firstBitmapBlock; bm <= lastBitmapBlock; bm++) {
		status_t status = grow_read(volume, bm * (off_t)blockSize, block,
			blockSize);
		if (status != B_OK)
			return status;

		off_t coverStart = (bm - 1) * (off_t)bitsPerBlock;
		off_t from = gapStart > coverStart ? gapStart : coverStart;
		off_t to = gapEnd < coverStart + bitsPerBlock
			? gapEnd : coverStart + bitsPerBlock;
		for (off_t b = from; b < to; b++) {
			off_t bit = b - coverStart;
			if ((block[bit >> 3] & (uint8)(1 << (bit & 7))) == 0) {
				INFORM(("bfs grow: reserved gap block %" B_PRIdOFF " is not "
					"marked allocated; refusing (not a clean baked gap)\n", b));
				return B_NOT_SUPPORTED;
			}
		}
	}

	return B_OK;
}


//	#pragma mark - Part C: intent record


static void
grow_build_intent(const GrowGeometry& geometry, const disk_super_block& oldSuper,
	const disk_super_block& targetSuper, uint32 phase, bfs_grow_intent& intent)
{
	memset(&intent, 0, sizeof(intent));
	intent.magic = HOST_ENDIAN_TO_BFS_INT32(BFS_GROW_INTENT_MAGIC);
	intent.version = HOST_ENDIAN_TO_BFS_INT32(BFS_GROW_INTENT_VERSION);
	intent.phase = HOST_ENDIAN_TO_BFS_INT32(phase);
	intent.partition_blocks = HOST_ENDIAN_TO_BFS_INT64(geometry.partitionBlocks);
	intent.old_num_blocks = HOST_ENDIAN_TO_BFS_INT64(geometry.oldNumBlocks);
	intent.new_num_blocks = HOST_ENDIAN_TO_BFS_INT64(geometry.newNumBlocks);
	intent.old_bitmap_blocks
		= HOST_ENDIAN_TO_BFS_INT64(geometry.oldBitmapBlocks);
	intent.new_bitmap_blocks
		= HOST_ENDIAN_TO_BFS_INT64(geometry.newBitmapBlocks);
	intent.max_bitmap_blocks
		= HOST_ENDIAN_TO_BFS_INT64(geometry.maxBitmapBlocks);
	intent.intent_block = HOST_ENDIAN_TO_BFS_INT64(geometry.intentBlock);
	intent.block_size = HOST_ENDIAN_TO_BFS_INT32(geometry.blockSize);
	intent.old_super_block = oldSuper;
	intent.new_super_block = targetSuper;

	intent.crc = 0;
	uint32 crc = grow_crc32((const uint8*)&intent, sizeof(intent));
	intent.crc = HOST_ENDIAN_TO_BFS_INT32(crc);
}


static bool
grow_intent_valid(const bfs_grow_intent& intent)
{
	if (intent.Magic() != BFS_GROW_INTENT_MAGIC)
		return false;
	if (intent.Version() != BFS_GROW_INTENT_VERSION)
		return false;
	bfs_grow_intent copy = intent;
	uint32 stored = intent.Crc();
	copy.crc = 0;
	return grow_crc32((const uint8*)&copy, sizeof(copy)) == stored;
}


static status_t
grow_write_intent(Volume* volume, const GrowGeometry& geometry,
	const bfs_grow_intent& intent)
{
	uint32 blockSize = geometry.blockSize;
	uint8* block = (uint8*)malloc(blockSize);
	if (block == NULL)
		return B_NO_MEMORY;
	MemoryDeleter deleter(block);

	memset(block, 0, blockSize);
	memcpy(block, &intent, sizeof(intent));

	return grow_write(volume, geometry.intentBlock * (off_t)blockSize, block,
		blockSize);
}


static status_t
grow_erase_intent(Volume* volume, const GrowGeometry& geometry)
{
	uint32 blockSize = geometry.blockSize;
	uint8* block = (uint8*)malloc(blockSize);
	if (block == NULL)
		return B_NO_MEMORY;
	MemoryDeleter deleter(block);

	// Zero the whole block (magic gone -> "no grow in progress"); the block
	// stays allocated in the new bitmap, which is one block of documented
	// waste.
	memset(block, 0, blockSize);
	return grow_write(volume, geometry.intentBlock * (off_t)blockSize, block,
		blockSize);
}


//	#pragma mark - Part D: bitmap fill + commit


/*!	Writes the new bitmap blocks [oldBitmapBlocks+1, newBitmapBlocks+1) into the
	pre-reserved gap and clears the old last bitmap block's out-of-range tail
	bits. Every block written here lies either in the baked-empty gap or is the
	old last bitmap block whose tail bits are, by construction, free -- so this
	destroys nothing the old geometry reads.

	If \a abortMidway is set (fault injection), returns after writing roughly
	half of the new bitmap blocks, before the durability flush.
*/
static status_t
grow_fill_bitmap(Volume* volume, const GrowGeometry& geometry)
{
	uint32 blockSize = geometry.blockSize;
	uint32 bitsPerBlock = geometry.bitsPerBlock;
	off_t intentBlock = geometry.intentBlock;
	off_t newNumBlocks = geometry.newNumBlocks;

	uint8* block = (uint8*)malloc(blockSize);
	if (block == NULL)
		return B_NO_MEMORY;
	MemoryDeleter deleter(block);

#ifdef BFS_GROW_FAULT_INJECTION
	off_t newBlocksToWrite = geometry.newBitmapBlocks - geometry.oldBitmapBlocks;
	off_t midpoint = geometry.oldBitmapBlocks + newBlocksToWrite / 2;
	bool abortMid = GROW_ABORT_IS("bitmap-mid");
#endif

	// New bitmap blocks: device block address k in [oldBitmapBlocks+1,
	// newBitmapBlocks], covering data blocks [(k-1)*bitsPerBlock,
	// k*bitsPerBlock). All such blocks are >= oldNumBlocks (new territory).
	for (off_t k = geometry.oldBitmapBlocks + 1; k <= geometry.newBitmapBlocks;
			k++) {
		memset(block, 0, blockSize);

		off_t coverStart = (k - 1) * (off_t)bitsPerBlock;
		off_t coverEnd = coverStart + bitsPerBlock;

		// Mark the intent block used if it lives in this bitmap block and
		// inside the new FS. Everything else in the new range is free; bits
		// past newNumBlocks are left zero (matching a freshly formatted
		// volume, which never sets out-of-range tail bits).
		if (intentBlock < newNumBlocks && intentBlock >= coverStart
			&& intentBlock < coverEnd) {
			off_t bit = intentBlock - coverStart;
			block[bit >> 3] |= (uint8)(1 << (bit & 7));
		}

		status_t status = grow_write(volume, k * (off_t)blockSize, block,
			blockSize);
		if (status != B_OK)
			return status;

#ifdef BFS_GROW_FAULT_INJECTION
		if (abortMid && k >= midpoint)
			GROW_CHECKPOINT("bitmap-mid");
#endif
	}

	// Clear the old last bitmap block's out-of-range tail bits: blocks
	// [oldNumBlocks, oldBitmapBlocks*bitsPerBlock) are real free blocks under
	// the new geometry. They are already free (zero) on a healthy volume, so
	// this is an idempotent safety step.
	off_t oldLastCoverStart
		= (geometry.oldBitmapBlocks - 1) * (off_t)bitsPerBlock;
	off_t oldLastCoverEnd = oldLastCoverStart + bitsPerBlock;
	if (geometry.oldNumBlocks < oldLastCoverEnd) {
		off_t oldLastBlockAddr = geometry.oldBitmapBlocks * (off_t)blockSize;
		status_t status = grow_read(volume, oldLastBlockAddr, block, blockSize);
		if (status != B_OK)
			return status;

		for (off_t b = geometry.oldNumBlocks; b < oldLastCoverEnd; b++) {
			off_t bit = b - oldLastCoverStart;
			block[bit >> 3] &= (uint8)~(1 << (bit & 7));
		}

		status = grow_write(volume, oldLastBlockAddr, block, blockSize);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}


/*!	Part D step 6: the atomic commit -- a single superblock write at byte
	offset 512. A torn write here is caught by recovery (Part E) because the
	superblock carries no checksum but the intent record stores both images.
*/
static status_t
grow_commit(Volume* volume, const disk_super_block& target)
{
#ifdef BFS_GROW_FAULT_INJECTION
	if (GROW_ABORT_IS("commit-torn")) {
		// Synthesize a torn superblock: persist only the first half of the
		// on-disk superblock, leaving the rest as the old image, then die.
		size_t half = sizeof(disk_super_block) / 2;
		write_pos(volume->Device(), 512, &target, half);
		GROW_LOG_WRITE_IO(512, &target, half);
		GROW_CHECKPOINT("commit-torn");
	}
#endif
	if (write_pos(volume->Device(), 512, &target, sizeof(disk_super_block))
			!= (ssize_t)sizeof(disk_super_block))
		return B_IO_ERROR;
	GROW_LOG_WRITE_IO(512, &target, sizeof(disk_super_block));
	return B_OK;
}


/*!	Executes the grow from \a startPhase forward (Part D steps 2-7). Used both
	for a fresh grow (startPhase == 0) and roll-forward recovery.
*/
static status_t
grow_execute(Volume* volume, const GrowGeometry& geometry,
	const disk_super_block& oldSuper, const disk_super_block& targetSuper,
	uint32 startPhase)
{
	bfs_grow_intent intent;

	if (startPhase < BFS_GROW_PHASE_BITMAP_DONE) {
		// (1) intent = STARTED + flush
		grow_build_intent(geometry, oldSuper, targetSuper,
			BFS_GROW_PHASE_STARTED, intent);
		status_t status = grow_write_intent(volume, geometry, intent);
		if (status != B_OK)
			return status;
		grow_flush(volume);
		GROW_CHECKPOINT("intent");

		// (2-3) fill new bitmap blocks + clear old tail
		status = grow_fill_bitmap(volume, geometry);
		if (status != B_OK)
			return status;
		grow_flush(volume);
		GROW_CHECKPOINT("bitmap-flush");

		// (4) intent = BITMAP_DONE + flush
		grow_build_intent(geometry, oldSuper, targetSuper,
			BFS_GROW_PHASE_BITMAP_DONE, intent);
		status = grow_write_intent(volume, geometry, intent);
		if (status != B_OK)
			return status;
		grow_flush(volume);
		GROW_CHECKPOINT("phase");
	}

	// (5) atomic commit + flush
	status_t status = grow_commit(volume, targetSuper);
	if (status != B_OK)
		return status;
	grow_flush(volume);
	GROW_CHECKPOINT("commit");

	// (6) erase intent magic + flush
	status = grow_erase_intent(volume, geometry);
	if (status != B_OK)
		return status;
	grow_flush(volume);
	GROW_CHECKPOINT("erase");

	return B_OK;
}


//	#pragma mark - Part E: recovery


/*!	A grow was found in progress (intent record present and valid). Roll it
	forward to completion; never roll back. Rebuilds the geometry from the
	stored intent so recovery needs nothing from the (possibly committed) block
	0.
*/
static status_t
grow_recover(Volume* volume, const bfs_grow_intent& intent)
{
	GrowGeometry geometry;
	geometry.partitionBlocks = intent.PartitionBlocks();
	geometry.oldNumBlocks = intent.OldNumBlocks();
	geometry.newNumBlocks = intent.NewNumBlocks();
	geometry.oldBitmapBlocks = intent.OldBitmapBlocks();
	geometry.newBitmapBlocks = intent.NewBitmapBlocks();
	geometry.maxBitmapBlocks = intent.MaxBitmapBlocks();
	geometry.intentBlock = intent.IntentBlock();
	geometry.blockSize = intent.BlockSize();
	geometry.bitsPerBlock = geometry.blockSize << 3;
	geometry.newNumAGs = intent.new_super_block.AllocationGroups();

	const disk_super_block& oldSuper = intent.old_super_block;
	const disk_super_block& targetSuper = intent.new_super_block;

	uint32 phase = intent.Phase();

	if (phase == BFS_GROW_PHASE_STARTED) {
		// bitmap writes may be partial; redo everything from the bitmap fill.
		INFORM(("bfs grow: recovering STARTED intent, redoing bitmap+commit\n"));
		status_t status = grow_fill_bitmap(volume, geometry);
		if (status != B_OK)
			return status;
		grow_flush(volume);
		status = grow_commit(volume, targetSuper);
		if (status != B_OK)
			return status;
		grow_flush(volume);
		return grow_erase_intent(volume, geometry);
	}

	if (phase == BFS_GROW_PHASE_BITMAP_DONE) {
		// bitmap is durable in target state; the commit may or may not have
		// landed, possibly torn. Compare block 0 to the two stored images.
		disk_super_block onDisk;
		status_t status = grow_read(volume, 512, &onDisk, sizeof(onDisk));
		if (status != B_OK)
			return status;

		bool matchesOld = memcmp(&onDisk, &oldSuper, sizeof(onDisk)) == 0;
		bool matchesNew = memcmp(&onDisk, &targetSuper, sizeof(onDisk)) == 0;

		if (matchesNew) {
			INFORM(("bfs grow: recovering BITMAP_DONE, commit already landed\n"));
			return grow_erase_intent(volume, geometry);
		}

		// matches old, or a torn write matching neither: rewrite block 0 to the
		// target (the bitmap is already fully in target state), then erase.
		INFORM(("bfs grow: recovering BITMAP_DONE, %s block 0 -> target\n",
			matchesOld ? "rewriting" : "repairing torn"));
		status = grow_commit(volume, targetSuper);
		if (status != B_OK)
			return status;
		grow_flush(volume);
		return grow_erase_intent(volume, geometry);
	}

	// Unknown phase: refuse to touch anything.
	return B_BAD_DATA;
}


//	#pragma mark - Part B: mount-time entry


status_t
bfs_grow_at_mount(Volume* volume, off_t deviceSize)
{
	if (volume->IsReadOnly())
		return B_OK;

	uint32 blockSize = volume->BlockSize();
	off_t partitionBlocks = deviceSize / blockSize;
	off_t fsBlocks = volume->NumBlocks();

	// The intent record lives at partitionBlocks - 1. Probe it whenever the
	// partition is larger than the filesystem (the exact grow trigger) so a
	// crash mid-grow is always recovered, even if a later mount no longer wants
	// to grow.
	if (partitionBlocks <= fsBlocks)
		return B_OK;

	off_t intentBlock = partitionBlocks - 1;

	uint8* probe = (uint8*)malloc(blockSize);
	if (probe == NULL)
		return B_NO_MEMORY;
	MemoryDeleter deleter(probe);

	if (read_pos(volume->Device(), intentBlock * (off_t)blockSize, probe,
			blockSize) == (ssize_t)blockSize) {
		bfs_grow_intent* intent = (bfs_grow_intent*)probe;
		if (grow_intent_valid(*intent)) {
			// Part E: a grow is in progress -- roll it forward.
			status_t status = grow_recover(volume, *intent);
			if (status == B_OK) {
				// Adopt the completed geometry.
				volume->SuperBlock() = intent->new_super_block;
			} else {
				FATAL(("bfs grow: recovery failed (%s); mounting at the "
					"superblock currently on block 0\n", strerror(status)));
				// Re-read block 0 so the in-memory superblock matches disk.
				disk_super_block onDisk;
				if (grow_read(volume, 512, &onDisk, sizeof(onDisk)) == B_OK
					&& onDisk.IsValid()) {
					volume->SuperBlock() = onDisk;
				}
			}
			return status;
		}
		// No valid magic: either no grow in progress or the intent write
		// itself was torn -- indistinguishable and equally safe. Fall through
		// to a fresh grow.
	}

	// Fresh grow (Part D). Classify; refuse anything not provably safe.
	GrowGeometry geometry;
	status_t status = grow_classify(volume, deviceSize, geometry);
	if (status != B_OK) {
		INFORM(("bfs grow: not eligible for mount-time large grow (%s); "
			"mounting at current size\n", strerror(status)));
		return B_OK;
	}

	// Part D step 1 sanity: refuse unless the gap is provably a clean, fully
	// reserved baked gap. This runs only on the fresh path; recovery trusts
	// the intent record it already validated.
	status = grow_check_gap_reserved(volume, geometry);
	if (status != B_OK) {
		INFORM(("bfs grow: gap sanity check failed (%s); mounting at current "
			"size\n", strerror(status)));
		return B_OK;
	}

	disk_super_block oldSuper = volume->SuperBlock();
	disk_super_block targetSuper;
	grow_build_target_super(volume, geometry, targetSuper);
	if (!targetSuper.IsValid()) {
		FATAL(("bfs grow: computed target superblock is invalid; refusing\n"));
		return B_OK;
	}

	INFORM(("bfs grow: growing %" B_PRIdOFF " -> %" B_PRIdOFF " blocks (%"
		B_PRIdOFF " -> %" B_PRIdOFF " bitmap blocks)\n", geometry.oldNumBlocks,
		geometry.newNumBlocks, geometry.oldBitmapBlocks,
		geometry.newBitmapBlocks));

	status = grow_execute(volume, geometry, oldSuper, targetSuper, 0);
	if (status != B_OK) {
		FATAL(("bfs grow: grow failed (%s)\n", strerror(status)));
		// Nothing before the commit changes anything old geometry reads, so
		// re-read block 0 and mount at whatever is there.
		disk_super_block onDisk;
		if (grow_read(volume, 512, &onDisk, sizeof(onDisk)) == B_OK
			&& onDisk.IsValid()) {
			volume->SuperBlock() = onDisk;
		}
		return status;
	}

	volume->SuperBlock() = targetSuper;
	return B_OK;
}


#ifdef _BOOT_MODE
}	// namespace BFS
#endif
