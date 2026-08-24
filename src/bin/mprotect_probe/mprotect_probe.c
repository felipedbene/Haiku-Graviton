/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * mprotect_probe - unprivileged probe for the arm64 Query() valid-bit defect.
 *
 * VMSAv8TranslationMap::Query() walks down to the level-3 table and then reads
 * whatever slot the address selects. A level-3 table is allocated as soon as
 * *any* single page in its 2MB span is mapped, so slots belonging to pages that
 * were never faulted in are reachable and hold an invalid (zero) entry. An
 * unpatched Query() sets PAGE_PRESENT for such a slot and hands back a zero
 * physical address. _user_set_memory_protection() trusts PAGE_PRESENT, feeds
 * the zero address to vm_lookup_page(), gets NULL and panics -- so an ordinary
 * unprivileged mprotect() can take the kernel down.
 *
 * Each probe below builds a range that deliberately mixes faulted-in and
 * never-touched pages inside one level-3 table, then calls mprotect() over it.
 * On an unpatched kernel the call never returns. On a patched kernel every
 * probe returns and the program prints its final PASS line.
 *
 * Output is unbuffered so that the serial console keeps everything printed
 * before a panic.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>


#define MARKER "mprotect_probe"

/* arm64 with 4KB granules and 48-bit VAs: one level-3 table describes 512
   pages, so a 2MB-aligned run of pages is covered by a single table. */
static const size_t kPageSize = 4096;
static const size_t kL3Span = 512 * 4096;


/* Reserve a run of pages that starts on a level-3 table boundary, so that
   every page we touch below is described by the same level-3 table. */
static char*
reserve_aligned_run(void** _rawBase, size_t* _rawSize)
{
	size_t rawSize = kL3Span * 2;
	char* raw = (char*)mmap(NULL, rawSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED) {
		fprintf(stderr, MARKER ": mmap failed: %s\n", strerror(errno));
		return NULL;
	}

	*_rawBase = raw;
	*_rawSize = rawSize;

	return (char*)(((size_t)raw + kL3Span - 1) & ~(kL3Span - 1));
}


static void
release_run(void* rawBase, size_t rawSize)
{
	if (rawBase != NULL)
		munmap(rawBase, rawSize);
}


/*
 * touchIndex   - the single page we fault in, to force the level-3 table to
 *                exist (-1 to touch nothing).
 * firstIndex   - first page of the mprotect() range.
 * pageCount    - length of the mprotect() range, in pages.
 */
static int
probe(const char* name, long touchIndex, size_t firstIndex, size_t pageCount)
{
	void* rawBase = NULL;
	size_t rawSize = 0;
	char* run = reserve_aligned_run(&rawBase, &rawSize);
	if (run == NULL)
		return -1;

	if (touchIndex >= 0)
		run[(size_t)touchIndex * kPageSize] = 0x42;

	printf(MARKER ": %s: mprotect(pages %zu..%zu, touched page %ld) ...\n",
		name, firstIndex, firstIndex + pageCount - 1, touchIndex);

	if (mprotect(run + firstIndex * kPageSize, pageCount * kPageSize,
			PROT_READ) != 0) {
		printf(MARKER ": %s: mprotect failed: %s\n", name, strerror(errno));
		release_run(rawBase, rawSize);
		return -1;
	}

	printf(MARKER ": %s: returned\n", name);

	release_run(rawBase, rawSize);
	return 0;
}


int
main(void)
{
	/* Unbuffered: a panic must not swallow the progress we already printed. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	printf(MARKER ": start\n");

	/* The first page is mapped, the rest of the range never was. The first
	   slot Query() sees is valid, the second is not. */
	if (probe("head-mapped-tail-untouched", 0, 0, 16) != 0)
		return 1;

	/* The range starts on an invalid slot: the very first Query() of the loop
	   is the one that must report "not present". */
	if (probe("head-untouched", 1, 0, 16) != 0)
		return 1;

	/* Nothing in the range was ever touched, but the level-3 table exists
	   because a page far away inside its span was faulted in. */
	if (probe("all-untouched", 500, 0, 16) != 0)
		return 1;

	/* The whole level-3 table in one call: 2 valid slots, 510 invalid. */
	if (probe("full-l3-span", 0, 0, 512) != 0)
		return 1;

	printf(MARKER ": PASS (all probes returned, kernel survived)\n");
	return 0;
}
