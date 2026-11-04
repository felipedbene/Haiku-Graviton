/*
 * down1078.c -- issue 1078: take an interface down and up while one socket
 * captures on it and another keeps starting and stopping a capture on it.
 *
 * Taking the device down calls each capture's event hook with the device's
 * monitor lock held; the hook used to unregister the capture, which takes
 * the device list lock. Starting or stopping a capture takes the device list
 * lock and then the monitor lock: ABBA.
 *
 * On tap/0 (frames written into /dev/tap/0, paced): socket A captures and
 * reads; a second thread starts and stops a capture on socket B; the main
 * thread takes tap/0 down and up again, N times. After each down, A must
 * report its capture ended (once it has read what was queued, its read
 * fails) and must be able to start a new
 * capture once the interface is up. A watchdog reports a HANG when nothing
 * moves for 15 s.
 *
 * Set the interface up first:  ifconfig tap/0 up
 * usage: down1078 [cycles]   (default 300; run as root)
 *        down1078 remove     a capture must end when tap/0 is deleted (ifconfig -d)
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char* kTap = "tap/0";
static volatile int sStop;
static volatile long sProgress;
static volatile long sWritten;
static volatile long sTogglerStarts;
static volatile const char* sStep = "setup";


static double
now()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}


static void
tap_request(struct ifreq* ifr)
{
	memset(ifr, 0, sizeof(*ifr));
	strlcpy(ifr->ifr_name, kTap, sizeof(ifr->ifr_name));
}


static void*
injector(void* arg)
{
	uint8_t frame[60];
	memset(frame, 0, sizeof(frame));
	memset(frame, 0xff, 6);
	frame[6] = 0x02; frame[11] = 0x78;
	frame[12] = 0x88; frame[13] = 0xb5;
	while (!sStop) {
		int fd = open("/dev/tap/0", O_RDWR);
		if (fd < 0) {
			usleep(1000);
			continue;
		}
		while (!sStop && write(fd, frame, sizeof(frame)) == sizeof(frame)) {
			sWritten++;
			usleep(50);
		}
		close(fd);
		usleep(1000);
	}
	return NULL;
}


/* Starts and stops a capture on its own socket, over and over. */
static void*
toggler(void* arg)
{
	int fd = socket(AF_LINK, SOCK_DGRAM, 0);
	struct ifreq ifr;
	while (!sStop) {
		tap_request(&ifr);
		if (ioctl(fd, SIOCSPACKETCAP, &ifr, sizeof(ifr)) == 0)
			sTogglerStarts++;
		usleep(200);
		tap_request(&ifr);
		ioctl(fd, SIOCCPACKETCAP, &ifr, sizeof(ifr));
		usleep(200);
		sProgress++;
	}
	close(fd);
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
			printf("HANG at step %s: no progress for 15 s (progress %ld,"
				" toggler starts %ld, frames written %ld)\n", sStep, p,
				sTogglerStarts, sWritten);
			fflush(stdout);
			_exit(99);
		}
	}
	return NULL;
}


static int
set_up(int control, int up)
{
	struct ifreq ifr;
	tap_request(&ifr);
	if (ioctl(control, SIOCGIFFLAGS, &ifr, sizeof(ifr)) < 0)
		return -1;
	if (up)
		ifr.ifr_flags |= IFF_UP;
	else
		ifr.ifr_flags &= ~IFF_UP;
	return ioctl(control, SIOCSIFFLAGS, &ifr, sizeof(ifr));
}


/* Reads until a frame arrives (1) or the read fails (0), within a second. */
static int
read_one(int fd, int* failed)
{
	uint8_t buf[2048];
	double end = now() + 1.0;
	*failed = 0;
	while (now() < end) {
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n > 0)
			return 1;
		if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
			*failed = 1;
			return 0;
		}
		usleep(200);
	}
	return 0;
}


/* "remove": a capture must end when its interface is removed. */
static int
remove_mode()
{
	pthread_t thread;
	pthread_create(&thread, NULL, injector, NULL);
	int capture = socket(AF_LINK, SOCK_DGRAM, 0);
	struct ifreq ifr;
	tap_request(&ifr);
	int r = ioctl(capture, SIOCSPACKETCAP, &ifr, sizeof(ifr));
	int failed;
	int frame = read_one(capture, &failed);
	sStop = 1;
	pthread_join(thread, NULL);
	int rc = system("ifconfig -d tap/0");
	double end = now() + 3.0;
	failed = 0;
	while (!failed && now() < end)
		read_one(capture, &failed);
	int pass = r == 0 && frame && failed;
	printf("%-12s %s  capture = %d, a frame before = %d, ifconfig -d tap/0"
		" = %d, capture ended within 3 s = %d\n", "remove", pass ? "PASS"
		: "FAIL", r, frame, rc, failed);
	printf("RESULT failures=%d\n", !pass);
	return !pass;
}


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "remove") == 0)
		return remove_mode();

	long cycles = argc > 1 ? atol(argv[1]) : 300;
	int control = socket(AF_INET, SOCK_DGRAM, 0);

	pthread_t threads[3];
	pthread_create(&threads[0], NULL, injector, NULL);
	pthread_create(&threads[1], NULL, toggler, NULL);
	pthread_create(&threads[2], NULL, watchdog, NULL);

	int capture = socket(AF_LINK, SOCK_DGRAM, 0);
	long restarted = 0, sawFrame = 0, sawEnd = 0, downOk = 0, upOk = 0;
	double start = now();
	for (long i = 0; i < cycles; i++) {
		struct ifreq ifr;
		int failed;

		sStep = "start capture";
		tap_request(&ifr);
		if (ioctl(capture, SIOCSPACKETCAP, &ifr, sizeof(ifr)) == 0)
			restarted++;
		sStep = "read before down";
		sawFrame += read_one(capture, &failed);

		sStep = "down";
		if (set_up(control, 0) == 0)
			downOk++;
		sStep = "read after down";
		/* frames queued before the down are read first; then the read
		 * must fail */
		double end = now() + 1.0;
		failed = 0;
		while (!failed && now() < end)
			read_one(capture, &failed);
		sawEnd += failed;

		sStep = "up";
		if (set_up(control, 1) == 0)
			upOk++;
		usleep(2000);
		sProgress++;
	}
	double wall = now() - start;
	sStop = 1;

	int pass = restarted == cycles && downOk == cycles && upOk == cycles
		&& sawEnd == cycles && sawFrame >= cycles * 9 / 10;
	printf("%-12s %s  %ld cycles in %.1fs: capture (re)started %ld, a frame"
		" before down %ld, capture ended at down %ld, down %ld, up %ld;"
		" toggler starts %ld; frames written %ld\n", "down-up",
		pass ? "PASS" : "FAIL", cycles, wall, restarted, sawFrame, sawEnd,
		downOk, upOk, sTogglerStarts, sWritten);
	printf("RESULT failures=%d\n", !pass);
	return !pass;
}
