/*
 * Copyright 2026, DeBeOS contributors. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BRANDING_H
#define _BRANDING_H


/*!	Product identity, for user-facing text only.

	DeBeOS is a derivative of Haiku, which is MIT licensed. Attribution is
	retained rather than erased: wherever there is room for a second line,
	OS_ATTRIBUTION_STRING is expected to appear next to OS_DISPLAY_NAME, and
	the upstream copyright, licence and credits text is left intact. Do not
	drop the attribution just because the name fits without it.

	These are display strings and nothing else. Every machine-readable
	identifier deliberately still reads "Haiku", and must keep doing so:

	  - the \c x-vnd.Haiku-* application signatures are MIME types, which every
	    launch declaration, settings file and HaikuPorts package refers to;
	  - \c B_HAIKU_VERSION is API-level and ports test against it;
	  - the \c haiku*.hpkg package names are structural to the build and to
	    packagefs;
	  - \c uname()'s sysname stays "Haiku" because config.guess, CMake and
	    autotools detect the OS from it, so changing it breaks every port's
	    configure step.

	None of those are visible to a user, so renaming them would cost a great
	deal and show nothing.
*/

#define OS_DISPLAY_NAME			"DeBeOS"
#define OS_UPSTREAM_NAME		"Haiku"
#define OS_ATTRIBUTION_STRING	"based on " OS_UPSTREAM_NAME


#endif	// _BRANDING_H
