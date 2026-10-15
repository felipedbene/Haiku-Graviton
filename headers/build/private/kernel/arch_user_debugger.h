/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_KERNEL_ARCH_USER_DEBUGGER_H
#define _BUILD_KERNEL_ARCH_USER_DEBUGGER_H


/*!	Host stand-in for the per-architecture <arch_user_debugger.h>.

	Reached because <thread_types.h> includes <arch/user_debugger.h>. Nothing
	in a host build debugs a team, and the arm64 target version is itself two
	empty structs, so matching it exactly costs nothing. <arch/user_debugger.h>
	defaults DEBUG_MAX_BREAKPOINTS and friends to zero when we leave them
	undefined, which is what we want.
*/

struct arch_team_debug_info {
};


struct arch_thread_debug_info {
};


#endif	// _BUILD_KERNEL_ARCH_USER_DEBUGGER_H
