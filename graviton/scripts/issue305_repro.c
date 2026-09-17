/*
 * #305 net-stack lock-inversion repro.
 *
 * Drives the TEARDOWN edge of the AB-BA pair (fLock -> sSocketLock):
 * churns TCP sockets with a large SO_RCVBUF through connect()/close(), which
 * runs Connect/Listen -> set_max_backlog -> RemoveFromParent (takes sSocketLock
 * while holding fLock). Run concurrently with a socket-table enumeration loop
 * (`while true; do netstat -nt; done`) which drives the ENUMERATION edge
 * (sSocketLock -> fLock via socket_get_next_stat -> FillStat).
 *
 * A background "canary" thread times a fresh socket()+close() each 200 ms and
 * prints a monotonically increasing heartbeat with system_time() deltas. On the
 * buggy build the whole net stack freezes: the heartbeat stops advancing and
 * socket() never returns. On the fixed build the heartbeat keeps advancing.
 *
 * Usage: issue305_repro <seconds> <churn_threads>
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <OS.h>

static volatile bool sStop = false;
static volatile int64 sCanaryBeat = 0;
static volatile bigtime_t sCanaryLastStart = 0;

static void*
canary(void* arg)
{
	while (!sStop) {
		bigtime_t t0 = system_time();
		sCanaryLastStart = t0;
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		bigtime_t t1 = system_time();
		if (fd >= 0)
			close(fd);
		int64 beat = ++sCanaryBeat;
		if ((beat % 5) == 0) {
			printf("[canary] beat=%lld socket()=%lld us wall=%lld ms\n",
				beat, (long long)(t1 - t0), (long long)(t1 / 1000));
			fflush(stdout);
		}
		snooze(200000);
	}
	return NULL;
}

static void*
churn(void* arg)
{
	while (!sStop) {
		// Listener bound to an ephemeral loopback port.
		int lfd = socket(AF_INET, SOCK_STREAM, 0);
		if (lfd < 0)
			continue;
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) != 0
			|| listen(lfd, 8) != 0) {
			close(lfd);
			continue;
		}
		socklen_t alen = sizeof(addr);
		getsockname(lfd, (struct sockaddr*)&addr, &alen);

		// Client with a large receive buffer, connect, accept, tear all down.
		int cfd = socket(AF_INET, SOCK_STREAM, 0);
		if (cfd >= 0) {
			int rcv = 8 * 1024 * 1024;
			setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
			setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &rcv, sizeof(rcv));
			if (connect(cfd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
				int afd = accept(lfd, NULL, NULL);
				if (afd >= 0)
					close(afd);
			}
			close(cfd);
		}
		close(lfd);
	}
	return NULL;
}

int
main(int argc, char** argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 30;
	int threads = argc > 2 ? atoi(argv[2]) : 4;
	if (threads < 1)
		threads = 1;

	printf("[repro] #305: %d churn threads for %d s (large-SO_RCVBUF teardown)\n",
		threads, seconds);
	fflush(stdout);

	pthread_t canaryThread;
	pthread_create(&canaryThread, NULL, canary, NULL);

	pthread_t* churnThreads = (pthread_t*)malloc(sizeof(pthread_t) * threads);
	for (int i = 0; i < threads; i++)
		pthread_create(&churnThreads[i], NULL, churn, NULL);

	// Watch the canary from the main thread; declare a wedge if it stalls.
	int64 lastBeat = 0;
	int stalled = 0;
	for (int s = 0; s < seconds; s++) {
		snooze(1000000);
		int64 beat = sCanaryBeat;
		bigtime_t sinceStart = system_time() - sCanaryLastStart;
		if (beat == lastBeat) {
			stalled++;
			printf("[repro] t=%ds NO CANARY PROGRESS (beat stuck at %lld, "
				"socket() outstanding %lld ms) -- possible net-stack wedge\n",
				s, beat, (long long)(sinceStart / 1000));
		} else {
			printf("[repro] t=%ds canary beat=%lld (advancing)\n", s, beat);
			stalled = 0;
		}
		fflush(stdout);
		lastBeat = beat;
		if (stalled >= 5) {
			printf("[repro] VERDICT: WEDGED -- canary stalled >=5 s\n");
			fflush(stdout);
			// Do not join; the churn/canary threads are likely stuck in the
			// kernel. Exit so the harness can observe the verdict.
			_exit(2);
		}
	}

	sStop = true;
	printf("[repro] VERDICT: SURVIVED -- canary advanced for the full run\n");
	fflush(stdout);
	_exit(0);
}
