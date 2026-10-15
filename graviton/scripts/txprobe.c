/* txprobe -- time the transmit path from an ordinary userland thread (#582).
 *
 * Why this exists: an ICMP round trip cannot tell a transmit-side queue from a
 * receive-side one, because on this stack the echo reply is built and sent by
 * the network stack's single receive-FIFO consumer thread -- the same thread
 * that does TCP's transmit work. This probe sends from a thread that is NOT
 * that consumer, so what it measures is the egress chain alone:
 *
 *     send() -> udp -> ipv4 -> datalink -> ethernet -> ioctl -> ena_send()
 *               (txLock, the transmit bounce slots and the submission queue)
 *
 * Run it against a port nothing answers: no reply means no receive-side work,
 * so a large figure here is transmit-side by construction and a small figure
 * exonerates the transmit path.
 *
 * Timed with system_time() (CNTVCT_EL0/CNTFRQ_EL0), which is accurate on arm64
 * -- the known arm64 measurement bugs (#140, #103) are in counter/PMU
 * configuration, not in the clock rate.
 *
 * usage: txprobe <host> <port> <count> <interval-ms> [payload-bytes]
 */
#include <OS.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static int
compare_bigtime(const void* a, const void* b)
{
	bigtime_t left = *(const bigtime_t*)a;
	bigtime_t right = *(const bigtime_t*)b;
	return left < right ? -1 : (left > right ? 1 : 0);
}


int
main(int argc, char** argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s <host> <port> <count> <interval-ms> "
			"[payload]\n", argv[0]);
		return 2;
	}

	const char* host = argv[1];
	int port = atoi(argv[2]);
	int count = atoi(argv[3]);
	int intervalMs = atoi(argv[4]);
	size_t payload = argc > 5 ? (size_t)atol(argv[5]) : 64;

	char* data = (char*)malloc(payload);
	if (data == NULL)
		return 1;
	memset(data, 0x31, payload);

	bigtime_t* samples = (bigtime_t*)malloc(count * sizeof(bigtime_t));
	if (samples == NULL)
		return 1;

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = inet_addr(host);
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("connect");
		return 1;
	}

	/* One warm-up send: the first one pays for the route lookup and the ARP
	   entry, which is not what this measures. */
	send(fd, data, payload, 0);

	int taken = 0;
	for (int i = 0; i < count; i++) {
		bigtime_t start = system_time();
		ssize_t n = send(fd, data, payload, 0);
		bigtime_t elapsed = system_time() - start;
		if (n >= 0)
			samples[taken++] = elapsed;
		snooze(intervalMs * 1000LL);
	}
	close(fd);

	if (taken == 0) {
		fprintf(stderr, "txprobe: every send() failed\n");
		return 1;
	}

	bigtime_t total = 0;
	int overMilli = 0;
	for (int i = 0; i < taken; i++) {
		total += samples[i];
		if (samples[i] >= 1000)
			overMilli++;
	}
	qsort(samples, taken, sizeof(bigtime_t), compare_bigtime);

	printf("txprobe: n=%d min %lld us  p50 %lld us  p90 %lld us  p99 %lld us  "
		"max %lld us  mean %.1f us  >=1ms %d (%.1f%%)\n",
		taken, (long long)samples[0], (long long)samples[taken / 2],
		(long long)samples[(taken * 9) / 10],
		(long long)samples[(taken * 99) / 100],
		(long long)samples[taken - 1], (double)total / taken, overMilli,
		100.0 * overMilli / taken);
	return 0;
}
