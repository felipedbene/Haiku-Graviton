/*
 * linksend1037.c -- issue 1037: can userland send an Ethernet frame of a type
 * the stack already receives itself (ARP, IPv4, IPv6), and do the frames
 * reach the wire?
 *
 * Builds whole Ethernet frames, the way libpcap's pcap_inject() and libdnet's
 * eth_send() receive them, and puts them on the wire:
 *   Haiku: an AF_LINK socket bound to the device with no frame type (send
 *          only); each frame goes out with sendto(), whose sockaddr_dl carries
 *          the frame type and the destination address. A second AF_LINK
 *          socket in packet-capture mode (SIOCSPACKETCAP) watches the device.
 *   Linux: an AF_PACKET SOCK_RAW socket bound to the interface sends the
 *          whole frame; a second one watches every frame in and out (a
 *          packet socket does not see its own sends).
 *
 * Rows (each prints PASS/FAIL; the exit code is the FAIL count):
 *   typed-bind-*      bind() with a frame type: the receive claim. ARP stays
 *                     refused on Haiku (the stack owns it); 0x88b5 is fine.
 *   arp               broadcast an ARP request for <peer-ip>; an ARP reply
 *                     must come back. Gives the peer's MAC for the next rows.
 *   icmp              ICMP echo request to <peer-ip>; the peer's echo reply
 *                     must come back (on-wire delivery, both directions).
 *   udp               a UDP datagram with a marker to <peer-ip>:9, for the
 *                     peer's tcpdump.
 *   ipv6              ICMPv6 echo to ff02::1 from the EUI-64 link-local
 *                     address; it must be seen leaving the device.
 *   x88b5             a frame of an unclaimed type; seen leaving.
 *   bind-by-name+mac  (Haiku) a send-only socket bound by name and address,
 *                     whose address follows the name, frames with it.
 *   foreign-mac-bind* (Haiku) an address naming the device but carrying
 *                     another MAC is refused, so no frame leaves with it.
 *   bound-capture-close (Haiku) a typed socket that also captured releases
 *                     its frame type on close.
 *   bad-*             (Haiku) malformed destinations are refused with EINVAL:
 *                     no frame type, no address, a name that pushes the
 *                     address out of the sockaddr, not AF_LINK, too short.
 *   bad-dst-short-bcast, bad-bind-short (Haiku) a short sendto() address under
 *                     MSG_BCAST, a short bind address that claims a MAC:
 *                     EINVAL.
 *   nonroot-*         (Haiku) a non-root process may neither bind nor send.
 *   burst             N ARP requests and N ICMP echoes back to back.
 *
 * usage: linksend1037 <ifname> <peer-ipv4> [burst-count]
 *   Haiku: linksend1037 /dev/net/ena/0 10.128.4.20 2000
 *   Linux: linksend1037 ens5 10.128.4.30 2000
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __HAIKU__
#	include <sys/sockio.h>
#	include <net/if_dl.h>
#	include <net/if_types.h>
#else
#	include <linux/if_packet.h>
#	include <net/ethernet.h>
#endif

#define MARKER	"J1037-LINKSEND-"

static int sFailures;
static const char* sIfName;
static uint8_t sMac[6];
static uint32_t sIp;		/* network order */
static uint32_t sPeerIp;	/* network order */
static uint8_t sPeerMac[6];
static int sHavePeerMac;
static uint16_t sIcmpId;

static int sSendFd = -1;	/* Haiku: send-only link socket; Linux: packet */
static int sWatchFd = -1;	/* Haiku: capture socket; Linux: same packet */
#ifndef __HAIKU__
static int sIfIndex;
#endif


static void
row(const char* name, int pass, const char* fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void
row(const char* name, int pass, const char* fmt, ...)
{
	va_list args;
	printf("%-22s %s  ", name, pass ? "PASS" : "FAIL");
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	putchar('\n');
	fflush(stdout);
	if (!pass)
		sFailures++;
}


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


static uint16_t
csum(const void* data, size_t len, uint32_t sum)
{
	const uint8_t* p = (const uint8_t*)data;
	while (len > 1) {
		sum += (p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return htons(~sum & 0xffff);
}


static size_t
eth_header(uint8_t* f, const uint8_t* dst, uint16_t type)
{
	memcpy(f, dst, 6);
	memcpy(f + 6, sMac, 6);
	f[12] = type >> 8;
	f[13] = type & 0xff;
	return 14;
}


static size_t
pad(uint8_t* f, size_t len)
{
	if (len < 60) {
		memset(f + len, 0, 60 - len);
		len = 60;
	}
	return len;
}


static const uint8_t kBroadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};


static size_t
build_arp_request(uint8_t* f, uint32_t target)
{
	size_t n = eth_header(f, kBroadcast, 0x0806);
	uint8_t* a = f + n;
	a[0] = 0; a[1] = 1;			/* Ethernet */
	a[2] = 0x08; a[3] = 0x00;	/* IPv4 */
	a[4] = 6; a[5] = 4;
	a[6] = 0; a[7] = 1;			/* request */
	memcpy(a + 8, sMac, 6);
	memcpy(a + 14, &sIp, 4);
	memset(a + 18, 0, 6);
	memcpy(a + 24, &target, 4);
	return pad(f, n + 28);
}


static size_t
ipv4_header(uint8_t* p, uint8_t proto, size_t payload, uint16_t id)
{
	size_t total = 20 + payload;
	p[0] = 0x45;
	p[1] = 0;
	p[2] = total >> 8;
	p[3] = total & 0xff;
	p[4] = id >> 8;
	p[5] = id & 0xff;
	p[6] = 0x40;	/* DF */
	p[7] = 0;
	p[8] = 64;
	p[9] = proto;
	p[10] = p[11] = 0;
	memcpy(p + 12, &sIp, 4);
	memcpy(p + 16, &sPeerIp, 4);
	uint16_t c = csum(p, 20, 0);
	memcpy(p + 10, &c, 2);
	return 20;
}


static size_t
build_icmp_echo(uint8_t* f, uint16_t seq)
{
	char marker[32];
	snprintf(marker, sizeof(marker), MARKER "ICMP-%05u", seq);
	size_t payload = 8 + strlen(marker);
	size_t n = eth_header(f, sPeerMac, 0x0800);
	n += ipv4_header(f + n, 1, payload, seq);
	uint8_t* i = f + n;
	i[0] = 8; i[1] = 0; i[2] = i[3] = 0;
	i[4] = sIcmpId >> 8; i[5] = sIcmpId & 0xff;
	i[6] = seq >> 8; i[7] = seq & 0xff;
	memcpy(i + 8, marker, strlen(marker));
	uint16_t c = csum(i, payload, 0);
	memcpy(i + 2, &c, 2);
	return pad(f, n + payload);
}


static size_t
build_udp(uint8_t* f)
{
	const char* marker = MARKER "UDP";
	size_t payload = 8 + strlen(marker);
	size_t n = eth_header(f, sPeerMac, 0x0800);
	n += ipv4_header(f + n, 17, payload, 0x1037);
	uint8_t* u = f + n;
	u[0] = 0x10; u[1] = 0x37;	/* source port 4151 */
	u[2] = 0; u[3] = 9;			/* discard */
	u[4] = payload >> 8; u[5] = payload & 0xff;
	u[6] = u[7] = 0;			/* no checksum */
	memcpy(u + 8, marker, strlen(marker));
	return pad(f, n + payload);
}


static size_t
build_ipv6_echo(uint8_t* f)
{
	static const uint8_t dstMac[6] = {0x33, 0x33, 0, 0, 0, 1};
	const char* marker = MARKER "ICMPV6";
	size_t payload = 8 + strlen(marker);
	size_t n = eth_header(f, dstMac, 0x86dd);
	uint8_t* h = f + n;
	memset(h, 0, 40);
	h[0] = 0x60;
	h[4] = payload >> 8;
	h[5] = payload & 0xff;
	h[6] = 58;		/* ICMPv6 */
	h[7] = 255;
	uint8_t* src = h + 8;
	src[0] = 0xfe; src[1] = 0x80;
	src[8] = sMac[0] ^ 0x02; src[9] = sMac[1]; src[10] = sMac[2];
	src[11] = 0xff; src[12] = 0xfe;
	src[13] = sMac[3]; src[14] = sMac[4]; src[15] = sMac[5];
	uint8_t* dst = h + 24;
	dst[0] = 0xff; dst[1] = 0x02; dst[15] = 1;
	uint8_t* i = h + 40;
	i[0] = 128; i[1] = 0; i[2] = i[3] = 0;
	i[4] = 0x10; i[5] = 0x37; i[6] = 0; i[7] = 1;
	memcpy(i + 8, marker, strlen(marker));
	/* pseudo-header: source, destination, length, next header */
	uint32_t sum = 0;
	for (int k = 0; k < 32; k += 2)
		sum += (h[8 + k] << 8) | h[9 + k];
	sum += payload + 58;
	uint16_t c = csum(i, payload, sum);
	memcpy(i + 2, &c, 2);
	return n + 40 + payload;
}


static size_t
build_x88b5(uint8_t* f)
{
	size_t n = eth_header(f, kBroadcast, 0x88b5);
	const char* marker = MARKER "88B5";
	memcpy(f + n, marker, strlen(marker));
	return pad(f, n + strlen(marker));
}


/* #pragma mark - platform: open, send a whole frame, watch the device */


#ifdef __HAIKU__

static void
make_link_addr(struct sockaddr_dl* sdl, int withName)
{
	memset(sdl, 0, sizeof(*sdl));
	sdl->sdl_len = sizeof(*sdl);
	sdl->sdl_family = AF_LINK;
	sdl->sdl_type = 0;		/* no frame type: send only */
	if (withName) {
		/* the name, unterminated, then the address after it */
		sdl->sdl_nlen = strlen(sIfName);
		memcpy(sdl->sdl_data, sIfName, sdl->sdl_nlen);
	}
	sdl->sdl_alen = 6;
	memcpy(LLADDR(sdl), sMac, 6);
}


static int
platform_open()
{
	int fd = socket(AF_LINK, SOCK_DGRAM, 0);
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, sIfName, sizeof(ifr.ifr_name));
	if (ioctl(fd, SIOCGIFADDR, &ifr, sizeof(ifr)) < 0) {
		printf("SIOCGIFADDR(AF_LINK) %s: %s\n", sIfName, strerror(errno));
		return -1;
	}
	struct sockaddr_dl sdl;
	memcpy(&sdl, &ifr.ifr_addr, sizeof(ifr) - offsetof(struct ifreq, ifr_addr)
		< sizeof(sdl) ? sizeof(ifr) - offsetof(struct ifreq, ifr_addr)
		: sizeof(sdl));
	memcpy(sMac, LLADDR(&sdl), 6);
	close(fd);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, sIfName, sizeof(ifr.ifr_name));
	if (ioctl(fd, SIOCGIFADDR, &ifr, sizeof(ifr)) < 0) {
		printf("SIOCGIFADDR(AF_INET) %s: %s\n", sIfName, strerror(errno));
		return -1;
	}
	sIp = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr;
	close(fd);

	sWatchFd = socket(AF_LINK, SOCK_DGRAM, 0);
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, sIfName, sizeof(ifr.ifr_name));
	if (ioctl(sWatchFd, SIOCSPACKETCAP, &ifr, sizeof(ifr)) < 0) {
		printf("SIOCSPACKETCAP %s: %s\n", sIfName, strerror(errno));
		return -1;
	}

	sSendFd = socket(AF_LINK, SOCK_DGRAM, 0);
	struct sockaddr_dl b;
	make_link_addr(&b, 0);
	int r = bind(sSendFd, (struct sockaddr*)&b, sizeof(b));
	row("send-only-bind", r == 0, "bind(sdl_type 0, own MAC) = %d (%s)", r,
		r ? strerror(errno) : "ok");
	return r == 0 ? 0 : -1;
}

/* What eth_send()/pcap_inject() on Haiku would do with a whole frame. */
static ssize_t
send_frame_on(int fd, const uint8_t* f, size_t len)
{
	struct sockaddr_dl d;
	memset(&d, 0, sizeof(d));
	d.sdl_len = sizeof(d);
	d.sdl_family = AF_LINK;
	d.sdl_type = IFT_ETHER;
	memcpy(&d.sdl_e_type, f + 12, 2);	/* already network order */
	d.sdl_alen = 6;
	memcpy(LLADDR(&d), f, 6);
	ssize_t n = sendto(fd, f + 14, len - 14, 0, (struct sockaddr*)&d,
		sizeof(d));
	return n < 0 ? n : n + 14;
}

static ssize_t
send_frame(const uint8_t* f, size_t len)
{
	return send_frame_on(sSendFd, f, len);
}

static ssize_t
watch_frame(uint8_t* buf, size_t size)
{
	return recv(sWatchFd, buf, size, MSG_DONTWAIT);
}

static void
haiku_rows()
{
	uint8_t f[256], buf[2048];

	/* The receive claim: ARP is the stack's own, so a typed bind is still
	 * refused -- that part of the issue is by design. */
	{
		int fd = socket(AF_LINK, SOCK_DGRAM, 0);
		struct sockaddr_dl b;
		make_link_addr(&b, 0);
		b.sdl_type = IFT_ETHER;
		b.sdl_e_type = htons(0x0806);
		int r = bind(fd, (struct sockaddr*)&b, sizeof(b));
		row("typed-bind-0806", r != 0, "bind(IFT_ETHER, 0x0806) = %d (%s);"
			" expected refused", r, r ? strerror(errno) : "ok");
		close(fd);

		/* Repeat it: each refused bind leaked a device-interface reference. */
		int refused = 0;
		for (int k = 0; k < 1000; k++) {
			fd = socket(AF_LINK, SOCK_DGRAM, 0);
			if (bind(fd, (struct sockaddr*)&b, sizeof(b)) != 0)
				refused++;
			close(fd);
		}
		row("typed-bind-0806-x1000", refused == 1000, "%d/1000 refused",
			refused);
	}
	{
		int fd = socket(AF_LINK, SOCK_DGRAM, 0);
		struct sockaddr_dl b;
		make_link_addr(&b, 0);
		b.sdl_type = IFT_ETHER;
		b.sdl_e_type = htons(0x88b5);
		int r = bind(fd, (struct sockaddr*)&b, sizeof(b));
		size_t len = build_x88b5(f);
		ssize_t n = r == 0 ? send_frame_on(fd, f, len) : -1;
		row("typed-bind-88b5", r == 0 && n == (ssize_t)len,
			"bind = %d, sendto = %zd (%s)", r, n, n < 0 ? strerror(errno) : "ok");
		close(fd);

		/* Rebinding the same type after close: the receive handler and the
		 * device reference must both be released. */
		int ok = 0;
		for (int k = 0; k < 1000; k++) {
			fd = socket(AF_LINK, SOCK_DGRAM, 0);
			if (bind(fd, (struct sockaddr*)&b, sizeof(b)) == 0)
				ok++;
			close(fd);
		}
		row("typed-bind-88b5-x1000", ok == 1000, "%d/1000 bound", ok);

		/* Bound and then capturing: closing it must still release the
		 * receive handler, or the type can never be bound again. */
		fd = socket(AF_LINK, SOCK_DGRAM, 0);
		r = bind(fd, (struct sockaddr*)&b, sizeof(b));
		struct ifreq ifr;
		memset(&ifr, 0, sizeof(ifr));
		strlcpy(ifr.ifr_name, sIfName, sizeof(ifr.ifr_name));
		int cap = ioctl(fd, SIOCSPACKETCAP, &ifr, sizeof(ifr));
		close(fd);
		fd = socket(AF_LINK, SOCK_DGRAM, 0);
		int again = bind(fd, (struct sockaddr*)&b, sizeof(b));
		row("bound-capture-close", r == 0 && again == 0, "bind = %d,"
			" SIOCSPACKETCAP = %d, close, bind again = %d (%s)", r, cap, again,
			again ? strerror(errno) : "ok");
		close(fd);
	}

	/* Bound by name and address: the address follows the name in sdl_data,
	 * so the stack must find it through sdl_nlen. */
	{
		int fd = socket(AF_LINK, SOCK_DGRAM, 0);
		struct sockaddr_dl b;
		make_link_addr(&b, 1);
		int r = bind(fd, (struct sockaddr*)&b, sizeof(b));
		int bindErrno = errno;
		size_t len = build_x88b5(f);
		f[14 + strlen(MARKER) + 4] = '@';	/* tell it apart */
		while (watch_frame(buf, sizeof(buf)) > 0)
			;
		ssize_t n = r == 0 ? send_frame_on(fd, f, len) : -1;
		int sendErrno = errno;
		int seen = 0;
		uint8_t src[6] = {0};
		double end = now() + 1.0;
		while (n > 0 && !seen && now() < end) {
			ssize_t got = watch_frame(buf, sizeof(buf));
			if (got <= 0) {
				usleep(1000);
				continue;
			}
			if (got >= (ssize_t)len && memcmp(buf + 12, f + 12, len - 12) == 0) {
				seen = 1;
				memcpy(src, buf + 6, 6);
			}
		}
		row("bind-by-name+mac", r == 0 && n == (ssize_t)len && seen
			&& memcmp(src, sMac, 6) == 0, "bind(nlen %u + MAC) = %d (%s),"
			" sendto = %zd (%s), seen leaving = %d, source %s", b.sdl_nlen, r,
			r ? strerror(bindErrno) : "ok", n,
			n < 0 ? (r ? "not bound" : strerror(sendErrno)) : "ok", seen,
			mac_str(src));
		close(fd);
	}

	/* A bound address naming the device but carrying another MAC: the
	 * frames would go out with that MAC as their source. The MAC starts with
	 * 0, so the name test in get_interface_for_link() matches it. */
	{
		static const uint8_t foreign[6] = {0x00, 0x10, 0x37, 0x10, 0x37, 0x01};
		for (int typed = 1; typed >= 0; typed--) {
			int fd = socket(AF_LINK, SOCK_DGRAM, 0);
			struct sockaddr_dl b;
			make_link_addr(&b, 1);
			memcpy(LLADDR(&b), foreign, 6);
			if (typed) {
				b.sdl_type = IFT_ETHER;
				b.sdl_e_type = htons(0x88b5);
			}
			int r = bind(fd, (struct sockaddr*)&b, sizeof(b));
			int bindErrno = errno;
			size_t len = build_x88b5(f);
			f[14 + strlen(MARKER) + 4] = typed ? 'F' : 'f';
			while (watch_frame(buf, sizeof(buf)) > 0)
				;
			ssize_t n = r == 0 ? send_frame_on(fd, f, len) : -1;
			int seen = 0;
			uint8_t src[6] = {0};
			double end = now() + 1.0;
			while (n > 0 && !seen && now() < end) {
				ssize_t got = watch_frame(buf, sizeof(buf));
				if (got <= 0) {
					usleep(1000);
					continue;
				}
				if (got >= (ssize_t)len
					&& memcmp(buf + 12, f + 12, len - 12) == 0) {
					seen = 1;
					memcpy(src, buf + 6, 6);
				}
			}
			row(typed ? "foreign-mac-bind-typed" : "foreign-mac-bind", r != 0,
				"bind(name + MAC %s) = %d (%s); expected refused; sendto = %zd,"
				" seen leaving = %d, source %s", mac_str(foreign), r,
				r ? strerror(bindErrno) : "ok", n, seen, mac_str(src));
			close(fd);
		}
	}

	/* Malformed destinations on a send-only socket. */
	{
		const char* names[] = {"bad-dst-no-type", "bad-dst-no-addr",
			"bad-dst-long-name", "bad-dst-family", "bad-dst-short"};
		for (int k = 0; k < 5; k++) {
			socklen_t length = sizeof(struct sockaddr_dl);
			struct sockaddr_dl d;
			memset(&d, 0, sizeof(d));
			d.sdl_len = sizeof(d);
			d.sdl_family = AF_LINK;
			d.sdl_type = IFT_ETHER;
			d.sdl_e_type = htons(0x88b5);
			d.sdl_alen = 6;
			memset(LLADDR(&d), 0xff, 6);
			if (k == 0)
				d.sdl_type = 0;
			else if (k == 1)
				d.sdl_alen = 0;
			else if (k == 2)
				d.sdl_nlen = 250;
			else if (k == 3)
				d.sdl_family = AF_INET;
			else {
				/* ends before the address: its bytes would be stale */
				length = offsetof(struct sockaddr_dl, sdl_data) + 2;
				d.sdl_len = length;
			}
			ssize_t n = sendto(sSendFd, "x", 1, 0, (struct sockaddr*)&d,
				length);
			row(names[k], n < 0, "sendto = %zd (%s); expected refused", n,
				n < 0 ? strerror(errno) : "ok");
		}
	}

	/* A 2-byte destination under MSG_BCAST: the frame type would come from
	 * whatever an earlier buffer left behind. */
	{
		struct sockaddr_dl d;
		memset(&d, 0, sizeof(d));
		d.sdl_len = 2;
		d.sdl_family = AF_LINK;
		ssize_t n = sendto(sSendFd, "x", 1, MSG_BCAST, (struct sockaddr*)&d,
			2);
		row("bad-dst-short-bcast", n < 0 && errno == EINVAL,
			"sendto(2-byte address, MSG_BCAST) = %zd (%s); expected EINVAL", n,
			n < 0 ? strerror(errno) : "ok");
	}

	/* A bind address with the device's name that claims a MAC but ends
	 * before it: the rest of the kernel's copy is zeroes, which would become
	 * the frames' source. */
	{
		int fd = socket(AF_LINK, SOCK_DGRAM, 0);
		struct sockaddr_dl b;
		make_link_addr(&b, 1);
		socklen_t length = offsetof(struct sockaddr_dl, sdl_data) + b.sdl_nlen;
		b.sdl_len = length;
		int r = bind(fd, (struct sockaddr*)&b, length);
		row("bad-bind-short", r != 0 && errno == EINVAL, "bind(name, alen 6,"
			" %u-byte address) = %d (%s); expected EINVAL", (unsigned)length, r,
			r ? strerror(errno) : "ok");
		close(fd);
	}

	/* A non-root process may neither bind nor send. */
	{
		pid_t pid = fork();
		if (pid == 0) {
			if (setuid(1000) != 0)
				_exit(9);
			int fd = socket(AF_LINK, SOCK_DGRAM, 0);
			struct sockaddr_dl b;
			make_link_addr(&b, 0);
			int r = bind(fd, (struct sockaddr*)&b, sizeof(b));
			int bindErrno = errno;
			size_t len = build_x88b5(f);
			ssize_t n = send_frame_on(fd, f, len);
			int sendErrno = errno;
			printf("%-22s %s  uid %d: bind = %d (%s)\n", "nonroot-bind",
				r != 0 ? "PASS" : "FAIL", getuid(), r,
				r ? strerror(bindErrno) : "ok");
			printf("%-22s %s  uid %d: sendto unbound = %zd (%s)\n",
				"nonroot-send", n < 0 ? "PASS" : "FAIL", getuid(), n,
				n < 0 ? strerror(sendErrno) : "ok");
			fflush(stdout);
			_exit((r != 0 ? 0 : 1) + (n < 0 ? 0 : 1));
		}
		int status = 0;
		waitpid(pid, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			if (WIFEXITED(status) && WEXITSTATUS(status) == 9)
				row("nonroot", 0, "setuid(1000) failed");
			else
				sFailures += WIFEXITED(status) ? WEXITSTATUS(status) : 1;
		}
	}
}

#else	/* Linux */

static int
platform_open()
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, sIfName, sizeof(ifr.ifr_name) - 1);
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
		printf("SIOCGIFHWADDR %s: %s\n", sIfName, strerror(errno));
		return -1;
	}
	memcpy(sMac, ifr.ifr_hwaddr.sa_data, 6);
	if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
		printf("SIOCGIFADDR %s: %s\n", sIfName, strerror(errno));
		return -1;
	}
	sIp = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr;
	close(fd);
	sIfIndex = if_nametoindex(sIfName);

	sSendFd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	struct sockaddr_ll ll;
	memset(&ll, 0, sizeof(ll));
	ll.sll_family = AF_PACKET;
	ll.sll_protocol = htons(ETH_P_ALL);
	ll.sll_ifindex = sIfIndex;
	int r = bind(sSendFd, (struct sockaddr*)&ll, sizeof(ll));
	row("packet-bind", r == 0, "AF_PACKET SOCK_RAW bind(ifindex %d) = %d (%s)",
		sIfIndex, r, r ? strerror(errno) : "ok");
	/* A packet socket does not see its own sends; watch with another. */
	sWatchFd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (sWatchFd < 0 || bind(sWatchFd, (struct sockaddr*)&ll, sizeof(ll)) != 0)
		r = -1;
	return r == 0 ? 0 : -1;
}

static ssize_t
send_frame(const uint8_t* f, size_t len)
{
	return send(sSendFd, f, len, 0);
}

static ssize_t
watch_frame(uint8_t* buf, size_t size)
{
	return recv(sWatchFd, buf, size, MSG_DONTWAIT);
}

#endif


/* #pragma mark - common rows */


/* Waits up to timeout seconds for a frame match() accepts. */
static int
await(double timeout, int (*match)(const uint8_t*, ssize_t, void*),
	void* cookie)
{
	uint8_t buf[2048];
	double end = now() + timeout;
	while (now() < end) {
		ssize_t n = watch_frame(buf, sizeof(buf));
		if (n <= 0) {
			usleep(1000);
			continue;
		}
		if (match(buf, n, cookie))
			return 1;
	}
	return 0;
}


static void
drain()
{
	uint8_t buf[2048];
	while (watch_frame(buf, sizeof(buf)) > 0)
		;
}


static int
match_arp_reply(const uint8_t* b, ssize_t n, void* cookie)
{
	(void)cookie;
	if (n < 42 || b[12] != 0x08 || b[13] != 0x06 || b[20] != 0 || b[21] != 2)
		return 0;
	if (memcmp(b + 28, &sPeerIp, 4) != 0)
		return 0;
	memcpy(sPeerMac, b + 22, 6);
	return 1;
}


/* The frame we sent, seen leaving the device (same bytes from the type on). */
struct sent_frame {
	const uint8_t* data;
	size_t len;
	uint8_t source[6];
};

static int
match_sent(const uint8_t* b, ssize_t n, void* cookie)
{
	struct sent_frame* s = (struct sent_frame*)cookie;
	if (n < (ssize_t)s->len || memcmp(b, s->data, 6) != 0
		|| memcmp(b + 12, s->data + 12, s->len - 12) != 0)
		return 0;
	memcpy(s->source, b + 6, 6);
	return 1;
}


static int
match_echo_reply(const uint8_t* b, ssize_t n, void* cookie)
{
	uint16_t seq = *(uint16_t*)cookie;
	if (n < 42 || b[12] != 0x08 || b[13] != 0x00 || b[23] != 1)
		return 0;
	const uint8_t* ip = b + 14;
	if (memcmp(ip + 12, &sPeerIp, 4) != 0 || memcmp(ip + 16, &sIp, 4) != 0)
		return 0;
	const uint8_t* i = ip + (ip[0] & 0xf) * 4;
	return i[0] == 0 && ((i[4] << 8) | i[5]) == sIcmpId
		&& ((i[6] << 8) | i[7]) == seq;
}


static void
send_row(const char* name, const uint8_t* f, size_t len, const char* what)
{
	drain();
	ssize_t n = send_frame(f, len);
	int err = errno;
	struct sent_frame s = {f, len, {0}};
	int seen = n == (ssize_t)len && await(1.0, match_sent, &s);
	row(name, n == (ssize_t)len && seen && memcmp(s.source, sMac, 6) == 0,
		"%s: send = %zd (%s), seen leaving = %d, source %s", what, n,
		n < 0 ? strerror(err) : "ok", seen, mac_str(s.source));
}


int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <ifname> <peer-ipv4> [burst-count]\n",
			argv[0]);
		return 2;
	}
	sIfName = argv[1];
	if (inet_pton(AF_INET, argv[2], &sPeerIp) != 1) {
		fprintf(stderr, "bad peer address %s\n", argv[2]);
		return 2;
	}
	int burst = argc > 3 ? atoi(argv[3]) : 1000;
	sIcmpId = (uint16_t)(getpid() ^ 0x1037);

	if (platform_open() != 0) {
		printf("RESULT failures=%d (open failed)\n", sFailures + 1);
		return 1;
	}
	char ip[16], peer[16];
	inet_ntop(AF_INET, &sIp, ip, sizeof(ip));
	inet_ntop(AF_INET, &sPeerIp, peer, sizeof(peer));
	printf("interface %s mac %s ip %s peer %s\n", sIfName, mac_str(sMac), ip,
		peer);

	uint8_t f[256];
	size_t len;

	/* ARP: the request must leave, and the reply names the peer's MAC. */
	len = build_arp_request(f, sPeerIp);
	send_row("arp-request-tx", f, len, "ARP who-has peer");
	drain();
	ssize_t n = send_frame(f, len);
	int err = errno;
	sHavePeerMac = n == (ssize_t)len && await(2.0, match_arp_reply, NULL);
	row("arp-reply-rx", sHavePeerMac, "send = %zd (%s), reply from peer: %s",
		n, n < 0 ? strerror(err) : "ok",
		sHavePeerMac ? mac_str(sPeerMac) : "none");

	/* IPv4: the peer must answer the echo. */
	if (sHavePeerMac) {
		uint16_t seq = 1;
		len = build_icmp_echo(f, seq);
		send_row("icmp-echo-tx", f, len, "ICMP echo to peer");
		seq = 2;
		len = build_icmp_echo(f, seq);
		drain();
		n = send_frame(f, len);
		err = errno;
		int reply = n == (ssize_t)len && await(2.0, match_echo_reply, &seq);
		row("icmp-echo-reply-rx", reply, "send = %zd (%s), echo reply = %d",
			n, n < 0 ? strerror(err) : "ok", reply);

		len = build_udp(f);
		send_row("udp-tx", f, len, "UDP to peer:9 marker " MARKER "UDP");
	} else
		row("icmp/udp", 0, "skipped: no peer MAC");

	len = build_ipv6_echo(f);
	send_row("ipv6-tx", f, len, "ICMPv6 echo to ff02::1");

	len = build_x88b5(f);
	send_row("x88b5-tx", f, len, "ethertype 0x88b5 broadcast");

#ifdef __HAIKU__
	haiku_rows();
#endif

	/* Burst: back-to-back frames, every send must succeed; then the peer
	 * must still answer. */
	{
		int arpOk = 0, icmpOk = 0;
		double start = now();
		for (int k = 0; k < burst; k++) {
			len = build_arp_request(f, sPeerIp);
			if (send_frame(f, len) == (ssize_t)len)
				arpOk++;
			if (sHavePeerMac) {
				len = build_icmp_echo(f, 1000 + (k % 60000));
				if (send_frame(f, len) == (ssize_t)len)
					icmpOk++;
			}
		}
		double wall = now() - start;
		row("burst", arpOk == burst && (!sHavePeerMac || icmpOk == burst),
			"%d/%d ARP, %d/%d ICMP sent in %.3fs", arpOk, burst, icmpOk,
			sHavePeerMac ? burst : 0, wall);
		usleep(200000);
		if (sHavePeerMac) {
			uint16_t seq = 3;
			len = build_icmp_echo(f, seq);
			drain();
			n = send_frame(f, len);
			int reply = n == (ssize_t)len && await(2.0, match_echo_reply, &seq);
			row("after-burst-echo", reply, "echo reply = %d", reply);
		}
	}

	printf("RESULT failures=%d\n", sFailures);
	return sFailures;
}
