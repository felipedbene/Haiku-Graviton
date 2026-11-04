/*
 * Copyright 2001-2011, Haiku Inc. All rights reserved.
 * This file may be used under the terms of the MIT License.
 *
 * Authors:
 *		Jérôme Duval
 */


#include "ExtentStream.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>

#include "CachedBlock.h"
#include "Inode.h"
#include "Volume.h"


#undef ASSERT
//#define TRACE_EXT2
#ifdef TRACE_EXT2
#	define TRACE(x...) dprintf("\33[34mext2:\33[0m ExtentStream::" x)
#	define ASSERT(x) { if (!(x)) kernel_debugger("ext2: assert failed: " #x "\n"); }
#else
#	define TRACE(x...) ;
#	define ASSERT(x) ;
#endif
#define ERROR(x...)	dprintf("\33[34mext2:\33[0m ExtentStream::" x)


ExtentStream::ExtentStream(Volume* volume, Inode* inode,
	ext2_extent_stream* stream, off_t size)
	:
	fVolume(volume),
	fInode(inode),
	fStream(stream),
	fFirstBlock(volume->FirstDataBlock()),
	fAllocatedPos(fFirstBlock),
	fPending(0),
	fSize(size)
{
	fNumBlocks = size == 0 ? 0 : ((size - 1) >> fVolume->BlockShift()) + 1;
}


ExtentStream::~ExtentStream()
{
}


status_t
ExtentStream::FindBlock(off_t offset, fsblock_t& block, uint32 *_count)
{
	fileblock_t index = offset >> fVolume->BlockShift();
	TRACE("FindBlock(%" B_PRIdOFF ", %" B_PRIu64 ")\n", offset, index);

	if (offset >= fSize) {
		TRACE("FindBlock: offset larger than inode size\n");
		return B_ENTRY_NOT_FOUND;
	}

	ext2_extent_stream *stream = fStream;
	if (!stream->extent_header.IsValid())
		panic("ExtentStream::FindBlock() invalid header\n");

	CachedBlock cached(fVolume);
	fileblock_t next = ~(fileblock_t)0;
		// where the next leaf starts: a hole at the end of this one ends there
	while (stream->extent_header.Depth() != 0) {
		TRACE("FindBlock() depth %d\n",
			stream->extent_header.Depth());
		int32 i = 1;
		while (i < stream->extent_header.NumEntries()
			&& stream->extent_index[i].LogicalBlock() <= index) {
			i++;
		}
		if (i < stream->extent_header.NumEntries()
			&& stream->extent_index[i].LogicalBlock() < next) {
			next = stream->extent_index[i].LogicalBlock();
		}
		TRACE("FindBlock() getting index %" B_PRId32 " at %" B_PRIu64 "\n",
			i - 1, stream->extent_index[i - 1].PhysicalBlock());
		stream = (ext2_extent_stream *)cached.SetTo(
			stream->extent_index[i - 1].PhysicalBlock());
		if (!stream->extent_header.IsValid())
			panic("ExtentStream::FindBlock() invalid header\n");
		if (!fInode->VerifyExtentChecksum(stream)) {
			panic("ExtentStream::FindBlock() invalid checksum\n");
			return B_BAD_DATA;
		}
	}

	// find the extend following the one that should contain the logical block
	int32 extentIndex;
	if (stream->extent_header.NumEntries() > 7) {
		// binary search when enough entries
		int32 low = 0;
		int32 high = stream->extent_header.NumEntries() - 1;
		while (low < high) {
			int32 middle = (high + low + 1) / 2;
			if (stream->extent_entries[middle].LogicalBlock() > index)
				high = middle - 1;
			else
				low = middle;
		}

		extentIndex = low + 1;
	} else {
		extentIndex = stream->extent_header.NumEntries();
		for (int32 i = 0; i < stream->extent_header.NumEntries(); i++) {
			if (stream->extent_entries[i].LogicalBlock() > index) {
				extentIndex = i;
				break;
			}
		}
	}

	fileblock_t logicalEndIndex
		= (fSize + fVolume->BlockSize() - 1) >> fVolume->BlockShift();
	if (logicalEndIndex > next)
		logicalEndIndex = next;

	if (extentIndex == 0) {
		// sparse block at the beginning of the stream
		block = 0;
		if (_count != NULL) {
			*_count = stream->extent_header.NumEntries() == 0
				? logicalEndIndex - index
				: stream->extent_entries[0].LogicalBlock() - index;
		}
		TRACE("FindBlock() sparse block index %" B_PRIu64 " at beginning of "
			"stream\n", index);
		return B_OK;
	}

	const ext2_extent_entry& extent = stream->extent_entries[extentIndex - 1];
		// the extent supposedly containing the offset
	fileblock_t diff = index - extent.LogicalBlock();
	if (diff >= extent.Length()) {
		// sparse block between extends or at the end of the stream
		TRACE("FindBlock() sparse block index %" B_PRIu64 " at %" B_PRIu32
			"\n", index, extent.LogicalBlock());
		block = 0;
		if (_count != NULL) {
			*_count = stream->extent_header.NumEntries() == extentIndex
				? logicalEndIndex - index
				: stream->extent_entries[extentIndex].LogicalBlock() - index;
		}
		return B_OK;
	}

	block = extent.PhysicalBlock() + diff;
	if (_count != NULL)
		*_count = extent.Length() - diff;

	TRACE("FindBlock(offset %" B_PRIdOFF "): %" B_PRIu64 " %" B_PRIu32
		"\n", offset, block, _count != NULL ? *_count : 1);

	return B_OK;
}


status_t
ExtentStream::Enlarge(Transaction& transaction, off_t& numBlocks)
{
	TRACE("Enlarge(): current size: %" B_PRIdOFF ", target size: %" B_PRIdOFF
		"\n", fNumBlocks, numBlocks);

	off_t targetBlocks = numBlocks;
	fPending = 0;
	numBlocks = 0;
		// Counts the blocks mapped, data and tree blocks, for the caller's
		// i_blocks.

	// Linux can preallocate past the end of the file (fallocate with
	// FALLOC_FL_KEEP_SIZE). Those extents lie beyond fNumBlocks, which
	// comes from the size, where new extents are appended; give them back
	// first, so the tree stays in order. They hold no visible data.
	off_t mappedEnd;
	status_t status = _MappedEnd(mappedEnd);
	if (status != B_OK)
		return status;
	if (mappedEnd > fNumBlocks) {
		off_t freed = fNumBlocks;
		status = Shrink(transaction, freed);
		numBlocks -= freed;
		if (status != B_OK)
			return status;
	}

	off_t oldNumBlocks = fNumBlocks;

	status = _Enlarge(transaction, targetBlocks, numBlocks);
	if (status != B_OK) {
		// Undo what was done, so that the file keeps exactly the blocks it
		// had and the caller can fail the resize (a full volume, mostly).
		// Data blocks already allocated but not yet in an extent go back
		// first, then Shrink() frees what made it into the tree.
		if (fPending != 0) {
			fVolume->FreeBlocks(transaction, fAllocatedPos, fPending);
			fPending = 0;
		}
		off_t freed = oldNumBlocks;
		Shrink(transaction, freed);
		numBlocks -= freed;
	}
	return status;
}


status_t
ExtentStream::_Enlarge(Transaction& transaction, off_t targetBlocks,
	off_t& numBlocks)
{
	while (fNumBlocks < targetBlocks) {
		// allocate new blocks
		uint32 blockGroup = (fAllocatedPos - fFirstBlock)
				/ fVolume->BlocksPerGroup();
		if (blockGroup >= fVolume->NumGroups())
			blockGroup = 0;

		if (fPending == 0) {
			// One run becomes one extent, which holds at most
			// EXT2_EXTENT_MAX_LENGTH blocks.
			status_t status = fVolume->AllocateBlocks(transaction, 1,
				min_c(targetBlocks - fNumBlocks, (off_t)EXT2_EXTENT_MAX_LENGTH),
				blockGroup, fAllocatedPos, fPending);
			if (status != B_OK) {
				ERROR("Enlarge(): AllocateBlocks() failed()\n");
				return status;
			}
		}

		ASSERT(_CheckBlock(fStream, fAllocatedPos) == B_OK);

		ext2_extent_stream *stream = fStream;
		fsblock_t path[stream->extent_header.Depth()];
	
		// search for the leaf
		CachedBlock cached(fVolume);
		int32 level = -1;
		for (; stream->extent_header.Depth() != 0;) {
			if (stream->extent_header.NumEntries() == 0)
				panic("stream->extent_header.NumEntries() == 0\n");
			int32 lastIndex = stream->extent_header.NumEntries() - 1;
			TRACE("Enlarge() depth %d\n", stream->extent_header.Depth());
			TRACE("Enlarge() getting index %" B_PRId32 " at %" B_PRIu64 "\n",
				lastIndex, stream->extent_index[lastIndex].PhysicalBlock());
			path[++level] = stream->extent_index[lastIndex].PhysicalBlock();
			stream = (ext2_extent_stream *)cached.SetTo(path[level]);
			if (stream == NULL)
				return B_IO_ERROR;
		}

		// check if the new extent is adjacent
		if (stream->extent_header.NumEntries() > 0) {
			ext2_extent_entry &last = stream->extent_entries[
				stream->extent_header.NumEntries() - 1];
			TRACE("Enlarge() last %" B_PRIu64 " allocatedPos %" B_PRIu64 "\n",
				last.PhysicalBlock() + last.Length(), fAllocatedPos);
			// The blocks must follow on logically too: a sparse file (made
			// by Linux) can end in a hole, and then the new blocks start a
			// new extent past it. A preallocated (uninitialized) extent can't
			// take written blocks either.
			if (last.PhysicalBlock() + last.Length() == fAllocatedPos
				&& last.LogicalBlock() + last.Length() == fNumBlocks
				&& !last.IsUninitialized()
				&& (last.Length() + fPending) <= EXT2_EXTENT_MAX_LENGTH) {
				if (stream != fStream) {
					stream = (ext2_extent_stream *)cached.SetToWritable(
						transaction, cached.BlockNumber());
					if (stream == NULL)
						return B_IO_ERROR;
				}
				stream->extent_entries[stream->extent_header.NumEntries() - 1]
					.SetLength(last.Length() + fPending);
				fInode->SetExtentChecksum(stream);
				fNumBlocks += fPending;
				numBlocks += fPending;
				fPending = 0;
				TRACE("Enlarge() entry extended\n");
				continue;
			}
		}

		if (stream->extent_header.NumEntries()
			>= stream->extent_header.MaxEntries()) {
			TRACE("Enlarge() adding leaf and indexes at depth %d level %"
				B_PRId32 "\n", stream->extent_header.Depth(), level);
			// try to add a leaf and indexes
			while (--level >= 0) {
				stream = (ext2_extent_stream *)cached.SetTo(path[level]);
				if (stream == NULL)
					return B_IO_ERROR;
				if (stream->extent_header.NumEntries()
					< stream->extent_header.MaxEntries()) {
					break;
				}
				TRACE("Enlarge() going up from level %" B_PRId32 "\n", level);
			}

			if (level < 0 && fStream->extent_header.NumEntries()
					>= fStream->extent_header.MaxEntries()) {
				TRACE("Enlarge() add a level, increment root depth\n");
				cached.Unset();
				status_t status = _GrowDepth(transaction);
				if (status != B_OK) {
					ERROR("Enlarge(): growing the tree failed\n");
					return status;
				}
				numBlocks++;
				ASSERT(Check());
				continue;
			}

			if (level < 0)
				stream = fStream;
			else {
				// the new index entry goes into this block
				stream = (ext2_extent_stream *)cached.SetToWritable(
					transaction, path[level]);
				if (stream == NULL)
					return B_IO_ERROR;
			}

			// A node at depth d needs d new blocks below it: d - 1 index
			// blocks and a leaf. Get them all first, set them up as a chain
			// of nodes with one entry each, and only then link the chain
			// into the tree: running out of space (or an I/O error) on the
			// way can't leave an empty index block in the tree, which
			// Shrink() and the next Enlarge() could not walk.
			uint16 depth = stream->extent_header.Depth();
			fsblock_t newBlocks[kMaxTreeDepth];
			if (depth == 0 || depth > kMaxTreeDepth)
				return B_BAD_DATA;
			for (uint16 i = 0; i < depth; i++) {
				newBlocks[i] = level >= 0 ? path[level]
					: fStream->extent_index[0].PhysicalBlock();
				uint32 _allocated;
				status_t status = fVolume->AllocateBlocks(transaction, 1, 1,
					blockGroup, newBlocks[i], _allocated);
				if (status != B_OK) {
					ERROR("Enlarge(): AllocateBlocks() failed()\n");
					while (i-- > 0)
						fVolume->FreeBlocks(transaction, newBlocks[i], 1);
					return status;
				}
				ASSERT(_CheckBlock(fStream, newBlocks[i]) == B_OK);
			}
			for (uint16 i = 0; i < depth; i++) {
				CachedBlock nodeCached(fVolume);
				ext2_extent_stream* node = (ext2_extent_stream*)
					nodeCached.SetToWritable(transaction, newBlocks[i], true);
				if (node == NULL) {
					for (uint16 j = 0; j < depth; j++)
						fVolume->FreeBlocks(transaction, newBlocks[j], 1);
					return B_IO_ERROR;
				}
				uint16 nodeDepth = depth - 1 - i;
				node->extent_header.magic = EXT2_EXTENT_MAGIC;
				node->extent_header.SetMaxEntries((fVolume->BlockSize()
					- sizeof(ext2_extent_header)) / (nodeDepth > 0
						? sizeof(ext2_extent_index) : sizeof(ext2_extent_entry)));
				node->extent_header.SetDepth(nodeDepth);
				node->extent_header.SetGeneration(0);
				if (nodeDepth > 0) {
					node->extent_index[0].SetLogicalBlock(fNumBlocks);
					node->extent_index[0].SetPhysicalBlock(newBlocks[i + 1]);
					node->extent_header.SetNumEntries(1);
				} else
					node->extent_header.SetNumEntries(0);
				fInode->SetExtentChecksum(node);
			}

			TRACE("Enlarge() linking %d new tree blocks at depth %d level %"
				B_PRId32 "\n", depth, depth, level);
			int32 index = stream->extent_header.NumEntries();
			stream->extent_index[index].SetLogicalBlock(fNumBlocks);
			stream->extent_index[index].SetPhysicalBlock(newBlocks[0]);
			stream->extent_header.SetNumEntries(index + 1);
			fInode->SetExtentChecksum(stream);
			numBlocks += depth;

			// the new entry goes into the new leaf
			stream = (ext2_extent_stream *)cached.SetToWritable(transaction,
				newBlocks[depth - 1]);
			if (stream == NULL)
				return B_IO_ERROR;
			ASSERT(Check());
		}
		
		// add a new entry
		TRACE("Enlarge() add entry %" B_PRIu64 "\n", fAllocatedPos);
		if (stream != fStream) {
			stream = (ext2_extent_stream *)cached.SetToWritable(
				transaction, cached.BlockNumber());
			if (stream == NULL)
				return B_IO_ERROR;
		}
		int32 index = stream->extent_header.NumEntries();
		stream->extent_entries[index].SetLogicalBlock(fNumBlocks);
		stream->extent_entries[index].SetLength(fPending);
		stream->extent_entries[index].SetPhysicalBlock(fAllocatedPos);
		stream->extent_header.SetNumEntries(index + 1);
		fInode->SetExtentChecksum(stream);
		TRACE("Enlarge() entry added at index %" B_PRId32 "\n", index);
		ASSERT(stream->extent_header.IsValid());

		fNumBlocks += fPending;
		numBlocks += fPending;
		fPending = 0;
	}
	
	return B_OK;
}


status_t
ExtentStream::Shrink(Transaction& transaction, off_t& numBlocks)
{
	TRACE("DataStream::Shrink(): current size: %" B_PRIdOFF ", target size: %"
		B_PRIdOFF "\n", fNumBlocks, numBlocks);

	off_t targetBlocks = numBlocks;
	numBlocks = 0;
		// As in Enlarge(), counts the data and tree blocks really freed.

	// Extents preallocated past the end of the file go too.
	off_t mappedEnd;
	status_t mappedStatus = _MappedEnd(mappedEnd);
	if (mappedStatus != B_OK)
		return mappedStatus;
	if (mappedEnd > fNumBlocks)
		fNumBlocks = mappedEnd;

	while (targetBlocks < fNumBlocks) {
		ext2_extent_stream *stream = fStream;
		fsblock_t path[stream->extent_header.Depth()];
	
		// search for the leaf
		CachedBlock cached(fVolume);
		int32 level = -1;
		for (; stream->extent_header.Depth() != 0;) {
			if (stream->extent_header.NumEntries() == 0)
				panic("stream->extent_header.NumEntries() == 0\n");
			int32 lastIndex = stream->extent_header.NumEntries() - 1;
			TRACE("Shrink() depth %d\n", stream->extent_header.Depth());
			TRACE("Shrink() getting index %" B_PRId32 " at %" B_PRIu64 "\n",
				lastIndex, stream->extent_index[lastIndex].PhysicalBlock());
			path[++level] = stream->extent_index[lastIndex].PhysicalBlock();
			stream = (ext2_extent_stream *)cached.SetToWritable(transaction, 
				path[level]);
			if (stream == NULL)
				return B_IO_ERROR;
			if (!stream->extent_header.IsValid())
				panic("Shrink() invalid header\n");
		}
		
		if (stream == fStream && stream->extent_header.NumEntries() == 0) {
			// An empty tree: whatever is left above the target is a hole.
			// (An empty leaf below the root is removed further down.)
			fNumBlocks = targetBlocks;
			break;
		}

		// fNumBlocks comes from the file size, and a sparse file (made by
		// Linux, say) need not have an extent reaching up to it: the last
		// extent can end below it, or below the target. Only the blocks of
		// an extent that lie at or above the target are freed.
		int32 index = stream->extent_header.NumEntries() - 1;
		status_t status = B_OK;
		while (index >= 0 && targetBlocks < fNumBlocks) {
			ext2_extent_entry &last = stream->extent_entries[index];
			off_t start = last.LogicalBlock();
			off_t end = start + last.Length();
			if (end <= targetBlocks) {
				// the rest, up to fNumBlocks, is a hole
				fNumBlocks = targetBlocks;
				break;
			}
			if (start < targetBlocks)
				start = targetBlocks;
			uint16 length = end - start;
			fsblock_t block = last.PhysicalBlock()
				+ (start - last.LogicalBlock());
			TRACE("Shrink() free block %" B_PRIu64 " length %d\n", block,
				length);
			status = fVolume->FreeBlocks(transaction, block, length);
			if (status != B_OK)
				break;
			fNumBlocks = start;
			numBlocks += length;
			stream->extent_entries[index].SetLength(last.Length() - length,
				last.IsUninitialized());
			TRACE("Shrink() new length for %" B_PRId32 ": %d\n", index, last.Length());
			if (last.Length() != 0)
				break;
			index--;
			TRACE("Shrink() next index: %" B_PRId32 "\n", index);
		}
		TRACE("Shrink() new entry count: %" B_PRId32 "\n", index + 1);
		stream->extent_header.SetNumEntries(index + 1);
		fInode->SetExtentChecksum(stream);
		ASSERT(Check());
		
		if (status != B_OK)
			return status;

		while (stream != fStream && stream->extent_header.NumEntries() == 0) {
			TRACE("Shrink() empty leaf at depth %d\n", 
				stream->extent_header.Depth());
			level--;
			if (level >= 0) {
				stream = (ext2_extent_stream *)cached.SetToWritable(
					transaction, path[level]);
				if (stream == NULL)
					return B_IO_ERROR;
			} else
				stream = fStream;
			if (!stream->extent_header.IsValid())
				panic("Shrink() invalid header\n");
			uint16 index = stream->extent_header.NumEntries() - 1;
			ext2_extent_index &last = stream->extent_index[index];
			if (last.PhysicalBlock() != path[level + 1])
				panic("Shrink() last.PhysicalBlock() != path[level + 1]\n");
			status = fVolume->FreeBlocks(transaction, last.PhysicalBlock(), 1);
			if (status != B_OK)
				return status;
			numBlocks++;
			stream->extent_header.SetNumEntries(index);
			fInode->SetExtentChecksum(stream);
			TRACE("Shrink() new entry count: %d\n", index);
		}
		if (stream == fStream && stream->extent_header.NumEntries() == 0)
			stream->extent_header.SetDepth(0);

		ASSERT(Check());
	}

	return B_OK;
}


status_t
ExtentStream::_LookUp(fileblock_t first, CachedBlock& cached,
	ext2_extent_stream*& stream, fsblock_t& leafBlock, int32& index,
	fileblock_t& next)
{
	// Finds the leaf that would hold logical block "first", and in it the
	// first extent that ends after it (index == the entry count if there is
	// none). "next" is where the following leaf starts, or ~0.
	stream = fStream;
	leafBlock = 0;
	next = ~(fileblock_t)0;
	while (stream->extent_header.Depth() != 0) {
		int32 count = stream->extent_header.NumEntries();
		if (count == 0)
			return B_BAD_DATA;
		int32 i = 0;
		while (i + 1 < count
			&& stream->extent_index[i + 1].LogicalBlock() <= first) {
			i++;
		}
		if (i + 1 < count && stream->extent_index[i + 1].LogicalBlock() < next)
			next = stream->extent_index[i + 1].LogicalBlock();
		leafBlock = stream->extent_index[i].PhysicalBlock();
		stream = (ext2_extent_stream*)cached.SetTo(leafBlock);
		if (stream == NULL)
			return B_IO_ERROR;
		if (!stream->extent_header.IsValid()
			|| !fInode->VerifyExtentChecksum(stream)) {
			return B_BAD_DATA;
		}
	}

	int32 count = stream->extent_header.NumEntries();
	index = 0;
	while (index < count && stream->extent_entries[index].LogicalBlock()
			+ stream->extent_entries[index].Length() <= first) {
		index++;
	}
	return B_OK;
}


status_t
ExtentStream::MarkWritten(Transaction& transaction, off_t pos, off_t length,
	bool& changed)
{
	changed = false;
	if (length <= 0)
		return B_OK;

	// The file cache writes whole pages, so every block of the pages the
	// write touches has to be written data on disk.
	uint32 shift = fVolume->BlockShift();
	fileblock_t first = (pos & ~(off_t)(B_PAGE_SIZE - 1)) >> shift;
	fileblock_t end = ((pos + length + B_PAGE_SIZE - 1)
		& ~(off_t)(B_PAGE_SIZE - 1)) >> shift;
	if (end > (fileblock_t)fNumBlocks)
		end = fNumBlocks;

	bool zeroed = false;
	status_t status = B_OK;
	while (first < end) {
		CachedBlock cached(fVolume);
		ext2_extent_stream* stream;
		fsblock_t leafBlock;
		int32 i;
		fileblock_t next;
		status = _LookUp(first, cached, stream, leafBlock, i, next);
		if (status != B_OK)
			break;
		if (i == stream->extent_header.NumEntries()) {
			// a hole up to the next leaf
			first = next;
			continue;
		}

		const ext2_extent_entry& extent = stream->extent_entries[i];
		if (extent.LogicalBlock() >= end)
			break;
		if (extent.IsUninitialized()) {
			// Zero the whole extent on disk, then flag it written: the
			// blocks the write doesn't cover must still read as zeros.
			// (Splitting the extent instead would avoid the zeroing, but
			// can need room in the tree that isn't there.)
			status = _ZeroBlocks(extent.PhysicalBlock(), extent.Length());
			if (status != B_OK)
				break;
			zeroed = true;

			if (leafBlock != 0) {
				stream = (ext2_extent_stream*)cached.SetToWritable(
					transaction, leafBlock);
				if (stream == NULL) {
					status = B_IO_ERROR;
					break;
				}
			}
			ext2_extent_entry& written = stream->extent_entries[i];
			written.SetLength(written.Length(), false);
			fInode->SetExtentChecksum(stream);
			changed = true;
		}
		first = (fileblock_t)stream->extent_entries[i].LogicalBlock()
			+ stream->extent_entries[i].Length();
	}

	if (zeroed) {
		// the zeros must be on the disk before the flag change can be
		ioctl(fVolume->Device(), B_FLUSH_DRIVE_CACHE);
	}
	return status;
}


status_t
ExtentStream::ZeroUninitialized(off_t pos, size_t length, const iovec* vecs,
	size_t count)
{
	// Called after a read of [pos, pos + length) into vecs: the parts that
	// lie in preallocated (uninitialized) extents must read as zeros, not
	// as whatever their blocks hold.
	if (length == 0)
		return B_OK;

	uint32 shift = fVolume->BlockShift();
	fileblock_t first = pos >> shift;
	fileblock_t end = (pos + length + fVolume->BlockSize() - 1) >> shift;

	while (first < end) {
		CachedBlock cached(fVolume);
		ext2_extent_stream* stream;
		fsblock_t leafBlock;
		int32 i;
		fileblock_t next;
		status_t status = _LookUp(first, cached, stream, leafBlock, i, next);
		if (status != B_OK)
			return status;
		if (i == stream->extent_header.NumEntries()) {
			first = next;
			continue;
		}

		const ext2_extent_entry& extent = stream->extent_entries[i];
		if (extent.LogicalBlock() >= end)
			return B_OK;
		off_t extentEnd = ((off_t)extent.LogicalBlock() + extent.Length())
			<< shift;
		if (extent.IsUninitialized()) {
			off_t from = max_c(pos, (off_t)extent.LogicalBlock() << shift);
			off_t to = min_c(pos + (off_t)length, extentEnd);
			_ZeroVecs(vecs, count, from - pos, to - from);
		}
		first = extentEnd >> shift;
	}
	return B_OK;
}


/*static*/ void
ExtentStream::_ZeroVecs(const iovec* vecs, size_t count, off_t offset,
	off_t length)
{
	for (size_t i = 0; i < count && length > 0; i++) {
		if (offset >= (off_t)vecs[i].iov_len) {
			offset -= vecs[i].iov_len;
			continue;
		}
		size_t chunk = min_c(vecs[i].iov_len - offset, (size_t)length);
		memset((uint8*)vecs[i].iov_base + offset, 0, chunk);
		length -= chunk;
		offset = 0;
	}
}


status_t
ExtentStream::FillHoles(Transaction& transaction, off_t pos, off_t length,
	off_t& added)
{
	// A file made elsewhere can have holes inside its size (truncate -s,
	// fallocate --punch-hole). The page writer cannot write to a hole, so
	// before a write's pages reach one, give it blocks. The file cache
	// writes the whole of each page it dirties, so the new blocks get data
	// or the zeros the hole read as.
	added = 0;
	if (length <= 0)
		return B_OK;

	uint32 shift = fVolume->BlockShift();
	fileblock_t first = (pos & ~(off_t)(B_PAGE_SIZE - 1)) >> shift;
	fileblock_t end = ((pos + length + B_PAGE_SIZE - 1)
		& ~(off_t)(B_PAGE_SIZE - 1)) >> shift;
	if (end > (fileblock_t)fNumBlocks)
		end = fNumBlocks;

	bool zeroed = false;
	status_t status = _FillHoles(transaction, first, end, added, zeroed);
	if (zeroed) {
		// the zeros must be on the disk before the new mapping can be
		ioctl(fVolume->Device(), B_FLUSH_DRIVE_CACHE);
	}
	return status;
}


status_t
ExtentStream::_FillHoles(Transaction& transaction, fileblock_t first,
	fileblock_t end, off_t& added, bool& zeroed)
{
	while (first < end) {
		CachedBlock cached(fVolume);
		ext2_extent_stream* stream;
		fsblock_t leafBlock;
		int32 i;
		fileblock_t next;
		status_t status = _LookUp(first, cached, stream, leafBlock, i, next);
		if (status != B_OK)
			return status;

		int32 count = stream->extent_header.NumEntries();
		fileblock_t holeEnd = end;
		if (i < count) {
			const ext2_extent_entry& extent = stream->extent_entries[i];
			if (extent.LogicalBlock() <= first) {
				// mapped already
				first = (fileblock_t)extent.LogicalBlock() + extent.Length();
				continue;
			}
			holeEnd = min_c(holeEnd, (fileblock_t)extent.LogicalBlock());
		}
		// an extent must not reach into the range of the next leaf
		holeEnd = min_c(holeEnd, next);

		// allocate a run for (the start of) the hole, right after what
		// precedes it if possible
		fsblock_t start = fFirstBlock;
		if (i > 0) {
			const ext2_extent_entry& previous = stream->extent_entries[i - 1];
			start = previous.PhysicalBlock() + previous.Length();
		}
		uint32 blockGroup = (start - fFirstBlock) / fVolume->BlocksPerGroup();
		if (blockGroup >= fVolume->NumGroups())
			blockGroup = 0;
		uint32 allocated;
		status = fVolume->AllocateBlocks(transaction, 1,
			min_c(holeEnd - first, (fileblock_t)EXT2_EXTENT_MAX_LENGTH),
			blockGroup, start, allocated);
		if (status != B_OK)
			return status;

		bool merge = false;
		if (i > 0) {
			const ext2_extent_entry& previous = stream->extent_entries[i - 1];
			merge = !previous.IsUninitialized()
				&& (fileblock_t)previous.LogicalBlock() + previous.Length()
					== first
				&& previous.PhysicalBlock() + previous.Length() == start
				&& previous.Length() + allocated <= EXT2_EXTENT_MAX_LENGTH;
		}

		if (!merge && count >= stream->extent_header.MaxEntries()) {
			// The leaf has no room for a new extent: split it (and what is
			// full above it), then look the hole up again, as it may now
			// belong to the new leaf.
			fVolume->FreeBlocks(transaction, start, allocated);
			cached.Unset();
			status = _MakeRoom(transaction, first, added);
			if (status != B_OK)
				return status;
			continue;
		}

		// The blocks may hold anything; a partial write of an uncached page
		// reads them back first. Zero them before they are mapped.
		status = _ZeroBlocks(start, allocated);
		if (status != B_OK) {
			fVolume->FreeBlocks(transaction, start, allocated);
			return status;
		}
		zeroed = true;

		if (leafBlock != 0) {
			stream = (ext2_extent_stream*)cached.SetToWritable(transaction,
				leafBlock);
			if (stream == NULL) {
				fVolume->FreeBlocks(transaction, start, allocated);
				return B_IO_ERROR;
			}
		}

		if (merge) {
			ext2_extent_entry& previous = stream->extent_entries[i - 1];
			previous.SetLength(previous.Length() + allocated);
		} else {
			// insert the extent at index i, keeping the leaf sorted
			memmove(&stream->extent_entries[i + 1],
				&stream->extent_entries[i],
				(count - i) * sizeof(ext2_extent_entry));
			stream->extent_entries[i].SetLogicalBlock(first);
			stream->extent_entries[i].SetLength(allocated);
			stream->extent_entries[i].SetPhysicalBlock(start);
			stream->extent_header.SetNumEntries(count + 1);
		}
		fInode->SetExtentChecksum(stream);

		if (!merge && i == 0 && leafBlock != 0) {
			// the leaf now starts lower: so must the keys leading to it
			status = _LowerKeys(transaction, first);
			if (status != B_OK)
				return status;
		}
		added += allocated;
		first += allocated;
	}

	return B_OK;
}


status_t
ExtentStream::_AllocateTreeBlock(Transaction& transaction, fsblock_t goal,
	fsblock_t& block)
{
	uint32 blockGroup = goal < fFirstBlock
		? 0 : (goal - fFirstBlock) / fVolume->BlocksPerGroup();
	if (blockGroup >= fVolume->NumGroups())
		blockGroup = 0;
	block = goal;
	uint32 allocated;
	return fVolume->AllocateBlocks(transaction, 1, 1, blockGroup, block,
		allocated);
}


status_t
ExtentStream::_GrowDepth(Transaction& transaction)
{
	// As ext4_ext_grow_indepth(): the root in the inode moves, as it is, to
	// a new block, and becomes an index with that block as its only child.
	// The block has room for many more entries than the inode.
	uint16 depth = fStream->extent_header.Depth();
	if (depth >= kMaxTreeDepth)
		return B_FILE_TOO_LARGE;

	// near what the root points to
	fsblock_t goal = 0;
	if (fStream->extent_header.NumEntries() > 0) {
		goal = depth > 0 ? fStream->extent_index[0].PhysicalBlock()
			: fStream->extent_entries[0].PhysicalBlock();
	}
	fsblock_t newBlock;
	status_t status = _AllocateTreeBlock(transaction, goal, newBlock);
	if (status != B_OK)
		return status;
	CachedBlock cached(fVolume);
	ext2_extent_stream* node = (ext2_extent_stream*)cached.SetToWritable(
		transaction, newBlock, true);
	if (node == NULL) {
		fVolume->FreeBlocks(transaction, newBlock, 1);
		return B_IO_ERROR;
	}
	memcpy(node, fStream, sizeof(ext2_extent_stream));
	node->extent_header.SetMaxEntries(_MaxBlockEntries());
	fInode->SetExtentChecksum(node);

	// an index key is the first logical block of its child, as e2fsck
	// checks; entries and indexes keep it at the same place
	uint32 key = fStream->extent_header.NumEntries() > 0
		? fStream->extent_entries[0].LogicalBlock() : 0;
	fStream->extent_header.SetDepth(depth + 1);
	fStream->extent_header.SetNumEntries(1);
	memset(&fStream->extent_index[0], 0, sizeof(fStream->extent_index));
	fStream->extent_index[0].SetLogicalBlock(key);
	fStream->extent_index[0].SetPhysicalBlock(newBlock);
	fInode->SetExtentChecksum(fStream);
	return B_OK;
}


status_t
ExtentStream::_SplitNode(Transaction& transaction, fsblock_t parentBlock,
	int32 parentIndex, fsblock_t childBlock, int32 insertAt)
{
	// As ext4_ext_split(), one level at a time: the upper part of the full
	// child moves to a new block, which gets an index entry right after the
	// child's in the parent. The parent must have room.
	CachedBlock parentCached(fVolume);
	ext2_extent_stream* parent = fStream;
	if (parentBlock != 0) {
		parent = (ext2_extent_stream*)parentCached.SetToWritable(transaction,
			parentBlock);
		if (parent == NULL)
			return B_IO_ERROR;
	}
	CachedBlock childCached(fVolume);
	ext2_extent_stream* child = (ext2_extent_stream*)
		childCached.SetToWritable(transaction, childBlock);
	if (child == NULL)
		return B_IO_ERROR;

	int32 parentCount = parent->extent_header.NumEntries();
	int32 count = child->extent_header.NumEntries();
	if (parentCount >= parent->extent_header.MaxEntries()
		|| parentIndex >= parentCount || count < 2) {
		return B_BAD_DATA;
	}

	// Where the entry that needs the room goes decides the split. Appending
	// (Linux's files fill their leaves that way) moves only the last entry,
	// so that the old node stays all but full; ext4 moves nothing then, but
	// a node can't be left empty here. Anywhere else, half moves, so that
	// both halves have room for what comes next.
	int32 split;
	if (insertAt >= count)
		split = count - 1;
	else if (insertAt <= 0)
		split = 1;
	else
		split = count / 2;

	fsblock_t newBlock;
	status_t status = _AllocateTreeBlock(transaction, childBlock, newBlock);
	if (status != B_OK)
		return status;
	CachedBlock newCached(fVolume);
	ext2_extent_stream* node = (ext2_extent_stream*)newCached.SetToWritable(
		transaction, newBlock, true);
	if (node == NULL) {
		fVolume->FreeBlocks(transaction, newBlock, 1);
		return B_IO_ERROR;
	}

	// extent entries and index entries have the same size, and both start
	// with their first logical block
	int32 moved = count - split;
	node->extent_header.magic = EXT2_EXTENT_MAGIC;
	node->extent_header.SetNumEntries(moved);
	node->extent_header.SetMaxEntries(_MaxBlockEntries());
	node->extent_header.SetDepth(child->extent_header.Depth());
	node->extent_header.SetGeneration(0);
	memcpy(&node->extent_entries[0], &child->extent_entries[split],
		moved * sizeof(ext2_extent_entry));
	fInode->SetExtentChecksum(node);

	memset(&child->extent_entries[split], 0,
		moved * sizeof(ext2_extent_entry));
	child->extent_header.SetNumEntries(split);
	fInode->SetExtentChecksum(child);

	memmove(&parent->extent_index[parentIndex + 2],
		&parent->extent_index[parentIndex + 1],
		(parentCount - parentIndex - 1) * sizeof(ext2_extent_index));
	ext2_extent_index& index = parent->extent_index[parentIndex + 1];
	memset(&index, 0, sizeof(ext2_extent_index));
	index.SetLogicalBlock(node->extent_entries[0].LogicalBlock());
	index.SetPhysicalBlock(newBlock);
	parent->extent_header.SetNumEntries(parentCount + 1);
	fInode->SetExtentChecksum(parent);
	return B_OK;
}


status_t
ExtentStream::_MakeRoom(Transaction& transaction, fileblock_t first,
	off_t& added)
{
	// Makes room for one more extent in the leaf _LookUp() finds for
	// "first", as ext4_ext_create_new_leaf() does: the topmost full node on
	// the path whose parent has room is split, or the tree grows by a level
	// if even the root is full, until the leaf has room. Each step leaves a
	// valid tree, so running out of space on the way loses nothing; the
	// blocks the tree got so far are in "added" (for i_blocks).
	for (;;) {
		uint16 depth = fStream->extent_header.Depth();
		if (depth > kMaxTreeDepth)
			return B_BAD_DATA;

		fsblock_t blocks[kMaxTreeDepth + 1];
			// of the node at each level; the root in the inode has none
		int32 positions[kMaxTreeDepth + 1];
			// the entry taken in each index node, where the extent goes in
			// the leaf
		bool full[kMaxTreeDepth + 1];

		CachedBlock cached(fVolume);
		ext2_extent_stream* stream = fStream;
		blocks[0] = 0;
		for (uint16 level = 0;; level++) {
			int32 count = stream->extent_header.NumEntries();
			full[level] = count >= stream->extent_header.MaxEntries();
			if (level == depth) {
				int32 i = 0;
				while (i < count && stream->extent_entries[i].LogicalBlock()
						+ stream->extent_entries[i].Length() <= first) {
					i++;
				}
				positions[level] = i;
				break;
			}
			if (count == 0)
				return B_BAD_DATA;
			int32 i = 0;
			while (i + 1 < count
				&& stream->extent_index[i + 1].LogicalBlock() <= first) {
				i++;
			}
			positions[level] = i;
			blocks[level + 1] = stream->extent_index[i].PhysicalBlock();
			stream = (ext2_extent_stream*)cached.SetTo(blocks[level + 1]);
			if (stream == NULL)
				return B_IO_ERROR;
			if (!stream->extent_header.IsValid()
				|| stream->extent_header.Depth() != depth - level - 1
				|| !fInode->VerifyExtentChecksum(stream)) {
				return B_BAD_DATA;
			}
		}
		cached.Unset();

		if (!full[depth])
			return B_OK;

		int32 level = depth;
		while (level > 0 && full[level - 1])
			level--;

		status_t status;
		if (level == 0)
			status = _GrowDepth(transaction);
		else {
			status = _SplitNode(transaction, blocks[level - 1],
				positions[level - 1], blocks[level],
				level == depth ? positions[level] : positions[level] + 1);
		}
		if (status != B_OK)
			return status;
		added++;
	}
}


uint16
ExtentStream::_MaxBlockEntries() const
{
	// what an extent tree block holds, entries or indexes (12 bytes each)
	return (fVolume->BlockSize() - sizeof(ext2_extent_header))
		/ sizeof(ext2_extent_entry);
}


status_t
ExtentStream::_LowerKeys(Transaction& transaction, fileblock_t first)
{
	// Along the path _LookUp() takes to "first", lower every index key that
	// is above it to "first" (only the first entry of a node can be).
	ext2_extent_stream* stream = fStream;
	CachedBlock cached(fVolume);
	fsblock_t nodeBlock = 0;
	while (stream->extent_header.Depth() != 0) {
		int32 count = stream->extent_header.NumEntries();
		if (count == 0)
			return B_BAD_DATA;
		int32 i = 0;
		while (i + 1 < count
			&& stream->extent_index[i + 1].LogicalBlock() <= first) {
			i++;
		}
		if (stream->extent_index[i].LogicalBlock() > first) {
			if (nodeBlock != 0) {
				stream = (ext2_extent_stream*)cached.SetToWritable(
					transaction, nodeBlock);
				if (stream == NULL)
					return B_IO_ERROR;
			}
			stream->extent_index[i].SetLogicalBlock(first);
			fInode->SetExtentChecksum(stream);
		}
		nodeBlock = stream->extent_index[i].PhysicalBlock();
		stream = (ext2_extent_stream*)cached.SetTo(nodeBlock);
		if (stream == NULL)
			return B_IO_ERROR;
	}
	return B_OK;
}


status_t
ExtentStream::_ZeroBlocks(fsblock_t start, uint32 count)
{
	// File data does not go through the block cache, so this writes to
	// the device directly. The buffer spans several pages (see #1069).
	const size_t kBufferSize = 256 * 1024;
	uint8* buffer = (uint8*)calloc(1, kBufferSize);
	if (buffer == NULL)
		return B_NO_MEMORY;

	off_t offset = (off_t)start << fVolume->BlockShift();
	off_t left = (off_t)count << fVolume->BlockShift();
	status_t status = B_OK;
	while (left > 0) {
		size_t chunk = left > (off_t)kBufferSize ? kBufferSize : left;
		ssize_t written = write_pos(fVolume->Device(), offset, buffer, chunk);
		if (written != (ssize_t)chunk) {
			status = written < 0 ? (status_t)written : B_IO_ERROR;
			break;
		}
		offset += chunk;
		left -= chunk;
	}

	free(buffer);
	return status;
}


status_t
ExtentStream::_MappedEnd(off_t& end)
{
	// the logical end of the last extent, which can lie past fNumBlocks
	ext2_extent_stream* stream = fStream;
	if (!stream->extent_header.IsValid()
		|| stream->extent_header.NumEntries()
			> stream->extent_header.MaxEntries()) {
		return B_BAD_DATA;
	}
	CachedBlock cached(fVolume);
	while (stream->extent_header.Depth() != 0) {
		int32 count = stream->extent_header.NumEntries();
		if (count == 0)
			return B_BAD_DATA;
		stream = (ext2_extent_stream*)cached.SetTo(
			stream->extent_index[count - 1].PhysicalBlock());
		if (stream == NULL)
			return B_IO_ERROR;
		if (!stream->extent_header.IsValid())
			return B_BAD_DATA;
	}

	int32 count = stream->extent_header.NumEntries();
	end = count == 0 ? 0 : (off_t)stream->extent_entries[count - 1]
		.LogicalBlock() + stream->extent_entries[count - 1].Length();
	return B_OK;
}


void
ExtentStream::Init()
{
	fStream->extent_header.magic = EXT2_EXTENT_MAGIC;
	fStream->extent_header.SetNumEntries(0);
	fStream->extent_header.SetMaxEntries(4);
	fStream->extent_header.SetDepth(0);
	fStream->extent_header.SetGeneration(0);
	fInode->SetExtentChecksum(fStream);
}


status_t
ExtentStream::_Check(ext2_extent_stream *stream, fileblock_t &block)
{
	if (!stream->extent_header.IsValid()) {
		panic("_Check() invalid header\n");
		return B_BAD_VALUE;
	}
	if (stream->extent_header.Depth() == 0) {
		for (int32 i = 0; i < stream->extent_header.NumEntries(); i++) {
			ext2_extent_entry &entry = stream->extent_entries[i];
			if (entry.LogicalBlock() < block) {
				panic("_Check() entry %" B_PRId32 " %" B_PRIu64 " %" B_PRIu32
					"\n", i, block, entry.LogicalBlock());
				return B_BAD_VALUE;
			}
			block = entry.LogicalBlock() + entry.Length();
		}
		return B_OK;
	} 

	CachedBlock cached(fVolume);
	for (int32 i = 0; i < stream->extent_header.NumEntries(); i++) {
		ext2_extent_index &index = stream->extent_index[i];
		if (index.LogicalBlock() < block) {
			panic("_Check() index %" B_PRId32 " %" B_PRIu64 " %" B_PRIu32
				"\n", i, block, index.LogicalBlock());
			return B_BAD_VALUE;
		}
		ext2_extent_stream *child = (ext2_extent_stream *)cached.SetTo(
			index.PhysicalBlock());
		if (child->extent_header.Depth() != (stream->extent_header.Depth() - 1) 
			|| _Check(child, block) != B_OK)
			return B_BAD_VALUE;
	}
	return B_OK;
}


bool
ExtentStream::Check()
{
	fileblock_t block = 0;
	return _Check(fStream, block) == B_OK;
}


status_t
ExtentStream::_CheckBlock(ext2_extent_stream *stream, fsblock_t block)
{
	if (stream->extent_header.Depth() == 0) {
		for (int32 i = 0; i < stream->extent_header.NumEntries(); i++) {
			ext2_extent_entry &entry = stream->extent_entries[i];
			if (entry.PhysicalBlock() <= block
				&& (entry.PhysicalBlock() + entry.Length()) > block) {
				panic("_CheckBlock() entry %" B_PRId32 " %" B_PRIu64 " %"
					B_PRIu64 " %d\n", i, block, entry.PhysicalBlock(),
					entry.Length());
				return B_BAD_VALUE;
			}
		}
		return B_OK;
	}

	CachedBlock cached(fVolume);
	for (int32 i = 0; i < stream->extent_header.NumEntries(); i++) {
		ext2_extent_index &index = stream->extent_index[i];
		if (index.PhysicalBlock() == block) {
			panic("_CheckBlock() index %" B_PRId32 " %" B_PRIu64 "\n", i, block);
			return B_BAD_VALUE;
		}
		ext2_extent_stream *child = (ext2_extent_stream *)cached.SetTo(
			index.PhysicalBlock());
		if (child->extent_header.Depth() != (stream->extent_header.Depth() - 1)
			|| _CheckBlock(child, block) != B_OK)
			return B_BAD_VALUE;
	}
	return B_OK;
}

