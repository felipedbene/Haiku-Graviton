/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * BFS mount-time large auto-grow (design v2 Parts B-F). See
 * graviton/docs/develop/bfs-auto-grow-design.md.
 */
#ifndef BFS_GROW_ENGINE_H
#define BFS_GROW_ENGINE_H


#include "bfs.h"


#ifdef _BOOT_MODE
namespace BFS {
#endif


class Volume;


// On-disk intent record (Part C). Lives in a single block at
// partitionBlocks - 1 (outside the old filesystem). All scalar fields are
// stored little-endian (BFS byte order) via the accessors below; the two
// embedded disk_super_block copies are stored verbatim (already BFS-endian).
#define BFS_GROW_INTENT_MAGIC		0x47724f77	/* 'GrOw' */
#define BFS_GROW_INTENT_VERSION		1

enum bfs_grow_phase {
	BFS_GROW_PHASE_STARTED		= 1,
		// intent written, bitmap fill may be partially applied
	BFS_GROW_PHASE_BITMAP_DONE	= 2,
		// bitmap fully durable, superblock commit may or may not have landed
};


struct bfs_grow_intent {
	uint32		magic;
	uint32		version;
	uint32		crc;
		// crc32 over the whole record with this field taken as zero
	uint32		phase;
	uint64		partition_blocks;
	uint64		old_num_blocks;
	uint64		new_num_blocks;
	uint64		old_bitmap_blocks;
	uint64		new_bitmap_blocks;
	uint64		max_bitmap_blocks;
	uint64		intent_block;
	uint32		block_size;
	uint32		_pad;
	disk_super_block	old_super_block;
	disk_super_block	new_super_block;

	uint32 Magic() const { return BFS_ENDIAN_TO_HOST_INT32(magic); }
	uint32 Version() const { return BFS_ENDIAN_TO_HOST_INT32(version); }
	uint32 Crc() const { return BFS_ENDIAN_TO_HOST_INT32(crc); }
	uint32 Phase() const { return BFS_ENDIAN_TO_HOST_INT32(phase); }
	uint64 PartitionBlocks() const
		{ return BFS_ENDIAN_TO_HOST_INT64(partition_blocks); }
	uint64 OldNumBlocks() const
		{ return BFS_ENDIAN_TO_HOST_INT64(old_num_blocks); }
	uint64 NewNumBlocks() const
		{ return BFS_ENDIAN_TO_HOST_INT64(new_num_blocks); }
	uint64 OldBitmapBlocks() const
		{ return BFS_ENDIAN_TO_HOST_INT64(old_bitmap_blocks); }
	uint64 NewBitmapBlocks() const
		{ return BFS_ENDIAN_TO_HOST_INT64(new_bitmap_blocks); }
	uint64 MaxBitmapBlocks() const
		{ return BFS_ENDIAN_TO_HOST_INT64(max_bitmap_blocks); }
	uint64 IntentBlock() const
		{ return BFS_ENDIAN_TO_HOST_INT64(intent_block); }
	uint32 BlockSize() const { return BFS_ENDIAN_TO_HOST_INT32(block_size); }
} _PACKED;


// Part B entry point. Called from Volume::Mount() after the superblock has been
// validated and the device size is known, but BEFORE the block cache is created
// and the journal is brought up (single-threaded, no in-flight transactions).
//
// It probes for a pending grow (crash recovery, Part E) or, if the partition is
// larger than the recorded filesystem and the volume was baked with grow
// headroom, performs a fresh large grow (Part D) -- all through raw device I/O
// with explicit flushes (Part F). On success the volume's in-memory superblock
// is updated to the resulting (possibly grown) geometry.
//
// It NEVER leaves the volume in an inconsistent state and never requires the
// mount to fail: any refusal or recoverable error leaves the volume mountable
// at a geometry whose superblock is on block 0 (old or completed-new). The
// return value is informational for the caller's log only.
status_t bfs_grow_at_mount(Volume* volume, off_t deviceSize);


#ifdef _BOOT_MODE
}	// namespace BFS
#endif


#endif	// BFS_GROW_ENGINE_H
