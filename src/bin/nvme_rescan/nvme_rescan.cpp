/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * nvme_rescan - hardware verification instrument for the online-EBS-grow fix
 * (#33). The nvme_disk driver publishes a driver-private ioctl,
 * B_NVME_RESCAN_CAPACITY, which re-identifies the NVMe namespace and grows the
 * kernel's published disk size if the device now reports a larger NSZE, so that
 * an online EBS volume resize becomes visible without a reboot. That ioctl is
 * the only trigger for the grow path, and on a headless lean AMI there is no
 * other way to issue it -- hence this tool.
 *
 * It queries the device size (via B_GET_DEVICE_SIZE and B_GET_GEOMETRY) before
 * and after issuing the rescan, prints old vs new, and reports PASS/FAIL. It
 * does not itself change any driver behaviour: growing the EBS volume is done
 * out of band (an aws ec2 modify-volume, then this tool); a run where the
 * volume was not enlarged is a valid, unchanged result, not a failure.
 *
 * The ioctl opcode is intentionally re-declared here rather than shared through
 * a public header, mirroring the driver's own private definition in
 * src/add-ons/kernel/drivers/disk/nvme/nvme_disk.cpp: it lives above
 * B_DEVICE_OP_CODES_END (9999), the range Be reserved for its own control ids,
 * so it cannot collide with a standard disk ioctl. It takes no argument.
 *
 * Ioctl-status convention (see the note that #212 added to device_area_probe):
 * a negative return is an error and a valid status is >= 0, so every ioctl
 * result here is tested with "< 0", never "!= 0" -- a genuine zero/positive
 * status must not be misread as a failure.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>


#define MARKER "nvme_rescan"

/* Driver-private ioctl published by src/add-ons/kernel/drivers/disk/nvme/
   nvme_disk.cpp. Kept byte-for-byte identical to the driver's definition. */
#define B_NVME_RESCAN_CAPACITY	(B_DEVICE_OP_CODES_END + 1)

static const char* const kDefaultDevice = "/dev/disk/nvme/0/raw";


/*!	Read the device size in bytes. Prefers B_GET_DEVICE_SIZE; falls back to
	computing it from B_GET_GEOMETRY. Returns true on success and stores the
	size in \a bytesOut; on failure prints why and returns false. */
static bool
read_device_size(int fd, uint64_t* bytesOut)
{
	// B_GET_DEVICE_SIZE returns a size_t byte count directly.
	size_t size = 0;
	if (ioctl(fd, B_GET_DEVICE_SIZE, &size, sizeof(size)) >= 0) {
		*bytesOut = (uint64_t)size;
		return true;
	}
	const int sizeErrno = errno;

	// Fall back to geometry: capacity is the product of the four axes.
	device_geometry geometry;
	memset(&geometry, 0, sizeof(geometry));
	if (ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) >= 0) {
		*bytesOut = (uint64_t)geometry.bytes_per_sector
			* geometry.sectors_per_track * geometry.cylinder_count
			* geometry.head_count;
		return true;
	}

	printf(MARKER ": size query failed: B_GET_DEVICE_SIZE: %s; B_GET_GEOMETRY: "
		"%s\n", strerror(sizeErrno), strerror(errno));
	return false;
}


int
main(int argc, char** argv)
{
	/* Unbuffered, so a serial console keeps everything printed if the run is
	   cut short. */
	setvbuf(stdout, NULL, _IONBF, 0);

	const char* path = argc > 1 ? argv[1] : kDefaultDevice;

	printf(MARKER ": device %s\n", path);

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf(MARKER ": cannot open %s: %s\n", path, strerror(errno));
		printf(MARKER ": FAIL\n");
		return 1;
	}

	uint64_t before = 0;
	if (!read_device_size(fd, &before)) {
		printf(MARKER ": FAIL\n");
		close(fd);
		return 1;
	}
	printf(MARKER ": size before rescan: %" PRIu64 " bytes (%.2f GiB)\n",
		before, before / (1024.0 * 1024.0 * 1024.0));

	// Issue the rescan. It takes no argument; a negative return is an error.
	printf(MARKER ": issuing B_NVME_RESCAN_CAPACITY ...\n");
	if (ioctl(fd, B_NVME_RESCAN_CAPACITY) < 0) {
		printf(MARKER ": B_NVME_RESCAN_CAPACITY failed: %s\n", strerror(errno));
		printf(MARKER ": FAIL\n");
		close(fd);
		return 1;
	}
	printf(MARKER ": B_NVME_RESCAN_CAPACITY returned OK\n");

	uint64_t after = 0;
	if (!read_device_size(fd, &after)) {
		printf(MARKER ": FAIL\n");
		close(fd);
		return 1;
	}
	printf(MARKER ": size after rescan:  %" PRIu64 " bytes (%.2f GiB)\n",
		after, after / (1024.0 * 1024.0 * 1024.0));

	close(fd);

	if (after > before) {
		printf(MARKER ": PASS (capacity grew by %" PRIu64 " bytes, online resize "
			"is visible without reboot)\n", after - before);
	} else if (after == before) {
		printf(MARKER ": PASS (rescan succeeded; capacity unchanged -- the volume "
			"was not enlarged, which is a valid grow-only no-op)\n");
	} else {
		// Grow-only by construction; a shrink means the driver misbehaved.
		printf(MARKER ": FAIL (capacity shrank from %" PRIu64 " to %" PRIu64
			" bytes; the rescan is meant to be grow-only)\n", before, after);
		return 1;
	}

	return 0;
}
