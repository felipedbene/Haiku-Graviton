/*
 * close1063.c -- PR 1063 review: close an AF_LINK socket while frames of its
 * type are arriving.
 *
 * The device's receive thread holds the device's receive lock while it hands
 * a frame to a link socket, which then takes the socket's lock. Unbinding
 * (close) used to hold the socket's lock while it took the receive lock to
 * unregister the socket's handler, and stopping a capture (SIOCCPACKETCAP)
 * did the same with the monitor lock: a lock-order inversion that only bites
 * when a frame for that very socket is being delivered at that moment.
 *
 * On EC2 the stack itself owns every frame type the VPC delivers, so no link
 * socket ever receives one there. A TAP interface does: frames written to
 * /dev/tap/0 are received by the stack as if they came off the wire. One
 * thread writes 0x88b5 frames to it as fast as it can, while the main thread
 * repeatedly
 *   bind:    binds a socket to 0x88b5 on the TAP interface, receives a frame
 *            (so its handler is in use), closes it
 *   capture: starts a capture on the TAP interface, receives a frame, stops
 *            it with SIOCCPACKETCAP, closes the socket
 *   capture-race: two threads start and stop a capture on one socket at
 *            once; then a fresh capture must still receive
 * A third thread reports progress every second; a phase that stops making
 * progress for 15 s is reported as a HANG (a deadlocked thread cannot be
 * killed, so the process then stays behind).
 *
 * Set the interface up first:  ifconfig tap/0 up
 * usage: close1063 [iterations [pause-us]]
 *   iterations per phase (default 3000); pause-us between injected frames
 *   (default 0, flat out). Run as root.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char* kTap = "tap/0";
static uint8_t sMac[6];
static volatile int sStop;
static volatile long sWritten;
static volatile long sProgress;
static volatile const char* sPhase = "setup";
static long sInjectPause;	/* microseconds between frames, 0 = flat out */


static double
now()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}


static void*
injector(void* arg)
{
	int fd = *(int*)arg;
	uint8_t frame[60];
	memset(frame, 0, sizeof(frame));
	memset(frame, 0xff, 6);
	frame[6] = 0x02; frame[7] = 0x10; frame[8] = 0x63;
	frame[9] = 0x10; frame[10] = 0x63; frame[11] = 0x01;
	frame[12] = 0x88;
	frame[13] = 0xb5;
	memcpy(frame + 14, "C1063", 5);
	while (!sStop) {
		if (write(fd, frame, sizeof(frame)) == (ssize_t)sizeof(frame))
			sWritten++;
		else
			usleep(100);
		if (sInjectPause > 0)
			usleep(sInjectPause);
	}
	return NULL;
}


static void*
watchdog(void* arg)
{
	long last = -1;
	double lastChange = now();
	while (!sStop) {
		sleep(1);
		long p = sProgress;
		if (p != last) {
			last = p;
			lastChange = now();
		} else if (now() - lastChange > 15) {
			printf("HANG phase %s: no progress for 15 s at iteration %ld"
				" (frames written %ld)\n", sPhase, p, sWritten);
			fflush(stdout);
			_exit(99);
		}
	}
	return NULL;
}


/* Receives one frame within a second; returns whether it got one. */
static int
receive_one(int fd)
{
	uint8_t buf[2048];
	double end = now() + 1.0;
	while (now() < end) {
		if (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0)
			return 1;
		usleep(200);
	}
	return 0;
}


struct race {
	int		fd;
	long	iterations;
	long	started[2];
};
static struct race* sRace;


static void*
race_thread(void* arg)
{
	int id = *(int*)arg;
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, kTap, sizeof(ifr.ifr_name));
	for (long i = 0; i < sRace->iterations; i++) {
		if (ioctl(sRace->fd, SIOCSPACKETCAP, &ifr, sizeof(ifr)) == 0)
			sRace->started[id]++;
		usleep(id == 0 ? 10 : 15);
		ioctl(sRace->fd, SIOCCPACKETCAP, &ifr, sizeof(ifr));
		sProgress++;
	}
	return NULL;
}


int
main(int argc, char** argv)
{
	long iterations = argc > 1 ? atol(argv[1]) : 3000;
	sInjectPause = argc > 2 ? atol(argv[2]) : 0;

	int fd = socket(AF_LINK, SOCK_DGRAM, 0);
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, kTap, sizeof(ifr.ifr_name));
	if (ioctl(fd, SIOCGIFADDR, &ifr, sizeof(ifr)) < 0) {
		printf("SIOCGIFADDR(AF_LINK) %s: %s (ifconfig %s up first)\n", kTap,
			strerror(errno), kTap);
		return 1;
	}
	struct sockaddr_dl sdl;
	memcpy(&sdl, &ifr.ifr_addr, sizeof(ifr) - offsetof(struct ifreq, ifr_addr)
		< sizeof(sdl) ? sizeof(ifr) - offsetof(struct ifreq, ifr_addr)
		: sizeof(sdl));
	memcpy(sMac, LLADDR(&sdl), 6);
	close(fd);

	int tap = open("/dev/tap/0", O_RDWR);
	if (tap < 0) {
		printf("open /dev/tap/0: %s\n", strerror(errno));
		return 1;
	}
	printf("%s: mac %02x:%02x:%02x:%02x:%02x:%02x, %ld iterations per phase\n",
		kTap, sMac[0], sMac[1], sMac[2], sMac[3], sMac[4], sMac[5],
		iterations);
	fflush(stdout);

	pthread_t injectThread, watchThread;
	pthread_create(&injectThread, NULL, injector, &tap);
	pthread_create(&watchThread, NULL, watchdog, NULL);

	int failures = 0;

	/* bind / receive / close */
	{
		sPhase = "bind";
		struct sockaddr_dl b;
		memset(&b, 0, sizeof(b));
		b.sdl_len = sizeof(b);
		b.sdl_family = AF_LINK;
		b.sdl_type = IFT_ETHER;
		b.sdl_e_type = htons(0x88b5);
		b.sdl_alen = 6;
		memcpy(LLADDR(&b), sMac, 6);
		long bound = 0, received = 0;
		double start = now();
		for (long i = 0; i < iterations; i++) {
			int s = socket(AF_LINK, SOCK_DGRAM, 0);
			if (bind(s, (struct sockaddr*)&b, sizeof(b)) == 0) {
				bound++;
				received += receive_one(s);
			}
			close(s);
			sProgress++;
		}
		int pass = bound == iterations && received == iterations;
		printf("%-16s %s  %ld/%ld bound, %ld received a frame before close,"
			" %.1fs, frames written %ld\n", "bind-close", pass ? "PASS" : "FAIL",
			bound, iterations, received, now() - start, sWritten);
		fflush(stdout);
		failures += !pass;
	}

	/* capture / receive / SIOCCPACKETCAP / close */
	{
		sPhase = "capture";
		long started = 0, received = 0, stopped = 0;
		double start = now();
		for (long i = 0; i < iterations; i++) {
			int s = socket(AF_LINK, SOCK_DGRAM, 0);
			memset(&ifr, 0, sizeof(ifr));
			strlcpy(ifr.ifr_name, kTap, sizeof(ifr.ifr_name));
			if (ioctl(s, SIOCSPACKETCAP, &ifr, sizeof(ifr)) == 0) {
				started++;
				received += receive_one(s);
				if (ioctl(s, SIOCCPACKETCAP, &ifr, sizeof(ifr)) == 0)
					stopped++;
			}
			close(s);
			sProgress++;
		}
		int pass = started == iterations && received == iterations
			&& stopped == iterations;
		printf("%-16s %s  %ld/%ld started, %ld received, %ld stopped, %.1fs,"
			" frames written %ld\n", "capture-stop", pass ? "PASS" : "FAIL",
			started, iterations, received, stopped, now() - start, sWritten);
		fflush(stdout);
		failures += !pass;
	}

	/* Two threads start and stop a capture on the same socket at once: a
	 * start must not slip in while a stop is still unregistering. Then a
	 * fresh capture must still see frames, so the device's list is intact. */
	{
		sPhase = "capture-race";
		struct race race;
		race.fd = socket(AF_LINK, SOCK_DGRAM, 0);
		race.iterations = iterations;
		race.started[0] = race.started[1] = 0;
		double start = now();
		pthread_t threads[2];
		int ids[2] = {0, 1};
		sRace = &race;
		for (int t = 0; t < 2; t++)
			pthread_create(&threads[t], NULL, race_thread, &ids[t]);
		for (int t = 0; t < 2; t++)
			pthread_join(threads[t], NULL);
		close(race.fd);

		int s = socket(AF_LINK, SOCK_DGRAM, 0);
		memset(&ifr, 0, sizeof(ifr));
		strlcpy(ifr.ifr_name, kTap, sizeof(ifr.ifr_name));
		int cap = ioctl(s, SIOCSPACKETCAP, &ifr, sizeof(ifr));
		int got = cap == 0 && receive_one(s);
		close(s);
		long total = race.started[0] + race.started[1];
		int pass = total > 0 && got;
		printf("%-16s %s  2 threads x %ld start/stop on one socket: %ld + %ld"
			" started, %.1fs; a fresh capture afterwards received = %d,"
			" frames written %ld\n", "capture-race", pass ? "PASS" : "FAIL",
			iterations, race.started[0], race.started[1], now() - start, got,
			sWritten);
		fflush(stdout);
		failures += !pass;
	}

	sStop = 1;
	pthread_join(injectThread, NULL);
	printf("RESULT failures=%d\n", failures);
	return failures;
}
