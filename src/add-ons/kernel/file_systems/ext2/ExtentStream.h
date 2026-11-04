/*
 * Copyright 2001-2011, Haiku Inc. All rights reserved.
 * This file may be used under the terms of the MIT License.
 *
 * Authors:
 *		Jérôme Duval
 */
#ifndef EXTENTSTREAM_H
#define EXTENTSTREAM_H


#include <sys/uio.h>

#include "ext2.h"
#include "Transaction.h"


class CachedBlock;
class Inode;
class Volume;


class ExtentStream
{
public:
					ExtentStream(Volume* volume, Inode* inode,
						ext2_extent_stream* stream, off_t size);
					~ExtentStream();

	status_t		FindBlock(off_t offset, fsblock_t& block,
						uint32 *_count = NULL);
	status_t		Enlarge(Transaction& transaction, off_t& numBlocks);
	status_t		Shrink(Transaction& transaction, off_t& numBlocks);
	status_t		MarkWritten(Transaction& transaction, off_t pos,
						off_t length, bool& changed);
	status_t		ZeroUninitialized(off_t pos, size_t length,
						const iovec* vecs, size_t count);
	void			Init();
	
	bool			Check();

private:
	static const uint16	kMaxTreeDepth = 8;
		// ext4 itself builds at most 5 levels

	status_t		_Enlarge(Transaction& transaction, off_t targetBlocks,
						off_t& numBlocks);
	status_t		_LookUp(fileblock_t first, CachedBlock& cached,
						ext2_extent_stream*& stream, fsblock_t& leafBlock,
						int32& index, fileblock_t& next);
	status_t		_ZeroBlocks(fsblock_t start, uint32 count);
	static	void	_ZeroVecs(const iovec* vecs, size_t count, off_t offset,
						off_t length);
	status_t		_Check(ext2_extent_stream *stream, fileblock_t &block);
	status_t		_CheckBlock(ext2_extent_stream *stream, fsblock_t block);

	Volume*			fVolume;
	Inode*			fInode;
	ext2_extent_stream* fStream;
	fsblock_t		fFirstBlock;

	fsblock_t		fAllocatedPos;
	uint32			fPending;
		// blocks at fAllocatedPos that Enlarge() allocated, not yet mapped

	off_t			fNumBlocks;
	off_t			fSize;
};

#endif	// EXTENTSTREAM_H

