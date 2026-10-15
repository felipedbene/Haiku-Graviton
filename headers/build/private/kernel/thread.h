/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_THREAD_H
#define _BUILD_KERNEL_THREAD_H


#include <OS.h>


/*!	Host stand-in for the kernel's <thread.h>.

	The real header cannot be built off Haiku: it pulls in <ksignal.h> and
	<thread_types.h>, which do arithmetic on sigset_t. Haiku's sigset_t is an
	integer; glibc's is a struct, so "mask | mask" does not even compile. There
	is no way to have both that and glibc's own <signal.h> in one translation
	unit, and the host build needs glibc's.

	Host-built code does not schedule threads, so none of the real header is
	wanted. What it does reach for is the network stack's syscall-restart
	helpers in <syscall_restart.h>, which read exactly two Thread fields. This
	declares a Thread with those two fields and nothing else, and the host
	platform layer hands out a zeroed one per thread -- no syscall, nothing to
	restart, which is the truth for a host test.
*/

#define SYSCALL_RESTART_PARAMETER_SIZE	32

#define	THREAD_FLAGS_RESTART_SYSCALL		0x0080
#define	THREAD_FLAGS_DONT_RESTART_SYSCALL	0x0100
#define	THREAD_FLAGS_ALWAYS_RESTART_SYSCALL	0x0200
#define	THREAD_FLAGS_SYSCALL_RESTARTED		0x0400
#define	THREAD_FLAGS_SYSCALL				0x0800


namespace BKernel {

struct Thread {
	int32		flags;

	struct {
		uint8	parameters[SYSCALL_RESTART_PARAMETER_SIZE];
	} syscall_restart;
};

}	// namespace BKernel

using BKernel::Thread;


#ifdef __cplusplus
extern "C" {
#endif

Thread*		thread_get_current_thread(void);
thread_id	thread_get_current_thread_id(void);

#ifdef __cplusplus
}
#endif


#endif	// _BUILD_KERNEL_THREAD_H
