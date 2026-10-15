/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_LIBROOT_USER_THREAD_H
#define _BUILD_LIBROOT_USER_THREAD_H


#include <user_thread_defs.h>


/*!	Host stand-in for libroot's <user_thread.h>.

	The real one reads the user_thread out of a Haiku TLS slot and pokes the
	signal-deferral counter, neither of which exists on the host (and whose
	inline code does not even compile against a glibc sigset_t, which is a
	struct rather than an integer). What the kernel-land emulation actually
	needs from it is the per-thread wait_status that mutex_lock() and
	ConditionVariable park on, so the host side keeps that and nothing else.

	Signal deferral is a no-op: the host emulation blocks threads through
	_kern_block_thread()/_kern_unblock_thread(), which are implemented with
	host condition variables and never run code from a signal handler.
*/

#ifdef __cplusplus
extern "C" {
#endif

struct user_thread*	_haiku_build_get_user_thread(void);

#ifdef __cplusplus
}
#endif


static inline struct user_thread*
get_user_thread()
{
	return _haiku_build_get_user_thread();
}


static inline void
defer_signals()
{
}


static inline void
undefer_signals()
{
}


#endif	// _BUILD_LIBROOT_USER_THREAD_H
