/*
 * Copyright 2019 Leorize <leorize+oss@disroot.org>
 * Copyright 2015 Tiancheng "Timothy" Gu, timothygu99@gmail.com
 * Copyright 2001-2002 François Revol (mmu_man)
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600 /* for NZERO */
#endif

#include "pthread_private.h"

#include <errno.h>
#include <sys/resource.h>
#include <unistd.h>

#include <OS.h>
#include <sys/param.h> // for MAX() and MIN() macros
#include <syscall_utils.h> // RETURN_AND_SET_ERRNO()

// Part of this file is adapted from src/bin/renice.c, by François Revol.

// Some notes on different priority notations:
//
// Notation system          | Real-time | High Prio | Default | Low Prio |
// -------------------------|----------:|----------:|--------:|---------:|
// BeOS/Haiku               |      120* |        99 |      10 |      1** |
// UNIX [gs]etpriority()*** |      N.A. |       -20 |       0 |       19 |
// UNIX internal            |      N.A. |         0 |      20 |       39 |
//
// * 	Note that BeOS/Haiku does not have an absolute highest priority value
//   	(else than the maximum of int32), and (B_REAL_TIME_PRIORITY - 1) is
//   	the highest non-real-time priority and usually used as the priority
//   	limit. On UNIX systems there is no such concept as "real-time."
// **	0 only for idle_thread
// ***	Also used for UNIX nice(1) and nice(3)

#ifdef CLIP
#undef CLIP
#endif
#define CLIP(n, max, min) MAX(MIN((n), (max)), (min))

// In these contexts, "min" does not mean "lowest value," but "lowest
// priority," which is the maximum value in UNIX priority notation.
// "Zero" means normal priority, which in UNIX function notation is 0.
// POSIX specification also refers to it as "NZERO."
#define NMAX 0
#define NMIN (NZERO * 2 - 1)
#define CLIP_TO_UNIX(n) CLIP(n, NMIN, NMAX)

#define BZERO B_NORMAL_PRIORITY
#define BMAX (B_REAL_TIME_DISPLAY_PRIORITY - 1)
#define BMIN 1
#define CLIP_TO_BEOS(n) CLIP(n, BMAX, BMIN)

// To accurately convert the notation values, we need to use an exponential
// function:
//
// 	f(x) = 99e^(-0.116x)
//
// where f() represents the BeOS priority value, and x represents the Unix
// priority value.
//
// But that's too complicated. And slow. So we use a simple piecewise linear
// approach here, by a simple rescaling of the values.

// returns an equivalent UNIX priority for a given BeOS priority.
static int32
prio_be_to_unix(int32 prio)
{
	int out;
	if (prio >= BZERO)
		out = NZERO
			- ((prio - BZERO) * NZERO + (BMAX - BZERO) / 2) / (BMAX - BZERO);
			// `(BMAX - BZERO) / 2` for rounding
	else
		out = NZERO
			+ ((BZERO - prio) * (NZERO - 1)) / (BZERO - BMIN)
			+ 1;
			// `+1` for rounding
	return CLIP_TO_UNIX(out);
}


// returns an equivalent BeOS priority for a given UNIX priority.
static int32
prio_unix_to_be(int32 prio)
{
	int out;
	// Do not need to care about rounding
	if (prio >= NZERO)
		out = BZERO - ((prio - NZERO) * (BZERO - BMIN)) / (NZERO - 1);
	else
		out = BZERO + ((NZERO - prio) * (BMAX - BZERO)) / (NZERO);
	return CLIP_TO_BEOS(out);
}


// The nice value setpriority() last set for this team, and the BeOS priority
// it applied for it (-1 if setpriority() was never called). The conversion
// above maps several nice values onto one BeOS priority (nice 0 to 19 onto
// 10 to 1), so the priority alone cannot give back the value that was set.
// The pair is process memory, so it survives fork(), as a nice value does.
static int sNiceValue = 0;
static int32 sNicePriority = -1;


int
getpriority(int which, id_t who)
{
	bool found = false;
	int out = -100;

	if (who < 0)
		RETURN_AND_SET_ERRNO(EINVAL);
	switch (which) {
		case PRIO_PROCESS:
		{
			int32 th_cookie = 0;
			thread_info thread;

			while (get_next_thread_info(who, &th_cookie, &thread) == B_OK) {
				if (thread.priority > out) {
					found = true;
					out = thread.priority;
				}
			}

			// Report the nice value set for this team as long as its threads
			// still run at the priority setpriority() gave them; once that was
			// changed by other means, fall back to converting the priority.
			if (found && (who == 0 || who == getpid())
				&& out == sNicePriority) {
				return sNiceValue;
			}
			break;
		}
		case PRIO_PGRP:
		{
			int32 team_cookie = 0, th_cookie = 0;
			team_info team;
			thread_info thread;

			who = who == 0 ? getpgrp() : who;
			while (get_next_team_info(&team_cookie, &team) == B_OK) {
				if (getpgid(team.team) != who)
					continue;
				th_cookie = 0;
				while (get_next_thread_info(team.team, &th_cookie, &thread)
					== B_OK) {
					if (thread.priority > out) {
						found = true;
						out = thread.priority;
					}
				}
			}
			break;
		}
		case PRIO_USER:
		{
			// `who` (id_t) is int32, but uid_t is uint32, so using this
			// indirection to get rid of compiler warnings
			// `who` can never be negative because of the `who < 0` check
			// above.
			uid_t euid = who == 0 ? geteuid() : (uid_t)who;
			int32 team_cookie = 0, th_cookie = 0;
			team_info team;
			thread_info thread;

			while (get_next_team_info(&team_cookie, &team) == B_OK) {
				if (team.uid != euid)
					continue;
				th_cookie = 0;
				while (get_next_thread_info(team.team, &th_cookie, &thread)
					== B_OK) {
					if (thread.priority > out) {
						found = true;
						out = thread.priority;
					}
				}
			}
			break;
		}
		default:
			RETURN_AND_SET_ERRNO(EINVAL);
	}
	if (!found)
		RETURN_AND_SET_ERRNO(ESRCH);
	return prio_be_to_unix(out) - NZERO;
}


int
setpriority(int which, id_t who, int value)
{
	int32 th_cookie = 0;
	thread_info thread;

	// TODO: implement for other processes
	if (who != 0 && which != PRIO_PROCESS)
		RETURN_AND_SET_ERRNO(EINVAL);
	value = value > NMIN ? NMIN : CLIP_TO_UNIX(value + NZERO);
	int nice = value - NZERO;
	value = prio_unix_to_be(value);

	__pthread_set_default_priority(value);
	while (get_next_thread_info(B_CURRENT_TEAM, &th_cookie, &thread) == B_OK)
		set_thread_priority(thread.thread, value);

	sNiceValue = nice;
	sNicePriority = value;

	return 0;
}
