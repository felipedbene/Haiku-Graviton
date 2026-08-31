/*
 * Copyright 2016-2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <boot/partitions.h>
#include <boot/platform.h>
#include <boot/stage2.h>

#include <boot/net/Ethernet.h>
#include <boot/net/NetStack.h>
#include <boot/net/RemoteDisk.h>

#include "Header.h"

#include "efi_platform.h"
#include <efi/protocol/block-io.h>

#include "gpt.h"
#include "gpt_known_guids.h"
#include "crc32.h"
#include "utility.h"


//#define TRACE_DEVICES
#ifdef TRACE_DEVICES
#   define TRACE(x...) dprintf("efi/devices: " x)
#else
#   define TRACE(x...) ;
#endif


static efi_guid BlockIoGUID = EFI_BLOCK_IO_PROTOCOL_GUID;


class EfiDevice : public Node
{
	public:
		EfiDevice(efi_block_io_protocol *blockIo);
		virtual ~EfiDevice();

		virtual ssize_t ReadAt(void *cookie, off_t pos, void *buffer,
			size_t bufferSize);
		virtual ssize_t WriteAt(void *cookie, off_t pos, const void *buffer,
			size_t bufferSize) { return B_UNSUPPORTED; }
		virtual off_t Size() const {
			return (fBlockIo->Media->LastBlock + 1) * BlockSize(); }

		uint32 BlockSize() const { return fBlockIo->Media->BlockSize; }

#ifdef __aarch64__
		// Grow the trailing GPT (BFS root) partition to fill the disk when the
		// disk is larger than the partition. See the definition for the full
		// rationale (the Graviton "two-boot" auto-grow problem).
		void GrowBootGptPartition();
#endif
	private:
		efi_block_io_protocol*		fBlockIo;
};


EfiDevice::EfiDevice(efi_block_io_protocol *blockIo)
	:
	fBlockIo(blockIo)
{
}


EfiDevice::~EfiDevice()
{
}


ssize_t
EfiDevice::ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize)
{
	TRACE("%s called. pos: %" B_PRIdOFF ", %p, %" B_PRIuSIZE "\n", __func__,
		pos, buffer, bufferSize);

	const uint32 blockSize = BlockSize();
	if (blockSize == 0)
		return B_ERROR;

	// Reads must start on a block boundary and land in a buffer the firmware
	// considers suitably aligned, so they go through a bounce buffer.
	//
	// This buffer must be of a fixed size. It used to be a variable length
	// array sized from the caller's request, which put an unbounded
	// allocation on the boot loader's stack: a 16 KB partition entry array or
	// a kernel image read is enough to run off the end of it. Some firmware
	// faults and resets the machine rather than reporting anything, which
	// makes the failure look like a spontaneous reboot. Being static also
	// keeps it out of the stack entirely; the loader is single threaded.
	static const size_t kBounceSize = 65536;
	static char sBounceBuffer[kBounceSize] __attribute__((aligned(4096)));

	if (blockSize > kBounceSize)
		return B_ERROR;

	size_t totalRead = 0;
	while (bufferSize > 0) {
		const off_t block = pos / blockSize;
		const uint32 offset = pos % blockSize;

		uint32 blocks = (offset + bufferSize + blockSize - 1) / blockSize;
		if (blocks * blockSize > kBounceSize)
			blocks = kBounceSize / blockSize;

		if (fBlockIo->ReadBlocks(fBlockIo, fBlockIo->Media->MediaId, block,
				blocks * blockSize, sBounceBuffer) != EFI_SUCCESS) {
			dprintf("%s: blockIo error reading from device!\n", __func__);
			return totalRead > 0 ? (ssize_t)totalRead : B_ERROR;
		}

		size_t chunk = blocks * blockSize - offset;
		if (chunk > bufferSize)
			chunk = bufferSize;

		memcpy((char*)buffer + totalRead, sBounceBuffer + offset, chunk);

		totalRead += chunk;
		pos += chunk;
		bufferSize -= chunk;
	}

	return totalRead;
}


#ifdef __aarch64__
// A single aligned bounce buffer for the whole-block reads and writes the GPT
// grow performs. Whole-block firmware I/O wants an aligned buffer, and (as the
// note on ReadAt's bounce buffer explains) a large buffer on the loader stack
// can silently reset the machine on some firmware. Keeping it static and file
// scope avoids both problems; the loader is single threaded, and the grow runs
// to completion before anything else touches it. 64 KiB comfortably holds a
// standard 16 KiB (128 x 128 byte) GPT entry array.
static const size_t kGptBounceSize = 65536;
static uint8 sGptBounce[kGptBounceSize] __attribute__((aligned(4096)));


static bool
gpt_read_blocks(efi_block_io_protocol* blockIo, uint64 lba, uint32 count,
	void* buffer)
{
	uint32 blockSize = blockIo->Media->BlockSize;
	size_t bytes = (size_t)count * blockSize;
	if (bytes == 0 || bytes > kGptBounceSize)
		return false;

	if (blockIo->ReadBlocks(blockIo, blockIo->Media->MediaId, lba, bytes,
			sGptBounce) != EFI_SUCCESS)
		return false;

	memcpy(buffer, sGptBounce, bytes);
	return true;
}


static bool
gpt_write_blocks(efi_block_io_protocol* blockIo, uint64 lba, uint32 count,
	const void* buffer)
{
	uint32 blockSize = blockIo->Media->BlockSize;
	size_t bytes = (size_t)count * blockSize;
	if (bytes == 0 || bytes > kGptBounceSize)
		return false;

	memcpy(sGptBounce, buffer, bytes);
	if (blockIo->WriteBlocks(blockIo, blockIo->Media->MediaId, lba, bytes,
			sGptBounce) != EFI_SUCCESS)
		return false;

	return true;
}


/*!	Grow the trailing (BFS root) GPT partition to fill the disk when the disk is
	larger than the partition table describes.

	WHY this lives in the loader: on the Graviton EC2 target the root BFS never
	auto-grows onto a larger EBS volume, because convergence is otherwise "two
	boots by construction". A userland launch_daemon job (partition_grow) extends
	the GPT partition on the first boot, and BFS's mount-time grow engine
	(GrowEngine.cpp / grow_classify) only enlarges the filesystem at mount, when
	the partition it is mounted on has more blocks than the FS. But the root is
	already mounted before that userland job runs, and nothing triggers a second
	boot on Graviton -- an inbound reboot is a no-op on Haiku arm64. So the
	enlarged partition is never consumed.

	Doing the partition grow here, in the EFI loader before the kernel mounts the
	boot BFS, makes partitionBlocks > oldNumBlocks already true at the very first
	mount, so the existing BFS grow-at-mount engine grows the filesystem on boot
	one -- no reboot, no userland ordering dependency.

	Safety contract: arm64 only; grow-only, never shrink; the trailing partition
	only, and only when it is BFS; idempotent (a no-op once the partition already
	fills the disk). On any parse or geometry inconsistency this does nothing and
	returns, so a failed grow can never keep the machine from booting.
*/
void
EfiDevice::GrowBootGptPartition()
{
	uint32 blockSize = BlockSize();
	if (blockSize == 0 || blockSize > kGptBounceSize)
		return;

	// The last addressable LBA of the whole disk (not the partition).
	uint64 diskLastBlock = fBlockIo->Media->LastBlock;
	if (diskLastBlock < 2)
		return;

	// All working buffers are heap allocated. The header/backup blocks and the
	// entry array can each be several KiB; putting them on the loader stack is
	// the same hazard ReadAt's bounce-buffer note describes (some firmware faults
	// and resets rather than reporting). A single cleanup path frees them.
	uint8* headerBlock = (uint8*)malloc(blockSize);
	uint8* backupBlock = (uint8*)malloc(blockSize);
	uint8* entries = NULL;
	if (headerBlock == NULL || backupBlock == NULL)
		goto out;

	// Read and validate the primary GPT header from LBA 1. We keep the whole
	// block so we can rewrite it verbatim (preserving its reserved tail) with
	// only the fields we change touched.
	if (!gpt_read_blocks(fBlockIo, EFI_HEADER_LOCATION, 1, headerBlock))
		goto out;

	{
	gpt_table_header* header = (gpt_table_header*)headerBlock;
	if (memcmp(header->header, EFI_PARTITION_HEADER, sizeof(header->header)) != 0)
		goto out;

	// Validate the header CRC exactly as the GPT reader does.
	uint32 storedHeaderCRC = header->HeaderCRC();
	header->SetHeaderCRC(0);
	if (storedHeaderCRC != crc32((const uint8*)header, sizeof(gpt_table_header)))
		goto out;
	header->SetHeaderCRC(storedHeaderCRC);

	uint32 entryCount = header->EntryCount();
	uint32 entrySize = header->EntrySize();
	if (entrySize < sizeof(gpt_partition_entry) || entryCount == 0)
		goto out;

	size_t entryArrayBytes = (size_t)entryCount * entrySize;
	uint32 entryArrayBlocks = (entryArrayBytes + blockSize - 1) / blockSize;
	size_t entryBufferBytes = (size_t)entryArrayBlocks * blockSize;
	if (entryBufferBytes > kGptBounceSize)
		goto out;

	// The backup structures live at the very end of the disk: the backup header
	// in the last block, the backup entry array in the blocks just below it.
	// Everything below that is usable. If the disk cannot hold both, bail.
	if (diskLastBlock <= (uint64)entryArrayBlocks + 1)
		goto out;
	uint64 backupEntriesBlock = diskLastBlock - entryArrayBlocks;
	uint64 newLastUsable = diskLastBlock - 1 - entryArrayBlocks;

	entries = (uint8*)malloc(entryBufferBytes);
	if (entries == NULL)
		goto out;

	uint64 entriesBlock = header->EntriesBlock();
	if (!gpt_read_blocks(fBlockIo, entriesBlock, entryArrayBlocks, entries))
		goto out;

	// Validate the entry-array CRC before trusting any entry.
	if (header->EntriesCRC() != crc32(entries, entryArrayBytes))
		goto out;

	// Find the trailing partition -- the non-empty entry with the greatest
	// ending block. We only ever grow the last thing on the disk, so growing it
	// to fill the disk can never overlap another partition.
	int32 trailingIndex = -1;
	uint64 trailingEnd = 0;
	for (uint32 i = 0; i < entryCount; i++) {
		gpt_partition_entry* entry
			= (gpt_partition_entry*)(entries + (size_t)i * entrySize);
		if (entry->partition_type == kEmptyGUID)
			continue;
		if (trailingIndex < 0 || entry->EndBlock() > trailingEnd) {
			trailingIndex = (int32)i;
			trailingEnd = entry->EndBlock();
		}
	}
	if (trailingIndex < 0)
		goto out;

	gpt_partition_entry* target
		= (gpt_partition_entry*)(entries + (size_t)trailingIndex * entrySize);

	// Only grow the boot filesystem: require the trailing partition to be BFS.
	bool isBFS = false;
	for (size_t i = 0; i < sizeof(kTypeMap) / sizeof(struct type_map); i++) {
		if (strcmp(kTypeMap[i].type, BFS_NAME) == 0
			&& kTypeMap[i].guid == target->partition_type) {
			isBFS = true;
			break;
		}
	}
	if (!isBFS)
		goto out;

	// Grow-only and idempotent: if the partition already reaches (or somehow
	// exceeds) the last usable block, there is nothing to do.
	if (target->EndBlock() >= newLastUsable
		|| target->StartBlock() >= newLastUsable) {
		goto out;
	}

	dprintf("efi/devices: growing boot GPT partition %" B_PRId32 " end %"
		B_PRIu64 " -> %" B_PRIu64 " (disk last block %" B_PRIu64 ")\n",
		trailingIndex, target->EndBlock(), newLastUsable, diskLastBlock);

	// Extend the entry and recompute the entry-array CRC.
	target->SetEndBlock(newLastUsable);
	uint32 newEntriesCRC = crc32(entries, entryArrayBytes);

	// Update the primary header: it now describes a disk whose backup header is
	// at diskLastBlock and whose last usable block moved out to newLastUsable.
	header->SetEntriesCRC(newEntriesCRC);
	header->SetAlternateBlock(diskLastBlock);
	header->SetLastUsableBlock(newLastUsable);
	header->SetHeaderCRC(0);
	header->SetHeaderCRC(crc32((const uint8*)header, sizeof(gpt_table_header)));

	// Build a fresh backup header block from the primary. The block at
	// diskLastBlock is new territory on the grown disk, so we write a fully
	// zeroed block with only the header struct populated (reserved tail zero).
	memset(backupBlock, 0, blockSize);
	gpt_table_header* backup = (gpt_table_header*)backupBlock;
	memcpy(backup, header, sizeof(gpt_table_header));
	backup->SetAbsoluteBlock(diskLastBlock);
	backup->SetAlternateBlock(EFI_HEADER_LOCATION);
	backup->SetEntriesBlock(backupEntriesBlock);
	backup->SetHeaderCRC(0);
	backup->SetHeaderCRC(crc32((const uint8*)backup, sizeof(gpt_table_header)));

	// Write the backup set first, then the primary. If power is lost midway, a
	// still-valid primary keeps the old (smaller-but-consistent) table, so the
	// disk stays bootable and the grow is simply retried next boot -- it never
	// leaves the primary GPT corrupt.
	bool ok = gpt_write_blocks(fBlockIo, backupEntriesBlock, entryArrayBlocks,
			entries)
		&& gpt_write_blocks(fBlockIo, diskLastBlock, 1, backupBlock)
		&& gpt_write_blocks(fBlockIo, entriesBlock, entryArrayBlocks, entries)
		&& gpt_write_blocks(fBlockIo, EFI_HEADER_LOCATION, 1, headerBlock);

	if (ok && fBlockIo->FlushBlocks != NULL)
		fBlockIo->FlushBlocks(fBlockIo);

	if (!ok)
		dprintf("efi/devices: boot GPT partition grow failed to write\n");
	}

out:
	free(headerBlock);
	free(backupBlock);
	free(entries);
}
#endif	// __aarch64__


static bool
device_contains_partition(EfiDevice *device, boot::Partition *partition)
{
	EFI::Header *header = (EFI::Header*)partition->content_cookie;
	if (header != NULL && header->InitCheck() == B_OK) {
		// check if device is GPT, and contains partition entry
		uint32 blockSize = device->BlockSize();
		gpt_table_header *deviceHeader =
			(gpt_table_header*)malloc(blockSize);
		ssize_t bytesRead = device->ReadAt(NULL, blockSize, deviceHeader,
			blockSize);
		if (bytesRead != (ssize_t)blockSize)
			return false;

		if (memcmp(deviceHeader, &header->TableHeader(),
				sizeof(gpt_table_header)) != 0)
			return false;

		// partition->cookie == int partition entry index
		uint32 index = (uint32)(addr_t)partition->cookie;
		uint32 size = sizeof(gpt_partition_entry) * (index + 1);
		gpt_partition_entry *entries = (gpt_partition_entry*)malloc(size);
		bytesRead = device->ReadAt(NULL,
			deviceHeader->entries_block * blockSize, entries, size);
		if (bytesRead != (ssize_t)size)
			return false;

		if (memcmp(&entries[index], &header->EntryAt(index),
				sizeof(gpt_partition_entry)) != 0)
			return false;

		for (size_t i = 0; i < sizeof(kTypeMap) / sizeof(struct type_map); ++i)
			if (strcmp(kTypeMap[i].type, BFS_NAME) == 0)
				if (kTypeMap[i].guid == header->EntryAt(index).partition_type)
					return true;

		// Our partition has an EFI header, but we couldn't find one, so bail
		return false;
	}

	if ((partition->offset + partition->size) <= device->Size())
			return true;

	return false;
}


status_t
platform_add_boot_device(struct stage2_args *args, NodeList *devicesList)
{
	TRACE("%s: called\n", __func__);

	efi_block_io_protocol *blockIo;
	size_t memSize = 0;

	// If the bootloader was started from the network (net_stack_init will find
	// the ip= parameter in the LoadOptions), add network booting as the first
	// entry if available
	status_t error = net_stack_init();
	if (error != B_OK) {
		TRACE("Can't init network...\n");
	} else {
		TRACE("Network is initialized! Search for remote disk...\n");
		RemoteDisk *remoteDisk = RemoteDisk::FindAnyRemoteDisk();
		if (remoteDisk != NULL) {
			devicesList->Add(remoteDisk);
		}
	}

	// Read to zero sized buffer to get memory needed for handles
	if (kBootServices->LocateHandle(ByProtocol, &BlockIoGUID, 0, &memSize, 0)
			!= EFI_BUFFER_TOO_SMALL)
		panic("Cannot read size of block device handles!");

	uint32 noOfHandles = memSize / sizeof(efi_handle);

	efi_handle handles[noOfHandles];
	if (kBootServices->LocateHandle(ByProtocol, &BlockIoGUID, 0, &memSize,
			handles) != EFI_SUCCESS)
		panic("Failed to locate block devices!");

	// All block devices has one for the disk and one per partition
	// There is a special case for a device with one fixed partition
	// But we probably do not care about booting on that kind of device
	// So find all disk block devices and let Haiku do partition scan
	for (uint32 n = 0; n < noOfHandles; n++) {
		if (kBootServices->HandleProtocol(handles[n], &BlockIoGUID,
				(void**)&blockIo) != EFI_SUCCESS)
			panic("Cannot get block device handle!");

		TRACE("%s: %p: present: %s, logical: %s, removeable: %s, "
			"blocksize: %" PRIu32 ", lastblock: %" PRIu64 "\n",
			__func__, blockIo,
			blockIo->Media->MediaPresent ? "true" : "false",
			blockIo->Media->LogicalPartition ? "true" : "false",
			blockIo->Media->RemovableMedia ? "true" : "false",
			blockIo->Media->BlockSize, blockIo->Media->LastBlock);

		if (!blockIo->Media->MediaPresent
			|| blockIo->Media->LogicalPartition
			|| blockIo->Media->BlockSize == 0)
			continue;

		// The qemu flash device with a 256K block sizes sometime show up
		// in edk2. If flash is unconfigured, bad things happen on arm.
		// edk2 bug: https://bugzilla.tianocore.org/show_bug.cgi?id=2856
		// We're not ready for flash devices in efi, so skip anything odd.
		if (blockIo->Media->BlockSize > 8192)
			continue;

		EfiDevice *device = new(std::nothrow)EfiDevice(blockIo);
		if (device == NULL)
			panic("Can't allocate memory for block devices!");

#ifdef __aarch64__
		// Grow the boot disk's trailing BFS partition to fill the disk before
		// anything scans partitions or checksums the disk identifier. This must
		// happen here, before the partition scan and the disk-identifier
		// checksum in register_boot_file_system(), so the kernel sees the grown
		// partition and BFS grows the filesystem on the first mount. See
		// GrowBootGptPartition() for the full rationale. Grow-only and a no-op on
		// any inconsistency, so it is safe to attempt on every physical disk.
		device->GrowBootGptPartition();
#endif

		devicesList->Insert(device);
	}

	return devicesList->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_add_block_devices(struct stage2_args *args, NodeList *devicesList)
{
	TRACE("%s: called\n", __func__);

	//TODO: Currently we add all in platform_add_boot_device
	return devicesList->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_get_boot_partitions(struct stage2_args *args, Node *bootDevice,
		NodeList *partitions, NodeList *bootPartitions)
{
	NodeIterator iterator = partitions->GetIterator();
	boot::Partition *partition = NULL;
	while ((partition = (boot::Partition*)iterator.Next()) != NULL) {
		if (device_contains_partition((EfiDevice*)bootDevice, partition)) {
			bootPartitions->Insert(partition);
		}
	}

	return bootPartitions->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_register_boot_device(Node *device, disk_identifier* defaultDiskID)
{
	TRACE("%s: called\n", __func__);

	// TODO is there no better way to identify network boot?
	char buffer[11];
	device->GetName(buffer, sizeof(buffer));
	TRACE("Booting from %s\n", buffer);

	if (strcmp(buffer, "RemoteDisk") == 0) {
		TRACE("Booting from network\n");
		RemoteDisk* remoteDisk = static_cast<RemoteDisk*>(device);
		// Tell the kernel we're booting from network and forward the network
		// configuration
		auto ethernetInterface = NetStack::Default()->GetEthernetInterface();
		if (gBootParams.SetInt32(BOOT_METHOD, BOOT_METHOD_NET)
			|| gBootParams.AddInt64("client MAC",
				ethernetInterface->MACAddress().ToUInt64()) != B_OK
			|| gBootParams.AddInt32("client IP",
				ethernetInterface->IPAddress()) != B_OK
			|| gBootParams.AddInt32("server IP", remoteDisk->ServerIPAddress())
				!= B_OK
			|| gBootParams.AddInt32("server port", remoteDisk->ServerPort())
				!= B_OK) {
			return B_NO_MEMORY;
		}
	} else {
		// ...HARD_DISK, as we pick partition and have checksum (no need to use _CD)
		gBootParams.SetInt32(BOOT_METHOD, BOOT_METHOD_HARD_DISK);
	}

	gBootParams.SetData(BOOT_VOLUME_DISK_IDENTIFIER, B_RAW_TYPE,
		defaultDiskID, sizeof(disk_identifier));

	return B_OK;
}


void
platform_cleanup_devices()
{
}
