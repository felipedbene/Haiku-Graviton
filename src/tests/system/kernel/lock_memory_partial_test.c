/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 */

/*	lock_memory_etc() failing in the middle of an area.

	A read() from a raw disk device into a page-aligned user buffer is served
	by an IORequest that wires the buffer (IOBuffer::LockMemory() ->
	lock_memory_etc(B_READ_DEVICE)). Here the buffer is two pages of one area
	whose second page is read-only, so wiring the first page succeeds and the
	second fails with B_PERMISSION_DENIED. lock_memory_etc() then unwound
	[start, failure) through unlock_memory_etc(), which looks the area's wired
	range up by its exact base and size -- but the range had been registered
	for the whole two pages, so the lookup missed and the kernel panicked
	("VMArea::Unwire(...): no such range"). On a kernel that survives that, the
	range would also have leaked, leaving the area wired for good.

	Expected: every read() fails cleanly, the buffer can be unmapped and
	re-mapped many times (an area left wired would make munmap() wait), and
	after all rounds the second page is still read-only and the first still
	writable.

	Usage: lock_memory_partial_test [raw device] [rounds]
	Default device: the first NVMe disk, /dev/disk/nvme/0/raw. Needs root.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>


static double
now()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}


int
main(int argc, char** argv)
{
	const char* device = argc > 1 ? argv[1] : "/dev/disk/nvme/0/raw";
	int rounds = argc > 2 ? atoi(argv[2]) : 200;
	long pageSize = sysconf(_SC_PAGESIZE);
	int failures = 0;
	double worstUnmap = 0;
	int i;

	int fd = open(device, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", device, strerror(errno));
		return 2;
	}

	for (i = 0; i < rounds; i++) {
		char* buffer = mmap(NULL, 2 * pageSize, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (buffer == MAP_FAILED) {
			fprintf(stderr, "round %d: mmap: %s\n", i, strerror(errno));
			return 2;
		}
		if (mprotect(buffer + pageSize, pageSize, PROT_READ) != 0) {
			fprintf(stderr, "round %d: mprotect: %s\n", i, strerror(errno));
			return 2;
		}

		ssize_t bytes = pread(fd, buffer, 2 * pageSize, 0);
		if (bytes == 2 * pageSize) {
			fprintf(stderr, "round %d: pread wrote into a read-only page\n",
				i);
			failures++;
		}

		// the first page must still be usable, the second still read-only
		buffer[0] = 'x';
		if (buffer[0] != 'x') {
			fprintf(stderr, "round %d: first page not writable\n", i);
			failures++;
		}

		double start = now();
		if (munmap(buffer, 2 * pageSize) != 0) {
			fprintf(stderr, "round %d: munmap: %s\n", i, strerror(errno));
			failures++;
		}
		double elapsed = now() - start;
		if (elapsed > worstUnmap)
			worstUnmap = elapsed;
		if (elapsed > 1.0) {
			fprintf(stderr, "round %d: munmap took %.3f s (area left wired?)\n",
				i, elapsed);
			failures++;
		}

		if (i == 0) {
			printf("pread into [rw][ro]: %zd (%s)\n", bytes,
				bytes < 0 ? strerror(errno) : "no error");
		}
	}

	close(fd);
	printf("lock_memory_partial_test: %d rounds, worst munmap %.6f s, "
		"%d failure(s)\n", rounds, worstUnmap, failures);
	return failures == 0 ? 0 : 1;
}
