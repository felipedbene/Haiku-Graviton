/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	DeBeOS glue for the vendored Arm optimized-routines strcpy. See
	arm_memcmp.S for why the alias lives in the same unit as the definition.

	The upstream source can also build stpcpy (BUILD_STPCPY); DeBeOS keeps its
	musl stpcpy, so only strcpy is exposed here. */


#define WANT_GNU_PROPERTY 0	/* no BTI/PAC note section; see arm_memcmp.S */

#include "arm-optimized-routines/strcpy.S"

.global strcpy
.type strcpy, %function
.set strcpy, __strcpy_aarch64
