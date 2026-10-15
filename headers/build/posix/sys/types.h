/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_SYS_TYPES_H
#define _BUILD_SYS_TYPES_H


/*	Unlike the other headers here this one chains to the host's instead of
	replacing it: everything in it is already there and compatible, bar the few
	Haiku types that kernel headers expect <sys/types.h> to have declared.
*/
#include_next <sys/types.h>

#include <config/types.h>

#ifndef addr_t
typedef __haiku_addr_t	addr_t;
#endif


#endif	// _BUILD_SYS_TYPES_H
