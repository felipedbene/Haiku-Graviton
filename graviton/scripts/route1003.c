/*
 * route1003.c -- exercise libdnet's route API on Haiku (issue 1003).
 *
 * Build against nmap's bundled libdnet after a haikuporter build:
 *   D=.../nmap-7.94/libdnet-stripped
 *   gcc -Wall -o route1003 route1003.c -I$D/include $D/src/.libs/libdnet.a -lnetwork
 * Usage: route1003 [lookup-addr...] [-a <dst> <gw>]
 * Lists the table (route_loop), looks up each address (route_get), and with -a
 * adds the route, lists again, deletes it and lists again. With route-none.c,
 * route_open() fails with ENOSYS.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dnet.h"

static int
print_route(const struct route_entry *e, void *arg)
{
	printf("  %-16s %-20s via %s\n", e->intf_name, addr_ntoa(&e->route_dst),
		addr_ntoa(&e->route_gw));
	return 0;
}

int
main(int argc, char **argv)
{
	route_t *r = route_open();
	if (r == NULL) {
		printf("route_open: %s\n", strerror(errno));
		return 1;
	}
	printf("route_loop rc=%d\n", route_loop(r, print_route, NULL));

	for (int i = 1; i < argc; i++) {
		struct route_entry e;
		memset(&e, 0, sizeof(e));
		if (strcmp(argv[i], "-a") == 0 && i + 2 < argc) {
			addr_aton(argv[i + 1], &e.route_dst);
			addr_aton(argv[i + 2], &e.route_gw);
			printf("route_add %s via %s rc=%d\n", argv[i + 1], argv[i + 2],
				route_add(r, &e));
			route_loop(r, print_route, NULL);
			printf("route_delete rc=%d\n", route_delete(r, &e));
			route_loop(r, print_route, NULL);
			i += 2;
			continue;
		}
		addr_aton(argv[i], &e.route_dst);
		if (route_get(r, &e) == 0)
			printf("route_get %s -> %s via %s\n", argv[i], e.intf_name,
				addr_ntoa(&e.route_gw));
		else
			printf("route_get %s: %s\n", argv[i], strerror(errno));
	}
	route_close(r);
	return 0;
}
