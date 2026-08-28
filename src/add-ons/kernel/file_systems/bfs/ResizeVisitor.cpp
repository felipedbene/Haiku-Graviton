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

	Only growth that fits inside the block bitmap already on disk is handled
	here; that is the common cloud case (an image baked at one size booted on a
	larger EBS volume) and, crucially, it needs no relocation: the bitmap, the
	log area and the file data all keep their fixed on-disk positions. Growth
	large enough to need extra bitmap blocks -- which would push the log and any
	data behind it -- and shrinking (which would have to evacuate inodes out of
	the truncated tail) are deliberately left unimplemented; see the ToDo file
	and the class comment. Both remain B_NOT_SUPPORTED so a caller never
	silently corrupts the volume.
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
	// count at the volume size. We can therefore hand those bits to the
	// allocator without touching anything else -- but only while the new size
	// still fits in the bitmap blocks already present. A larger grow needs new
	// bitmap blocks (and hence log/data relocation), which is a separate change.
	off_t oldBitmapBlocks = (oldNumBlocks + bitsPerBlock - 1) / bitsPerBlock;
	off_t newBitmapBlocks = (newNumBlocks + bitsPerBlock - 1) / bitsPerBlock;
	if (newBitmapBlocks != oldBitmapBlocks) {
		INFORM(("bfs: resize to %" B_PRIdOFF " blocks needs %" B_PRIdOFF
			" bitmap blocks (have %" B_PRIdOFF "); bitmap extension is not "
			"implemented\n", newNumBlocks, newBitmapBlocks, oldBitmapBlocks));
		return B_NOT_SUPPORTED;
	}

	// Because a single bitmap block never spans more than one allocation group,
	// staying inside the same bitmap-block count also keeps the number of
	// allocation groups constant; verify the invariant IsValid() enforces so a
	// grow can never produce a superblock the mount path would reject.
	int32 agSize = 1L << volume->AllocationGroupShift();
	if (divide_roundup(newNumBlocks, agSize)
			!= (int64)volume->AllocationGroups()) {
		return B_NOT_SUPPORTED;
	}

	// Persist the new block count, then rebuild the in-memory allocator so the
	// freshly-covered bitmap bits are picked up as free space
	// (BlockAllocator::_Initialize derives the last group's bit count from
	// Volume::NumBlocks()). used_blocks is unchanged -- growth only adds free
	// blocks -- so FreeBlocks() widens automatically.
	disk_super_block& superBlock = volume->SuperBlock();
	superBlock.num_blocks = HOST_ENDIAN_TO_BFS_INT64(newNumBlocks);

	status_t status = volume->WriteSuperBlock();
	if (status != B_OK) {
		// Roll the in-memory value back so the volume keeps describing what is
		// actually on disk.
		superBlock.num_blocks = HOST_ENDIAN_TO_BFS_INT64(oldNumBlocks);
		return status;
	}

	status = volume->Allocator().Reinitialize();
	if (status != B_OK)
		return status;

	return B_OK;
}
