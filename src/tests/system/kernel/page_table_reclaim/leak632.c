/*
 * leak632.c -- the #632 acceptance test, ported from the arm64 positive control
 * of #629: N x (mmap 64 KiB, touch it, munmap) in one child process, comparing
 * system used memory while the child is alive against after it exits.
 *
 *   leak632 [CYCLES] [random|default]
 *
 * "random" places each mapping at a random hint in a 512 GiB window (the
 * placement VMUserAddressSpace's kMaxRandomize produces), "default" lets the
 * kernel choose. Prints one LEAK632 line with the three readings in KiB.
 */
#include <OS.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>


static int64
used_kib(void)
{
	system_info info;
	get_system_info(&info);
	return (int64)info.used_pages * B_PAGE_SIZE / 1024;
}


static uint64_t
rand64(uint64_t* state)
{
	/* xorshift64*: deterministic, so both images churn the same addresses */
	uint64_t x = *state;
	x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
	*state = x;
	return x * 0x2545F4914F6CDD1DULL;
}


static void
churn(int cycles, int randomPlacement)
{
	const size_t size = 64 * 1024;
	uint64_t state = 0x632632632ULL;
	int failed = 0;
	for (int i = 0; i < cycles; i++) {
		void* hint = NULL;
		if (randomPlacement) {
			hint = (void*)(uintptr_t)((0x100000000ULL
				+ rand64(&state) % (512ULL << 30)) & ~(uint64_t)0xffff);
		}
		char* p = mmap(hint, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			failed++;
			continue;
		}
		for (size_t o = 0; o < size; o += B_PAGE_SIZE)
			p[o] = (char)i;
		munmap(p, size);
	}
	if (failed)
		fprintf(stderr, "leak632: %d mmaps failed\n", failed);
}


int
main(int argc, char** argv)
{
	int cycles = argc > 1 ? atoi(argv[1]) : 20000;
	int randomPlacement = argc > 2 ? strcmp(argv[2], "default") != 0 : 1;

	int toParent[2], toChild[2];
	if (pipe(toParent) != 0 || pipe(toChild) != 0)
		return 2;

	snooze(2000000);
	int64 baseline = used_kib();

	pid_t child = fork();
	if (child == 0) {
		churn(cycles, randomPlacement);
		char c = 'd';
		write(toParent[1], &c, 1);
		read(toChild[0], &c, 1);	/* stay alive until measured */
		_exit(0);
	}

	char c;
	read(toParent[0], &c, 1);
	snooze(1000000);
	int64 alive = used_kib();
	write(toChild[1], &c, 1);
	int status;
	waitpid(child, &status, 0);
	snooze(2000000);
	int64 after = used_kib();

	printf("LEAK632 placement=%s cycles=%d baseline=%lldKiB alive=%lldKiB "
		"(%+lld) after_exit=%lldKiB (%+lld)\n",
		randomPlacement ? "random" : "default", cycles, (long long)baseline,
		(long long)alive, (long long)(alive - baseline), (long long)after,
		(long long)(after - baseline));
	return 0;
}
