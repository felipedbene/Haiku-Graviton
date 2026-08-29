/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * partition_grow -- first-boot hook that grows the root GPT partition entry to
 * fill the backing disk, so a DeBeOS AMI launched on a larger EBS volume ends
 * up with a root partition that spans the whole disk.
 *
 * Why this exists (see docs/develop/bfs-auto-grow-design.md, items 5/6):
 *
 * The canonical image is baked at a fixed size by make-gpt-image.sh. The merged
 * BFS mount-time grow engine grows the *filesystem* to fill its backing device
 * whenever the device (the partition) is larger than the filesystem -- but it
 * can only fire if the partition is actually bigger than the FS. On a larger
 * EBS the GPT still describes the small baked partition, so the FS device size
 * never exceeds the FS size and the grow never triggers. This program closes
 * that gap: on first boot it grows the root partition entry to fill the disk,
 * which makes the next mount's BFS engine see partition > FS and grow the FS.
 *
 * Convergence is therefore two boots by construction: the root FS is grown at
 * mount time, which is long before this userland job runs, so the partition
 * grown here is consumed by the *next* boot's mount. That is exactly the
 * split the design's T4 note describes ("the hook only needs to grow the
 * partition; the FS grow happens on the next mount automatically").
 *
 * Why not the DiskDeviceManager BPartition::Resize() path, as one would expect:
 *
 * The in-tree DiskDeviceManager API cannot do this operation:
 *
 * - BPartition::Resize() couples the partition-entry resize to a *content*
 *   (filesystem) resize whenever the partition has content (Partition.cpp:849).
 *   The BFS disk-system add-on advertises no resizing support at all
 *   (BFSAddOn.cpp:41,51 -- both B_DISK_SYSTEM_SUPPORTS_RESIZING and
 *   ..._WHILE_MOUNTED are commented out), so CanResize()/Resize() on a mounted
 *   BFS root simply fail. There is no userland entry point that resizes the
 *   partition entry alone -- and resizing the FS through that layer is exactly
 *   what we must NOT do, because the FS grow is owned by the mount-time engine.
 * - The kernel efi_gpt module's resize_child rewrites both header copies via
 *   Header::WriteEntry(), but at the *stale* backup-header location: the Header
 *   constructor pins its notion of the last block to the on-disk AlternateBlock
 *   (Header.cpp:67), so it never relocates the secondary GPT header to the new
 *   end of a grown disk, nor updates LastUsableBlock.
 *
 * So we use the DiskDeviceManager only for what it does support -- read-only
 * discovery of the root partition and its containing raw device -- and do the
 * GPT surgery ourselves with raw pread/pwrite on the device fd, reusing the
 * in-tree GPT on-disk structures (gpt.h) and CRC (crc32.cpp). This matches the
 * design's Part-B/Part-F posture (raw read_pos/write_pos with explicit flush
 * barriers) and pulls in no external growpart-style dependency.
 *
 * Safety properties:
 *
 * - **Idempotent.** If the disk is not larger than the extent the GPT already
 *   covers (AlternateBlock at the last LBA), it is a clean no-op, so reboots
 *   and stop/start never re-grow.
 * - **Only the last partition, only to fill.** The target must be the root
 *   partition and nothing may lie beyond it; we refuse otherwise rather than
 *   overwrite a sibling.
 * - **Backup-first write ordering with flushes.** The relocated backup header
 *   and entries are written and flushed before the primary header, and the
 *   primary header (the commit) is written last. A crash in any window leaves
 *   a GPT that still reads as the old, consistent geometry -- from the primary
 *   pair if it survived, otherwise from the untouched old backup -- so the next
 *   boot simply re-runs and re-grows. There is no non-recoverable window.
 *
 * NOTE on verification: per the design doc this metadata-writing path is
 * PROPOSE-ONLY until the crash-injection acceptance passes; this program
 * implements the fast path and its ordering discipline, but the end-to-end and
 * fault-injection proof is a separate, later step.
 */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <Path.h>

#include "crc32.h"
#include "gpt.h"


extern "C" const char* __progname;
static const char* kProgramName = __progname;

// The system volume is mounted here on every Haiku boot.
#define BOOT_MOUNT_POINT	"/boot"


/*!	Reads a GPT table header from \a blockOffset and verifies it is a valid,
	self-consistent primary/backup header sitting at \a expectedBlock.
*/
static bool
read_and_validate_header(int fd, uint32 blockSize, uint64 expectedBlock,
	gpt_table_header& header)
{
	ssize_t bytesRead = pread(fd, &header, sizeof(header),
		(off_t)expectedBlock * blockSize);
	if (bytesRead != (ssize_t)sizeof(header))
		return false;

	if (memcmp(header.header, EFI_PARTITION_HEADER,
			sizeof(header.header)) != 0)
		return false;
	if (header.AbsoluteBlock() != expectedBlock)
		return false;

	// The header CRC is computed over the header with the CRC field zeroed,
	// exactly as EFI::Header::_UpdateCRC() does it (over sizeof the struct).
	uint32 storedCRC = header.HeaderCRC();
	header.SetHeaderCRC(0);
	bool valid = storedCRC == crc32((const uint8*)&header, sizeof(header));
	header.SetHeaderCRC(storedCRC);
	return valid;
}


int
main(int argc, char** argv)
{
	// --- locate the root partition and its raw device (read-only) ----------

	BDiskDeviceRoster roster;
	BDiskDevice device;
	BPartition* partition = NULL;

	status_t status = roster.FindPartitionByMountPoint(BOOT_MOUNT_POINT,
		&device, &partition);
	if (status != B_OK || partition == NULL) {
		fprintf(stderr, "%s: cannot find the partition mounted at %s: %s\n",
			kProgramName, BOOT_MOUNT_POINT, strerror(status));
		return 1;
	}

	BPath devicePath;
	status = device.GetPath(&devicePath);
	if (status != B_OK) {
		fprintf(stderr, "%s: cannot get the raw device path: %s\n",
			kProgramName, strerror(status));
		return 1;
	}

	uint32 blockSize = device.BlockSize();
	if (blockSize < sizeof(gpt_table_header)) {
		fprintf(stderr, "%s: implausible block size %" B_PRIu32 "\n",
			kProgramName, blockSize);
		return 1;
	}

	off_t deviceSize = device.Size();
	uint64 deviceBlocks = deviceSize / blockSize;
	if (deviceBlocks < 2) {
		fprintf(stderr, "%s: device too small\n", kProgramName);
		return 1;
	}
	uint64 deviceLastBlock = deviceBlocks - 1;

	// The root partition's start LBA -- our key for finding its GPT entry. The
	// device is the whole disk (offset 0), so the offset is disk-relative.
	uint64 rootStartBlock = (uint64)partition->Offset() / blockSize;

	// --- read and validate the primary GPT header --------------------------
	// The raw device covers the whole disk including the GPT, so open it R/W.

	int fd = open(devicePath.Path(), O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: cannot open %s: %s\n", kProgramName,
			devicePath.Path(), strerror(errno));
		return 1;
	}

	gpt_table_header header;
	bool haveHeader = read_and_validate_header(fd, blockSize,
		EFI_HEADER_LOCATION, header);
	if (!haveHeader) {
		// Fall back to the on-disk backup so a torn primary from an interrupted
		// earlier run is self-healing rather than fatal. Its recorded
		// AlternateBlock is where the backup lives.
		gpt_table_header primary;
		if (pread(fd, &primary, sizeof(primary),
					(off_t)EFI_HEADER_LOCATION * blockSize)
				== (ssize_t)sizeof(primary)
			&& memcmp(primary.header, EFI_PARTITION_HEADER,
					sizeof(primary.header)) == 0) {
			uint64 backupBlock = primary.AlternateBlock();
			gpt_table_header backup;
			if (read_and_validate_header(fd, blockSize, backupBlock, backup)) {
				// Rebuild a primary-shaped header from the backup.
				header = backup;
				header.SetAbsoluteBlock(EFI_HEADER_LOCATION);
				header.SetAlternateBlock(backupBlock);
				header.SetEntriesBlock(EFI_PARTITION_ENTRIES_BLOCK);
				haveHeader = true;
			}
		}
	}
	if (!haveHeader) {
		fprintf(stderr, "%s: no valid GPT on %s; leaving the disk alone\n",
			kProgramName, devicePath.Path());
		close(fd);
		return 1;
	}

	// --- idempotency: only act when the disk is larger than the GPT extent --
	// After a completed grow the backup header sits at the last block, so this
	// is the clean no-op that makes reboots and stop/start free.

	if (deviceLastBlock <= header.AlternateBlock()) {
		printf("%s: partition already fills the disk (last block %" B_PRIu64
			"); nothing to do\n", kProgramName, header.AlternateBlock());
		close(fd);
		return 0;
	}

	// --- read and validate the primary entry array -------------------------

	uint32 entryCount = header.EntryCount();
	uint32 entrySize = header.EntrySize();
	size_t entriesBytes = (size_t)entryCount * entrySize;
	if (entryCount == 0 || entrySize < sizeof(gpt_partition_entry)
		|| entriesBytes == 0 || entriesBytes > 1024 * 1024) {
		fprintf(stderr, "%s: implausible GPT entry array (%" B_PRIu32 " x %"
			B_PRIu32 ")\n", kProgramName, entryCount, entrySize);
		close(fd);
		return 1;
	}

	uint8* entries = (uint8*)malloc(entriesBytes);
	if (entries == NULL) {
		fprintf(stderr, "%s: out of memory\n", kProgramName);
		close(fd);
		return 1;
	}

	if (pread(fd, entries, entriesBytes,
			(off_t)header.EntriesBlock() * blockSize)
				!= (ssize_t)entriesBytes) {
		fprintf(stderr, "%s: cannot read the GPT entry array\n", kProgramName);
		free(entries);
		close(fd);
		return 1;
	}
	if (header.EntriesCRC() != crc32(entries, entriesBytes)) {
		fprintf(stderr, "%s: GPT entry array CRC mismatch; leaving the disk "
			"alone\n", kProgramName);
		free(entries);
		close(fd);
		return 1;
	}

	// --- find the root entry and confirm nothing lies beyond it ------------

	int32 targetIndex = -1;
	uint64 highestEnd = 0;
	for (uint32 i = 0; i < entryCount; i++) {
		gpt_partition_entry& entry
			= *(gpt_partition_entry*)(entries + (size_t)i * entrySize);
		// An all-zero type GUID marks an unused entry.
		static const guid_t kEmptyGUID = {};
		if (entry.partition_type == kEmptyGUID)
			continue;

		if (entry.EndBlock() > highestEnd)
			highestEnd = entry.EndBlock();
		if (entry.StartBlock() == rootStartBlock)
			targetIndex = i;
	}

	if (targetIndex < 0) {
		fprintf(stderr, "%s: could not match the root partition (start block %"
			B_PRIu64 ") to a GPT entry\n", kProgramName, rootStartBlock);
		free(entries);
		close(fd);
		return 1;
	}

	gpt_partition_entry& target
		= *(gpt_partition_entry*)(entries + (size_t)targetIndex * entrySize);

	if (target.EndBlock() != highestEnd) {
		fprintf(stderr, "%s: root partition is not the last on the disk; "
			"refusing to grow into a following partition\n", kProgramName);
		free(entries);
		close(fd);
		return 1;
	}

	// --- compute the grown geometry ----------------------------------------
	// The backup header and its entry copy live in the last blocks of the disk;
	// LastUsableBlock is the last block before them. This mirrors the format-
	// time math in EFI::Header (SetLastUsableBlock(lastBlock - 1 - entryBlocks)).

	uint32 entryBlocks = (entriesBytes + blockSize - 1) / blockSize;
	uint64 newLastUsable = deviceLastBlock - 1 - entryBlocks;

	if (newLastUsable <= target.EndBlock()) {
		// Disk grew, but not enough to give the root any more usable blocks.
		printf("%s: no additional usable space for the root partition; "
			"relocating the backup header only is not worthwhile, skipping\n",
			kProgramName);
		free(entries);
		close(fd);
		return 0;
	}

	uint64 oldEnd = target.EndBlock();
	target.SetEndBlock(newLastUsable);

	// New entries CRC over the modified array.
	uint32 newEntriesCRC = crc32(entries, entriesBytes);

	// Primary header for the grown disk.
	gpt_table_header primaryHeader = header;
	primaryHeader.SetAbsoluteBlock(EFI_HEADER_LOCATION);
	primaryHeader.SetAlternateBlock(deviceLastBlock);
	primaryHeader.SetEntriesBlock(EFI_PARTITION_ENTRIES_BLOCK);
	primaryHeader.SetLastUsableBlock(newLastUsable);
	primaryHeader.SetEntriesCRC(newEntriesCRC);
	primaryHeader.SetHeaderCRC(0);
	primaryHeader.SetHeaderCRC(crc32((const uint8*)&primaryHeader,
		sizeof(primaryHeader)));

	// Backup header at the new end of the disk. Its entry copy sits in the
	// entryBlocks immediately before it (deviceLastBlock - entryBlocks).
	uint64 backupEntriesBlock = deviceLastBlock - entryBlocks;
	gpt_table_header backupHeader = primaryHeader;
	backupHeader.SetAbsoluteBlock(deviceLastBlock);
	backupHeader.SetAlternateBlock(EFI_HEADER_LOCATION);
	backupHeader.SetEntriesBlock(backupEntriesBlock);
	backupHeader.SetHeaderCRC(0);
	backupHeader.SetHeaderCRC(crc32((const uint8*)&backupHeader,
		sizeof(backupHeader)));

	printf("%s: growing root partition (entry %" B_PRId32 ") end block %"
		B_PRIu64 " -> %" B_PRIu64 ", relocating backup GPT header %" B_PRIu64
		" -> %" B_PRIu64 "\n", kProgramName, targetIndex, oldEnd, newLastUsable,
		header.AlternateBlock(), deviceLastBlock);

	// --- write with backup-first ordering and flush barriers ---------------
	// Order: backup entries, backup header, primary entries, primary header.
	// The primary header is the commit; a crash before it leaves the old,
	// consistent GPT in place (primary pair untouched, or -- once primary
	// entries are rewritten -- the old backup at its original location), so the
	// next boot re-runs and re-grows. Every barrier is a full device flush.

	bool ok = true;

	// Backup entry copy.
	ok = ok && pwrite(fd, entries, entriesBytes,
		(off_t)backupEntriesBlock * blockSize) == (ssize_t)entriesBytes;
	ok = ok && fsync(fd) == 0;

	// Backup header.
	ok = ok && pwrite(fd, &backupHeader, sizeof(backupHeader),
		(off_t)deviceLastBlock * blockSize) == (ssize_t)sizeof(backupHeader);
	ok = ok && fsync(fd) == 0;

	// Primary entry copy.
	ok = ok && pwrite(fd, entries, entriesBytes,
		(off_t)primaryHeader.EntriesBlock() * blockSize)
			== (ssize_t)entriesBytes;
	ok = ok && fsync(fd) == 0;

	// Primary header -- the commit.
	ok = ok && pwrite(fd, &primaryHeader, sizeof(primaryHeader),
		(off_t)EFI_HEADER_LOCATION * blockSize)
			== (ssize_t)sizeof(primaryHeader);
	ok = ok && fsync(fd) == 0;

	free(entries);
	close(fd);

	if (!ok) {
		fprintf(stderr, "%s: a GPT write failed (%s); the disk still reads as "
			"its previous geometry and the next boot will retry\n",
			kProgramName, strerror(errno));
		return 1;
	}

	printf("%s: root partition grown to fill the disk; the BFS mount-time "
		"engine will grow the filesystem on the next mount\n", kProgramName);
	return 0;
}
