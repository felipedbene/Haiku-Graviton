/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BUILD_STRING_H
#define _BUILD_STRING_H


/*	The host's <string.h>, plus strlcpy()/strlcat().

	Both are standard in Haiku's libroot and in the kernel, and both are used
	by the network stack. glibc only grew them in 2.38, so on an older host the
	declarations have to come from here and the definitions from the host side
	of the kernel-land emulation. The guard is glibc's own feature test, so a
	host that has them keeps them.
*/
#include_next <string.h>

#include <sys/cdefs.h>

#if !defined(__GLIBC__) || !__GLIBC_PREREQ(2, 38)

__BEGIN_DECLS

extern size_t	strlcpy(char* dest, const char* source, size_t size);
extern size_t	strlcat(char* dest, const char* source, size_t size);

__END_DECLS

#endif


/*	errno carries Haiku's error values, not the host's: the build <Errors.h>
	redirects it through _haiku_build_errno(), which maps what the host set into
	Haiku's numbering so that code comparing against EINVAL and friends still
	works. strerror() therefore has to map back before the host's can describe
	it, or every message would be the wrong one.

	BeOSBuildCompatibility.h does this same remap, but it is force-included only
	for host tools built against the Be API; kernel-mode sources do not get it,
	so the remap lives here with the rest of the host string layer.
*/
#ifndef BUILDING_HAIKU_ERROR_MAPPER

__BEGIN_DECLS

extern char*	_haiku_build_strerror(int errnum);

__END_DECLS

#undef strerror
#define strerror(errnum)	_haiku_build_strerror(errnum)

#endif	// BUILDING_HAIKU_ERROR_MAPPER


#endif	// _BUILD_STRING_H
