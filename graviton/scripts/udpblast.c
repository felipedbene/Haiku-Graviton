/* udpblast -- minimal UDP transmit load generator (#582, arm P5).
 *
 * Why this exists: nettput is TCP-only, and TCP's transmit work on this stack
 * happens on the network stack's single receive-FIFO consumer thread (it is
 * emitted from TCPEndpoint::SegmentReceived()). UDP egress instead runs on the
 * calling thread, with no congestion control and -- when the datagrams are sent
 * to a port nothing answers -- no return traffic at all, so the receive
 * consumer stays idle. That is what tells a transmit-side queue apart from a
 * receive-side one under transmit load.
 *
 * Cross-compiled for Haiku arm64 with cross-tools-arm64; see
 * graviton/docs/ena-transmit-latency-account.md Part II for the run recipe.
 *
 * usage: udpblast <host> <port> <payload-bytes> <seconds> [pps-cap]
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>


static double
now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}


int
main(int argc, char** argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s <host> <port> <payload> <seconds> [pps]\n",
			argv[0]);
		return 2;
	}

	const char* host = argv[1];
	int port = atoi(argv[2]);
	size_t payload = (size_t)atol(argv[3]);
	double seconds = atof(argv[4]);
	double pps = argc > 5 ? atof(argv[5]) : 0.0;

	char* data = (char*)malloc(payload);
	if (data == NULL)
		return 1;
	memset(data, 0x5a, payload);

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

	unsigned long long sent = 0;
	unsigned long long failed = 0;
	double start = now();
	double deadline = start + seconds;
	for (;;) {
		double t = now();
		if (t >= deadline)
			break;
		if (pps > 0.0) {
			/* Open-loop pacing: a rate-capped arm exists to hold the frame
			   rate fixed while the flow count changes, so sleep when ahead of
			   schedule rather than spinning (a spin would itself be load). */
			double due = start + (double)sent / pps;
			if (due > t) {
				usleep((unsigned)((due - t) * 1e6) + 1);
				continue;
			}
		}
		ssize_t n = send(fd, data, payload, 0);
		if (n < 0)
			failed++;
		else
			sent++;
	}
	double elapsed = now() - start;

	printf("udpblast: %llu datagrams (%llu failed) in %.3f s -- %.0f pps, "
		"%.1f Mbit/s\n", sent, failed, elapsed, sent / elapsed,
		(sent * (double)payload * 8.0) / elapsed / 1e6);
	close(fd);
	return 0;
}
