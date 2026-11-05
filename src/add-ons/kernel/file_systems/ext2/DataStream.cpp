/*
 * Copyright 2001-2010, Haiku Inc. All rights reserved.
 * This file may be used under the terms of the MIT License.
 *
 * Authors:
 *		Janito V. Ferreira Filho
 */


#include "DataStream.h"

#include <stdlib.h>
#include <unistd.h>

#include <Drivers.h>

#include "CachedBlock.h"
#include "Volume.h"


//#define TRACE_EXT2
#ifdef TRACE_EXT2
#	define TRACE(x...) dprintf("\33[34mext2:\33[0m " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...)	dprintf("\33[34mext2:\33[0m " x)


DataStream::DataStream(Volume* volume, ext2_data_stream* stream,
	off_t size)
	:
	kBlockSize(volume->BlockSize()),
	kIndirectsPerBlock(kBlockSize / sizeof(uint32)),
	kIndirectsPerBlock2(kIndirectsPerBlock * kIndirectsPerBlock),
	kIndirectsPerBlock3(kIndirectsPerBlock2 * kIndirectsPerBlock),
	kMaxDirect(EXT2_DIRECT_BLOCKS),
	kMaxIndirect(kMaxDirect + kIndirectsPerBlock),
	kMaxDoubleIndirect(kMaxIndirect + kIndirectsPerBlock2),
	fVolume(volume),
	fStream(stream),
	fFirstBlock(volume->FirstDataBlock()),
	fAllocated(0),
	fAllocatedPos(fFirstBlock),
	fWaiting(0),
	fFreeStart(0),
	fFreeCount(0),
	fRemovedBlocks(0),
	fAddedBlocks(0),
	fFreedBlocks(0),
	fZeroBuffer(NULL),
	fSize(size)
{
	fNumBlocks = size == 0 ? 0 : ((size - 1) >> fVolume->BlockShift()) + 1;
}


DataStream::~DataStream()
{
}


status_t
DataStream::FindBlock(off_t offset, fsblock_t& block, uint32 *_count)
{
	uint32 index = offset >> fVolume->BlockShift();

	if (offset >= fSize) {
		TRACE("FindBlock: offset larger than inode size\n");
		return B_ENTRY_NOT_FOUND;
	}

	// TODO: we could return the size of the sparse range, as this might be more
	// than just a block

	// A block of 0 is a hole, which a sparse file made elsewhere can have
	// anywhere, also where a whole indirect block is missing.
	if (_count != NULL)
		*_count = 1;

	if (index < EXT2_DIRECT_BLOCKS) {
		// direct blocks
		block = B_LENDIAN_TO_HOST_INT32(fStream->direct[index]);
		if (_count && block != 0) {
			*_count = 1;
			uint32 nextBlock = block;
			while (++index < EXT2_DIRECT_BLOCKS
				&& fStream->direct[index] == ++nextBlock)
				(*_count)++;
		}
	} else if ((index -= EXT2_DIRECT_BLOCKS) < kIndirectsPerBlock) {
		// indirect blocks
		CachedBlock cached(fVolume);
		if (fStream->indirect == 0) {
			block = 0;
			return B_OK;
		}
		uint32* indirectBlocks = (uint32*)cached.SetTo(B_LENDIAN_TO_HOST_INT32(
			fStream->indirect));
		if (indirectBlocks == NULL)
			return B_IO_ERROR;

		block = B_LENDIAN_TO_HOST_INT32(indirectBlocks[index]);
		if (_count) {
			*_count = 1;
			uint32 nextBlock = block;
			while (++index < kIndirectsPerBlock
				&& indirectBlocks[index] == ++nextBlock)
				(*_count)++;
		}
	} else if ((index -= kIndirectsPerBlock) < kIndirectsPerBlock2) {
		// double indirect blocks
		CachedBlock cached(fVolume);
		if (fStream->double_indirect == 0) {
			block = 0;
			return B_OK;
		}
		uint32* indirectBlocks = (uint32*)cached.SetTo(B_LENDIAN_TO_HOST_INT32(
			fStream->double_indirect));
		if (indirectBlocks == NULL)
			return B_IO_ERROR;

		uint32 indirectIndex = B_LENDIAN_TO_HOST_INT32(indirectBlocks[index 
			/ kIndirectsPerBlock]);
		if (indirectIndex == 0) {
			// a sparse indirect block
			block = 0;
		} else {
			indirectBlocks = (uint32*)cached.SetTo(indirectIndex);
			if (indirectBlocks == NULL)
				return B_IO_ERROR;

			block = B_LENDIAN_TO_HOST_INT32(
				indirectBlocks[index & (kIndirectsPerBlock - 1)]);
			if (_count) {
				*_count = 1;
				uint32 nextBlock = block;
				while (((++index & (kIndirectsPerBlock - 1)) != 0)
					&& indirectBlocks[index & (kIndirectsPerBlock - 1)]
						== ++nextBlock)
					(*_count)++;
			}
		}
	} else if ((index -= kIndirectsPerBlock2) < kIndirectsPerBlock3) {
		// triple indirect blocks
		CachedBlock cached(fVolume);
		if (fStream->triple_indirect == 0) {
			block = 0;
			return B_OK;
		}
		uint32* indirectBlocks = (uint32*)cached.SetTo(B_LENDIAN_TO_HOST_INT32(
			fStream->triple_indirect));
		if (indirectBlocks == NULL)
			return B_IO_ERROR;

		uint32 indirectIndex = B_LENDIAN_TO_HOST_INT32(indirectBlocks[index
			/ kIndirectsPerBlock2]);
		if (indirectIndex == 0) {
			// a sparse indirect block
			block = 0;
		} else {
			indirectBlocks = (uint32*)cached.SetTo(indirectIndex);
			if (indirectBlocks == NULL)
				return B_IO_ERROR;

			indirectIndex = B_LENDIAN_TO_HOST_INT32(
				indirectBlocks[(index / kIndirectsPerBlock) & (kIndirectsPerBlock - 1)]);
			if (indirectIndex == 0) {
				// a sparse indirect block
				block = 0;
			} else {
				indirectBlocks = (uint32*)cached.SetTo(indirectIndex);
				if (indirectBlocks == NULL)
					return B_IO_ERROR;

				block = B_LENDIAN_TO_HOST_INT32(
					indirectBlocks[index & (kIndirectsPerBlock - 1)]);
				if (_count) {
					*_count = 1;
					uint32 nextBlock = block;
					while (((++index & (kIndirectsPerBlock - 1)) != 0)
						&& indirectBlocks[index & (kIndirectsPerBlock - 1)]
							== ++nextBlock)
						(*_count)++;
				}
			}
		}
	} else {
		// Outside of the possible data stream
		ERROR("ext2: block outside datastream!\n");
		return B_ERROR;
	}

	TRACE("FindBlock(offset %" B_PRIdOFF "): %" B_PRIu64" %" B_PRIu32 "\n", offset,
		block, _count != NULL ? *_count : 1);
	return B_OK;
}


status_t
DataStream::FillHoles(Transaction& transaction, off_t pos, off_t length,
	off_t& added)
{
	// As ExtentStream::FillHoles(): give blocks (and any missing indirect
	// blocks) to the holes of a sparse file that a write's pages land in,
	// since the page writer cannot write to a hole.
	added = 0;
	if (length <= 0)
		return B_OK;

	uint32 shift = fVolume->BlockShift();
	off_t first = (pos & ~(off_t)(B_PAGE_SIZE - 1)) >> shift;
	off_t end = ((pos + length + B_PAGE_SIZE - 1)
		& ~(off_t)(B_PAGE_SIZE - 1)) >> shift;
	if (end > fNumBlocks)
		end = fNumBlocks;

	fAddedBlocks = 0;
	fAllocated = 0;
	fZeroBuffer = NULL;
	status_t status = B_OK;
	for (off_t index = first; index < end; index++) {
		fsblock_t block;
		status = FindBlock(index << shift, block);
		if (status != B_OK)
			break;
		if (block != 0)
			continue;

		// at most the rest of the range (what isn't used goes back below)
		fWaiting = end - index;
		status = _FillHole(transaction, index);
		if (status != B_OK)
			break;
	}
	if (fAllocated != 0) {
		fVolume->FreeBlocks(transaction, fAllocatedPos, fAllocated);
		fAllocated = 0;
	}
	if (fZeroBuffer != NULL) {
		// the zeros must be on the disk before the new mapping can be
		ioctl(fVolume->Device(), B_FLUSH_DRIVE_CACHE);
		free(fZeroBuffer);
		fZeroBuffer = NULL;
	}

	added = fAddedBlocks;
	return status;
}


status_t
DataStream::_FillHole(Transaction& transaction, off_t index)
{
	CachedBlock cached1(fVolume);
	CachedBlock cached2(fVolume);
	CachedBlock cached3(fVolume);
	uint32* level1;
	uint32* level2;
	uint32* level3;
	uint32* slot;
	status_t status;

	if (index < kMaxDirect)
		slot = &fStream->direct[index];
	else if ((index -= kMaxDirect) < kIndirectsPerBlock) {
		status = _HoleLevel(transaction, &fStream->indirect, cached1, level1);
		if (status != B_OK)
			return status;
		slot = &level1[index];
	} else if ((index -= kIndirectsPerBlock) < kIndirectsPerBlock2) {
		status = _HoleLevel(transaction, &fStream->double_indirect, cached1,
			level1);
		if (status == B_OK) {
			status = _HoleLevel(transaction,
				&level1[index / kIndirectsPerBlock], cached2, level2);
		}
		if (status != B_OK)
			return status;
		slot = &level2[index % kIndirectsPerBlock];
	} else {
		index -= kIndirectsPerBlock2;
		status = _HoleLevel(transaction, &fStream->triple_indirect, cached1,
			level1);
		if (status == B_OK) {
			status = _HoleLevel(transaction,
				&level1[index / kIndirectsPerBlock2], cached2, level2);
		}
		if (status == B_OK) {
			status = _HoleLevel(transaction,
				&level2[(index / kIndirectsPerBlock) % kIndirectsPerBlock],
				cached3, level3);
		}
		if (status != B_OK)
			return status;
		slot = &level3[index % kIndirectsPerBlock];
	}

	if (*slot != 0)
		return B_OK;

	uint32 blockNum;
	status = _GetBlock(transaction, blockNum);
	if (status != B_OK)
		return status;

	// The block may hold anything, and a partial write of an uncached page
	// reads it back first: zero it before it is mapped. File data doesn't
	// go through the block cache, so write to the device directly.
	if (fZeroBuffer == NULL) {
		fZeroBuffer = calloc(1, kBlockSize);
		if (fZeroBuffer == NULL)
			return B_NO_MEMORY;
	}
	if (write_pos(fVolume->Device(), (off_t)blockNum * kBlockSize,
			fZeroBuffer, kBlockSize) != (ssize_t)kBlockSize) {
		return B_IO_ERROR;
	}

	*slot = B_HOST_TO_LENDIAN_INT32(blockNum);
	return B_OK;
}


status_t
DataStream::_HoleLevel(Transaction& transaction, uint32* pointer,
	CachedBlock& cached, uint32*& block)
{
	// the indirect block *pointer refers to, made writable; allocated (and
	// zeroed) first if it is missing
	uint32 blockNum = B_LENDIAN_TO_HOST_INT32(*pointer);
	bool clear = false;
	if (blockNum == 0) {
		status_t status = _GetBlock(transaction, blockNum);
		if (status != B_OK)
			return status;
		clear = true;
	}
	block = (uint32*)cached.SetToWritable(transaction, blockNum, clear);
	if (block == NULL) {
		if (clear) {
			fVolume->FreeBlocks(transaction, blockNum, 1);
			fAddedBlocks--;
		}
		return B_IO_ERROR;
	}
	if (clear)
		*pointer = B_HOST_TO_LENDIAN_INT32(blockNum);
	return B_OK;
}


status_t
DataStream::Enlarge(Transaction& transaction, off_t& numBlocks)
{
	TRACE("DataStream::Enlarge(): current size: %" B_PRIdOFF ", target size: %"
		B_PRIdOFF "\n", fNumBlocks, numBlocks);

	off_t targetBlocks = numBlocks;
	off_t oldNumBlocks = fNumBlocks;
	fWaiting = _BlocksNeeded(numBlocks);
	fAddedBlocks = 0;
	fFreedBlocks = 0;

	status_t status = _Enlarge(transaction, targetBlocks);
	if (status != B_OK) {
		// Undo what was done, so that the file keeps exactly the blocks it
		// had and the caller can fail the resize (a full volume, mostly).
		// Everything this call mapped, data or indirect blocks, lies below
		// targetBlocks and at or above oldNumBlocks, and the pointers
		// past what it got to are still 0, which _Shrink() skips.
		fNumBlocks = targetBlocks;
		fFreeStart = 0;
		fFreeCount = 0;
		fRemovedBlocks = 0;
		if (_Shrink(transaction, oldNumBlocks) != B_OK)
			_PerformFree(transaction);
		fNumBlocks = oldNumBlocks;
	}
	if (fAllocated != 0) {
		// allocated in one go but not handed out
		fVolume->FreeBlocks(transaction, fAllocatedPos, fAllocated);
		fAllocated = 0;
	}

	// The caller charges the inode's i_blocks with what is mapped now,
	// indirect blocks included.
	numBlocks = fAddedBlocks - fFreedBlocks;
	return status;
}


status_t
DataStream::_Enlarge(Transaction& transaction, off_t targetBlocks)
{
	status_t status;

	if (fNumBlocks <= kMaxDirect) {
		status = _AddForDirectBlocks(transaction, targetBlocks);

		if (status != B_OK) {
			ERROR("DataStream::Enlarge(): _AddForDirectBlocks() failed\n");
			return status;
		}

		TRACE("DataStream::Enlarge(): current size: %" B_PRIdOFF
			", target size: %" B_PRIdOFF "\n", fNumBlocks, targetBlocks);
	
		if (fNumBlocks == targetBlocks)
			return B_OK;
	}

	TRACE("DataStream::Enlarge(): indirect current size: %" B_PRIdOFF
		", target size: %" B_PRIdOFF "\n", fNumBlocks, targetBlocks);

	if (fNumBlocks <= kMaxIndirect) {
		status = _AddForIndirectBlock(transaction, targetBlocks);

		if (status != B_OK) {
			ERROR("DataStream::Enlarge(): _AddForIndirectBlock() failed\n");
			return status;
		}

		TRACE("DataStream::Enlarge(): current size: %" B_PRIdOFF
			", target size: %" B_PRIdOFF "\n", fNumBlocks, targetBlocks);
	
		if (fNumBlocks == targetBlocks)
			return B_OK;
	}

	TRACE("DataStream::Enlarge(): indirect2 current size: %" B_PRIdOFF
		", target size: %" B_PRIdOFF "\n", fNumBlocks, targetBlocks);

	if (fNumBlocks <= kMaxDoubleIndirect) {
		status = _AddForDoubleIndirectBlock(transaction, targetBlocks);

		if (status != B_OK) {
			ERROR("DataStream::Enlarge(): _AddForDoubleIndirectBlock() failed\n");
			return status;
		}

		TRACE("DataStream::Enlarge(): current size: %" B_PRIdOFF
			", target size: %" B_PRIdOFF "\n", fNumBlocks, targetBlocks);
	
		if (fNumBlocks == targetBlocks)
			return B_OK;
	}

	TRACE("DataStream::Enlarge(): indirect3 current size: %" B_PRIdOFF
		", target size: %" B_PRIdOFF "\n", fNumBlocks, targetBlocks);

	TRACE("DataStream::Enlarge(): allocated: %" B_PRIu32 ", waiting: %"
		B_PRIu32 "\n", fAllocated, fWaiting);

	return _AddForTripleIndirectBlock(transaction, targetBlocks);
}


status_t
DataStream::Shrink(Transaction& transaction, off_t& numBlocks)
{
	TRACE("DataStream::Shrink(): current size: %" B_PRIdOFF ", target size: %"
		B_PRIdOFF "\n", fNumBlocks, numBlocks);

	fFreeStart = 0;
	fFreeCount = 0;
	fRemovedBlocks = 0;
	fFreedBlocks = 0;

	status_t status = _Shrink(transaction, numBlocks);
	if (status != B_OK) {
		// the pointers of the pending run are cleared already
		_PerformFree(transaction);
	}

	// As in Enlarge(): report the blocks really released, indirect blocks
	// included, so that the caller can take them off i_blocks.
	numBlocks = fFreedBlocks;
	return status;
}


status_t
DataStream::_Shrink(Transaction& transaction, off_t numBlocks)
{
	off_t blocksToRemove = fNumBlocks - numBlocks;

	status_t status;

	if (numBlocks < kMaxDirect) {
		status = _RemoveFromDirectBlocks(transaction, numBlocks);

		if (status != B_OK) {
			ERROR("DataStream::Shrink(): _RemoveFromDirectBlocks() failed\n");
			return status;
		}

		if (fRemovedBlocks == blocksToRemove) {
			fNumBlocks -= fRemovedBlocks;

			return _PerformFree(transaction);
		}
	}

	if (numBlocks < kMaxIndirect) {
		status = _RemoveFromIndirectBlock(transaction, numBlocks);

		if (status != B_OK) {
			ERROR("DataStream::Shrink(): _RemoveFromIndirectBlock() failed\n");
			return status;
		}

		if (fRemovedBlocks == blocksToRemove) {
			fNumBlocks -= fRemovedBlocks;

			return _PerformFree(transaction);
		}
	}

	if (numBlocks < kMaxDoubleIndirect) {
		status = _RemoveFromDoubleIndirectBlock(transaction, numBlocks);

		if (status != B_OK) {
			ERROR("DataStream::Shrink(): _RemoveFromDoubleIndirectBlock() failed\n");
			return status;
		}

		if (fRemovedBlocks == blocksToRemove) {
			fNumBlocks -= fRemovedBlocks;

			return _PerformFree(transaction);
		}
	}

	status = _RemoveFromTripleIndirectBlock(transaction, numBlocks);

	if (status != B_OK) {
		ERROR("DataStream::Shrink(): _RemoveFromTripleIndirectBlock() failed\n");
		return status;
	}

	fNumBlocks -= fRemovedBlocks;

	return _PerformFree(transaction);
}


uint32
DataStream::_BlocksNeeded(off_t numBlocks)
{
	TRACE("DataStream::BlocksNeeded(): num blocks %" B_PRIdOFF "\n", numBlocks);
	off_t blocksNeeded = 0;

	if (numBlocks > fNumBlocks) {
		blocksNeeded += numBlocks - fNumBlocks;

		if (numBlocks > kMaxDirect) {
			if (fNumBlocks <= kMaxDirect)
				blocksNeeded += 1;

			if (numBlocks > kMaxIndirect) {
				if (fNumBlocks <= kMaxIndirect) {
					blocksNeeded += 2 + (numBlocks - kMaxIndirect - 1)
						/ kIndirectsPerBlock;
				} else {
					blocksNeeded += (numBlocks - kMaxIndirect - 1)
						/ kIndirectsPerBlock - (fNumBlocks
							- kMaxIndirect - 1) / kIndirectsPerBlock;
				}

				if (numBlocks > kMaxDoubleIndirect) {
					if (fNumBlocks <= kMaxDoubleIndirect) {
						blocksNeeded += 2 + (numBlocks - kMaxDoubleIndirect - 1)
							/ kIndirectsPerBlock2;
					} else {
						// the leaf blocks were counted with the double
						// indirect range above; add the new second level ones
						blocksNeeded += (numBlocks - kMaxDoubleIndirect - 1)
							/ kIndirectsPerBlock2 - (fNumBlocks
								- kMaxDoubleIndirect - 1) / kIndirectsPerBlock2;
					}
				}
			}
		}
	}

	TRACE("DataStream::BlocksNeeded(): %" B_PRIdOFF "\n", blocksNeeded);
	return blocksNeeded;
}


status_t
DataStream::_GetBlock(Transaction& transaction, uint32& blockNum)
{
	TRACE("DataStream::_GetBlock(): allocated: %" B_PRIu32 ", pos: %" B_PRIu64
		", waiting: %" B_PRIu32 "\n", fAllocated, fAllocatedPos, fWaiting);

	if (fAllocated == 0) {
		uint32 blockGroup = (fAllocatedPos - fFirstBlock)
			/ fVolume->BlocksPerGroup();
		if (blockGroup >= fVolume->NumGroups())
			blockGroup = 0;

		// fWaiting is an estimate, which can run out on a sparse file made
		// elsewhere; asking for at most 0 blocks would get 0, and the
		// fAllocated-- below would then hand out a block never allocated.
		status_t status = fVolume->AllocateBlocks(transaction, 1,
			fWaiting > 0 ? fWaiting : 1, blockGroup, fAllocatedPos,
			fAllocated);
		if (status != B_OK) {
			ERROR("DataStream::_GetBlock(): AllocateBlocks() failed()\n");
			return status;
		}
		if (fAllocatedPos > UINT_MAX)
			return B_FILE_TOO_LARGE;

		fWaiting -= min_c(fWaiting, fAllocated);

		TRACE("DataStream::_GetBlock(): newAllocated: %" B_PRIu32 ", newpos: %"
			B_PRIu64 ", newwaiting: %" B_PRIu32 "\n", fAllocated,
			fAllocatedPos, fWaiting);
	}

	fAllocated--;
	blockNum = (uint32)fAllocatedPos++;
	fAddedBlocks++;

	return B_OK;
}


status_t
DataStream::_PrepareBlock(Transaction& transaction, uint32* pos,
	uint32& blockNum, bool& clear)
{
	blockNum = B_LENDIAN_TO_HOST_INT32(*pos);
	clear = false;

	if (blockNum == 0) {
		status_t status = _GetBlock(transaction, blockNum);
		if (status != B_OK) {
			ERROR("DataStream::_PrepareBlock() _GetBlock() failed blockNum %"
				B_PRIu32 "\n", blockNum);
			return status;
		}

		*pos = B_HOST_TO_LENDIAN_INT32(blockNum);
		clear = true;
	}

	return B_OK;
}


status_t
DataStream::_AddBlocks(Transaction& transaction, uint32* block, off_t _count)
{
	off_t count = _count;
	TRACE("DataStream::_AddBlocks(): count: %" B_PRIdOFF "\n", count);

	while (count > 0) {
		uint32 blockNum;
		status_t status = _GetBlock(transaction, blockNum);
		if (status != B_OK)
			return status;

		*(block++) = B_HOST_TO_LENDIAN_INT32(blockNum);
		--count;
	}

	fNumBlocks += _count;

	return B_OK;
}


status_t
DataStream::_AddBlocks(Transaction& transaction, uint32* block, off_t start,
	off_t end, int recursion)
{
	TRACE("DataStream::_AddBlocks(): start: %" B_PRIdOFF ", end %" B_PRIdOFF
		", recursion: %d\n", start, end, recursion);

	bool clear;
	uint32 blockNum;
	status_t status = _PrepareBlock(transaction, block, blockNum, clear);
	if (status != B_OK)
		return status;

	CachedBlock cached(fVolume);	
	uint32* childBlock = (uint32*)cached.SetToWritable(transaction, blockNum,
		clear);
	if (childBlock == NULL) {
		if (clear) {
			// Never zeroed: don't leave it linked, or undoing this Enlarge()
			// would read its old contents as block numbers.
			*block = 0;
			fVolume->FreeBlocks(transaction, blockNum, 1);
			fAddedBlocks--;
		}
		return B_IO_ERROR;
	}

	if (recursion == 0)
		return _AddBlocks(transaction, &childBlock[start], end - start);

	uint32 elementWidth;
	if (recursion == 1)
		elementWidth = kIndirectsPerBlock;
	else if (recursion == 2)
		elementWidth = kIndirectsPerBlock2;
	else {
		panic("Undefined recursion level\n");
		elementWidth = 0;
	}

	uint32 elementPos = start / elementWidth;
	uint32 endPos = end / elementWidth;

	TRACE("DataStream::_AddBlocks(): element pos: %" B_PRIu32 ", end pos: %"
		B_PRIu32 "\n", elementPos, endPos);

	recursion--;

	if (elementPos == endPos) {
		return _AddBlocks(transaction, &childBlock[elementPos],
			start % elementWidth, end % elementWidth, recursion);
	}
	
	if (start % elementWidth != 0) {
		status = _AddBlocks(transaction, &childBlock[elementPos],
			start % elementWidth, elementWidth, recursion);
		if (status != B_OK) {
			ERROR("DataStream::_AddBlocks() _AddBlocks() start failed\n");
			return status;
		}

		elementPos++;
	}

	while (elementPos < endPos) {
		status = _AddBlocks(transaction, &childBlock[elementPos], 0,
			elementWidth, recursion);
		if (status != B_OK) {
			ERROR("DataStream::_AddBlocks() _AddBlocks() mid failed\n");
			return status;
		}

		elementPos++;
	}

	if (end % elementWidth != 0) {
		status = _AddBlocks(transaction, &childBlock[elementPos], 0,
			end % elementWidth, recursion);
		if (status != B_OK) {
			ERROR("DataStream::_AddBlocks() _AddBlocks() end failed\n");
			return status;
		}
	}
		
	return B_OK;
}


status_t
DataStream::_AddForDirectBlocks(Transaction& transaction, uint32 numBlocks)
{
	TRACE("DataStream::_AddForDirectBlocks(): current size: %" B_PRIdOFF
		", target size: %" B_PRIu32 "\n", fNumBlocks, numBlocks);
	uint32* direct = &fStream->direct[fNumBlocks];
	uint32 end = numBlocks > kMaxDirect ? kMaxDirect : numBlocks;

	return _AddBlocks(transaction, direct, end - fNumBlocks);
}


status_t
DataStream::_AddForIndirectBlock(Transaction& transaction, uint32 numBlocks)
{
	TRACE("DataStream::_AddForIndirectBlocks(): current size: %" B_PRIdOFF
		", target size: %" B_PRIu32 "\n", fNumBlocks, numBlocks);
	uint32 *indirect = &fStream->indirect;
	uint32 start = fNumBlocks - kMaxDirect;
	uint32 end = numBlocks - kMaxDirect;

	if (end > kIndirectsPerBlock)
		end = kIndirectsPerBlock;

	return _AddBlocks(transaction, indirect, start, end, 0);
}


status_t
DataStream::_AddForDoubleIndirectBlock(Transaction& transaction,
	uint32 numBlocks)
{
	TRACE("DataStream::_AddForDoubleIndirectBlock(): current size: %" B_PRIdOFF
		", target size: %" B_PRIu32 "\n", fNumBlocks, numBlocks);
	uint32 *doubleIndirect = &fStream->double_indirect;
	uint32 start = fNumBlocks - kMaxIndirect;
	uint32 end = numBlocks - kMaxIndirect;

	if (end > kIndirectsPerBlock2)
		end = kIndirectsPerBlock2;

	return _AddBlocks(transaction, doubleIndirect, start, end, 1);
}


status_t
DataStream::_AddForTripleIndirectBlock(Transaction& transaction,
	uint32 numBlocks)
{
	TRACE("DataStream::_AddForTripleIndirectBlock(): current size: %" B_PRIdOFF
		", target size: %" B_PRIu32 "\n", fNumBlocks, numBlocks);
	uint32 *tripleIndirect = &fStream->triple_indirect;
	uint32 start = fNumBlocks - kMaxDoubleIndirect;
	uint32 end = numBlocks - kMaxDoubleIndirect;

	return _AddBlocks(transaction, tripleIndirect, start, end, 2);
}


status_t
DataStream::_PerformFree(Transaction& transaction)
{
	TRACE("DataStream::_PerformFree(): start: %" B_PRIu32 ", count: %" B_PRIu32
		"\n", fFreeStart, fFreeCount);
	status_t status;

	if (fFreeCount == 0)
		status = B_OK;
	else
		status = fVolume->FreeBlocks(transaction, fFreeStart, fFreeCount);

	fFreeStart = 0;
	fFreeCount = 0;

	return status;
}


status_t
DataStream::_MarkBlockForRemoval(Transaction& transaction, uint32* block)
{
	
	TRACE("DataStream::_MarkBlockForRemoval(*(%p) = %" B_PRIu32
		"): free start: %" B_PRIu32 ", free count: %" B_PRIu32 "\n", block,
		B_LENDIAN_TO_HOST_INT32(*block), fFreeStart, fFreeCount);
	uint32 blockNum = B_LENDIAN_TO_HOST_INT32(*block);
	if (blockNum == 0) {
		// A hole in a sparse file: there is nothing to free, and nothing to
		// take off i_blocks.
		return B_OK;
	}
	*block = 0;
	fFreedBlocks++;

	if (blockNum != fFreeStart + fFreeCount) {
		if (fFreeCount != 0) {
			status_t status = fVolume->FreeBlocks(transaction, fFreeStart,
				fFreeCount);
			if (status != B_OK)
				return status;
		}

		fFreeStart = blockNum;
		fFreeCount = 0;
	}

	fFreeCount++;

	return B_OK;
}


status_t
DataStream::_FreeBlocks(Transaction& transaction, uint32* block, uint32 _count)
{
	uint32 count = _count;
	TRACE("DataStream::_FreeBlocks(%p, %" B_PRIu32 ")\n", block, count);

	while (count > 0) {
		status_t status = _MarkBlockForRemoval(transaction, block);
		if (status != B_OK)
			return status;

		block++;
		count--;
	}

	fRemovedBlocks += _count;

	return B_OK;
}


status_t
DataStream::_FreeBlocks(Transaction& transaction, uint32* block, off_t start,
	off_t end, bool freeParent, int recursion)
{
	// TODO: Designed specifically for shrinking. Perhaps make it more general?
	TRACE("DataStream::_FreeBlocks(%p, %" B_PRIdOFF ", %" B_PRIdOFF
		", %c, %d)\n", block, start, end, freeParent ? 't' : 'f', recursion);

	uint32 blockNum = B_LENDIAN_TO_HOST_INT32(*block);
	if (blockNum == 0) {
		// The whole range is a hole (a sparse file written elsewhere); don't
		// read block 0 as an indirect block. Shrink() still has to see the
		// range as removed.
		fRemovedBlocks += end - start;
		return B_OK;
	}

	if (freeParent) {
		status_t status = _MarkBlockForRemoval(transaction, block);
		if (status != B_OK)
			return status;
	}

	CachedBlock cached(fVolume);
	uint32* childBlock = (uint32*)cached.SetToWritable(transaction, blockNum);
	if (childBlock == NULL)
		return B_IO_ERROR;

	if (recursion == 0)
		return _FreeBlocks(transaction, &childBlock[start], end - start);

	uint32 elementWidth;
	if (recursion == 1)
		elementWidth = kIndirectsPerBlock;
	else if (recursion == 2)
		elementWidth = kIndirectsPerBlock2;
	else {
		panic("Undefinied recursion level\n");
		elementWidth = 0;
	}

	uint32 elementPos = start / elementWidth;
	uint32 endPos = end / elementWidth;

	recursion--;

	if (elementPos == endPos) {
		bool free = freeParent || start % elementWidth == 0;
		return _FreeBlocks(transaction, &childBlock[elementPos],
			start % elementWidth, end % elementWidth, free, recursion);
	}

	status_t status = B_OK;

	if (start % elementWidth != 0) {
		status = _FreeBlocks(transaction, &childBlock[elementPos],
			start % elementWidth, elementWidth, false, recursion);
		if (status != B_OK)
			return status;

		elementPos++;
	}

	while (elementPos < endPos) {
		status = _FreeBlocks(transaction, &childBlock[elementPos], 0,
			elementWidth, true, recursion);
		if (status != B_OK)
			return status;

		elementPos++;
	}

	if (end % elementWidth != 0) {
		status = _FreeBlocks(transaction, &childBlock[elementPos], 0,
			end % elementWidth, true, recursion);
	}

	return status;
}


status_t
DataStream::_RemoveFromDirectBlocks(Transaction& transaction, uint32 numBlocks)
{
	TRACE("DataStream::_RemoveFromDirectBlocks(): current size: %" B_PRIdOFF
		", target size: %" B_PRIu32 "\n", fNumBlocks, numBlocks);
	uint32* direct = &fStream->direct[numBlocks];
	off_t end = fNumBlocks > kMaxDirect ? kMaxDirect : fNumBlocks;

	return _FreeBlocks(transaction, direct, end - numBlocks);
}


status_t
DataStream::_RemoveFromIndirectBlock(Transaction& transaction, uint32 numBlocks)
{
	TRACE("DataStream::_RemoveFromIndirectBlock(): current size: %" B_PRIdOFF
		", target size: %" B_PRIu32 "\n", fNumBlocks, numBlocks);
	uint32* indirect = &fStream->indirect;
	off_t start = numBlocks <= kMaxDirect ? 0 : numBlocks - kMaxDirect;
	off_t end = fNumBlocks - kMaxDirect;

	if (end > kIndirectsPerBlock)
		end = kIndirectsPerBlock;

	bool freeAll = start == 0;

	return _FreeBlocks(transaction, indirect, start, end, freeAll, 0);
}


status_t
DataStream::_RemoveFromDoubleIndirectBlock(Transaction& transaction,
	uint32 numBlocks)
{
	TRACE("DataStream::_RemoveFromDoubleIndirectBlock(): current size: %" B_PRIdOFF
		", target size: %" B_PRIu32 "\n", fNumBlocks, numBlocks);
	uint32* doubleIndirect = &fStream->double_indirect;
	off_t start = numBlocks <= kMaxIndirect ? 0 : numBlocks - kMaxIndirect;
	off_t end = fNumBlocks - kMaxIndirect;

	if (end > kIndirectsPerBlock2)
		end = kIndirectsPerBlock2;

	bool freeAll = start == 0;

	return _FreeBlocks(transaction, doubleIndirect, start, end, freeAll, 1);
}


status_t
DataStream::_RemoveFromTripleIndirectBlock(Transaction& transaction,
	uint32 numBlocks)
{
	TRACE("DataStream::_RemoveFromTripleIndirectBlock(): current size: %" B_PRIdOFF
		", target size: %" B_PRIu32 "\n", fNumBlocks, numBlocks);
	uint32* tripleIndirect = &fStream->triple_indirect;
	off_t start = numBlocks <= kMaxDoubleIndirect ? 0
		: numBlocks - kMaxDoubleIndirect;
	off_t end = fNumBlocks - kMaxDoubleIndirect;

	bool freeAll = start == 0;

	return _FreeBlocks(transaction, tripleIndirect, start, end, freeAll, 2);
}
