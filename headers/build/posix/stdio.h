/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_STDIO_H
#define _BUILD_STDIO_H


/*	The host's <stdio.h>, minus dprintf().

	Haiku's kernel has a dprintf() of its own -- void dprintf(const char*, ...),
	the debug output the code under test writes to -- and glibc has an unrelated
	one, int dprintf(int fd, const char*, ...). Declaring both in one
	translation unit is an error, so the host build has to drop one, and it is
	not the kernel's: that is the one with callers here.

	Renaming it across <stdio.h> only hides the declaration; glibc's dprintf
	stays in libc, unreferenced, and the kernel-land emulation's definition
	keeps the name.
*/
#define dprintf __libc_dprintf_unused
#include_next <stdio.h>
#undef dprintf


#endif	// _BUILD_STDIO_H
