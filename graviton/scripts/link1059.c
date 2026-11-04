/*
 * link1059.c -- issues 1059 and 1061: which interface does an AF_LINK
 * address select?
 *
 * get_interface_for_link() picks the interface a link socket binds to, by the
 * address's hardware address, its interface name (sdl_nlen bytes of sdl_data,
 * not terminated) or its index. For each address below this binds a socket
 * to frame type 0x88b5 (a type the stack leaves alone), and if that works,
 * sends a frame and checks with a capture socket on the Ethernet device that
 * the frame left there, and from which source address.
 *
 * The expected result is the intended behaviour with #1037, #1059 and #1061
 * all applied:
 *   - by index, by name, by MAC, by name + own MAC: bound to the Ethernet
 *     device, the frame leaves it from the device's MAC
 *   - loopback, by index or by name: selected, but AF_LINK cannot use it,
 *     so EAFNOSUPPORT (before #1059's status change, EINVAL like a miss)
 *   - an unknown index or name, a name prefix, a name with a NUL inside its
 *     sdl_nlen bytes, an empty address, a name whose length runs past the
 *     address storage: EINVAL
 *   - a name with bytes after it that are not a NUL (no link address):
 *     bound -- sdl_nlen, not a terminator, ends the name
 *   - the device's name with another MAC: EINVAL, so no frame leaves with a
 *     foreign source (#1037's own-MAC rule)
 * Each row prints PASS/FAIL against that; the exit code is the FAIL count.
 *
 * usage: link1059 [ethernet-ifname]   (default /dev/net/ena/0; run as root)
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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MARKER	"L1059-"

static const char* sIfName;
static uint8_t sMac[6];
static int sWatchFd;
static int sFailures;


static double
now()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}


static const char*
mac_str(const uint8_t* m)
{
	static char buf[4][18];
	static int slot;
	char* b = buf[slot++ & 3];
	snprintf(b, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3],
		m[4], m[5]);
	return b;
}


enum expect {
	BOUND_HERE,		/* bound; the frame leaves the device from its MAC */
	LOOPBACK,		/* EAFNOSUPPORT: selected loopback */
	REFUSED			/* EINVAL */
};


/* Binds a 0x88b5 socket to a, sends a frame, reports where it went. */
static void
try_address(const char* name, struct sockaddr_dl* a, enum expect expected)
{
	a->sdl_family = AF_LINK;
	a->sdl_type = IFT_ETHER;
	a->sdl_e_type = htons(0x88b5);
	if (a->sdl_len == 0)
		a->sdl_len = sizeof(*a);

	int fd = socket(AF_LINK, SOCK_DGRAM, 0);
	int r = bind(fd, (struct sockaddr*)a, sizeof(struct sockaddr_storage));
	int bindErrno = r ? errno : 0;

	int seen = 0;
	uint8_t src[6] = {0};
	ssize_t n = -1;
	if (r == 0) {
		uint8_t buf[2048], payload[46];
		memset(payload, 0, sizeof(payload));
		snprintf((char*)payload, sizeof(payload), MARKER "%s", name);
		while (recv(sWatchFd, buf, sizeof(buf), MSG_DONTWAIT) > 0)
			;
		struct sockaddr_dl d;
		memset(&d, 0, sizeof(d));
		d.sdl_len = sizeof(d);
		d.sdl_family = AF_LINK;
		d.sdl_type = IFT_ETHER;
		d.sdl_e_type = htons(0x88b5);
		d.sdl_alen = 6;
		memset(LLADDR(&d), 0xff, 6);
		n = sendto(fd, payload, sizeof(payload), 0, (struct sockaddr*)&d,
			sizeof(d));
		double end = now() + 1.0;
		while (n > 0 && !seen && now() < end) {
			ssize_t got = recv(sWatchFd, buf, sizeof(buf), MSG_DONTWAIT);
			if (got <= 0) {
				usleep(1000);
				continue;
			}
			if (got >= 14 + (ssize_t)sizeof(payload) && buf[12] == 0x88
				&& buf[13] == 0xb5
				&& memcmp(buf + 14, payload, sizeof(payload)) == 0) {
				seen = 1;
				memcpy(src, buf + 6, 6);
			}
		}
	}
	close(fd);

	const char* got;
	int pass;
	if (r == 0) {
		got = seen ? (memcmp(src, sMac, 6) == 0 ? "bound, left the device from"
				" its MAC" : "bound, left the device from ANOTHER MAC")
			: "bound, frame NOT seen on the device";
		pass = expected == BOUND_HERE && seen && memcmp(src, sMac, 6) == 0;
	} else if (bindErrno == EAFNOSUPPORT) {
		got = "selected loopback (EAFNOSUPPORT)";
		pass = expected == LOOPBACK;
	} else {
		got = strerror(bindErrno);
		pass = expected == REFUSED && bindErrno == EINVAL;
	}
	static const char* kExpected[] = {"bound here", "loopback", "EINVAL"};
	printf("%-26s %s  bind=%d -> %s; source %s; expected %s\n", name,
		pass ? "PASS" : "FAIL", r, got, seen ? mac_str(src) : "-",
		kExpected[expected]);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


/* bind() is given a whole sockaddr_storage, so clear all of it */
static union {
	struct sockaddr_dl dl;
	struct sockaddr_storage storage;
} sAddress;


static struct sockaddr_dl*
cleared()
{
	memset(&sAddress, 0, sizeof(sAddress));
	return &sAddress.dl;
}


static void
with_name(struct sockaddr_dl* a, const char* name, size_t length)
{
	memset(&sAddress, 0, sizeof(sAddress));
	a->sdl_nlen = length;
	memcpy(a->sdl_data, name, strlen(name));
}


int
main(int argc, char** argv)
{
	sIfName = argc > 1 ? argv[1] : "/dev/net/ena/0";

	int fd = socket(AF_LINK, SOCK_DGRAM, 0);
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, sIfName, sizeof(ifr.ifr_name));
	if (ioctl(fd, SIOCGIFADDR, &ifr, sizeof(ifr)) < 0) {
		printf("SIOCGIFADDR(AF_LINK) %s: %s\n", sIfName, strerror(errno));
		return 1;
	}
	struct sockaddr_dl sdl;
	memcpy(&sdl, &ifr.ifr_addr, sizeof(ifr) - offsetof(struct ifreq, ifr_addr)
		< sizeof(sdl) ? sizeof(ifr) - offsetof(struct ifreq, ifr_addr)
		: sizeof(sdl));
	memcpy(sMac, LLADDR(&sdl), 6);
	close(fd);

	unsigned etherIndex = if_nametoindex(sIfName);
	unsigned loopIndex = if_nametoindex("loop");
	printf("%s: mac %s, device index %u, interface index %u; loop: interface"
		" index %u\n", sIfName, mac_str(sMac), sdl.sdl_index, etherIndex,
		loopIndex);

	sWatchFd = socket(AF_LINK, SOCK_DGRAM, 0);
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, sIfName, sizeof(ifr.ifr_name));
	if (ioctl(sWatchFd, SIOCSPACKETCAP, &ifr, sizeof(ifr)) < 0) {
		printf("SIOCSPACKETCAP: %s\n", strerror(errno));
		return 1;
	}

	struct sockaddr_dl* a = &sAddress.dl;
	static const uint8_t foreign0[6] = {0x00, 0x10, 0x59, 0x10, 0x61, 0x01};
	static const uint8_t foreignA[6] = {0x0a, 0x10, 0x59, 0x10, 0x61, 0x02};
	size_t nameLength = strlen(sIfName);

	/* by index */
	cleared();
	a->sdl_index = etherIndex;
	try_address("index-ether", a, BOUND_HERE);
	cleared();
	a->sdl_index = loopIndex;
	try_address("index-loop", a, LOOPBACK);
	cleared();
	a->sdl_index = 999;
	try_address("index-unknown", a, REFUSED);

	/* by name, NUL after it */
	with_name(a, sIfName, nameLength);
	try_address("name-ether", a, BOUND_HERE);
	with_name(a, "loop", 4);
	try_address("name-loop", a, LOOPBACK);
	with_name(a, "/dev/net/nosuch/0", strlen("/dev/net/nosuch/0"));
	try_address("name-unknown", a, REFUSED);

	/* by name, not NUL-terminated: other bytes follow the name */
	with_name(a, sIfName, nameLength);
	memcpy(a->sdl_data + nameLength, "XYZ", 3);
	try_address("name-ether-unterminated", a, BOUND_HERE);

	/* "loop", a NUL, then another byte, all within sdl_nlen 6: the name is
	 * not "loop" */
	with_name(a, "loop", 6);
	a->sdl_data[5] = 'X';
	try_address("name-embedded-nul", a, REFUSED);

	/* a prefix of the name, with the rest of the name after it */
	with_name(a, sIfName, 4);
	try_address("name-prefix", a, REFUSED);

	/* by MAC, and by name + MAC */
	cleared();
	a->sdl_alen = 6;
	memcpy(LLADDR(a), sMac, 6);
	try_address("mac-own", a, BOUND_HERE);
	with_name(a, sIfName, nameLength);
	a->sdl_alen = 6;
	memcpy(LLADDR(a), sMac, 6);
	try_address("name-ether+mac-own", a, BOUND_HERE);

	/* the device's name with another MAC: no foreign source */
	with_name(a, sIfName, nameLength);
	a->sdl_alen = 6;
	memcpy(LLADDR(a), foreign0, 6);
	try_address("name-ether+mac-foreign00", a, REFUSED);
	with_name(a, sIfName, nameLength);
	a->sdl_alen = 6;
	memcpy(LLADDR(a), foreignA, 6);
	try_address("name-ether+mac-foreign0a", a, REFUSED);

	/* malformed */
	cleared();
	try_address("empty", a, REFUSED);
	cleared();
	a->sdl_nlen = 120;
	a->sdl_alen = 6;
	try_address("name-length-past-end", a, REFUSED);

	printf("RESULT failures=%d\n", sFailures);
	return sFailures;
}
