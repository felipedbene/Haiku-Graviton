/* linksend.c -- can an AF_LINK socket send an Ethernet frame on Haiku? (issue 1019) */
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
try_type(const char *ifname, unsigned etype, const unsigned char *mac)
{
	int fd = socket(AF_LINK, SOCK_DGRAM, 0);
	struct sockaddr_dl b;
	memset(&b, 0, sizeof(b));
	b.sdl_len = sizeof(b);
	b.sdl_family = AF_LINK;
	b.sdl_type = etype ? IFT_ETHER : 0;
	b.sdl_e_type = htons(etype);
	b.sdl_alen = 6;
	memcpy(LLADDR(&b), mac, 6);
	int r = bind(fd, (struct sockaddr *)&b, sizeof(b));
	printf("ethertype 0x%04x: bind=%d (%s)\n", etype, r, r ? strerror(errno) : "ok");
	struct sockaddr_dl d;
	memset(&d, 0, sizeof(d));
	d.sdl_len = sizeof(d);
	d.sdl_family = AF_LINK;
	d.sdl_type = IFT_ETHER;
	d.sdl_e_type = htons(etype);
	d.sdl_alen = 6;
	memset(LLADDR(&d), 0xff, 6);
	char payload[64];
	memset(payload, 0x5a, sizeof(payload));
	ssize_t n = sendto(fd, payload, sizeof(payload), 0, (struct sockaddr *)&d, sizeof(d));
	printf("ethertype 0x%04x: sendto=%zd (%s)\n", etype, n, n < 0 ? strerror(errno) : "ok");
	close(fd);
}

int
main(int argc, char **argv)
{
	const char *ifname = argc > 1 ? argv[1] : "/dev/net/ena/0";
	int fd = socket(AF_LINK, SOCK_DGRAM, 0);
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(fd, SIOCGIFADDR, &ifr, sizeof(ifr)) < 0) {
		printf("SIOCGIFADDR(AF_LINK): %s\n", strerror(errno));
		return 1;
	}
	struct sockaddr_dl *sdl = (struct sockaddr_dl *)&ifr.ifr_addr;
	unsigned char mac[6];
	memcpy(mac, LLADDR(sdl), 6);
	printf("SIOCGIFADDR(AF_LINK) %s: family %d type %d alen %d index %d mac %02x:%02x:%02x:%02x:%02x:%02x\n",
		ifname, sdl->sdl_family, sdl->sdl_type, sdl->sdl_alen, sdl->sdl_index,
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	close(fd);
	try_type(ifname, 0x88b5, mac);	/* IEEE local experimental */
	try_type(ifname, 0x0806, mac);	/* ARP, handled by the stack */
	try_type(ifname, 0x0800, mac);	/* IPv4, handled by the stack */
	try_type(ifname, 0, mac);	/* no type: no receive handler */
	return 0;
}
