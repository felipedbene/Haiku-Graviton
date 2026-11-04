/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ICMP6_ERROR_RATE_LIMIT_H
#define ICMP6_ERROR_RATE_LIMIT_H


/*!	The token bucket that limits the ICMPv6 errors this node originates
	(RFC 4443, section 2.4 (f)). It holds no lock and reads no clock, so that
	graviton/scripts/icmp6ratelimit1082.cpp can drive it with chosen times;
	the caller serializes access and passes system_time().
*/
struct ErrorRateLimit {
	// Linux's defaults for the same purpose: net.ipv4.icmp_msgs_per_sec =
	// 1000 and icmp_msgs_burst = 50.
	static const bigtime_t	kInterval = 1000;
		// one error per millisecond on average
	static const int32		kBurst = 50;

	ErrorRateLimit()
		:
		fCredit(kBurst * kInterval),
		fLastTime(0)
	{
	}

	/*!	Takes one error's worth of credit; returns \c false if there is none
		left.
	*/
	bool Take(bigtime_t now)
	{
		bigtime_t elapsed = now - fLastTime;
		if (elapsed < 0)
			elapsed = 0;
		fLastTime = now;

		fCredit += elapsed;
		if (fCredit > kBurst * kInterval)
			fCredit = kBurst * kInterval;

		if (fCredit < kInterval)
			return false;

		fCredit -= kInterval;
		return true;
	}

private:
	bigtime_t	fCredit;
	bigtime_t	fLastTime;
};


#endif	// ICMP6_ERROR_RATE_LIMIT_H
