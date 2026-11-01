/*
 * append892 -- concurrent O_APPEND writes must not overwrite each other
 * (issue 892).
 *
 * Each round, <threads> threads each open(O_WRONLY | O_APPEND) the same file,
 * wait at a barrier, write one 161-byte record and close. A correct append
 * leaves threads * 161 bytes holding every record exactly once. Two modes: the
 * file exists before the round ("pre-created"), or the racing opens create it
 * with O_CREAT ("created"). A third mode keeps the descriptors open and has
 * each thread append <burst> records ("burst").
 *
 * Usage: append892 [dir] [rounds] [threads]   (default /tmp 2000 2)
 * Build: cc -O2 -Wall -o append892 append892.c -lpthread   Exit: 0 iff no loss.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECORD 161
#define MAX_THREADS 16

static char sPath[256];
static int sThreads, sBurst, sCreate;
static pthread_barrier_t sBarrier;

static void
make_record(char* record, int thread, int index)
{
	memset(record, 'a' + thread, RECORD);
	snprintf(record, RECORD, "t%02d-r%04d|", thread, index);
	record[strlen(record)] = 'a' + thread;
	record[RECORD - 1] = '\n';
}

static void*
writer(void* arg)
{
	int thread = (int)(long)arg;
	int flags = O_WRONLY | O_APPEND | (sCreate ? O_CREAT : 0);
	int fd = open(sPath, flags, 0644);
	pthread_barrier_wait(&sBarrier);
	for (int i = 0; i < sBurst; i++) {
		char record[RECORD];
		make_record(record, thread, i);
		if (write(fd, record, RECORD) != RECORD)
			fprintf(stderr, "write: %s\n", strerror(errno));
	}
	close(fd);
	return NULL;
}

/* returns the number of missing records */
static int
round_once(void)
{
	unlink(sPath);
	if (!sCreate)
		close(open(sPath, O_CREAT | O_WRONLY | O_TRUNC, 0644));

	pthread_t threads[MAX_THREADS];
	pthread_barrier_init(&sBarrier, NULL, sThreads);
	for (int i = 0; i < sThreads; i++)
		pthread_create(&threads[i], NULL, writer, (void*)(long)i);
	for (int i = 0; i < sThreads; i++)
		pthread_join(threads[i], NULL);
	pthread_barrier_destroy(&sBarrier);

	int expected = sThreads * sBurst;
	struct stat st;
	stat(sPath, &st);
	char* data = malloc(st.st_size + 1);
	int fd = open(sPath, O_RDONLY);
	ssize_t got = read(fd, data, st.st_size);
	close(fd);

	int found = 0;
	for (int t = 0; t < sThreads; t++) {
		for (int i = 0; i < sBurst; i++) {
			char record[RECORD];
			make_record(record, t, i);
			for (ssize_t o = 0; o + RECORD <= got; o += RECORD) {
				if (memcmp(data + o, record, RECORD) == 0) {
					found++;
					break;
				}
			}
		}
	}
	free(data);
	if (st.st_size != (off_t)expected * RECORD && found == expected)
		return 1;
	return expected - found;
}

static int
run(const char* mode, int rounds, int threads, int burst, int create)
{
	sThreads = threads;
	sBurst = burst;
	sCreate = create;
	int lossyRounds = 0, lost = 0;
	for (int r = 0; r < rounds; r++) {
		int missing = round_once();
		if (missing > 0) {
			lossyRounds++;
			lost += missing;
		}
	}
	printf("%-4s %-11s rounds %d threads %d records/thread %d: rounds with a "
		"lost append %d, records lost %d\n", lossyRounds == 0 ? "PASS" : "FAIL",
		mode, rounds, threads, burst, lossyRounds, lost);
	return lossyRounds;
}

int
main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : "/tmp";
	int rounds = argc > 2 ? atoi(argv[2]) : 2000;
	int threads = argc > 3 ? atoi(argv[3]) : 2;
	if (threads < 2 || threads > MAX_THREADS)
		threads = 2;
	snprintf(sPath, sizeof(sPath), "%s/append892.%d", dir, (int)getpid());

	int failures = 0;
	failures += run("pre-created", rounds, threads, 1, 0) != 0;
	failures += run("created", rounds, threads, 1, 1) != 0;
	failures += run("burst", rounds / 20 + 1, 8, 200, 0) != 0;
	unlink(sPath);
	printf("append892: %d failed\n", failures);
	return failures;
}
