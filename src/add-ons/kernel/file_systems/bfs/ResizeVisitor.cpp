/*
 * Copyright (C) 2020 Adrien Destugues <pulkomandy@pulkomandy.tk>
 *
 * Distributed under terms of the MIT license.
 */

#include "ResizeVisitor.h"

#include "BlockAllocator.h"
#include "Debug.h"
#include "Utility.h"
#include "Volume.h"


ResizeVisitor::ResizeVisitor(Volume* volume)
	:
	FileSystemVisitor(volume)
{
}


/*!	Grows the file system so it spans \a size bytes of the underlying device.

	Two grow cases are classified here; neither ever relocates live data:

	1. Growth that fits inside the block bitmap already on disk (item 1) is
	   applied directly -- the common cloud case (an image baked at one size
	   booted on a slightly larger volume): the bitmap, log and file data keep
	   their fixed positions and the already-zeroed trailing bitmap bits are
	   handed to the allocator.
	2. A larger grow that needs more bitmap blocks is only eligible on a volume
	   formatted with format-time headroom (Volume::Initialize +
	   VOLUME_GROW_HEADROOM): the extra bitmap blocks then fall inside a
	   pre-reserved, already-allocated gap below the high-placed log. The
	   classify/guard for that case lives below, but the in-place fill + atomic
	   commit + crash recovery (design Part D) are not yet implemented, so it is
	   still refused.

	Shrinking (which would have to evacuate inodes out of the truncated tail)
	and the general relocating grow (a bitmap extension on a volume with no
	baked gap) are deliberately unimplemented and return B_NOT_SUPPORTED, so a
	caller can never silently corrupt the volume. See the ToDo file and
	graviton/docs/develop/bfs-auto-grow-design.md.
*/
status_t
ResizeVisitor::Resize(off_t size, disk_job_id job)
{
	Volume* volume = GetVolume();

	if (volume->IsReadOnly())
		return B_READ_ONLY_DEVICE;

	const uint32 blockShift = volume->BlockShift();
	const uint32 bitsPerBlock = volume->BlockSize() << 3;

	off_t newNumBlocks = size >> blockShift;
	off_t oldNumBlocks = volume->NumBlocks();

	// Shrinking is not supported (it would require relocating data out of the
	// truncated tail).
	if (newNumBlocks < oldNumBlocks)
		return B_NOT_SUPPORTED;

	// Already the right size: a no-op keeps the first-boot trigger idempotent.
	if (newNumBlocks == oldNumBlocks)
		return B_OK;

	// The block bitmap starts right after the superblock and is followed by the
	// log area and then file data, all at fixed offsets. The trailing bits of
	// the last bitmap block past oldNumBlocks are already zero (free) on disk
	// but are not counted as usable because the allocation group caps its bit
	// count at the volume size.
	off_t oldBitmapBlocks = (oldNumBlocks + bitsPerBlock - 1) / bitsPerBlock;
	off_t newBitmapBlocks = (newNumBlocks + bitsPerBlock - 1) / bitsPerBlock;

	if (newBitmapBlocks == oldBitmapBlocks) {
		// Item 1 (merged): the grow fits inside the block bitmap already on
		// disk, so we can hand the already-zeroed trailing bits to the
		// allocator without touching anything else -- no relocation.
		//
		// Because a single bitmap block never spans more than one allocation
		// group, staying inside the same bitmap-block count also keeps the
		// number of allocation groups constant; verify the invariant IsValid()
		// enforces so a grow can never produce a superblock the mount path
		// would reject.
		int32 agSize = 1L << volume->AllocationGroupShift();
		if (divide_roundup(newNumBlocks, agSize)
				!= (int64)volume->AllocationGroups()) {
			return B_NOT_SUPPORTED;
		}

		// Persist the new block count, then rebuild the in-memory allocator so
		// the freshly-covered bitmap bits are picked up as free space
		// (BlockAllocator::_Initialize derives the last group's bit count from
		// Volume::NumBlocks()). used_blocks is unchanged -- growth only adds
		// free blocks -- so FreeBlocks() widens automatically.
		disk_super_block& superBlock = volume->SuperBlock();
		superBlock.num_blocks = HOST_ENDIAN_TO_BFS_INT64(newNumBlocks);

		status_t status = volume->WriteSuperBlock();
		if (status != B_OK) {
			// Roll the in-memory value back so the volume keeps describing what
			// is actually on disk.
			superBlock.num_blocks = HOST_ENDIAN_TO_BFS_INT64(oldNumBlocks);
			return status;
		}

		status = volume->Allocator().Reinitialize();
		if (status != B_OK)
			return status;

		return B_OK;
	}

	// Large grow: the target needs MORE bitmap blocks than are on disk. The v2
	// design handles this without relocation, but ONLY on a volume that was
	// formatted with format-time headroom (Part A): the extra bitmap blocks
	// then land inside a pre-reserved, already-allocated gap that sits below the
	// (high-placed) log, so nothing owned by the old filesystem is overwritten.
	//
	// This ONLINE (mounted, via BFS_IOCTL_RESIZE) path deliberately keeps
	// refusing the large grow: v2 executes it at MOUNT time instead
	// (bfs_grow_at_mount in GrowEngine.cpp, wired into Volume::Mount before the
	// block cache and journal come up), which is single-threaded with no
	// transactions in flight and needs no block_cache_set_size. The online large
	// grow was dropped in v2; only the in-capacity item-1 grow above is online.
	// The mount-time fill + atomic commit + crash recovery (Part D/E) are
	// implemented and gated behind BFS_ENABLE_LARGE_GROW, which stays undefined
	// until the fault-injection acceptance in
	// graviton/docs/develop/bfs-auto-grow-verification.md (T1.5 + T2) passes on
	// real Graviton. The classify/guard below still refuses here, unchanged.

	// Foreign / stock-layout volume: no headroom was baked (grow_max_blocks is
	// 0 on every legacy volume, since Initialize() zeroes the superblock). The
	// gap does not exist, so a large grow here would require the general
	// relocating path, which is not shipped. Refuse; the FS is untouched.
	off_t growMaxBlocks = volume->SuperBlock().GrowMaxBlocks();
	if (growMaxBlocks <= 0) {
		INFORM(("bfs: large resize to %" B_PRIdOFF " blocks needs bitmap "
			"extension, but this volume has no format-time grow headroom; "
			"refusing (relocating grow is not supported)\n", newNumBlocks));
		return B_NOT_SUPPORTED;
	}

	// Target beyond the baked cap: the reserved gap only covers growth up to
	// grow_max_blocks, so a larger target would run the new bitmap into the log.
	// Refuse rather than clamp -- a partition genuinely bigger than the baked
	// cap is an operator/format mismatch worth surfacing.
	if (newNumBlocks > growMaxBlocks) {
		INFORM(("bfs: resize target %" B_PRIdOFF " exceeds the baked grow cap "
			"%" B_PRIdOFF "; refusing\n", newNumBlocks, growMaxBlocks));
		return B_NOT_SUPPORTED;
	}

	// Eligible for the headroom large grow -- but that runs at mount time
	// (bfs_grow_at_mount), not online through this ioctl. Refuse here and let
	// the next mount grow the volume; the online large grow is not shipped.
	INFORM(("bfs: resize to %" B_PRIdOFF " blocks is eligible for the headroom "
		"grow (cap %" B_PRIdOFF ", %" B_PRIdOFF " -> %" B_PRIdOFF " bitmap "
		"blocks); this runs at mount time, not online -- refusing the ioctl\n",
		newNumBlocks, growMaxBlocks, oldBitmapBlocks, newBitmapBlocks));
	return B_NOT_SUPPORTED;
}
