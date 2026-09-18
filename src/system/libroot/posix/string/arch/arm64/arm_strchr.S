/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	DeBeOS glue for the vendored Arm optimized-routines strchr. See
	arm_memcmp.S for why the alias lives in the same unit as the definition. */


#define WANT_GNU_PROPERTY 0	/* no BTI/PAC note section; see arm_memcmp.S */

#include "arm-optimized-routines/strchr.S"

.global strchr
.type strchr, %function
.set strchr, __strchr_aarch64
