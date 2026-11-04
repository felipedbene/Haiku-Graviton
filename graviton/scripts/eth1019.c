/*
 * eth1019.c -- exercise libdnet's eth API on Haiku (issue 1019).
 *
 * Build against nmap's bundled libdnet after a haikuporter build:
 *   D=.../nmap-7.94/libdnet-stripped
 *   gcc -Wall -o eth1019 eth1019.c -I$D/include $D/src/.libs/libdnet.a -lnetwork
 * Usage: eth1019 <device>
 * Opens the device, reads its MAC, and sends frames while a packet-capture
 * (SIOCSPACKETCAP) socket on the same device watches what leaves: an
 * experimental-ethertype (0x88b5) frame from the device's own MAC (sent),
 * the same from another source MAC (EADDRNOTAVAIL: the stack frames only
 * with the device's address), the first again through the cached sender, and
 * an ARP request (refused by the stack, issue 1037). Each captured frame's
 * header is printed. With eth-none.c, eth_open() fails.
 */
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dnet.h"

static int capfd = -1;

static void
drain(const char *what, uint16_t want)
{
	unsigned char buf[2048];
	struct pollfd p = { capfd, POLLIN, 0 };
	int seen = 0;

	while (poll(&p, 1, 500) > 0) {
		ssize_t n = recv(capfd, buf, sizeof(buf), 0);
		if (n < ETH_HDR_LEN)
			break;
		struct eth_hdr *h = (struct eth_hdr *)buf;
		if (ntohs(h->eth_type) != want)
			continue;
		char d[32], s[32];
		eth_ntop(&h->eth_dst, d, sizeof(d));
		eth_ntop(&h->eth_src, s, sizeof(s));
		printf("  captured %s: dst %s src %s type 0x%04x len %zd\n", what, d, s,
			ntohs(h->eth_type), n);
		seen = 1;
	}
	if (!seen)
		printf("  captured %s: nothing\n", what);
}

static void
send_frame(eth_t *e, const char *what, eth_addr_t dst, eth_addr_t src,
	uint16_t type, const void *payload, size_t plen)
{
	unsigned char frame[ETH_HDR_LEN + 64];
	memset(frame, 0, sizeof(frame));
	eth_pack_hdr(frame, dst, src, type);
	memcpy(frame + ETH_HDR_LEN, payload, plen);
	ssize_t n = eth_send(e, frame, ETH_HDR_LEN + plen);
	printf("eth_send %s: %zd%s%s\n", what, n, n < 0 ? " " : "",
		n < 0 ? strerror(errno) : "");
	drain(what, type);
}

int
main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/net/ena/0";
	eth_t *e = eth_open(dev);
	if (e == NULL) {
		printf("eth_open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	eth_addr_t mac, other, bcast;
	char buf[32];
	if (eth_get(e, &mac) < 0) {
		printf("eth_get: %s\n", strerror(errno));
		return 1;
	}
	printf("eth_get %s: %s\n", dev, eth_ntop(&mac, buf, sizeof(buf)));
	printf("eth_set: %d (%s)\n", eth_set(e, &mac), strerror(errno));

	capfd = socket(AF_LINK, SOCK_DGRAM, 0);
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name));
	if (ioctl(capfd, SIOCSPACKETCAP, &ifr, sizeof(ifr)) < 0)
		printf("SIOCSPACKETCAP: %s\n", strerror(errno));

	memset(&bcast, 0xff, sizeof(bcast));
	memcpy(&other, &mac, sizeof(other));
	other.data[5] ^= 0x01;
	const char payload[46] = "DeBeOS issue 1019 eth_send probe";

	send_frame(e, "0x88b5 own-src", bcast, mac, 0x88b5, payload, sizeof(payload));
	send_frame(e, "0x88b5 other-src", bcast, other, 0x88b5, payload, sizeof(payload));
	send_frame(e, "0x88b5 own-src again (cached sender)", bcast, mac, 0x88b5,
		payload, sizeof(payload));

	struct arp_hdr ah;
	unsigned char arp[ARP_HDR_LEN + ARP_ETHIP_LEN];
	memset(arp, 0, sizeof(arp));
	ah.ar_hrd = htons(ARP_HRD_ETH);
	ah.ar_pro = htons(ARP_PRO_IP);
	ah.ar_hln = ETH_ADDR_LEN;
	ah.ar_pln = IP_ADDR_LEN;
	ah.ar_op = htons(ARP_OP_REQUEST);
	memcpy(arp, &ah, sizeof(ah));
	send_frame(e, "ARP request", bcast, mac, ETH_TYPE_ARP, arp, sizeof(arp));

	eth_close(e);
	return 0;
}
