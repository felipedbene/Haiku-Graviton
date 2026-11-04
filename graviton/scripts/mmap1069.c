/*
 * mmap1069.c -- userland mmap/munmap churn, a regression check for issue 1069.
 *
 * Issue 1069 widened the arm64 kernel address space to the whole TTBR1 half.
 * User address spaces are untouched by that, but every user mapping is backed
 * by kernel structures (areas, caches, page tables) that the kernel now
 * allocates from the wider space, so this churns them hard and checks that no
 * byte goes astray: anonymous and file-backed mappings of varied sizes, a hole
 * punched in the middle of a mapping, MAP_FIXED over a live mapping, mprotect,
 * and a fork whose child checks its copy-on-write view.
 *
 * Usage: mmap1069 [iterations]   (default 2000). Prints "mmap1069: PASS" on
 * success; any mismatch prints what and where, and exits 1.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>


static long sPageSize;


static int
fail(const char* what, long iteration, size_t offset)
{
	fprintf(stderr, "mmap1069: FAIL %s (iteration %ld, offset %zu, errno %s)\n",
		what, iteration, offset, strerror(errno));
	exit(1);
	return 1;
}


static unsigned char
pattern(long iteration, size_t page)
{
	return (unsigned char)(iteration * 31 + page * 7 + 1);
}


static void
fill(unsigned char* base, size_t pages, long iteration)
{
	for (size_t i = 0; i < pages; i++) {
		base[i * sPageSize] = pattern(iteration, i);
		base[i * sPageSize + sPageSize - 1] = pattern(iteration, i) ^ 0xff;
	}
}


static void
check(const unsigned char* base, size_t first, size_t last, long iteration,
	const char* what)
{
	for (size_t i = first; i < last; i++) {
		if (base[i * sPageSize] != pattern(iteration, i)
			|| base[i * sPageSize + sPageSize - 1]
				!= (unsigned char)(pattern(iteration, i) ^ 0xff)) {
			fail(what, iteration, i * sPageSize);
		}
	}
}


int
main(int argc, char** argv)
{
	long iterations = argc > 1 ? atol(argv[1]) : 2000;
	sPageSize = sysconf(_SC_PAGESIZE);

	char path[] = "/tmp/mmap1069.XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0)
		return fail("mkstemp", 0, 0);
	unlink(path);
	const size_t kFilePages = 256;
	if (ftruncate(fd, kFilePages * sPageSize) != 0)
		return fail("ftruncate", 0, 0);

	for (long it = 0; it < iterations; it++) {
		// 1 page .. 16 MiB, varied
		size_t pages = 1 + (size_t)((it * 2654435761u) % 4096);
		size_t size = pages * sPageSize;

		unsigned char* anon = mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (anon == MAP_FAILED)
			return fail("mmap anon", it, 0);
		fill(anon, pages, it);
		check(anon, 0, pages, it, "anon readback");

		// punch a hole in the middle, the rest must survive
		if (pages >= 3) {
			size_t hole = pages / 3;
			if (munmap(anon + hole * sPageSize, sPageSize) != 0)
				return fail("munmap hole", it, hole * sPageSize);
			check(anon, 0, hole, it, "below the hole");
			check(anon, hole + 1, pages, it, "above the hole");

			// MAP_FIXED back into the hole, zero-filled
			unsigned char* fixed = mmap(anon + hole * sPageSize, sPageSize,
				PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
				-1, 0);
			if (fixed != anon + hole * sPageSize)
				return fail("mmap MAP_FIXED", it, hole * sPageSize);
			for (long b = 0; b < sPageSize; b++) {
				if (fixed[b] != 0)
					return fail("MAP_FIXED page not zero", it, b);
			}
		}

		if (mprotect(anon, sPageSize, PROT_READ) != 0)
			return fail("mprotect", it, 0);

		// fork every 100 iterations: the child must see the parent's data
		if (it % 100 == 0) {
			pid_t child = fork();
			if (child < 0)
				return fail("fork", it, 0);
			if (child == 0) {
				check(anon, 0, pages >= 3 ? pages / 3 : pages, it, "child view");
				if (pages >= 2) {
					anon[(pages - 1) * sPageSize] ^= 0x55;
						// copy-on-write: must not reach the parent
				}
				_exit(0);
			}
			int status;
			if (waitpid(child, &status, 0) != child || !WIFEXITED(status)
				|| WEXITSTATUS(status) != 0) {
				return fail("child", it, 0);
			}
			if (pages >= 2)
				check(anon, pages - 1, pages, it, "parent after child write");
		}

		if (munmap(anon, size) != 0)
			return fail("munmap", it, 0);

		// file-backed, shared: write through one mapping, read through another
		size_t filePages = 1 + (size_t)(it % kFilePages);
		unsigned char* a = mmap(NULL, filePages * sPageSize,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		unsigned char* b = mmap(NULL, filePages * sPageSize, PROT_READ,
			MAP_SHARED, fd, 0);
		if (a == MAP_FAILED || b == MAP_FAILED)
			return fail("mmap file", it, 0);
		fill(a, filePages, it);
		check(b, 0, filePages, it, "shared file view");
		munmap(a, filePages * sPageSize);
		munmap(b, filePages * sPageSize);
	}

	close(fd);
	printf("mmap1069: PASS (%ld iterations)\n", iterations);
	return 0;
}
