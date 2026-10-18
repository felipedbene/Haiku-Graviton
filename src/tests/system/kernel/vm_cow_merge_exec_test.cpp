/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Exercises the anonymous-cache merge that runs when a forked child execs
	(issue 834).

	The shape it builds:

	- a private anonymous area, every page of it dirtied, so the cache behind
	  it holds a large number of MODIFIED pages;
	- fork(), which turns that cache into a shared source and gives parent and
	  child a fresh consumer cache each;
	- a handful of writes, at the same offsets, in both the child and then the
	  parent, so the parent's consumer holds a few pages that shadow source
	  pages at those offsets and nothing maps the source pages any more;
	- exec() in the child, which deletes the child's consumer. The source is
	  then left with one consumer and is merged into it.

	Because the consumer has fewer pages than the source and spans the same
	range, that merge goes through VMAnonymousCache::_MergePagesSmallerConsumer,
	which frees the shadowed source pages -- the MODIFIED ones among them after
	clearing the state. Clearing it has to happen while the page is still in its
	cache: page_queue_for() reads the page's cache to find which modified queue
	the page is on, so a detached MODIFIED page faults on the NULL cache there.

	Getting a source page into MODIFIED in the first place takes memory
	pressure, and that is what the third argument is for. A freshly written
	anonymous page is ACTIVE with the hardware dirty bit set; it reaches
	MODIFIED only when the page daemon walks it down through INACTIVE, finds it
	unmapped and dirty, and queues it for the page writer -- and the daemon only
	walks when pages are scarce (vm_page.cpp, page_daemon's "apparently we
	really need memory" scan). So between the parent's writes and the child's
	exec the parent dirties a pressure region sized to not fit, which is exactly
	when the sixteen source pages it has just shadowed -- now mapped by nobody --
	are the best candidates the daemon has.

		./vm_cow_merge_exec_test 8 64 3072		# 3 GiB of pressure per round

	With no pressure argument the test exercises the same merge without being
	able to reach the MODIFIED arm of it, which is a much weaker thing to run.

	Nothing is asserted beyond survival: the failure is a kernel panic, so the
	test either prints its summary or never returns.
*/


#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/wait.h>

#include <OS.h>


static const int kShadowPages = 16;
static const int kDefaultRounds = 64;
static const size_t kDefaultAreaSize = 64 * 1024 * 1024;

static size_t sAreaSize = kDefaultAreaSize;
static size_t sPressureSize = 0;


/*!	Dirties a region sized to not fit, so the page daemon has to scan. The
	pages we actually care about are the ones it passes over on the way, so the
	region has to stay held until the merge has run -- see release_memory_pressure().
*/
static uint8*
acquire_memory_pressure()
{
	if (sPressureSize == 0)
		return NULL;

	uint8* pressure = (uint8*)mmap(NULL, sPressureSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pressure == MAP_FAILED) {
		// Not fatal: a reservation refused is pressure of its own.
		snooze(500000);
		return NULL;
	}

	for (size_t offset = 0; offset < sPressureSize; offset += B_PAGE_SIZE)
		pressure[offset] = 0x55;

	// Give the daemon two passes with it: the active-queue scan to push our
	// unmapped source pages down to INACTIVE, and the inactive-queue scan to
	// find them dirty there and move them to MODIFIED.
	snooze(500000);

	return pressure;
}


static void
release_memory_pressure(uint8* pressure)
{
	if (pressure != NULL)
		munmap(pressure, sPressureSize);
}


static int
child_main()
{
	// Reached through exec() only; the merge we are after happens in the
	// exec() that got us here, before main() runs.
	return 0;
}


static status_t
one_round(const char* self, int round)
{
	int goPipe[2];
	int readyPipe[2];
	if (pipe(goPipe) != 0 || pipe(readyPipe) != 0) {
		fprintf(stderr, "FAIL: pipe(): %s\n", strerror(errno));
		return B_ERROR;
	}

	uint8* area = (uint8*)mmap(NULL, sAreaSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED) {
		fprintf(stderr, "FAIL: mmap(%zu): %s\n", sAreaSize, strerror(errno));
		close(goPipe[0]);
		close(goPipe[1]);
		close(readyPipe[0]);
		close(readyPipe[1]);
		return B_ERROR;
	}

	const size_t pageSize = B_PAGE_SIZE;
	const size_t stride = (sAreaSize / kShadowPages) & ~(pageSize - 1);

	// Dirty every page. These become the source cache's dirty pages.
	for (size_t offset = 0; offset < sAreaSize; offset += pageSize)
		area[offset] = (uint8)(round + 1);

	pid_t child = fork();
	if (child < 0) {
		fprintf(stderr, "FAIL: fork(): %s\n", strerror(errno));
		munmap(area, sAreaSize);
		close(goPipe[0]);
		close(goPipe[1]);
		close(readyPipe[0]);
		close(readyPipe[1]);
		return B_ERROR;
	}

	if (child == 0) {
		close(goPipe[1]);
		close(readyPipe[0]);

		// Copy-on-write the pages the parent is about to copy as well. Both
		// sides have to leave the source page behind for it to be mapped by
		// nobody, which is the only way the page daemon will take it: a page
		// still mapped by the other side is requeued, never moved to MODIFIED.
		for (int i = 0; i < kShadowPages; i++)
			area[i * stride] = 0xcc;

		char token = 'r';
		write(readyPipe[1], &token, 1);
		close(readyPipe[1]);

		// Then exec. Execing is what deletes this cache and leaves the source
		// with the parent's cache as its only consumer, which is the merge.
		read(goPipe[0], &token, 1);
		close(goPipe[0]);

		execl(self, self, "--child", (char*)NULL);
		_exit(127);
	}

	close(goPipe[0]);
	close(readyPipe[1]);

	char token;
	if (read(readyPipe[0], &token, 1) != 1) {
		fprintf(stderr, "FAIL: round %d: child never reported ready\n", round);
		munmap(area, sAreaSize);
		close(readyPipe[0]);
		close(goPipe[1]);
		return B_ERROR;
	}
	close(readyPipe[0]);

	// Write the same pages, spread out, so this consumer cache ends up much
	// smaller than the source while shadowing it at those offsets.
	for (int i = 0; i < kShadowPages; i++)
		area[i * stride] = 0xee;

	// Those source pages are now mapped by nobody. Make the daemon notice them
	// before the child's exec runs the merge, and keep the pressure on across
	// the merge so the page writer cannot quietly take them back out of
	// MODIFIED in the meantime.
	uint8* pressure = acquire_memory_pressure();

	token = 'g';
	write(goPipe[1], &token, 1);
	close(goPipe[1]);

	int childStatus = 0;
	if (waitpid(child, &childStatus, 0) < 0) {
		fprintf(stderr, "FAIL: waitpid(): %s\n", strerror(errno));
		release_memory_pressure(pressure);
		munmap(area, sAreaSize);
		return B_ERROR;
	}

	release_memory_pressure(pressure);

	if (!WIFEXITED(childStatus) || WEXITSTATUS(childStatus) != 0) {
		fprintf(stderr, "FAIL: round %d: child did not exec cleanly "
			"(status 0x%x)\n", round, childStatus);
		munmap(area, sAreaSize);
		return B_ERROR;
	}

	// Read the shadowing pages back: the merge moved pages between caches
	// underneath this mapping, and a page taken off the wrong queue shows up
	// here as the wrong contents.
	for (int i = 0; i < kShadowPages; i++) {
		if (area[i * stride] != 0xee) {
			fprintf(stderr, "FAIL: round %d: page at offset %zu reads 0x%02x, "
				"expected 0xee\n", round, (size_t)i * stride,
				area[i * stride]);
			munmap(area, sAreaSize);
			return B_ERROR;
		}
	}

	munmap(area, sAreaSize);
	return B_OK;
}


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--child") == 0)
		return child_main();

	int rounds = kDefaultRounds;
	if (argc > 1)
		rounds = atoi(argv[1]);
	if (argc > 2)
		sAreaSize = (size_t)atoi(argv[2]) * 1024 * 1024;
	if (argc > 3)
		sPressureSize = (size_t)atoi(argv[3]) * 1024 * 1024;
	if (rounds <= 0 || sAreaSize < (size_t)kShadowPages * B_PAGE_SIZE) {
		fprintf(stderr, "usage: %s [rounds] [area MiB] [pressure MiB]\n",
			argv[0]);
		return 1;
	}

	printf("%s: %d round(s), %zu MiB area, %d shadowing page(s), "
		"%zu MiB pressure\n", argv[0], rounds, sAreaSize / (1024 * 1024),
		kShadowPages, sPressureSize / (1024 * 1024));

	for (int round = 0; round < rounds; round++) {
		if (one_round(argv[0], round) != B_OK) {
			printf("vm_cow_merge_exec_test: FAIL\n");
			return 1;
		}

		if ((round + 1) % 8 == 0) {
			printf("  %d/%d rounds survived\n", round + 1, rounds);
			fflush(stdout);
		}
	}

	printf("vm_cow_merge_exec_test: PASS\n");
	return 0;
}
