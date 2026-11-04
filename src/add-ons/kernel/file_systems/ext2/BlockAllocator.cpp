/*
 * Copyright 2001-2011, Haiku Inc. All rights reserved.
 * This file may be used under the terms of the MIT License.
 *
 * Authors:
 *		Janito V. Ferreira Filho
 *		Jérôme Duval
 */


#include "BlockAllocator.h"

#include <util/AutoLock.h>

#include "BitmapBlock.h"
#include "Inode.h"


#undef ASSERT
//#define TRACE_EXT2
#ifdef TRACE_EXT2
#	define TRACE(x...) dprintf("\33[34mext2:\33[0m " x)
#	define ASSERT(x) { if (!(x)) kernel_debugger("ext2: assert failed: " #x "\n"); }
#else
#	define TRACE(x...) ;
#	define ASSERT(x) ;
#endif
#define ERROR(x...) dprintf("\33[34mext2:\33[0m " x)


class AllocationBlockGroup : public TransactionListener {
public:
						AllocationBlockGroup();
						~AllocationBlockGroup();

			status_t	Initialize(Volume* volume, uint32 blockGroup,
							uint32 numBits);

			bool		IsFull() const;

			status_t	Allocate(Transaction& transaction, fsblock_t start,
							uint32 length);
			status_t	Free(Transaction& transaction, uint32 start,
							uint32 length);
			status_t	FreeAll(Transaction& transaction);
			status_t	Check(uint32 start, uint32 length);

			uint32		NumBits() const;
			uint32		FreeBits() const;
			fsblock_t	Start() const;

			fsblock_t	LargestStart() const;
			uint32		LargestLength() const;
			uint32		FreeLengthAt(fsblock_t start, uint32 maximum);

			// TransactionListener implementation
			void		TransactionDone(bool success);
			void		RemovedFromTransaction();

private:
			struct MetadataRange {
				uint32	start;
				uint32	length;
			};
			enum {
				kMaxMetadataRanges = 4
					// the superblock and descriptor backups, the two
					// bitmaps and the inode table
			};

			status_t	_ScanFreeRanges(bool verify = false);
			void		_AddFreeRange(uint32 start, uint32 length);
			void		_LockInTransaction(Transaction& transaction);
			status_t	_InitGroup(Transaction& transaction);
			bool		_HasSuperBlock();
			uint32		_NumBaseMetadataBlocks();
			int32		_MetadataRanges(MetadataRange* ranges);
			void		_SetBlockBitmapChecksum(BitmapBlock& block);
			bool		_VerifyBlockBitmapChecksum(BitmapBlock& block);

			Volume*		fVolume;
			uint32		fBlockGroup;
			ext2_block_group* fGroupDescriptor;

			mutex		fLock;
			mutex		fTransactionLock;
			int32		fCurrentTransaction;

			fsblock_t	fStart;
			uint32		fNumBits;
			fsblock_t	fBitmapBlock;

			uint32		fFreeBits;
			uint32		fFirstFree;
			uint32		fLargestStart;
			uint32		fLargestLength;
			
			uint32		fPreviousFreeBits;
			uint32		fPreviousFirstFree;
			uint32		fPreviousLargestStart;
			uint32		fPreviousLargestLength;
};


AllocationBlockGroup::AllocationBlockGroup()
	:
	fVolume(NULL),
	fBlockGroup(0),
	fGroupDescriptor(NULL),
	fCurrentTransaction(-1),
	fStart(0),
	fNumBits(0),
	fBitmapBlock(0),
	fFreeBits(0),
	fFirstFree(0),
	fLargestStart(0),
	fLargestLength(0),
	fPreviousFreeBits(0),
	fPreviousFirstFree(0),
	fPreviousLargestStart(0),
	fPreviousLargestLength(0)
{
	mutex_init(&fLock, "ext2 allocation block group");
	mutex_init(&fTransactionLock, "ext2 allocation block group transaction");
}


AllocationBlockGroup::~AllocationBlockGroup()
{
	mutex_destroy(&fLock);
	mutex_destroy(&fTransactionLock);
}


status_t
AllocationBlockGroup::Initialize(Volume* volume, uint32 blockGroup,
	uint32 numBits)
{
	fVolume = volume;
	fBlockGroup = blockGroup;
	fNumBits = numBits;
	// numBits is this group's size, which is smaller for a short last group;
	// where a group starts depends on the volume's blocks per group only.
	fStart = (fsblock_t)blockGroup * fVolume->BlocksPerGroup()
		+ fVolume->FirstDataBlock();

	status_t status = fVolume->GetBlockGroup(blockGroup, &fGroupDescriptor);
	if (status != B_OK)
		return status;

	fBitmapBlock = fGroupDescriptor->BlockBitmap(fVolume->Has64bitFeature());
	if (fBitmapBlock == 0) {
		ERROR("AllocationBlockGroup(%" B_PRIu32 ")::Initialize(): "
			"blockBitmap is zero\n", fBlockGroup);
		return B_BAD_VALUE;
	}

	if (fGroupDescriptor->Flags() & EXT2_BLOCK_GROUP_BLOCK_UNINIT) {
		// The bitmap of this group has never been written: the blocks in use
		// are its own metadata, which _InitGroup() will mark. The free ranges
		// are the gaps between them.
		MetadataRange ranges[kMaxMetadataRanges];
		int32 count = _MetadataRanges(ranges);
		fFreeBits = 0;
		fFirstFree = fNumBits;
		fLargestStart = 0;
		fLargestLength = 0;
		uint32 start = 0;
		for (int32 i = 0; i <= count; i++) {
			uint32 end = i < count ? ranges[i].start : fNumBits;
			if (end > start)
				_AddFreeRange(start, end - start);
			if (i < count)
				start = ranges[i].start + ranges[i].length;
		}

		if (fGroupDescriptor->FreeBlocks(fVolume->Has64bitFeature())
				!= fFreeBits) {
			ERROR("AllocationBlockGroup(%" B_PRIu32 ")::Initialize(): "
				"uninitialized group has %" B_PRIu32 "/%" B_PRIu32 " blocks "
				"free after its metadata, but the group descriptor says %"
				B_PRIu32 "\n", fBlockGroup, fFreeBits, fNumBits,
				fGroupDescriptor->FreeBlocks(fVolume->Has64bitFeature()));
			return B_BAD_DATA;
		}
		TRACE("Group %" B_PRIu32 " is uninit\n", fBlockGroup);
		// An aborted transaction restores these: left at zero, the first
		// abort in this group would make it look full until the next mount.
		fPreviousFreeBits = fFreeBits;
		fPreviousFirstFree = fFirstFree;
		fPreviousLargestStart = fLargestStart;
		fPreviousLargestLength = fLargestLength;
		return B_OK;
	}
	
	status = _ScanFreeRanges(true);
	if (status != B_OK)
		return status;

	if (fGroupDescriptor->FreeBlocks(fVolume->Has64bitFeature())
		!= fFreeBits) {
		ERROR("AllocationBlockGroup(%" B_PRIu32 ",%" B_PRIu64 ")::Initialize()"
			": Mismatch between counted free blocks (%" B_PRIu32 "/%" B_PRIu32
			") and what is set on the group descriptor (%" B_PRIu32 ")\n",
			fBlockGroup, fBitmapBlock, fFreeBits, fNumBits,
			fGroupDescriptor->FreeBlocks(fVolume->Has64bitFeature()));
		return B_BAD_DATA;
	}

	fPreviousFreeBits = fFreeBits;
	fPreviousFirstFree = fFirstFree;
	fPreviousLargestStart = fLargestStart;
	fPreviousLargestLength = fLargestLength;
	
	return status;
}


status_t
AllocationBlockGroup::_ScanFreeRanges(bool verify)
{
	TRACE("AllocationBlockGroup::_ScanFreeRanges() for group %" B_PRIu32 "\n",
		fBlockGroup);
	BitmapBlock block(fVolume, fNumBits);

	if (!block.SetTo(fBitmapBlock))
		return B_ERROR;

	if (verify && !_VerifyBlockBitmapChecksum(block)) {
		ERROR("AllocationBlockGroup(%" B_PRIu32 ")::_ScanFreeRanges(): "
			"blockGroup verification failed\n", fBlockGroup);
		return B_BAD_DATA;
	}

	fFreeBits = 0;
	uint32 start = 0;
	uint32 end = 0;

	while (end < fNumBits) {
		block.FindNextUnmarked(start);
		ASSERT(block.CheckMarked(end, start - end));
		end = start;

		if (start != block.NumBits()) {
			block.FindNextMarked(end);
			_AddFreeRange(start, end - start);
			ASSERT(block.CheckUnmarked(fLargestStart, fLargestLength));
			start = end;
		}
	}

	return B_OK;
}


bool
AllocationBlockGroup::IsFull() const
{
	return fFreeBits == 0;
}


status_t
AllocationBlockGroup::Allocate(Transaction& transaction, fsblock_t _start,
	uint32 length)
{
	uint32 start = _start - fStart;
	TRACE("AllocationBlockGroup::Allocate(%" B_PRIu32 ",%" B_PRIu32 ")\n",
		start, length);
	if (length == 0)
		return B_OK;

	uint32 end = start + length;
	if (end > fNumBits)
		return B_BAD_VALUE;

	_LockInTransaction(transaction);
	status_t status = _InitGroup(transaction);
	if (status != B_OK)
		return status;

	BitmapBlock block(fVolume, fNumBits);

	if (!block.SetToWritable(transaction, fBitmapBlock))
		return B_ERROR;

	TRACE("AllocationBlockGroup::Allocate(): Largest range in %" B_PRIu32 "-%"
		B_PRIu32 "\n", fLargestStart, fLargestStart + fLargestLength);
	ASSERT(block.CheckUnmarked(fLargestStart, fLargestLength));
	
	if (!block.Mark(start, length)) {
		ERROR("Failed to allocate blocks from %" B_PRIu32 " to %" B_PRIu32
			". Some were already allocated.\n", start, start + length);
		return B_ERROR;
	}

	fFreeBits -= length;
	fGroupDescriptor->SetFreeBlocks(fFreeBits, fVolume->Has64bitFeature());
	_SetBlockBitmapChecksum(block);
	fVolume->WriteBlockGroup(transaction, fBlockGroup);

	if (fFirstFree >= start && fFirstFree < end)
		fFirstFree = end;

	// The range need not start at the largest free one (AllocateBlocks()
	// takes the run at a goal block first), nor lie inside it: the largest
	// free range is not always maximal, as Free() only joins it to a freed
	// range right next to it. Keep the larger part left on either side.
	uint32 largestEnd = fLargestStart + fLargestLength;
	if (start < largestEnd && end > fLargestStart) {
		uint32 firstLength = start > fLargestStart ? start - fLargestStart : 0;
		uint32 secondLength = end < largestEnd ? largestEnd - end : 0;

		if (firstLength >= secondLength) {
			fLargestLength = firstLength;
		} else {
			fLargestLength = secondLength;
			fLargestStart = end;
		}
	} else {
		// No need to revalidate the largest free range
		return B_OK;
	}

	TRACE("AllocationBlockGroup::Allocate(): Largest range in %" B_PRIu32 "-%"
		B_PRIu32 "\n", fLargestStart, fLargestStart + fLargestLength);
	ASSERT(block.CheckUnmarked(fLargestStart, fLargestLength));

	if (fLargestLength < fNumBits / 2)
		block.FindLargestUnmarkedRange(fLargestStart, fLargestLength);
	ASSERT(block.CheckUnmarked(fLargestStart, fLargestLength));

	return B_OK;
}


status_t
AllocationBlockGroup::Free(Transaction& transaction, uint32 start,
	uint32 length)
{
	TRACE("AllocationBlockGroup::Free(): start: %" B_PRIu32 ", length %"
		B_PRIu32 "\n", start, length);

	if (length == 0)
		return B_OK;

	uint32 end = start + length;
	if (end > fNumBits)
		return B_BAD_VALUE;

	_LockInTransaction(transaction);
	if (fGroupDescriptor->Flags() & EXT2_BLOCK_GROUP_BLOCK_UNINIT)
		panic("AllocationBlockGroup::Free() can't free blocks if uninit\n");

	BitmapBlock block(fVolume, fNumBits);

	if (!block.SetToWritable(transaction, fBitmapBlock))
		return B_ERROR;

	TRACE("AllocationBlockGroup::Free(): Largest range in %" B_PRIu32 "-%"
		B_PRIu32 "\n", fLargestStart, fLargestStart + fLargestLength);
	ASSERT(block.CheckUnmarked(fLargestStart, fLargestLength));

	if (!block.Unmark(start, length)) {
		ERROR("Failed to free blocks from %" B_PRIu32 " to %" B_PRIu32
			". Some were already freed.\n", start, start + length);
		return B_ERROR;
	}

	TRACE("AllocationGroup::Free(): Unmarked bits in bitmap\n");

	if (fFirstFree > start)
		fFirstFree = start;

	if (start + length == fLargestStart) {
		fLargestStart = start;
		fLargestLength += length;
	} else if (start == fLargestStart + fLargestLength) {
		fLargestLength += length;
	} else if (fLargestLength <= fNumBits / 2) {
		// May have merged with some free blocks, becoming the largest
		uint32 newEnd = start + length;
		block.FindNextMarked(newEnd);

		uint32 newStart = start;
		block.FindPreviousMarked(newStart);
		newStart++;

		if (newEnd - newStart > fLargestLength) {
			fLargestLength = newEnd - newStart;
			fLargestStart = newStart;
		}
	}

	TRACE("AllocationBlockGroup::Free(): Largest range in %" B_PRIu32 "-%"
		B_PRIu32 "\n", fLargestStart, fLargestStart + fLargestLength);
	ASSERT(block.CheckUnmarked(fLargestStart, fLargestLength));

	fFreeBits += length;
	fGroupDescriptor->SetFreeBlocks(fFreeBits, fVolume->Has64bitFeature());
	_SetBlockBitmapChecksum(block);
	fVolume->WriteBlockGroup(transaction, fBlockGroup);

	return B_OK;
}


status_t
AllocationBlockGroup::FreeAll(Transaction& transaction)
{
	return Free(transaction, 0, fNumBits);
}


uint32
AllocationBlockGroup::NumBits() const
{
	return fNumBits;
}


uint32
AllocationBlockGroup::FreeBits() const
{
	return fFreeBits;
}


fsblock_t
AllocationBlockGroup::Start() const
{
	return fStart;
}


fsblock_t
AllocationBlockGroup::LargestStart() const
{
	return fStart + fLargestStart;
}


uint32
AllocationBlockGroup::LargestLength() const
{
	return fLargestLength;
}


uint32
AllocationBlockGroup::FreeLengthAt(fsblock_t _start, uint32 maximum)
{
	// The length of the free run that starts at _start, up to maximum; 0 if
	// that block is in use. An uninitialized group has no valid bitmap yet.
	if (_start < fStart || _start - fStart >= fNumBits || fFreeBits == 0
		|| (fGroupDescriptor->Flags() & EXT2_BLOCK_GROUP_BLOCK_UNINIT) != 0) {
		return 0;
	}

	uint32 start = _start - fStart;
	BitmapBlock block(fVolume, fNumBits);
	if (!block.SetTo(fBitmapBlock))
		return 0;

	uint32 end = start;
	block.FindNextMarked(end);
	return min_c(end - start, maximum);
}


void
AllocationBlockGroup::_AddFreeRange(uint32 start, uint32 length)
{
	if (IsFull()) {
		fFirstFree = start;
		fLargestStart = start;
		fLargestLength = length;
	} else if (length > fLargestLength) {
		fLargestStart = start;
		fLargestLength = length;
	}

	fFreeBits += length;
}


void
AllocationBlockGroup::_LockInTransaction(Transaction& transaction)
{
	mutex_lock(&fLock);

	if (transaction.ID() != fCurrentTransaction) {
		mutex_unlock(&fLock);

		mutex_lock(&fTransactionLock);
		mutex_lock(&fLock);

		fCurrentTransaction = transaction.ID();
		transaction.AddListener(this);
	}

	mutex_unlock(&fLock);
}


status_t
AllocationBlockGroup::_InitGroup(Transaction& transaction)
{
	TRACE("AllocationBlockGroup::_InitGroup()\n");
	uint16 flags = fGroupDescriptor->Flags();
	if ((flags & EXT2_BLOCK_GROUP_BLOCK_UNINIT) == 0)
		return B_OK;

	TRACE("AllocationBlockGroup::_InitGroup() initing\n");

	// As ext4_init_block_bitmap(): whatever the block holds is not a bitmap
	// yet. Mark the group's own metadata, and, like ext4, the bits past the
	// end of a group shorter than the bitmap block.
	uint32 bitmapBits = fVolume->BlockSize() * 8;
	BitmapBlock blockBitmap(fVolume, bitmapBits);
	if (!blockBitmap.SetToWritable(transaction, fBitmapBlock))
		return B_ERROR;

	blockBitmap.Unmark(0, bitmapBits, true);

	MetadataRange ranges[kMaxMetadataRanges];
	int32 count = _MetadataRanges(ranges);
	for (int32 i = 0; i < count; i++) {
		if (!blockBitmap.Mark(ranges[i].start, ranges[i].length))
			return B_ERROR;
	}
	if (fNumBits < bitmapBits
		&& !blockBitmap.Mark(fNumBits, bitmapBits - fNumBits)) {
		return B_ERROR;
	}

	status_t status = _ScanFreeRanges();
	if (status != B_OK)
		return status;

	if (fGroupDescriptor->FreeBlocks(fVolume->Has64bitFeature())
		!= fFreeBits) {
		ERROR("AllocationBlockGroup(%" B_PRIu32 ",%" B_PRIu64 ")::_InitGroup()"
			": Mismatch between counted free blocks (%" B_PRIu32 "/%" B_PRIu32
			") and what is set on the group descriptor (%" B_PRIu32 ")\n",
			fBlockGroup, fBitmapBlock, fFreeBits, fNumBits,
			fGroupDescriptor->FreeBlocks(fVolume->Has64bitFeature()));
		return B_BAD_DATA;
	}

	// Only now that the bitmap is right: a group left uninitialized
	// ignores it.
	fGroupDescriptor->SetFlags(flags & ~EXT2_BLOCK_GROUP_BLOCK_UNINIT);
	_SetBlockBitmapChecksum(blockBitmap);
	fVolume->WriteBlockGroup(transaction, fBlockGroup);

	TRACE("AllocationBlockGroup::_InitGroup() init OK\n");

	return B_OK;
}


static_assert(offsetof(ext2_super_block, backup_block_groups) == 0x24c,
	"s_backup_bgs is at 0x24c in the ext4 superblock");


/*!	Whether this group holds a backup of the superblock (group 0 holds the
	superblock itself), as ext4_bg_has_super().
*/
bool
AllocationBlockGroup::_HasSuperBlock()
{
	if (fBlockGroup == 0)
		return true;

	ext2_super_block& superBlock = fVolume->SuperBlock();
	if ((superBlock.CompatibleFeatures() & EXT2_FEATURE_SPARSESUPER2) != 0) {
		return fBlockGroup == superBlock.BackupBlockGroup(0)
			|| fBlockGroup == superBlock.BackupBlockGroup(1);
	}
	if (fBlockGroup == 1 || (superBlock.ReadOnlyFeatures()
			& EXT2_READ_ONLY_FEATURE_SPARSE_SUPER) == 0) {
		return true;
	}

	// With sparse_super, the other backups are in the groups that are a
	// power of 3, 5 or 7: all odd.
	if ((fBlockGroup & 0x1) == 0)
		return false;

	uint32 i = fBlockGroup;
	while (i % 7 == 0)
		i /= 7;
	if (i == 1)
		return true;

	i = fBlockGroup;
	while (i % 5 == 0)
		i /= 5;
	if (i == 1)
		return true;

	i = fBlockGroup;
	while (i % 3 == 0)
		i /= 3;
	if (i == 1)
		return true;

	return false;
}


/*!	The number of blocks at the start of this group taken by the superblock
	(or its backup) and the group descriptor table that goes with it, as
	ext4_num_base_meta_blocks().
*/
uint32
AllocationBlockGroup::_NumBaseMetadataBlocks()
{
	ext2_super_block& superBlock = fVolume->SuperBlock();
	uint32 descriptorsPerBlock = fVolume->BlockSize()
		/ fVolume->GroupDescriptorSize();
	uint32 count = _HasSuperBlock() ? 1 : 0;

	if (!fVolume->HasMetaGroupFeature()
		|| fBlockGroup < superBlock.FirstMetaBlockGroup()
			* descriptorsPerBlock) {
		if (count != 0) {
			if (fVolume->HasMetaGroupFeature())
				count += superBlock.FirstMetaBlockGroup();
			else {
				count += (fVolume->NumGroups() + descriptorsPerBlock - 1)
					/ descriptorsPerBlock;
			}
			count += superBlock.ReservedGDTBlocks();
		}
	} else {
		// With meta_bg, a descriptor block is kept in the first, second and
		// last group of the groups it describes.
		uint32 first = fBlockGroup / descriptorsPerBlock * descriptorsPerBlock;
		if (fBlockGroup == first || fBlockGroup == first + 1
			|| fBlockGroup == first + descriptorsPerBlock - 1) {
			count++;
		}
	}

	return count;
}


/*!	Fills \a ranges with the blocks of this group its metadata uses, as
	ext4_init_block_bitmap() marks them: sorted, joined where they touch, and
	relative to the start of the group. Returns how many there are.
	The bitmaps and the inode table only count when they lie in this group,
	which with flex_bg is usually not the case. The metadata of other groups
	is not looked for: mke2fs and ext4 never leave a group that holds any
	uninitialized.
*/
int32
AllocationBlockGroup::_MetadataRanges(MetadataRange* ranges)
{
	bool has64bits = fVolume->Has64bitFeature();
	uint32 inodeTableBlocks = ((uint64)fVolume->InodesPerGroup()
		* fVolume->InodeSize() + fVolume->BlockSize() - 1)
		/ fVolume->BlockSize();

	struct {
		fsblock_t	start;
		uint32		length;
	} candidates[kMaxMetadataRanges] = {
		{ fStart, _NumBaseMetadataBlocks() },
		{ fGroupDescriptor->BlockBitmap(has64bits), 1 },
		{ fGroupDescriptor->InodeBitmap(has64bits), 1 },
		{ fGroupDescriptor->InodeTable(has64bits), inodeTableBlocks }
	};

	int32 count = 0;
	for (int32 i = 0; i < kMaxMetadataRanges; i++) {
		// Keep the part inside this group
		fsblock_t start = max_c(candidates[i].start, fStart);
		fsblock_t end = min_c(candidates[i].start + candidates[i].length,
			fStart + fNumBits);
		if (start >= end)
			continue;

		MetadataRange range = { (uint32)(start - fStart),
			(uint32)(end - start) };

		// Insert it in order
		int32 j = count;
		while (j > 0 && ranges[j - 1].start > range.start) {
			ranges[j] = ranges[j - 1];
			j--;
		}
		ranges[j] = range;
		count++;
	}

	// Join the ranges that overlap or touch
	int32 joined = 0;
	for (int32 i = 0; i < count; i++) {
		if (joined > 0 && ranges[i].start
				<= ranges[joined - 1].start + ranges[joined - 1].length) {
			uint32 end = max_c(ranges[joined - 1].start
				+ ranges[joined - 1].length, ranges[i].start
				+ ranges[i].length);
			ranges[joined - 1].length = end - ranges[joined - 1].start;
		} else
			ranges[joined++] = ranges[i];
	}

	return joined;
}


void
AllocationBlockGroup::_SetBlockBitmapChecksum(BitmapBlock& block)
{
	if (fVolume->HasMetaGroupChecksumFeature()) {
		uint32 checksum = block.Checksum(fVolume->BlocksPerGroup());
		fGroupDescriptor->block_bitmap_csum = checksum & 0xffff;
		if (fVolume->GroupDescriptorSize() >= offsetof(ext2_block_group,
			inode_bitmap_high)) {
			fGroupDescriptor->block_bitmap_csum_high = checksum >> 16;
		}
	}
}


bool
AllocationBlockGroup::_VerifyBlockBitmapChecksum(BitmapBlock& block) {
	if (fVolume->HasMetaGroupChecksumFeature()) {
		uint32 checksum = block.Checksum(fVolume->BlocksPerGroup());
		uint32 provided = fGroupDescriptor->block_bitmap_csum;
		if (fVolume->GroupDescriptorSize() >= offsetof(ext2_block_group,
			inode_bitmap_high)) {
			provided |= (fGroupDescriptor->block_bitmap_csum_high << 16);
		} else
			checksum &= 0xffff;
		return provided == checksum;
	}
	return true;
}


void
AllocationBlockGroup::TransactionDone(bool success)
{
	if (success) {
		TRACE("AllocationBlockGroup::TransactionDone(): The transaction "
			"succeeded, discarding previous state\n");
		fPreviousFreeBits = fFreeBits;
		fPreviousFirstFree = fFirstFree;
		fPreviousLargestStart = fLargestStart;
		fPreviousLargestLength = fLargestLength;
	} else {
		TRACE("AllocationBlockGroup::TransactionDone(): The transaction "
			"failed, restoring to previous state\n");
		fFreeBits = fPreviousFreeBits;
		fFirstFree = fPreviousFirstFree;
		fLargestStart = fPreviousLargestStart;
		fLargestLength = fPreviousLargestLength;
	}
}


void
AllocationBlockGroup::RemovedFromTransaction()
{
	mutex_unlock(&fTransactionLock);
	fCurrentTransaction = -1;
}


BlockAllocator::BlockAllocator(Volume* volume)
	:
	fVolume(volume),
	fGroups(NULL),
	fBlocksPerGroup(0),
	fNumBlocks(0),
	fNumGroups(0),
	fFirstBlock(0)
{
	mutex_init(&fLock, "ext2 block allocator");
}


BlockAllocator::~BlockAllocator()
{
	mutex_destroy(&fLock);

	if (fGroups != NULL)
		delete [] fGroups;
}


status_t
BlockAllocator::Initialize()
{
	fBlocksPerGroup = fVolume->BlocksPerGroup();
	fNumGroups = fVolume->NumGroups();
	fFirstBlock = fVolume->FirstDataBlock();
	fNumBlocks = fVolume->NumBlocks();
	
	TRACE("BlockAllocator::Initialize(): blocks per group: %" B_PRIu32
		", block groups: %" B_PRIu32 ", first block: %" B_PRIu64
		", num blocks: %" B_PRIu64 "\n", fBlocksPerGroup, fNumGroups,
		fFirstBlock, fNumBlocks);

	fGroups = new(std::nothrow) AllocationBlockGroup[fNumGroups];
	if (fGroups == NULL)
		return B_NO_MEMORY;

	TRACE("BlockAllocator::Initialize(): allocated allocation block groups\n");

	mutex_lock(&fLock);
		// Released by _Initialize

#if 0
	thread_id id = spawn_kernel_thread(
		(thread_func)BlockAllocator::_Initialize, "ext2 block allocator",
		B_LOW_PRIORITY, this);
	if (id < B_OK)
		return _Initialize(this);

	mutex_transfer_lock(&fLock, id);

	return resume_thread(id);
#else
	return _Initialize(this);
#endif
}


status_t
BlockAllocator::AllocateBlocks(Transaction& transaction, uint32 minimum,
	uint32 maximum, uint32& blockGroup, fsblock_t& start, uint32& length)
{
	TRACE("BlockAllocator::AllocateBlocks()\n");
	MutexLocker lock(fLock);
	TRACE("BlockAllocator::AllocateBlocks(): Acquired lock\n");

	TRACE("BlockAllocator::AllocateBlocks(): transaction: %" B_PRId32 ", min: "
		"%" B_PRIu32 ", max: %" B_PRIu32 ", block group: %" B_PRIu32 ", start:"
		" %" B_PRIu64 ", num groups: %" B_PRIu32 "\n", transaction.ID(),
		minimum, maximum, blockGroup, start, fNumGroups);

	fsblock_t bestStart = 0;
	uint32 bestLength = 0;
	uint32 bestGroup = 0;
	bool atGoal = false;

	// A caller passes the block that would continue its last run (the end
	// of the extent before the blocks it maps): when that one is free, take
	// the run there, so that the extent grows instead of a new one being
	// added, which would fill the extent tree faster.
	if (start >= fFirstBlock && start < fNumBlocks) {
		uint32 goalGroup = (start - fFirstBlock) / fBlocksPerGroup;
		if (goalGroup < fNumGroups) {
			uint32 goalLength = fGroups[goalGroup].FreeLengthAt(start,
				maximum);
			if (goalLength >= minimum) {
				bestStart = start;
				bestLength = goalLength;
				bestGroup = goalGroup;
				atGoal = true;
			}
		}
	}

	uint32 groupNum = blockGroup;

	AllocationBlockGroup* last = &fGroups[fNumGroups];
	AllocationBlockGroup* group = &fGroups[blockGroup];

	for (int32 iterations = atGoal ? 2 : 0; iterations < 2; iterations++) {
		for (; group < last; ++group, ++groupNum) {
			TRACE("BlockAllocator::AllocateBlocks(): Group %" B_PRIu32
				" has largest length of %" B_PRIu32 "\n", groupNum,
				group->LargestLength());

			if (group->LargestLength() > bestLength) {
				if (start <= group->LargestStart()) {
					bestStart = group->LargestStart();
					bestLength = group->LargestLength();
					bestGroup = groupNum;

					TRACE("BlockAllocator::AllocateBlocks(): Found a better "
						"range: block group: %" B_PRIu32 ", %" B_PRIu64 "-%"
						B_PRIu64 "\n", groupNum, bestStart,
						bestStart + bestLength);

					if (bestLength >= maximum)
						break;
				}
			}

			start = 0;
		}

		if (bestLength >= maximum)
			break;

		groupNum = 0;

		group = &fGroups[0];
		last = &fGroups[blockGroup + 1];
	}

	if (bestLength < minimum) {
		TRACE("BlockAllocator::AllocateBlocks(): best range (length %" B_PRIu32
			") doesn't have minimum length of %" B_PRIu32 "\n", bestLength,
			minimum);
		return B_DEVICE_FULL;
	}

	if (bestLength > maximum)
		bestLength = maximum;

	TRACE("BlockAllocator::AllocateBlocks(): Selected range: block group %"
		B_PRIu32 ", %" B_PRIu64 "-%" B_PRIu64 "\n", bestGroup, bestStart,
		bestStart + bestLength);

	status_t status = fGroups[bestGroup].Allocate(transaction, bestStart,
		bestLength);
	if (status != B_OK) {
		TRACE("BlockAllocator::AllocateBlocks(): Failed to allocate %" B_PRIu32
		" blocks inside block group %" B_PRIu32 ".\n", bestLength, bestGroup);
		return status;
	}

	start = bestStart;
	length = bestLength;
	blockGroup = bestGroup;

	return B_OK;
}


status_t
BlockAllocator::Allocate(Transaction& transaction, Inode* inode,
	off_t numBlocks, uint32 minimum, fsblock_t& start, uint32& allocated)
{
	if (numBlocks <= 0)
		return B_ERROR;
	if (start > fNumBlocks)
		return B_BAD_VALUE;

	uint32 group = inode->ID() / fVolume->InodesPerGroup();

	if (inode->Size() > 0) {
		// Try to allocate near it's last blocks
		ext2_data_stream* dataStream = &inode->Node().stream;
		uint32 numBlocks = inode->Size() / fVolume->BlockSize() + 1;
		uint32 lastBlock = 0;

		// DANGER! What happens with sparse files?
		if (numBlocks < EXT2_DIRECT_BLOCKS) {
			// Only direct blocks
			lastBlock = dataStream->direct[numBlocks];
		} else {
			numBlocks -= EXT2_DIRECT_BLOCKS - 1;
			uint32 numIndexes = fVolume->BlockSize() / 4;
				// block size / sizeof(int32)
			uint32 numIndexes2 = numIndexes * numIndexes;
			uint32 numIndexes3 = numIndexes2 * numIndexes;
			uint32 indexesInIndirect = numIndexes;
			uint32 indexesInDoubleIndirect = indexesInIndirect
				+ numIndexes2;
			// uint32 indexesInTripleIndirect = indexesInDoubleIndirect
				// + numIndexes3;

			uint32 doubleIndirectBlock = EXT2_DIRECT_BLOCKS + 1;
			uint32 indirectBlock = EXT2_DIRECT_BLOCKS;

			CachedBlock cached(fVolume);
			uint32* indirectData;

			if (numBlocks > indexesInDoubleIndirect) {
				// Triple indirect blocks
				indirectData = (uint32*)cached.SetTo(EXT2_DIRECT_BLOCKS + 2);
				if (indirectData == NULL)
					return B_IO_ERROR;

				uint32 index = (numBlocks - indexesInDoubleIndirect)
					/ numIndexes3;
				doubleIndirectBlock = indirectData[index];
			}

			if (numBlocks > indexesInIndirect) {
				// Double indirect blocks
				indirectData = (uint32*)cached.SetTo(doubleIndirectBlock);
				if (indirectData == NULL)
					return B_IO_ERROR;

				uint32 index = (numBlocks - indexesInIndirect) / numIndexes2;
				indirectBlock = indirectData[index];
			}

			indirectData = (uint32*)cached.SetTo(indirectBlock);
				if (indirectData == NULL)
					return B_IO_ERROR;

			uint32 index = numBlocks / numIndexes;
			lastBlock = indirectData[index];
		}

		group = (lastBlock - fFirstBlock) / fBlocksPerGroup;
	}

	// TODO: Apply some more policies

	return AllocateBlocks(transaction, minimum, minimum + 8, group, start,
		allocated);
}


status_t
BlockAllocator::Free(Transaction& transaction, fsblock_t start, uint32 length)
{
	TRACE("BlockAllocator::Free(%" B_PRIu64 ", %" B_PRIu32 ")\n", start,
		length);
	MutexLocker lock(fLock);

	if (start <= fFirstBlock) {
		panic("Trying to free superblock!\n");
		return B_BAD_VALUE;
	}

	if (length == 0)
		return B_OK;
	if (start > fNumBlocks || length > fNumBlocks)
		return B_BAD_VALUE;

	TRACE("BlockAllocator::Free(): first block: %" B_PRIu64
		", blocks per group: %" B_PRIu32 "\n", fFirstBlock, fBlocksPerGroup);

	start -= fFirstBlock;
	off_t end = start + length - 1;

	uint32 group = start / fBlocksPerGroup;
	if (group >= fNumGroups) {
		panic("BlockAllocator::Free() group %" B_PRIu32 " too big (fNumGroups "
			"%" B_PRIu32 ")\n", group, fNumGroups);
	}
	uint32 lastGroup = end / fBlocksPerGroup;
	start = start % fBlocksPerGroup;

	if (group == lastGroup)
		return fGroups[group].Free(transaction, start, length);

	TRACE("BlockAllocator::Free(): Freeing from group %" B_PRIu32 ": %"
		B_PRIu64 ", %" B_PRIu64 "\n", group,
		start, fGroups[group].NumBits() - start);

	status_t status = fGroups[group].Free(transaction, start,
		fGroups[group].NumBits() - start);
	if (status != B_OK)
		return status;

	for (++group; group < lastGroup; ++group) {
		TRACE("BlockAllocator::Free(): Freeing all from group %" B_PRIu32 "\n",
			group);
		status = fGroups[group].FreeAll(transaction);
		if (status != B_OK)
			return status;
	}

	TRACE("BlockAllocator::Free(): Freeing from group %" B_PRIu32 ": 0-%"
		B_PRIu64 " \n", group, end % fBlocksPerGroup);
	// end is the last block freed; when it is the last block of its group,
	// (end + 1) % fBlocksPerGroup would be 0 and free nothing
	return fGroups[group].Free(transaction, 0, end % fBlocksPerGroup + 1);
}


/*static*/ status_t
BlockAllocator::_Initialize(BlockAllocator* allocator)
{
	TRACE("BlockAllocator::_Initialize()\n");
	// fLock is already heald
	Volume* volume = allocator->fVolume;

	AllocationBlockGroup* groups = allocator->fGroups;
	uint32 numGroups = allocator->fNumGroups - 1;

	off_t freeBlocks = 0;
	TRACE("BlockAllocator::_Initialize(): free blocks: %" B_PRIdOFF "\n",
		freeBlocks);

	for (uint32 i = 0; i < numGroups; ++i) {
		status_t status = groups[i].Initialize(volume, i, 
			allocator->fBlocksPerGroup);
		if (status != B_OK) {
			mutex_unlock(&allocator->fLock);
			return status;
		}

		freeBlocks += groups[i].FreeBits();
		TRACE("BlockAllocator::_Initialize(): free blocks: %" B_PRIdOFF "\n",
			freeBlocks);
	}
	
	// Last block group may have less blocks
	status_t status = groups[numGroups].Initialize(volume, numGroups,
		allocator->fNumBlocks - allocator->fBlocksPerGroup * numGroups
		- allocator->fFirstBlock);
	if (status != B_OK) {
		mutex_unlock(&allocator->fLock);
		return status;
	}
	
	freeBlocks += groups[numGroups].FreeBits();

	TRACE("BlockAllocator::_Initialize(): free blocks: %" B_PRIdOFF "\n",
		freeBlocks);

	mutex_unlock(&allocator->fLock);

	if (freeBlocks != volume->NumFreeBlocks()) {
		TRACE("Counted free blocks (%" B_PRIdOFF ") doesn't match value in the"
			" superblock (%" B_PRIdOFF ").\n", freeBlocks,
			volume->NumFreeBlocks());
		return B_BAD_DATA;
	}

	return B_OK;
}

