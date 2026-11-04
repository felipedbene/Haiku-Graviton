/*
 * icmp6ratelimit1082 -- drives the ICMPv6 error token bucket
 * (src/add-ons/kernel/network/protocols/icmp6/ErrorRateLimit.h) with chosen
 * times, on the build host. Issue 1082 exempts errors to this host itself
 * from the limit, and this platform has no IPv6 path to another host to test
 * it with, so the limiter is checked here instead of on a running system.
 *
 * Rows: a burst of 50 from full; the 51st refused; one more per millisecond
 * after that; a sustained 1000 per second; no more than the burst after an
 * idle hour; a clock that steps back neither grants nor takes credit.
 *
 * Build: c++ -I src/add-ons/kernel/network/protocols/icmp6 \
 *            -o icmp6ratelimit1082 graviton/scripts/icmp6ratelimit1082.cpp
 */

#include <stdint.h>
#include <stdio.h>

typedef int64_t bigtime_t;
typedef int32_t int32;

#include "ErrorRateLimit.h"


static int sFailures = 0;


static void
row(const char* label, long expected, long got)
{
	printf("%-52s %s  expected %ld, got %ld\n", label,
		expected == got ? "PASS" : "FAIL", expected, got);
	if (expected != got)
		sFailures++;
}


static long
take_all(ErrorRateLimit& limit, bigtime_t now, int tries)
{
	long granted = 0;
	for (int i = 0; i < tries; i++)
		granted += limit.Take(now);
	return granted;
}


int
main()
{
	bigtime_t now = 1000000;

	ErrorRateLimit limit;
	row("200 at one instant from full: the burst", 50,
		take_all(limit, now, 200));
	row("same instant again: none", 0, take_all(limit, now, 10));
	now += 999;
	row("999 us later: none", 0, take_all(limit, now, 10));
	now += 1;
	row("1 ms after the burst: one", 1, take_all(limit, now, 10));
	now += 10000;
	row("10 ms later: ten", 10, take_all(limit, now, 100));

	long sustained = 0;
	for (int i = 0; i < 10000; i++) {
		now += 100;
		sustained += take_all(limit, now, 1);
	}
	row("1 s of one try every 100 us: 1000", 1000, sustained);

	now += 3600LL * 1000000;
	row("after an idle hour: no more than the burst", 50,
		take_all(limit, now, 200));

	now -= 5000000;
	row("clock 5 s back: no credit granted", 0, take_all(limit, now, 10));
	now += 2000;
	row("then 2 ms forward: two", 2, take_all(limit, now, 10));

	printf("%s: %d failure(s)\n", sFailures == 0 ? "PASS" : "FAIL", sFailures);
	return sFailures;
}
