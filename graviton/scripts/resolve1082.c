/*
 * resolve1082 -- how long does a name lookup take to fail when no name server
 * answers? (issue 1082)
 *
 * With no resolv.conf, the stub resolver falls back to 0.0.0.0:53 and [::]:53.
 * Nothing listens on either, so the lookup should fail as fast as the two
 * port unreachables come back; where the IPv6 one never does, each query
 * waits out its timeout instead.
 *
 * Build: cc -o resolve1082 resolve1082.c   (add -lnetwork on Haiku)
 * Usage: resolve1082 <name> [...]
 */

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>


static double
now(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}


int
main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {
		struct addrinfo hints, *result = NULL;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;

		double start = now();
		int status = getaddrinfo(argv[i], "80", &hints, &result);
		double elapsed = now() - start;

		printf("getaddrinfo(%s) = %d (%s)  %.3fs\n", argv[i], status,
			status == 0 ? "ok" : gai_strerror(status), elapsed);
		if (result != NULL)
			freeaddrinfo(result);
	}
	return 0;
}
