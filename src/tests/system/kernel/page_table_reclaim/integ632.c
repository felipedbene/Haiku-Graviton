/*
 * integ632.c -- data-integrity stress for the x86_64 page-table reclaim (#632).
 * A wrong or missing TLB shootdown before a freed table is reused shows up as a
 * thread on another CPU reading through a stale paging-structure-cache entry
 * into a page that now belongs to someone else: wrong data, or a fault.
 *
 *   integ632 MINUTES [WORKERS]
 *
 * Each worker process runs:
 *  - 2 churn threads: random-hint mmaps of 1..1024 pages, every page filled
 *    with a pattern of (pid, address, generation), mprotect read-only and
 *    verify, split with a partial munmap (cut_area) and verify both halves;
 *  - a same-VA remap pair: an owner thread maps 16 pages at one fixed 2 MiB
 *    slot (alone in its slot, so its page table is freed on every munmap),
 *    publishes a generation, a reader thread on another CPU verifies it, the
 *    owner waits for the reader to stand down, munmaps (table reclaimed),
 *    and maps the same VA again with the next generation. A stale cached PDE
 *    on the reader's CPU walks the freed page table -- now another mapping's.
 *  - a long-lived working set rewritten and verified by 3 threads while a
 *    neighbour thread maps and unmaps whole 2 MiB slots right next to it, so
 *    page tables under the working set's page directory keep being freed;
 *  - fork + exit children (deletingAddressSpace path).
 * Every check compares exact bytes; any mismatch prints MISMATCH and fails.
 */
#include <OS.h>

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG 4096
#define SLOT (2 * 1024 * 1024)

static volatile int sStop = 0;
static volatile int sBad = 0;
static pid_t sPid;


static uint64_t
rnd(uint64_t* s)
{
	uint64_t x = *s;
	x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
	*s = x;
	return x * 0x2545F4914F6CDD1DULL;
}


static void
fill(char* p, size_t pages, uint64_t gen)
{
	for (size_t i = 0; i < pages; i++) {
		uint64_t* w = (uint64_t*)(p + i * PG);
		uint64_t a = (uint64_t)(uintptr_t)(p + i * PG);
		for (int k = 0; k < PG / 8; k += 4) {
			w[k] = (uint64_t)sPid; w[k + 1] = a; w[k + 2] = gen;
			w[k + 3] = a ^ 0x5a5aa5a5c3c33c3cULL ^ (uint64_t)k;
		}
	}
}


static int
check(const char* what, char* p, size_t pages, uint64_t gen)
{
	for (size_t i = 0; i < pages; i++) {
		uint64_t* w = (uint64_t*)(p + i * PG);
		uint64_t a = (uint64_t)(uintptr_t)(p + i * PG);
		for (int k = 0; k < PG / 8; k += 4) {
			uint64_t want[4] = { (uint64_t)sPid, a, gen,
				a ^ 0x5a5aa5a5c3c33c3cULL ^ (uint64_t)k };
			for (int j = 0; j < 4; j++) {
				if (w[k + j] != want[j]) {
					__sync_fetch_and_add(&sBad, 1);
					printf("MISMATCH %s pid=%d va=%#llx off=%#x want=%016llx "
						"got=%016llx xor=%016llx\n", what, (int)sPid,
						(unsigned long long)a, (k + j) * 8,
						(unsigned long long)want[j],
						(unsigned long long)w[k + j],
						(unsigned long long)(want[j] ^ w[k + j]));
					fflush(stdout);
					return 1;
				}
			}
		}
	}
	return 0;
}


/* The worker's window stays reserved PROT_NONE for its whole life, so no
   other mapping (thread stacks, churn) can ever land inside it. Mapping and
   unmapping inside it replace part of the reservation with MAP_FIXED: on
   Haiku that deletes the old area under the address space lock -- the
   UnmapArea() path that reclaims tables -- and a PROT_NONE area maps no
   pages, so the slot's tables empty out exactly as with munmap(). */
static char*
map_at(char* va, size_t pages)
{
	char* p = mmap(va, pages * PG, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}


static void
unmap_at(char* va, size_t pages)
{
	if (mmap(va, pages * PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS
			| MAP_FIXED, -1, 0) == MAP_FAILED) {
		printf("unmap_at %p failed\n", va);
		fflush(stdout);
	}
}


static void*
hint_addr(uint64_t* s)
{
	if (rnd(s) & 1)
		return NULL;
	return (void*)(uintptr_t)((0x100000000ULL + rnd(s) % (512ULL << 30))
		& ~(uint64_t)0xffff);
}


static void*
churn(void* arg)
{
	uint64_t s = (uint64_t)(uintptr_t)arg * 0x9e3779b97f4a7c15ULL + sPid;
	static const size_t sizes[] = { 1, 16, 64, 256, 1024 };
	uint64_t gen = 0;
	while (!sStop) {
		size_t n = sizes[rnd(&s) % 5];
		char* p = mmap(hint_addr(&s), n * PG, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			continue;
		fill(p, n, ++gen);
		if (n >= 16) {
			mprotect(p, n * PG, PROT_READ);
			check("churn-ro", p, n, gen);
			mprotect(p, n * PG, PROT_READ | PROT_WRITE);
			size_t q = n / 4;
			munmap(p + q * PG, q * PG);		/* cut_area split */
			check("cut-left", p, q, gen);
			check("cut-right", p + 2 * q * PG, n - 2 * q, gen);
			munmap(p, q * PG);
			munmap(p + 2 * q * PG, (n - 2 * q) * PG);
		} else {
			check("churn", p, n, gen);
			munmap(p, n * PG);
		}
	}
	return NULL;
}


/* same-VA remap pair */
static char* sRemapVa;
static volatile char* sRemapCur = NULL;
static volatile uint64_t sRemapGen = 0;
static volatile int sReaderActive = 0;
static volatile int sReaderStandDown = 0;
static volatile long sRemapRounds = 0;


static void*
remap_owner(void* arg)
{
	(void)arg;
	uint64_t gen = 0;
	while (!sStop) {
		char* p = map_at(sRemapVa, 16);
		if (p == NULL) {
			snooze(1000);
			continue;
		}
		fill(p, 16, ++gen);
		if (gen == 5 && getenv("INTEG632_INJECT") != NULL)
			((volatile uint64_t*)p)[PG / 8 * 3 + 2] ^= 0x400;	/* self-test */
		sRemapGen = gen;
		__sync_synchronize();
		sRemapCur = p;
		snooze(200 + gen % 800);
		sReaderStandDown = 1;
		__sync_synchronize();
		while (sReaderActive && !sStop)
			sched_yield();
		sRemapCur = NULL;
		__sync_synchronize();
		sReaderStandDown = 0;
		unmap_at(p, 16);		/* the only mapping in its slot: table freed */
		sRemapRounds++;
	}
	return NULL;
}


static void*
remap_reader(void* arg)
{
	(void)arg;
	while (!sStop) {
		if (sReaderStandDown || sRemapCur == NULL) {
			sched_yield();
			continue;
		}
		sReaderActive = 1;
		__sync_synchronize();
		char* p = (char*)sRemapCur;
		uint64_t gen = sRemapGen;
		if (!sReaderStandDown && p != NULL)
			check("remap-reader", p, 16, gen);
		sReaderActive = 0;
		__sync_synchronize();
		while (sReaderStandDown && !sStop)
			sched_yield();
	}
	return NULL;
}


/* working set + neighbour slots */
#define WS_SLOTS 8
static char* sWs;
static uint64_t sWsGen[3];


static void*
ws_toucher(void* arg)
{
	int k = (int)(intptr_t)arg;
	while (!sStop) {
		sWsGen[k]++;
		for (int slot = 0; slot < WS_SLOTS; slot++) {
			for (int i = k; i < 64; i += 3)
				fill(sWs + (size_t)slot * SLOT + (size_t)i * PG, 1, sWsGen[k]);
		}
		for (int slot = 0; slot < WS_SLOTS; slot++) {
			for (int i = k; i < 64; i += 3) {
				check("wset", sWs + (size_t)slot * SLOT + (size_t)i * PG, 1,
					sWsGen[k]);
			}
		}
	}
	return NULL;
}


static void*
ws_neighbour(void* arg)
{
	(void)arg;
	uint64_t s = 0x1234 ^ sPid, gen = 0;
	/* slots WS_SLOTS..2*WS_SLOTS-1: whole slots, so their tables empty out */
	while (!sStop) {
		int slot = WS_SLOTS + (int)(rnd(&s) % WS_SLOTS);
		char* va = sWs + (size_t)slot * SLOT;
		char* p = map_at(va, 32);
		if (p == NULL)
			continue;
		fill(p, 32, ++gen);
		check("nbr", p, 32, gen);
		unmap_at(p, 32);
	}
	return NULL;
}


static int
worker(double minutes, int idx)
{
	sPid = getpid();
	uint64_t s = 0x632 + (uint64_t)idx * 7919;

	/* a 2 MiB-aligned private window: slots 0..7 working set (first 64 pages
	   of each), 8..15 neighbour churn, 16 the same-VA remap slot */
	size_t span = (size_t)(2 * WS_SLOTS + 2) * SLOT;
	char* raw = mmap(NULL, span + SLOT, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (raw == MAP_FAILED)
		return 2;
	char* base = (char*)(((uintptr_t)raw + SLOT - 1) & ~(uintptr_t)(SLOT - 1));
	sWs = base;
	sRemapVa = base + (size_t)(2 * WS_SLOTS) * SLOT;
	for (int slot = 0; slot < WS_SLOTS; slot++) {
		if (map_at(sWs + (size_t)slot * SLOT, 64) == NULL)
			return 2;
	}

	pthread_t th[9];
	int n = 0;
	pthread_create(&th[n++], NULL, churn, (void*)(uintptr_t)(idx * 2));
	pthread_create(&th[n++], NULL, churn, (void*)(uintptr_t)(idx * 2 + 1));
	pthread_create(&th[n++], NULL, remap_owner, NULL);
	pthread_create(&th[n++], NULL, remap_reader, NULL);
	for (int k = 0; k < 3; k++)
		pthread_create(&th[n++], NULL, ws_toucher, (void*)(intptr_t)k);
	pthread_create(&th[n++], NULL, ws_neighbour, NULL);

	bigtime_t end = system_time() + (bigtime_t)(minutes * 60e6);
	long forks = 0;
	while (system_time() < end && !sBad) {
		pid_t c = fork();
		if (c == 0) {
			/* a child that maps, touches and exits: deletingAddressSpace */
			uint64_t cs = s ^ (uint64_t)getpid();
			for (int i = 0; i < 64; i++) {
				char* p = mmap(hint_addr(&cs), 16 * PG, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
				if (p != MAP_FAILED)
					memset(p, i, 16 * PG);
			}
			_exit(0);
		}
		if (c > 0) {
			waitpid(c, NULL, 0);
			forks++;
		}
		snooze(20000);
	}
	sStop = 1;
	for (int i = 0; i < n; i++)
		pthread_join(th[i], NULL);
	printf("WORKER %d pid=%d bad=%d remap_rounds=%ld wset_gens=%llu forks=%ld\n",
		idx, (int)sPid, sBad, sRemapRounds,
		(unsigned long long)(sWsGen[0] + sWsGen[1] + sWsGen[2]), forks);
	fflush(stdout);
	return sBad ? 1 : 0;
}


int
main(int argc, char** argv)
{
	double minutes = argc > 1 ? atof(argv[1]) : 10;
	int workers = argc > 2 ? atoi(argv[2]) : 8;
	bigtime_t t0 = system_time();
	pid_t pids[256];
	for (int i = 0; i < workers && i < 256; i++) {
		pids[i] = fork();
		if (pids[i] == 0)
			_exit(worker(minutes, i));
	}
	int failed = 0, crashed = 0;
	for (int i = 0; i < workers && i < 256; i++) {
		int status;
		waitpid(pids[i], &status, 0);
		if (!WIFEXITED(status))
			crashed++;
		else if (WEXITSTATUS(status) != 0)
			failed++;
	}
	printf("INTEG632_DONE workers=%d minutes=%.1f failed_workers=%d crashed=%d "
		"elapsed=%llds\n", workers, minutes, failed, crashed,
		(long long)((system_time() - t0) / 1000000));
	return failed || crashed;
}
