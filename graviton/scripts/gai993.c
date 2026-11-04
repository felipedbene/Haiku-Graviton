/*
 * gai993 -- does getaddrinfo() treat an address literal as an address when
 * AI_ADDRCONFIG is set?
 *
 * OpenSSL's BIO_lookup_ex() passes AI_ADDRCONFIG for every client lookup, so
 * "openssl cmp -server [::1]:port" resolves "::1" that way. On a host with no
 * non-loopback IPv6 address (any IPv4-only EC2 instance), netresolv skipped
 * the numeric parse for AF_INET6 and sent "::1" to the name servers as a host
 * name. Where those do not answer -- haikuporter's chroot has no
 * settings/network/, and the resolver's [::]:53 fallback never refuses --
 * each lookup cost 7 s (issue 993).
 *
 * Rows: "::1" and "127.0.0.1", with and without AI_ADDRCONFIG; the result,
 * the families returned, and the wall time. Linux and FreeBSD return the
 * literal without any DNS traffic (glibc: EAI_ADDRFAMILY for an unconfigured
 * family; FreeBSD: the address).
 *
 * Build: cc -o gai993 gai993.c   (add -lnetwork on Haiku)
 */

#define _GNU_SOURCE
	/* EAI_ADDRFAMILY on glibc */

#include <netdb.h>
#include <netinet/in.h>
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


/*
 * Returns 1 if the literal was treated as an address: the matching family
 * came back (FreeBSD, and Linux without AI_ADDRCONFIG) or EAI_ADDRFAMILY did
 * (glibc with AI_ADDRCONFIG and no such family configured), and quickly. A
 * name-lookup error such as EAI_NONAME means it was sent to DNS as a name.
 */
static int
row(const char* host, int flags)
{
	struct addrinfo hints, *result = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = flags;

	double start = now();
	int error = getaddrinfo(host, "80", &hints, &result);
	double elapsed = now() - start;

	int wanted = strchr(host, ':') != NULL ? AF_INET6 : AF_INET;
	int matched = 0;
	char families[64] = "";
	for (struct addrinfo* entry = result; entry != NULL;
			entry = entry->ai_next) {
		strncat(families, entry->ai_family == AF_INET6 ? " inet6"
			: entry->ai_family == AF_INET ? " inet" : " other",
			sizeof(families) - strlen(families) - 1);
		if (entry->ai_family == wanted)
			matched = 1;
	}
	if (result != NULL)
		freeaddrinfo(result);

	// An answer about a literal needs no name server; anything that took
	// longer than this went to one.
	int ok = elapsed < 0.5
		&& ((error == 0 && matched) || error == EAI_ADDRFAMILY);
	printf("getaddrinfo(%-9s, %-13s) %s  %.3fs  %s%s\n", host,
		flags & AI_ADDRCONFIG ? "AI_ADDRCONFIG" : "0", ok ? "PASS" : "FAIL",
		elapsed, error == 0 ? "ok:" : gai_strerror(error), families);
	return ok;
}


int
main(void)
{
	int passed = 0;
	passed += row("::1", 0);
	passed += row("::1", AI_ADDRCONFIG);
	passed += row("127.0.0.1", 0);
	passed += row("127.0.0.1", AI_ADDRCONFIG);
	printf("%s: %d/4\n", passed == 4 ? "PASS" : "FAIL", passed);
	return passed == 4 ? 0 : 1;
}
